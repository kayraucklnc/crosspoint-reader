#include "LibraryMetaStore.h"

#include <Logging.h>

#include <algorithm>

void LibraryMetaStore::toJson(JsonDocument& doc) const {
  doc["v"] = 1;
  doc["view"] = view;
  doc["sort"] = sort;
  doc["filter"] = filter;
  JsonObject obj = doc["books"].to<JsonObject>();
  for (const Entry& e : books) {
    JsonObject book = obj[e.path.c_str()].to<JsonObject>();
    if (e.flags & ARCHIVED) book["archived"] = true;
    if (e.flags & FINISHED) book["finished"] = true;
  }
}

bool LibraryMetaStore::fromJson(JsonVariantConst doc) {
  view = std::min<uint8_t>(doc["view"] | 0, VIEW_COUNT - 1);
  sort = std::min<uint8_t>(doc["sort"] | 0, SORT_COUNT - 1);
  filter = std::min<uint8_t>(doc["filter"] | 0, FILTER_COUNT - 1);
  books.clear();
  JsonObjectConst obj = doc["books"].as<JsonObjectConst>();
  books.reserve(std::min(obj.size(), MAX_ENTRIES));
  for (JsonPairConst pair : obj) {
    if (books.size() >= MAX_ENTRIES) break;
    uint8_t flags = 0;
    if (pair.value()["archived"] | false) flags |= ARCHIVED;
    if (pair.value()["finished"] | false) flags |= FINISHED;
    if (flags != 0) books.push_back({pair.key().c_str(), flags});
  }
  LOG_DBG("LMS", "Library meta loaded (%d flagged books)", static_cast<int>(books.size()));
  return true;
}

uint8_t LibraryMetaStore::flagsFor(const std::string& path) const {
  for (const Entry& e : books) {
    if (e.path == path) return e.flags;
  }
  return 0;
}

uint8_t LibraryMetaStore::toggle(const std::string& path, const Flag flag) {
  auto it = std::find_if(books.begin(), books.end(), [&](const Entry& e) { return e.path == path; });
  uint8_t flags;
  if (it == books.end()) {
    if (books.size() >= MAX_ENTRIES) {
      LOG_ERR("LMS", "Library meta full; flag not stored");
      return 0;
    }
    books.push_back({path, flag});
    flags = flag;
  } else {
    it->flags ^= flag;
    flags = it->flags;
    if (flags == 0) books.erase(it);
  }
  if (!saveToFile()) LOG_ERR("LMS", "Failed to save library meta");
  return flags;
}

void LibraryMetaStore::savePrefs(const uint8_t newView, const uint8_t newSort, const uint8_t newFilter) {
  if (newView == view && newSort == sort && newFilter == filter) return;
  view = newView;
  sort = newSort;
  filter = newFilter;
  if (!saveToFile()) LOG_ERR("LMS", "Failed to save library prefs");
}
