// host test for the optional control server. it compiles flick.cpp with main renamed, binds a real
// loopback socket on port 0, and drives ctlService() directly so every deadline, reply, and config
// write can be checked without a phone, a touchscreen, or a sleep of several seconds.
//
//   g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Werror ctrl_test.cpp -o ctrl-test -lm
//
// the token below is synthetic and only exists in this test file.
#define main flickMain
#include "flick.cpp"
#undef main

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

// runs the real entry point with mutable argument strings, like the shell would
static int runMain(const std::vector<std::string> &args) {
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const std::string &s : args) argv.push_back(const_cast<char *>(s.c_str()));
  argv.push_back(nullptr);
  return flickMain((int)args.size(), argv.data());
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

  std::string cliPid =
      (fs::temp_directory_path() / ("flick-cli-" + std::to_string((long)getpid()) + ".pid")).string();
  unlink(cliPid.c_str());
  check(runMain({"flick", "--listen", "8765", "--pidfile", cliPid, "--config", "/nonexistent"}) == 10,
        "--listen without FLICK_TOKEN exits 10");
  check(runMain({"flick", "--listen"}) == 10, "missing port value exits 10");
  check(runMain({"flick", "--listen", "0"}) == 10, "zero port exits 10");
  check(runMain({"flick", "--listen", "70000"}) == 10, "out-of-range port exits 10");
  check(runMain({"flick", "--listen", "not-a-port"}) == 10, "malformed port exits 10");
  setenv("FLICK_TOKEN", shortToken.c_str(), 1);
  check(runMain({"flick", "--listen", "8765", "--pidfile", cliPid, "--config", "/nonexistent"}) == 10,
        "invalid token exits 10");
  check(getenv("FLICK_TOKEN") == nullptr, "invalid token start scrubs the environment");

  int blocker = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in ba;
  memset(&ba, 0, sizeof ba);
  ba.sin_family = AF_INET;
  ba.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool blocked =
      blocker >= 0 &&
      bind(blocker, reinterpret_cast<sockaddr *>(static_cast<void *>(&ba)), sizeof ba) == 0 &&
      listen(blocker, 1) == 0;
  socklen_t bl = sizeof ba;
  if (blocked)
    blocked = getsockname(blocker, reinterpret_cast<sockaddr *>(static_cast<void *>(&ba)), &bl) == 0;
  check(blocked, "blocker socket owns a loopback port");
  if (blocked) {
    setenv("FLICK_TOKEN", token.c_str(), 1);
    check(runMain({"flick", "--listen", std::to_string((unsigned)ntohs(ba.sin_port)),
                   "--pidfile", cliPid, "--config", "/nonexistent"}) == 11,
          "bind failure exits 11");
    check(getenv("FLICK_TOKEN") == nullptr, "failed bind start scrubs the token");
  }
  if (blocker >= 0) close(blocker);
  setenv("FLICK_TOKEN", token.c_str(), 1);
  check(ctlLoadToken(), "token is reloaded for the protocol test");

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
  std::string hello = std::string("OK flick=") + FLICK_VERSION + " proto=1 caps=hold,err";
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
  expectLine(a, "EVT state=off paused=1 hold=0", "PAUSE publishes the paused event");
  check(gPaused, "PAUSE sets the in-memory flag");
  check(sendAll(a, "RESUME\n"), "RESUME is sent");
  serviceAt(e + 2);
  expectLine(a, "OK", "RESUME is acknowledged");
  expectLine(a, "EVT state=idle paused=0 hold=0", "RESUME publishes the idle event");
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
  check(sendAll(n, "HELLO  " + token + "\n"), "replacement HELLO with extra spacing is sent");
  serviceAt(r + 1);
  expectLine(n, hello, "replacement client authenticates across extra spacing");
  check(gCtlClient.fd >= 0 && !gCtlClient.subscribed, "replacement starts unsubscribed");
  expectClosed(a, "old authenticated client is closed on replacement");
  close(a);

  check(sendAll(n, "PING\n"), "replacement client PING is sent");
  serviceAt(r + 2);
  expectLine(n, "PONG", "replacement client is usable");

  int p1 = connectClient();
  check(p1 >= 0, "first extra pending client connects");
  serviceAt(r + 3);
  check(gCtlPending.fd >= 0 && gCtlPending.acceptedMs == r + 3,
        "first extra connection occupies the pending slot");
  int p2 = connectClient();
  check(p2 >= 0, "second extra pending client connects");
  serviceAt(r + 4);
  check(gCtlPending.fd >= 0 && gCtlPending.acceptedMs == r + 4,
        "new pending connection replaces the old pending slot");
  expectClosed(p1, "old pending connection is closed on replacement");
  close(p1);
  check(sendAll(n, "PING\n"), "authenticated client PINGs during pending replacement");
  serviceAt(r + 5);
  expectLine(n, "PONG", "pending replacement does not disturb the authenticated client");
  close(p2);
  serviceAt(r + 6);
  check(gCtlPending.fd < 0, "closed pending connection is released");

  std::string maxLine(ctlLineMax, 'A');
  check(sendAll(n, maxLine + "\n"), "1024-byte line is sent");
  serviceAt(r + 20);
  expectLine(n, "ERR cmd", "1024-byte line is accepted and parsed");

  int o = connectClient();
  check(o >= 0, "overlong-line client connects");
  serviceAt(r + 30);
  check(gCtlPending.fd >= 0, "overlong-line connection is pending");
  check(sendAll(o, std::string(ctlLineMax + 1, 'B')), "1025-byte line is sent");
  serviceAt(r + 31);
  check(gCtlPending.fd < 0, "1025-byte line closes the connection");
  expectClosed(o, "overlong-line peer sees the close");
  close(o);

  int64_t errAt = r + 100;
  setState("error:grab_timeout");
  char errStatus[192];
  snprintf(errStatus, sizeof errStatus,
           "ST state=off paused=0 up=%lld pid=%d hold=0 err=grab_timeout",
           (long long)((errAt - T) / 1000), (int)getpid());
  check(sendAll(n, "STATUS\n"), "STATUS with a safe error reason is sent");
  serviceAt(errAt);
  expectLine(n, errStatus, "STATUS reports a sanitized error reason");

  setState("error:Grab Bad!");
  snprintf(errStatus, sizeof errStatus, "ST state=off paused=0 up=%lld pid=%d hold=0 err=unknown",
           (long long)((errAt + 1 - T) / 1000), (int)getpid());
  check(sendAll(n, "STATUS\n"), "STATUS with an unsafe error reason is sent");
  serviceAt(errAt + 1);
  expectLine(n, errStatus, "unsafe error characters become unknown");

  setState("error:abcdefghijklmnopqrstuvwxyz1234567");
  snprintf(errStatus, sizeof errStatus, "ST state=off paused=0 up=%lld pid=%d hold=0 err=unknown",
           (long long)((errAt + 2 - T) / 1000), (int)getpid());
  check(sendAll(n, "STATUS\n"), "STATUS with an overlong error reason is sent");
  serviceAt(errAt + 2);
  expectLine(n, errStatus, "overlong error reason becomes unknown");

  setState("idle");
  int64_t holdAt = r + 200;
  check(sendAll(n, "SUB\n"), "SUB before HOLD is sent");
  serviceAt(holdAt);
  expectLine(n, "OK", "SUB before HOLD is acknowledged");
  expectNoLine(n, "SUB before HOLD sends no initial EVT");
  std::string beforeHold = readFile(cfgPath);
  check(sendAll(n, "HOLD 0\n"), "out-of-range HOLD is sent");
  serviceAt(holdAt + 1);
  expectLine(n, "ERR hold range", "out-of-range HOLD is rejected");
  check(!gCtlHoldActive && !gPaused, "rejected HOLD changes nothing");

  int64_t h1 = holdAt + 10;
  check(sendAll(n, "HOLD 1\n"), "one-second HOLD is sent");
  serviceAt(h1);
  expectLine(n, "OK", "HOLD is acknowledged");
  expectLine(n, "EVT state=off paused=1 hold=1", "HOLD publishes paused and hold");
  check(gCtlHoldActive && !gPaused, "HOLD is separate from the PAUSE flag");
  check(gCtlHoldDeadlineMs == h1 + 1000, "HOLD expires after the requested lease");
  check(ctlDeadline() == gCtlHoldDeadlineMs, "poll deadline follows the hold lease");
  check(readFile(cfgPath) == beforeHold, "HOLD does not write the config file");

  snprintf(errStatus, sizeof errStatus,
           "ST state=off paused=1 up=%lld pid=%d hold=1 err=none",
           (long long)((h1 + 1 - T) / 1000), (int)getpid());
  check(sendAll(n, "STATUS\n"), "STATUS with an active hold is sent");
  serviceAt(h1 + 1);
  expectLine(n, errStatus, "STATUS reports paused=1 and hold=1");

  serviceAt(h1 + 1000);
  expectLine(n, "EVT state=idle paused=0 hold=0", "one-second lease expiry releases the hold");
  check(!gCtlHoldActive && !ctlEffectivePaused(), "expired lease clears effective pause");

  int64_t h2 = holdAt + 1250;
  check(sendAll(n, "HOLD 1\n"), "HOLD before the extension test is sent");
  serviceAt(h2);
  expectLine(n, "OK", "HOLD before the extension test is acknowledged");
  expectLine(n, "EVT state=off paused=1 hold=1", "HOLD before the extension is published");
  int64_t renew = h2 + 100;
  check(sendAll(n, "HOLD 3\n"), "repeated HOLD is sent");
  serviceAt(renew);
  expectLine(n, "OK", "repeated HOLD is acknowledged");
  expectNoLine(n, "repeated HOLD sends no event when the state is unchanged");
  check(gCtlHoldDeadlineMs == renew + 3000, "repeated HOLD extends the lease");
  serviceAt(h2 + 1001);
  check(gCtlHoldActive, "extended lease survives the original deadline");
  expectNoLine(n, "renewed hold sends no spurious event");
  serviceAt(renew + 3000);
  expectLine(n, "EVT state=idle paused=0 hold=0", "extended lease expiry releases the hold");
  check(!gCtlHoldActive, "extended lease is cleared on expiry");

  int64_t h3 = renew + 3100;
  check(sendAll(n, "HOLD\n"), "default HOLD is sent");
  serviceAt(h3);
  expectLine(n, "OK", "default HOLD is acknowledged");
  expectLine(n, "EVT state=off paused=1 hold=1", "default HOLD is published");
  check(gCtlHoldDeadlineMs == h3 + 30000, "HOLD defaults to 30 seconds");
  check(sendAll(n, "UNHOLD\n"), "UNHOLD is sent");
  serviceAt(h3 + 1);
  expectLine(n, "OK", "UNHOLD is acknowledged");
  expectLine(n, "EVT state=idle paused=0 hold=0", "UNHOLD publishes the cleared state");
  check(!gCtlHoldActive, "UNHOLD clears the lease");
  check(ctlDeadline() == -1, "UNHOLD removes the hold deadline");

  int64_t h4 = h3 + 100;
  check(sendAll(n, "HOLD 1\n"), "HOLD before disconnect is sent");
  serviceAt(h4);
  expectLine(n, "OK", "HOLD before disconnect is acknowledged");
  expectLine(n, "EVT state=off paused=1 hold=1", "HOLD before disconnect is published");
  close(n);
  serviceAt(h4 + 1);
  check(!gCtlHoldActive && !ctlEffectivePaused(), "client disconnect clears the hold");
  check(gCtlClient.fd < 0, "disconnected authenticated client is released");

  int64_t h5 = h4 + 100;
  n = connectClient();
  check(n >= 0, "client reconnects after hold cleanup");
  serviceAt(h5);
  check(gCtlPending.fd >= 0, "reconnected client occupies the pending slot");
  check(sendAll(n, "HELLO " + token + "\n"), "reconnected client HELLOs");
  serviceAt(h5 + 1);
  expectLine(n, hello, "reconnected client authenticates");

  check(sendAll(n, "SUB\n"), "SUB before replacement is sent");
  serviceAt(h5 + 2);
  expectLine(n, "OK", "SUB before replacement is acknowledged");
  expectNoLine(n, "SUB before replacement sends no initial EVT");
  check(sendAll(n, "HOLD 30\n"), "HOLD before replacement is sent");
  serviceAt(h5 + 3);
  expectLine(n, "OK", "HOLD before replacement is acknowledged");
  expectLine(n, "EVT state=off paused=1 hold=1", "HOLD before replacement is published");
  int q = connectClient();
  check(q >= 0, "replacement client connects during HOLD");
  serviceAt(h5 + 10);
  check(gCtlPending.fd >= 0, "replacement connection is pending");
  check(sendAll(q, "HELLO " + token + "\n"), "replacement HELLO is sent");
  serviceAt(h5 + 11);
  expectLine(q, hello, "replacement client authenticates during HOLD");
  check(gCtlClient.fd >= 0 && !gCtlClient.subscribed, "replacement starts unsubscribed");
  check(!gCtlHoldActive && !ctlEffectivePaused(), "replacement clears the old hold");
  expectClosed(n, "old client is closed when replaced during HOLD");
  close(n);

  int64_t h6 = h5 + 100;
  check(sendAll(q, "SUB\n"), "SUB for PAUSE/HOLD interaction is sent");
  serviceAt(h6);
  expectLine(q, "OK", "SUB for PAUSE/HOLD interaction is acknowledged");
  expectNoLine(q, "SUB for PAUSE/HOLD interaction sends no initial EVT");
  check(sendAll(q, "PAUSE\n"), "PAUSE before HOLD is sent");
  serviceAt(h6 + 1);
  expectLine(q, "OK", "PAUSE before HOLD is acknowledged");
  expectLine(q, "EVT state=off paused=1 hold=0", "PAUSE before HOLD is published");
  check(sendAll(q, "HOLD 30\n"), "HOLD while PAUSE is active is sent");
  serviceAt(h6 + 2);
  expectLine(q, "OK", "HOLD while PAUSE is active is acknowledged");
  expectLine(q, "EVT state=off paused=1 hold=1", "HOLD while PAUSE is active is published");
  check(sendAll(q, "UNHOLD\n"), "UNHOLD while PAUSE is active is sent");
  serviceAt(h6 + 3);
  expectLine(q, "OK", "UNHOLD while PAUSE is active is acknowledged");
  expectLine(q, "EVT state=off paused=1 hold=0", "UNHOLD leaves PAUSE effective");
  check(gPaused && !gCtlHoldActive && ctlEffectivePaused(), "UNHOLD does not clear PAUSE");
  check(sendAll(q, "HOLD 30\n"), "second HOLD while PAUSE is active is sent");
  serviceAt(h6 + 4);
  expectLine(q, "OK", "second HOLD while PAUSE is active is acknowledged");
  expectLine(q, "EVT state=off paused=1 hold=1", "second HOLD while PAUSE is active is published");
  check(sendAll(q, "RESUME\n"), "RESUME while HOLD is active is sent");
  serviceAt(h6 + 5);
  expectLine(q, "OK", "RESUME while HOLD is active is acknowledged");
  expectNoLine(q, "RESUME while HOLD is active sends no event");
  check(!gPaused && gCtlHoldActive && ctlEffectivePaused(), "RESUME leaves the hold effective");
  snprintf(errStatus, sizeof errStatus,
           "ST state=off paused=1 up=%lld pid=%d hold=1 err=none",
           (long long)((h6 + 6 - T) / 1000), (int)getpid());
  check(sendAll(q, "STATUS\n"), "STATUS with PAUSE cleared and HOLD active is sent");
  serviceAt(h6 + 6);
  expectLine(q, errStatus, "STATUS keeps paused=1 while HOLD is active");
  check(sendAll(q, "UNHOLD\n"), "UNHOLD after RESUME is sent");
  serviceAt(h6 + 7);
  expectLine(q, "OK", "UNHOLD after RESUME is acknowledged");
  expectLine(q, "EVT state=idle paused=0 hold=0", "UNHOLD after RESUME clears effective pause");
  check(!gCtlHoldActive && !gPaused, "final UNHOLD clears the lease");

  int64_t statusAt = T + 20000;
  gStatGrabs = 7;
  gStatReleases = 6;
  gStatSynDropped = 2;
  gProcScans = 42;
  char status[160];
  snprintf(status, sizeof status, "ST state=idle paused=0 up=20 pid=%d hold=0 err=none",
           (int)getpid());
  check(sendAll(q, "STATUS\n"), "STATUS is sent");
  serviceAt(statusAt);
  expectLine(q, status, "STATUS reports state, paused, uptime, pid, hold, and err");
  check(sendAll(q, "STATS\n"), "STATS is sent");
  serviceAt(statusAt + 1);
  expectLine(q, "STATS grabs=7 releases=6 syn_dropped=2 scans=42",
             "STATS reports all four counters");

  int64_t stopAt = T + 21000;
  check(sendAll(q, "HOLD 30\n"), "HOLD before STOP is sent");
  serviceAt(stopAt - 10);
  expectLine(q, "OK", "HOLD before STOP is acknowledged");
  expectLine(q, "EVT state=off paused=1 hold=1", "HOLD before STOP is published");
  check(gCtlHoldActive, "STOP test hold is active");
  std::string beforeStop = readFile(cfgPath);
  gCtlLastPersistMs = stopAt;
  gCtlPersistDueMs = 0;
  check(sendAll(q, "SET resp=42\nSTOP\n"), "SET and STOP are pipelined");
  serviceAt(stopAt);
  expectLine(q, "OK", "SET before STOP applies");
  expectLine(q, "OK", "STOP is acknowledged");
  expectLine(q, "EVT state=idle paused=0 hold=0", "STOP releases the hold and publishes it");
  check(gStop == 1, "STOP requests a clean daemon exit");
  check(!gCtlHoldActive, "STOP clears the hold");
  check(readFile(cfgPath) == beforeStop, "deferred SET is not written by the control loop");
  check(gCtlPendingMask == cfgBitResp, "STOP leaves the deferred write queued");
  ctlFlushConfigNow();
  file = readFile(cfgPath);
  check(file.find("resp=42\n") != std::string::npos, "pending write is flushed on exit");
  check(countOf(file, "resp=") == 1u, "flushed key appears once");
  check(gCtlPendingMask == 0, "exit flush clears the queue");
  expectClosed(q, "STOP peer sees the close");
  close(q);

  gStop = 0;
  ctlShutdown();
  fs::remove_all(dir, ec);
  printf("%s\n", gFails ? "SELFTEST CTRL FAIL" : "SELFTEST CTRL PASS");
  return gFails ? 1 : 0;
}
