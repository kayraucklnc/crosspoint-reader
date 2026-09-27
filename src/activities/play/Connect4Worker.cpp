#include "Connect4Worker.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

namespace {
// negamax recurses once per ply (up to 42) with ~160-byte frames on RV32.
constexpr uint32_t TASK_STACK_BYTES = 10 * 1024;
// Idle priority: the loop and render tasks (priority 1) preempt the search,
// and the idle task still runs, so its watchdog stays fed.
constexpr int TASK_PRIORITY = 0;
constexpr unsigned long STOP_WARN_MS = 2000;

uint32_t nowMs() { return static_cast<uint32_t>(millis()); }
}  // namespace

void Connect4Worker::start() {
  if (!table) {
    // 4 KB, held for the game's lifetime so every search reuses it.
    table = makeUniqueNoThrow<c4::TTEntry[]>(c4::TT_SIZE);
    if (!table) LOG_ERR("C4", "OOM: transposition table, searching without it");
  }
  search = c4::Search(table.get());
  search.clearTable();
  if (task) return;
  quit.store(false);
  alive.store(true);
  xTaskCreate(&Connect4Worker::taskMain, "Connect4AI", TASK_STACK_BYTES, this, TASK_PRIORITY, &task);
  if (!task) {
    LOG_ERR("C4", "No AI task, thinking on the main loop");
    alive.store(false);
    task = nullptr;
  }
}

void Connect4Worker::stop() {
  if (!task) return;
  quit.store(true);
  abortFlag.store(true);
  xTaskNotify(task, 1, eIncrement);
  const unsigned long start = millis();
  bool warned = false;
  while (alive.load()) {
    if (!warned && millis() - start > STOP_WARN_MS) {
      LOG_ERR("C4", "AI task slow to stop");
      warned = true;
    }
    vTaskDelay(1);
  }
  task = nullptr;
  running.store(false);
}

uint32_t Connect4Worker::post(const c4::Position& pos, const c4::Level& level, const uint32_t seed) {
  if (running.load()) return 0;
  running.store(true);
  abortFlag.store(false);
  jobPos = pos;
  jobLevel = level;
  jobSeed = seed | 1u;
  if (++nextTicket == 0) nextTicket = 1;
  jobTicket = nextTicket;
  if (task) {
    xTaskNotify(task, 1, eIncrement);
  } else {
    runJob();
  }
  return jobTicket;
}

bool Connect4Worker::take(const uint32_t ticket, int& column) {
  if (ticket == 0 || running.load() || doneTicket.load() != ticket) return false;
  column = doneColumn.load();
  doneTicket.store(0);
  return true;
}

void Connect4Worker::taskMain(void* arg) {
  static_cast<Connect4Worker*>(arg)->serve();
  vTaskDelete(nullptr);
}

void Connect4Worker::serve() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (quit.load()) break;
    if (running.load()) runJob();
  }
  alive.store(false);  // last touch of `this`: stop() may free it right after
}

void Connect4Worker::runJob() {
  uint32_t rng = jobSeed;
  const unsigned long start = millis();
  const int column = search.choose(jobPos, jobLevel, &nowMs, &abortFlag, rng);
  LOG_DBG("C4", "move %d: col %d depth %d score %d nodes %lu in %lu ms", jobPos.moves, column, search.lastDepth,
          search.lastScore, static_cast<unsigned long>(search.nodes), millis() - start);
  doneColumn.store(column);
  doneTicket.store(jobTicket);
  running.store(false);
}
