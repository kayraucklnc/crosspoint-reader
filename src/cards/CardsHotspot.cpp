#include "CardsHotspot.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include "network/CrossPointWebServer.h"
#include "util/TaskWatchdog.h"

namespace cards {

namespace {
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr uint16_t DNS_PORT = 53;
// Requests handled per service() call; enough for an image upload to stream
// without starving the activity's input handling.
constexpr int MAX_REQUESTS_PER_SERVICE = 64;

DNSServer* dnsServer = nullptr;

void stopDns() {
  if (!dnsServer) return;
  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}
}  // namespace

CardsHotspot::CardsHotspot() = default;

CardsHotspot::~CardsHotspot() { stop(); }

bool CardsHotspot::start(const GfxRenderer& renderer) {
  if (running()) return true;
  // WiFi plus the server need a large contiguous block; SD-font caches are rebuildable.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();

  used = true;
  WiFi.mode(WIFI_AP);
  delay(100);
  if (!WiFi.softAP(SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    LOG_ERR("CARDS", "Hotspot failed to start");
    stop();
    return false;
  }
  delay(100);
  const IPAddress apIp = WiFi.softAPIP();
  address = apIp.toString().c_str();

  MDNS.end();
  if (!MDNS.begin(HOSTNAME)) LOG_DBG("CARDS", "mDNS failed to start");
  stopDns();
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIp);

  server.reset(new CrossPointWebServer());
  server->begin();
  if (!server->isRunning()) {
    LOG_ERR("CARDS", "Web server failed to start");
    stop();
    return false;
  }
  LOG_DBG("CARDS", "Hotspot up at %s, free heap %d", address.c_str(), ESP.getFreeHeap());
  return true;
}

void CardsHotspot::service() {
  if (dnsServer) dnsServer->processNextRequest();
  if (!server || !server->isRunning()) return;
  resetTaskWatchdogIfSubscribed();
  for (int i = 0; i < MAX_REQUESTS_PER_SERVICE && server->isRunning(); i++) {
    server->handleClient();
    if ((i & 0x1F) == 0x1F) resetTaskWatchdogIfSubscribed();
  }
}

void CardsHotspot::stop() {
  stopDns();
  server.reset();
  address.clear();
  if (!used) return;
  MDNS.end();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

bool CardsHotspot::running() const { return server && server->isRunning(); }

std::string CardsHotspot::editorUrl() const { return "http://" + address + "/#cards"; }

}  // namespace cards
