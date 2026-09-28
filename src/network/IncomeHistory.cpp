#include "IncomeHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

#include "IncomeCalendar.h"

namespace income_history {
namespace {
constexpr const char* DIR = "/.crosspoint/income";
constexpr uint8_t MAGIC[2] = {'I', 'M'};
constexpr uint8_t VERSION = 1;
constexpr int DAYS = IncomeMonth::MAX_DAYS;
// magic, version, mode, boundary, known, then the day values (days 1..31).
constexpr size_t FILE_SIZE = 4 + 4 + 4 + DAYS * sizeof(int64_t) + DAYS * sizeof(uint16_t);

void pathOf(const int monthKey, const bool net, char* out, const size_t size) {
  snprintf(out, size, "%s/%06d%c.bin", DIR, monthKey, net ? 'n' : 'g');
}
}  // namespace

bool load(const int monthKey, const bool net, const uint8_t mode, IncomeMonth& out) {
  out = IncomeMonth{};
  char path[48];
  pathOf(monthKey, net, path, sizeof(path));
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("INCOME", path, file)) return false;
  if (file.fileSize() != FILE_SIZE) {
    LOG_ERR("INCOME", "Ignoring truncated %s", path);
    return false;
  }
  uint8_t header[4];
  uint32_t boundary = 0;
  uint32_t known = 0;
  // Fields are read straight into aligned members, never cast from a byte buffer.
  if (file.read(header, sizeof(header)) != sizeof(header) || header[0] != MAGIC[0] || header[1] != MAGIC[1] ||
      header[2] != VERSION || file.read(&boundary, sizeof(boundary)) != sizeof(boundary) ||
      file.read(&known, sizeof(known)) != sizeof(known) ||
      file.read(&out.cents[1], DAYS * sizeof(int64_t)) != static_cast<int>(DAYS * sizeof(int64_t)) ||
      file.read(&out.nonUsd[1], DAYS * sizeof(uint16_t)) != static_cast<int>(DAYS * sizeof(uint16_t))) {
    LOG_ERR("INCOME", "Ignoring unreadable %s", path);
    out = IncomeMonth{};
    return false;
  }
  if (header[3] != mode || boundary != income_calendar::boundaryId()) {
    out = IncomeMonth{};
    return false;
  }
  out.known = known;
  return true;
}

bool save(const int monthKey, const bool net, const uint8_t mode, const IncomeMonth& month) {
  if (!Storage.ensureDirectoryExists(DIR)) {
    LOG_ERR("INCOME", "Cannot create %s", DIR);
    return false;
  }
  char path[48];
  pathOf(monthKey, net, path, sizeof(path));
  HalFile file;
  if (!Storage.openFileForWrite("INCOME", path, file)) return false;
  const uint8_t header[4] = {MAGIC[0], MAGIC[1], VERSION, mode};
  const uint32_t boundary = income_calendar::boundaryId();
  size_t written = file.write(header, sizeof(header));
  written += file.write(&boundary, sizeof(boundary));
  written += file.write(&month.known, sizeof(month.known));
  written += file.write(&month.cents[1], DAYS * sizeof(int64_t));
  written += file.write(&month.nonUsd[1], DAYS * sizeof(uint16_t));
  if (written != FILE_SIZE) {
    LOG_ERR("INCOME", "Short write to %s", path);
    file.close();  // close before remove
    Storage.remove(path);
    return false;
  }
  return true;
}

}  // namespace income_history
