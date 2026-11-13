// flick is a no-root touchscreen sensitivity daemon for free fire and free fire max. it runs as the
// adb shell user (uid 2000) inside an axmanager plugin.
//
// idle state: nothing is grabbed and no touch event is read. the daemon wakes once per tick to look
// for the game. active state: the game is in the foreground, the real touchscreen is grabbed with
// eviocgrab, finger movement is scaled and optionally smoothed, then re-emitted through a uinput
// clone of the panel.
//
// safety rules. if this process dies the kernel drops the grab and normal touch returns. never grab
// while a finger is down, and never grab when that cannot be checked. release the grab only when no
// virtual finger is down, after a syn_dropped the fingers are lifted explicitly first. no dumpsys, ps
// or other child process in the polling path.
//
// cli: flick --daemon --config file --pidfile file --dev device. once the daemon is up it prints one
// ok line, otherwise one err line and an exit code. 1 already running, 2 touchscreen not found,
// 3 uinput failure, 5 daemonize failure, 6 proc hidden.
//
// files live next to the config. config is key=value and is reloaded on change through inotify with
// an mtime fallback. flick.pid holds the daemon pid. flick.state holds idle, active, or error
// followed by a colon and the reason. hw.sh is an optional hook called with 1 or 0 while flick is
// applied. notify.sh is called with active or idle on every change between the two and posts the
// notification. ctl.sh is the entry point of the webui (status, save, start, stop), it only calls this
// binary and reads these files.
//
// build: arm64 and armv7a (32-bit), see .github/workflows/build.yml.
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The proc paths are compile-time constants in production. selftest builds may point them at a
// fake tree to exercise foreground detection without touching the host's real /proc.
#ifndef FLICK_PROC_ROOT
#define FLICK_PROC_ROOT "/proc"
#endif

// The release workflow passes the module version here. Host tests and development builds use dev.
#ifndef FLICK_VERSION
#define FLICK_VERSION "dev"
#endif

constexpr int maxSlot = 32;
constexpr int maxPid = 8192;
constexpr int tickMs = 3000;
constexpr int retryMs = 250;
constexpr int pkgBuf = 1024;
constexpr int maxPkgs = 16;
constexpr int pkgLen = 128;
constexpr int batchSize = 256;
constexpr int ctlLineMax = 1024;
constexpr int ctlKeyMax = 31;
constexpr int ctlValueMax = 1000;
constexpr int ctlTokenMin = 32;
constexpr int ctlTokenMax = 256;
constexpr int ctlOutMax = 4096;
constexpr int ctlChunk = 512;
constexpr int64_t ctlAuthMs = 2000;
constexpr int64_t ctlLingerMs = 300;
constexpr int64_t ctlPersistMs = 500;
constexpr char defPkgs[] = "com.dts.freefireth,com.dts.freefiremax";
constexpr float curveRef = 0.04f;
constexpr float accelMax = 1.8f;
constexpr float decelDrop = 0.5f;
enum Curve { curveLinear, curveAccel, curveDecel };

constexpr int bitBytes(int n) { return n / 8 + 1; }

static volatile sig_atomic_t gStop = 0;
static void onSignal(int) { gStop = 1; }

static char gDir[256] = ".";
static constexpr const char *gProcRoot = FLICK_PROC_ROOT;
static const char *gCfgPath = "config";

// origin of the calculation. the frame factor (the blend of sens_h and sens_v by the direction of the
// finger, the speed curve) and the filter parameters follow the Magic Touch feature of the Game
// Corner app by FahrezONE, taken from that app by reverse engineering as reported by the project
// owner. only the calculation comes from there, the daemon around it (grab, uinput clone,
// foreground detection, config, webui) is this project's own. the reference numbers that
// selftest.cpp checks against come from the same source.
//
// config keys are all optional and malformed values are ignored. sens_h and sens_v are 1.0 to 5.0
// on the screen axes while the game is landscape, h is left to right and v is up and down. zone is 0
// for the whole screen, 1 for the left half, 2 for the right half. orient is 1 or 3 and tells which
// landscape rotation is in use, 1 means screen-left is the low raw value. smooth 1 turns the one
// euro filter on, strength and resp are 1 to 100 percent. curve is 0 linear (default), 1 accel,
// 2 decel. hw 1 runs hw.sh 1 while flick is applied (setprop debug.sf.hw) and restores it afterwards.
// filter 1 applies flick only while one of the packages is in the foreground, 0 applies it always.
// pkgs is a comma separated package list for filter 1, letters digits underscore and dot only, 16 at
// most, empty or invalid falls back to the two free fire packages.
//
// the scaled position is never clamped to the panel, so a swipe cannot stall at the edge.
// the factor of a frame is k times m. k blends sens_h and sens_v by the direction of the finger
// movement, k is h to the power 1 minus w times v to the power w, where w is the share of the
// vertical travel in the frame. m depends on the curve and on the travel of that frame, both axes
// together, measured on the raw samples. u is the travel over the long side over 0.04, clamped to 0
// to 1. linear has m of 1. accel has m of 1 plus 0.8 u, so slow aim keeps k and a fast flick gets up
// to 1.8 k. decel has m of 1 minus 0.5 u, so slow aim keeps k and a fast flick gets down to 0.5 k.
// the numbers follow the reference implementation sample by sample, the webui draws these curves.
// k, smooth, curve and zone are fixed when a finger touches down, so a finger already on the screen
// never jumps. strength and resp apply live, the filter is continuous so nothing jumps.
// defaults of strength 40 and resp 85 come from simulating free fire style aiming: about half of the
// resting jitter is removed while a fast flick is delayed by less than one touch frame.
struct Config {
  float sensH = 1.0f, sensV = 1.0f;
  int zone = 0, orient = 1;
  int smooth = 0, strength = 40, resp = 85;
  int curve = curveLinear, hw = 0;
  int filter = 1;
  char pkgs[pkgBuf];
  float minCut = 3.0f, beta = 5.0f;  // derived one euro parameters
  Config() { memcpy(pkgs, defPkgs, sizeof defPkgs); memset(pkgs + sizeof defPkgs, 0, sizeof pkgs - sizeof defPkgs); }
};

static void setPkgs(const char *s);
static bool pkgChar(char ch);

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// strength maps to the min cutoff, 20 hz for light down to 0.5 hz for heavy. resp maps to beta, 0.5
// to 60 hz per panel width per second, which is how fast the filter opens on fast swipes.
static void deriveFilter(Config &c) {
  float s = (float)(c.strength - 1) / 99.0f, r = (float)(c.resp - 1) / 99.0f;
  c.minCut = 20.0f * powf(0.025f, s);
  c.beta = 0.5f * powf(120.0f, r);
}

// One config pair can come from the file or from the control protocol. The file keeps its
// historical lenient behavior: a value that does not start with a digit is ignored and numbers
// are clamped. The protocol uses the same ranges in strict mode, where the whole value must parse
// and the result must be inside the range. A strict SET is applied only after every pair passes.
enum ApplyResult { applyOk, applyBad, applyUnknown };

static bool strictFloat(const char *v, float &out) {
  if (*v < '0' || *v > '9') return false;
  char *end = nullptr;
  errno = 0;
  double d = strtod(v, &end);
  if (end == v || *end || errno == ERANGE || !isfinite(d)) return false;
  out = (float)d;
  return true;
}

static bool strictInt(const char *v, int &out) {
  if (*v < '0' || *v > '9') return false;
  char *end = nullptr;
  errno = 0;
  long x = strtol(v, &end, 10);
  if (end == v || *end || errno == ERANGE || x < INT32_MIN || x > INT32_MAX) return false;
  out = (int)x;
  return true;
}

static bool applyFloat(const char *v, float &dst, float lo, float hi, bool strict, const char **why) {
  float x;
  if (strict) {
    if (!strictFloat(v, x)) { *why = "invalid"; return false; }
    if (x < lo || x > hi) { *why = "range"; return false; }
  } else {
    if (*v < '0' || *v > '9') return false;
    x = (float)atof(v);
  }
  dst = clampf(x, lo, hi);
  return true;
}

static bool applyInt(const char *v, int &dst, int lo, int hi, bool strict, const char **why) {
  int x;
  if (strict) {
    if (!strictInt(v, x)) { *why = "invalid"; return false; }
    if (x < lo || x > hi) { *why = "range"; return false; }
  } else {
    if (*v < '0' || *v > '9') return false;
    x = atoi(v);
  }
  dst = clampi(x, lo, hi);
  return true;
}

static bool applyBool(const char *v, int &dst, bool strict, const char **why) {
  int x;
  if (strict) {
    if (!strictInt(v, x)) { *why = "invalid"; return false; }
    if (x != 0 && x != 1) { *why = "range"; return false; }
  } else {
    if (*v < '0' || *v > '9') return false;
    x = atoi(v) != 0;
  }
  dst = x != 0;
  return true;
}

static bool applyOrient(const char *v, int &dst, bool strict, const char **why) {
  int x;
  if (strict) {
    if (!strictInt(v, x)) { *why = "invalid"; return false; }
    if (x != 1 && x != 3) { *why = "range"; return false; }
  } else {
    if (*v < '0' || *v > '9') return false;
    x = atoi(v);
  }
  dst = x == 3 ? 3 : 1;
  return true;
}

// the package list rules are shared with parsePkgs, but the protocol rejects a malformed list
// instead of silently dropping tokens or falling back to the defaults.
static bool validPkgList(const char *v, const char **why) {
  size_t len = strlen(v);
  if (!len) { *why = "invalid"; return false; }
  if (len >= pkgBuf) { *why = "length"; return false; }
  int n = 0;
  const char *s = v;
  for (;;) {
    const char *t = s;
    while (*s && *s != ',') s++;
    size_t l = (size_t)(s - t);
    if (!l) { *why = "invalid"; return false; }
    if (l >= pkgLen) { *why = "length"; return false; }
    for (size_t i = 0; i < l; i++)
      if (!pkgChar(t[i])) { *why = "invalid"; return false; }
    if (++n > maxPkgs) { *why = "range"; return false; }
    if (!*s) break;
    s++;
  }
  return true;
}

// keys and values are bounded by the caller; unknown keys are ignored by the file parser and
// reported by the protocol.
static ApplyResult applySetting(const char *k, const char *v, Config &c, bool strict, const char **why) {
  if (!strcmp(k, "pkgs")) {
    if (strict && !validPkgList(v, why)) return applyBad;
    snprintf(c.pkgs, sizeof c.pkgs, "%s", v);
    return applyOk;
  }
  bool ok;
  if (!strcmp(k, "sens_h")) ok = applyFloat(v, c.sensH, 1.0f, 5.0f, strict, why);
  else if (!strcmp(k, "sens_v")) ok = applyFloat(v, c.sensV, 1.0f, 5.0f, strict, why);
  else if (!strcmp(k, "zone")) ok = applyInt(v, c.zone, 0, 2, strict, why);
  else if (!strcmp(k, "orient")) ok = applyOrient(v, c.orient, strict, why);
  else if (!strcmp(k, "smooth")) ok = applyBool(v, c.smooth, strict, why);
  else if (!strcmp(k, "strength")) ok = applyInt(v, c.strength, 1, 100, strict, why);
  else if (!strcmp(k, "resp")) ok = applyInt(v, c.resp, 1, 100, strict, why);
  else if (!strcmp(k, "curve")) ok = applyInt(v, c.curve, 0, 2, strict, why);
  else if (!strcmp(k, "hw")) ok = applyBool(v, c.hw, strict, why);
  else if (!strcmp(k, "filter")) ok = applyBool(v, c.filter, strict, why);
  else {
    if (strict) *why = "unknown";
    return applyUnknown;
  }
  return ok ? applyOk : applyBad;
}

// applies one config line, key=value with the value cut at the line end. a value that does not
// start with a digit is malformed and the previous setting stays, pkgs is the only text value.
static void parseLine(char *line, Config &c) {
  char *eq = strchr(line, '=');
  if (!eq) return;
  *eq = 0;
  const char *k = line;
  char *v = eq + 1;
  v[strcspn(v, "\r\n")] = 0;
  (void)applySetting(k, v, c, false, nullptr);
}

// reads the config in chunks and cuts it into lines. a line longer than the line buffer is dropped
// whole, the tail of it included, so it can never be mistaken for a key.
static void loadConfig(const char *path, Config &c) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    char chunk[1024], line[pkgBuf + 64];
    size_t len = 0;
    bool skip = false;
    for (;;) {
      ssize_t n = read(fd, chunk, sizeof chunk);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      for (ssize_t i = 0; i < n; i++) {
        if (chunk[i] == '\n') {
          if (!skip) { line[len] = 0; parseLine(line, c); }
          len = 0;
          skip = false;
        } else if (len < sizeof line - 2) {
          line[len++] = chunk[i];
        } else {
          skip = true;
        }
      }
    }
    if (len && !skip) { line[len] = 0; parseLine(line, c); }
    close(fd);
  }
  deriveFilter(c);
  setPkgs(c.pkgs);
}

static bool writeAll(int fd, const void *buf, size_t len);

// formats one short line and writes it with a single write, used for the ok and err lines
static void say(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int fd, const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)writeAll(fd, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

// the state file holds idle, active, or error with a reason. it is rewritten only when the text
// changes. the flag next to it is the applied value as last reported in that file.
static char gState[64];
static bool gOn = false;
static bool gCtlEnabled = false;
static bool gPaused = false;
static unsigned long long gStatGrabs = 0, gStatReleases = 0, gStatSynDropped = 0;
static void ctlPublishState();

static void setState(const char *s) {
  if (!strcmp(s, gState)) return;
  snprintf(gState, sizeof gState, "%s", s);
  char p[300];
  snprintf(p, sizeof p, "%s/flick.state", gDir);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
  ctlPublishState();
}

// foreground detection uses no dumpsys. proc is fully visible to the shell user through the readproc
// group. the main process of an app is named after its package and the top app has an oom_score_adj
// of 0 or less. every tick lists proc and reads cmdline only for pids that are new since the last
// tick. on a match the oom_score_adj file stays open and one pread per tick tells whether any match
// is in the foreground.
static char gPkgs[maxPkgs][pkgLen];
static int gPkgCount = 0;
static int gAdjFds[maxPkgs];
static int gAdjCount = 0;
static int gAdjErr = 0;
static int gPidBuf[3][maxPid];
static int *gPrev = gPidBuf[0], *gSnap = gPidBuf[1], *gCur = gPidBuf[2];
static int gPrevCount = 0;
static bool gProcStale = false;  // a foreground period skipped scans, so the pid cache may be old
static unsigned long long gProcScans = 0;

static bool pkgChar(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
         ch == '.';
}

// splits a comma list into names. blank or invalid tokens are skipped, returns how many were kept.
static int parsePkgs(const char *s, char out[][pkgLen]) {
  int n = 0;
  while (*s && n < maxPkgs) {
    while (*s == ',' || *s == ' ' || *s == '\t') s++;
    const char *t = s;
    while (*s && *s != ',') s++;
    const char *e = s;
    while (e > t && (e[-1] == ' ' || e[-1] == '\t')) e--;
    int len = (int)(e - t);
    bool ok = len > 0 && len < pkgLen;
    for (int i = 0; ok && i < len; i++) ok = pkgChar(t[i]);
    if (ok) { memcpy(out[n], t, (size_t)len); out[n][len] = 0; n++; }
  }
  return n;
}

// a changed list drops the tracked processes and forgets the seen pids, so proc is rescanned
static void setPkgs(const char *s) {
  char tmp[maxPkgs][pkgLen] = {};
  int n = parsePkgs(s, tmp);
  if (!n) n = parsePkgs(defPkgs, tmp);
  bool same = n == gPkgCount;
  for (int i = 0; same && i < n; i++) same = !strcmp(tmp[i], gPkgs[i]);
  if (same) return;
  memcpy(gPkgs, tmp, sizeof gPkgs);
  gPkgCount = n;
  for (int i = 0; i < gAdjCount; i++) close(gAdjFds[i]);
  gAdjCount = 0;
  gAdjErr = 0;
  gPrevCount = 0;
  gProcStale = false;
}

static int cmpInt(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

// one record of getdents64, the same layout on arm64 and arm
struct DirEntry {
  uint64_t ino;
  int64_t off;
  uint16_t len;
  uint8_t type;
  char name[1];
};

// lists the numeric entries of proc, ascending, at most max. reads the directory with getdents64
// straight into a static buffer so the polling tick never allocates.
static int readPids(int *out, int max) {
  alignas(8) static char buf[16384];
  int fd = open(gProcRoot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -1;
  int n = 0;
  for (;;) {
    long got = syscall(SYS_getdents64, fd, buf, sizeof buf);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    for (long off = 0; off < got;) {
      // buf is alignas(8) and getdents64 records are 8-byte aligned; void* keeps -Wcast-align quiet
      const DirEntry *e = static_cast<const DirEntry *>(static_cast<const void *>(buf + off));
      if (e->len == 0) { got = 0; break; }
      off += e->len;
      if (e->name[0] < '1' || e->name[0] > '9') continue;
      if (n < max) out[n++] = atoi(e->name);
    }
  }
  close(fd);
  for (int i = 1; i < n; i++)
    if (out[i - 1] > out[i]) { qsort(out, (size_t)n, sizeof(int), cmpInt); break; }
  return n;
}

// 1 is a listed app, 0 is not one and final, -1 is not renamed yet so look again next tick
static int checkPid(int pid) {
  char p[128], b[pkgLen + 32];
  snprintf(p, sizeof p, "%s/%d/cmdline", gProcRoot, pid);
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0) return 0;
  b[n] = 0;
  for (int i = 0; i < gPkgCount; i++)
    if (!strcmp(b, gPkgs[i])) return 1;
  return (!strncmp(b, "zygote", 6) || b[0] == '<') ? -1 : 0;
}

// looks at pids that appeared since the last tick and keeps oom_score_adj open for listed apps
static void scanProc() {
  gProcScans++;
  int n = readPids(gCur, maxPid);
  if (n < 0) return;
  gAdjErr = 0;
  int sn = 0, j = 0;
  for (int i = 0; i < n; i++) {
    int pid = gCur[i];
    while (j < gPrevCount && gPrev[j] < pid) j++;
    if (j < gPrevCount && gPrev[j] == pid) { gSnap[sn++] = pid; continue; }
    int r = checkPid(pid);
    if (r == 1 && gAdjCount < maxPkgs) {
      char p[128];
      snprintf(p, sizeof p, "%s/%d/oom_score_adj", gProcRoot, pid);
      int fd = open(p, O_RDONLY | O_CLOEXEC);
      if (fd < 0) { gAdjErr = errno; continue; }
      gAdjFds[gAdjCount++] = fd;
    }
    if (r >= 0) gSnap[sn++] = pid;
  }
  int *t = gPrev;
  gPrev = gSnap;
  gSnap = t;
  gPrevCount = sn;
}

// checks only the already tracked oom_score_adj fds, closing ones whose process is gone
static bool trackedForeground() {
  bool fg = false;
  for (int i = 0; i < gAdjCount;) {
    char b[16];
    ssize_t n = pread(gAdjFds[i], b, sizeof b - 1, 0);
    if (n <= 0) {
      close(gAdjFds[i]);
      gAdjFds[i] = gAdjFds[--gAdjCount];
      continue;
    }
    b[n] = 0;
    if (atoi(b) <= 0) fg = true;
    i++;
  }
  return fg;
}

// While a tracked app is already foreground the answer cannot be false, so the expensive /proc walk
// is skipped. If a foreground period skipped scans, the pid cache may be old (pid reuse), so the
// first scan after it is a full one. Idle ticks keep the incremental scan from the original design.
static bool inGame() {
  if (trackedForeground()) {
    gProcStale = true;
    return true;
  }
  if (gProcStale) {
    gPrevCount = 0;
    gProcStale = false;
  }
  scanProc();
  return trackedForeground();
}

static bool hasBit(const unsigned char *b, int n) { return b[n / 8] & (1 << (n % 8)); }

static input_absinfo gAbs[ABS_CNT];
static bool gVertX = true;
static char gPath[64];
static int gUinput = -1;
static int gSource = -1;

static int findTouch(char *path, size_t len) {
  for (int i = 0; i < 32; i++) {
    snprintf(path, len, "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned char bits[bitBytes(ABS_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof bits), bits) >= 0 && hasBit(bits, ABS_MT_SLOT) &&
        hasBit(bits, ABS_MT_POSITION_X) && hasBit(bits, ABS_MT_POSITION_Y))
      return fd;
    close(fd);
  }
  return -1;
}

static int fail(int u, const char *what) {
  say(2, "ERR %s errno=%d\n", what, errno);
  if (u >= 0) close(u);
  return -1;
}

static int cloneDev(int src) {
  int u = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  if (u < 0) return fail(-1, "open /dev/uinput");
  unsigned char bits[bitBytes(KEY_MAX)];
  bool have[ABS_CNT] = {false};

  if (ioctl(u, UI_SET_EVBIT, EV_SYN) < 0 || ioctl(u, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(u, UI_SET_EVBIT, EV_ABS) < 0)
    return fail(u, "uinput ioctl blocked");

  memset(bits, 0, sizeof bits);
  ioctl(src, EVIOCGBIT(EV_KEY, sizeof bits), bits);
  for (int k = 0; k <= KEY_MAX; k++) if (hasBit(bits, k)) ioctl(u, UI_SET_KEYBIT, k);

  memset(bits, 0, sizeof bits);
  ioctl(src, EVIOCGBIT(EV_ABS, sizeof bits), bits);
  for (int a = 0; a <= ABS_MAX; a++)
    if (hasBit(bits, a)) {
      if (ioctl(u, UI_SET_ABSBIT, a) < 0 || ioctl(src, EVIOCGABS(a), &gAbs[a]) < 0)
        return fail(u, "abs axis copy");
      have[a] = true;
    }
  if (!have[ABS_MT_SLOT] || !have[ABS_MT_POSITION_X] || !have[ABS_MT_POSITION_Y])
    return fail(u, "not a multitouch panel");

  memset(bits, 0, sizeof bits);
  if (ioctl(src, EVIOCGPROP(sizeof bits), bits) >= 0)
    for (int p = 0; p <= INPUT_PROP_MAX; p++) if (hasBit(bits, p)) ioctl(u, UI_SET_PROPBIT, p);

  input_id id;
  memset(&id, 0, sizeof id);
  ioctl(src, EVIOCGID, &id);
  char name[80] = "touch";
  ioctl(src, EVIOCGNAME(sizeof name - 1), name);

  uinput_setup us;
  memset(&us, 0, sizeof us);
  us.id = id;
  snprintf(us.name, UINPUT_MAX_NAME_SIZE, "%s", name);
  if (ioctl(u, UI_DEV_SETUP, &us) == 0) {
    for (int a = 0; a <= ABS_MAX; a++)
      if (have[a]) {
        uinput_abs_setup as;
        memset(&as, 0, sizeof as);
        as.code = a;
        as.absinfo = gAbs[a];
        if (ioctl(u, UI_ABS_SETUP, &as) < 0) return fail(u, "UI_ABS_SETUP");
      }
  } else {
    uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    ud.id = id;
    for (int a = 0; a <= ABS_MAX; a++)
      if (have[a]) {
        ud.absmin[a] = gAbs[a].minimum;
        ud.absmax[a] = gAbs[a].maximum;
        ud.absfuzz[a] = gAbs[a].fuzz;
        ud.absflat[a] = gAbs[a].flat;
      }
    if (write(u, &ud, sizeof ud) < 0) return fail(u, "uinput setup");
  }
  if (ioctl(u, UI_DEV_CREATE) < 0) return fail(u, "UI_DEV_CREATE");
  return u;
}

// per finger, an optional one euro filter on the raw position (casiez et al., chi 2012) and then
// scaling of the filtered movement around the touch-down point. filtering comes first so jitter is
// not amplified. the scaled position is never clamped to the panel, so a swipe never stalls before
// the finger reaches the screen edge, games only look at the movement between events. both axes of
// a finger are handled together when the frame closes at syn_report, because the factor depends on
// the movement of x and y at once. the events of the frame are patched in place.
struct Axis {
  bool known, started, dirty;  // position known, filter started, new value waiting for the frame end
  int raw, next, index;        // last raw value, new raw value, position of its event in the batch
  float hat, speed, prev, out;  // filtered position, its speed, previous filtered position, scaled position
  int64_t time;                // time of the last filter step in ms
};
struct Slot {
  Axis x, y;
  bool zoneSet, active, smooth;  // active is false when this finger needs no work at all
  int curve;
  float sensH, sensV;
};
static Slot gSlots[maxSlot + 1];
static int gCurSlot = 0;
static input_event gBatch[batchSize];
static uint64_t gDown = 0;
static uint64_t gDirty = 0;
static float gInvShort = 1.0f / 720;
static float gLongSide = 2400.0f;

static float alphaOf(float cutoff, float dt) { return 1.0f / (1.0f + 1.0f / (6.2831853f * cutoff) / dt); }

// m of the curve for a travel of speed, as a share of the long side, in one frame
static float curveFactor(int curve, float speed) {
  if (curve == curveLinear) return 1.0f;
  float u = clampf(speed / curveRef, 0.0f, 1.0f);
  if (curve == curveAccel) return u * (accelMax - 1.0f) + 1.0f;
  if (curve == curveDecel) return 1.0f - u * decelDrop;
  return 1.0f;
}

// geometric blend of two sensitivities, w of 0 gives a and w of 1 gives b
static float blendGain(float a, float b, float w) {
  if (a == b) return a;
  return (float)pow((double)a, (double)(1.0f - w)) * (float)pow((double)b, (double)w);
}

// factor of one frame from the raw travel in screen terms, dh is left to right and dv is up and down
static float gainOf(float sensH, float sensV, int curve, float dh, float dv) {
  float l1 = fabsf(dh) + fabsf(dv);
  if (!(l1 > 1e-4f)) return 1.0f;
  return blendGain(sensH, sensV, fabsf(dv) / l1) * curveFactor(curve, l1 / gLongSide);
}

// one euro step on one axis. time is in whole ms and dt has no upper bound. the speed estimate is
// taken against the previous filtered value, the first step of a touch passes the value through.
static float euroStep(Axis &a, float x, int64_t ms, const Config &c) {
  if (!a.started) {
    a.started = true;
    a.hat = x;
    a.speed = 0.0f;
    a.time = ms;
    return x;
  }
  float dt = (float)(ms - a.time) / 1000.0f;
  if (!(dt > 0.0f)) dt = 0.001f;
  a.time = ms;
  a.speed += alphaOf(1.0f, dt) * ((x - a.hat) / dt - a.speed);
  float cut = fabsf(a.speed) * c.beta * gInvShort + c.minCut;
  a.hat += alphaOf(cut, dt) * (x - a.hat);
  return a.hat;
}

static void startTouch(Slot &s, const Config *c) {
  s = Slot();
  if (!c) return;
  s.sensH = c->sensH;
  s.sensV = c->sensV;
  s.smooth = c->smooth != 0;
  s.curve = c->curve;
  s.zoneSet = c->zone == 0;
  s.active = s.smooth || s.sensH > 1.0f || s.sensV > 1.0f || s.curve != curveLinear;
}

static void pickZone(Slot &s, int v, const input_absinfo &r, const Config &c) {
  s.zoneSet = true;
  int64_t span = (int64_t)r.maximum - r.minimum;
  if (c.zone == 0 || span <= 0) return;
  bool low = ((int64_t)v - r.minimum) * 2 < span;
  bool in = (c.zone == 1) == (low == (c.orient == 1));
  if (!in) {
    s.sensH = s.sensV = 1.0f;
    s.smooth = false;
    s.curve = curveLinear;
    s.active = false;
  }
}

// floor of v plus one half as an int. it saturates so a runaway value can never overflow the cast.
static int roundInt(float v) {
  float y = v + 0.5f;
  if (!(y > -2.0e9f)) return y != y ? 0 : -2000000000;
  if (y > 2.0e9f) return 2000000000;
  int i = (int)y;
  return (float)i > y ? i - 1 : i;
}

static int64_t timeMs(const input_event &e) { return (int64_t)e.time.tv_sec * 1000 + e.time.tv_usec / 1000; }

// closes the frame of one finger. the factor comes from the raw travel of both axes, then the filter
// and the scaling run per axis and the result goes back into the event.
static void closeSlot(Slot &s, input_event *b, int64_t ms, const Config &c) {
  Axis *ax[2] = {&s.x, &s.y};
  float d[2] = {0.0f, 0.0f};
  bool moved[2] = {false, false};
  for (int i = 0; i < 2; i++) {
    Axis &a = *ax[i];
    if (!a.dirty) continue;
    a.dirty = false;
    if (!s.active) continue;
    if (!a.known) {
      a.known = true;
      a.raw = a.next;
      a.hat = a.prev = a.out = (float)a.next;
      a.speed = 0.0f;
      continue;
    }
    d[i] = (float)((int64_t)a.next - a.raw);
    moved[i] = true;
  }
  if (!moved[0] && !moved[1]) return;
  float g = gVertX ? gainOf(s.sensH, s.sensV, s.curve, d[1], d[0]) : gainOf(s.sensH, s.sensV, s.curve, d[0], d[1]);
  for (int i = 0; i < 2; i++) {
    if (!moved[i]) continue;
    Axis &a = *ax[i];
    float fx = (float)a.next;
    if (s.smooth) fx = euroStep(a, fx, ms, c);
    a.raw = a.next;
    float dl = fx - a.prev;
    a.prev = fx;
    a.out += dl * g;
    b[a.index].value = roundInt(a.out);
  }
}

static void closeFrame(input_event *b, int64_t ms, const Config &c) {
  uint64_t m = gDirty;
  gDirty = 0;
  for (; m; m &= m - 1) closeSlot(gSlots[__builtin_ctzll(m)], b, ms, c);
}

// the rest of the batch is stale after a syn_dropped, so the open frame is forgotten and its
// events go out unchanged
static void dropFrame() {
  for (uint64_t m = gDirty; m; m &= m - 1) {
    Slot &s = gSlots[__builtin_ctzll(m)];
    s.x.dirty = s.y.dirty = false;
  }
  gDirty = 0;
}

static void markAxis(Slot &s, Axis &a, int v, int index) {
  if (!s.active || gCurSlot >= maxSlot) return;
  a.next = v;
  a.index = index;
  a.dirty = true;
  gDirty |= 1ull << gCurSlot;
}

static void handleEvent(input_event *b, int i, const Config &c) {
  input_event &e = b[i];
  if (e.type == EV_SYN) { if (e.code == SYN_REPORT && gDirty) closeFrame(b, timeMs(e), c); return; }
  if (e.type != EV_ABS) return;
  Slot &s = gSlots[gCurSlot];
  switch (e.code) {
    case ABS_MT_SLOT:
      gCurSlot = (e.value >= 0 && e.value < maxSlot) ? e.value : maxSlot;
      break;
    case ABS_MT_TRACKING_ID:
      if (gCurSlot < maxSlot) {
        if (e.value >= 0) gDown |= 1ull << gCurSlot; else gDown &= ~(1ull << gCurSlot);
        gDirty &= ~(1ull << gCurSlot);
      }
      startTouch(s, (e.value >= 0 && gCurSlot < maxSlot) ? &c : nullptr);
      break;
    case ABS_MT_POSITION_X:
      if (!s.zoneSet && !gVertX) pickZone(s, e.value, gAbs[ABS_MT_POSITION_X], c);
      markAxis(s, s.x, e.value, i);
      break;
    case ABS_MT_POSITION_Y:
      if (!s.zoneSet && gVertX) pickZone(s, e.value, gAbs[ABS_MT_POSITION_Y], c);
      markAxis(s, s.y, e.value, i);
      break;
  }
}

// returns 1 when a finger is down, 0 when none, -1 when that could not be told, which is never
// treated as none
static int touching(int fd) {
  int ns = clampi(gAbs[ABS_MT_SLOT].maximum + 1, 1, maxSlot);
  struct { __u32 code; __s32 v[maxSlot]; } q;
  memset(&q, 0, sizeof q);
  q.code = ABS_MT_TRACKING_ID;
  if (ioctl(fd, EVIOCGMTSLOTS(sizeof(__u32) + (size_t)ns * sizeof(__s32)), &q) < 0) return -1;
  for (int i = 0; i < ns; i++) if (q.v[i] >= 0) return 1;
  return 0;
}

// grab and release are wrapped so the control protocol can report how often the panel changed
// hands without touching the actual safety logic.
static int grabPanel(int fd) {
  int r = ioctl(fd, EVIOCGRAB, 1);
  if (r == 0) gStatGrabs++;
  return r;
}

static int releasePanel(int fd) {
  int r = ioctl(fd, EVIOCGRAB, 0);
  if (r == 0) gStatReleases++;
  return r;
}

// returns 0 when grabbed, 1 when a finger is down so retry soon, -1 on error with the reason in the
// state file
static int activate() {
  char e[64];
  int fd = open(gPath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { snprintf(e, sizeof e, "error:open errno=%d", errno); setState(e); return -1; }
  int t = touching(fd);
  if (t != 0) {
    if (t < 0) { snprintf(e, sizeof e, "error:touch errno=%d", errno); setState(e); }
    close(fd);
    return t;
  }
  if (grabPanel(fd) < 0) {
    snprintf(e, sizeof e, "error:grab errno=%d", errno);
    setState(e);
    close(fd);
    return -1;
  }
  // the check and the grab are not atomic. re-check after the grab and release at once when a finger
  // landed in the window, so the daemon never holds the panel while a touch is down.
  t = touching(fd);
  if (t != 0) {
    releasePanel(fd);
    if (t < 0) { snprintf(e, sizeof e, "error:touch errno=%d", errno); setState(e); }
    close(fd);
    return t;
  }
  while (read(fd, gBatch, sizeof gBatch) > 0) {}
  memset(gSlots, 0, sizeof gSlots);
  gCurSlot = 0;
  gDown = 0;
  gDirty = 0;
  gSource = fd;
  return 0;
}

static void deactivate() {
  releasePanel(gSource);
  close(gSource);
  gSource = -1;
}

static bool writeAll(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  while (len) {
    ssize_t w = write(fd, p, len);
    if (w < 0) { if (errno == EINTR) continue; return false; }
    p += w; len -= (size_t)w;
  }
  return true;
}

// lifts every finger android still thinks is down, used when our view of the panel is lost
static void liftAll() {
  input_event e[2 * maxSlot + 1];
  memset(e, 0, sizeof e);
  int n = 0;
  for (int s = 0; s < maxSlot; s++)
    if (gDown >> s & 1) {
      e[n].type = EV_ABS; e[n].code = ABS_MT_SLOT; e[n++].value = s;
      e[n].type = EV_ABS; e[n].code = ABS_MT_TRACKING_ID; e[n++].value = -1;
    }
  if (!n) return;
  e[n].type = EV_SYN; e[n++].code = SYN_REPORT;
  (void)writeAll(gUinput, e, (size_t)n * sizeof(input_event));
  gDown = 0;
}

// reads one batch from the real panel and forwards it. returns 1 when ok, 0 on syn_dropped, which
// means the kernel lost events and the finger state is unreliable: fingers are lifted, the caller
// releases the grab and grabs again once the panel is idle. returns -1 when fatal.
static int pump(const Config &c) {
  ssize_t n = read(gSource, gBatch, sizeof gBatch);
  if (n < 0) return (errno == EINTR || errno == EAGAIN) ? 1 : -1;
  if (n == 0) return -1;
  int cnt = (int)(n / (ssize_t)sizeof(input_event)), ok = cnt;
  for (int i = 0; i < cnt; i++) {
    if (gBatch[i].type == EV_SYN && gBatch[i].code == SYN_DROPPED) { ok = i; break; }
    handleEvent(gBatch, i, c);
  }
  if (ok != cnt) gStatSynDropped++;
  if (ok == cnt) { if (gDirty) closeFrame(gBatch, timeMs(gBatch[cnt - 1]), c); }
  else dropFrame();
  if (ok && !writeAll(gUinput, gBatch, (size_t)ok * sizeof(input_event))) return -1;
  if (ok == cnt) return 1;
  liftAll();
  return 0;
}

// runs a plugin script through sh with one argument, detached. a missing script is skipped
static void runHook(const char *name, const char *arg) {
  char sc[300];
  snprintf(sc, sizeof sc, "%s/%s", gDir, name);
  if (access(sc, R_OK) != 0) return;
  if (fork() == 0) {
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGHUP, SIG_DFL);
    execl("/system/bin/sh", "sh", sc, arg, (char *)nullptr);
    _exit(127);
  }
}
static void hwRun(bool on) { runHook("hw.sh", on ? "1" : "0"); }

// the hw property follows flick applied and hw set, also when hw changes while on
static bool gHw = false;
static void hwSync(const Config &c) {
  bool want = gOn && c.hw;
  if (want == gHw) return;
  gHw = want;
  hwRun(want);
}

// the hw.sh children are detached and sigchld is ignored, so waitpid reports no child once they are
// all gone. on exit wait for them for a short while, so that the restore of the property is done
// before this process ends, whoever sent the stop. bounded, a hung hook never delays the exit long.
static void hwWait() {
  for (int i = 0; i < 40; i++) {
    pid_t r = waitpid(-1, nullptr, WNOHANG);
    if (r < 0 && errno != EINTR) return;
    usleep(50000);
  }
}

static void onChange(bool on, const Config &c) {
  setState(on ? "active" : "idle");
  if (on == gOn) return;
  gOn = on;
  hwSync(c);
  // in app mode the application posts its own notification, the daemon must not add a second one
  if (!gCtlEnabled) runHook("notify.sh", on ? "active" : "idle");
}

static int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// reads up to cap minus one bytes of a small file into buf and ends it with a zero, returns the
// byte count or -1 when the file cannot be opened
static ssize_t readSmall(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n;
  do { n = read(fd, buf, cap - 1); } while (n < 0 && errno == EINTR);
  close(fd);
  if (n < 0) n = 0;
  buf[n] = 0;
  return n;
}

static bool running(const char *pidPath) {
  char b[32];
  if (readSmall(pidPath, b, sizeof b) < 0) return false;
  char *end;
  long p = strtol(b, &end, 10);
  if (end == b || p <= 1 || p > INT32_MAX || kill((pid_t)p, 0) != 0) return false;
  char cp[64], comm[32];
  snprintf(cp, sizeof cp, "/proc/%d/comm", (int)p);
  if (readSmall(cp, comm, sizeof comm) < 0) return false;
  return !strncmp(comm, "flick", 5);
}

static void writePid(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (fd < 0) return;
  char b[16];
  int n = snprintf(b, sizeof b, "%d\n", getpid());
  (void)writeAll(fd, b, (size_t)n);
  close(fd);
}

static void okLine(int procs, const Config &c) {
  say(1, "OK dev=%s vert_is_x=%d filter=%d pkgs=%d procs=%d\n", gPath, (int)gVertX, c.filter, gPkgCount, procs);
}

// splits the config path into the folder, kept for hw.sh and the state files, and the file name
static void splitPath(const char *cfg, char *base, size_t bn) {
  snprintf(gDir, sizeof gDir, "%s", cfg);
  snprintf(base, bn, "%s", cfg);
  if (char *sl = strrchr(gDir, '/')) {
    snprintf(base, bn, "%s", sl + 1);
    *sl = 0;
    if (!gDir[0]) snprintf(gDir, sizeof gDir, "/");
  } else {
    snprintf(gDir, sizeof gDir, ".");
  }
}

// drains inotify, true if the config file itself changed
static bool cfgChanged(int ifd, const char *base) {
  char ib[512] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t n = read(ifd, ib, sizeof ib);
  bool hit = false;
  for (char *q = ib; n > 0 && q < ib + n;) {
    // ib is aligned for inotify_event and q only moves by whole records
    inotify_event *ev = static_cast<inotify_event *>(static_cast<void *>(q));
    if (ev->len && !strcmp(ev->name, base)) hit = true;
    q += sizeof(inotify_event) + ev->len;
  }
  return hit;
}

// -------------------------------------------------------------------------------------------------
// optional control server. it lives in the same poll loop and only runs when --listen was given.
// all buffers are fixed, every socket is non-blocking, and no client input ever reaches a shell or
// a filesystem path.

enum {
  cfgBitSensH = 1u << 0,
  cfgBitSensV = 1u << 1,
  cfgBitZone = 1u << 2,
  cfgBitOrient = 1u << 3,
  cfgBitSmooth = 1u << 4,
  cfgBitStrength = 1u << 5,
  cfgBitResp = 1u << 6,
  cfgBitCurve = 1u << 7,
  cfgBitHw = 1u << 8,
  cfgBitFilter = 1u << 9,
  cfgBitPkgs = 1u << 10
};

// no default member initializers: they would place the two 5 KB connection buffers in .data
// instead of .bss. ctlConnReset() establishes the same initial state without inflating the binary.
struct CtlConn {
  int fd;
  bool authed;
  bool failed;
  bool subscribed;
  bool stopWhenDrained;
  bool peerClosed;
  int64_t acceptedMs;
  int64_t closeAtMs;
  size_t inLen;
  size_t outLen;
  char in[ctlLineMax + 1];
  char out[ctlOutMax];
};

static int gCtlListen = -1;
static unsigned gCtlPort = 0;
static char gCtlToken[ctlTokenMax + 1];
static size_t gCtlTokenLen = 0;
static CtlConn gCtlPending, gCtlClient;
static Config *gCfg = nullptr;
static int64_t gCtlStartMs = 0;
static bool gTickNow = false;
static int64_t gCtlLastPersistMs = -ctlPersistMs;
static int64_t gCtlPersistDueMs = 0;
static unsigned gCtlPendingMask = 0;
static char gCtlEvtState[16];
static int gCtlEvtPaused = -1;

// the compiler must not turn this into a dead store
static void wipeMem(void *p, size_t n) {
  volatile unsigned char *q = (volatile unsigned char *)p;
  while (n--) *q++ = 0;
}

static void ctlConnReset(CtlConn &c) {
  wipeMem(&c, sizeof c);
  c.fd = -1;
}

// reads the token once, wipes the original environment value, and removes the variable before any
// hook can be forked. a malformed token is also scrubbed.
static bool ctlLoadToken() {
  gCtlTokenLen = 0;
  wipeMem(gCtlToken, sizeof gCtlToken);
  char *e = getenv("FLICK_TOKEN");
  if (!e) return false;
  size_t n = strlen(e);
  bool ok = n >= ctlTokenMin && n <= ctlTokenMax;
  for (size_t i = 0; ok && i < n; i++) {
    unsigned char ch = (unsigned char)e[i];
    if (ch < 0x21 || ch > 0x7e) ok = false;
  }
  if (ok) {
    memcpy(gCtlToken, e, n);
    gCtlToken[n] = 0;
    gCtlTokenLen = n;
  }
  wipeMem(e, n);
  unsetenv("FLICK_TOKEN");
  if (!ok) wipeMem(gCtlToken, sizeof gCtlToken);
  return ok;
}

// compares the candidate and the stored token without an early exit on content or length
static bool tokenEqual(const char *s, size_t n) {
  unsigned diff = (unsigned)(n ^ gCtlTokenLen);
  for (size_t i = 0; i < (size_t)ctlTokenMax; i++) {
    unsigned char a = i < n ? (unsigned char)s[i] : 0;
    unsigned char b = i < gCtlTokenLen ? (unsigned char)gCtlToken[i] : 0;
    diff |= (unsigned)(a ^ b);
  }
  return diff == 0;
}

static void ctlFdSetup(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int fdf = fcntl(fd, F_GETFD, 0);
  if (fdf >= 0) (void)fcntl(fd, F_SETFD, fdf | FD_CLOEXEC);
}

static void ctlClose(CtlConn &c) {
  if (c.fd >= 0) close(c.fd);
  ctlConnReset(c);
}

static bool ctlQueue(CtlConn &c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool ctlQueue(CtlConn &c, const char *fmt, ...) {
  if (c.fd < 0 || c.outLen >= sizeof c.out) return false;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(c.out + c.outLen, sizeof c.out - c.outLen, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof c.out - c.outLen) return false;
  c.outLen += (size_t)n;
  return true;
}

static void ctlReply(CtlConn &c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ctlReply(CtlConn &c, const char *fmt, ...) {
  if (c.fd < 0 || c.outLen >= sizeof c.out) { ctlClose(c); return; }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(c.out + c.outLen, sizeof c.out - c.outLen, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof c.out - c.outLen) { ctlClose(c); return; }
  c.outLen += (size_t)n;
}

// returns false only when the connection is broken, EAGAIN keeps the rest queued
static bool ctlFlush(CtlConn &c) {
  if (c.fd < 0) return false;
  size_t off = 0;
  while (off < c.outLen) {
    ssize_t w = write(c.fd, c.out + off, c.outLen - off);
    if (w > 0) { off += (size_t)w; continue; }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    return false;
  }
  if (off) {
    memmove(c.out, c.out + off, c.outLen - off);
    c.outLen -= off;
  }
  return true;
}

static const char *ctlStateName() {
  if (gSource >= 0) return "active";
  if (gPaused || !strncmp(gState, "error:", 6)) return "off";
  return "idle";
}

static void ctlPublishState() {
  const char *s = ctlStateName();
  int p = gPaused ? 1 : 0;
  if (!strcmp(s, gCtlEvtState) && p == gCtlEvtPaused) return;
  snprintf(gCtlEvtState, sizeof gCtlEvtState, "%s", s);
  gCtlEvtPaused = p;
  if (gCtlClient.fd >= 0 && gCtlClient.authed && gCtlClient.subscribed)
    if (!ctlQueue(gCtlClient, "EVT state=%s paused=%d\n", s, p)) ctlClose(gCtlClient);
}

static unsigned ctlMaskKey(const char *k) {
  if (!strcmp(k, "sens_h")) return cfgBitSensH;
  if (!strcmp(k, "sens_v")) return cfgBitSensV;
  if (!strcmp(k, "zone")) return cfgBitZone;
  if (!strcmp(k, "orient")) return cfgBitOrient;
  if (!strcmp(k, "smooth")) return cfgBitSmooth;
  if (!strcmp(k, "strength")) return cfgBitStrength;
  if (!strcmp(k, "resp")) return cfgBitResp;
  if (!strcmp(k, "curve")) return cfgBitCurve;
  if (!strcmp(k, "hw")) return cfgBitHw;
  if (!strcmp(k, "filter")) return cfgBitFilter;
  if (!strcmp(k, "pkgs")) return cfgBitPkgs;
  return 0;
}

// shortest fixed decimal that reads back as the same float, always with a decimal point
static void fmtFloat(char *b, size_t n, float f) {
  for (int p = 1; p <= 9; p++) {
    snprintf(b, n, "%.*f", p, (double)f);
    if (strtof(b, nullptr) == f) return;
  }
}

static bool fmtCfg(char *b, size_t n, unsigned bit, const Config &c) {
  char f[32];
  int r;
  switch (bit) {
    case cfgBitSensH: fmtFloat(f, sizeof f, c.sensH); r = snprintf(b, n, "sens_h=%s\n", f); break;
    case cfgBitSensV: fmtFloat(f, sizeof f, c.sensV); r = snprintf(b, n, "sens_v=%s\n", f); break;
    case cfgBitZone: r = snprintf(b, n, "zone=%d\n", c.zone); break;
    case cfgBitOrient: r = snprintf(b, n, "orient=%d\n", c.orient); break;
    case cfgBitSmooth: r = snprintf(b, n, "smooth=%d\n", c.smooth); break;
    case cfgBitStrength: r = snprintf(b, n, "strength=%d\n", c.strength); break;
    case cfgBitResp: r = snprintf(b, n, "resp=%d\n", c.resp); break;
    case cfgBitCurve: r = snprintf(b, n, "curve=%d\n", c.curve); break;
    case cfgBitHw: r = snprintf(b, n, "hw=%d\n", c.hw); break;
    case cfgBitFilter: r = snprintf(b, n, "filter=%d\n", c.filter); break;
    case cfgBitPkgs: r = snprintf(b, n, "pkgs=%s\n", c.pkgs); break;
    default: return false;
  }
  return r > 0 && (size_t)r < n;
}

// merges the pending keys into the existing file. unknown lines survive; a changed key appears
// exactly once. the temporary file is unique per pid and the rename is atomic.
static bool ctlWriteConfig() {
  if (!gCfg || !gCtlPendingMask) return true;
  char tmp[320];
  int tn = snprintf(tmp, sizeof tmp, "%s.flick.%d.tmp", gCfgPath, (int)getpid());
  if (tn < 0 || (size_t)tn >= sizeof tmp) return false;
  FILE *in = fopen(gCfgPath, "r");
  FILE *out = fopen(tmp, "w");
  if (!out) { if (in) fclose(in); return false; }
  bool ok = true;
  unsigned written = 0;
  char line[ctlLineMax + 2], cfgLine[ctlLineMax + 64];
  bool skip = false;
  while (in && fgets(line, sizeof line, in)) {
    size_t len = strlen(line);
    bool hasNl = len > 0 && line[len - 1] == '\n';
    if (skip) { if (hasNl) skip = false; continue; }
    if (!hasNl && len == sizeof line - 1) { skip = true; continue; }
    char *eq = strchr(line, '=');
    unsigned bit = 0;
    if (eq) {
      *eq = 0;
      bit = ctlMaskKey(line);
      *eq = '=';
    }
    if (bit && (gCtlPendingMask & bit)) {
      if (!(written & bit)) {
        if (!fmtCfg(cfgLine, sizeof cfgLine, bit, *gCfg)) { ok = false; break; }
        if (fputs(cfgLine, out) < 0) { ok = false; break; }
        written |= bit;
      }
      continue;
    }
    if (fputs(line, out) < 0) { ok = false; break; }
    if (len && line[len - 1] != '\n' && fputc('\n', out) < 0) { ok = false; break; }
  }
  for (unsigned bit = 1; ok && bit <= cfgBitPkgs; bit <<= 1)
    if ((gCtlPendingMask & bit) && !(written & bit)) {
      if (!fmtCfg(cfgLine, sizeof cfgLine, bit, *gCfg)) { ok = false; break; }
      if (fputs(cfgLine, out) < 0) { ok = false; break; }
    }
  if (in) fclose(in);
  if (ok && (ferror(out) || fflush(out) != 0)) ok = false;
  if (fclose(out) != 0) ok = false;
  if (!ok) { unlink(tmp); return false; }
  if (rename(tmp, gCfgPath) < 0) { unlink(tmp); return false; }
  return true;
}

static void ctlFlushConfig(int64_t now) {
  if (!gCtlPersistDueMs || now < gCtlPersistDueMs) return;
  if (ctlWriteConfig()) {
    gCtlPendingMask = 0;
    gCtlPersistDueMs = 0;
    gCtlLastPersistMs = now;
  } else {
    gCtlPersistDueMs = now + ctlPersistMs;
  }
}

static void ctlFlushConfigNow() {
  if (gCtlPendingMask) (void)ctlWriteConfig();
  gCtlPendingMask = 0;
  gCtlPersistDueMs = 0;
}

static void ctlSchedulePersist(unsigned mask, int64_t now) {
  gCtlPendingMask |= mask;
  if (gCtlPersistDueMs) return;
  if (now - gCtlLastPersistMs >= ctlPersistMs) {
    if (ctlWriteConfig()) {
      gCtlPendingMask = 0;
      gCtlLastPersistMs = now;
    } else {
      gCtlPersistDueMs = now + ctlPersistMs;
    }
  } else {
    gCtlPersistDueMs = gCtlLastPersistMs + ctlPersistMs;
  }
}

static int64_t ctlDeadline() {
  int64_t d = -1;
  if (gCtlPending.fd >= 0)
    d = gCtlPending.failed ? gCtlPending.closeAtMs : gCtlPending.acceptedMs + ctlAuthMs;
  if (gCtlPersistDueMs && (d < 0 || gCtlPersistDueMs < d)) d = gCtlPersistDueMs;
  return d;
}

static void ctlFailAuth(CtlConn &c, int64_t now) {
  if (c.failed) return;
  c.failed = true;
  wipeMem(c.in, sizeof c.in);
  c.inLen = 0;
  c.closeAtMs = now + ctlLingerMs;
  ctlReply(c, "ERR auth\n");
}

static bool ctlAccept(int64_t now) {
  bool fresh = false;
  if (gCtlListen < 0) return fresh;
  for (;;) {
    sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = accept(gCtlListen, reinterpret_cast<sockaddr *>(static_cast<void *>(&sa)), &sl);
    if (fd < 0) { if (errno == EINTR) continue; return fresh; }
    if (gCtlPending.fd >= 0 && !gCtlPending.failed) { close(fd); continue; }
    if (gCtlPending.fd >= 0) ctlClose(gCtlPending);
    ctlFdSetup(fd);
    ctlConnReset(gCtlPending);
    gCtlPending.fd = fd;
    gCtlPending.acceptedMs = now;
    fresh = true;
  }
}

// a new handshake replaces the old client only after the token is accepted
static bool ctlPromote(CtlConn &src) {
  if (gCtlClient.fd >= 0) ctlClose(gCtlClient);
  gCtlClient = src;
  gCtlClient.authed = true;
  gCtlClient.failed = false;
  gCtlClient.closeAtMs = 0;
  gCtlClient.stopWhenDrained = false;
  gCtlClient.subscribed = false;
  ctlConnReset(gCtlPending);
  if (!ctlQueue(gCtlClient, "OK flick=%s proto=1\n", FLICK_VERSION)) {
    ctlClose(gCtlClient);
    return false;
  }
  return true;
}

static void ctlCmdSet(CtlConn &c, char *args, int64_t now) {
  if (!gCfg) { ctlReply(c, "ERR cmd\n"); return; }
  Config next = *gCfg;
  unsigned mask = 0;
  bool any = false;
  char *p = args;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    char *tok = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    char *eq = strchr(tok, '=');
    if (!eq || eq == tok) { ctlReply(c, "ERR cmd\n"); return; }
    *eq = 0;
    const char *k = tok;
    const char *v = eq + 1;
    size_t kl = strlen(k), vl = strlen(v);
    if (kl > ctlKeyMax) { ctlReply(c, "ERR cmd\n"); return; }
    if (vl > ctlValueMax) { ctlReply(c, "ERR %s length\n", k); return; }
    const char *why = "invalid";
    ApplyResult r = applySetting(k, v, next, true, &why);
    if (r == applyUnknown) { ctlReply(c, "ERR %s unknown\n", k); return; }
    if (r != applyOk) { ctlReply(c, "ERR %s %s\n", k, why); return; }
    mask |= ctlMaskKey(k);
    any = true;
  }
  if (!any) { ctlReply(c, "ERR cmd\n"); return; }
  bool pkgsChanged = strcmp(next.pkgs, gCfg->pkgs) != 0;
  deriveFilter(next);
  *gCfg = next;
  if (pkgsChanged) setPkgs(gCfg->pkgs);
  hwSync(*gCfg);
  gTickNow = true;
  ctlSchedulePersist(mask, now);
  ctlReply(c, "OK\n");
}

static void ctlCmdGet(CtlConn &c) {
  if (!gCfg) { ctlReply(c, "ERR cmd\n"); return; }
  char h[32], v[32];
  fmtFloat(h, sizeof h, gCfg->sensH);
  fmtFloat(v, sizeof v, gCfg->sensV);
  ctlReply(c,
           "CFG sens_h=%s sens_v=%s zone=%d orient=%d smooth=%d strength=%d resp=%d curve=%d hw=%d "
           "filter=%d pkgs=%s\n",
           h, v, gCfg->zone, gCfg->orient, gCfg->smooth, gCfg->strength, gCfg->resp, gCfg->curve,
           gCfg->hw, gCfg->filter, gCfg->pkgs);
}

// returns true when a successful HELLO moved this connection into the authenticated slot
static bool ctlHandleLine(CtlConn &c, int64_t now) {
  char *line = c.in;
  if (!c.authed) {
    if (!line[0]) {
      ctlReply(c, "ERR cmd\n");
      return false;
    }
    bool ok = !strncmp(line, "HELLO ", 6) && tokenEqual(line + 6, strlen(line + 6));
    if (!ok) {
      ctlFailAuth(c, now);
      wipeMem(c.in, sizeof c.in);
      return false;
    }
    wipeMem(c.in, sizeof c.in);
    c.inLen = 0;
    return ctlPromote(c);
  }
  if (!strcmp(line, "PING")) ctlReply(c, "PONG\n");
  else if (!strcmp(line, "GET")) ctlCmdGet(c);
  else if (!strncmp(line, "SET ", 4)) ctlCmdSet(c, line + 4, now);
  else if (!strcmp(line, "STATUS")) {
    int64_t up = (now - gCtlStartMs) / 1000;
    if (up < 0) up = 0;
    ctlReply(c, "ST state=%s paused=%d up=%lld pid=%d\n", ctlStateName(), gPaused ? 1 : 0,
             (long long)up, (int)getpid());
  } else if (!strcmp(line, "PAUSE")) {
    gPaused = true;
    gTickNow = true;
    ctlReply(c, "OK\n");
    ctlPublishState();
  } else if (!strcmp(line, "RESUME")) {
    gPaused = false;
    gTickNow = true;
    ctlReply(c, "OK\n");
    ctlPublishState();
  } else if (!strcmp(line, "SUB")) {
    c.subscribed = true;
    ctlReply(c, "OK\n");
  } else if (!strcmp(line, "UNSUB")) {
    c.subscribed = false;
    ctlReply(c, "OK\n");
  } else if (!strcmp(line, "STATS")) {
    ctlReply(c, "STATS grabs=%llu releases=%llu syn_dropped=%llu scans=%llu\n", gStatGrabs,
             gStatReleases, gStatSynDropped, gProcScans);
  } else if (!strcmp(line, "STOP")) {
    ctlReply(c, "OK\n");
    c.stopWhenDrained = true;
    c.subscribed = false;
  } else {
    ctlReply(c, "ERR cmd\n");
  }
  return false;
}

static void ctlRead(CtlConn &c, int64_t now) {
  if (c.fd < 0 || c.failed || c.stopWhenDrained) return;
  char buf[ctlChunk];
  CtlConn *cur = &c;
  for (;;) {
    ssize_t n = read(cur->fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      ctlClose(*cur);
      return;
    }
    if (n == 0) {
      cur->peerClosed = true;
      return;
    }
    for (ssize_t i = 0; i < n; i++) {
      char ch = buf[i];
      if (ch == '\n') {
        if (cur->inLen && cur->in[cur->inLen - 1] == '\r') cur->inLen--;
        cur->in[cur->inLen] = 0;
        if (memchr(cur->in, 0, cur->inLen)) { wipeMem(buf, (size_t)n); ctlClose(*cur); return; }
        bool promoted = ctlHandleLine(*cur, now);
        if (promoted) cur = &gCtlClient;
        cur->inLen = 0;
        if (cur->fd < 0 || cur->failed || cur->stopWhenDrained) { wipeMem(buf, (size_t)n); return; }
      } else {
        if (cur->inLen >= (size_t)ctlLineMax) { wipeMem(buf, (size_t)n); ctlClose(*cur); return; }
        cur->in[cur->inLen++] = ch;
      }
    }
    wipeMem(buf, (size_t)n);
  }
}

static void ctlConnService(CtlConn &c, short ev, int64_t now) {
  if (c.fd < 0) return;
  bool stop = c.stopWhenDrained;
  if (!ctlFlush(c) || (ev & (POLLERR | POLLNVAL))) {
    ctlClose(c);
    if (stop) gStop = 1;
    return;
  }
  if (ev & (POLLIN | POLLHUP)) ctlRead(c, now);
  stop = c.stopWhenDrained;
  if (c.fd < 0) {
    if (stop) gStop = 1;
    return;
  }
  if (!ctlFlush(c)) {
    ctlClose(c);
    if (stop) gStop = 1;
    return;
  }
  if (c.peerClosed && c.outLen == 0) {
    ctlClose(c);
    if (stop) gStop = 1;
    return;
  }
  if (stop && c.outLen == 0) {
    ctlClose(c);
    gStop = 1;
  }
}

// called after poll with the readiness of the three control descriptors
static void ctlService(int64_t now, short listenEv, short pendingEv, short clientEv) {
  ctlFlushConfig(now);
  int pendingFd = gCtlPending.fd;
  int clientFd = gCtlClient.fd;
  bool fresh = false;
  if (gCtlListen >= 0 && (listenEv & POLLIN)) fresh = ctlAccept(now);
  if (gCtlPending.fd >= 0) {
    if (!gCtlPending.authed && !gCtlPending.failed &&
        now - gCtlPending.acceptedMs >= ctlAuthMs)
      ctlFailAuth(gCtlPending, now);
    ctlConnService(gCtlPending, !fresh && pendingFd == gCtlPending.fd ? pendingEv : 0, now);
    if (gCtlPending.fd >= 0 && gCtlPending.failed && now >= gCtlPending.closeAtMs)
      ctlClose(gCtlPending);
  }
  if (gCtlClient.fd >= 0)
    ctlConnService(gCtlClient, clientFd == gCtlClient.fd ? clientEv : 0, now);
}

// binds only the loopback address. port 0 is accepted for the host test, which learns the real
// port from getsockname.
static int ctlStart(unsigned port) {
  if (gCtlListen < 0) {
    ctlConnReset(gCtlPending);
    ctlConnReset(gCtlClient);
  }
  if (gCtlTokenLen < ctlTokenMin) { errno = EACCES; return -1; }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr *>(static_cast<void *>(&sa)), sizeof sa) < 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  if (listen(fd, 4) < 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  ctlFdSetup(fd);
  socklen_t sl = sizeof sa;
  if (getsockname(fd, reinterpret_cast<sockaddr *>(static_cast<void *>(&sa)), &sl) < 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  gCtlPort = ntohs(sa.sin_port);
  if (!gCtlPort) {
    close(fd);
    errno = EADDRNOTAVAIL;
    return -1;
  }
  gCtlListen = fd;
  if (!gCtlStartMs) gCtlStartMs = nowMs();
  return 0;
}

static void ctlShutdown() {
  ctlClose(gCtlPending);
  ctlClose(gCtlClient);
  if (gCtlListen >= 0) { close(gCtlListen); gCtlListen = -1; }
  wipeMem(gCtlToken, sizeof gCtlToken);
  gCtlTokenLen = 0;
}

int main(int argc, char **argv) {
  ctlConnReset(gCtlPending);
  ctlConnReset(gCtlClient);
  const char *cfgPath = "config", *pidPath = "flick.pid", *dev = nullptr;
  long listenPort = -1;
  bool detach = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--daemon")) detach = true;
    else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
    else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) pidPath = argv[++i];
    else if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
    else if (!strcmp(argv[i], "--listen")) {
      if (i + 1 >= argc) { say(2, "ERR listen port\n"); return 2; }
      char *end = nullptr;
      long p = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end || p < 1 || p > 65535) { say(2, "ERR listen port\n"); return 2; }
      listenPort = p;
    }
  }
  if (running(pidPath)) { say(2, "ERR already running\n"); return 1; }

  if (listenPort >= 0) {
    gCtlEnabled = true;
    if (!ctlLoadToken()) { say(2, "ERR FLICK_TOKEN invalid\n"); return 2; }
    if (ctlStart((unsigned)listenPort) < 0) {
      say(2, "ERR listen errno=%d\n", errno);
      ctlShutdown();
      return 3;
    }
  }

  char base[128];
  splitPath(cfgPath, base, sizeof base);
  Config cfg;
  loadConfig(cfgPath, cfg);
  gCfgPath = cfgPath;
  gCfg = &cfg;

  int src;
  if (dev) { snprintf(gPath, sizeof gPath, "%s", dev); src = open(gPath, O_RDONLY | O_CLOEXEC); }
  else src = findTouch(gPath, sizeof gPath);
  if (src < 0) { say(2, "ERR touchscreen not found errno=%d\n", errno); ctlShutdown(); return 2; }
  gUinput = cloneDev(src);
  close(src);
  if (gUinput < 0) { ctlShutdown(); return 3; }
  const input_absinfo &ax = gAbs[ABS_MT_POSITION_X], &ay = gAbs[ABS_MT_POSITION_Y];
  int64_t rx = (int64_t)ax.maximum - ax.minimum, ry = (int64_t)ay.maximum - ay.minimum;
  int64_t ref = rx < ry ? rx : ry;
  gVertX = rx < ry;
  if (ref > 0) gInvShort = 1.0f / (float)ref;
  if (rx > 0 || ry > 0) gLongSide = (float)(rx > ry ? rx : ry);

  int procs = readPids(gCur, maxPid);
  if (cfg.filter && procs < 20) {
    say(2, "ERR /proc not visible (pids=%d), set filter=0 to bypass\n", procs);
    ioctl(gUinput, UI_DEV_DESTROY);
    ctlShutdown();
    return 6;
  }
  usleep(300000);

  if (detach) {
    int pp[2];
    if (pipe2(pp, O_CLOEXEC) < 0) { say(2, "ERR pipe errno=%d\n", errno); ctlShutdown(); return 5; }
    if (fork() > 0) {
      close(pp[1]);
      char b = 0;
      ssize_t n;
      do { n = read(pp[0], &b, 1); } while (n < 0 && errno == EINTR);
      if (n == 1) { okLine(procs, cfg); _exit(0); }
      say(2, "ERR daemon failed to start\n");
      _exit(5);
    }
    setsid();
    if (fork() > 0) _exit(0);
    close(pp[0]);
    int z = open("/dev/null", O_RDWR);
    if (z >= 0) { dup2(z, 0); dup2(z, 1); dup2(z, 2); if (z > 2) close(z); }
    writePid(pidPath);
    setState("idle");
    (void)!write(pp[1], "1", 1);
    close(pp[1]);
  } else {
    writePid(pidPath);
    setState("idle");
    okLine(procs, cfg);
  }

  // best effort: some shell configurations deny a negative nice value. correctness does not depend
  // on it, the event loop only sleeps in poll and never busy-waits.
  (void)setpriority(PRIO_PROCESS, 0, -8);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = onSignal;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  signal(SIGHUP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (ifd >= 0 && inotify_add_watch(ifd, gDir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) { close(ifd); ifd = -1; }
  struct stat st;
  long ms = 0, mn = 0;
  if (!stat(cfgPath, &st)) { ms = st.st_mtim.tv_sec; mn = st.st_mtim.tv_nsec; }

  bool want = false;
  int64_t next = 0;
  gCtlStartMs = nowMs();
  pollfd pf[5];
  while (!gStop) {
    if (gTickNow) { gTickNow = false; next = 0; }
    int64_t now = nowMs();
    if (now >= next) {
      next = now + tickMs;
      if (ifd < 0 && !stat(cfgPath, &st) && (st.st_mtim.tv_sec != ms || st.st_mtim.tv_nsec != mn)) {
        ms = st.st_mtim.tv_sec; mn = st.st_mtim.tv_nsec;
        loadConfig(cfgPath, cfg);
        hwSync(cfg);
      }
      want = !gPaused && (!cfg.filter || inGame());
      if (gSource < 0) {
        if (!want) {
          if (gOn) onChange(false, cfg);
          char e[64];
          snprintf(e, sizeof e, "error:adj errno=%d", gAdjErr);
          setState(gAdjErr ? e : "idle");
        } else {
          int r = activate();
          if (r == 0) onChange(true, cfg);
          else if (r > 0) next = now + retryMs;
        }
      }
    }
    if (gSource >= 0 && !want && gDown == 0) {
      deactivate();
      onChange(false, cfg);
    }

    int64_t w = next - now;
    if (w < 0) w = 0;
    if (w > tickMs) w = tickMs;
    int64_t deadline = ctlDeadline();
    if (deadline >= 0) {
      int64_t d = deadline - now;
      if (d < 0) d = 0;
      if (d < w) w = d;
    }
    pf[0].fd = gSource;         pf[0].events = POLLIN; pf[0].revents = 0;
    pf[1].fd = ifd;             pf[1].events = POLLIN; pf[1].revents = 0;
    pf[2].fd = gCtlListen;      pf[2].events = POLLIN; pf[2].revents = 0;
    pf[3].fd = gCtlPending.fd;  pf[3].events = POLLIN; pf[3].revents = 0;
    pf[4].fd = gCtlClient.fd;   pf[4].events = POLLIN; pf[4].revents = 0;
    if (gCtlPending.outLen) pf[3].events |= POLLOUT;
    if (gCtlClient.outLen) pf[4].events |= POLLOUT;
    int r = poll(pf, 5, (int)w);
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (r == 0) { ctlService(nowMs(), 0, 0, 0); continue; }

    int64_t after = nowMs();
    ctlService(after, pf[2].revents, pf[3].revents, pf[4].revents);
    if (gStop) break;

    if ((pf[1].revents & POLLIN) && cfgChanged(ifd, base)) { loadConfig(cfgPath, cfg); hwSync(cfg); next = 0; }
    if (gSource >= 0) {
      if (pf[0].revents & POLLIN) {
        int pr = pump(cfg);
        if (pr < 0) break;
        if (pr == 0) { deactivate(); next = nowMs() + retryMs; continue; }
      }
      if (pf[0].revents & ~POLLIN) break;
    }
  }
  ctlFlushConfigNow();
  ctlShutdown();
  if (gSource >= 0) deactivate();
  gOn = false;
  hwSync(cfg);
  hwWait();
  ioctl(gUinput, UI_DEV_DESTROY);
  unlink(pidPath);
  char sp[300];
  snprintf(sp, sizeof sp, "%s/flick.state", gDir);
  unlink(sp);
  return 0;
}
