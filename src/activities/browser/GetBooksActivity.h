#pragma once

#include <I18n.h>
#include <OpdsParser.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Get Books: search and download free public-domain EPUBs straight from
// Project Gutenberg and Standard Ebooks (plus any OPDS catalogs the user
// configured), without a companion server.
//
//   Sources   Left/Right or side buttons pick a catalog, Confirm opens it
//             (search sources ask for a title or author first)
//   Results   Left/Right step through books, side buttons flip pages,
//             Confirm shows the book's details
//   Details   Confirm downloads into /books (or opens it when already there),
//             Left/Right step to the neighbouring result
//   Done      Confirm opens the new book
//
// Network work runs from loop() behind a status popup; Wi-Fi joins a saved
// network silently and only falls back to the picker when that fails.
class GetBooksActivity final : public Activity {
 public:
  GetBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GetBooks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return job != Job::None; }
  bool preventAutoSleep() override { return job != Job::None || screen == Screen::Download; }

 private:
  enum class Screen : uint8_t { Sources, Results, Detail, Download, Done, Error };
  enum class Job : uint8_t { None, List, Detail, Download };

  Screen screen = Screen::Sources;
  Job job = Job::None;
  Job retryJob = Job::None;  // what Confirm on the error screen repeats
  // Set while the Wi-Fi picker is open; the job resumes when it returns.
  bool waitingForWifi = false;
  bool wifiUsed = false;
  bool openingBook = false;
  ButtonNavigator navigator;

  // Sources: built-in catalogs first, then the user's OPDS servers.
  int sourceFocus = 0;
  int sourceCount() const;

  // Results of the open source.
  int sourceIndex = 0;  // < SOURCE_COUNT: built-in; otherwise a configured server
  std::string query;
  std::string listUrl;
  std::string nextUrl;
  std::vector<OpdsEntry> results;
  int focus = 0;
  std::vector<std::string> previousUrls;  // earlier pages of a paged feed

  // Details of results[focus], completed from its own feed when needed.
  OpdsEntry detail;
  std::string detailUrl;  // absolute download URL
  bool detailOnCard = false;
  bool hasCover = false;  // COVER_BMP_PATH holds this book's cover
  std::string savedPath;

  size_t downloaded = 0;
  size_t total = 0;
  bool cancelDownload = false;
  StrId errorText = StrId::STR_FETCH_FEED_FAILED;
  Screen errorReturn = Screen::Sources;

  // --- flow -----------------------------------------------------------------
  void openSource(int index);
  void startJob(Job next);
  void runJob();
  bool ensureWifi();
  void onWifiResult(bool connected);
  bool fetchList();
  bool fetchDetail();
  bool fetchCover(const std::string& url);
  bool download();
  void showError(StrId text, Screen returnTo);
  void openSavedBook();
  std::string destinationFor(const OpdsEntry& entry) const;
  const char* originName() const;

  // --- input ----------------------------------------------------------------
  void loopSources();
  void loopResults();
  void loopDetail();
  void stepFocus(int delta);
  void goBack();

  // --- drawing (GetBooksViews.cpp) -----------------------------------------
  int rowsPerPage() const;
  void drawTitle(const char* title, const char* info) const;
  void drawSources() const;
  void drawResults() const;
  void drawDetail() const;
  void drawDownload() const;
  void drawDone() const;
  void drawError() const;
  void drawHints() const;
  const char* statusText() const;
};
