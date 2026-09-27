#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t ENTRY_STORAGE_CAPACITY = 64;
constexpr size_t MAX_ENTRIES = ENTRY_STORAGE_CAPACITY - 2;
constexpr size_t MAX_TITLE_CHARS = 160;
constexpr size_t MAX_AUTHOR_CHARS = 120;
constexpr size_t MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_SEARCH_TEMPLATE_CHARS = 768;
constexpr size_t MAX_PAGE_URL_CHARS = 768;
constexpr char SUMMARY_PREFIX[] = "Summary:";

bool hasLocalName(const char* name, const char* local) {
  if (strcmp(name, local) == 0) return true;
  const char* colon = strrchr(name, ':');
  return colon && strcmp(colon + 1, local) == 0;
}

bool isSummaryElement(const char* name) { return hasLocalName(name, "summary") || hasLocalName(name, "content"); }

// Cuts text that hit the summary limit back to a word boundary and marks the cut.
std::string finishSummary(std::string text) {
  const size_t space = text.rfind(' ');
  if (space != std::string::npos && space > text.size() / 2) text.resize(space);
  while (!text.empty() && (text.back() == ' ' || text.back() == ',' || text.back() == ';')) text.pop_back();
  return text + "\xE2\x80\xA6";
}

// Drops <...> markup from type="html" text; paragraph and line breaks become
// spaces, inline tags (<i>, <a>) vanish so punctuation stays attached.
std::string stripTags(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool inTag = false;
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (c == '<') {
      inTag = true;
      const size_t name = i + 1 < text.size() && text[i + 1] == '/' ? i + 2 : i + 1;
      if (name < text.size() && (text[name] == 'p' || text[name] == 'b' || text[name] == 'd')) out += ' ';
    } else if (c == '>') {
      inTag = false;
    } else if (!inTag) {
      out += c;
    }
  }
  return out;
}

// Trims and folds runs of ASCII whitespace into single spaces.
std::string collapseWhitespace(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  for (const char c : text) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) out += ' ';
    pendingSpace = false;
    out += c;
  }
  return out;
}
}  // namespace

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
    return;
  }
  entries.reserve(ENTRY_STORAGE_CAPACITY);
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
  collectCurrentEntry = false;
  feedTruncated = false;
  summaryDepth = 0;
  inParagraph = summaryLocked = false;
  paragraphText.clear();
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void OpdsParser::assignBounded(std::string& target, const char* value, const size_t maxLen) {
  if (!value) {
    target.clear();
    return;
  }
  target.assign(value, strnlen(value, maxLen));
}

void OpdsParser::appendBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  if (target.size() >= maxLen) return;
  const size_t remaining = maxLen - target.size();
  target.append(value, len < remaining ? len : remaining);
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->collectCurrentEntry = self->entries.size() < MAX_ENTRIES;
    self->feedTruncated = self->feedTruncated || !self->collectCurrentEntry;
    self->currentEntry = OpdsEntry{};
    self->currentText.clear();
    self->inTitle = self->inAuthor = self->inAuthorName = self->inId = false;
    self->summaryDepth = 0;
    self->inParagraph = self->summaryLocked = false;
    self->paragraphText.clear();
    return;
  }

  // Markup inside <summary>/<content> (XHTML paragraphs, links) is text, not
  // entry metadata.
  if (self->summaryDepth > 0) {
    if (self->summaryDepth < UINT8_MAX) self->summaryDepth++;
    if (hasLocalName(name, "p")) {
      self->inParagraph = true;
      self->paragraphText.clear();
    }
    return;
  }

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        if (strstr(href, "{searchTerms}") != nullptr) {
          assignBounded(self->searchTemplate, href, MAX_SEARCH_TEMPLATE_CHARS);
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        assignBounded(self->nextPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        assignBounded(self->prevPageUrl, href, MAX_PAGE_URL_CHARS);
      }

      if (self->inEntry && self->collectCurrentEntry) {
        // rel="enclosure" is how plain Atom feeds (Standard Ebooks) attach files.
        const bool acquisition =
            rel && (strstr(rel, "opds-spec.org/acquisition") != nullptr || strcmp(rel, "enclosure") == 0);
        if (acquisition && type && strcmp(type, "application/epub+zip") == 0) {
          // Prefer plain EPUB links over derived formats when multiple
          // acquisition links are present for one entry.
          const bool isPlainEpub = strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr;
          const bool alreadyHasPlainEpub = self->currentEntry.type == OpdsEntryType::BOOK &&
                                           (self->currentEntry.href.find(".epub") != std::string::npos ||
                                            self->currentEntry.href.find("/epub/") != std::string::npos);
          if (self->currentEntry.type != OpdsEntryType::BOOK || (isPlainEpub && !alreadyHasPlainEpub)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
            const char* length = findAttribute(atts, "length");
            self->currentEntry.size = length ? static_cast<uint32_t>(strtoul(length, nullptr, 10)) : 0;
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
          }
        }
      }
    }
  }

  if (!self->inEntry || !self->collectCurrentEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (self->summaryLimit > 0 && isSummaryElement(name) && !self->summaryLocked &&
             (self->currentEntry.summary.empty() ||
              (hasLocalName(name, "content") && self->currentEntry.summary.size() < self->summaryLimit / 2))) {
    // A short <summary> (Standard Ebooks' one-liner) is extended by the
    // <content> description that follows it.
    self->summaryAppend = !self->currentEntry.summary.empty();
    self->summaryBudget = self->summaryLimit - self->currentEntry.summary.size();
    const char* type = findAttribute(atts, "type");
    self->summaryHtml = type && strcmp(type, "html") == 0;
    self->summaryDepth = 1;
    self->inParagraph = false;
    self->currentText.clear();
    self->paragraphText.clear();
  }
}

void OpdsParser::finishParagraph() {
  inParagraph = false;
  if (summaryLocked) return;
  const bool cut = paragraphText.size() >= summaryBudget;
  const std::string paragraph = collapseWhitespace(paragraphText);
  paragraphText.clear();
  if (paragraph.rfind(SUMMARY_PREFIX, 0) == 0) {
    const std::string text = collapseWhitespace(paragraph.substr(sizeof(SUMMARY_PREFIX) - 1));
    currentEntry.summary = cut ? finishSummary(text) : text;
    summaryLocked = true;
    return;
  }
  if (!currentText.empty()) appendBounded(currentText, " ", 1, summaryBudget);
  appendBounded(currentText, paragraph.c_str(), paragraph.size(), summaryBudget);
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (self->summaryDepth > 1) {
    if (self->inParagraph && hasLocalName(name, "p")) self->finishParagraph();
    self->summaryDepth--;
    return;
  }
  if (self->summaryDepth == 1) {
    if (self->inParagraph) self->finishParagraph();
    if (!self->summaryLocked) {
      const bool cut = self->currentText.size() >= self->summaryBudget;
      std::string text = collapseWhitespace(self->summaryHtml ? stripTags(self->currentText) : self->currentText);
      if (cut) text = finishSummary(std::move(text));
      if (!self->summaryAppend) {
        self->currentEntry.summary = std::move(text);
      } else if (!text.empty()) {
        self->currentEntry.summary += ' ';
        self->currentEntry.summary += text;
      }
    }
    self->summaryAppend = self->summaryHtml = false;
    self->summaryDepth = 0;
    self->currentText.clear();
    return;
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (self->collectCurrentEntry && !self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      self->entries.push_back(self->currentEntry);
    }
    self->inEntry = false;
    self->collectCurrentEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = collapseWhitespace(self->currentText);
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = collapseWhitespace(self->currentText);
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (!self->collectCurrentEntry) return;
  if (self->summaryDepth > 0) {
    if (self->summaryLocked) return;
    appendBounded(self->inParagraph ? self->paragraphText : self->currentText, s, len, self->summaryBudget);
    return;
  }
  if (self->inTitle) {
    appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
  } else if (self->inAuthorName) {
    appendBounded(self->currentText, s, len, MAX_AUTHOR_CHARS);
  } else if (self->inId) {
    appendBounded(self->currentText, s, len, MAX_ID_CHARS);
  }
}
