#pragma once

#include <I18n.h>
#include <LegacyOpdsParser.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Built-in free catalogs for Get Books. Each is a plain OPDS/Atom URL the
// device fetches directly; nothing sits between the reader and the library.
namespace getbooks {

struct Source {
  StrId title;
  StrId description;
  // Feed URL. Search sources carry a "{q}" placeholder for the query.
  const char* url;
  StrId origin;  // short catalog name for the details screen
};

inline constexpr char QUERY_PLACEHOLDER[] = "{q}";
// Scratch files for the cover on the details screen: the download, then the
// dithered 1-bit bitmap the screen draws.
inline constexpr char COVER_JPG_PATH[] = "/.crosspoint/getbooks_cover.jpg";
inline constexpr char COVER_BMP_PATH[] = "/.crosspoint/getbooks_cover.bmp";
inline constexpr int COVER_WIDTH = 150;
inline constexpr int COVER_HEIGHT = 225;

inline constexpr Source SOURCES[] = {
    {StrId::STR_GB_SEARCH_PG, StrId::STR_GB_SEARCH_PG_DESC, "https://www.gutenberg.org/ebooks/search.opds/?query={q}",
     StrId::STR_GB_ORIGIN_PG},
    {StrId::STR_GB_SEARCH_SE, StrId::STR_GB_SEARCH_SE_DESC,
     "https://standardebooks.org/feeds/opds/all?query={q}&per-page=24", StrId::STR_GB_ORIGIN_SE},
    {StrId::STR_GB_POPULAR_PG, StrId::STR_GB_POPULAR_PG_DESC,
     "https://www.gutenberg.org/ebooks/search.opds/?sort_order=downloads", StrId::STR_GB_ORIGIN_PG},
    {StrId::STR_GB_NEW_SE, StrId::STR_GB_NEW_SE_DESC, "https://standardebooks.org/feeds/atom/new-releases",
     StrId::STR_GB_ORIGIN_SE},
    {StrId::STR_GB_RECENT_PG, StrId::STR_GB_RECENT_PG_DESC,
     "https://www.gutenberg.org/ebooks/search.opds/?sort_order=release_date", StrId::STR_GB_ORIGIN_PG},
};
inline constexpr int SOURCE_COUNT = sizeof(SOURCES) / sizeof(SOURCES[0]);

inline bool isSearch(const Source& source) { return strstr(source.url, QUERY_PLACEHOLDER) != nullptr; }

// The source URL with `query` percent-encoded into its placeholder.
std::string searchUrl(const Source& source, const std::string& query);

// True for rows worth listing: downloadable books, and navigation rows that
// open a single-book feed (Gutenberg's "/ebooks/1342.opds"). Subject/author
// indexes and similar sub-catalogs are dropped.
bool isBookRow(const legacy_opds::OpdsEntry& entry);

// Cover image for a book link from a built-in catalog, derived from the
// catalogs' stable URL layouts; empty when unknown. Gutenberg's medium cover
// (200px wide) is used because its thumbnail is only 66px.
std::string coverUrlFor(const std::string& href);

// "Austen, Jane" -> "Jane Austen"; anything else is returned unchanged.
std::string displayAuthor(const std::string& author);

// "545 KB", "1.3 MB"; empty for 0.
void formatSize(uint32_t bytes, char* out, size_t outSize);

}  // namespace getbooks
