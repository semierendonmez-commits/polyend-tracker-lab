// DATAMATICS -- audio engine (dm_engine.h).
#include "ws/dm_engine.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

#include "fastrun.h"

namespace dm {

namespace {

const char* const kSlotNames[] = {"S1", "S2", "S3", "S4"};
const char* const kUnitNames[] = {"BYTE", "BIT"};
const char* const kOrderNames[] = {"FWD", "REV", "PING", "COL", "HOP"};
const char* const kSyncNames[] = {"MS", "BEAT"};
const char* const kSoundNames[] = {"SINE", "CLICK", "NOISE", "IMPULSE"};
const char* const kScaleNames[] = {"FREE", "HARM", "OCT", "12TET"};
const char* const kShapeNames[] = {"RECT", "FADE", "DECAY"};
const char* const kPanNames[] = {"FIX", "DATA", "BIT", "ALT", "SWEEP"};
const char* const kStemNames[] = {"ST1", "ST2", "ST3", "ST4"};
const char* const kOffOn[] = {"OFF", "ON"};
const char* const kLenNames[] = {"ALL", "1", "2", "3", "4", "5", "6", "7", "8", "12", "16", "24", "32", "48", "64",
                                 "96", "128", "192", "256", "384", "512", "768", "1K", "2K", "4K", "8K", "16K", "32K", "64K"};
const uint32_t kLenVals[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
                             1024, 2048, 4096, 8192, 16384, 32768, 65536};
const char* const kDivNames[kRateDivs] = {"1/128", "1/64", "1/48", "1/32", "1/24", "1/16", "1/12",
                                         "1/8",   "1/6",  "1/4",  "1/2",  "1/1",  "2/1",  "4/1"};
// Step length in beats as num / den (exact integer grids).
const uint8_t kDivNum[kRateDivs] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 4, 8, 16};
const uint8_t kDivDen[kRateDivs] = {32, 16, 12, 8, 6, 4, 3, 2, 3, 1, 1, 1, 1, 1};

const ParamInfo kParams[P_COUNT] = {
    {"SLOT", 0, 3, 0, kSlotNames},     {"UNIT", 0, 1, 0, kUnitNames},     {"ORDER", 0, 4, 0, kOrderNames},
    {"START", 0, 999, 0, nullptr},     {"LENGTH", 0, 28, 0, kLenNames},   {"STRIDE", 1, 64, 1, nullptr},
    {"GRID", 0, 1, 0, kSyncNames},     {"RATE", 1, 2000, 50, nullptr},    {"DIV", 0, kRateDivs - 1, 5, kDivNames},
    {"SOUND", 0, 3, 0, kSoundNames},   {"THRESH", 0, 255, 128, nullptr},  {"LOW", 1, 21000, 220, nullptr},
    {"HIGH", 1, 21000, 3520, nullptr}, {"SCALE", 0, 3, 0, kScaleNames},   {"DUR", 1, 20000, 100, nullptr},
    {"DURMOD", 0, 100, 0, nullptr},    {"SHAPE", 0, 2, 0, kShapeNames},   {"LEVEL", 0, 100, 70, nullptr},
    {"VEL", 0, 100, 0, nullptr},       {"PANMODE", 0, 4, 0, kPanNames},   {"PAN", -100, 100, 0, nullptr},
    {"STEM", 0, 3, 0, kStemNames},     {"MIDI", 0, 1, 1, kOffOn},         {"MUTE", 0, 1, 0, kOffOn},
    {"SOLO", 0, 1, 0, kOffOn},
};

const char* const kThrowNames[kThrows] = {"CUT", "FREEZE", "STUTTER", "DENSE", "THIN", "REVERSE", "INVERT", "OCT+"};
const char* const kQuantNames[Q_COUNT] = {"NOW", "BEAT", "BAR", "2 BARS", "4 BARS"};
const char* const kSecNames[T_COUNT] = {"PLAY", "SCAN", "DENSIFY", "THIN", "SILENCE", "CUT", "FREEZE"};
const char* const kSecShort[T_COUNT] = {"PLY", "SCN", "DNS", "THN", "SIL", "CUT", "FRZ"};

// Sine table, 1024 + 1 entries.
constexpr int kSinBits = 10;
float gSin[(1 << kSinBits) + 1];
bool gSinInit = false;
void initSin() {
    if (gSinInit) return;
    for (int i = 0; i <= (1 << kSinBits); ++i) gSin[i] = sinf(6.28318531f * float(i) / float(1 << kSinBits));
    gSinInit = true;
}
inline float sinPh(uint32_t ph) {
    const uint32_t i = ph >> (32 - kSinBits);
    const float f = float(ph & ((1u << (32 - kSinBits)) - 1u)) * (1.0f / float(1u << (32 - kSinBits)));
    const float a = gSin[i];
    return a + (gSin[i + 1] - a) * f;
}

inline uint32_t xs(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

constexpr uint64_t kOne = 65536ull;   // Q16 sample

}  // namespace

const ParamInfo& paramInfo(int p) { return kParams[p < 0 ? 0 : (p >= P_COUNT ? P_COUNT - 1 : p)]; }
float rateDivBeats(int i) {
    i = i < 0 ? 0 : (i >= kRateDivs ? kRateDivs - 1 : i);
    return float(kDivNum[i]) / float(kDivDen[i]);
}
uint64_t beatsQ(uint32_t num, uint32_t den, float bpm) {
    // num/den beats in Q16 samples, integer math: 60 * 44100 * 65536 * 100 / (den * bpm * 100)
    uint32_t b100 = uint32_t(bpm * 100.0f + 0.5f);
    if (b100 < 100) b100 = 100;
    return (uint64_t(num) * 60ull * 44100ull * 65536ull * 100ull) / (uint64_t(den) * b100);
}
const char* rateDivName(int i) { return kDivNames[i < 0 ? 0 : (i >= kRateDivs ? kRateDivs - 1 : i)]; }
const char* throwName(int i) { return (i >= 0 && i < kThrows) ? kThrowNames[i] : "?"; }
const char* quantName(int q) { return (q >= 0 && q < Q_COUNT) ? kQuantNames[q] : "?"; }
const char* secTypeName(int t) { return (t >= 0 && t < T_COUNT) ? kSecNames[t] : "?"; }
const char* secTypeShort(int t) { return (t >= 0 && t < T_COUNT) ? kSecShort[t] : "?"; }

uint32_t unitCount(const Slot& s, int unit) { return unit == U_BIT ? s.n * 8u : s.n; }

uint32_t readIndex(const Stream& st, const Slot& slot, uint32_t k, uint32_t scan) {
    const int* p = st.p;
    const uint32_t total = unitCount(slot, p[P_UNIT]);
    if (!total) return 0;
    const uint32_t start = uint32_t((uint64_t(total) * uint32_t(p[P_START])) / 1000u);
    uint32_t len = kLenVals[p[P_LEN] < 0 ? 0 : (p[P_LEN] > 28 ? 28 : p[P_LEN])];
    const uint32_t room = total - (start < total ? start : total - 1);
    if (!len || len > room) len = room;
    if (!len) len = 1;
    const uint32_t kk = k * uint32_t(p[P_STRIDE] < 1 ? 1 : p[P_STRIDE]);
    uint32_t j;
    switch (p[P_ORDER]) {
        case O_REV: j = len - 1 - (kk % len); break;
        case O_PING: {
            if (len < 2) { j = 0; break; }
            const uint32_t per = 2 * len - 2;
            const uint32_t q = kk % per;
            j = q < len ? q : per - q;
            break;
        }
        case O_COL: {
            uint32_t w = p[P_UNIT] == U_BIT ? uint32_t(slot.width) * 8u : slot.width;
            if (!w || w >= len) { j = kk % len; break; }
            const uint32_t rows = len / w;
            const uint32_t area = rows * w;
            const uint32_t q = kk % area;
            j = (q % rows) * w + q / rows;
            break;
        }
        case O_HOP: j = uint32_t((uint64_t(kk) * 2654435761ull) % len); break;
        default: j = kk % len; break;
    }
    return start + ((j + scan) % len);
}

void readValue(const Slot& slot, int unit, uint32_t idx, uint8_t& val, uint8_t& aux) {
    const uint32_t n = slot.n;
    if (!n) { val = aux = 0; return; }
    if (unit == U_BIT) {
        const uint32_t i = (idx >> 3) % n;
        const uint8_t b = slot.v[i];
        val = ((b >> (7 - (idx & 7u))) & 1u) ? 255 : 0;
        aux = b;
    } else {
        const uint32_t i = idx % n;
        val = slot.v[i];
        aux = slot.aux ? slot.aux[i] : val;
    }
}

float mapFreq(const int* p, uint8_t val) {
    float lo = float(p[P_LO]), hi = float(p[P_HI]);
    if (lo < 1.0f) lo = 1.0f;
    if (hi < 1.0f) hi = 1.0f;
    const float t = float(val) * (1.0f / 255.0f);
    switch (p[P_SCALE]) {
        case SC_HARM: {   // harmonics of LOW up to HIGH
            const float nmax = hi > lo ? floorf(hi / lo) : 1.0f;
            return lo * (1.0f + floorf(t * (nmax - 1.0f) + 0.5f));
        }
        case SC_OCT: {
            const float oct = hi > lo ? log2f(hi / lo) : 0.0f;
            return lo * exp2f(floorf(t * oct + 0.5f));
        }
        case SC_12TET: {
            const float semis = hi > lo ? 12.0f * log2f(hi / lo) : 0.0f;
            return lo * exp2f(floorf(t * semis + 0.5f) * (1.0f / 12.0f));
        }
        default: return lo * exp2f(t * log2f(hi / lo));
    }
}

bool Engine::init(lab::Arena& a) {
    initSin();
    for (int i = 0; i < kSlots; ++i) {
        slot[i].v = static_cast<uint8_t*>(a.alloc(kMaxValues, 32));
        slot[i].aux = static_cast<uint8_t*>(a.alloc(kMaxValues, 32));
        if (!slot[i].v || !slot[i].aux) return false;
    }
    for (int s = 0; s < kStreams; ++s)
        for (int v = 0; v < kVoices; ++v) v_[s][v].left = -1;
    defaults();
    return true;
}

void Engine::defaults() {
    for (int s = 0; s < kStreams; ++s)
        for (int p = 0; p < P_COUNT; ++p) st[s].p[p] = kParams[p].def;
    auto set = [&](int s, std::initializer_list<int> kv) {
        const int* x = kv.begin();
        for (size_t i = 0; i + 1 < kv.size(); i += 2) st[s].p[x[i]] = x[i + 1];
    };
    // A demo piece on the built-in data sets (slots: TESTCARD, PRIMES, LFSR, CHIRP).
    set(0, {P_SLOT, 1, P_SYNC, 0, P_RATE_MS, 30, P_SOUND, S_CLICK, P_DUR, 3, P_THR, 128, P_PANMODE, PM_ALT, P_PAN, 60,
            P_LEVEL, 80, P_STEM, 0});
    set(1, {P_SLOT, 0, P_SYNC, 1, P_RATE_DIV, 3, P_SOUND, S_SINE, P_LO, 2000, P_HI, 12000, P_DUR, 80, P_THR, 160,
            P_PANMODE, PM_SWEEP, P_PAN, 100, P_LEVEL, 45, P_STEM, 1});
    set(2, {P_SLOT, 2, P_SYNC, 1, P_RATE_DIV, 9, P_SOUND, S_SINE, P_LO, 38, P_HI, 76, P_SCALE, SC_OCT, P_DUR, 2400,
            P_SHAPE, SH_FADE, P_THR, 140, P_LEVEL, 90, P_STEM, 2});
    set(3, {P_SLOT, 2, P_UNIT, U_BIT, P_SYNC, 0, P_RATE_MS, 5, P_SOUND, S_IMPULSE, P_THR, 128, P_PANMODE, PM_DATA,
            P_PAN, 100, P_LEVEL, 60, P_STEM, 0});
    set(4, {P_SLOT, 3, P_SYNC, 1, P_RATE_DIV, 5, P_SOUND, S_SINE, P_LO, 220, P_HI, 1760, P_SCALE, SC_12TET, P_DUR, 200,
            P_THR, 0, P_VEL, 60, P_LEVEL, 40, P_STEM, 1});
    set(5, {P_SLOT, 0, P_SYNC, 1, P_RATE_DIV, 5, P_ORDER, O_COL, P_SOUND, S_NOISE, P_LO, 4000, P_HI, 18000, P_DUR, 20,
            P_THR, 128, P_PANMODE, PM_BIT, P_PAN, 100, P_LEVEL, 50, P_STEM, 3});
    set(6, {P_SLOT, 1, P_SYNC, 1, P_RATE_DIV, 7, P_SOUND, S_SINE, P_LO, 17000, P_HI, 19000, P_DUR, 400, P_THR, 128,
            P_LEVEL, 20, P_STEM, 3, P_MIDI, 0});
    set(7, {P_SLOT, 1, P_SYNC, 0, P_RATE_MS, 1, P_ORDER, O_HOP, P_SOUND, S_IMPULSE, P_THR, 128, P_PANMODE, PM_DATA,
            P_PAN, 80, P_LEVEL, 50, P_STEM, 0, P_MIDI, 0});
    const Section demo[] = {
        {T_PLAY, 4, 0xFF, 50},  {T_DENSIFY, 8, 0xFF, 80}, {T_SCAN, 4, 0x32, 100}, {T_CUT, 4, 0xFF, 50},
        {T_SILENCE, 1, 0x00, 0}, {T_FREEZE, 2, 0xFF, 70}, {T_THIN, 8, 0xFF, 60},  {T_PLAY, 4, 0x89, 50},
    };
    secCount = int(sizeof demo / sizeof demo[0]);
    for (int i = 0; i < kMaxSections; ++i) sec[i] = i < secCount ? demo[i] : Section();
    arrange = 1;
    endMode = 0;
    startSec = 0;
    songMode = 0;
    quant = Q_BAR;
    for (int i = 0; i < kSlots; ++i) scrub[i] = 0.0f;
    stutterDiv = 3;
    freezeWin = 4;
    throwDepth = 100;
    throws = 0;
    fillMask = 0;
    slotSolo = 0;
}

bool Engine::anySolo() const {
    for (int s = 0; s < kStreams; ++s)
        if (st[s].p[P_SOLO]) return true;
    return false;
}

bool Engine::streamAudible(int s) const {
    if (st[s].p[P_MUTE]) return false;
    const uint32_t ss = slotSolo;
    if (ss && !((ss >> (st[s].p[P_SLOT] & 3)) & 1u)) return false;
    return anySolo() ? st[s].p[P_SOLO] != 0 : true;
}

uint64_t Engine::stepQ(int s, float bpm) const {
    const int* p = st[s].p;
    if (p[P_SYNC] == 0) {
        const uint32_t ms = uint32_t(p[P_RATE_MS] < 1 ? 1 : p[P_RATE_MS]);
        return uint64_t(ms) * 44100ull * kOne / 1000ull;   // exact: ms grid
    }
    int d = p[P_RATE_DIV];
    d = d < 0 ? 0 : (d >= kRateDivs ? kRateDivs - 1 : d);
    const uint64_t q = beatsQ(kDivNum[d], kDivDen[d], bpm);
    return q < kOne ? kOne : q;
}

uint32_t Engine::voicesActive() const {
    uint32_t n = 0;
    for (int s = 0; s < kStreams; ++s)
        for (int v = 0; v < kVoices; ++v)
            if (v_[s][v].left > 0) ++n;
    return n;
}

void Engine::killStream(int s) {
    for (int v = 0; v < kVoices; ++v) v_[s][v].left = -1;
}
void Engine::killAll() {
    for (int s = 0; s < kStreams; ++s) killStream(s);
}

void Engine::midiOff(int s) {
    if (lastNote_[s] && ctx_) {
        ctx_->midiNote(false, uint8_t(lastNote_[s] - 1), 0, uint8_t(midiMode == 2 ? s : 0));
        lastNote_[s] = 0;
    }
}

int Engine::totalBars() const {
    int b = 0;
    for (int i = 0; i < secCount; ++i) b += sec[i].bars;
    return b;
}

bool Engine::sectionAllows(int s) const {
    if (!arrange) return true;
    const Section& c = sec[curSec];
    if (c.type == T_SILENCE) return false;
    if (!((c.mask >> s) & 1)) return false;
    if (c.type == T_CUT && !gateOpen) return false;
    if (c.type == T_DENSIFY || c.type == T_THIN) {
        int m = 0, r = 0;
        for (int i = 0; i < kStreams; ++i)
            if ((c.mask >> i) & 1) {
                if (i < s) ++r;
                ++m;
            }
        const float pr = secProgress * float(m);
        return c.type == T_DENSIFY ? pr >= float(r) : pr < float(m - r);
    }
    return true;
}

int Engine::thrFor(int s) const {
    if ((fillMask >> s) & 1u) return 0;
    int thr = st[s].p[P_THR];
    if (arrange) {
        const Section& c = sec[curSec];
        const float d = float(c.depth) * 0.01f * secProgress;
        if (c.type == T_DENSIFY) thr = int(float(thr) * (1.0f - d));
        else if (c.type == T_THIN) thr = thr + int(float(256 - thr) * d);
    }
    const uint32_t th = throws;
    const float td = float(throwDepth) * 0.01f;
    if (th & TH_DENSE) thr = int(float(thr) * (1.0f - td));
    if (th & TH_THIN) thr = thr + int(float(256 - thr) * td);
    return thr;
}

uint32_t Engine::unitsOffset(int s) const {
    const Stream& x = st[s];
    const int sl = x.p[P_SLOT] & 3;
    float sc = scrub[sl];
    if (!(sc > 0.0f)) return 0;
    if (sc > 0.9999f) sc = 0.9999f;
    const uint32_t total = unitCount(slot[sl], x.p[P_UNIT]);
    return uint32_t(float(total) * sc);
}

uint32_t Engine::boundaryAfter(int q) const {
    static const uint8_t num[Q_COUNT] = {0, 1, 4, 8, 16};
    const uint32_t c = clock;
    if (q <= Q_NOW || q >= Q_COUNT) return c;
    const uint32_t len = uint32_t(beatsQ(num[q], 1, bpm_) >> 16);
    if (!len) return c;
    return ((c + len - 1) / len) * len;
}

uint32_t Engine::scanOffset(int s) const {
    const uint32_t off = unitsOffset(s);
    if (!arrange || sec[curSec].type != T_SCAN) return off;
    const Stream& x = st[s];
    const Slot& sl = slot[x.p[P_SLOT] & 3];
    const uint32_t total = unitCount(sl, x.p[P_UNIT]);
    return off + uint32_t(float(total) * secProgress * float(sec[curSec].depth) * 0.01f);
}

float Engine::panFor(int s, uint8_t val, uint8_t aux, uint32_t k) {
    const int* p = st[s].p;
    const float w = float(p[P_PAN]) * 0.01f;
    switch (p[P_PANMODE]) {
        case PM_DATA: return (float(aux) * (2.0f / 255.0f) - 1.0f) * w;
        case PM_BIT: return ((aux ^ val) & 1u) ? w : -w;
        case PM_ALT: altPan_[s] ^= 1; return altPan_[s] ? w : -w;
        case PM_SWEEP: {
            const uint32_t per = 64;
            return (float(k % per) * (2.0f / float(per - 1)) - 1.0f) * w;
        }
        default: return w;
    }
}

void Engine::fire(int s, int at, float velScale) {
    (void)at;
    Stream& x = st[s];
    const int* p = x.p;
    const Slot& sl = slot[p[P_SLOT] & 3];
    const bool audition = velScale > 0.0f;
    if (!sl.n) {
        if (!audition) x.k = x.k + 1;
        return;
    }
    const uint32_t th = audition ? 0u : throws;
    uint32_t k = x.k;
    if (audition) k = k + x.audOff;   // audition: the unit audOff steps ahead of the read head
    else if (th & TH_STUTTER) k = stutK_[s];   // stutter: the same unit again
    const uint32_t idx = readIndex(x, sl, k, scanOffset(s));
    uint8_t val, aux;
    readValue(sl, p[P_UNIT], idx, val, aux);
    if (!audition && !(th & TH_STUTTER)) {
        uint32_t nk = (th & TH_REVERSE) ? k - 1u : k + 1u;
        if (arrange && sec[curSec].type == T_FREEZE) {
            const int32_t win = 1 + (100 - sec[curSec].depth) / 6;
            int32_t d = int32_t(nk - freezeK_[s]) % win;
            if (d < 0) d += win;
            nk = freezeK_[s] + uint32_t(d);
        }
        if (th & TH_FREEZE) {
            const int32_t win = freezeWin < 1 ? 1 : freezeWin;
            int32_t d = int32_t(nk - thFreezeK_[s]) % win;
            if (d < 0) d += win;
            nk = thFreezeK_[s] + uint32_t(d);
        }
        x.k = nk;
    }
    srcData[s] = float(val) * (1.0f / 255.0f);
    x.lastIdx = idx;
    x.lastVal = val;
    x.lastAux = aux;
    if (!audition) {
        if ((th & TH_CUT) || !sectionAllows(s) || !streamAudible(s)) return;
        const int thr = thrFor(s);
        bool hit = !(int(val) < thr || (thr > 0 && val == 0));
        if (th & TH_INVERT) hit = !hit;
        if (!hit) return;
    }
    // amplitude
    const float lvl = float(p[P_LEVEL]) * 0.01f;
    const float vd = float(p[P_VEL]) * 0.01f;
    float amp = lvl * lvl * (1.0f - vd + vd * float(aux) * (1.0f / 255.0f));
    if (audition) amp *= velScale;
    if (amp <= 0.0f) return;
    // duration (samples)
    const float dm = float(p[P_DURMOD]) * 0.01f;
    const float dk = 1.0f - dm + dm * float(aux) * (2.0f / 255.0f);
    int32_t dur = int32_t(float(p[P_DUR]) * 4.41f * dk);
    if (dur < 1) dur = 1;
    // pan
    float pan = panFor(s, val, aux, k);
    pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
    const uint32_t pph = uint32_t((pan + 1.0f) * 536870912.0f);   // 0..2^30 = 0..1/4 cycle, exact at the ends
    const float gl = sinPh(pph + 0x40000000u), gr = sinPh(pph);   // cos, sin
    const float hz = mapFreq(p, val) * ((th & TH_OCT) ? 2.0f : 1.0f);

    Voice& v = v_[s][rr_[s]];
    rr_[s] = uint8_t((rr_[s] + 1) % kVoices);
    v.type = uint8_t(p[P_SOUND]);
    v.shape = uint8_t(p[P_SHAPE]);
    v.total = v.left = v.type == S_IMPULSE ? 1 : dur;
    v.age = 0;
    v.ph = 0;
    float f = hz < 22000.0f ? hz : 22000.0f;
    v.inc = uint32_t(f * (4294967296.0f / 44100.0f));
    v.gl = gl * amp;
    v.gr = gr * amp;
    v.stem = p[P_STEM] & 3;
    v.sign = (k & 1u) ? -1.0f : 1.0f;
    if (v.type == S_CLICK || v.type == S_IMPULSE) v.sign = (val & 1u) ? -1.0f : 1.0f;
    v.lp = 0.0f;
    float a = 1.0f - expf(-6.2831853f * f * (1.0f / 44100.0f));
    v.lpa = a > 1.0f ? 1.0f : a;
    if (v.shape == SH_DECAY) {
        v.env = 1.0f;
        v.k = expf(-6.9f / float(v.total));
    } else if (v.shape == SH_FADE) {
        v.env = 0.0f;
    } else {
        v.env = 1.0f;
    }
    x.events = x.events + 1;
    ++evBlock_;
    x.lastHz = hz;
    x.lastAmp = amp;
    EventRec e;
    e.stream = uint8_t(s);
    e.val = val;
    e.aux = aux;
    e.sound = v.type;
    e.idx = idx;
    e.hz = hz;
    if (!events.push(e)) eventsLost = eventsLost + 1;

    // MIDI out only for streams of 10 ms steps or slower (DIN carries ~1000 notes/s).
    if (midiMode && p[P_MIDI] && ctx_ && !audition && stepQ(s, bpm_) >= 441ull * kOne) {
        midiOff(s);
        int note;
        if (v.type == S_SINE) {
            note = int(69.0f + 12.0f * log2f((hz > 8.2f ? hz : 8.2f) / 440.0f) + 0.5f);
            note = note < 0 ? 0 : (note > 127 ? 127 : note);
        } else {
            note = 36 + s;
        }
        int vel = int(amp * 127.0f / (lvl * lvl > 0.01f ? lvl * lvl : 1.0f));
        vel = vel < 1 ? 1 : (vel > 127 ? 127 : vel);
        ctx_->midiNote(true, uint8_t(note), uint8_t(vel), uint8_t(midiMode == 2 ? s : 0));
        lastNote_[s] = uint8_t(note + 1);
    }
}

// Render stream s's voices for samples [a, b) of the block into its stem buffers.
OT_FASTRUN_FN void Engine::renderVoices(int s, int a, int b) {
    for (int vi = 0; vi < kVoices; ++vi) {
        Voice& v = v_[s][vi];
        if (v.left <= 0) continue;
        float* L = stL_[v.stem];
        float* R = stR_[v.stem];
        int i = a;
        while (i < b && v.left > 0) {
            int run = b - i;
            if (run > v.left) run = v.left;
            float e = v.env, de = 0.0f, ke = 1.0f;
            if (v.shape == SH_FADE) {
                int ramp = v.total / 2;
                if (ramp > 44) ramp = 44;
                if (ramp < 1) ramp = 1;
                if (v.age < ramp) {
                    if (run > ramp - v.age) run = ramp - v.age;
                    de = 1.0f / float(ramp);
                } else if (v.left <= ramp) {
                    de = -1.0f / float(ramp);
                } else {
                    if (run > v.left - ramp) run = v.left - ramp;
                    e = 1.0f;
                }
            } else if (v.shape == SH_DECAY) {
                ke = v.k;
            }
            const float gl = v.gl, gr = v.gr;
            float* l = L + i;
            float* r = R + i;
            switch (v.type) {
                case S_SINE: {
                    uint32_t ph = v.ph;
                    const uint32_t inc = v.inc;
                    for (int j = 0; j < run; ++j) {
                        const float x = sinPh(ph) * e;
                        ph += inc;
                        e = e * ke + de;
                        l[j] += x * gl;
                        r[j] += x * gr;
                    }
                    v.ph = ph;
                    break;
                }
                case S_CLICK: {
                    const float sg = v.sign;
                    for (int j = 0; j < run; ++j) {
                        const float x = sg * e;
                        e = e * ke + de;
                        l[j] += x * gl;
                        r[j] += x * gr;
                    }
                    break;
                }
                case S_NOISE: {
                    uint32_t rs = rng_;
                    float lp = v.lp;
                    const float la = v.lpa;
                    for (int j = 0; j < run; ++j) {
                        const float n = float(int32_t(xs(rs))) * (1.0f / 2147483648.0f);
                        lp += la * (n - lp);
                        const float x = lp * e;
                        e = e * ke + de;
                        l[j] += x * gl;
                        r[j] += x * gr;
                    }
                    rng_ = rs;
                    v.lp = lp;
                    break;
                }
                default: {   // single-sample impulse
                    l[0] += v.sign * gl;
                    r[0] += v.sign * gr;
                    break;
                }
            }
            v.env = e;
            v.age += run;
            v.left -= run;
            i += run;
        }
        if (v.left == 0) v.left = -1;
    }
}

void Engine::startPlay() {
    playSerial = playSerial + 1;
    now_ = 0;
    clock = 0;
    for (int s = 0; s < kStreams; ++s) {
        next_[s] = 0;
        if (st[s].cue < 0) st[s].k = 0;
        altPan_[s] = 0;
    }
    int ss = startSec;
    if (ss < 0 || ss >= secCount) ss = 0;
    enterSection(ss, 0);
}

void Engine::enterSection(int i, uint32_t) {
    const int prev = curSec;
    (void)prev;
    curSec = i;
    secStartQ_ = now_;
    secProgress = 0.0f;
    secBarsDone = 0;
    gateOpen = 1;
    cutK_ = 0;
    cutNextQ_ = now_;
    const Section& c = sec[i];
    // hard cut at every boundary: streams not in the new section stop at once
    for (int s = 0; s < kStreams; ++s) {
        if (!arrange) break;
        if (c.type == T_SILENCE || !((c.mask >> s) & 1)) {
            killStream(s);
            midiOff(s);
        }
        freezeK_[s] = st[s].k;
    }
}

OT_FASTRUN_FN void Engine::render(const lab::Transport& t, float* l, float* r, float (*stemL)[lab::kBlock],
                                  float (*stemR)[lab::kBlock]) {
    stL_ = stemL;
    stR_ = stemR;
    for (int i = 0; i < 4; ++i) {
        std::memset(stemL[i], 0, sizeof(float) * lab::kBlock);
        std::memset(stemR[i], 0, sizeof(float) * lab::kBlock);
    }
    bpm_ = t.bpm > 1.0f ? t.bpm : 120.0f;
    const bool playing = t.playing && !wantStop;
    if (playing && !wasPlaying_) startPlay();
    if (!playing && wasPlaying_) {
        for (int s = 0; s < kStreams; ++s) midiOff(s);
        killAll();
        curSec = startSec < secCount ? startSec : 0;
        secProgress = 0.0f;
    }
    wasPlaying_ = playing;

    // main-loop requests
    for (int s = 0; s < kStreams; ++s) {
        const int32_t c = st[s].cue;
        if (c >= 0) {
            st[s].k = uint32_t(c);
            st[s].cue = -1;
        }
        const uint8_t a = st[s].audition;
        if (a) {
            st[s].audition = 0;
            fire(s, 0, float(a) * (1.0f / 127.0f));
        }
    }

    // throw edges: a CUT kills every voice on its first block, FREEZE and
    // STUTTER remember where each read head was
    const uint32_t th = throws;
    const uint32_t rise = th & ~lastThrows_;
    if (rise & TH_CUT) {
        killAll();
        for (int s = 0; s < kStreams; ++s) midiOff(s);
    }
    const uint32_t fall = lastThrows_ & ~th;
    for (int s = 0; s < kStreams; ++s) {
        if (rise & TH_FREEZE) thFreezeK_[s] = st[s].k;
        if (rise & TH_STUTTER) stutK_[s] = st[s].k ? st[s].k - 1u : 0u;
        // a stutter starts and ends at once: the stream's grid restarts at this block
        if (((rise | fall) & TH_STUTTER) && wasPlaying_ && next_[s] > now_) next_[s] = now_;
    }
    lastThrows_ = th;
    evBlock_ = 0;

    const uint64_t blockEnd = now_ + uint64_t(lab::kBlock) * kOne;
    const uint64_t barQ = beatsQ(4, 1, bpm_);
    const uint64_t sixteenthQ = barQ / 16;
    int pos = 0;
    if (playing) {
        uint64_t step[kStreams];
        int sd = stutterDiv < 0 ? 0 : (stutterDiv >= kRateDivs ? kRateDivs - 1 : stutterDiv);
        const uint64_t stutQ = beatsQ(kDivNum[sd], kDivDen[sd], bpm_);
        for (int s = 0; s < kStreams; ++s) step[s] = (th & TH_STUTTER) ? stutQ : stepQ(s, bpm_);
        for (int guard = 0; guard < 8192; ++guard) {
            // section progress (per segment start)
            const Section& c = sec[curSec];
            secEndQ_ = secStartQ_ + barQ * uint64_t(c.bars < 1 ? 1 : c.bars);
            const uint64_t tq = now_ + uint64_t(pos) * kOne;
            const uint64_t span = secEndQ_ - secStartQ_;
            secProgress = span ? float(uint32_t((tq - secStartQ_) >> 16)) / float(uint32_t(span >> 16)) : 0.0f;
            // next event time
            uint64_t tEnd = blockEnd;
            uint64_t boundary = secEndQ_;
            const int jr = jumpReq;
            if (jr >= 0) {
                static const uint8_t qBeats[Q_COUNT] = {0, 1, 4, 8, 16};
                const int qo = jumpQuantOnce;
                const int q0 = qo >= 0 ? qo : quant;
                const int q = q0 < 0 ? 0 : (q0 >= Q_COUNT ? Q_COUNT - 1 : q0);
                if (q == Q_NOW) {
                    boundary = tq;
                } else {
                    const uint64_t qQ = beatsQ(qBeats[q], 1, bpm_);
                    const uint64_t nb = ((tq + qQ - 1) / qQ) * qQ;   // grid from Play
                    boundary = nb < secEndQ_ ? nb : secEndQ_;
                }
            }
            if (arrange && boundary < tEnd) tEnd = boundary;
            if (arrange && c.type == T_CUT && cutNextQ_ < tEnd) tEnd = cutNextQ_;
            for (int s = 0; s < kStreams; ++s)
                if (next_[s] < tEnd) tEnd = next_[s];
            if (tEnd < tq) tEnd = tq;
            int to = int((tEnd - now_ + kOne - 1) / kOne);   // first sample at or after tEnd
            if (to > int(lab::kBlock)) to = int(lab::kBlock);
            if (to > pos) {
                for (int s = 0; s < kStreams; ++s) renderVoices(s, pos, to);
                pos = to;
            }
            if (pos >= int(lab::kBlock) && tEnd >= blockEnd) break;
            const uint64_t at = now_ + uint64_t(pos) * kOne;   // events due at or before this sample
            bool did = false;
            if (arrange && boundary <= at) {
                int nx;
                if (jr >= 0) {
                    nx = jr < secCount ? jr : 0;
                    jumpReq = -1;
                    jumpQuantOnce = -1;
                } else if (songMode == 0) {
                    nx = curSec;   // LIVE: the section repeats until another is launched
                } else {
                    nx = curSec + 1;
                    if (nx >= secCount) {
                        if (endMode == 1) {
                            wantStop = true;
                            killAll();
                            for (int s = 0; s < kStreams; ++s) midiOff(s);
                            break;
                        }
                        nx = 0;
                    }
                }
                enterSection(nx, 0);
                secStartQ_ = boundary;
                cutNextQ_ = boundary;
                did = true;
                continue;
            }
            if (!arrange && jr >= 0) jumpReq = -1;
            if (arrange && c.type == T_CUT && cutNextQ_ <= at) {
                const Slot& sl = slot[st[0].p[P_SLOT] & 3];
                uint8_t gv = 0, ga = 0;
                if (sl.n) readValue(sl, U_BYTE, cutK_, gv, ga);
                else gv = uint8_t((cutK_ * 97u) & 255u);
                const uint8_t open = gv >= uint8_t(c.depth * 255 / 100) ? 1 : 0;
                if (!open && gateOpen) killAll();
                gateOpen = open;
                ++cutK_;
                cutNextQ_ += sixteenthQ ? sixteenthQ : kOne;
                did = true;
            }
            for (int s = 0; s < kStreams; ++s)
                while (next_[s] <= at) {
                    fire(s, pos, 0.0f);
                    next_[s] += step[s];
                    did = true;
                }
            if (!did && pos >= int(lab::kBlock)) break;
        }
        now_ = blockEnd;
        clock = uint32_t(now_ >> 16);
        srcSec = secProgress;
        // MIDI note-offs at the end of the event (block resolution)
        if (midiMode)
            for (int s = 0; s < kStreams; ++s) {
                if (!lastNote_[s]) continue;
                bool any = false;
                for (int v = 0; v < kVoices; ++v)
                    if (v_[s][v].left > 0) any = true;
                if (!any) midiOff(s);
            }
    } else {
        for (int s = 0; s < kStreams; ++s) renderVoices(s, 0, int(lab::kBlock));
    }
    for (int i = 0; i < 4; ++i)
        for (uint32_t j = 0; j < lab::kBlock; ++j) {
            l[j] += stemL[i][j];
            r[j] += stemR[i][j];
        }
    // mod sources
    const float dTarget = evBlock_ >= 8 ? 1.0f : float(evBlock_) * 0.125f;
    srcDens += (dTarget - srcDens) * 0.02f;
    srcHit = evBlock_ ? 1.0f : srcHit * 0.9f;
}

}  // namespace dm
