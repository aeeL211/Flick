// host microbenchmark for the per-frame touch math. it processes complete SYN_REPORT frames through
// handleEvent/closeFrame, the same code path used after pump() read a batch. it is not a device
// benchmark; it exists to compare compiler modes on identical code and keep the -O2/-Os choice honest.
//
// build twice and run:
//   g++ -std=c++17 -O2 -ffp-contract=off -DFLICK_BENCH_OPT=2 tools/bench.cpp -o bench-o2 -lm
//   g++ -std=c++17 -Os -ffp-contract=off -DFLICK_BENCH_OPT=0 tools/bench.cpp -o bench-os -lm
#define main flickMain
#include "../flick.cpp"
#undef main

#include <time.h>

static double nowNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main() {
  memset(gAbs, 0, sizeof gAbs);
  gAbs[ABS_MT_POSITION_X].maximum = 1079;
  gAbs[ABS_MT_POSITION_Y].maximum = 2399;
  gAbs[ABS_MT_SLOT].maximum = 9;
  gVertX = true;
  gLongSide = 2399.0f;
  gInvShort = 1.0f / 1079.0f;

  Config c;
  c.sensH = 2.0f;
  c.sensV = 3.0f;
  c.curve = curveAccel;
  c.smooth = 1;
  c.minCut = 8.0f;
  c.beta = 12.0f;

  static input_event b[batchSize];
  constexpr int batches = 40000;
  double events = 0.0;
  uint64_t sum = 0;
  double t0 = nowNs();
  for (int k = 0; k < batches; k++) {
    memset(gSlots, 0, sizeof gSlots);
    gCurSlot = 0;
    gDirty = 0;
    gDown = 0;
    int n = 0;
    int64_t ms = 1000 + k;
    b[n].type = EV_ABS; b[n].code = ABS_MT_SLOT; b[n++].value = 0;
    b[n].type = EV_ABS; b[n].code = ABS_MT_TRACKING_ID; b[n++].value = 1;
    int x = 500, y = 1000;
    while (n + 3 <= batchSize) {
      x += 3 - (k % 7);
      y += 2 - (k % 5);
      b[n].type = EV_ABS; b[n].code = ABS_MT_POSITION_X; b[n++].value = x;
      b[n].type = EV_ABS; b[n].code = ABS_MT_POSITION_Y; b[n++].value = y;
      b[n].type = EV_SYN; b[n].code = SYN_REPORT; b[n++].value = 0;
      b[n - 1].time.tv_sec = (time_t)(ms / 1000);
      b[n - 1].time.tv_usec = (suseconds_t)((ms % 1000) * 1000);
      ms++;
    }
    for (int i = 0; i < n; i++) handleEvent(b, i, c);
    if (gDirty) closeFrame(b, timeMs(b[n - 1]), c);
    events += (double)n;
    for (int i = 0; i < n; i++) sum += (uint64_t)(uint32_t)b[i].value;
  }
  double dt = nowNs() - t0;
  printf("BENCH opt=-O%s batches=%d events=%.0f ns_per_event=%.2f checksum=%llu\n",
         FLICK_BENCH_OPT ? "2" : "s", batches, events, dt / events, (unsigned long long)sum);
  return 0;
}
