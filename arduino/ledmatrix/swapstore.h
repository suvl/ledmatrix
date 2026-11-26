// A value that one task reads very often (the render task, every frame) and another task changes rarely and must also write to slow
// storage (flash). Pure C++ with no Arduino includes, so tools/test_swapstore.cpp can run it on the laptop with std::mutex.
//
// Two locks, always taken in this order when both are needed: `save`, then `state`.
//   state  guards the value. It is held only to copy the struct or to swap in a new one: microseconds, never across flash.
//   save   guards the slow storage and makes a change one-at-a-time: it is held from reading the current value until the flash
//          write is done. Readers never take it.
// So a reader waits at most for a struct copy, however long a save takes, and always sees either the old value or the new one,
// never half of each. Two changes cannot interleave: the second starts from the first one's result, and its flash write
// starts only after the first one's has finished, so the flash ends up holding the last value that was applied.
#pragma once
#include <utility>

// Mutex: any type with lock() and unlock() (std::mutex on the laptop, a small FreeRTOS wrapper on the board).
template <typename T, typename Mutex>
class SwapStore {
 public:
  SwapStore(T &value, Mutex &state, Mutex &save) : v(value), stateMu(state), saveMu(save) {}

  T get() {
    Guard g(stateMu);
    return v;
  }

  // edit(T &n) -> bool: n starts as a copy of the current value; return false to refuse, and nothing changes and persist is not called.
  // persist(const T &n): the slow write of the new value. It runs after the new value is visible to readers, from the copy,
  // with only the save lock held.
  template <typename Edit, typename Persist>
  bool update(Edit &&edit, Persist &&persist) {
    Guard s(saveMu);
    T n = get();
    if (!edit(n)) return false;
    {
      Guard g(stateMu);
      v = n;  // the swap
    }
    persist(n);
    return true;
  }

 private:
  struct Guard {
    Mutex &m;
    explicit Guard(Mutex &mu) : m(mu) { m.lock(); }
    ~Guard() { m.unlock(); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
  };
  T &v;
  Mutex &stateMu, &saveMu;
};
