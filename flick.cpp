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
#include <math.h>
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

constexpr int maxSlot = 32;
constexpr int maxPid = 8192;
constexpr int tickMs = 3000;
constexpr int retryMs = 250;
constexpr int pkgBuf = 1024;
constexpr int maxPkgs = 16;
constexpr int pkgLen = 128;
constexpr int batchSize = 256;
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

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// strength maps to the min cutoff, 20 hz for light down to 0.5 hz for heavy. resp maps to beta, 0.5
// to 60 hz per panel width per second, which is how fast the filter opens on fast swipes.
static void deriveFilter(Config &c) {
  float s = (float)(c.strength - 1) / 99.0f, r = (float)(c.resp - 1) / 99.0f;
  c.minCut = 20.0f * powf(0.025f, s);
  c.beta = 0.5f * powf(120.0f, r);
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
  if (!strcmp(k, "pkgs")) { snprintf(c.pkgs, sizeof c.pkgs, "%s", v); return; }
  if (*v < '0' || *v > '9') return;
  if (!strcmp(k, "sens_h")) c.sensH = clampf((float)atof(v), 1.0f, 5.0f);
  else if (!strcmp(k, "sens_v")) c.sensV = clampf((float)atof(v), 1.0f, 5.0f);
  else if (!strcmp(k, "zone")) c.zone = clampi(atoi(v), 0, 2);
  else if (!strcmp(k, "orient")) c.orient = atoi(v) == 3 ? 3 : 1;
  else if (!strcmp(k, "smooth")) c.smooth = atoi(v) != 0;
  else if (!strcmp(k, "strength")) c.strength = clampi(atoi(v), 1, 100);
  else if (!strcmp(k, "resp")) c.resp = clampi(atoi(v), 1, 100);
  else if (!strcmp(k, "curve")) c.curve = clampi(atoi(v), 0, 2);
  else if (!strcmp(k, "hw")) c.hw = atoi(v) != 0;
  else if (!strcmp(k, "filter")) c.filter = atoi(v) != 0;
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

static void setState(const char *s) {
  if (!strcmp(s, gState)) return;
  snprintf(gState, sizeof gState, "%s", s);
  char p[300];
  snprintf(p, sizeof p, "%s/flick.state", gDir);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
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
#ifdef FLICK_PROC_TEST
static unsigned gProcScans = 0;
#endif

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
      const DirEntry *e = (const DirEntry *)(buf + off);
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
#ifdef FLICK_PROC_TEST
  gProcScans++;
#endif
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
  if (ioctl(fd, EVIOCGRAB, 1) < 0) {
    snprintf(e, sizeof e, "error:grab errno=%d", errno);
    setState(e);
    close(fd);
    return -1;
  }
  // the check and the grab are not atomic. re-check after the grab and release at once when a finger
  // landed in the window, so the daemon never holds the panel while a touch is down.
  t = touching(fd);
  if (t != 0) {
    ioctl(fd, EVIOCGRAB, 0);
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
  ioctl(gSource, EVIOCGRAB, 0);
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
  runHook("notify.sh", on ? "active" : "idle");
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
    inotify_event *ev = (inotify_event *)q;
    if (ev->len && !strcmp(ev->name, base)) hit = true;
    q += sizeof(inotify_event) + ev->len;
  }
  return hit;
}

int main(int argc, char **argv) {
  const char *cfgPath = "config", *pidPath = "flick.pid", *dev = nullptr;
  bool detach = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--daemon")) detach = true;
    else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
    else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) pidPath = argv[++i];
    else if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
  }
  if (running(pidPath)) { say(2, "ERR already running\n"); return 1; }

  char base[128];
  splitPath(cfgPath, base, sizeof base);
  Config cfg;
  loadConfig(cfgPath, cfg);

  int src;
  if (dev) { snprintf(gPath, sizeof gPath, "%s", dev); src = open(gPath, O_RDONLY | O_CLOEXEC); }
  else src = findTouch(gPath, sizeof gPath);
  if (src < 0) { say(2, "ERR touchscreen not found errno=%d\n", errno); return 2; }
  gUinput = cloneDev(src);
  close(src);
  if (gUinput < 0) return 3;
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
    return 6;
  }
  usleep(300000);

  if (detach) {
    int pp[2];
    if (pipe2(pp, O_CLOEXEC) < 0) { say(2, "ERR pipe errno=%d\n", errno); return 5; }
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
  pollfd pf[2];
  while (!gStop) {
    int64_t now = nowMs();
    if (now >= next) {
      next = now + tickMs;
      if (ifd < 0 && !stat(cfgPath, &st) && (st.st_mtim.tv_sec != ms || st.st_mtim.tv_nsec != mn)) {
        ms = st.st_mtim.tv_sec; mn = st.st_mtim.tv_nsec;
        loadConfig(cfgPath, cfg);
        hwSync(cfg);
      }
      want = !cfg.filter || inGame();
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
    pf[0].fd = gSource; pf[0].events = POLLIN; pf[0].revents = 0;
    pf[1].fd = ifd;     pf[1].events = POLLIN; pf[1].revents = 0;
    int r = poll(pf, 2, (int)(w < 0 ? 0 : (w > tickMs ? tickMs : w)));
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (r == 0) continue;

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
