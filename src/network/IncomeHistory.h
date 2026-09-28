#pragma once

#include <cstdint>

// Daily income of one calendar month. Days are 1-based; bit n of `known` means
// day n holds a value.
struct IncomeMonth {
  static constexpr int MAX_DAYS = 31;
  int64_t cents[MAX_DAYS + 1] = {};
  uint16_t nonUsd[MAX_DAYS + 1] = {};  // non-USD charges left out (charges source only)
  uint32_t known = 0;

  bool isKnown(const int day) const { return (known >> day) & 1u; }
  void set(const int day, const int64_t value, const uint16_t skipped) {
    cents[day] = value;
    nonUsd[day] = skipped;
    known |= 1u << day;
  }
};

// Closed months on SD, one 322-byte file per month and amount mode:
// /.crosspoint/income/YYYYMMg.bin (Gross) or YYYYMMn.bin (Net). A month is
// written only once it has ended, so each is fetched from Stripe at most once.
namespace income_history {

// False when the file is missing, corrupt, or was bucketed with a different
// mode byte (amount, source, UTC days) or day boundary.
bool load(int monthKey, bool net, uint8_t mode, IncomeMonth& out);

bool save(int monthKey, bool net, uint8_t mode, const IncomeMonth& month);

}  // namespace income_history
