// host test for the optional control server. it compiles flick.cpp with main renamed, binds a real
// loopback socket on port 0, and drives ctlService() directly so every deadline, reply, and config
// write can be checked without a phone, a touchscreen, or a sleep of several seconds.
//
//   g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
//       ctrl_test.cpp -o ctrl-test -lm
//
// the token below is synthetic and only exists in this test file.
#define main flickMain
#include "flick.cpp"
#undef main

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static int gFails = 0;

static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) gFails++;
}

static std::string readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

static bool writeFile(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), (std::streamsize)data.size());
  return f.good();
}

static size_t countOf(const std::string &hay, const std::string &needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) n++;
  return n;
}

static bool sendAll(int fd, const std::string &s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = write(fd, s.data() + off, s.size() - off);
    if (n > 0) { off += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd p;
      p.fd = fd;
      p.events = POLLOUT;
      p.revents = 0;
      if (poll(&p, 1, 500) > 0) continue;
    }
    return false;
  }
  return true;
}

static int connectClient() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)gCtlPort);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1 ||
      connect(fd, reinterpret_cast<sockaddr *>(static_cast<void *>(&sa)), sizeof sa) < 0) {
    close(fd);
    return -1;
  }
  ctlFdSetup(fd);
  return fd;
}

// runs the server exactly like the daemon does after poll. the event bits are harmless when a slot
// is empty or a socket has nothing pending.
static void serviceAt(int64_t now) { ctlService(now, POLLIN, POLLIN, POLLIN); }

// reads one LF line from a non-blocking client. the server has already flushed it before this call.
static bool readLine(int fd, std::string &out) {
  out.clear();
  for (;;) {
    char ch;
    ssize_t n = read(fd, &ch, 1);
    if (n == 1) {
      if (ch == '\n') return true;
      if (out.size() >= (size_t)4096) return false;
      out += ch;
      continue;
    }
    if (n == 0) return false;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      pollfd p;
      p.fd = fd;
      p.events = POLLIN;
      p.revents = 0;
      if (poll(&p, 1, 500) > 0) continue;
      return false;
    }
    return false;
  }
}

static void expectLine(int fd, const std::string &want, const char *what) {
  std::string got;
  bool ok = readLine(fd, got) && got == want;
  if (!ok) printf("  got=[%s] want=[%s]\n", got.c_str(), want.c_str());
  check(ok, what);
}

static void expectNoLine(int fd, const char *what) {
  pollfd p;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  int r = poll(&p, 1, 60);
  bool ok = r == 0;
  if (r > 0) {
    char b[128];
    ssize_t n = read(fd, b, sizeof b);
    if (n > 0) printf("  unexpected [%.*s]\n", (int)n, b);
    else ok = false;
  }
  check(ok, what);
}

static void expectClosed(int fd, const char *what) {
  int64_t end = nowMs() + 1000;
  while (nowMs() < end) {
    char b[128];
    ssize_t n = read(fd, b, sizeof b);
    if (n == 0) { check(true, what); return; }
    if (n < 0 && (errno == ECONNRESET || errno == EPIPE)) { check(true, what); return; }
    if (n < 0 && errno == EINTR) continue;
    if (n > 0) continue;
    pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    (void)poll(&p, 1, 10);
  }
  check(false, what);
}

int main() {
  const std::string token = "test-only-token-0123456789-abcdefghij";
  const std::string shortToken(31, 'a');
  std::string badToken = token;
  badToken[5] = ' ';
  const std::string longToken(257, 'a');

  unsetenv("FLICK_TOKEN");
  check(!ctlLoadToken() && gCtlTokenLen == 0, "missing token is rejected");

  setenv("FLICK_TOKEN", shortToken.c_str(), 1);
  check(!ctlLoadToken(), "31-byte token is rejected");
  check(getenv("FLICK_TOKEN") == nullptr, "short token is removed from the environment");

  setenv("FLICK_TOKEN", badToken.c_str(), 1);
  check(!ctlLoadToken(), "token with a space is rejected");
  check(getenv("FLICK_TOKEN") == nullptr, "invalid token is removed from the environment");

  setenv("FLICK_TOKEN", longToken.c_str(), 1);
  check(!ctlLoadToken(), "257-byte token is rejected");

  setenv("FLICK_TOKEN", token.c_str(), 1);
  check(ctlLoadToken(), "valid token is accepted");
  check(gCtlTokenLen == token.size(), "token length is retained");
  check(getenv("FLICK_TOKEN") == nullptr, "valid token is removed from the environment");
  check(tokenEqual(token.c_str(), token.size()), "stored token compares equal");
  check(!tokenEqual("x", 1), "wrong token compares unequal");

  check(ctlStart(0) == 0, "listener binds an ephemeral loopback port");
  check(gCtlListen >= 0 && gCtlPort != 0, "listener and assigned port are published");
  sockaddr_in sa;
  socklen_t sl = sizeof sa;
  check(getsockname(gCtlListen, reinterpret_cast<sockaddr *>(static_cast<void *>(&sa)), &sl) == 0 &&
            ntohl(sa.sin_addr.s_addr) == INADDR_LOOPBACK,
        "listener is bound to 127.0.0.1 only");
  int fl = fcntl(gCtlListen, F_GETFL, 0);
  check(fl >= 0 && (fl & O_NONBLOCK), "listener is non-blocking");

  int listenBefore = gCtlListen;
  errno = 0;
  check(ctlStart(gCtlPort) < 0 && errno == EADDRINUSE && gCtlListen == listenBefore,
        "second bind on the same port fails with EADDRINUSE");

  std::error_code ec;
  std::string dir = (fs::temp_directory_path() / ("flick-ctrl-" + std::to_string((long)getpid()))).string();
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) {
    printf("FAIL cannot create %s: %s\nSELFTEST CTRL FAIL\n", dir.c_str(), ec.message().c_str());
    return 1;
  }
  snprintf(gDir, sizeof gDir, "%s", dir.c_str());
  std::string cfgPath = dir + "/config";
  std::string initial =
      "sens_h=2.0\n"
      "# keep this comment\n"
      "zone=1\n"
      "unknown_key=keep\n"
      "pkgs=com.dta\n"
      "filter=1\n";
  check(writeFile(cfgPath, initial), "test config is writable");

  Config cfg;
  loadConfig(cfgPath.c_str(), cfg);
  gCfgPath = cfgPath.c_str();
  gCfg = &cfg;
  gCtlEnabled = true;
  gCtlLastPersistMs = -ctlPersistMs;
  gCtlPersistDueMs = 0;
  gCtlPendingMask = 0;
  gCtlEvtState[0] = 0;
  gCtlEvtPaused = -1;

  int64_t T = nowMs();
  gCtlStartMs = T;

  int a = connectClient();
  check(a >= 0, "first test client connects");
  serviceAt(T + 10);
  check(gCtlPending.fd >= 0, "first connection occupies the pending slot");
  check(sendAll(a, "HELLO " + token + "\n"), "HELLO is sent");
  serviceAt(T + 11);
  std::string hello = std::string("OK flick=") + FLICK_VERSION + " proto=1";
  expectLine(a, hello, "correct HELLO returns the version and protocol");
  check(gCtlClient.fd >= 0 && gCtlPending.fd < 0, "successful HELLO promotes the client");

  int b = connectClient();
  check(b >= 0, "second test client connects");
  serviceAt(T + 100);
  check(gCtlPending.fd >= 0, "second connection uses the pending slot");
  check(sendAll(b, "PING\n"), "pre-HELLO command is sent");
  serviceAt(T + 101);
  expectLine(b, "ERR auth", "command before HELLO returns ERR auth");
  check(gCtlPending.failed, "pre-HELLO failure starts the linger");
  check(sendAll(a, "PING\n"), "authenticated client still sends");
  serviceAt(T + 102);
  expectLine(a, "PONG", "failed handshake does not replace the authenticated client");
  serviceAt(T + 401);
  check(gCtlPending.fd < 0, "failed handshake closes after 300 ms");
  expectClosed(b, "pre-HELLO peer sees the close");
  close(b);

  b = connectClient();
  check(b >= 0, "wrong-token client connects");
  serviceAt(T + 500);
  check(gCtlPending.fd >= 0, "wrong-token connection is pending");
  check(sendAll(b, "HELLO wrong-token-value-0123456789-abcdef\n"), "wrong token is sent");
  serviceAt(T + 501);
  expectLine(b, "ERR auth", "wrong token returns ERR auth");
  serviceAt(T + 800);
  check(gCtlPending.fd >= 0, "wrong-token connection still lingers at 299 ms");
  serviceAt(T + 801);
  check(gCtlPending.fd < 0, "wrong-token connection closes at 300 ms");
  expectClosed(b, "wrong-token peer sees the close");
  close(b);

  b = connectClient();
  check(b >= 0, "silent client connects");
  serviceAt(T + 1000);
  check(gCtlPending.fd >= 0, "silent connection is pending");
  int64_t accepted = gCtlPending.acceptedMs;
  serviceAt(accepted + ctlAuthMs - 1);
  check(gCtlPending.fd >= 0, "silent connection survives just before the auth deadline");
  expectNoLine(b, "no auth reply before the deadline");
  serviceAt(accepted + ctlAuthMs);
  expectLine(b, "ERR auth", "auth timeout returns ERR auth");
  serviceAt(accepted + ctlAuthMs + ctlLingerMs);
  check(gCtlPending.fd < 0, "timed-out handshake closes after the linger");
  expectClosed(b, "timed-out peer sees the close");
  close(b);

  check(sendAll(a, "PING\n"), "PING is sent");
  serviceAt(T + 3500);
  expectLine(a, "PONG", "PING returns PONG");
  check(sendAll(a, "BOGUS\n"), "unknown command is sent");
  serviceAt(T + 3501);
  expectLine(a, "ERR cmd", "unknown command returns ERR cmd");
  check(sendAll(a, "\n"), "empty line is sent");
  serviceAt(T + 3502);
  expectLine(a, "ERR cmd", "empty line returns ERR cmd");
  check(sendAll(a, "\r\n"), "CR-only line is sent");
  serviceAt(T + 3503);
  expectLine(a, "ERR cmd", "trailing CR is tolerated and stripped");

  int64_t s = T + 4000;
  std::string setAll =
      "SET sens_h=2.5 sens_v=3.25 zone=2 orient=3 smooth=1 strength=60 resp=70 curve=1 hw=1 "
      "filter=0 pkgs=com.example.one,com.example.two\n";
  check(sendAll(a, setAll), "full SET is sent");
  serviceAt(s);
  expectLine(a, "OK", "valid multi-key SET applies");
  check(cfg.sensH == 2.5f && cfg.sensV == 3.25f, "SET updates both sensitivities");
  check(cfg.zone == 2 && cfg.orient == 3 && cfg.smooth == 1, "SET updates zone/orient/smooth");
  check(cfg.strength == 60 && cfg.resp == 70 && cfg.curve == 1, "SET updates filter curves");
  check(cfg.hw == 1 && cfg.filter == 0, "SET updates hw/filter");
  check(!strcmp(cfg.pkgs, "com.example.one,com.example.two"), "SET updates the package list");
  check(gCtlLastPersistMs == s, "first SET writes immediately");
  std::string file = readFile(cfgPath);
  check(file.find("sens_h=2.5\n") != std::string::npos, "persisted sensitivity is effective");
  check(file.find("sens_v=3.25\n") != std::string::npos, "persisted second sensitivity is effective");
  check(file.find("zone=2\n") != std::string::npos, "persisted zone is effective");
  check(file.find("orient=3\n") != std::string::npos, "persisted orient is effective");
  check(file.find("smooth=1\n") != std::string::npos, "persisted smooth is effective");
  check(file.find("strength=60\n") != std::string::npos, "persisted strength is effective");
  check(file.find("resp=70\n") != std::string::npos, "persisted resp is effective");
  check(file.find("curve=1\n") != std::string::npos, "persisted curve is effective");
  check(file.find("hw=1\n") != std::string::npos, "persisted hw is effective");
  check(file.find("filter=0\n") != std::string::npos, "persisted filter is effective");
  check(file.find("pkgs=com.example.one,com.example.two\n") != std::string::npos,
        "persisted packages are effective");
  check(file.find("# keep this comment\n") != std::string::npos, "unknown comment line survives");
  check(file.find("unknown_key=keep\n") != std::string::npos, "unknown key line survives");
  check(countOf(file, "sens_h=") == 1u && countOf(file, "zone=") == 1u, "changed keys appear once");
  check(!fs::exists(cfgPath + ".flick." + std::to_string((long)getpid()) + ".tmp"),
        "temporary config file is removed");

  check(sendAll(a, "GET\n"), "GET is sent");
  serviceAt(s + 1);
  expectLine(a,
             "CFG sens_h=2.5 sens_v=3.25 zone=2 orient=3 smooth=1 strength=60 resp=70 curve=1 hw=1 "
             "filter=0 pkgs=com.example.one,com.example.two",
             "GET returns the effective in-memory values");

  std::string beforeFile = file;
  check(sendAll(a, "SET sens_h=4.0 strength=999\n"), "invalid multi-pair SET is sent");
  serviceAt(s + 10);
  expectLine(a, "ERR strength range", "out-of-range SET names the failing key and reason");
  check(cfg.sensH == 2.5f && cfg.strength == 60, "invalid SET leaves memory unchanged");
  check(readFile(cfgPath) == beforeFile, "invalid SET leaves the config file unchanged");
  check(gCtlPendingMask == 0, "invalid SET queues no write");

  check(sendAll(a, "SET sens_h=4.0 zone=9\n"), "partially valid SET is sent");
  serviceAt(s + 11);
  expectLine(a, "ERR zone range", "partial SET is rejected on the failing pair");
  check(cfg.sensH == 2.5f && cfg.zone == 2, "partial SET applies nothing");

  check(sendAll(a, "SET nope=1\n"), "unknown key SET is sent");
  serviceAt(s + 12);
  expectLine(a, "ERR nope unknown", "unknown SET key is reported");
  check(readFile(cfgPath) == beforeFile, "unknown SET key leaves the file unchanged");

  check(sendAll(a, "SET sens_h=4.0 junk\n"), "malformed SET is sent");
  serviceAt(s + 13);
  expectLine(a, "ERR cmd", "malformed pair returns ERR cmd");
  check(cfg.sensH == 2.5f && readFile(cfgPath) == beforeFile, "malformed SET applies nothing");

  check(sendAll(a, "SET sens_h=2.75\n"), "first coalesced SET is sent");
  serviceAt(s + 100);
  expectLine(a, "OK", "first coalesced SET applies");
  check(readFile(cfgPath) == beforeFile, "first coalesced SET waits for the window");
  check(gCtlLastPersistMs == s, "last write time is unchanged inside the window");
  check(gCtlPendingMask == cfgBitSensH, "only sens_h is queued");
  check(sendAll(a, "SET curve=2\n"), "second coalesced SET is sent");
  serviceAt(s + 200);
  expectLine(a, "OK", "second coalesced SET applies");
  check(readFile(cfgPath) == beforeFile, "second coalesced SET waits for the same window");
  check(gCtlPendingMask == (cfgBitSensH | cfgBitCurve), "both keys are queued together");
  serviceAt(s + 500);
  check(gCtlLastPersistMs == s + 500, "coalesced write starts at the 500 ms boundary");
  file = readFile(cfgPath);
  check(file.find("sens_h=2.75\n") != std::string::npos, "coalesced sens_h is persisted");
  check(file.find("curve=2\n") != std::string::npos, "coalesced curve is persisted");
  check(file.find("sens_h=2.5\n") == std::string::npos, "old sensitivity line is replaced");
  check(countOf(file, "sens_h=") == 1u, "coalesced sensitivity appears once");
  check(gCtlPendingMask == 0, "coalesced write clears the queue");

  int64_t e = s + 2000;
  check(sendAll(a, "SUB\n"), "SUB is sent");
  serviceAt(e);
  expectLine(a, "OK", "SUB is acknowledged");
  expectNoLine(a, "SUB sends no initial EVT");
  check(sendAll(a, "PAUSE\n"), "PAUSE is sent");
  serviceAt(e + 1);
  expectLine(a, "OK", "PAUSE is acknowledged");
  expectLine(a, "EVT state=off paused=1", "PAUSE publishes the paused event");
  check(gPaused, "PAUSE sets the in-memory flag");
  check(sendAll(a, "RESUME\n"), "RESUME is sent");
  serviceAt(e + 2);
  expectLine(a, "OK", "RESUME is acknowledged");
  expectLine(a, "EVT state=idle paused=0", "RESUME publishes the idle event");
  check(!gPaused, "RESUME clears the in-memory flag");
  check(sendAll(a, "UNSUB\n"), "UNSUB is sent");
  serviceAt(e + 3);
  expectLine(a, "OK", "UNSUB is acknowledged");
  check(sendAll(a, "PAUSE\n"), "PAUSE without subscription is sent");
  serviceAt(e + 4);
  expectLine(a, "OK", "PAUSE after UNSUB is acknowledged");
  expectNoLine(a, "UNSUB stops events");
  check(sendAll(a, "RESUME\n"), "RESUME without subscription is sent");
  serviceAt(e + 5);
  expectLine(a, "OK", "RESUME after UNSUB is acknowledged");
  expectNoLine(a, "RESUME after UNSUB sends no event");

  int64_t r = s + 3000;
  int n = connectClient();
  check(n >= 0, "replacement client connects");
  serviceAt(r);
  check(sendAll(n, "HELLO " + token + "\n"), "replacement HELLO is sent");
  serviceAt(r + 1);
  expectLine(n, hello, "replacement client authenticates");
  check(gCtlClient.fd >= 0 && !gCtlClient.subscribed, "replacement starts unsubscribed");
  expectClosed(a, "old authenticated client is closed on replacement");
  close(a);

  check(sendAll(n, "PING\n"), "replacement client PING is sent");
  serviceAt(r + 2);
  expectLine(n, "PONG", "replacement client is usable");

  std::string maxLine(ctlLineMax, 'A');
  check(sendAll(n, maxLine + "\n"), "1024-byte line is sent");
  serviceAt(r + 3);
  expectLine(n, "ERR cmd", "1024-byte line is accepted and parsed");

  int o = connectClient();
  check(o >= 0, "overlong-line client connects");
  serviceAt(r + 10);
  check(gCtlPending.fd >= 0, "overlong-line connection is pending");
  check(sendAll(o, std::string(ctlLineMax + 1, 'B')), "1025-byte line is sent");
  serviceAt(r + 11);
  check(gCtlPending.fd < 0, "1025-byte line closes the connection");
  expectClosed(o, "overlong-line peer sees the close");
  close(o);

  int64_t statusAt = T + 9000;
  gStatGrabs = 7;
  gStatReleases = 6;
  gStatSynDropped = 2;
  gProcScans = 42;
  char status[160];
  snprintf(status, sizeof status, "ST state=idle paused=0 up=9 pid=%d", (int)getpid());
  check(sendAll(n, "STATUS\n"), "STATUS is sent");
  serviceAt(statusAt);
  expectLine(n, status, "STATUS reports state, paused, uptime, and pid");
  check(sendAll(n, "STATS\n"), "STATS is sent");
  serviceAt(statusAt + 1);
  expectLine(n, "STATS grabs=7 releases=6 syn_dropped=2 scans=42",
             "STATS reports all four counters");

  int64_t stopAt = T + 10000;
  std::string beforeStop = readFile(cfgPath);
  gCtlLastPersistMs = stopAt;
  gCtlPersistDueMs = 0;
  check(sendAll(n, "SET resp=42\nSTOP\n"), "SET and STOP are pipelined");
  serviceAt(stopAt);
  expectLine(n, "OK", "SET before STOP applies");
  expectLine(n, "OK", "STOP is acknowledged");
  check(gStop == 1, "STOP requests a clean daemon exit");
  check(readFile(cfgPath) == beforeStop, "deferred SET is not written by the control loop");
  check(gCtlPendingMask == cfgBitResp, "STOP leaves the deferred write queued");
  ctlFlushConfigNow();
  file = readFile(cfgPath);
  check(file.find("resp=42\n") != std::string::npos, "pending write is flushed on exit");
  check(countOf(file, "resp=") == 1u, "flushed key appears once");
  check(gCtlPendingMask == 0, "exit flush clears the queue");
  expectClosed(n, "STOP peer sees the close");
  close(n);

  gStop = 0;
  ctlShutdown();
  fs::remove_all(dir, ec);
  printf("%s\n", gFails ? "SELFTEST CTRL FAIL" : "SELFTEST CTRL PASS");
  return gFails ? 1 : 0;
}
