// DATAMATICS -- composition files (.dmx): a small versioned text format.
//
//   DATAMATICS 1
//   bpm 120.0
//   opt <arrange> <end> <midi> <jump>
//   slot <i> <mode> <path...>          (path to end of line; "-" = empty)
//   stream <i> <P_COUNT ints>
//   section <type> <bars> <mask> <depth>
//
// Unknown lines are ignored, missing values keep their defaults, so later
// versions can add keys without breaking old files.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ws/dm_ws.h"

namespace dm {

namespace {
struct Out {
    char* p;
    uint32_t cap, n = 0;
    bool ok = true;
    void put(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
void Out::put(const char* fmt, ...) {
    if (!ok) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(p + n, cap - n, fmt, ap);
    va_end(ap);
    if (w < 0 || uint32_t(w) >= cap - n) {
        ok = false;
        return;
    }
    n += uint32_t(w);
}

bool nextInt(const char*& s, const char* end, int& v) {
    while (s < end && (*s == ' ' || *s == '\t')) ++s;
    if (s >= end) return false;
    bool neg = false;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        ++s;
    }
    if (s >= end || *s < '0' || *s > '9') return false;
    long x = 0;
    while (s < end && *s >= '0' && *s <= '9') {
        x = x * 10 + (*s - '0');
        if (x > 100000000) x = 100000000;
        ++s;
    }
    v = int(neg ? -x : x);
    return true;
}

bool startsWith(const char* s, const char* end, const char* kw) {
    const size_t n = std::strlen(kw);
    return size_t(end - s) > n && !std::strncmp(s, kw, n) && (s[n] == ' ' || s[n] == '\t');
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

uint32_t writePiece(const Engine& e, float bpm, char* out, uint32_t cap) {
    Out o{out, cap};
    o.put("DATAMATICS %d\n", kPieceVersion);
    const int b10 = int(bpm * 10.0f + 0.5f);
    o.put("bpm %d.%d\n", b10 / 10, b10 % 10);
    o.put("opt %d %d %d %d %d\n", e.arrange, e.endMode, e.midiMode, e.quant, e.songMode);
    for (int i = 0; i < kSlots; ++i)
        o.put("slot %d %d %s\n", i, int(e.slot[i].wantMode), e.slot[i].path[0] ? e.slot[i].path : "-");
    for (int s = 0; s < kStreams; ++s) {
        o.put("stream %d", s);
        for (int p = 0; p < P_COUNT; ++p) o.put(" %d", e.st[s].p[p]);
        o.put("\n");
    }
    for (int i = 0; i < e.secCount; ++i)
        o.put("section %d %d %d %d\n", e.sec[i].type, e.sec[i].bars, e.sec[i].mask, e.sec[i].depth);
    o.put("end\n");
    return o.ok ? o.n : 0;
}

bool readPiece(Engine& e, const char* text, uint32_t len, float* bpmOut) {
    const char* s = text;
    const char* end = text + len;
    int version = 1;
    // header
    if (len < 12 || std::strncmp(text, "DATAMATICS ", 11) != 0) return false;
    {
        const char* q = text + 11;
        int ver = 0;
        if (!nextInt(q, end, ver) || ver < 1) return false;
        version = ver;
    }
    int nsec = 0;
    Section secs[kMaxSections];
    bool sawSection = false;
    while (s < end) {
        const char* eol = s;
        while (eol < end && *eol != '\n') ++eol;
        const char* le = eol;
        if (le > s && le[-1] == '\r') --le;
        const char* q;
        if (startsWith(s, le, "bpm")) {
            q = s + 3;
            int whole = 0;
            if (nextInt(q, le, whole) && bpmOut) {
                float f = float(whole);
                if (q < le && *q == '.' && q + 1 < le && q[1] >= '0' && q[1] <= '9') f += float(q[1] - '0') * 0.1f;
                *bpmOut = f;
            }
        } else if (startsWith(s, le, "opt")) {
            q = s + 3;
            int v;
            if (nextInt(q, le, v)) e.arrange = clampi(v, 0, 1);
            if (nextInt(q, le, v)) e.endMode = clampi(v, 0, 1);
            if (nextInt(q, le, v)) e.midiMode = clampi(v, 0, 2);
            if (nextInt(q, le, v)) {
                if (version < 2) e.quant = v ? Q_NOW : Q_BAR;   // v1: 0 next bar, 1 immediate
                else e.quant = clampi(v, 0, Q_COUNT - 1);
            }
            if (nextInt(q, le, v)) e.songMode = clampi(v, 0, 1);
            else if (version < 2) e.songMode = 1;   // v1 pieces played their sections in order
        } else if (startsWith(s, le, "slot")) {
            q = s + 4;
            int i = 0, m = 0;
            if (nextInt(q, le, i) && nextInt(q, le, m) && i >= 0 && i < kSlots) {
                while (q < le && (*q == ' ' || *q == '\t')) ++q;
                Slot& sl = e.slot[i];
                sl.wantMode = uint8_t(clampi(m, 0, kModeCount - 1));
                size_t n = size_t(le - q);
                if (n == 1 && *q == '-') n = 0;
                if (n >= sizeof sl.path) n = sizeof sl.path - 1;
                std::memcpy(sl.path, q, n);
                sl.path[n] = 0;
            }
        } else if (startsWith(s, le, "stream")) {
            q = s + 6;
            int i = 0;
            if (nextInt(q, le, i) && i >= 0 && i < kStreams) {
                for (int p = 0; p < P_COUNT; ++p) {
                    int v;
                    if (!nextInt(q, le, v)) break;
                    const ParamInfo& pi = paramInfo(p);
                    e.st[i].p[p] = clampi(v, pi.min, pi.max);
                }
            }
        } else if (startsWith(s, le, "section")) {
            q = s + 7;
            Section c;
            int v;
            sawSection = true;
            if (nextInt(q, le, v)) c.type = clampi(v, 0, T_COUNT - 1);
            if (nextInt(q, le, v)) c.bars = clampi(v, 1, 64);
            if (nextInt(q, le, v)) c.mask = clampi(v, 0, 255);
            if (nextInt(q, le, v)) c.depth = clampi(v, 0, 100);
            if (nsec < kMaxSections) secs[nsec++] = c;
        }
        s = eol < end ? eol + 1 : end;
    }
    if (sawSection && nsec > 0) {
        for (int i = 0; i < kMaxSections; ++i) e.sec[i] = i < nsec ? secs[i] : Section();
        e.secCount = nsec;
    }
    if (e.startSec >= e.secCount) e.startSec = 0;
    return true;
}

}  // namespace dm
