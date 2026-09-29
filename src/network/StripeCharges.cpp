#include "StripeCharges.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <cctype>
#include <cstdio>

#include "ChargeDayTally.h"
#include "IncomeCalendar.h"
#include "StripeChargeScanner.h"
#include "StripeHttp.h"

namespace stripe_charges {
namespace {
constexpr const char* SEARCH_URL = "https://api.stripe.com/v1/charges/search";
constexpr const char* LIST_URL = "https://api.stripe.com/v1/charges";
constexpr const char* REFUNDS_URL = "https://api.stripe.com/v1/refunds";
// A month is at most a few thousand charges; more pages than this is a runaway cursor.
constexpr int MAX_PAGES = 120;
constexpr time_t FUTURE_SLACK = 600;
constexpr int MAX_REFUNDS = 4;
constexpr int MAX_EDGE_IDS = 8;

// Charges search sometimes repeats the last record of a page as the first of
// the next. The records sharing a page's oldest `created` second are kept so
// the next page can skip them.
struct PageEdge {
  int64_t created = -1;
  std::string ids[MAX_EDGE_IDS];
  int count = 0;

  void note(const StripeCharge& record) {
    if (record.created != created) {
      created = record.created;
      count = 0;
    }
    if (count < MAX_EDGE_IDS) ids[count++] = record.id;
  }
  bool contains(const StripeCharge& record) const {
    if (record.created != created) return false;
    for (int i = 0; i < count; i++) {
      if (ids[i] == record.id) return true;
    }
    return false;
  }
};

// Search returns only successful charges (about 40% fewer bytes than the
// list); it is switched off for the session if the account cannot search.
bool searchAvailable = true;

bool deadlineReached(const unsigned long deadline) { return static_cast<long>(millis() - deadline) >= 0; }

void appendEncoded(std::string& out, const std::string& value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  for (const char c : value) {
    const auto u = static_cast<unsigned char>(c);
    if (isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += HEX_DIGITS[u >> 4];
      out += HEX_DIGITS[u & 15];
    }
  }
}

std::string pageUrl(const time_t from, const time_t until, const std::string& cursor) {
  char head[200];
  if (searchAvailable) {
    snprintf(head, sizeof(head),
             "%s?limit=%d&query=status%%3A%%27succeeded%%27%%20AND%%20created%%3E%%3D%lld%%20AND%%20created%%3C%lld",
             SEARCH_URL, PAGE_LIMIT, static_cast<long long>(from), static_cast<long long>(until));
  } else {
    snprintf(head, sizeof(head), "%s?limit=%d&created%%5Bgte%%5D=%lld&created%%5Blt%%5D=%lld", LIST_URL, PAGE_LIMIT,
             static_cast<long long>(from), static_cast<long long>(until));
  }
  std::string url = head;
  if (!cursor.empty()) {
    url += searchAvailable ? "&page=" : "&starting_after=";
    appendEncoded(url, cursor);
  }
  return url;
}

StripeClient::Result failedResult(const int status) {
  if (status == 401) return StripeClient::Result::INVALID_KEY;
  if (status == 403) return StripeClient::Result::FORBIDDEN;
  if (stripe_http::lastFailure().failure == stripe_http::Failure::LOW_MEMORY) return StripeClient::Result::LOW_MEMORY;
  return StripeClient::Result::HTTP_ERROR;
}

// A reply that is not a whole list, or a list without a cursor, cannot be trusted.
StripeClient::Result badReply() {
  stripe_http::fail(stripe_http::Failure::BAD_REPLY);
  return StripeClient::Result::HTTP_ERROR;
}
}  // namespace

StripeClient::Result fetchDays(const std::string& authorization, const int year, const int month, const int firstDay,
                               const int lastDay, const time_t now, const unsigned long deadline, DaySink& sink) {
  // 640 bytes of day totals: kept off the loop task's stack.
  auto tally = makeUniqueNoThrow<ChargeDayTally>(firstDay, lastDay);
  if (!tally) {
    LOG_ERR("STRIPE", "OOM: day tally");
    stripe_http::fail(stripe_http::Failure::LOW_MEMORY, 0);
    return StripeClient::Result::LOW_MEMORY;
  }
  const time_t from = income_calendar::dayStart(year, month, firstDay);
  const time_t until = income_calendar::dayStart(year, month, lastDay + 1);
  int future = 0;
  PageEdge previousEdge;
  PageEdge edge;
  const StripeChargeScanner::ChargeCallback onRecord = [&](const StripeCharge& record) {
    if (previousEdge.contains(record)) return;
    edge.note(record);
    if (record.status != "succeeded") return;  // the list also returns failed charges
    if (record.created > now + FUTURE_SLACK) {
      future++;
      return;
    }
    struct tm day;
    income_calendar::calendarOf(static_cast<time_t>(record.created), day);
    if (day.tm_year + 1900 != year || day.tm_mon + 1 != month) return;
    const bool usd = record.currency == "usd";
    tally->add(day.tm_mday, usd ? record.amount - record.amountRefunded : 0, !usd);
  };

  std::string cursor;
  StripeChargeScanner scanner(onRecord);
  for (int page = 0; tally->top() >= firstDay; page++) {
    if (page > 0 && deadlineReached(deadline)) return StripeClient::Result::PARTIAL;
    if (page >= MAX_PAGES) return badReply();
    char tag[24];
    snprintf(tag, sizeof(tag), "g %02d%02d-%02d%02d p%d", month, firstDay, month, tally->top(), page + 1);
    const std::string url = pageUrl(from, until, cursor);
    int status = 0;
    const bool ok = stripe_http::get(
        url, authorization,
        [&scanner](const uint8_t* data, const size_t len) {
          scanner.feed(data, len);
          return true;
        },
        tag,
        [&] {
          scanner = StripeChargeScanner(onRecord);
          tally->discardPage();
          edge = PageEdge{};
        },
        status);
    if (!ok) {
      if (searchAvailable && page == 0 && (status == 400 || status == 404)) {
        LOG_ERR("STRIPE", "Charge search unavailable (HTTP %d); using the charges list", status);
        searchAvailable = false;
        page = -1;
        continue;
      }
      return failedResult(status);
    }
    if (!scanner.finished()) return badReply();
    const bool more = scanner.hasMore();
    const int top = tally->top();
    const int lowest = tally->commitPage(more);
    for (int day = top; day >= lowest; day--) sink.day(day, tally->cents(day), tally->nonUsd(day));
    if (lowest <= top) sink.pageDone();
    LOG_DBG("STRIPE", "Charges page %d: %u records, days %d..%d done", page + 1,
            static_cast<unsigned>(scanner.chargeCount()), lowest, top);
    if (!more) break;
    previousEdge = edge;
    edge = PageEdge{};
    cursor = searchAvailable ? scanner.nextPage() : scanner.lastId();
    if (cursor.empty()) return badReply();
  }
  if (future > 0) LOG_ERR("STRIPE", "Ignored %d charges dated after now; clock is behind", future);
  return StripeClient::Result::OK;
}

StripeClient::Result refundedCharges(const std::string& authorization, const time_t since, time_t* created,
                                     const int maxCharges, int& count, bool& truncated) {
  count = 0;
  truncated = false;
  std::string intents[MAX_REFUNDS];
  int intentCount = 0;
  int refundCount = 0;
  const StripeChargeScanner::ChargeCallback onRefund = [&](const StripeCharge& refund) {
    if (refund.status != "succeeded" && refund.status != "pending") return;
    if (refund.paymentIntent.empty()) {
      LOG_ERR("STRIPE", "Refund %s has no payment intent; its day is not rechecked", refund.id.c_str());
      return;
    }
    refundCount++;
    if (intentCount < MAX_REFUNDS) intents[intentCount++] = refund.paymentIntent;
  };
  StripeChargeScanner refunds(onRefund);
  char url[160];
  snprintf(url, sizeof(url), "%s?limit=20&created%%5Bgte%%5D=%lld", REFUNDS_URL, static_cast<long long>(since));
  int status = 0;
  const bool ok = stripe_http::get(
      url, authorization,
      [&refunds](const uint8_t* data, const size_t len) {
        refunds.feed(data, len);
        return true;
      },
      "refunds",
      [&] {
        refunds = StripeChargeScanner(onRefund);
        intentCount = 0;
        refundCount = 0;
      },
      status);
  if (!ok) {
    if (status == 403) {
      LOG_INF("STRIPE", "Key cannot read refunds; refunded days are not rechecked");
      return StripeClient::Result::OK;
    }
    return failedResult(status);
  }
  if (!refunds.finished()) return badReply();
  truncated = refunds.hasMore() || refundCount > intentCount;

  for (int i = 0; i < intentCount && count < maxCharges; i++) {
    std::string chargesUrl = std::string(LIST_URL) + "?limit=3&payment_intent=";
    appendEncoded(chargesUrl, intents[i]);
    const StripeChargeScanner::ChargeCallback onCharge = [&](const StripeCharge& charge) {
      if (charge.status != "succeeded") return;
      if (count < maxCharges) {
        created[count++] = static_cast<time_t>(charge.created);
      } else {
        truncated = true;
      }
    };
    StripeChargeScanner charges(onCharge);
    const int before = count;
    if (!stripe_http::get(
            chargesUrl, authorization,
            [&charges](const uint8_t* data, const size_t len) {
              charges.feed(data, len);
              return true;
            },
            "refunded charge",
            [&] {
              charges = StripeChargeScanner(onCharge);
              count = before;
            },
            status)) {
      return failedResult(status);
    }
  }
  if (intentCount > 0 && count >= maxCharges) truncated = true;
  return StripeClient::Result::OK;
}

}  // namespace stripe_charges
