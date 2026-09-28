#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// One charge or balance transaction, reduced to the fields the income screen needs.
struct StripeCharge {
  std::string id;
  std::string currency;
  std::string status;
  std::string type;  // balance transactions only
  int64_t amount = 0;
  int64_t amountRefunded = 0;
  int64_t net = 0;  // balance transactions only
  int64_t created = 0;
  double exchangeRate = 0;  // balance transactions only: settlement units per presentment unit
};

/**
 * Incremental scanner for a Stripe list response (charges or balance transactions) ({"data":[{...},...],"has_more":...}).
 *
 * Stripe charge objects are several KB each, so a 100-item page never fits in
 * C3 heap. This consumes the body chunk by chunk and keeps only the top-level
 * scalars of each item in "data"; nested objects are skipped by depth.
 */
class StripeChargeScanner {
 public:
  using ChargeCallback = std::function<void(const StripeCharge&)>;

  explicit StripeChargeScanner(ChargeCallback onCharge) : onCharge(std::move(onCharge)) {}

  void feed(const uint8_t* data, size_t len);

  bool hasMore() const { return hasMoreFlag; }
  const std::string& lastId() const { return lastChargeId; }
  size_t chargeCount() const { return count; }

 private:
  static constexpr int MAX_DEPTH = 32;
  static constexpr size_t MAX_TOKEN = 64;

  ChargeCallback onCharge;
  char containers[MAX_DEPTH] = {};  // '{' or '['
  int depth = 0;
  bool expectKey[MAX_DEPTH] = {};
  bool inString = false;
  bool escape = false;
  bool inScalar = false;
  std::string token;
  std::string key;          // last key seen at the current object level
  std::string rootKey;      // key at depth 1 that owns the current subtree
  bool inDataArray = false;
  StripeCharge current;
  bool hasMoreFlag = false;
  std::string lastChargeId;
  size_t count = 0;

  void onValue(const std::string& value);
  void finishScalar();
  bool atChargeLevel() const { return inDataArray && depth == 3 && containers[2] == '{'; }
};
