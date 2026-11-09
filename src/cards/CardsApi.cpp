#include "CardsApi.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WebServer.h>

#include <string>
#include <vector>

#include "CardStore.h"
#include "util/TaskWatchdog.h"

namespace {
using cards::Card;
using cards::CardType;

constexpr const char* UPLOAD_TMP = "/.crosspoint/cards/upload.tmp";
constexpr size_t BMP_HEADER_BYTES = 30;
constexpr size_t MAX_IMAGE_BYTES = 256 * 1024;
constexpr size_t STREAM_CHUNK = 1024;

void sendJson(WebServer& server, const JsonDocument& doc, const int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendError(WebServer& server, const int code, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(server, doc, code);
}

bool readBody(WebServer& server, JsonDocument& doc) {
  if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain").c_str()) != DeserializationError::Ok) {
    sendError(server, 400, "Invalid JSON body");
    return false;
  }
  return true;
}

void sendCards(WebServer& server) {
  JsonDocument doc;
  JsonObject screen = doc["screen"].to<JsonObject>();
  screen["width"] = CARD_STORE.screenWidth();
  screen["height"] = CARD_STORE.screenHeight();
  screen["band"] = cards::STATUS_BAND;
  doc["max"] = cards::MAX_CARDS;
  JsonArray list = doc["cards"].to<JsonArray>();
  for (const auto& card : CARD_STORE.all()) {
    JsonObject item = list.add<JsonObject>();
    item["id"] = card.id;
    item["type"] = card.type == CardType::Image ? "image" : "qr";
    item["title"] = card.title;
    item["subtitle"] = card.subtitle;
    item["payload"] = card.payload;
  }
  sendJson(server, doc);
}

void handleGet(WebServer& server) { sendCards(server); }

void handleSave(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  Card card;
  card.id = body["id"] | "";
  card.title = body["title"] | "";
  card.subtitle = body["subtitle"] | "";
  card.payload = body["payload"] | "";
  if (card.id.empty()) {
    std::string id;
    if (!CARD_STORE.add(card, id)) return sendError(server, 400, "Could not add the card (empty, too long, or full)");
  } else if (!CARD_STORE.update(card)) {
    return sendError(server, 400, "Could not save the card");
  }
  sendCards(server);
}

void handleDelete(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  if (!CARD_STORE.remove(body["id"] | "")) return sendError(server, 404, "No such card");
  sendCards(server);
}

void handleOrder(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  std::vector<std::string> ids;
  for (JsonVariantConst id : body["ids"].as<JsonArrayConst>()) ids.emplace_back(id | "");
  if (!CARD_STORE.reorder(ids)) return sendError(server, 400, "The order must list every card once");
  sendCards(server);
}

void handleShow(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  const int index = CARD_STORE.indexOf(body["id"] | "");
  if (index < 0) return sendError(server, 404, "No such card");
  CARD_STORE.requestShow(index);
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(server, doc);
}

// --- Picture upload: streamed to a temp file, checked, then moved into place.

struct Upload {
  HalFile file;
  size_t bytes = 0;
  bool failed = false;
};
Upload upload;

uint32_t readLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// The phone converts pictures to exactly what the screen draws without
// scaling: an uncompressed 1-bit BMP at the panel's portrait size.
bool validCardBitmap(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("CARDS", path, file)) return false;
  uint8_t header[BMP_HEADER_BYTES];
  const bool read = file.read(header, sizeof(header)) == static_cast<int>(sizeof(header));
  file.close();
  if (!read || header[0] != 'B' || header[1] != 'M') return false;
  const auto width = static_cast<int32_t>(readLE32(header + 18));
  const auto height = static_cast<int32_t>(readLE32(header + 22));
  const int bpp = header[28] | (header[29] << 8);
  // Outside the Cards screen the panel size is unknown; the phone then sizes
  // by device model, as for wallpapers.
  if (bpp != 1) return false;
  if (CARD_STORE.screenWidth() == 0) return width > 0 && height != 0;
  return width == CARD_STORE.screenWidth() &&
         (height == CARD_STORE.screenHeight() || height == -CARD_STORE.screenHeight());
}

void handleImageData(WebServer& server) {
  HTTPUpload& part = server.upload();
  resetTaskWatchdogIfSubscribed();
  if (part.status == UPLOAD_FILE_START) {
    upload.file.close();
    upload.bytes = 0;
    upload.failed = !Storage.ensureDirectoryExists("/.crosspoint/cards") ||
                    !Storage.openFileForWrite("CARDS", UPLOAD_TMP, upload.file);
  } else if (part.status == UPLOAD_FILE_WRITE && !upload.failed) {
    upload.bytes += part.currentSize;
    upload.failed = upload.bytes > MAX_IMAGE_BYTES ||
                    upload.file.write(part.buf, part.currentSize) != static_cast<int>(part.currentSize);
  } else if (part.status == UPLOAD_FILE_END || part.status == UPLOAD_FILE_ABORTED) {
    upload.file.close();
    if (part.status == UPLOAD_FILE_ABORTED) upload.failed = true;
  }
}

void handleImageDone(WebServer& server) {
  upload.file.close();
  if (upload.failed || !validCardBitmap(UPLOAD_TMP)) {
    Storage.remove(UPLOAD_TMP);
    return sendError(server, 400, "Expected a 1-bit BMP the size of the screen");
  }

  std::string id = server.arg("id").c_str();
  Card existing;
  const int index = id.empty() ? -1 : CARD_STORE.indexOf(id);
  const bool replace = index >= 0 && CARD_STORE.cardAt(index, existing) && existing.type == CardType::Image;
  if (!replace) {
    Card card;
    card.type = CardType::Image;
    card.title = server.arg("title").c_str();
    if (!CARD_STORE.add(card, id)) {
      Storage.remove(UPLOAD_TMP);
      return sendError(server, 400, "Could not add the card (title too long, or full)");
    }
  }
  const std::string path = cards::CardStore::imagePath(id);
  if (!Storage.replaceFile(UPLOAD_TMP, path.c_str())) {
    if (!replace) CARD_STORE.remove(id);
    return sendError(server, 500, "Could not store the picture");
  }
  // A new picture is a change worth redrawing even when the list is the same.
  if (replace) CARD_STORE.update(existing);
  sendCards(server);
}

void handleImageGet(WebServer& server) {
  const std::string id = server.arg("id").c_str();
  HalFile file;
  if (!cards::CardStore::validId(id) ||
      !Storage.openFileForRead("CARDS", cards::CardStore::imagePath(id).c_str(), file)) {
    return sendError(server, 404, "No such picture");
  }
  server.setContentLength(file.size());
  server.send(200, "image/bmp", "");
  char buffer[STREAM_CHUNK];
  int read;
  while ((read = file.read(buffer, sizeof(buffer))) > 0) {
    server.sendContent(buffer, static_cast<size_t>(read));
    resetTaskWatchdogIfSubscribed();
  }
  file.close();
}
}  // namespace

void registerCardsApi(WebServer& server) {
  CARD_STORE.ensureLoaded();
  WebServer* s = &server;
  server.on("/api/cards", HTTP_GET, [s] { handleGet(*s); });
  server.on("/api/cards", HTTP_POST, [s] { handleSave(*s); });
  server.on("/api/cards/delete", HTTP_POST, [s] { handleDelete(*s); });
  server.on("/api/cards/order", HTTP_POST, [s] { handleOrder(*s); });
  server.on("/api/cards/show", HTTP_POST, [s] { handleShow(*s); });
  server.on("/api/cards/image", HTTP_POST, [s] { handleImageDone(*s); }, [s] { handleImageData(*s); });
  server.on("/api/cards/image", HTTP_GET, [s] { handleImageGet(*s); });
}
