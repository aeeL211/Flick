// foreground detection test. it builds flick.cpp with FLICK_PROC_ROOT pointing at a fake proc tree,
// so the incremental scan and the "skip while already foreground" path can be checked on the host
// without a phone and without touching the real /proc.
//
//   g++ -std=c++17 -O2 -ffp-contract=off -DFLICK_PROC_TEST
//       -DFLICK_PROC_ROOT='"/tmp/flick-proc-test"' proc_test.cpp -o proc-test
//   ./proc-test
#define main flickMain
#include "flick.cpp"
#undef main

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int gFails = 0;

static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) gFails++;
}

static bool writeFile(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), (std::streamsize)data.size());
  return f.good();
}

static bool makePid(int pid, const char *name, const char *adj) {
  std::string dir = std::string(FLICK_PROC_ROOT) + "/" + std::to_string(pid);
  std::error_code ec;
  fs::create_directories(dir, ec);
  return !ec && writeFile(dir + "/cmdline", name) && writeFile(dir + "/oom_score_adj", adj);
}

static bool setAdj(int pid, const char *adj) {
  return writeFile(std::string(FLICK_PROC_ROOT) + "/" + std::to_string(pid) + "/oom_score_adj", adj);
}

static void closeTracked() {
  for (int i = 0; i < gAdjCount; i++) close(gAdjFds[i]);
  gAdjCount = 0;
}

int main() {
  std::error_code ec;
  fs::remove_all(FLICK_PROC_ROOT, ec);
  fs::create_directories(FLICK_PROC_ROOT, ec);
  if (ec) {
    printf("FAIL cannot create %s: %s\nSELFTEST PROC FAIL\n", FLICK_PROC_ROOT, ec.message().c_str());
    return 1;
  }

  gPkgCount = 1;
  snprintf(gPkgs[0], pkgLen, "%s", "com.dts.freefireth");
  gPrevCount = 0;
  gProcStale = false;
  gProcScans = 0;

  check(makePid(101, "com.dts.freefireth", "0\n"), "fake process created");
  check(inGame(), "foreground target detected");
  check(gProcScans == 1, "one full scan finds the target");

  check(inGame(), "target still foreground");
  check(gProcScans == 1, "no scan while already foreground");

  check(setAdj(101, "500\n"), "target moved to background");
  check(!inGame(), "background target detected");
  check(gProcScans == 2, "one rescan after the foreground period");
  check(!gProcStale, "pid cache is fresh after the transition scan");

  check(makePid(202, "com.dts.freefireth", "0\n"), "second target process created");
  check(inGame(), "new foreground process detected");
  check(gProcScans == 3, "incremental scan finds the new pid");

  closeTracked();
  fs::remove_all(FLICK_PROC_ROOT, ec);
  printf("%s\n", gFails ? "SELFTEST PROC FAIL" : "SELFTEST PROC PASS");
  return gFails ? 1 : 0;
}
