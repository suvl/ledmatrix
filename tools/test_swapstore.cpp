// Host test for arduino/ledmatrix/swapstore.h: the pattern the settings use so that saving them to flash can never delay a frame.
//   c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -Iarduino/ledmatrix tools/test_swapstore.cpp -o test_swapstore -pthread && ./test_swapstore
// (also clean under -fsanitize=thread). The render task is the "reader" here, the net task the "writer"; the flash write is a
// persist function that takes a long time on purpose.
#include <stdio.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "swapstore.h"

static int gFails = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      printf("    line %d: check failed: %s\n", __LINE__, #cond);  \
      gFails++;                                                    \
    }                                                              \
  } while (0)

using ms = std::chrono::milliseconds;

// A value wide enough that a read in the middle of a write would show a mixture of old and new.
struct Big {
  int counter = 0;
  int fill[256] = {};
  bool consistent() const {
    for (int x : fill)
      if (x != counter) return false;
    return true;
  }
  void set(int v) {
    counter = v;
    for (int &x : fill) x = v;
  }
};

using Store = SwapStore<Big, std::mutex>;

static void testBasics() {
  printf("basics\n");
  Big cur;
  std::mutex st, sv;
  Store store(cur, st, sv);
  CHECK(store.get().counter == 0);

  int persisted = -1, seenInsidePersist = -1, persistCalls = 0;
  bool ok = store.update(
      [](Big &n) { n.set(5); return true; },
      [&](const Big &n) {
        persistCalls++;
        persisted = n.counter;
        seenInsidePersist = store.get().counter;  // the new value is already visible, and the state lock is free (no deadlock)
      });
  CHECK(ok);
  CHECK(persistCalls == 1);
  CHECK(persisted == 5);
  CHECK(seenInsidePersist == 5);
  CHECK(store.get().counter == 5 && store.get().consistent());

  // a refused edit changes nothing, even if it changed its copy first, and nothing is written
  persistCalls = 0;
  ok = store.update([](Big &n) { n.set(99); return false; }, [&](const Big &) { persistCalls++; });
  CHECK(!ok);
  CHECK(persistCalls == 0);
  CHECK(store.get().counter == 5 && store.get().consistent());

  // the next change starts from the last one
  int startedFrom = -1;
  store.update([&](Big &n) { startedFrom = n.counter; n.set(n.counter + 1); return true; }, [](const Big &) {});
  CHECK(startedFrom == 5 && store.get().counter == 6);
}

// The point of the whole header: while the flash write is running, a reader is answered at once.
static void testReaderNotBlockedByPersist() {
  printf("a reader is not blocked while the flash write runs\n");
  Big cur;
  std::mutex st, sv;
  Store store(cur, st, sv);
  std::promise<void> started, release;
  std::shared_future<void> releaseF = release.get_future().share();

  std::thread writer([&] {
    store.update([](Big &n) { n.set(7); return true; }, [&](const Big &) {
      started.set_value();
      releaseF.wait();  // the slow flash write: stays here until the test lets go
    });
  });
  started.get_future().wait();  // the new value is swapped in and the write is under way

  auto reads = std::async(std::launch::async, [&] {
    int last = -1;
    for (int i = 0; i < 1000; i++) {
      Big b = store.get();
      if (!b.consistent()) return -2;
      last = b.counter;
    }
    return last;
  });
  bool answered = reads.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  CHECK(answered);  // with the flash write under the state lock this would never be ready
  if (answered) CHECK(reads.get() == 7);  // already the new value: the swap came first
  release.set_value();
  writer.join();
  if (!answered) reads.wait();
}

// Two saves at the same time: the flash writes never overlap, none is lost, and they reach flash in the order they were applied.
static void testSavesDoNotInterleave() {
  printf("saves never interleave and none is lost\n");
  Big cur;
  std::mutex st, sv;
  Store store(cur, st, sv);
  const int THREADS = 6, EACH = 25;
  std::atomic<int> inside{0}, maxInside{0};
  std::vector<int> written;  // what reached "flash", in order (only touched inside persist, one at a time)

  std::vector<std::thread> ts;
  for (int t = 0; t < THREADS; t++)
    ts.emplace_back([&] {
      for (int i = 0; i < EACH; i++)
        store.update([](Big &n) { n.set(n.counter + 1); return true; },
                     [&](const Big &n) {
                       int now = ++inside;
                       int m = maxInside.load();
                       while (now > m && !maxInside.compare_exchange_weak(m, now)) {}
                       std::this_thread::sleep_for(std::chrono::microseconds(200));
                       written.push_back(n.counter);
                       --inside;
                     });
    });
  for (auto &t : ts) t.join();

  CHECK(maxInside.load() == 1);                       // never two flash writes at once
  CHECK(store.get().counter == THREADS * EACH);       // no lost update: every edit started from the previous result
  CHECK((int)written.size() == THREADS * EACH);
  bool increasing = true;
  for (size_t i = 0; i < written.size(); i++)
    if (written[i] != (int)i + 1) increasing = false;
  CHECK(increasing);                                  // flash saw 1, 2, 3 ... in order, so it ends up holding the last value applied
  CHECK(!written.empty() && written.back() == store.get().counter);
}

// A reader that runs all the time while saves happen never sees a half-applied value.
static void testNoHalfAppliedState() {
  printf("a reader never sees a half-applied value\n");
  Big cur;
  std::mutex st, sv;
  Store store(cur, st, sv);
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0}, reads{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; r++)
    readers.emplace_back([&] {
      int lastSeen = 0;
      while (!stop) {
        Big b = store.get();
        if (!b.consistent()) torn++;
        if (b.counter < lastSeen) torn++;  // a value never goes back
        lastSeen = b.counter;
        reads++;
      }
    });
  for (int i = 1; i <= 300; i++)
    store.update([](Big &n) { n.set(n.counter + 1); return true; }, [](const Big &) { std::this_thread::sleep_for(std::chrono::microseconds(50)); });
  stop = true;
  for (auto &t : readers) t.join();
  CHECK(torn == 0);
  CHECK(reads > 0);
  CHECK(store.get().counter == 300);
}

int main() {
  testBasics();
  testReaderNotBlockedByPersist();
  testSavesDoNotInterleave();
  testNoHalfAppliedState();
  if (gFails) {
    printf("FAILED: %d check(s)\n", gFails);
    return 1;
  }
  printf("all swapstore checks passed\n");
  return 0;
}
