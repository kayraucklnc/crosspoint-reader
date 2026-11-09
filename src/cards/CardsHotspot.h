#pragma once

#include <memory>
#include <string>

class CrossPointWebServer;
class GfxRenderer;

namespace cards {

// The reader's own Wi-Fi network with the web app on it, for editing cards
// from a phone: an open access point, a captive-portal DNS that answers every
// name with the reader, mDNS (crosspoint.local) and the regular web server.
class CardsHotspot {
 public:
  static constexpr const char* SSID = "CrossPoint-Reader";
  static constexpr const char* HOSTNAME = "crosspoint";

  CardsHotspot();
  ~CardsHotspot();
  CardsHotspot(const CardsHotspot&) = delete;
  CardsHotspot& operator=(const CardsHotspot&) = delete;

  // Brings the network and server up; false (and everything torn down) on failure.
  bool start(const GfxRenderer& renderer);
  // Serves pending DNS and HTTP requests; call from the activity loop.
  void service();
  void stop();

  bool running() const;
  // True once WiFi was started in this session: leaving needs the heap-defrag restart.
  bool wasUsed() const { return used; }
  const std::string& ip() const { return address; }
  // http://<ip>/#cards: opens the web app straight on the Cards panel.
  std::string editorUrl() const;

 private:
  std::unique_ptr<CrossPointWebServer> server;
  std::string address;
  bool used = false;
};

}  // namespace cards
