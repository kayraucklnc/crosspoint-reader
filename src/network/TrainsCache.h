#pragma once

#include "TrenordAlerts.h"
#include "TrenordClient.h"

// User route choices, edited from the web UI (/.crosspoint/trains.json).
struct TrainsConfig {
  static constexpr const char* PATH = "/.crosspoint/trains.json";
  static constexpr int MAX_FAVORITES = 6;

  int8_t origin = trenord::HOME;               // default board: origin -> towards
  int8_t towards = trenord::DEFAULT_TOWARDS;
  int8_t home = trenord::HOME;                 // return trips go towards this station
  int8_t favorites[MAX_FAVORITES] = {-1, -1, -1, -1, -1, -1};  // strip order, first

  bool load();
  bool save() const;
  // Clamps indices into range and drops duplicates or invalid favorites.
  void sanitize();
};

// Last boards and recently used return stations, persisted on SD so the
// trains page can draw instantly before the network refresh lands.
struct TrainsCache {
  static constexpr const char* PATH = "/.crosspoint/trains_cache.json";
  static constexpr int MAX_RECENT = 4;

  trenord::Board outbound;  // home -> default direction
  bool hasOutbound = false;
  trenord::Board inbound;  // last return board (towards home)
  bool hasInbound = false;
  int8_t recent[MAX_RECENT] = {-1, -1, -1, -1};  // most recent first
  trenord_alerts::Alerts alerts;  // line notices and strikes, refreshed at most every 30 min

  bool load();
  bool save() const;
  void markRecent(int station);
};
