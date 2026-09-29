#pragma once

#include <cstdint>

// Buckets pages of charges into days of one month. Pages arrive newest first
// (Stripe lists and searches by `created`, descending), so once a page ends
// on day D every later day is complete and can be stored before the next page.
// A page is staged until it commits, so a retried page is never counted twice.
class ChargeDayTally {
 public:
  static constexpr int MAX_DAYS = 31;

  // Covers days firstDay..lastDay (1-based) of one month.
  ChargeDayTally(int firstDay, int lastDay);

  // One successful charge of the page being read: USD cents, or a non-USD charge.
  void add(int day, int64_t cents, bool nonUsd);
  void discardPage();

  // Moves the staged page into the totals. Returns the lowest day that is now
  // complete: days from it up to the previous top() are final. With more pages
  // to come only days after the page's oldest day complete; without, all do.
  // Returns top() + 1 when none completed.
  int commitPage(bool hasMore);

  // Highest day not complete yet; firstDay - 1 once every day is.
  int top() const { return topDay; }
  int64_t cents(int day) const { return totalCents[day]; }
  uint16_t nonUsd(int day) const { return totalNonUsd[day]; }

 private:
  int firstDay;
  int topDay;
  int pageOldest = 0;  // 0 = the page has no records yet
  int64_t totalCents[MAX_DAYS + 1] = {};
  int64_t pageCents[MAX_DAYS + 1] = {};
  uint16_t totalNonUsd[MAX_DAYS + 1] = {};
  uint16_t pageNonUsd[MAX_DAYS + 1] = {};
};
