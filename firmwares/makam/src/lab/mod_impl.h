// LABKIT v2 -- Mod implementation (compiled inside shell.cpp, see params_impl.h).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/lab.h"

namespace lab {
namespace kit {

namespace {

const char* const kShapeNames[] = {"Sine", "Tri", "Saw", "Square", "S&H", "Smooth"};
const char* const kSyncNames[] = {"Free", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars"};
const char* const kOffOnNames[] = {"Off", "On"};
const char* const kRndModes[] = {"Step", "Drunk"};
const char* const kLfoGroups[kLfos] = {"LFO 1", "LFO 2", "LFO 3", "LFO 4"};
const char* const kRndGroups[kRands] = {"RND 1", "RND 2"};
const char* const kSrcNames[kSrcWs] = {"LFO1", "LFO2", "LFO3", "LFO4", "RND1", "RND2", "M1",
                                       "M2",   "M3",   "M4",   "M5",   "M6",   "M7",   "M8"};
constexpr float kDivBeats[11] = {0.0f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f};

__attribute__((always_inline)) inline float frac(float x) { return x - float(int(x)); }

__attribute__((always_inline)) inline float lfoShape(int shape, float p, float a, float b) {
    switch (shape) {
        case 0: {   // sine: parabola + one refinement step (~0.1 %)
            const float x = p < 0.5f ? p : p - 1.0f;
            float y = 8.0f * x - 16.0f * x * fabsf(x);
            return 0.225f * (y * fabsf(y) - y) + y;
        }
        case 1: return 1.0f - 4.0f * fabsf(p - 0.5f) ;
        case 2: return 2.0f * p - 1.0f;
        case 3: return p < 0.5f ? 1.0f : -1.0f;
        case 4: return b;
        default: {   // smooth random: smoothstep from a to b across the cycle
            const float s = p * p * (3.0f - 2.0f * p);
            return a + (b - a) * s;
        }
    }
}

}  // namespace

float Mod::divisionBeats(int sync) { return (sync > 0 && sync < 11) ? kDivBeats[sync] : 0.0f; }

LABKIT_HOT float Mod::rand01(uint32_t& s) {
    if (!s) s = 0x9E3779B9u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return float(s >> 8) * (1.0f / 16777216.0f);
}

// ---- sources --------------------------------------------------------------------
int Mod::addSource(const char* name, const float* value, bool bipolar) {
    if (wsCount_ >= kMaxWsSources || !value) return -1;
    ws_[wsCount_] = WsSource{name ? name : "SRC", value, bipolar};
    return kSrcWs + wsCount_++;
}

int Mod::findSource(const char* name) const {
    for (int s = 0; s < sourceCount(); ++s)
        if (std::strcmp(sourceName(s), name) == 0) return s;
    return -1;
}

const char* Mod::sourceName(int s) const {
    if (s >= kSrcMacro && s < kSrcMacro + kMacros) return macroName_[s - kSrcMacro][0] ? macroName_[s - kSrcMacro] : kSrcNames[s];
    if (s >= 0 && s < kSrcWs) return kSrcNames[s];
    if (s >= kSrcWs && s < kSrcWs + wsCount_) return ws_[s - kSrcWs].name;
    return "";
}

const float* Mod::sourcePtr(int s) const {
    if (s >= kSrcLfo && s < kSrcLfo + kLfos) return &lfoOut_[s - kSrcLfo];
    if (s >= kSrcRnd && s < kSrcRnd + kRands) return &rndOut_[s - kSrcRnd];
    if (s >= kSrcMacro && s < kSrcMacro + kMacros) return &macroVal_[s - kSrcMacro];
    if (s >= kSrcWs && s < kSrcWs + wsCount_) return ws_[s - kSrcWs].value;
    return &zero_;
}

float Mod::sourceValue(int s) const { return *sourcePtr(s); }

bool Mod::sourceBipolar(int s) const {
    if (s < kSrcMacro) return true;
    if (s < kSrcWs) return false;
    return s < kSrcWs + wsCount_ && ws_[s - kSrcWs].bipolar;
}

Rgb Mod::sourceColor(int s) const {
    if (s < kSrcRnd) return Rgb(0x37D6FF);
    if (s < kSrcMacro) return Rgb(0xB07CFF);
    if (s < kSrcWs) return Rgb(0xFFB020);
    return Rgb(0x60FF90);
}

// ---- routes ----------------------------------------------------------------------
int Mod::findRoute(int src, ParamId p) const {
    for (int i = 0; i < count_; ++i)
        if (routes_[i].src == src && routes_[i].param == p) return i;
    return -1;
}

void Mod::recount() {
    if (!reg_) return;
    for (int i = 0; i < reg_->count(); ++i) {
        int n = 0;
        for (int r = 0; r < count_; ++r)
            if (routes_[r].param == i) ++n;
        if (reg_->meta(ParamId(i)).mods != n) reg_->setModCount(ParamId(i), n);
    }
    dirty_ = true;
}

int Mod::route(int src, ParamId p, float depth) {
    if (!reg_ || src < 0 || src >= sourceCount() || !reg_->valid(p) || !reg_->modulatable(p)) return -1;
    if (depth < -1.0f) depth = -1.0f;
    if (depth > 1.0f) depth = 1.0f;
    const int f = findRoute(src, p);
    if (f >= 0) {
        routes_[f].depth = depth;
        dirty_ = true;
        return f;
    }
    if (count_ >= kMaxRoutes) return -1;
    if (isMacro(src)) {
        int n = 0;
        for (int i = 0; i < count_; ++i)
            if (routes_[i].src == src) ++n;
        if (n >= kMacroTargets) return -1;
    }
    Route& r = routes_[count_++];
    r = Route();
    r.src = int8_t(src);
    r.param = p;
    r.depth = depth;
    recount();
    return count_ - 1;
}

bool Mod::unroute(int src, ParamId p) {
    const int i = findRoute(src, p);
    if (i < 0) return false;
    removeRoute(i);
    return true;
}

void Mod::removeRoute(int i) {
    if (i < 0 || i >= count_) return;
    for (int k = i; k + 1 < count_; ++k) routes_[k] = routes_[k + 1];
    --count_;
    recount();
}

void Mod::clearRoutes() {
    count_ = 0;
    recount();
}

void Mod::setDepth(int i, float d) {
    if (i < 0 || i >= count_) return;
    routes_[i].depth = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
    dirty_ = true;
}

void Mod::setWindow(int i, bool on, float lo, float hi) {
    if (i < 0 || i >= count_) return;
    routes_[i].window = on ? 1 : 0;
    routes_[i].lo = lo < 0.0f ? 0.0f : (lo > 1.0f ? 1.0f : lo);
    routes_[i].hi = hi < 0.0f ? 0.0f : (hi > 1.0f ? 1.0f : hi);
    dirty_ = true;
}

int Mod::routesTo(ParamId p, int* out, int cap) const {
    int n = 0;
    for (int i = 0; i < count_; ++i)
        if (routes_[i].param == p) {
            if (out && n < cap) out[n] = i;
            ++n;
        }
    return n;
}

int Mod::routesFrom(int src, int* out, int cap) const {
    int n = 0;
    for (int i = 0; i < count_; ++i)
        if (routes_[i].src == src) {
            if (out && n < cap) out[n] = i;
            ++n;
        }
    return n;
}

// ---- macros -------------------------------------------------------------------------
float Mod::macro(int m) const { return (reg_ && m >= 0 && m < kMacros) ? reg_->norm(macroParam_[m]) : 0.0f; }
void Mod::setMacro(int m, float v) {
    if (reg_ && m >= 0 && m < kMacros) reg_->setNorm(macroParam_[m], v);
}
void Mod::setMacroName(int m, const char* name) {
    if (m < 0 || m >= kMacros) return;
    copyStr(macroName_[m], sizeof macroName_[m], (name && *name) ? name : kSrcNames[kSrcMacro + m]);
}

void Mod::registerShellParams() {
    if (!reg_ || shellParamsRegistered()) return;
    Registry& r = *reg_;
    const int keepPage = r.currentPage(), keepGroup = r.currentGroup();
    const bool keepFlag = kit_ ? kit_->registeringShell : false;
    if (kit_) kit_->registeringShell = true;
    r.setPage(kPageMacro);
    r.setGroup("MACRO", Rgb(0xFFB020));
    for (int m = 0; m < kMacros; ++m) {
        if (!macroName_[m][0]) setMacroName(m, nullptr);
        char key[24];
        std::snprintf(key, sizeof key, "lk.m%d", m + 1);
        macroParam_[m] = r.add(key, macroName_[m], &macroVal_[m]).range(0.0f, 1.0f).percent().noMod().def(0.0f).pad(m);
    }
    r.setPage(kPageMod);
    const Rgb lfoCol[kLfos] = {Rgb(0x37D6FF), Rgb(0x30B8E8), Rgb(0x2A9BD0), Rgb(0x247FB8)};
    for (int i = 0; i < kLfos; ++i) {
        r.setGroup(kLfoGroups[i], lfoCol[i]);
        char key[40];
        const int base = (i & 1) * 6 + (i >> 1) * 12;   // LFO1 row 1 left, LFO2 right, LFO3/4 row 2
        Lfo& L = lfo_[i];
        L.seed = 0x1234567u * uint32_t(i + 1);
        std::snprintf(key, sizeof key, "lk.lfo%d.shape", i + 1);
        lfoParam_[i][0] = r.add(key, "Shape", &L.shape).names(kShapeNames, 6).pad(base);
        std::snprintf(key, sizeof key, "lk.lfo%d.rate", i + 1);
        lfoParam_[i][1] = r.add(key, "Rate", &L.rate).range(0.02f, 40.0f).exp().unit("Hz").pad(base + 1);
        std::snprintf(key, sizeof key, "lk.lfo%d.sync", i + 1);
        lfoParam_[i][2] = r.add(key, "Sync", &L.sync).names(kSyncNames, 11).pad(base + 2);
        std::snprintf(key, sizeof key, "lk.lfo%d.phase", i + 1);
        lfoParam_[i][3] = r.add(key, "Phase", &L.phase).range(0.0f, 360.0f).integer().step(5.0f).unit("deg").pad(base + 3);
        std::snprintf(key, sizeof key, "lk.lfo%d.retrig", i + 1);
        lfoParam_[i][4] = r.add(key, "Retrig", &L.retrig).names(kOffOnNames, 2).pad(base + 4);
    }
    const Rgb rndCol[kRands] = {Rgb(0xB07CFF), Rgb(0x9060E0)};
    for (int i = 0; i < kRands; ++i) {
        r.setGroup(kRndGroups[i], rndCol[i]);
        char key[40];
        const int base = 24 + i * 6;
        Rnd& R = rnd_[i];
        R.seed = 0xBADC0DEu * uint32_t(i + 3);
        std::snprintf(key, sizeof key, "lk.rnd%d.mode", i + 1);
        rndParam_[i][0] = r.add(key, "Mode", &R.mode).names(kRndModes, 2).pad(base);
        std::snprintf(key, sizeof key, "lk.rnd%d.sync", i + 1);
        rndParam_[i][1] = r.add(key, "Sync", &R.sync).names(kSyncNames, 11).pad(base + 1);
        std::snprintf(key, sizeof key, "lk.rnd%d.rate", i + 1);
        rndParam_[i][2] = r.add(key, "Rate", &R.rate).range(0.05f, 40.0f).exp().unit("Hz").pad(base + 2);
    }
    r.setPage(keepPage);
    r.restoreGroup(keepGroup);
    if (kit_) kit_->registeringShell = keepFlag;
}

// ---- audio ------------------------------------------------------------------------------
LABKIT_HOT void Mod::tick(const Transport& t) {
    // The beat inside a 64-beat cycle in float: every division divides 64, so
    // synced phases stay continuous; one double step per block at most.
    const float blockSec = float(kBlock) * (1.0f / float(kSampleRate));
    const float bps = t.bpm > 1.0f ? t.bpm * (1.0f / 60.0f) : 2.0f;
    // The beat in float, exact from the double beat on Play and every 256
    // blocks (~0.75 s), advanced by the tempo in between (no double math per block).
    if (t.playing) {
        if (t.startedThisBlock || resync_ == 0) {
            const double q = t.beat * (1.0 / 64.0);
            b64_ = float(q - double(int64_t(q))) * 64.0f;
            resync_ = 256;
        } else {
            --resync_;
            b64_ += bps * blockSec;
            if (b64_ >= 64.0f) b64_ -= 64.0f;
        }
    } else {
        resync_ = 0;
    }
    const float b64 = b64_;
    for (int i = 0; i < kLfos; ++i) {
        Lfo& L = lfo_[i];
        const int sync = int(L.sync + 0.5f);
        const float div = divisionBeats(sync);
        bool wrapped = false;
        if (t.startedThisBlock && L.retrig > 0.5f) {
            L.ph = 0.0f;
            wrapped = true;
        }
        if (div > 0.0f && t.playing) {
            const float p = frac(b64 / div);
            if (p < L.ph) wrapped = true;
            L.ph = p;
        } else {
            const float hz = div > 0.0f ? bps / div : L.rate;
            L.ph += hz * blockSec;
            if (L.ph >= 1.0f) {
                L.ph = frac(L.ph);
                wrapped = true;
            }
        }
        if (wrapped) {
            L.a = L.b;
            L.b = rand01(L.seed) * 2.0f - 1.0f;
        }
        const float p = frac(L.ph + L.phase * (1.0f / 360.0f));
        lfoOut_[i] = lfoShape(int(L.shape + 0.5f), p, L.a, L.b);
    }
    for (int i = 0; i < kRands; ++i) {
        Rnd& R = rnd_[i];
        const int sync = int(R.sync + 0.5f);
        const float div = divisionBeats(sync);
        bool step = false;
        if (div > 0.0f && t.playing) {
            const int32_t s = int32_t(b64 / div);
            if (s != R.lastStep) {
                step = true;
                R.lastStep = s;
            }
        } else {
            const float hz = div > 0.0f ? bps / div : R.rate;
            R.ph += hz * blockSec;
            if (R.ph >= 1.0f) {
                R.ph = frac(R.ph);
                step = true;
            }
        }
        if (t.startedThisBlock) step = true;
        if (step) {
            if (R.mode < 0.5f) {
                R.out = rand01(R.seed) * 2.0f - 1.0f;
            } else {
                float v = R.out + (rand01(R.seed) - 0.5f) * 0.5f;
                if (v > 1.0f) v = 2.0f - v;
                if (v < -1.0f) v = -2.0f - v;
                R.out = v;
            }
        }
        rndOut_[i] = R.out;
    }
}

LABKIT_HOT void Mod::applyRoutes() {
    Plan* p = live_;
    if (!p || !reg_) return;
    audioGen_ = p->gen;
    float* base = reg_->baseArray();
    float* eff = reg_->effArray();
    const PlanRoute* r = p->r;
    const PlanParam* pp = p->p;
    for (int i = p->nParams; i > 0; --i, ++pp) {
        float n = base[pp->id];
        for (int j = pp->windows; j > 0; --j, ++r) {   // a window sets the value (the last one wins)
            const float v = *r->src;
            const float u = r->bipolar ? 0.5f * (v + 1.0f) : v;
            n = r->lo + (r->hi - r->lo) * u;
        }
        for (int j = pp->adds; j > 0; --j, ++r) n += *r->src * r->depth;
        n = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
        eff[pp->id] = n;
        switch (pp->mode) {
            case 1: *static_cast<float*>(pp->ptr) = pp->lo + n * pp->k; break;
            case 2: *static_cast<float*>(pp->ptr) = pp->lo * fastExp2(n * pp->k); break;
            case 3: *static_cast<int*>(pp->ptr) = roundi(pp->lo + n * pp->k); break;
            default: reg_->writeNorm(pp->id, n); break;
        }
    }
}

void Mod::rebuild(Plan& plan) {
    plan.nRoutes = plan.nParams = 0;
    for (int i = 0; i < count_; ++i) {   // each param once, in route order
        const ParamId id = routes_[i].param;
        if (!reg_->valid(id)) continue;
        bool seen = false;
        for (int k = 0; k < plan.nParams; ++k) seen |= plan.p[k].id == id;
        if (seen) continue;
        PlanParam& pp = plan.p[plan.nParams++];
        pp.id = id;
        pp.windows = pp.adds = 0;
        const Registry::Bind& b = reg_->binding(id);
        pp.lo = b.lo;
        pp.k = b.k;
        pp.ptr = b.ptr;
        pp.mode = 0;
        if (b.kind == Registry::kBindFloat && b.curve == uint8_t(Curve::kLinear)) pp.mode = 1;
        if (b.kind == Registry::kBindFloat && b.curve == uint8_t(Curve::kExp)) pp.mode = 2;
        if (b.kind == Registry::kBindInt && b.curve != uint8_t(Curve::kExp)) pp.mode = 3;
        for (int pass = 0; pass < 2; ++pass)   // window routes first, then the add routes
            for (int k = 0; k < count_; ++k) {
                const Route& r = routes_[k];
                if (r.param != id || int(r.window) != 1 - pass) continue;
                PlanRoute& pr = plan.r[plan.nRoutes++];
                pr.src = sourcePtr(r.src);
                pr.depth = r.depth;
                pr.lo = r.lo;
                pr.hi = r.hi;
                pr.window = r.window;
                pr.bipolar = sourceBipolar(r.src) ? 1 : 0;
                if (r.window) ++pp.windows;
                else ++pp.adds;
            }
    }
}

void Mod::update(bool audioRunning) {
    if (!dirty_ || !reg_) return;
    if (audioRunning && live_ && audioGen_ != gen_) return;   // the audio block has not taken the last plan yet
    Plan& next = (live_ == &plans_[0]) ? plans_[1] : plans_[0];
    rebuild(next);
    next.gen = ++gen_;
    __atomic_store_n(&live_, &next, __ATOMIC_RELEASE);
    dirty_ = false;
}

}  // namespace kit
}  // namespace lab
