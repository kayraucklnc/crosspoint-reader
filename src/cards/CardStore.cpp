#include "CardStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>

namespace cards {

namespace {
constexpr const char* DIR = "/.crosspoint/cards";
constexpr const char* LIST_PATH = "/.crosspoint/cards/cards.json";
constexpr const char* TMP_PATH = "/.crosspoint/cards/cards.json.tmp";
constexpr const char* BAD_PATH = "/.crosspoint/cards/cards.json.bad";
constexpr size_t ID_LEN = 8;

const char* typeName(const CardType type) { return type == CardType::Image ? "image" : "qr"; }

std::string clipped(const char* text, const size_t maxLen) {
  std::string out = text ? text : "";
  if (out.size() > maxLen) out.resize(utf8SafeTruncateBuffer(out.c_str(), static_cast<int>(maxLen)));
  return out;
}

bool validCard(const Card& card) {
  if (card.title.size() > MAX_TITLE || card.subtitle.size() > MAX_TITLE) return false;
  if (card.type == CardType::Qr) return !card.payload.empty() && card.payload.size() <= MAX_PAYLOAD;
  return true;
}
}  // namespace

CardStore& CardStore::instance() {
  static CardStore store;
  return store;
}

void CardStore::ensureLoaded() {
  bool needed;
  {
    std::lock_guard<std::mutex> lock(mutex);
    needed = !loaded;
  }
  if (needed) load();
}

void CardStore::load() {
  std::lock_guard<std::mutex> lock(mutex);
  loaded = true;
  readFailed = false;
  cardList.clear();
  // A power cut between replacing and renaming leaves only the new list.
  const char* path = Storage.exists(LIST_PATH) ? LIST_PATH : TMP_PATH;
  if (!Storage.exists(path)) return;
  const String raw = Storage.readFile(path);
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, raw.c_str());
  if (error == DeserializationError::NoMemory) {
    // Not the file's fault: try again next time, and keep it safe until then.
    LOG_ERR("CARDS", "No memory to read %s", path);
    loaded = false;
    readFailed = true;
    return;
  }
  if (error) {
    LOG_ERR("CARDS", "Unreadable %s, kept as %s", path, BAD_PATH);
    Storage.remove(BAD_PATH);
    Storage.rename(path, BAD_PATH);
    return;
  }
  for (JsonObjectConst item : doc["cards"].as<JsonArrayConst>()) {
    if (cardList.size() >= MAX_CARDS) break;
    Card card;
    card.id = clipped(item["id"] | "", ID_LEN);
    if (!validId(card.id)) continue;
    card.type = strcmp(item["type"] | "qr", "image") == 0 ? CardType::Image : CardType::Qr;
    card.title = clipped(item["title"] | "", MAX_TITLE);
    card.subtitle = clipped(item["subtitle"] | "", MAX_TITLE);
    card.payload = clipped(item["payload"] | "", MAX_PAYLOAD);
    if (validCard(card)) cardList.push_back(std::move(card));
  }
  rev++;
}

size_t CardStore::count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return cardList.size();
}

bool CardStore::cardAt(const size_t index, Card& out) const {
  std::lock_guard<std::mutex> lock(mutex);
  if (index >= cardList.size()) return false;
  out = cardList[index];
  return true;
}

int CardStore::indexOf(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex);
  for (size_t i = 0; i < cardList.size(); i++) {
    if (cardList[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

std::vector<Card> CardStore::all() const {
  std::lock_guard<std::mutex> lock(mutex);
  return cardList;
}

bool CardStore::add(Card card, std::string& idOut) {
  if (!validCard(card)) return false;
  std::lock_guard<std::mutex> lock(mutex);
  if (cardList.size() >= MAX_CARDS) return false;
  card.id = newIdLocked();
  cardList.push_back(card);
  if (!saveLocked()) {
    cardList.pop_back();
    return false;
  }
  idOut = card.id;
  rev++;
  return true;
}

bool CardStore::update(const Card& card) {
  if (!validCard(card)) return false;
  std::lock_guard<std::mutex> lock(mutex);
  const auto it = std::find_if(cardList.begin(), cardList.end(), [&](const Card& c) { return c.id == card.id; });
  if (it == cardList.end()) return false;
  const Card previous = *it;
  *it = card;
  it->type = previous.type;  // a card keeps its kind; the picture file belongs to it
  if (!saveLocked()) {
    *it = previous;
    return false;
  }
  rev++;
  return true;
}

bool CardStore::remove(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex);
  const auto it = std::find_if(cardList.begin(), cardList.end(), [&](const Card& c) { return c.id == id; });
  if (it == cardList.end()) return false;
  const Card removed = *it;
  const auto position = cardList.erase(it);
  if (!saveLocked()) {
    cardList.insert(position, removed);
    return false;
  }
  if (removed.type == CardType::Image) Storage.remove(imagePath(removed.id).c_str());
  rev++;
  return true;
}

bool CardStore::reorder(const std::vector<std::string>& ids) {
  std::lock_guard<std::mutex> lock(mutex);
  if (ids.size() != cardList.size()) return false;
  for (size_t i = 0; i < ids.size(); i++) {
    if (std::find(ids.begin(), ids.begin() + i, ids[i]) != ids.begin() + i) return false;  // listed twice
  }
  std::vector<Card> ordered;
  ordered.reserve(cardList.size());
  for (const auto& id : ids) {
    const auto it = std::find_if(cardList.begin(), cardList.end(), [&](const Card& c) { return c.id == id; });
    if (it == cardList.end()) return false;
    ordered.push_back(*it);
  }
  ordered.swap(cardList);
  if (!saveLocked()) {
    ordered.swap(cardList);
    return false;
  }
  rev++;
  return true;
}

uint32_t CardStore::revision() const {
  std::lock_guard<std::mutex> lock(mutex);
  return rev;
}

void CardStore::requestShow(const int index) {
  std::lock_guard<std::mutex> lock(mutex);
  pendingShow = index;
}

int CardStore::takeShowRequest() {
  std::lock_guard<std::mutex> lock(mutex);
  const int index = pendingShow;
  pendingShow = -1;
  return index;
}

void CardStore::setScreenSize(const int w, const int h) {
  std::lock_guard<std::mutex> lock(mutex);
  width = w;
  height = h;
}

int CardStore::screenWidth() const {
  std::lock_guard<std::mutex> lock(mutex);
  return width;
}

int CardStore::screenHeight() const {
  std::lock_guard<std::mutex> lock(mutex);
  return height;
}

std::string CardStore::imagePath(const std::string& id) { return std::string(DIR) + "/" + id + ".bmp"; }

bool CardStore::validId(const std::string& id) {
  return id.size() == ID_LEN && std::all_of(id.begin(), id.end(), [](const char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// Written to a temp file and moved over the list, so a power cut mid-write
// keeps the previous cards (load() falls back to the temp file if needed).
bool CardStore::saveLocked() const {
  if (readFailed) return false;
  JsonDocument doc;
  doc["v"] = 1;
  JsonArray list = doc["cards"].to<JsonArray>();
  for (const auto& card : cardList) {
    JsonObject item = list.add<JsonObject>();
    item["id"] = card.id;
    item["type"] = typeName(card.type);
    if (!card.title.empty()) item["title"] = card.title;
    if (!card.subtitle.empty()) item["subtitle"] = card.subtitle;
    if (!card.payload.empty()) item["payload"] = card.payload;
  }
  String out;
  if (doc.overflowed() || serializeJson(doc, out) == 0 || out.isEmpty()) {
    LOG_ERR("CARDS", "No memory to save the cards");
    return false;
  }
  if (!Storage.ensureDirectoryExists(DIR) || !Storage.writeFile(TMP_PATH, out)) {
    LOG_ERR("CARDS", "Failed to write %s", TMP_PATH);
    return false;
  }
  if (!Storage.replaceFile(TMP_PATH, LIST_PATH)) {
    LOG_ERR("CARDS", "Failed to replace %s", LIST_PATH);
    return false;
  }
  return true;
}

std::string CardStore::newIdLocked() const {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  std::string id;
  do {
    uint8_t bytes[ID_LEN / 2];
    esp_fill_random(bytes, sizeof(bytes));
    id.clear();
    for (const uint8_t b : bytes) {
      id += HEX_DIGITS[b >> 4];
      id += HEX_DIGITS[b & 15];
    }
  } while (std::any_of(cardList.begin(), cardList.end(), [&](const Card& c) { return c.id == id; }));
  return id;
}

}  // namespace cards
