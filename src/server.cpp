// chronos_server: C++ HTTP + WebSocket gateway around the matching engine. No auth: loopback by default.
//   ./build/chronos_server [--port 8080] [--host 127.0.0.1] [--web web] [--bench ./build/chronos_bench|none] [--results results] [--idle-stop SECONDS]
// The engine lives behind one mutex; the simulator/REST handlers take it per small chunk and the
// broadcaster takes it for ~microseconds to copy a snapshot. Rendering happens in the browser.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
using ssize_t = intptr_t;
#endif
#define popen _popen
#define pclose _pclose
#define SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <signal.h>

#include <filesystem>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "chronos/bench.hpp"
#include "chronos/engine.hpp"
#include "chronos/workload.hpp"

using namespace chronos;

// Socket calls that differ between POSIX and Winsock.
static void closeSock(int fd) {
#if defined(_WIN32)
  closesocket(SOCKET(fd));
#else
  close(fd);
#endif
}
static void setTimeout(int fd, int opt, long sec, long usec) {  // opt: SO_RCVTIMEO / SO_SNDTIMEO
#if defined(_WIN32)
  DWORD ms = DWORD(sec * 1000 + usec / 1000);
  setsockopt(SOCKET(fd), SOL_SOCKET, opt, (const char*)&ms, sizeof ms);
#else
  timeval tv;
  tv.tv_sec = sec, tv.tv_usec = decltype(tv.tv_usec)(usec);
  setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
#endif
}
static ssize_t rcv(int fd, void* b, size_t n) { return recv(fd, (char*)b, int(n), 0); }
static ssize_t snd(int fd, const void* b, size_t n) { return send(fd, (const char*)b, int(n), 0); }

// ---------------------------------------------------------------- small utilities
static std::string sha1(const std::string& s) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string m = s;
  m += char(0x80);
  while (m.size() % 64 != 56) m += char(0);
  uint64_t bits = uint64_t(s.size()) * 8;
  for (int i = 7; i >= 0; --i) m += char((bits >> (8 * i)) & 0xFF);
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (size_t o = 0; o < m.size(); o += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t((uint8_t)m[o + 4 * i]) << 24 | (uint8_t)m[o + 4 * i + 1] << 16 | (uint8_t)m[o + 4 * i + 2] << 8 | (uint8_t)m[o + 4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  std::string r;
  for (uint32_t x : h)
    for (int i = 3; i >= 0; --i) r += char((x >> (8 * i)) & 0xFF);
  return r;
}
static std::string b64(const std::string& s) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string r;
  size_t i = 0;
  for (; i + 2 < s.size(); i += 3) {
    uint32_t v = (uint8_t)s[i] << 16 | (uint8_t)s[i + 1] << 8 | (uint8_t)s[i + 2];
    r += t[v >> 18], r += t[(v >> 12) & 63], r += t[(v >> 6) & 63], r += t[v & 63];
  }
  if (i + 1 == s.size()) {
    uint32_t v = (uint8_t)s[i] << 16;
    r += t[v >> 18], r += t[(v >> 12) & 63], r += "==";
  } else if (i + 2 == s.size()) {
    uint32_t v = (uint8_t)s[i] << 16 | (uint8_t)s[i + 1] << 8;
    r += t[v >> 18], r += t[(v >> 12) & 63], r += t[(v >> 6) & 63], r += '=';
  }
  return r;
}

// Flat-JSON field extraction (request bodies are one-level objects we define ourselves).
static bool jraw(const std::string& b, const char* k, std::string& out) {
  size_t p = b.find(std::string("\"") + k + "\"");
  if (p == std::string::npos) return false;
  p = b.find(':', p);
  if (p == std::string::npos) return false;
  ++p;
  while (p < b.size() && (b[p] == ' ' || b[p] == '\t' || b[p] == '\n')) ++p;
  size_t e = p;
  if (p < b.size() && b[p] == '"') {
    e = b.find('"', p + 1);
    if (e == std::string::npos) return false;
    out = b.substr(p + 1, e - p - 1);
    return true;
  }
  while (e < b.size() && b[e] != ',' && b[e] != '}' && b[e] != ' ' && b[e] != '\n') ++e;
  out = b.substr(p, e - p);
  return !out.empty();
}
static bool jnum(const std::string& b, const char* k, double& v) {
  std::string s;
  if (!jraw(b, k, s)) return false;
  char* end;
  v = std::strtod(s.c_str(), &end);
  return end != s.c_str() && *end == 0 && std::isfinite(v);
}
static std::string esc(const std::string& s) {
  std::string r;
  for (char c : s) {
    if (c == '"' || c == '\\') r += '\\', r += c;
    else if ((unsigned char)c >= 0x20) r += c;
  }
  return r;
}

// ---------------------------------------------------------------- live engine state
static const char* modeName(Mode m) { return m == Mode::Tree ? "tree" : m == Mode::Array ? "array" : "adaptive"; }
static uint64_t wallUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// ---- limits and defaults: one source of truth, exposed to the dashboard through /api/meta ----
static const double kMaxQty = 1e12, kMaxRate = 2e6, kMaxCancel = 0.95, kMinBenchN = 1000, kMaxBenchN = 5e6, kDefaultRate = 50000;
static const int kMaxTrials = 10, kMinDepth = 5, kMaxDepth = 80;
static const Id kManualBase = 1ull << 40;  // ids below this belong to the simulator
static Band gBand{};                       // price collar of the live engine (--band-hi)
static size_t gBatch = 4096, gMaxActive = 1500000;
static std::atomic<int> gDepth{14};        // book levels per side in snapshots; the dashboard asks for what fits its screen
static std::atomic<int> gConn{0};
static const int kMaxConn = 256;

struct Live {
  std::mutex m;
  std::unique_ptr<Engine> e;
  Mode mode = Mode::Adaptive;
  Ring<Trade> hist{5000};
  Ring<uint32_t> lat{32768};
  std::unique_ptr<Gen> gen;
  bool sim = false;
  double rate = kDefaultRate;
  std::string note;  // why the simulator stopped on its own, if it did
  Wl wl = Wl::Mixed;
  uint64_t manualId = kManualBase;
  uint64_t t0 = wallUs();
  void reset(Mode md) {
    mode = md;
    Config c;
    c.mode = md, c.band = gBand, c.adapt.batch = gBatch;
    note.clear(), manualId = kManualBase;
    e = std::make_unique<Engine>(c);
    hist.clear(), lat.clear();
    gen.reset();
    sim = false;
  }
} L;

// caller holds L.m
static Status exec(const Cmd& c, std::vector<Trade>* out = nullptr) {
  const uint64_t a = bench::nowNs();
  Status s = L.e->process(c);
  const uint64_t b = bench::nowNs();
  L.lat.push(uint32_t(std::min<uint64_t>(b - a, 0xFFFFFFFFu)));
  for (const Trade& t : L.e->trades) {
    L.hist.push(t);
    if (out) out->push_back(t);
  }
  L.e->trades.clear();
  return s;
}

static void simLoop() {
  double carry = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    double n;
    {
      std::lock_guard<std::mutex> lk(L.m);
      if (!L.sim || !L.gen) continue;
      carry += L.rate * 0.02;
      n = std::floor(carry);
      carry -= n;
    }
    for (uint64_t left = uint64_t(n); left > 0;) {
      uint64_t k = std::min<uint64_t>(left, 512);
      std::lock_guard<std::mutex> lk(L.m);
      if (!L.sim || !L.gen) break;
      for (uint64_t i = 0; i < k; ++i) exec(L.gen->next(wallUs()));
      left -= k;
      if (L.e->active() > gMaxActive) {  // safety valve: an unbounded synthetic book would eventually exhaust memory
        L.sim = false, L.note = "simulator stopped: book reached " + std::to_string(gMaxActive) + " resting orders (raise --max-active)";
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- snapshot (what the dashboard renders)
static std::mutex gSnapM;
static uint64_t gPrevOps = 0, gPrevT = 0, gSentTid = 0;
static double gOpsPerSec = 0;

static std::string snapshot(bool withTrades) {
  std::string j;
  std::lock_guard<std::mutex> lk(L.m);
  Engine& e = *L.e;
  const auto& c = e.counters();
  const uint64_t now = wallUs();
  if (gPrevT && now > gPrevT + 100000) {
    gOpsPerSec = double(c.ops - gPrevOps) * 1e6 / double(now - gPrevT);
    gPrevOps = c.ops, gPrevT = now;
  } else if (!gPrevT) gPrevOps = c.ops, gPrevT = now;
  std::vector<uint32_t> lv;
  for (size_t i = 0; i < L.lat.size(); ++i) lv.push_back(L.lat.at(i));
  bench::Lat l = bench::summarize(lv);
  const auto ps = e.poolStats();
  const auto& b = e.lastBatch();
  auto num = [](double v) { char t[40]; std::snprintf(t, sizeof t, "%.4g", v); return std::string(t); };
  j += "{\"t\":" + std::to_string(now / 1000) + ",\"mode\":\"" + modeName(L.mode) + "\",\"sim\":" + (L.sim ? "true" : "false") + ",\"simKind\":\"" + wlName(L.wl) +
       "\",\"simNote\":\"" + esc(L.note) + "\",\"depth\":" + std::to_string(gDepth.load()) + ",\"rate\":" + num(L.rate) + ",\"band\":[" + std::to_string(e.config().band.lo) + "," + std::to_string(e.config().band.hi) + "]";
  j += ",\"stats\":{\"received\":" + std::to_string(c.received) + ",\"matched\":" + std::to_string(c.matchedOrders) + ",\"trades\":" + std::to_string(c.trades) +
       ",\"tradedQty\":" + std::to_string(c.tradedQty) + ",\"cancels\":" + std::to_string(c.cancels) + ",\"rejects\":" + std::to_string(c.rejects) +
       ",\"ops\":" + std::to_string(c.ops) + ",\"activeBuy\":" + std::to_string(e.activeBuy()) + ",\"activeSell\":" + std::to_string(e.activeSell()) +
       ",\"levels\":" + std::to_string(e.levels()) + ",\"opsPerSec\":" + num(gOpsPerSec) + ",\"avgNs\":" + num(l.avg) + ",\"p50Ns\":" + num(l.p50) + ",\"p99Ns\":" + num(l.p99) +
       ",\"p999Ns\":" + num(l.p999) + ",\"engineBytes\":" + std::to_string(e.bytes()) + ",\"heapBytes\":" + std::to_string(bench::heapInUse()) +
       ",\"poolUsed\":" + std::to_string(ps.used) + ",\"poolCap\":" + std::to_string(ps.capacity) + ",\"poolAllocs\":" + std::to_string(ps.allocs) +
       ",\"poolFrees\":" + std::to_string(ps.frees) + "}";
  j += ",\"book\":{";
  for (Side s : {Side::Buy, Side::Sell}) {
    j += s == Side::Buy ? "\"bids\":[" : ",\"asks\":[";
    Qty cum = 0;
    bool first = true;
    for (auto& d : e.depth(s, size_t(gDepth.load()))) {
      cum += d.qty;
      j += (first ? "" : ",") + std::string("[") + std::to_string(d.price) + "," + std::to_string(d.qty) + "," + std::to_string(d.n) + "," + std::to_string(cum) + "]";
      first = false;
    }
    j += "]";
  }
  j += "}";
  if (withTrades) {
    j += ",\"trades\":[";
    bool first = true;
    for (size_t i = std::max<size_t>(0, L.hist.size() > 60 ? L.hist.size() - 60 : 0); i < L.hist.size(); ++i) {
      const Trade& t = L.hist.at(i);
      if (t.tid <= gSentTid) continue;
      j += (first ? "" : ",") + std::string("[") + std::to_string(t.tid) + "," + std::to_string(t.buy) + "," + std::to_string(t.sell) + "," + std::to_string(t.price) + "," +
           std::to_string(t.qty) + "," + std::to_string(t.ts / 1000) + "]";
      first = false;
    }
    if (L.hist.size()) gSentTid = L.hist.at(L.hist.size() - 1).tid;
    j += "]";
  }
  const AdaptCfg& ac = e.config().adapt;
  j += ",\"adapt\":{\"usingArray\":" + std::string(e.usingArray() ? "true" : "false") + ",\"levels\":" + std::to_string(b.levels) + ",\"density\":" + num(b.density) +
       ",\"insertRate\":" + num(b.insertRate) + ",\"cancelRate\":" + num(b.cancelRate) + ",\"bestRate\":" + num(b.bestRate) + ",\"churn\":" + num(b.churn) + ",\"nsPerOp\":" + num(b.nsPerOp) +
       ",\"bytes\":" + std::to_string(b.bytes) + ",\"reason\":\"" + esc(e.reason()) + "\",\"cfg\":{\"policy\":" + std::to_string(ac.policy) + ",\"batch\":" + std::to_string(ac.batch) +
       ",\"levelsHi\":" + std::to_string(ac.levelsHi) + ",\"levelsLo\":" + std::to_string(ac.levelsLo) + ",\"memBudgetMB\":" + std::to_string(ac.memBudget >> 20) + ",\"denseHi\":" + num(ac.denseHi) +
       ",\"sparseLo\":" + num(ac.sparseLo) + ",\"minLevels\":" + std::to_string(ac.minLevels) + ",\"minChurn\":" + num(ac.minChurn) + ",\"confirm\":" + std::to_string(ac.confirm) +
       "},\"nEvents\":" + std::to_string(e.events().size()) + ",\"events\":[";
  const auto& ev = e.events();
  for (size_t i = ev.size() > 30 ? ev.size() - 30 : 0; i < ev.size(); ++i) {
    if (i != (ev.size() > 30 ? ev.size() - 30 : 0)) j += ",";
    j += std::string("{\"op\":") + std::to_string(ev[i].atOp) + ",\"toArray\":" + (ev[i].toArray ? "true" : "false") + ",\"ms\":" + num(ev[i].ms) + ",\"before\":" + std::to_string(ev[i].bytesBefore) +
         ",\"after\":" + std::to_string(ev[i].bytesAfter) + ",\"orders\":" + std::to_string(ev[i].orders) + ",\"levels\":" + std::to_string(ev[i].levels) + ",\"why\":\"" + esc(ev[i].reason) + "\"}";
  }
  j += "]}}";
  return j;
}

// ---------------------------------------------------------------- benchmark child process
struct BenchState {
  std::mutex m;
  bool running = false;
  int done = 0, total = 0, seq = 0;
  std::string label, file, error;
} BS;
static std::string gBenchBin = "./build/chronos_bench", gResults = "results", gWeb = "web";
static int gIdleStop = 0;  // seconds without any WebSocket viewer before the simulator pauses itself (0 = never)

static void benchThread(std::string cmd, std::string file) {
  FILE* p = popen((cmd + " 2>&1").c_str(), "r");
  std::string err;
  if (!p) err = "cannot launch benchmark binary";
  else {
    char line[512];
    std::string last;
    while (std::fgets(line, sizeof line, p)) {
      int d, t;
      char lab[200];
      if (std::sscanf(line, "PROGRESS %d %d %199s", &d, &t, lab) == 3) {
        std::lock_guard<std::mutex> lk(BS.m);
        BS.done = d, BS.total = t, BS.label = lab;
      } else last = line;
    }
    if (pclose(p) != 0) err = "benchmark exited with error: " + last;
  }
  std::lock_guard<std::mutex> lk(BS.m);
  BS.running = false, BS.error = err, BS.file = err.empty() ? file : "";
}

// ---------------------------------------------------------------- HTTP
static bool sendAll(int fd, const char* d, size_t n) {
  while (n) {
    ssize_t k = snd(fd, d, std::min<size_t>(n, 1 << 20));
    if (k <= 0) return false;
    d += k, n -= size_t(k);
  }
  return true;
}
static void reply(int fd, int code, const std::string& ctype, const std::string& body) {
  const char* st = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 409 ? "Conflict" : "Error";
  std::string h = "HTTP/1.1 " + std::to_string(code) + " " + st + "\r\nContent-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
  sendAll(fd, h.data(), h.size()), sendAll(fd, body.data(), body.size());
}
static void err(int fd, int code, const std::string& m) { reply(fd, code, "application/json", "{\"error\":\"" + esc(m) + "\"}"); }

static bool safeName(const std::string& n) {
  if (n.empty() || n.size() > 80 || n[0] == '.') return false;
  for (char c : n)
    if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
  return true;
}
static std::string readFile(const std::string& p, bool& ok) {
  std::ifstream f(p, std::ios::binary);
  ok = bool(f);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static std::string ctypeOf(const std::string& n) {
  auto ends = [&](const char* e) { size_t l = std::strlen(e); return n.size() >= l && n.compare(n.size() - l, l, e) == 0; };
  return ends(".html") ? "text/html; charset=utf-8" : ends(".js") ? "text/javascript" : ends(".css") ? "text/css" : ends(".json") ? "application/json" : ends(".csv") ? "text/csv" : ends(".svg") ? "image/svg+xml" : "text/plain";
}

static std::string statusJson(Status s, const std::vector<Trade>& tr) {
  std::string j = "{\"status\":\"" + std::string(name(s)) + "\",\"trades\":[";
  for (size_t i = 0; i < tr.size(); ++i)
    j += (i ? "," : "") + std::string("{\"id\":") + std::to_string(tr[i].tid) + ",\"buy\":" + std::to_string(tr[i].buy) + ",\"sell\":" + std::to_string(tr[i].sell) + ",\"price\":" + std::to_string(tr[i].price) +
         ",\"qty\":" + std::to_string(tr[i].qty) + "}";
  return j + "]}";
}

static void handleApi(int fd, const std::string& method, const std::string& path, const std::string& query, const std::string& body) {
  if (method == "GET" && path == "/api/state") return reply(fd, 200, "application/json", snapshot(false));
  if (method == "GET" && path == "/api/trades") {
    std::lock_guard<std::mutex> lk(L.m);
    size_t lim = 500;
    if (query.rfind("limit=", 0) == 0) lim = std::min<size_t>(5000, std::strtoul(query.c_str() + 6, 0, 10));
    std::string j = "[";
    size_t from = L.hist.size() > lim ? L.hist.size() - lim : 0;
    for (size_t i = from; i < L.hist.size(); ++i) {
      const Trade& t = L.hist.at(i);
      j += (i > from ? "," : "") + std::string("[") + std::to_string(t.tid) + "," + std::to_string(t.buy) + "," + std::to_string(t.sell) + "," + std::to_string(t.price) + "," + std::to_string(t.qty) + "," +
           std::to_string(t.ts / 1000) + "]";
    }
    return reply(fd, 200, "application/json", j + "]");
  }
  if (method == "GET" && path == "/api/bench/status") {
    std::lock_guard<std::mutex> lk(BS.m);
    return reply(fd, 200, "application/json",
                 "{\"running\":" + std::string(BS.running ? "true" : "false") + ",\"done\":" + std::to_string(BS.done) + ",\"total\":" + std::to_string(BS.total) + ",\"label\":\"" + esc(BS.label) +
                     "\",\"file\":\"" + esc(BS.file) + "\",\"error\":\"" + esc(BS.error) + "\"}");
  }
  if (method == "GET" && path == "/api/bench/list") {
    std::string j = "[";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(gResults, ec), end; !ec && it != end; it.increment(ec)) {
      std::string n = it->path().filename().string();
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0 && safeName(n)) j += (j.size() > 1 ? "," : "") + std::string("\"") + n + "\"";
    }
    return reply(fd, 200, "application/json", j + "]");
  }
  if (method == "GET" && path == "/api/bench/results") {
    if (query.rfind("file=", 0) != 0 || !safeName(query.substr(5))) return err(fd, 400, "bad file name");
    bool ok;
    std::string s = readFile(gResults + "/" + query.substr(5), ok);
    return ok ? reply(fd, 200, "application/json", s) : err(fd, 404, "no such result file");
  }
  if (method == "GET" && path == "/api/meta") {
    std::string wl;
    for (int i = 0; i < kNumWl; ++i) wl += (i ? ",\"" : "\"") + std::string(wlName(Wl(i))) + "\"";
    return reply(fd, 200, "application/json",
                 "{\"band\":[" + std::to_string(gBand.lo) + "," + std::to_string(gBand.hi) + "],\"workloads\":[" + wl + "],\"modes\":[\"adaptive\",\"tree\",\"array\"],\"strategies\":[\"std\",\"tree\",\"array\",\"adaptive\"],"
                 "\"limits\":{\"maxQty\":" + std::to_string(uint64_t(kMaxQty)) + ",\"maxRate\":" + std::to_string(uint64_t(kMaxRate)) + ",\"maxCancel\":" + std::to_string(kMaxCancel) + ",\"minBenchN\":" +
                 std::to_string(uint64_t(kMinBenchN)) + ",\"maxBenchN\":" + std::to_string(uint64_t(kMaxBenchN)) + ",\"maxTrials\":" + std::to_string(kMaxTrials) + ",\"minDepth\":" + std::to_string(kMinDepth) + ",\"maxDepth\":" +
                 std::to_string(kMaxDepth) + ",\"maxActive\":" + std::to_string(gMaxActive) + "},\"defaults\":{\"rate\":" + std::to_string(uint64_t(kDefaultRate)) + ",\"workload\":\"mixed\",\"mode\":\"adaptive\"}}");
  }
  if (method != "POST") return err(fd, 404, "not found");

  std::string s;
  double v;
  if (path == "/api/order") {
    Cmd c;
    c.kind = Kind::New;
    if (!jraw(body, "side", s) || (s != "buy" && s != "sell")) return err(fd, 400, "side must be buy or sell");
    c.side = s == "buy" ? Side::Buy : Side::Sell;
    if (!jraw(body, "type", s) || (s != "limit" && s != "market")) return err(fd, 400, "type must be limit or market");
    c.type = s == "limit" ? Type::Limit : Type::Market;
    if (!jnum(body, "qty", v) || v < 1 || v > kMaxQty || v != std::floor(v)) return err(fd, 400, "qty must be an integer in 1.." + std::to_string(uint64_t(kMaxQty)));
    c.qty = Qty(v);
    if (c.type == Type::Limit) {
      if (!jnum(body, "price", v) || v != std::floor(v)) return err(fd, 400, "limit orders need an integer price");
      c.price = Price(v);
      if (c.price < gBand.lo || c.price >= gBand.hi) return err(fd, 400, "price outside the band [" + std::to_string(gBand.lo) + ", " + std::to_string(gBand.hi) + ")");
    }
    std::lock_guard<std::mutex> lk(L.m);
    if (jnum(body, "id", v)) {
      if (v < 1 || v >= double(kManualBase) || v != std::floor(v)) return err(fd, 400, "id must be an integer in 1.." + std::to_string(kManualBase - 1));
      c.id = Id(v);
    } else c.id = L.manualId++;
    c.ts = wallUs();
    std::vector<Trade> tr;
    Status st = exec(c, &tr);
    return reply(fd, 200, "application/json", "{\"id\":" + std::to_string(c.id) + "," + statusJson(st, tr).substr(1));
  }
  if (path == "/api/cancel" || path == "/api/modify") {
    if (!jnum(body, "id", v) || v < 1 || v != std::floor(v)) return err(fd, 400, "id required");
    Cmd c;
    c.id = Id(v);
    c.kind = path == "/api/cancel" ? Kind::Cancel : Kind::Modify;
    if (c.kind == Kind::Modify) {
      double p, q;
      if (!jnum(body, "price", p) || !jnum(body, "qty", q) || p != std::floor(p) || q != std::floor(q) || q < 1 || q > kMaxQty) return err(fd, 400, "modify needs integer price and qty");
      c.price = Price(p), c.qty = Qty(q);
    }
    std::lock_guard<std::mutex> lk(L.m);
    c.ts = wallUs();
    std::vector<Trade> tr;
    Status st = exec(c, &tr);
    return reply(fd, 200, "application/json", statusJson(st, tr));
  }
  if (path == "/api/engine") {
    if (!jraw(body, "mode", s) || (s != "tree" && s != "array" && s != "adaptive")) return err(fd, 400, "mode must be tree, array or adaptive");
    std::lock_guard<std::mutex> lk(L.m);
    L.reset(s == "tree" ? Mode::Tree : s == "array" ? Mode::Array : Mode::Adaptive);
    gSentTid = 0, gPrevT = 0;
    return reply(fd, 200, "application/json", "{\"ok\":true,\"mode\":\"" + s + "\"}");
  }
  if (path == "/api/sim") {
    if (!jraw(body, "action", s) || (s != "start" && s != "stop")) return err(fd, 400, "action must be start or stop");
    std::lock_guard<std::mutex> lk(L.m);
    if (s == "stop") {
      L.sim = false;
      return reply(fd, 200, "application/json", "{\"ok\":true}");
    }
    Wl w = Wl::Mixed;
    if (jraw(body, "workload", s) && !wlParse(s, w)) return err(fd, 400, "unknown workload");
    double rate = kDefaultRate, seed = 42;
    if (jnum(body, "rate", v)) rate = v;
    if (rate < 1 || rate > kMaxRate) return err(fd, 400, "rate must be 1.." + std::to_string(uint64_t(kMaxRate)) + " orders/s");
    if (jnum(body, "seed", v)) seed = v;
    WlCfg c = preset(w);
    c.seed = uint64_t(seed);
    if (jnum(body, "cancel", v)) {
      if (v < 0 || v > kMaxCancel) return err(fd, 400, "cancel must be 0..0.95");
      c.cancel = v;
    }
    L.reset(L.mode);  // fresh book: generator ids restart at 1
    gSentTid = 0, gPrevT = 0;
    L.gen = std::make_unique<Gen>(c);
    L.wl = w, L.rate = rate, L.sim = true;
    return reply(fd, 200, "application/json", "{\"ok\":true,\"note\":\"book was reset\"}");
  }
  if (path == "/api/bench/run") {
    if (gBenchBin == "none") return err(fd, 403, "benchmark runs are disabled on this server (stored results can still be viewed)");
    std::string wl = "all", strat = "std,tree,array,adaptive";
    jraw(body, "workload", wl), jraw(body, "strategies", strat);
    Wl w;
    if (wl != "all" && !wlParse(wl, w)) return err(fd, 400, "unknown workload");
    for (size_t i = 0, j; i <= strat.size(); i = j + 1) {
      j = strat.find(',', i);
      if (j == std::string::npos) j = strat.size();
      std::string t = strat.substr(i, j - i);
      if (t != "std" && t != "tree" && t != "array" && t != "adaptive") return err(fd, 400, "bad strategy list");
    }
    double n = 200000, trials = 3, cancel = -1;
    if (jnum(body, "n", v)) n = v;
    if (jnum(body, "trials", v)) trials = v;
    bool hasCancel = jnum(body, "cancel", cancel);
    if (n < kMinBenchN || n > kMaxBenchN || n != std::floor(n)) return err(fd, 400, "n must be an integer in 1000..5000000");
    if (trials < 1 || trials > kMaxTrials || trials != std::floor(trials)) return err(fd, 400, "trials must be 1..10");
    if (hasCancel && (cancel < 0 || cancel > kMaxCancel)) return err(fd, 400, "cancel must be 0..0.95");
    std::lock_guard<std::mutex> lk(BS.m);
    if (BS.running) return err(fd, 409, "a benchmark is already running");
    {  // a live simulator would steal CPU and invalidate measurements
      std::lock_guard<std::mutex> ll(L.m);
      L.sim = false;
    }
    std::string file = "lab_" + std::to_string(++BS.seq) + "_" + std::to_string(wallUs() / 1000000) + ".json";
    std::string cmd = gBenchBin + " --n " + std::to_string(uint64_t(n)) + " --trials " + std::to_string(int(trials)) + " --strategies " + strat + " --progress --out " + gResults + "/" + file;
    if (wl != "all") cmd += " --workloads " + wl;
    if (hasCancel) cmd += " --cancel " + std::to_string(cancel);
    BS.running = true, BS.done = 0, BS.total = 0, BS.error.clear(), BS.file.clear();
    std::thread(benchThread, cmd, file).detach();
    return reply(fd, 200, "application/json", "{\"ok\":true,\"file\":\"" + file + "\",\"note\":\"live simulator paused while the benchmark runs\"}");
  }
  err(fd, 404, "not found");
}

// ---------------------------------------------------------------- WebSocket
static std::mutex gWsM;
static std::vector<int> gWs;

static std::string wsFrame(const std::string& p) {
  std::string f;
  f += char(0x81);
  if (p.size() < 126) f += char(p.size());
  else if (p.size() < 65536) f += char(126), f += char(p.size() >> 8), f += char(p.size() & 0xFF);
  else {
    f += char(127);
    for (int i = 7; i >= 0; --i) f += char((uint64_t(p.size()) >> (8 * i)) & 0xFF);
  }
  return f + p;
}
static bool readExact(int fd, void* b, size_t n) {
  char* p = (char*)b;
  while (n) {
    ssize_t k = rcv(fd, p, n);
    if (k <= 0) return false;
    p += k, n -= size_t(k);
  }
  return true;
}
static void wsReader(int fd) {
  while (true) {  // we only care about close/ping; payloads from the browser are ignored
    uint8_t h[2];
    if (!readExact(fd, h, 2)) break;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) { uint8_t e[2]; if (!readExact(fd, e, 2)) break; len = e[0] << 8 | e[1]; }
    else if (len == 127) { uint8_t e[8]; if (!readExact(fd, e, 8)) break; len = 0; for (int i = 0; i < 8; ++i) len = len << 8 | e[i]; }
    uint8_t mask[4] = {0, 0, 0, 0};
    if ((h[1] & 0x80) && !readExact(fd, mask, 4)) break;
    if (len > 4096) break;  // the only client message we understand is a tiny {"depth":N}
    std::string pl(len, 0);
    if (len && !readExact(fd, pl.data(), len)) break;
    for (size_t i = 0; i < pl.size(); ++i) pl[i] = char(pl[i] ^ mask[i & 3]);
    const int op = h[0] & 0x0F;
    if (op == 1) {
      double d;
      if (jnum(pl, "depth", d)) gDepth = int(std::max<double>(kMinDepth, std::min<double>(kMaxDepth, d)));
    }
    if (op == 8) break;
    if (op == 9) { std::string pong = std::string(1, char(0x8A)) + char(0); std::lock_guard<std::mutex> lk(gWsM); sendAll(fd, pong.data(), pong.size()); }
  }
  {
    std::lock_guard<std::mutex> lk(gWsM);
    gWs.erase(std::remove(gWs.begin(), gWs.end(), fd), gWs.end());
  }
  closeSock(fd);
}
static void broadcaster() {
  int idle = 0;  // ticks (250 ms) with no viewer
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
      std::lock_guard<std::mutex> lk(gWsM);
      if (gWs.empty()) {
        if (gIdleStop && ++idle >= gIdleStop * 4) {
          std::lock_guard<std::mutex> ll(L.m);
          if (L.sim) L.sim = false, L.note = "simulator paused: nobody was watching";
        }
        continue;
      }
      idle = 0;
    }
    std::string f = wsFrame(snapshot(true));  // batched: one message per 250 ms, never per engine event
    std::lock_guard<std::mutex> lk(gWsM);
    for (int fd : gWs)
      if (!sendAll(fd, f.data(), f.size())) shutdown(fd, SHUT_RDWR);  // reader thread then cleans up
  }
}

// ---------------------------------------------------------------- connection handling
static void handle(int fd) {
  setTimeout(fd, SO_RCVTIMEO, 10, 0);
#ifdef SO_NOSIGPIPE  // macOS; Linux relies on signal(SIGPIPE, SIG_IGN) in main
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  std::string req;
  char buf[4096];
  size_t hend;
  while ((hend = req.find("\r\n\r\n")) == std::string::npos) {
    ssize_t k = rcv(fd, buf, sizeof buf);
    if (k <= 0 || req.size() > 65536) return void(closeSock(fd));
    req.append(buf, size_t(k));
  }
  std::string head = req.substr(0, hend), body = req.substr(hend + 4);
  std::istringstream hs(head);
  std::string method, target, ver;
  hs >> method >> target >> ver;
  std::map<std::string, std::string> hd;
  std::string line;
  std::getline(hs, line);
  while (std::getline(hs, line)) {
    size_t c = line.find(':');
    if (c == std::string::npos) continue;
    std::string k = line.substr(0, c), v = line.substr(c + 1);
    for (auto& ch : k) ch = char(std::tolower((unsigned char)ch));
    while (!v.empty() && (v[0] == ' ')) v.erase(0, 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    hd[k] = v;
  }
  size_t q = target.find('?');
  std::string path = target.substr(0, q), query = q == std::string::npos ? "" : target.substr(q + 1);

  if (path == "/ws" && hd.count("sec-websocket-key")) {
    std::string acc = b64(sha1(hd["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + "\r\n\r\n";
    sendAll(fd, r.data(), r.size());
    setTimeout(fd, SO_RCVTIMEO, 0, 0);
    setTimeout(fd, SO_SNDTIMEO, 0, 200000);
    {
      std::lock_guard<std::mutex> lk(gWsM);
      gWs.push_back(fd);
    }
    return wsReader(fd);
  }
  size_t want = hd.count("content-length") ? std::strtoul(hd["content-length"].c_str(), 0, 10) : 0;
  if (want > (1 << 20)) return err(fd, 400, "body too large"), void(closeSock(fd));
  while (body.size() < want) {
    ssize_t k = rcv(fd, buf, sizeof buf);
    if (k <= 0) return void(closeSock(fd));
    body.append(buf, size_t(k));
  }
  if (path.rfind("/api/", 0) == 0) handleApi(fd, method, path, query, body);
  else if (method == "GET") {
    std::string f = path == "/" ? "index.html" : path.substr(1);
    bool ok = false;
    std::string data;
    if (safeName(f)) data = readFile(gWeb + "/" + f, ok);
    if (ok) reply(fd, 200, ctypeOf(f), data);
    else err(fd, 404, "not found");
  } else err(fd, 404, "not found");
  closeSock(fd);
}

int main(int argc, char** argv) {
  int port = 8080;
  std::string host = "127.0.0.1";
  if (const char* pe = std::getenv("PORT")) port = std::atoi(pe);  // PaaS convention; --port overrides
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k == "--port") port = std::atoi(argv[i + 1]);
    else if (k == "--host") host = argv[i + 1];
    else if (k == "--idle-stop") gIdleStop = std::atoi(argv[i + 1]);
    else if (k == "--web") gWeb = argv[i + 1];
    else if (k == "--bench") gBenchBin = argv[i + 1];
    else if (k == "--results") gResults = argv[i + 1];
    else if (k == "--band-hi") gBand.hi = std::atoll(argv[i + 1]);
    else if (k == "--batch") gBatch = std::strtoull(argv[i + 1], 0, 10);
    else if (k == "--max-active") gMaxActive = std::strtoull(argv[i + 1], 0, 10);
    else { std::fprintf(stderr, "unknown flag %s\n", k.c_str()); return 2; }
  }
  if (gBand.hi <= gBand.lo || gBand.hi > (1 << 24) || gBatch < 64) { std::fprintf(stderr, "bad --band-hi (1..16777216) or --batch (>=64)\n"); return 2; }
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::fprintf(stderr, "WSAStartup failed\n"); return 1; }
#endif
  L.reset(Mode::Adaptive);
  int s = int(socket(AF_INET, SOCK_STREAM, 0));
#if !defined(_WIN32)  // on Windows SO_REUSEADDR would let another process steal the port
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
  sockaddr_in a{};
  a.sin_family = AF_INET, a.sin_port = htons(uint16_t(port)), a.sin_addr.s_addr = 0;
  if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) { std::fprintf(stderr, "bad --host (IPv4 address expected)\n"); return 2; }
  if (port < 1 || port > 65535) { std::fprintf(stderr, "bad --port\n"); return 2; }
  if (bind(s, (sockaddr*)&a, sizeof a) < 0 || listen(s, 64) < 0) { std::perror("bind/listen"); return 1; }
  std::thread(simLoop).detach();
  std::thread(broadcaster).detach();
  std::printf("CHRONOS gateway on http://%s:%d  (web=%s, bench=%s)\n", host.c_str(), port, gWeb.c_str(), gBenchBin.c_str());
  std::fflush(stdout);
  while (true) {
    int c = int(accept(s, nullptr, nullptr));
    if (c < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }  // e.g. EMFILE: do not spin
    if (gConn >= kMaxConn) { closeSock(c); continue; }  // refuse floods instead of spawning unbounded threads
    ++gConn;
    try {  // a failed thread start or an exception in a handler must drop one connection, never the server
      std::thread([c] {
        try { handle(c); } catch (...) { closeSock(c); }
        --gConn;
      }).detach();
    } catch (...) { closeSock(c); --gConn; }
  }
}
