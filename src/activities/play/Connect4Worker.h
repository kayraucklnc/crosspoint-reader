#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "Connect4Brain.h"

// Runs Connect Four searches on a low-priority background task, so the
// activity keeps reading buttons and the panel keeps refreshing while the
// computer thinks. One search at a time; results are matched by ticket so a
// cancelled search can never land on a newer position.
class Connect4Worker {
 public:
  Connect4Worker() = default;
  Connect4Worker(const Connect4Worker&) = delete;
  Connect4Worker& operator=(const Connect4Worker&) = delete;
  ~Connect4Worker() { stop(); }

  // Allocates the transposition table and the task. Without a task (OOM),
  // post() searches synchronously instead.
  void start();
  // Cancels any search and waits for the task to exit.
  void stop();

  bool busy() const { return running.load(); }
  // Starts a search when idle and returns its ticket, or 0 when busy.
  uint32_t post(const c4::Position& pos, const c4::Level& level, uint32_t seed);
  // Asks the running search to stop early; its result will not be taken.
  void cancel() { abortFlag.store(true); }
  // True once the search for `ticket` has finished; `column` is its move.
  bool take(uint32_t ticket, int& column);

 private:
  std::unique_ptr<c4::TTEntry[]> table;
  c4::Search search{nullptr};
  TaskHandle_t task = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> alive{false};
  std::atomic<bool> quit{false};
  std::atomic<bool> abortFlag{false};
  std::atomic<uint32_t> doneTicket{0};
  std::atomic<int> doneColumn{-1};
  uint32_t nextTicket = 0;

  // The job; written only while no search is running.
  c4::Position jobPos;
  c4::Level jobLevel{};
  uint32_t jobSeed = 1;
  uint32_t jobTicket = 0;

  static void taskMain(void* arg);
  void serve();
  void runJob();
};
