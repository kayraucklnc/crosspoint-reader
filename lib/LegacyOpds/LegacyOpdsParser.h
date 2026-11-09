#pragma once
#include <Print.h>
#include <expat.h>

#include <cstdint>
#include <string>
#include <vector>

// Get Books keeps this Atom parser (with entry summaries); the OPDS browser
// uses the SDK parser, which shares these type names.
namespace legacy_opds {

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
  // <summary>, else <content>; for XHTML content a "Summary:" paragraph wins.
  std::string summary;
  uint32_t size = 0;  // acquisition link's length="" in bytes, 0 when unknown
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  OpdsParser();
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  // Collect <summary>/<content> text up to `chars` per entry (0, the default,
  // skips it so plain catalog browsing pays nothing for descriptions).
  void setSummaryLimit(size_t chars) { summaryLimit = chars; }

  bool error() const;
  bool truncated() const { return feedTruncated; }

  operator bool() { return !error(); }

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }

  /**
   * Get only book entries (legacy compatibility).
   * @return Vector of book entries
   */
  std::vector<OpdsEntry> getBooks() const;

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);
  static void assignBounded(std::string& target, const char* value, size_t maxLen);
  static void appendBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  XML_Parser parser = nullptr;
  std::vector<OpdsEntry> entries;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;
  bool collectCurrentEntry = false;
  // Summary/content collection. XHTML content arrives as <p> paragraphs:
  // paragraphText holds the open one so a "Summary:" paragraph can replace
  // the text gathered so far even after that text reached its cap.
  size_t summaryLimit = 0;
  size_t summaryBudget = 0;  // chars the open element may still add
  uint8_t summaryDepth = 0;
  bool summaryHtml = false;    // type="html": markup arrives as escaped text
  bool summaryAppend = false;  // <content> extending a short <summary>  // element nesting inside <summary>/<content>;
                               // 0 = not collecting
  bool inParagraph = false;
  bool summaryLocked = false;  // a "Summary:" paragraph was found; ignore the rest
  std::string paragraphText;
  void finishParagraph();

  bool errorOccured = false;
  bool feedTruncated = false;
};

}  // namespace legacy_opds
