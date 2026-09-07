// Host-side fake OpenEVSE controller.
//
// Speaks RAPI over a real serial port (or a PTY) so an ESP32 running this
// firmware sees a live controller with no OpenEVSE hardware attached. The
// protocol brain is src/fake_evse_core.cpp, shared verbatim with the
// in-firmware FAKE_EVSE build and its doctest suite; nothing in this file
// duplicates it.
//
//   build: make -C tools/fake_evse_host
//   run:   tools/fake_evse_host/fake_evse_host --port /dev/ttyUSB0
//
// Control channel: line commands on stdin and on a TCP port (default 9910),
// mirroring the firmware's old POST /fakeevse knobs. See usage() below.
//
// Every frame is logged both ways with a timestamp; that log is the point of
// the tool. Commands the core has no explicit case for are tagged UNHANDLED
// so a firmware that has moved on from the core is visible at a glance.

#include "fake_evse_core.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <set>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <vector>

// ───────────────────────────── logging ─────────────────────────────

static FILE *g_log = nullptr;   // frame log (stderr by default)
static bool  g_quiet = false;   // suppress the frame log on stdout

static double now_s()
{
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

static void logf(const char *fmt, ...)
{
  char stamp[32];
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm;
  localtime_r(&tv.tv_sec, &tm);
  snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d",
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));

  va_list ap;
  if(!g_quiet) {
    fprintf(stdout, "%s ", stamp);
    va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
  }
  if(g_log) {
    fprintf(g_log, "%s ", stamp);
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
  }
}

// Printable rendering of a frame for the log: CR/LF as escapes, the rest raw.
static std::string vis(const std::string &s)
{
  std::string o;
  for(char c : s) {
    if(c == '\r')      o += "\\r";
    else if(c == '\n') o += "\\n";
    else if((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) {
      char b[8]; snprintf(b, sizeof(b), "\\x%02x", (unsigned char)c); o += b;
    } else o += c;
  }
  return o;
}

// ─────────────────────── command coverage tracking ───────────────────────
//
// fake_evse_core answers "$OK" to anything it does not recognise, which keeps
// the link alive but silently hides a firmware that has started asking for
// something new. Mirror the core's explicit cases here so the log can say so.
// Keep in step with fake_evse_handle(); this list is diagnostics only and
// never changes a reply.
static const char *kCoreHandled[] = {
  "$GV", "$GS", "$GG", "$GU", "$GP", "$GF", "$GE", "$GC", "$GA", "$GT",
  "$GD", "$GI", "$SC", "$SV", "$FE", "$FS", "$FD", "$SY",
};

static bool core_handles(const std::string &verb)
{
  for(const char *c : kCoreHandled) {
    if(verb == c) return true;
  }
  return false;
}

static std::set<std::string> g_unhandled;   // reported once each

// ───────────────────────────── serial port ─────────────────────────────

static speed_t baud_constant(int baud)
{
  switch(baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return 0;
  }
}

static int open_serial(const char *path, int baud)
{
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if(fd < 0) {
    fprintf(stderr, "open %s: %s\n", path, strerror(errno));
    return -1;
  }

  speed_t spd = baud_constant(baud);
  if(spd == 0) {
    fprintf(stderr, "unsupported baud %d\n", baud);
    close(fd);
    return -1;
  }

  struct termios tio;
  if(tcgetattr(fd, &tio) != 0) {
    fprintf(stderr, "tcgetattr %s: %s\n", path, strerror(errno));
    close(fd);
    return -1;
  }
  cfmakeraw(&tio);
  cfsetispeed(&tio, spd);
  cfsetospeed(&tio, spd);
  tio.c_cflag |= (CLOCAL | CREAD);
  tio.c_cflag &= ~CRTSCTS;            // no hardware flow control on the RAPI link
  tio.c_cflag &= ~CSTOPB;             // 8N1
  tio.c_cflag &= ~PARENB;
  tio.c_cc[VMIN]  = 0;                // poll() decides when to read
  tio.c_cc[VTIME] = 0;
  if(tcsetattr(fd, TCSANOW, &tio) != 0) {
    fprintf(stderr, "tcsetattr %s: %s\n", path, strerror(errno));
    close(fd);
    return -1;
  }
  tcflush(fd, TCIOFLUSH);

  // Do not touch DTR/RTS: on a board wired straight to the ESP32's UART those
  // lines are the reset/boot strap and toggling them reboots the target.
  return fd;
}

// A PTY the firmware (or a native build, or socat) can open as its RAPI port.
// Lets the tool be exercised with no hardware attached.
static int open_pty(std::string &name_out)
{
  int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  if(fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
    fprintf(stderr, "posix_openpt: %s\n", strerror(errno));
    if(fd >= 0) close(fd);
    return -1;
  }
  char buf[128];
  if(ptsname_r(fd, buf, sizeof(buf)) != 0) {
    fprintf(stderr, "ptsname_r: %s\n", strerror(errno));
    close(fd);
    return -1;
  }
  name_out = buf;

  struct termios tio;
  if(tcgetattr(fd, &tio) == 0) {
    cfmakeraw(&tio);
    tcsetattr(fd, TCSANOW, &tio);
  }
  return fd;
}

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

// Set when the port fails underneath us. A CP2102 whose bulk-in URB stalls
// (kernel: "urb stopped: -32") keeps its device node but every read() fails
// until the tty is reopened; poll() reports POLLERR/POLLHUP. Before this the
// tool sat on the dead fd indefinitely and the firmware lost its controller
// without any warning here.
static bool g_port_lost = false;

static void write_all(int fd, const std::string &data)
{
  size_t off = 0;
  while(off < data.size()) {
    ssize_t n = write(fd, data.data() + off, data.size() - off);
    if(n > 0) { off += (size_t)n; continue; }
    if(n < 0 && (errno == EAGAIN || errno == EINTR)) {
      struct pollfd p = { fd, POLLOUT, 0 };
      poll(&p, 1, 200);
      continue;
    }
    logf("ERR  write: %s", strerror(errno));
    if(errno == EIO || errno == ENXIO || errno == ENODEV || errno == EPIPE) {
      g_port_lost = true;
    }
    return;
  }
}

// Close a failed port and retry opening it until it comes back or we are
// told to stop. Returns the new fd, or -1 on stop.
static int reopen_serial(int fd, const char *path, int baud)
{
  logf("PORT %s lost; reopening", path);
  if(fd >= 0) close(fd);
  while(!g_stop) {
    int nfd = open_serial(path, baud);
    if(nfd >= 0) {
      logf("PORT %s reopened", path);
      g_port_lost = false;
      return nfd;
    }
    usleep(500000);
  }
  return -1;
}

// ───────────────────────────── control channel ─────────────────────────────

static std::string trim(const std::string &s)
{
  size_t a = s.find_first_not_of(" \t\r\n");
  if(a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static bool truthy(const std::string &v)
{
  return v == "1" || v == "on" || v == "true" || v == "yes" || v == "y";
}

static std::string describe(const FakeEvseState &st)
{
  char buf[320];
  snprintf(buf, sizeof(buf),
           "state=%u pilot_state=%u vflags=0x%04X vehicle=%d fault=%s "
           "charging_allowed=%d pilot_a=%ld voltage=%.1f charge_a=%.1f "
           "session_wh=%.1f total_wh=%.1f elapsed=%us flicker=%d",
           st.state(), st.pilot_state(), st.vflags(), st.vehicle_present ? 1 : 0,
           st.fault.c_str(), st.charging_allowed ? 1 : 0, st.pilot_a,
           st.voltage_mv / 1000.0, st.charge_ma() / 1000.0,
           st.session_wh, st.total_wh, st.session_elapsed_s,
           st.pilot_flicker ? 1 : 0);
  return buf;
}

static const char *kUsage =
  "control commands (stdin or the control TCP port):\n"
  "  vehicle on|off            plug / unplug the car (rising edge resets the session)\n"
  "  fault none|gfci|noground|stuck|overtemp\n"
  "  voltage <volts>           supply voltage, e.g. 240\n"
  "  current <amps>            pilot/charge current, same field $SC writes\n"
  "  flicker on|off            pilot-state flicker (event-log flood repro)\n"
  "  temp <degC>               ambient temperature reported by $GP\n"
  "  status                    print the current state\n"
  "  help                      this text\n"
  "  quit                      exit the tool\n";

// Returns the reply text for the control channel; sets *quit on "quit".
static std::string control_command(FakeEvseState &st, const std::string &line, bool *quit)
{
  std::string s = trim(line);
  if(s.empty()) return "";

  // Accept "vehicle=on" as well as "vehicle on".
  for(char &c : s) if(c == '=' || c == ',') c = ' ';

  std::string verb, arg;
  size_t sp = s.find(' ');
  if(sp == std::string::npos) { verb = s; }
  else { verb = s.substr(0, sp); arg = trim(s.substr(sp + 1)); }
  for(char &c : verb) c = (char)tolower((unsigned char)c);

  if(verb == "help" || verb == "?")   return kUsage;
  if(verb == "status")                return describe(st);
  if(verb == "quit" || verb == "exit") { *quit = true; return "bye"; }

  if(verb == "vehicle") {
    st.set_vehicle(truthy(arg));
    return describe(st);
  }
  if(verb == "fault") {
    if(arg != "none" && arg != "gfci" && arg != "noground" &&
       arg != "stuck" && arg != "overtemp") {
      return "ERR fault must be none|gfci|noground|stuck|overtemp";
    }
    // Count the fault the way a real controller does, so $GF moves and the
    // firmware's fault-counter deltas are exercised.
    if(arg != st.fault) {
      if(arg == "gfci")          st.gfci_count++;
      else if(arg == "noground") st.nognd_count++;
      else if(arg == "stuck")    st.stuck_count++;
    }
    st.fault = arg;
    return describe(st);
  }
  if(verb == "voltage") {
    double v = atof(arg.c_str());
    if(v <= 0) return "ERR voltage must be > 0";
    st.voltage_mv = (long)(v * 1000.0 + 0.5);
    return describe(st);
  }
  if(verb == "current") {
    long a = strtol(arg.c_str(), nullptr, 10);
    if(a < 0) return "ERR current must be >= 0";
    st.pilot_a = a;
    return describe(st);
  }
  if(verb == "flicker") {
    st.pilot_flicker = truthy(arg);
    return describe(st);
  }
  if(verb == "temp") {
    st.temp1_tenths = (long)(atof(arg.c_str()) * 10.0 + 0.5);
    return describe(st);
  }
  return "ERR unknown command '" + verb + "' (try help)";
}

// ───────────────────────────── main ─────────────────────────────


static void usage(const char *argv0)
{
  fprintf(stderr,
    "usage: %s [options]\n"
    "  --port PATH        serial port to the ESP32 RAPI link (default /dev/ttyUSB0)\n"
    "  --baud N           default 115200\n"
    "  --pty              create a PTY instead of opening a port, print its name\n"
    "  --control-port N   TCP control port on 127.0.0.1 (default 9910, 0 disables)\n"
    "  --log FILE         append the frame log here as well as stdout\n"
    "  --quiet            frame log to --log only, not stdout\n"
    "  --vehicle          start with the car plugged in\n"
    "  --voltage V        initial supply voltage (default 240)\n"
    "  --current A        initial pilot current (default 32)\n"
    "\n%s", argv0, kUsage);
}

int main(int argc, char **argv)
{
  const char *port = "/dev/ttyUSB0";
  int  baud = 115200;
  bool pty = false;
  int  control_port = 9910;
  const char *logfile = nullptr;
  FakeEvseState st;

  for(int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char *what) -> const char * {
      if(i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", what); exit(2); }
      return argv[++i];
    };
    if(a == "--port")            port = next("--port");
    else if(a == "--baud")       baud = atoi(next("--baud"));
    else if(a == "--pty")        pty = true;
    else if(a == "--control-port") control_port = atoi(next("--control-port"));
    else if(a == "--log")        logfile = next("--log");
    else if(a == "--quiet")      g_quiet = true;
    else if(a == "--vehicle")    st.set_vehicle(true);
    else if(a == "--voltage")    st.voltage_mv = (long)(atof(next("--voltage")) * 1000.0 + 0.5);
    else if(a == "--current")    st.pilot_a = strtol(next("--current"), nullptr, 10);
    else if(a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(argv[0]); return 2; }
  }

  if(logfile) {
    g_log = fopen(logfile, "a");
    if(!g_log) { fprintf(stderr, "open %s: %s\n", logfile, strerror(errno)); return 1; }
  }
  if(g_quiet && !g_log) {
    fprintf(stderr, "--quiet without --log would discard the log; refusing\n");
    return 2;
  }

  int fd;
  if(pty) {
    std::string name;
    fd = open_pty(name);
    if(fd < 0) return 1;
    printf("%s\n", name.c_str());     // scriptable: first stdout line is the PTY
    fflush(stdout);
    logf("PTY  %s", name.c_str());
  } else {
    fd = open_serial(port, baud);
    if(fd < 0) return 1;
    logf("OPEN %s @%d 8N1", port, baud);
  }

  int listen_fd = -1;
  if(control_port > 0) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)control_port);
    if(bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
       listen(listen_fd, 4) != 0) {
      fprintf(stderr, "control port %d: %s\n", control_port, strerror(errno));
      return 1;
    }
    logf("CTRL listening on 127.0.0.1:%d", control_port);
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  std::string rx;                       // partial inbound line from the port
  std::vector<int> clients;
  std::vector<std::string> client_rx;
  double last_tick = now_s();
  // The WiFi firmware polls at least once a second ($SY when idle). A stalled
  // CP2102 read pipe (kernel "urb stopped: -32") surfaces as silence, not as
  // an error: read() returns 0 bytes forever and poll() is clean. Reopen on
  // silence rather than trusting the fd.
  const double kSilenceReopenS = 10.0;
  double last_rx = now_s();
  logf("INIT %s", describe(st).c_str());

  while(!g_stop) {
    std::vector<struct pollfd> pfds;
    pfds.push_back({ fd, POLLIN, 0 });
    pfds.push_back({ STDIN_FILENO, POLLIN, 0 });
    if(listen_fd >= 0) pfds.push_back({ listen_fd, POLLIN, 0 });
    for(int c : clients) pfds.push_back({ c, POLLIN, 0 });

    int rc = poll(pfds.data(), pfds.size(), 100);
    if(rc < 0 && errno != EINTR) { logf("ERR  poll: %s", strerror(errno)); break; }

    if(!pty && (g_port_lost || (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
                now_s() - last_rx > kSilenceReopenS)) {
      if(now_s() - last_rx > kSilenceReopenS) {
        logf("PORT no frames for %.0f s", now_s() - last_rx);
      }
      fd = reopen_serial(fd, port, baud);
      if(fd < 0) break;
      rx.clear();
      last_rx = now_s();
      continue;
    }

    // ── serial: read, split on CR/LF, answer each frame ──
    if(pfds[0].revents & POLLIN) {
      char buf[512];
      ssize_t n = read(fd, buf, sizeof(buf));
      if(n > 0) {
        rx.append(buf, (size_t)n);
        last_rx = now_s();
      } else if(n == 0) {
        // PTY with no slave attached keeps poll() readable; do not spin on it.
        usleep(20000);
      } else if(errno != EAGAIN && errno != EINTR) {
        logf("ERR  read: %s", strerror(errno));
        if(!pty) { g_port_lost = true; continue; }
      }
      size_t pos;
      while((pos = rx.find_first_of("\r\n")) != std::string::npos) {
        std::string line = rx.substr(0, pos);
        rx.erase(0, pos + 1);
        line = trim(line);
        if(line.empty()) continue;

        // Strip the checksum/sequence suffix the sender appends: "^XX" (XOR)
        // or "*XX" (additive). Verify it, but never refuse on a mismatch --
        // a mid-line reset would then wedge the link on a byte of noise.
        std::string body = line;
        size_t chk = line.find_last_of("^*");
        std::string suffix;
        if(chk != std::string::npos && chk + 1 < line.size()) {
          body   = line.substr(0, chk);
          suffix = line.substr(chk + 1);
        }

        if(body.empty() || body[0] != '$') {
          // Debug/console text sharing the UART, or a fragment after a reset.
          logf("NOISE  <- %s", vis(line).c_str());
          continue;
        }

        if(!suffix.empty() && line[chk] == '^') {
          std::string want = rapi_xor_checksum(body);
          if(strcasecmp(want.c_str(), suffix.c_str()) != 0) {
            logf("WARN   <- checksum %s, expected %s (%s)",
                 suffix.c_str(), want.c_str(), vis(line).c_str());
          }
        }

        std::string verb = body.substr(0, body.find(' '));
        bool known = core_handles(verb);
        if(!known && g_unhandled.insert(verb).second) {
          logf("UNHANDLED %s -- core has no case, replying bare $OK", verb.c_str());
        }

        std::string reply = fake_evse_handle(st, body);
        logf("RX  <- %-28s TX -> %s%s", vis(line).c_str(), vis(reply).c_str(),
             known ? "" : "   [UNHANDLED]");
        write_all(fd, reply);
      }
      if(rx.size() > 4096) {            // never let noise grow without bound
        logf("WARN   dropping %zu bytes of unterminated input", rx.size());
        rx.clear();
      }
    }

    // ── 1 Hz tick: accrue energy, emit async state changes ──
    double t = now_s();
    if(t - last_tick >= 1.0) {
      double dt = t - last_tick;
      last_tick = t;
      uint8_t before = st.state();
      std::string async = fake_evse_tick(st, dt);
      if(!async.empty()) {
        logf("TICK-> %-28s (state %u->%u)", vis(async).c_str(), before, st.state());
        write_all(fd, async);
      }
    }

    // ── control channel ──
    // pfds was built from the client list as it was at the top of the loop;
    // accept() below can grow that list, so only walk the entries poll() saw.
    size_t polled_clients = clients.size();
    size_t idx = 2;
    if(listen_fd >= 0) {
      if(pfds[idx].revents & POLLIN) {
        int c = accept(listen_fd, nullptr, nullptr);
        if(c >= 0) {
          fcntl(c, F_SETFL, O_NONBLOCK);
          clients.push_back(c);
          client_rx.push_back("");
          std::string greet = describe(st) + "\n";
          write_all(c, greet);
          logf("CTRL   client connected");
        }
      }
      idx++;
    }

    bool quit = false;
    std::vector<int> dead;
    for(size_t i = 0; i < polled_clients; i++) {
      short rev = pfds[idx + i].revents;
      if(!(rev & (POLLIN | POLLHUP | POLLERR))) continue;
      char buf[512];
      ssize_t n = read(clients[i], buf, sizeof(buf));
      if(n <= 0) { dead.push_back(clients[i]); continue; }
      client_rx[i].append(buf, (size_t)n);
      size_t pos;
      while((pos = client_rx[i].find('\n')) != std::string::npos) {
        std::string line = client_rx[i].substr(0, pos);
        client_rx[i].erase(0, pos + 1);
        if(trim(line).empty()) continue;
        logf("CTRL   %s", trim(line).c_str());
        std::string out = control_command(st, line, &quit);
        if(!out.empty()) {
          logf("CTRL   -> %s", out.c_str());
          write_all(clients[i], out + "\n");
        }
      }
    }
    for(int d : dead) {
      for(size_t i = 0; i < clients.size(); i++) {
        if(clients[i] != d) continue;
        close(clients[i]);
        clients.erase(clients.begin() + i);
        client_rx.erase(client_rx.begin() + i);
        logf("CTRL   client closed");
        break;
      }
    }

    // ── stdin ──
    if(pfds[1].revents & (POLLIN | POLLHUP)) {
      char buf[512];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if(n > 0) {
        static std::string sin_buf;
        sin_buf.append(buf, (size_t)n);
        size_t pos;
        while((pos = sin_buf.find('\n')) != std::string::npos) {
          std::string line = sin_buf.substr(0, pos);
          sin_buf.erase(0, pos + 1);
          if(trim(line).empty()) continue;
          logf("CTRL   %s", trim(line).c_str());
          std::string out = control_command(st, line, &quit);
          if(!out.empty()) logf("CTRL   -> %s", out.c_str());
        }
      }
    }
    if(quit) break;
  }

  logf("EXIT %s", describe(st).c_str());
  for(int c : clients) close(c);
  if(listen_fd >= 0) close(listen_fd);
  close(fd);
  if(g_log) fclose(g_log);
  return 0;
}
