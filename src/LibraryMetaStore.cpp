#include "LibraryMetaStore.h"

#include <Logging.h>

#include <algorithm>

namespace {
constexpr uint8_t ALL_FLAGS = LibraryMetaStore::ARCHIVED | LibraryMetaStore::FINISHED | LibraryMetaStore::UNFINISHED;
}  // namespace

void LibraryMetaStore::toJson(JsonDocument& doc) const {
  doc["v"] = 2;
  doc["sort"] = sort;
  doc["desc"] = descending;
  JsonObject obj = doc["books"].to<JsonObject>();
  for (const Entry& e : books) {
    JsonObject book = obj[e.path.c_str()].to<JsonObject>();
    if (e.flags & ARCHIVED) book["archived"] = true;
    if (e.flags & FINISHED) book["finished"] = true;
    if (e.flags & UNFINISHED) book["unfinished"] = true;
  }
}

bool LibraryMetaStore::fromJson(JsonVariantConst doc) {
  sort = std::min<uint8_t>(doc["sort"] | 0, SORT_COUNT - 1);
  descending = doc["desc"] | DEFAULT_DESCENDING;
  books.clear();
  JsonObjectConst obj = doc["books"].as<JsonObjectConst>();
  books.reserve(std::min(obj.size(), MAX_ENTRIES));
  for (JsonPairConst pair : obj) {
    if (books.size() >= MAX_ENTRIES) break;
    uint8_t flags = 0;
    if (pair.value()["archived"] | false) flags |= ARCHIVED;
    if (pair.value()["finished"] | false) flags |= FINISHED;
    if (pair.value()["unfinished"] | false) flags |= UNFINISHED;
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

uint8_t LibraryMetaStore::updateFlags(const std::string& path, const uint8_t set, const uint8_t clear) {
  auto it = std::find_if(books.begin(), books.end(), [&](const Entry& e) { return e.path == path; });
  const uint8_t old = it == books.end() ? 0 : it->flags;
  const uint8_t flags = static_cast<uint8_t>(((old & ~clear) | set) & ALL_FLAGS);
  if (flags == old) return flags;
  if (it == books.end()) {
    if (books.size() >= MAX_ENTRIES) {
      LOG_ERR("LMS", "Library meta full; flag not stored");
      return old;
    }
    books.push_back({path, flags});
  } else if (flags == 0) {
    books.erase(it);
  } else {
    it->flags = flags;
  }
  if (!saveToFile()) LOG_ERR("LMS", "Failed to save library meta");
  return flags;
}

void LibraryMetaStore::savePrefs(const uint8_t newSort, const uint8_t newDescending) {
  if (newSort == sort && newDescending == descending) return;
  sort = newSort;
  descending = newDescending;
  if (!saveToFile()) LOG_ERR("LMS", "Failed to save library prefs");
}
