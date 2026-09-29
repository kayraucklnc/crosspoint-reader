#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "StripeClient.h"

struct IncomeMonth;

// Gross income from charges, fetched only for days that are not final on SD.
// A day is final once it was fetched SETTLE_SECONDS after it ended; until
// then (today, and yesterday just after midnight) it is re-fetched each time.
namespace stripe_gross {

constexpr time_t SETTLE_SECONDS = 30 * 60;

// The five-day window first, then this month's missing days, newest first;
// progress is saved after every page. pendingDays: days of this month still
// missing afterwards.
StripeClient::Result fetchIncome(const std::string& authorization, time_t now, uint8_t mode, unsigned long deadline,
                                 bool backfillOnly, IncomeSummary& out, int& pendingDays);

// The missing days of a month that has ended at least SETTLE_SECONDS ago (NO_TIME otherwise).
StripeClient::Result fetchMonth(const std::string& authorization, time_t now, uint8_t mode, int year, int month,
                                unsigned long deadline, IncomeMonth& out);

// Final days the last fetchIncome() or fetchMonth() stored, whatever its result.
int daysSaved();

}  // namespace stripe_gross
