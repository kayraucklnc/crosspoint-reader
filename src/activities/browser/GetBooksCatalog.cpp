#include "GetBooksCatalog.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace getbooks {

std::string searchUrl(const Source& source, const std::string& query) {
  std::string encoded;
  encoded.reserve(query.size() * 3);
  for (const unsigned char c : query) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else if (c == ' ') {
      encoded += '+';
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      encoded += hex;
    }
  }
  std::string url = source.url;
  const size_t at = url.find(QUERY_PLACEHOLDER);
  if (at != std::string::npos) url.replace(at, sizeof(QUERY_PLACEHOLDER) - 1, encoded);
  return url;
}

bool isBookRow(const OpdsEntry& entry) {
  if (entry.type == OpdsEntryType::BOOK) return true;
  const size_t at = entry.href.find("/ebooks/");
  if (at == std::string::npos) return false;
  const size_t digit = at + strlen("/ebooks/");
  return digit < entry.href.size() && isdigit(static_cast<unsigned char>(entry.href[digit]));
}

std::string coverUrlFor(const std::string& href) {
  constexpr char PG_BOOK[] = "gutenberg.org/ebooks/";
  size_t at = href.find(PG_BOOK);
  if (at != std::string::npos) {
    at += sizeof(PG_BOOK) - 1;
    size_t end = at;
    while (end < href.size() && isdigit(static_cast<unsigned char>(href[end]))) end++;
    if (end == at || end - at > 7) return {};
    const std::string number = href.substr(at, end - at);
    return "https://www.gutenberg.org/cache/epub/" + number + "/pg" + number + ".cover.medium.jpg";
  }
  constexpr char SE_DOWNLOADS[] = "/downloads/";
  at = href.find(SE_DOWNLOADS);
  if (href.find("standardebooks.org/") != std::string::npos && at != std::string::npos) {
    return href.substr(0, at + sizeof(SE_DOWNLOADS) - 1) + "cover-thumbnail.jpg";
  }
  return {};
}

std::string displayAuthor(const std::string& author) {
  const size_t comma = author.find(", ");
  if (comma == std::string::npos || comma == 0 || author.find(',', comma + 1) != std::string::npos) return author;
  return author.substr(comma + 2) + " " + author.substr(0, comma);
}

void formatSize(const uint32_t bytes, char* out, const size_t outSize) {
  if (outSize == 0) return;
  if (bytes == 0) {
    out[0] = '\0';
  } else if (bytes < 1024 * 1024) {
    snprintf(out, outSize, "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
  } else {
    const unsigned tenths = static_cast<unsigned>((static_cast<uint64_t>(bytes) * 10 + 512 * 1024) / (1024 * 1024));
    snprintf(out, outSize, "%u.%u MB", tenths / 10, tenths % 10);
  }
}

}  // namespace getbooks
