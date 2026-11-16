#pragma once
// Replays a LOBSTER-format message file (headerless CSV: time, type, order id, size, price*10000, direction).
// Format as I understand LOBSTER's documentation; NOT yet verified against a real downloaded file (see docs/TESTING.md).
// Inputs only: an execution (type 4) is replayed as a market order from the opposite side and our own matching
// reproduces the trade; types 5-7 (hidden, cross, halt) are ignored. Partial cancels become a modify, which loses
// queue priority (small deviation from the real book). Prices are rebased to the middle of the band, in cents.
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "types.hpp"

namespace chronos {

struct LobsterStats {
  size_t lines = 0, submits = 0, cancels = 0, partials = 0, execs = 0, ignored = 0, outOfBand = 0, bad = 0;
};

inline std::vector<Cmd> parseLobster(std::FILE* f, Band band, LobsterStats& st) {
  struct Live { Qty rem; Price px; Side side; };
  std::unordered_map<uint64_t, Live> live;
  std::vector<Cmd> out;
  Price base = 0;
  bool haveBase = false;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    double t;
    int type, dir;
    long long id, size, px;
    if (std::sscanf(line, "%lf,%d,%lld,%lld,%lld,%d", &t, &type, &id, &size, &px, &dir) != 6 || size <= 0 || id < 0 || (dir != 1 && dir != -1)) { ++st.bad; continue; }
    ++st.lines;
    Cmd c;
    c.ts = uint64_t(t * 1e6), c.id = Id(id) + 1;  // id 0 is reserved by the engine
    c.side = dir == 1 ? Side::Buy : Side::Sell;
    Price tick = Price(px / 100);  // 1e-4 dollar units -> cents
    if (type == 1 || type == 2 || type == 4) {
      if (!haveBase) base = tick - (band.lo + band.hi) / 2, haveBase = true;
      tick -= base;
    }
    switch (type) {
      case 1:
        if (tick < band.lo || tick >= band.hi) { ++st.outOfBand; break; }
        c.kind = Kind::New, c.type = Type::Limit, c.price = tick, c.qty = size;
        live[c.id] = {size, tick, c.side}, out.push_back(c), ++st.submits;
        break;
      case 3: {
        if (!live.erase(c.id)) { ++st.ignored; break; }  // order from before the file starts, or out of band
        c.kind = Kind::Cancel, out.push_back(c), ++st.cancels;
        break;
      }
      case 2: {
        auto it = live.find(c.id);
        if (it == live.end()) { ++st.ignored; break; }
        it->second.rem -= size;
        if (it->second.rem <= 0) c.kind = Kind::Cancel, live.erase(it);
        else c.kind = Kind::Modify, c.type = Type::Limit, c.side = it->second.side, c.price = it->second.px, c.qty = it->second.rem;
        out.push_back(c), ++st.partials;
        break;
      }
      case 4: {
        auto it = live.find(c.id);
        if (it == live.end()) { ++st.ignored; break; }
        Side aggressor = it->second.side == Side::Buy ? Side::Sell : Side::Buy;
        it->second.rem -= size;
        if (it->second.rem <= 0) live.erase(it);
        c.kind = Kind::New, c.type = Type::Market, c.side = aggressor, c.id = Id(1) << 50 | out.size(), c.qty = size;  // fresh id: the resting order keeps its own
        out.push_back(c), ++st.execs;
        break;
      }
      default: ++st.ignored;
    }
  }
  return out;
}

inline std::vector<Cmd> loadLobster(const std::string& path, Band band, LobsterStats& st) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return {};
  auto v = parseLobster(f, band, st);
  std::fclose(f);
  return v;
}

}  // namespace chronos
