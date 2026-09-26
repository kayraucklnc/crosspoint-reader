#pragma once

#include <cstddef>
#include <cstdint>

// Streaming JSON tokenizer that reports each scalar with its path (object keys
// and array indices from the root). Nothing is buffered beyond one token, so
// arbitrarily large responses parse in constant memory. Keys and values longer
// than MAX_TOKEN are truncated.
class JsonPathScanner {
 public:
  static constexpr int MAX_DEPTH = 16;
  static constexpr size_t MAX_TOKEN = 48;

  struct Level {
    bool isObject;
    int index;                // element index inside an array
    char key[MAX_TOKEN + 1];  // current key inside an object
  };

  struct Path {
    const Level* levels;
    int depth;
    // Key at level i, or nullptr when level i is an array.
    const char* key(int i) const { return levels[i].isObject ? levels[i].key : nullptr; }
    int index(int i) const { return levels[i].index; }
    bool keyIs(int i, const char* name) const;
  };

  struct Handler {
    void* ctx;
    void (*onValue)(void* ctx, const Path& path, const char* value);
    // Called when the container at path.depth closes (path still includes it).
    void (*onClose)(void* ctx, const Path& path);
  };

  explicit JsonPathScanner(Handler handler) : handler(handler) {}
  void feed(const uint8_t* data, size_t len);

 private:
  Handler handler;
  Level levels[MAX_DEPTH] = {};
  int depth = 0;
  bool expectKey = false;
  bool inString = false;
  bool escape = false;
  bool inScalar = false;
  bool overflow = false;
  char token[MAX_TOKEN + 1] = {};
  size_t tokenLen = 0;

  void append(char c);
  void emitValue();
  void finishScalar();
};
