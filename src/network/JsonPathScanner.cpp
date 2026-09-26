#include "JsonPathScanner.h"

#include <cstring>

bool JsonPathScanner::Path::keyIs(const int i, const char* name) const {
  return i < depth && levels[i].isObject && strcmp(levels[i].key, name) == 0;
}

void JsonPathScanner::append(const char c) {
  if (tokenLen < MAX_TOKEN) token[tokenLen++] = c;
}

void JsonPathScanner::emitValue() {
  token[tokenLen] = '\0';
  if (handler.onValue && !overflow) handler.onValue(handler.ctx, Path{levels, depth}, token);
  tokenLen = 0;
}

void JsonPathScanner::finishScalar() {
  if (!inScalar) return;
  inScalar = false;
  emitValue();
}

void JsonPathScanner::feed(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = static_cast<char>(data[i]);

    if (inString) {
      if (escape) {
        escape = false;
        append(c);
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        inString = false;
        if (depth > 0 && levels[depth - 1].isObject && expectKey) {
          token[tokenLen] = '\0';
          memcpy(levels[depth - 1].key, token, tokenLen + 1);
          tokenLen = 0;
          expectKey = false;
        } else {
          emitValue();
        }
      } else {
        append(c);
      }
      continue;
    }

    switch (c) {
      case '"':
        finishScalar();
        inString = true;
        tokenLen = 0;
        break;
      case '{':
      case '[':
        finishScalar();
        if (depth >= MAX_DEPTH) {
          overflow = true;  // deeper than anything we read; stop reporting
          return;
        }
        levels[depth].isObject = c == '{';
        levels[depth].index = 0;
        levels[depth].key[0] = '\0';
        depth++;
        expectKey = c == '{';
        break;
      case '}':
      case ']':
        finishScalar();
        if (depth == 0) break;
        if (handler.onClose) handler.onClose(handler.ctx, Path{levels, depth});
        depth--;
        expectKey = false;
        break;
      case ',':
        finishScalar();
        if (depth > 0) {
          if (levels[depth - 1].isObject) {
            expectKey = true;
          } else {
            levels[depth - 1].index++;
          }
        }
        break;
      case ':':
      case ' ':
      case '\n':
      case '\r':
      case '\t':
        finishScalar();
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) break;  // PKCS7 padding after the document
        if (!inScalar) {
          inScalar = true;
          tokenLen = 0;
        }
        append(c);
        break;
    }
  }
}
