#pragma once

#include <Stream.h>

#include "LegacyOpdsParser.h"

// Get Books keeps this Atom parser (with entry summaries); the OPDS browser
// uses the SDK parser, which shares these type names.
namespace legacy_opds {

class OpdsParserStream : public Stream {
 public:
  explicit OpdsParserStream(OpdsParser& parser);

  // That functions are not implemented for that stream
  int available() override;
  int peek() override;
  int read() override;

  virtual size_t write(uint8_t c) override;
  virtual size_t write(const uint8_t* buffer, size_t size) override;

  ~OpdsParserStream() override;

 private:
  OpdsParser& parser;
};

}  // namespace legacy_opds
