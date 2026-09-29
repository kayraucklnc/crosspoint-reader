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
  std::string type;           // balance transactions only
  std::string paymentIntent;  // charges and refunds
  int64_t amount = 0;
  int64_t amountRefunded = 0;
  int64_t net = 0;  // balance transactions only
  int64_t created = 0;
  double exchangeRate = 0;  // balance transactions only: settlement units per presentment unit
};

/**
 * Incremental scanner for a Stripe list or search response (charges, balance
 * transactions or refunds) ({"data":[{...},...],"has_more":...,"next_page":...}).
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
  // The top-level object closed: the reply was read to its end.
  bool finished() const { return rootClosed; }
  const std::string& lastId() const { return lastChargeId; }
  // Search results only: the cursor for the next page (empty on the last one).
  const std::string& nextPage() const { return nextPageToken; }
  size_t chargeCount() const { return count; }

 private:
  static constexpr int MAX_DEPTH = 32;
  static constexpr size_t MAX_TOKEN = 64;
  static constexpr size_t MAX_ROOT_TOKEN = 256;  // search cursors are longer than item fields

  ChargeCallback onCharge;
  char containers[MAX_DEPTH] = {};  // '{' or '['
  int depth = 0;
  bool expectKey[MAX_DEPTH] = {};
  bool inString = false;
  bool escape = false;
  bool inScalar = false;
  std::string token;
  std::string key;      // last key seen at the current object level
  std::string rootKey;  // key at depth 1 that owns the current subtree
  bool inDataArray = false;
  StripeCharge current;
  bool hasMoreFlag = false;
  bool rootClosed = false;
  std::string lastChargeId;
  std::string nextPageToken;
  size_t count = 0;

  void onValue(const std::string& value);
  void finishScalar();
  bool atChargeLevel() const { return inDataArray && depth == 3 && containers[2] == '{'; }
  size_t tokenLimit() const { return depth == 1 ? MAX_ROOT_TOKEN : MAX_TOKEN; }
};
