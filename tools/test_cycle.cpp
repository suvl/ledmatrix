// Host test for arduino/ledmatrix/cycle.h (plane selection: one-shot queue and cycle mode). Run from the repo root:
//   c++ -std=c++17 -Wall -Wextra -Iarduino/ledmatrix tools/test_cycle.cpp -o /tmp/test_cycle && /tmp/test_cycle
#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "cycle.h"

struct TP { char id[8] = ""; float dst = 0; int alt = 0; uint32_t at = 0; };

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Plane { const char *id; float dst; int alt; };

// What fetchFlights does with a fresh answer: list filled in service order, then listReplaced.
static void fetch(PlaneCycle<TP> &pc, std::initializer_list<Plane> ps, uint32_t now, bool cycling) {
  pc.n = 0;
  for (const Plane &p : ps) {
    TP q; strcpy(q.id, p.id); q.dst = p.dst; q.alt = p.alt; q.at = now;
    if (pc.n < CYCLE_MAX && pc.find(q.id) < 0) pc.list[pc.n++] = q;
  }
  pc.listReplaced(cycling);
}

struct Rig {
  PlaneCycle<TP> pc;
  TP queue[3]; int qn = 0;
  uint32_t dwell = 10000;
  bool show = true, cycling = true, fresh = true;
  bool step(uint32_t now) {
    CycleFrame f; f.now = now; f.dwellMs = dwell; f.showFlights = show; f.cycling = cycling; f.listFresh = fresh;
    return pc.frame(f, queue, qn);
  }
  // run frames every 100 ms from t0 to t1 (ms), return the sequence of distinct shown ids ("-" = nothing), and note gaps
  void run(uint32_t t0, uint32_t t1, char *out, int cap, int *gaps) {
    out[0] = 0; *gaps = 0;
    char last[8] = "?"; bool seenAny = false;
    for (uint32_t t = t0; t < t1; t += 100) {
      bool a = step(t);
      const char *id = a ? pc.cur.id : "-";
      if (seenAny && !a) (*gaps)++;
      if (strcmp(last, id) != 0) { strncat(out, id, cap - strlen(out) - 2); strncat(out, " ", cap - strlen(out) - 1); }
      strcpy(last, id);
      if (a) seenAny = true;
    }
  }
};

static void tail(const char *name) { printf("%-60s %s\n", name, fails ? "FAILED" : "ok"); }
static int prevFails = 0;
#define SCENARIO(name) do { printf("-- %s\n", name); prevFails = fails; } while (0)
#define END() do { printf("   %s\n", fails == prevFails ? "ok" : "FAILED"); } while (0)

int main() {
  char seq[256]; int gaps;

  SCENARIO("1. three planes cycle in order and wrap, no blank gap");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 30000}, {"B", 2, 31000}, {"C", 3, 32000}}, 0, true);
    r.run(0, 45000, seq, sizeof seq, &gaps);
    printf("   shown: %s gaps=%d\n", seq, gaps);
    CHECK(strcmp(seq, "A B C A B ") == 0);
    CHECK(gaps == 0);
  }
  END();

  SCENARIO("1b. service order not nearest first: board sorts by dst");
  {
    Rig r;
    fetch(r.pc, {{"C", 3, 0}, {"A", 1, 0}, {"B", 2, 0}}, 0, true);
    r.run(0, 35000, seq, sizeof seq, &gaps);
    printf("   shown: %s\n", seq);
    CHECK(strcmp(seq, "A B C A ") == 0);
  }
  END();

  SCENARIO("2. the shown plane leaves at a fetch: finishes its turn, not picked again");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}, {"C", 3, 0}}, 0, true);
    r.run(0, 15000, seq, sizeof seq, &gaps);  // A then B on screen (B since 10 s)
    CHECK(r.pc.active && strcmp(r.pc.cur.id, "B") == 0);
    fetch(r.pc, {{"A", 1, 0}, {"C", 3, 0}}, 15000, true);  // B left the radar
    CHECK(r.step(15000) && strcmp(r.pc.cur.id, "B") == 0);   // not interrupted
    CHECK(r.step(19900) && strcmp(r.pc.cur.id, "B") == 0);
    CHECK(r.step(20000) && strcmp(r.pc.cur.id, "C") == 0);   // turn over: the one that followed B
    CHECK(r.step(30000) && strcmp(r.pc.cur.id, "A") == 0);
    CHECK(r.step(40000) && strcmp(r.pc.cur.id, "C") == 0);   // B never again
    CHECK(r.step(50000) && strcmp(r.pc.cur.id, "A") == 0);
  }
  END();

  SCENARIO("2b. the shown plane is the LAST in the list and leaves: wraps to the nearest");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}, {"C", 3, 0}}, 0, true);
    r.run(0, 25000, seq, sizeof seq, &gaps);  // C since 20 s
    CHECK(strcmp(r.pc.cur.id, "C") == 0);
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}}, 25000, true);
    CHECK(r.step(30000) && strcmp(r.pc.cur.id, "A") == 0);
  }
  END();

  SCENARIO("3. a new nearer plane arrives mid-turn: no interruption, reached within one round");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}, {"C", 3, 0}}, 0, true);
    r.run(0, 15000, seq, sizeof seq, &gaps);  // B on screen
    fetch(r.pc, {{"D", 0.5f, 0}, {"A", 1, 0}, {"B", 2, 0}, {"C", 3, 0}}, 15000, true);
    CHECK(r.step(15000) && strcmp(r.pc.cur.id, "B") == 0);
    CHECK(r.step(20000) && strcmp(r.pc.cur.id, "C") == 0);
    CHECK(r.step(30000) && strcmp(r.pc.cur.id, "D") == 0);
    CHECK(r.step(40000) && strcmp(r.pc.cur.id, "A") == 0);
    CHECK(r.step(50000) && strcmp(r.pc.cur.id, "B") == 0);
  }
  END();

  SCENARIO("4. list becomes empty, then non-empty: restarts at the nearest, not after the old plane");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}, {"C", 3, 0}}, 0, true);
    r.run(0, 5000, seq, sizeof seq, &gaps);  // A on screen
    fetch(r.pc, {}, 5000, true);              // radar empty
    CHECK(r.step(5000) && strcmp(r.pc.cur.id, "A") == 0);   // A finishes
    CHECK(r.step(9900));
    CHECK(!r.step(10000));                                  // nothing: normal screens may show
    CHECK(!r.step(10100));
    CHECK(!r.pc.cursor);
    fetch(r.pc, {{"E", 4, 0}, {"F", 2, 0}}, 30000, true);
    CHECK(r.step(30100) && strcmp(r.pc.cur.id, "F") == 0);  // nearest first
    CHECK(r.step(40100) && strcmp(r.pc.cur.id, "E") == 0);
  }
  END();

  SCENARIO("4b. radar empty while the plane is shown, refilled before its turn ends");
  {
    Rig r; r.dwell = 40000;
    fetch(r.pc, {{"A", 2, 0}}, 0, true);
    r.step(0);
    fetch(r.pc, {}, 30000, true);
    fetch(r.pc, {{"B", 1, 0}, {"C", 5, 0}}, 31000, true);
    CHECK(r.step(40000) && strcmp(r.pc.cur.id, "C") == 0);  // first at or beyond A's distance 2
    CHECK(r.step(80000) && strcmp(r.pc.cur.id, "B") == 0);  // wraps
  }
  END();

  SCENARIO("5. single plane stays on screen continuously; telemetry refreshes by id");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 10000}}, 0, true);
    r.run(0, 35000, seq, sizeof seq, &gaps);
    printf("   shown: %s gaps=%d\n", seq, gaps);
    CHECK(strcmp(seq, "A ") == 0);
    CHECK(gaps == 0);
    CHECK(r.pc.cur.alt == 10000 && r.pc.cur.at == 0);
    fetch(r.pc, {{"A", 0.8f, 9400}}, 35000, true);      // newer fetch: altitude and `at` come from it
    CHECK(r.pc.cur.alt == 9400 && r.pc.cur.at == 35000 && r.pc.cur.dst == 0.8f);
    CHECK(r.step(35100) && strcmp(r.pc.cur.id, "A") == 0);
    // turn boundary: still on, until moves on by one dwell
    r.step(39900);
    uint32_t u0 = r.pc.until;
    CHECK(r.step(u0) && r.pc.until == u0 + 10000);
  }
  END();

  SCENARIO("5b. a plane shown does not get its numbers refreshed in one-shot mode (as today)");
  {
    Rig r; r.cycling = false;
    TP q; strcpy(q.id, "A"); q.alt = 5000; q.at = 0; r.queue[r.qn++] = q;
    CHECK(r.step(0) && r.pc.cur.alt == 5000);
    fetch(r.pc, {{"A", 1, 4000}}, 5000, false);   // list kept, but cur is not touched
    CHECK(r.pc.cur.alt == 5000 && r.pc.cur.at == 0);
  }
  END();

  SCENARIO("6a. toggle off mid-turn: the turn finishes, then one-shot resumes");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}}, 0, true);
    CHECK(r.step(0) && strcmp(r.pc.cur.id, "A") == 0);
    r.cycling = false;
    CHECK(r.step(5000) && strcmp(r.pc.cur.id, "A") == 0);   // not cut
    CHECK(!r.step(10000));                                  // queue empty: no plane, no restart
    CHECK(!r.step(10100));
    // a one-shot plane arrives later
    TP q; strcpy(q.id, "Z"); r.queue[r.qn++] = q;
    CHECK(r.step(12000) && strcmp(r.pc.cur.id, "Z") == 0 && r.qn == 0);
    CHECK(!r.step(22000));
  }
  END();

  SCENARIO("6b. toggle on while the one-shot queue holds planes: queue dropped, cycle continues after the shown plane");
  {
    Rig r; r.cycling = false;
    TP a; strcpy(a.id, "B"); a.dst = 2; TP b; strcpy(b.id, "X"); TP c; strcpy(c.id, "Y");
    r.queue[0] = a; r.queue[1] = b; r.queue[2] = c; r.qn = 3;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}, {"C", 3, 0}}, 0, false);
    CHECK(r.step(0) && strcmp(r.pc.cur.id, "B") == 0 && r.qn == 2);
    r.cycling = true;
    CHECK(r.step(100) && strcmp(r.pc.cur.id, "B") == 0 && r.qn == 0);   // queue gone, B finishes
    CHECK(r.step(10000) && strcmp(r.pc.cur.id, "C") == 0);              // after B, no duplicate of B
    CHECK(r.step(20000) && strcmp(r.pc.cur.id, "A") == 0);
    r.cycling = false;
    CHECK(r.step(25000) && strcmp(r.pc.cur.id, "A") == 0);
    CHECK(!r.step(30000));                                              // stale queue entries X and Y never appear
    CHECK(r.qn == 0);
  }
  END();

  SCENARIO("6c. toggle on while idle: starts from the nearest at the next frame (list already there)");
  {
    Rig r; r.cycling = false;
    fetch(r.pc, {{"C", 3, 0}, {"A", 1, 0}, {"B", 2, 0}}, 0, false);
    CHECK(!r.step(0));
    CHECK(!r.step(500));
    r.cycling = true;
    CHECK(r.step(600) && strcmp(r.pc.cur.id, "A") == 0);
    CHECK(r.step(10600) && strcmp(r.pc.cur.id, "B") == 0);
  }
  END();

  SCENARIO("6d. one-shot plane shown earlier, still in range: toggling on later starts at the nearest, not after it");
  {
    Rig r; r.cycling = false;
    TP q; strcpy(q.id, "B"); q.dst = 2; r.queue[r.qn++] = q;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}}, 0, false);
    CHECK(r.step(0));
    CHECK(!r.step(10000));      // turn over, cursor reset
    r.cycling = true;
    CHECK(r.step(20000) && strcmp(r.pc.cur.id, "A") == 0);
  }
  END();

  SCENARIO("7a. showFlights off: no new turn in cycle mode (current turn finishes)");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}}, 0, true);
    CHECK(r.step(0));
    r.show = false;
    CHECK(r.step(5000));
    CHECK(!r.step(10000));
    CHECK(!r.step(20000));
    r.show = true;
    CHECK(r.step(21000) && strcmp(r.pc.cur.id, "A") == 0);   // starts at the nearest again
  }
  END();

  SCENARIO("7b. list too old (fetches failing): dropped, planes stop cycling");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}}, 0, true);
    CHECK(r.step(0));
    r.fresh = false;
    CHECK(r.step(5000));       // the shown plane finishes
    CHECK(!r.step(10000));
    CHECK(r.pc.n == 0);
    r.fresh = true;            // next good fetch refills
    fetch(r.pc, {{"B", 2, 0}}, 12000, true);
    CHECK(r.step(12100) && strcmp(r.pc.cur.id, "B") == 0);
  }
  END();

  SCENARIO("7c. millis wrap during a turn");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"B", 2, 0}}, 0xFFFFFF00u, true);
    CHECK(r.step(0xFFFFFF00u) && strcmp(r.pc.cur.id, "A") == 0);
    CHECK(r.step(0xFFFFFF00u + 9000u) && strcmp(r.pc.cur.id, "A") == 0);   // wraps past 2^32
    CHECK(r.step(0xFFFFFF00u + 10000u) && strcmp(r.pc.cur.id, "B") == 0);
  }
  END();

  SCENARIO("7d. duplicate id in an answer is listed once; more than 10 planes are cut at 10");
  {
    Rig r;
    fetch(r.pc, {{"A", 1, 0}, {"A", 1, 0}, {"B", 2, 0}}, 0, true);
    CHECK(r.pc.n == 2);
    PlaneCycle<TP> big;
    big.n = 0;
    char ids[12][3];
    for (int i = 0; i < 12; i++) { snprintf(ids[i], 3, "%c", 'a' + i); TP q; strcpy(q.id, ids[i]); q.dst = 12 - i; if (big.n < CYCLE_MAX && big.find(q.id) < 0) big.list[big.n++] = q; }
    big.listReplaced(true);
    CHECK(big.n == 10);
    for (int i = 1; i < big.n; i++) CHECK(big.list[i - 1].dst <= big.list[i].dst);
  }
  END();

  SCENARIO("8. ten planes: every one gets exactly one turn per round");
  {
    Rig r;
    char ids[10][3];
    PlaneCycle<TP> &pc = r.pc;
    pc.n = 0;
    for (int i = 0; i < 10; i++) { snprintf(ids[i], 3, "%c", 'a' + i); TP q; strcpy(q.id, ids[i]); q.dst = 1 + i; pc.list[pc.n++] = q; }
    pc.listReplaced(true);
    int count[10] = {0};
    for (uint32_t t = 0; t < 200000; t += 100) {
      bool a = r.step(t);
      CHECK(a);
      if (a && t % 10000 == 0) count[pc.cur.id[0] - 'a']++;   // sample at each turn start
    }
    for (int i = 0; i < 10; i++) CHECK(count[i] == 2);
  }
  END();

  printf("\n%d checks, %d failed\n", checks, fails);
  (void)tail;
  return fails ? 1 : 0;
}
