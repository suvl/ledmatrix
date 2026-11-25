// Which plane is on the panel (tickets 24 and 25). Pure logic with no Arduino headers, so it is tested on the laptop.
//
// Two modes share one "plane on the panel" slot:
//   one-shot (default): fetchFlights fills a small queue with planes not shown in the last 30 minutes; each is shown once.
//   cycle (Settings::cyclePlanes): every plane in the list from the last fetch is shown for the dwell time, nearest
//   first, round and round, while it stays in range.
// A turn is never cut short: when the mode, showFlights or the radar changes, the plane on the panel finishes its turn.
// All of it runs under live.cpp's lock; nothing here allocates.
#pragma once
#include <stdint.h>
#include <string.h>

const int CYCLE_MAX = 10;  // the most planes the service returns (n=10)

struct CycleFrame {
  uint32_t now = 0, dwellMs = 0;
  bool showFlights = true;  // off: no new turn starts, in either mode
  bool cycling = false;     // the owner's toggle
  bool listFresh = true;    // false: the last successful fetch is too old, so the list is dropped
};

// P needs: char id[8]; float dst; and to be copyable.
template <typename P>
struct PlaneCycle {
  P list[CYCLE_MAX];       // planes inside the radar at the last fetch, nearest first
  int n = 0;
  P cur;                   // the plane on the panel (meaningful while active)
  bool active = false;
  uint32_t until = 0;      // millis when the current turn ends
  bool cursor = false;     // cur says where the cycle is: the next turn goes to the plane after it

  int find(const char *id) const {
    for (int i = 0; i < n; i++)
      if (strcmp(list[i].id, id) == 0) return i;
    return -1;
  }

  // The fetch has filled list[0..n): put it nearest first and, in cycle mode, bring the shown plane's numbers up to date
  // (matched by id; a plane that left the list keeps what it had and is not picked again).
  void listReplaced(bool refreshCur) {
    for (int i = 1; i < n; i++)  // insertion sort by distance: stable, at most 10 planes, usually already in order
      for (int j = i; j > 0 && list[j].dst < list[j - 1].dst; j--) {
        P t = list[j]; list[j] = list[j - 1]; list[j - 1] = t;
      }
    if (refreshCur && cursor) {
      int i = find(cur.id);
      if (i >= 0) cur = list[i];
    }
  }

  // Where the cycle goes after cur: the next plane in the list, wrapping (a lone plane is its own successor, so it stays
  // on screen). A plane that is no longer listed gives way to the first one at or beyond its last distance. With nothing
  // shown yet the cycle starts at the nearest. -1 when the list is empty.
  int nextIndex() const {
    if (n == 0) return -1;
    if (!cursor) return 0;
    int i = find(cur.id);
    if (i >= 0) return (i + 1) % n;
    for (i = 0; i < n; i++)
      if (list[i].dst >= cur.dst) return i;
    return 0;
  }

  // One frame. queue/qn is the one-shot queue (used when not cycling). Returns true while a plane is on the panel.
  bool frame(const CycleFrame &f, P *queue, int &qn) {
    if (active && (int32_t)(f.now - until) >= 0) active = false;  // the turn is over
    if (!f.listFresh) n = 0;                                      // fetches keep failing: do not cycle old news
    if (f.cycling) qn = 0;                                        // the queue has no place here; its planes are in the list if still in range
    if (!active) {
      const P *next = nullptr;
      if (f.showFlights) {
        if (f.cycling) {
          int i = nextIndex();
          if (i >= 0) next = &list[i];
        } else if (qn > 0) {
          next = &queue[0];
        }
      }
      if (next) {
        cur = *next;  // copy first: the queue shifts below
        if (!f.cycling) {
          for (int i = 1; i < qn; i++) queue[i - 1] = queue[i];
          qn--;
        }
        active = true;
        until = f.now + f.dwellMs;
        cursor = true;
      } else {
        cursor = false;  // the radar emptied (or flights are off): the next cycle starts at the nearest again
      }
    }
    return active;
  }
};
