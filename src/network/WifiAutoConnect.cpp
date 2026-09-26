#include "WifiAutoConnect.h"

#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "WifiCredentialStore.h"

namespace wifi_auto_connect {
namespace {
constexpr unsigned long CONNECT_TIMEOUT_MS = 7000;  // matches WifiSelectionActivity's auto-connect budget
constexpr size_t MAX_CANDIDATES = 8;

bool tryJoin(const WifiCredential& cred) {
  LOG_DBG("WIFI", "Background join: %s", cred.ssid.c_str());
  if (cred.password.empty()) {
    WiFi.begin(cred.ssid.c_str());
  } else {
    WiFi.begin(cred.ssid.c_str(), cred.password.c_str());
  }
  const unsigned long start = millis();
  while (millis() - start < CONNECT_TIMEOUT_MS) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) return true;
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
    delay(100);
  }
  WiFi.disconnect();
  return false;
}
}  // namespace

Result connect() {
  if (WiFi.status() == WL_CONNECTED) return Result::CONNECTED;
  if (WIFI_STORE.getCredentialCount() == 0) return Result::NO_SAVED_NETWORKS;

  WiFi.mode(WIFI_STA);
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    const auto cred = WIFI_STORE.findCredential(lastSsid);
    if (cred && tryJoin(*cred)) return Result::CONNECTED;
  }

  struct Candidate {
    std::string ssid;
    int32_t rssi;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(MAX_CANDIDATES);
  const int found = WiFi.scanNetworks();
  for (int i = 0; i < found && candidates.size() < MAX_CANDIDATES; i++) {
    const std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty() || ssid == lastSsid || !WIFI_STORE.hasSavedCredential(ssid)) continue;
    auto it = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& c) { return c.ssid == ssid; });
    if (it == candidates.end()) {
      candidates.push_back({ssid, WiFi.RSSI(i)});
    } else {
      it->rssi = std::max(it->rssi, WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();
  // User priority (saved order) first; signal strength breaks ties.
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    const int pa = WIFI_STORE.priorityOf(a.ssid);
    const int pb = WIFI_STORE.priorityOf(b.ssid);
    return pa != pb ? pa < pb : a.rssi > b.rssi;
  });

  for (const Candidate& candidate : candidates) {
    const auto cred = WIFI_STORE.findCredential(candidate.ssid);
    if (cred && tryJoin(*cred)) {
      WIFI_STORE.setLastConnectedSsid(candidate.ssid);
      return Result::CONNECTED;
    }
  }
  LOG_INF("WIFI", "Background join failed (%u candidates)", static_cast<unsigned>(candidates.size()));
  return Result::FAILED;
}

}  // namespace wifi_auto_connect
