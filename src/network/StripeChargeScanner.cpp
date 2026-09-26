#include "StripeChargeScanner.h"

#include <cstdlib>

void StripeChargeScanner::onValue(const std::string& value) {
  if (depth == 1 && key == "has_more") {
    hasMoreFlag = value == "true";
    return;
  }
  if (!atChargeLevel()) return;

  if (key == "id") {
    current.id = value;
  } else if (key == "currency") {
    current.currency = value;
  } else if (key == "status") {
    current.status = value;
  } else if (key == "type") {
    current.type = value;
  } else if (key == "net") {
    current.net = strtoll(value.c_str(), nullptr, 10);
  } else if (key == "amount") {
    current.amount = strtoll(value.c_str(), nullptr, 10);
  } else if (key == "amount_refunded") {
    current.amountRefunded = strtoll(value.c_str(), nullptr, 10);
  } else if (key == "created") {
    current.created = strtoll(value.c_str(), nullptr, 10);
  }
}

void StripeChargeScanner::finishScalar() {
  if (!inScalar) return;
  inScalar = false;
  onValue(token);
  token.clear();
}

void StripeChargeScanner::feed(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = static_cast<char>(data[i]);

    if (inString) {
      if (escape) {
        escape = false;
        if (token.size() < MAX_TOKEN) token += c;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        inString = false;
        const bool isKey = depth > 0 && containers[depth - 1] == '{' && expectKey[depth - 1];
        if (isKey) {
          key = token;
          if (depth == 1) rootKey = token;
          expectKey[depth - 1] = false;
        } else {
          onValue(token);
        }
        token.clear();
      } else if (token.size() < MAX_TOKEN) {
        token += c;
      }
      continue;
    }

    switch (c) {
      case '"':
        finishScalar();
        inString = true;
        token.clear();
        break;
      case '{':
      case '[':
        finishScalar();
        if (depth >= MAX_DEPTH) return;  // malformed or unexpectedly deep; stop scanning
        if (depth == 1 && c == '[' && rootKey == "data") inDataArray = true;
        containers[depth] = c;
        expectKey[depth] = c == '{';
        depth++;
        if (atChargeLevel()) current = StripeCharge{};
        break;
      case '}':
      case ']':
        finishScalar();
        if (c == '}' && atChargeLevel()) {
          lastChargeId = current.id;
          count++;
          onCharge(current);
        }
        if (depth > 0) depth--;
        if (depth == 1 && c == ']') inDataArray = false;
        break;
      case ',':
        finishScalar();
        if (depth > 0 && containers[depth - 1] == '{') expectKey[depth - 1] = true;
        break;
      case ':':
        finishScalar();
        break;
      case ' ':
      case '\n':
      case '\r':
      case '\t':
        finishScalar();
        break;
      default:
        if (!inScalar) {
          inScalar = true;
          token.clear();
        }
        if (token.size() < MAX_TOKEN) token += c;
        break;
    }
  }
}
