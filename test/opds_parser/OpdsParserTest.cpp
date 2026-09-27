#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "OpdsParser.h"

namespace {

// Feeds the document in small chunks, the way a network stream arrives.
std::vector<OpdsEntry> parse(const std::string& xml, OpdsParser& parser) {
  constexpr size_t CHUNK = 37;
  for (size_t at = 0; at < xml.size(); at += CHUNK) {
    const size_t n = std::min(CHUNK, xml.size() - at);
    parser.write(reinterpret_cast<const uint8_t*>(xml.data() + at), n);
  }
  parser.flush();
  EXPECT_FALSE(parser.error());
  return parser.getEntries();
}

std::vector<OpdsEntry> parse(const std::string& xml) {
  OpdsParser parser;
  parser.setSummaryLimit(600);
  return parse(xml, parser);
}

constexpr const char* FEED_OPEN =
    R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:dcterms="http://purl.org/dc/terms/">)";
constexpr const char* FEED_CLOSE = "</feed>";

std::string feed(const std::string& body) { return std::string(FEED_OPEN) + body + FEED_CLOSE; }

}  // namespace

// Gutenberg search results: no <author>, the author rides in a text <content>,
// and each row links to a one-book feed.
TEST(OpdsParserTest, GutenbergSearchEntryKeepsContentAsSummary) {
  const auto entries = parse(feed(R"X(
<entry>
<id>https://www.gutenberg.org/ebooks/1342.opds</id>
<title>Pride and Prejudice</title>
<content type="text">Jane Austen</content>
<link type="application/atom+xml;profile=opds-catalog" rel="subsection" href="/ebooks/1342.opds"/>
<link type="image/png" rel="http://opds-spec.org/image/thumbnail" href="data:image/png;base64,AAAA"/>
</entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(entries[0].title, "Pride and Prejudice");
  EXPECT_EQ(entries[0].summary, "Jane Austen");
  EXPECT_EQ(entries[0].href, "/ebooks/1342.opds");
  EXPECT_EQ(entries[0].size, 0u);
}

// Gutenberg's one-book feed: XHTML content whose "Summary:" paragraph is the
// description, preceded by paragraphs of catalog noise.
TEST(OpdsParserTest, GutenbergBookFeedPicksSummaryParagraphAndLength) {
  const auto entries = parse(feed(R"X(
<entry>
<title>Pride and Prejudice</title>
<content type="xhtml">
<div xmlns="http://www.w3.org/1999/xhtml">
<p>This edition had all images removed.</p>
<p>
Title:
Pride and Prejudice
</p>
<p>
Credits:
Chuck Greif and the Online Distributed Proofreading Team at http:<a href="//www.pgdp.net">//www.pgdp.net</a>
</p>
<p>
Summary:
"Pride and Prejudice" by Jane Austen is a novel published in 1813.
It follows Elizabeth Bennet.
</p>
<p>Author: Austen, Jane, 1775-1817</p>
</div>
</content>
<id>urn:gutenberg:1342:2</id>
<author>
<name>Austen, Jane</name>
</author>
<link type="application/epub+zip" rel="http://opds-spec.org/acquisition" title="EPUB (older e-readers, no images)" length="558381" href="https://www.gutenberg.org/ebooks/1342.epub.noimages"/>
<link type="application/x-mobipocket-ebook" rel="http://opds-spec.org/acquisition" length="540013" href="https://www.gutenberg.org/ebooks/1342.kindle.noimages"/>
</entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_EQ(entries[0].author, "Austen, Jane");
  EXPECT_EQ(entries[0].href, "https://www.gutenberg.org/ebooks/1342.epub.noimages");
  EXPECT_EQ(entries[0].size, 558381u);
  EXPECT_EQ(entries[0].summary,
            "\"Pride and Prejudice\" by Jane Austen is a novel published in 1813. It follows Elizabeth Bennet.");
}

// Without a "Summary:" paragraph the paragraphs themselves are the summary.
TEST(OpdsParserTest, XhtmlContentWithoutSummaryParagraphFallsBackToText) {
  const auto entries = parse(feed(R"X(
<entry>
<title>Some Book</title>
<content type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><p>First  line.</p>
<p>Second
line.</p></div></content>
<link type="application/epub+zip" rel="http://opds-spec.org/acquisition" href="/b.epub"/>
</entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].summary, "First line. Second line.");
}

// Standard Ebooks OPDS: <summary> first, a long HTML <content> after it, and
// several EPUB flavours; the first plain .epub wins.
TEST(OpdsParserTest, StandardEbooksPrefersSummaryOverContent) {
  const auto entries = parse(feed(R"X(
<entry>
<id>https://standardebooks.org/ebooks/ring-lardner/short-fiction</id>
<title>Short Fiction</title>
<author><name>Ring Lardner</name><uri>https://standardebooks.org/ebooks/ring-lardner</uri></author>
<summary type="text">A collection of short stories by Ring Lardner.</summary>
<content type="html">&lt;p&gt;Ring Lardner was a prolific humorist.&lt;/p&gt;</content>
<link href="https://standardebooks.org/x/cover.jpg" rel="http://opds-spec.org/image" type="image/jpeg"/>
<link href="https://standardebooks.org/x/short-fiction.epub?source=feed" length="1350410" rel="http://opds-spec.org/acquisition/open-access" title="Recommended compatible epub" type="application/epub+zip" />
<link href="https://standardebooks.org/x/short-fiction_advanced.epub?source=feed" length="1411823" rel="http://opds-spec.org/acquisition/open-access" type="application/epub+zip" />
<link href="https://standardebooks.org/x/short-fiction.kepub.epub?source=feed" length="1482681" rel="http://opds-spec.org/acquisition/open-access" type="application/kepub+zip" />
</entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_EQ(entries[0].author, "Ring Lardner");
  EXPECT_EQ(entries[0].summary, "A collection of short stories by Ring Lardner. Ring Lardner was a prolific humorist.");
  EXPECT_EQ(entries[0].href, "https://standardebooks.org/x/short-fiction.epub?source=feed");
  EXPECT_EQ(entries[0].size, 1350410u);
}

// Standard Ebooks' public Atom feed marks downloads as rel="enclosure".
TEST(OpdsParserTest, AtomEnclosureEpubIsABook) {
  const auto entries = parse(feed(R"X(
<entry>
<title>The Autobiography of a Super-Tramp</title>
<author><name>W. H. Davies</name></author>
<summary type="text">A destitute tramp wanders.</summary>
<link href="https://standardebooks.org/y" rel="alternate" type="application/xhtml+xml"/>
<link href="https://standardebooks.org/y/tramp.epub?source=feed" length="795863" rel="enclosure" type="application/epub+zip"/>
</entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_EQ(entries[0].href, "https://standardebooks.org/y/tramp.epub?source=feed");
  EXPECT_EQ(entries[0].size, 795863u);
  EXPECT_EQ(entries[0].summary, "A destitute tramp wanders.");
}

TEST(OpdsParserTest, SummaryIsSkippedByDefault) {
  OpdsParser parser;
  const auto entries = parse(feed(R"X(<entry><title>T</title><summary>About T.</summary>
<link type="application/epub+zip" rel="http://opds-spec.org/acquisition" href="/t.epub"/></entry>)X"),
                             parser);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_TRUE(entries[0].summary.empty());
}

TEST(OpdsParserTest, LongSummaryIsCutAtAWord) {
  std::string words;
  while (words.size() < 2000) words += "lorem ipsum ";
  const auto entries = parse(feed(
      "<entry><title>T</title><summary>" + words +
      R"X(</summary><link type="application/epub+zip" rel="http://opds-spec.org/acquisition" href="/t.epub"/></entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  const std::string& summary = entries[0].summary;
  ASSERT_GT(summary.size(), 3u);
  EXPECT_EQ(summary.substr(summary.size() - 3), "\xE2\x80\xA6");
  const std::string body = summary.substr(0, summary.size() - 3);
  EXPECT_TRUE(body.size() >= 5 &&
              (body.compare(body.size() - 5, 5, "lorem") == 0 || body.compare(body.size() - 5, 5, "ipsum") == 0));
}

TEST(OpdsParserTest, SummaryIsBounded) {
  std::string longText(5000, 'a');
  const auto entries = parse(feed(
      "<entry><title>T</title><summary>" + longText +
      R"X(</summary><link type="application/epub+zip" rel="http://opds-spec.org/acquisition" href="/t.epub"/></entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_LE(entries[0].summary.size(), 1024u);
  EXPECT_GT(entries[0].summary.size(), 200u);
}

TEST(OpdsParserTest, NextPageLinkIsKept) {
  OpdsParser parser;
  parse(
      feed(
          R"X(<link rel="next" type="application/atom+xml;profile=opds-catalog" href="/ebooks/search.opds/?start_index=26"/>)X"),
      parser);
  EXPECT_EQ(parser.getNextPageUrl(), "/ebooks/search.opds/?start_index=26");
}

// Gutenberg titles can carry hard line breaks, which no UI font can draw.
TEST(OpdsParserTest, TitleAndAuthorWhitespaceIsCollapsed) {
  const auto entries = parse(feed(R"X(
<entry>
<title>Alice in Wonderland
</title>
<author><name>  Carroll,
 Lewis </name></author>
<link type="application/epub+zip" rel="http://opds-spec.org/acquisition" href="/a.epub"/>
</entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].title, "Alice in Wonderland");
  EXPECT_EQ(entries[0].author, "Carroll, Lewis");
}

// A long first description leaves no room for the <content> that follows.
TEST(OpdsParserTest, LongSummaryIsNotExtendedByContent) {
  std::string words;
  while (words.size() < 400) words += "word ";
  const auto entries = parse(feed("<entry><title>T</title><summary>" + words +
                                  R"X(</summary><content type="html">&lt;p&gt;More.&lt;/p&gt;</content>
<link type="application/epub+zip" rel="http://opds-spec.org/acquisition" href="/t.epub"/></entry>)X"));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].summary.find("More."), std::string::npos);
}
