// GRAF -- the patch engine (graf_engine.h).
#include "ws/graf_engine.h"

#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "core/fastrun.h"
#include "ws/graf_modules.h"

namespace graf {

namespace {
// Per-input scratch for cable sums (SRAM; render is never re-entered).
float gIn[kMaxIn][kBlock];
// Output buffers: a fast pool in SRAM first, the rest in the arena (SDRAM).
constexpr int kFastBufs = 72;
float gFast[kFastBufs][kBlock];
}  // namespace

// ------------------------------------------------------------------ init
bool Engine::init(lab::Arena& a) {
    initMath();
    if (!initWavetables(a)) return false;
    mods_ = a.make<Module>(kMaxSlots);
    cables_ = a.make<Cable>(kMaxCables);
    plans_ = a.make<Plan>(3);
    morphFrom_ = a.make<Snapshot>(1);
    if (!mods_ || !cables_ || !plans_ || !morphFrom_) return false;
    const int slow = kMaxSlots * kMaxOut - kFastBufs;
    bufs_ = static_cast<float*>(a.alloc(uint32_t(slow) * kBlock * sizeof(float)));
    if (!bufs_) return false;
    for (int i = 0; i < kSlabs; ++i) {
        slabs_[i] = static_cast<float*>(a.alloc(kSlabFloats * sizeof(float)));
        if (!slabs_[i]) return false;
    }
    for (int i = 0; i < kSampleBanks; ++i) {
        banks_[i] = static_cast<int16_t*>(a.alloc(kBankSamples * sizeof(int16_t)));
        if (!banks_[i]) return false;
    }
    for (int i = 0; i < 4; ++i) {
        stemL[i] = static_cast<float*>(a.alloc(kBlock * sizeof(float)));
        stemR[i] = static_cast<float*>(a.alloc(kBlock * sizeof(float)));
        if (!stemL[i] || !stemR[i]) return false;
    }
    // Output buffer per (slot, out): fixed assignment.
    int fast = 0, slowAt = 0;
    for (int s = 0; s < kMaxSlots; ++s)
        for (int o = 0; o < kMaxOut; ++o) {
            if (fast < kFastBufs) mods_[s].out[o] = gFast[fast++];
            else mods_[s].out[o] = bufs_ + uint32_t(slowAt++) * kBlock;
        }
    for (int p = 0; p < kPatches; ++p) {
        mix[p] = MixStrip();
        voices_[p] = PatchVoices();
        for (float& c : voices_[p].cc) c = 0.0f;
    }
    for (int i = 0; i < kSongSteps; ++i) {
        song[i].snap = uint8_t(i % 4);
        song[i].morphBars = 2;
        song[i].holdBars = 2;
    }
    for (int i = 0; i < kMaxSlots; ++i) sampleLoaded[i] = -1;
    rng.seed(0xC0FFEEu);
    rebuild();
    return true;
}

// ------------------------------------------------------------------ params
float Engine::mapParam(const ParamDef& d, float k) {
    k = clampf(k, 0.0f, 1.0f);
    switch (d.kind) {
        case kExp: {
            const float lo = d.min > 1e-6f ? d.min : 1e-6f;
            return lo * fexp2(k * log2f(d.max / lo));
        }
        case kInt: return floorf(d.min + k * (d.max - d.min) + 0.5f);
        default: return d.min + k * (d.max - d.min);
    }
}

float Engine::unmapParam(const ParamDef& d, float v) {
    if (d.max <= d.min) return 0.0f;
    if (d.kind == kExp) {
        const float lo = d.min > 1e-6f ? d.min : 1e-6f;
        if (v <= lo) return 0.0f;
        return clampf(log2f(v / lo) / log2f(d.max / lo), 0.0f, 1.0f);
    }
    return clampf((v - d.min) / (d.max - d.min), 0.0f, 1.0f);
}

void Engine::setParamK(int slot, int p, float k) {
    Module& m = mods_[slot];
    const ModDef& d = def(m.type);
    if (p < 0 || p >= d.nParam) return;
    m.k[p] = clampf(k, 0.0f, 1.0f);
    m.v[p] = mapParam(d.p[p], m.k[p]);
}

void Engine::refresh(int slot) {
    const ModDef& d = def(mods_[slot].type);
    for (int p = 0; p < d.nParam; ++p) mods_[slot].v[p] = mapParam(d.p[p], mods_[slot].k[p]);
}

void Engine::resetParams(int slot) {
    Module& m = mods_[slot];
    const ModDef& d = def(m.type);
    for (int p = 0; p < d.nParam; ++p) m.k[p] = unmapParam(d.p[p], d.p[p].def);
    refresh(slot);
}

void Engine::randomize(int slot, Rand& r) {
    Module& m = mods_[slot];
    const ModDef& d = def(m.type);
    for (int p = 0; p < d.nParam; ++p) {
        // keep levels, MIDI channels, voices and sample choice where they are
        const char* n = d.p[p].name;
        if (!std::strcmp(n, "Level") || !std::strcmp(n, "MIDI out") || !std::strcmp(n, "Voice") ||
            !std::strcmp(n, "Sample") || !std::strcmp(n, "Stem") || !std::strcmp(n, "Freeze"))
            continue;
        m.k[p] = clampf(m.k[p] + r.bi() * 0.35f, 0.0f, 1.0f);
    }
    refresh(slot);
    if (m.type == kPinb) pinbLayout(m, r.next());
}

// ------------------------------------------------------------------ graph editing
int Engine::slotAtCell(int patch, int cell) const {
    for (int s = 0; s < kMaxSlots; ++s)
        if (mods_[s].used && mods_[s].patch == patch && mods_[s].cell == cell) return s;
    return -1;
}

int Engine::freeCell(int patch, int preferred) const {
    if (preferred >= 0 && preferred < kCells && slotAtCell(patch, preferred) < 0) return preferred;
    for (int c = 0; c < kCells; ++c)
        if (slotAtCell(patch, c) < 0) return c;
    return -1;
}

int Engine::moduleCount(int patch) const {
    int n = 0;
    for (int s = 0; s < kMaxSlots; ++s)
        if (mods_[s].used && (patch < 0 || mods_[s].patch == patch)) ++n;
    return n;
}

int Engine::cableCount() const {
    int n = 0;
    for (int c = 0; c < kMaxCables; ++c)
        if (cables_[c].used) ++n;
    return n;
}

// VCO and NOIS render only the outputs that are patched; the module on the
// scope renders all of them (one module at a time: cpuCost() adds the
// largest such difference as headroom). VCO measured: ~4,000 + ~2,600 per
// output, ~+9,000 with FM patched (per-sample exponential).
int32_t Engine::modCost(int slot, bool allOuts) const {
    const Module& m = mods_[slot];
    const ModDef& d = def(m.type);
    if (m.type != kVco && m.type != kNoise) return d.cost;
    uint8_t used = 0;
    bool fm = false;
    for (int c = 0; c < kMaxCables; ++c) {
        if (!cables_[c].used) continue;
        if (cables_[c].src == slot) used |= uint8_t(1u << cables_[c].srcOut);
        if (cables_[c].dst == slot && cables_[c].dstIn == 1) fm = true;
    }
    int n = 0;
    for (int o = 0; o < d.nOut; ++o)
        if (allOuts || (used & (1u << o))) ++n;
    if (m.type == kVco) return 4400 + 2900 * (n ? n : 1) + (fm ? 10000 : 0);
    return n <= 1 ? 3500 : d.cost;
}

int32_t Engine::cpuCost() const {
    int32_t c = kEngineCost, scopeExtra = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (!mods_[s].used) continue;
        const int32_t k = modCost(s);
        c += k;
        const int32_t extra = modCost(s, true) - k;
        if (extra > scopeExtra) scopeExtra = extra;
    }
    for (int i = 0; i < kMaxCables; ++i)
        if (cables_[i].used) c += kCableCost;
    return c + scopeExtra;
}

int Engine::newNumber(int type) const {
    for (int n = 1; n < 100; ++n) {
        bool taken = false;
        for (int s = 0; s < kMaxSlots && !taken; ++s)
            if (mods_[s].used && mods_[s].type == type && mods_[s].number == n) taken = true;
        if (!taken) return n;
    }
    return 99;
}

int Engine::add(int type, int patch, int cell) {
    for (int s = 0; s < kMaxSlots; ++s)
        if (!mods_[s].used && !reserved_[s]) return addAt(s, type, patch, cell);
    err_ = "No free module slot";
    return -1;
}

int Engine::addAt(int slot, int type, int patch, int cell) {
    if (type < 0 || type >= kTypeCount || slot < 0 || slot >= kMaxSlots) {
        err_ = "Unknown module";
        return -1;
    }
    if (mods_[slot].used || reserved_[slot]) {
        err_ = "Slot busy";
        return -1;
    }
    const ModDef& d = def(type);
    if (!fits(d.cost)) {
        err_ = "CPU budget full";
        return -1;
    }
    cell = freeCell(patch, cell);
    if (cell < 0) {
        err_ = "No free cell in this patch";
        return -1;
    }
    int slab = -1, bank = -1;
    if (d.slab) {
        for (int i = 0; i < kSlabs; ++i)
            if (!slabUsed_[i]) {
                slab = i;
                break;
            }
        if (slab < 0) {
            err_ = "No memory left for another effect";
            return -1;
        }
    }
    if (type == kSmpl) {
        for (int i = 0; i < kSampleBanks; ++i)
            if (!bankUsed_[i]) {
                bank = i;
                break;
            }
        if (bank < 0) {
            err_ = "Sampler memory full (8 samplers)";
            return -1;
        }
    }
    Module& m = mods_[slot];
    float* outs[kMaxOut];
    for (int o = 0; o < kMaxOut; ++o) outs[o] = m.out[o];
    std::memset(m.st.bytes, 0, sizeof m.st.bytes);
    std::memset(m.ext, 0, sizeof m.ext);
    for (int o = 0; o < kMaxOut; ++o) {
        m.out[o] = outs[o];
        for (uint32_t i = 0; i < kBlock; ++i) m.out[o][i] = 0.0f;
    }
    m.type = uint8_t(type);
    m.patch = uint8_t(patch);
    m.cell = uint8_t(cell);
    m.number = uint8_t(newNumber(type));
    m.slab = nullptr;
    m.slabIdx = -1;
    m.bank = nullptr;
    m.bankIdx = -1;
    if (slab >= 0) {
        slabUsed_[slab] = true;
        m.slabIdx = int8_t(slab);
        m.slab = slabs_[slab];
        std::memset(m.slab, 0, kSlabFloats * sizeof(float));
    }
    if (bank >= 0) {
        bankUsed_[bank] = true;
        m.bankIdx = int8_t(bank);
        m.bank = banks_[bank];
        sampleFrames[slot] = 0;
        sampleLoaded[slot] = -1;
        sampleStereo[slot] = false;
        sampleName[slot][0] = 0;
    }
    for (int p = 0; p < kMaxParam; ++p) m.k[p] = m.v[p] = 0.0f;
    resetParams(slot);
    if (d.reset) d.reset(m);
    m.used = true;
    dirtyPlan_ = true;
    return slot;
}

void Engine::freeSlot(int slot) {
    Module& m = mods_[slot];
    if (m.slabIdx >= 0) slabUsed_[m.slabIdx] = false;
    if (m.bankIdx >= 0) bankUsed_[m.bankIdx] = false;
    m.slabIdx = -1;
    m.bankIdx = -1;
    m.slab = nullptr;
    m.bank = nullptr;
    reserved_[slot] = false;
}

bool Engine::remove(int slot) {
    if (slot < 0 || slot >= kMaxSlots || !mods_[slot].used) return false;
    for (int c = 0; c < kMaxCables; ++c)
        if (cables_[c].used && (cables_[c].src == slot || cables_[c].dst == slot)) cables_[c].used = false;
    mods_[slot].used = false;
    if (scopeSlot == slot) scopeSlot = -1;
    reserved_[slot] = true;
    rebuild();   // publish a plan without the slot before it may be reused
    if (nPending_ < kMaxSlots) pending_[nPending_++] = Pending{uint8_t(slot), renders_};
    return true;
}

void Engine::service(uint32_t nowMs) {
    (void)nowMs;
    // A pending slot is free once the audio side has started two blocks with
    // the new plan, or right away when no audio is running at all.
    static uint32_t lastRenders = 0, stuckSince = 0, stuckMs = 0;
    const uint32_t r = renders_;
    if (r == lastRenders) {
        if (stuckSince == 0) stuckSince = 1, stuckMs = nowMs;
    } else {
        stuckSince = 0;
    }
    lastRenders = r;
    const bool audioIdle = stuckSince && nowMs - stuckMs > 60;
    int w = 0;
    for (int i = 0; i < nPending_; ++i) {
        if (r - pending_[i].seq >= 2 || audioIdle || !concurrent_) freeSlot(pending_[i].slot);
        else pending_[w++] = pending_[i];
    }
    nPending_ = w;
    if (dirtyPlan_) rebuild();
}

void Engine::quiesce() {
    for (int i = 0; i < nPending_; ++i) freeSlot(pending_[i].slot);
    nPending_ = 0;
}

void Engine::clear() {
    for (int s = 0; s < kMaxSlots; ++s)
        if (mods_[s].used) remove(s);
    for (int c = 0; c < kMaxCables; ++c) cables_[c].used = false;
    rebuild();
}

int Engine::duplicate(int slot) {
    if (slot < 0 || !mods_[slot].used) return -1;
    const Module src = mods_[slot];
    const int n = add(src.type, src.patch, src.cell + 1 < kCells ? src.cell + 1 : -1);
    if (n < 0) return -1;
    Module& m = mods_[n];
    for (int p = 0; p < kMaxParam; ++p) m.k[p] = src.k[p];
    std::memcpy(m.ext, src.ext, sizeof m.ext);
    refresh(n);
    // copy the cables into its inputs (outputs are left free)
    for (int c = 0; c < kMaxCables; ++c)
        if (cables_[c].used && cables_[c].dst == slot)
            connect(cables_[c].src, cables_[c].srcOut, n, cables_[c].dstIn, cables_[c].amount);
    rebuild();
    return n;
}

bool Engine::move(int slot, int cell) {
    if (slot < 0 || !mods_[slot].used || cell < 0 || cell >= kCells) return false;
    const int other = slotAtCell(mods_[slot].patch, cell);
    if (other >= 0 && other != slot) mods_[other].cell = mods_[slot].cell;   // swap
    mods_[slot].cell = uint8_t(cell);
    return true;
}

int Engine::cableFind(int src, int srcOut, int dst, int dstIn) const {
    for (int c = 0; c < kMaxCables; ++c) {
        const Cable& k = cables_[c];
        if (k.used && k.src == src && k.srcOut == srcOut && k.dst == dst && k.dstIn == dstIn) return c;
    }
    return -1;
}

int Engine::connect(int src, int srcOut, int dst, int dstIn, float amount) {
    if (src < 0 || dst < 0 || src >= kMaxSlots || dst >= kMaxSlots || !mods_[src].used || !mods_[dst].used) {
        err_ = "No module";
        return -1;
    }
    if (srcOut >= def(mods_[src].type).nOut || dstIn >= def(mods_[dst].type).nIn) {
        err_ = "No such jack";
        return -1;
    }
    if (!rack && mods_[src].patch != mods_[dst].patch) {
        err_ = "Cables stay inside one patch";
        return -1;
    }
    const int have = cableFind(src, srcOut, dst, dstIn);
    if (have >= 0) {
        cables_[have].amount = amount;
        return have;
    }
    if (!fits(kCableCost)) {   // (the dearer case: a summed cable)
        err_ = "CPU budget full";
        return -1;
    }
    for (int c = 0; c < kMaxCables; ++c)
        if (!cables_[c].used) {
            Cable& k = cables_[c];
            k.src = uint8_t(src);
            k.srcOut = uint8_t(srcOut);
            k.dst = uint8_t(dst);
            k.dstIn = uint8_t(dstIn);
            k.amount = clampf(amount, -2.0f, 2.0f);
            k.used = true;
            dirtyPlan_ = true;
            return c;
        }
    err_ = "Too many cables";
    return -1;
}

bool Engine::disconnect(int c) {
    if (c < 0 || c >= kMaxCables || !cables_[c].used) return false;
    cables_[c].used = false;
    dirtyPlan_ = true;
    return true;
}

// ------------------------------------------------------------------ plan
void Engine::compile(Plan& p) {
    // Kahn's topological order over the cables; modules in feedback loops are
    // appended in slot order (their back edges read last block's output).
    uint8_t indeg[kMaxSlots] = {};
    bool done[kMaxSlots] = {};
    for (int c = 0; c < kMaxCables; ++c) {
        const Cable& k = cables_[c];
        if (k.used && k.src != k.dst && mods_[k.src].used && mods_[k.dst].used) ++indeg[k.dst];
    }
    p.count = 0;
    for (;;) {
        int pick = -1;
        for (int s = 0; s < kMaxSlots; ++s)
            if (mods_[s].used && !done[s] && indeg[s] == 0) {
                pick = s;
                break;
            }
        if (pick < 0) {   // a cycle (or done): take the unplaced module with the fewest open inputs
            int best = 255;
            for (int s = 0; s < kMaxSlots; ++s)
                if (mods_[s].used && !done[s] && indeg[s] < best) {
                    best = indeg[s];
                    pick = s;
                }
            if (pick < 0) break;
        }
        done[pick] = true;
        p.order[p.count++] = uint8_t(pick);
        for (int c = 0; c < kMaxCables; ++c) {
            const Cable& k = cables_[c];
            if (k.used && k.src == pick && k.dst != pick && indeg[k.dst]) --indeg[k.dst];
        }
    }
    for (int s = 0; s < kMaxSlots; ++s) p.used[s] = 0;
    for (int c = 0; c < kMaxCables; ++c)
        if (cables_[c].used) p.used[cables_[c].src] |= uint8_t(1u << cables_[c].srcOut);
    // Input source lists.
    uint16_t n = 0;
    for (int s = 0; s < kMaxSlots; ++s)
        for (int i = 0; i < kMaxIn; ++i) {
            p.first[s][i] = n;
            p.n[s][i] = 0;
            if (!mods_[s].used) continue;
            for (int c = 0; c < kMaxCables; ++c) {
                const Cable& k = cables_[c];
                if (!k.used || k.dst != s || k.dstIn != i || !mods_[k.src].used) continue;
                if (n >= kMaxCables) break;
                p.src[n].buf = mods_[k.src].out[k.srcOut];
                p.src[n].amt = &cables_[c].amount;
                ++n;
                ++p.n[s][i];
            }
        }
}

void Engine::rebuild() {
    dirtyPlan_ = false;
    collectVoices();
    // A plan neither published nor in use by a block that is running now
    // (the device's audio interrupt never overlaps the main loop, the
    // simulator's audio thread can).
    int next = 0;
    while (next == pub_ || next == reading_) ++next;
    compile(plans_[next]);
    __atomic_store_n(&pub_, next, __ATOMIC_RELEASE);
    pubSeq_ = pubSeq_ + 1;
}

void Engine::collectVoices() {
    for (int p = 0; p < kPatches; ++p) voices_[p].count = 1;
    for (int s = 0; s < kMaxSlots; ++s)
        if (mods_[s].used && mods_[s].type == kMidi) {
            PatchVoices& pv = voices_[voicePatch(s)];
            const int v = int(mods_[s].v[0]);
            if (v > pv.count) pv.count = v > 8 ? 8 : v;
        }
}

// ------------------------------------------------------------------ render
OT_FASTRUN_FN void Engine::render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r, lab::Context* ctx) {
    const int pi = __atomic_load_n(&pub_, __ATOMIC_ACQUIRE);
    reading_ = pi;
    const Plan& plan = plans_[pi];
    renders_ = renders_ + 1;
    mixL = l;
    mixR = r;
    for (int s = 0; s < 4; ++s) {
        stemUsed[s] = false;
        for (uint32_t i = 0; i < kBlock; ++i) stemL[s][i] = stemR[s][i] = 0.0f;
    }
    tempoBpm = t.bpm;
    beatQ8 = (t.beat > 0.0 && t.beat < 16000000.0) ? uint32_t(t.beat * 256.0) : 0u;
    Proc p;
    double b = t.beat;
    if (b > 1024.0) b -= double(int32_t(b * (1.0 / 1024.0))) * 1024.0;
    p.tr = &t;
    p.beat = float(b);
    p.bps = float(t.beatsPerSample);
    if (p.bps <= 0.0f) p.bps = 120.0f / 60.0f / 44100.0f;
    p.playing = t.playing;
    p.started = t.startedThisBlock;
    p.ain = &in;
    p.eng = this;
    for (int k = 0; k < plan.count; ++k) {
        const int s = plan.order[k];
        Module& m = mods_[s];
        const ModDef& d = kDefs[m.type];
        p.conn = 0;
        for (int i = 0; i < d.nIn; ++i) {
            const int n = plan.n[s][i];
            if (n == 0) {
                p.in[i] = gZero;
                continue;
            }
            p.conn |= uint8_t(1u << i);
            const Plan::Src* src = &plan.src[plan.first[s][i]];
            const float a0 = *src[0].amt;
            if (n == 1 && a0 == 1.0f) {
                p.in[i] = src[0].buf;
                continue;
            }
            float* dst = gIn[i];
            const float* b0 = src[0].buf;
#pragma GCC unroll 8
            for (uint32_t j = 0; j < kBlock; ++j) dst[j] = b0[j] * a0;
            for (int c = 1; c < n; ++c) {
                const float a = *src[c].amt;
                const float* bc = src[c].buf;
#pragma GCC unroll 8
                for (uint32_t j = 0; j < kBlock; ++j) dst[j] += bc[j] * a;
            }
            p.in[i] = dst;
        }
        for (int i = d.nIn; i < kMaxIn; ++i) p.in[i] = gZero;
        p.used = (allOutputs || s == scopeSlot) ? 0xFF : plan.used[s];
        if (m.type == kOut) p.used = 0xFF;
        d.process(m, p);
    }
    // scope + stems + master meter
    const int sc = scopeSlot;
    if (sc >= 0 && sc < kMaxSlots && mods_[sc].used) {
        const int o = scopeOut < def(mods_[sc].type).nOut ? scopeOut : 0;
        if (def(mods_[sc].type).nOut) std::memcpy(scope, mods_[sc].out[o], sizeof scope);
        else std::memcpy(scope, l, sizeof scope);
    } else {
        for (uint32_t i = 0; i < kBlock; ++i) scope[i] = 0.5f * (l[i] + r[i]);
    }
    scopeSeq = scopeSeq + 1;
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float a = fabsf(l[i]) > fabsf(r[i]) ? fabsf(l[i]) : fabsf(r[i]);
        if (a > pk) pk = a;
    }
    if (pk > masterPeak) masterPeak = pk;
    if (ctx)
        for (int s = 0; s < 4; ++s)
            if (stemUsed[s]) ctx->stem(s, stemL[s], stemR[s]);
}

// ------------------------------------------------------------------ voices
void Engine::noteOn(int patch, uint8_t note, uint8_t vel) {
    PatchVoices& pv = voices_[patch & 7];
    ++pv.clock;
    const float pitch = float(int(note) - 60) * (1.0f / 12.0f);
    if (pv.count <= 1) {
        // mono, last-note priority with a held-note stack
        int w = 0;
        for (int i = 0; i < pv.held; ++i)
            if (pv.stack[i] != note) {
                pv.stack[w] = pv.stack[i];
                pv.stackVel[w++] = pv.stackVel[i];
            }
        pv.held = w;
        if (pv.held >= 16) {
            for (int i = 1; i < 16; ++i) {
                pv.stack[i - 1] = pv.stack[i];
                pv.stackVel[i - 1] = pv.stackVel[i];
            }
            pv.held = 15;
        }
        pv.stack[pv.held] = note;
        pv.stackVel[pv.held++] = vel;
        Voice& v = pv.v[0];
        const bool legato = v.gate;
        v.note = note;
        v.pitch = pitch;
        v.vel = float(vel) * (1.0f / 127.0f);
        v.gate = true;
        if (!legato) v.trig = v.trig + 1;
        else v.trig = v.trig + 1;   // retrigger every note (envelopes restart)
        return;
    }
    // poly: reuse the voice already on this note, else a free one, else the oldest
    int pick = -1;
    for (int i = 0; i < pv.count; ++i)
        if (pv.v[i].gate && pv.v[i].note == note) pick = i;
    if (pick < 0)
        for (int i = 0; i < pv.count; ++i)
            if (!pv.v[i].gate && (pick < 0 || pv.v[i].age < pv.v[pick].age)) pick = i;
    if (pick < 0) {
        pick = 0;
        for (int i = 1; i < pv.count; ++i)
            if (pv.v[i].age < pv.v[pick].age) pick = i;
    }
    Voice& v = pv.v[pick];
    v.note = note;
    v.pitch = pitch;
    v.vel = float(vel) * (1.0f / 127.0f);
    v.gate = true;
    v.age = pv.clock;
    v.trig = v.trig + 1;
}

void Engine::noteOff(int patch, uint8_t note) {
    PatchVoices& pv = voices_[patch & 7];
    if (pv.count <= 1) {
        int w = 0;
        for (int i = 0; i < pv.held; ++i)
            if (pv.stack[i] != note) {
                pv.stack[w] = pv.stack[i];
                pv.stackVel[w++] = pv.stackVel[i];
            }
        pv.held = w;
        Voice& v = pv.v[0];
        if (pv.held > 0) {
            const uint8_t n = pv.stack[pv.held - 1];
            if (n != v.note) {
                v.note = n;
                v.pitch = float(int(n) - 60) * (1.0f / 12.0f);
            }
        } else if (v.note == note) {
            v.gate = false;
        } else {
            v.gate = false;
        }
        return;
    }
    for (int i = 0; i < 8; ++i)
        if (pv.v[i].gate && pv.v[i].note == note) {
            pv.v[i].gate = false;
            pv.v[i].age = ++pv.clock;
        }
}

void Engine::allNotesOff() {
    for (int p = 0; p < kPatches; ++p) {
        voices_[p].held = 0;
        for (int i = 0; i < 8; ++i) voices_[p].v[i].gate = false;
    }
}

void Engine::midiIn(const lab::MidiMsg& m) {
    // Rack: every channel plays the rack. 8 patches: channel n plays patch n
    // (channels 9..16 play patch 1..8 too).
    const int patch = rack ? 0 : (m.channel() & 7);
    switch (m.type()) {
        case 0x90:
            if (m.d2) noteOn(patch, m.d1, m.d2);
            else noteOff(patch, m.d1);
            break;
        case 0x80: noteOff(patch, m.d1); break;
        case 0xB0:
            voices_[patch].cc[m.d1 & 127] = float(m.d2) * (1.0f / 127.0f);
            if (m.d1 == 123) allNotesOff();
            break;
        case 0xE0: voices_[patch].bend = float((int(m.d2) << 7 | m.d1) - 8192) * (1.0f / 8192.0f); break;
        default: break;
    }
}

// ------------------------------------------------------------------ snapshots
void Engine::captureLive(Snapshot& s) const {
    s.valid = true;
    s.nMod = 0;
    s.nCab = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        const Module& m = mods_[i];
        if (!m.used) continue;
        SnapMod& sm = s.mod[s.nMod++];
        sm.type = m.type;
        sm.patch = m.patch;
        sm.cell = m.cell;
        sm.number = m.number;
        std::memcpy(sm.k, m.k, sizeof sm.k);
        std::memcpy(sm.ext, m.ext, sizeof sm.ext);
    }
    for (int c = 0; c < kMaxCables; ++c) {
        const Cable& k = cables_[c];
        if (!k.used) continue;
        SnapCable& sc = s.cab[s.nCab++];
        sc.src = uint16_t(mods_[k.src].patch * kCells + mods_[k.src].cell);
        sc.dst = uint16_t(mods_[k.dst].patch * kCells + mods_[k.dst].cell);
        sc.srcOut = k.srcOut;
        sc.dstIn = k.dstIn;
        sc.amount = k.amount;
    }
    for (int p = 0; p < kPatches; ++p) s.level[p] = mix[p].level;
}

void Engine::storeSnap(int i) {
    if (i < 0 || i >= kSnaps) return;
    captureLive(snaps_[i]);
}

void Engine::clearSnap(int i) {
    if (i >= 0 && i < kSnaps) snaps_[i].valid = false;
}

// Live state = a -> b at t. Modules matched by position (patch, cell) and
// type: their knobs interpolate. Modules only in b appear at the start (if
// the CPU allows), modules only in a leave at the end; cables only in a fade
// out, cables only in b fade in. Sequencer data (ext) switches half way.
void Engine::morphBetween(const Snapshot& a, const Snapshot& b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    bool changed = false;
    auto live = [&](uint16_t key, uint8_t type) -> int {
        const int s = slotAtCell(key / kCells, key % kCells);
        return (s >= 0 && mods_[s].type == type) ? s : -1;
    };
    // 1. at the end, remove modules that b does not have
    if (t >= 1.0f)
        for (int s = 0; s < kMaxSlots; ++s) {
            if (!mods_[s].used) continue;
            const uint16_t key = uint16_t(mods_[s].patch * kCells + mods_[s].cell);
            const int bi = b.find(key);
            if (bi < 0 || b.mod[bi].type != mods_[s].type) {
                remove(s);
                changed = true;
            }
        }
    // 2. modules of b: create when missing, interpolate knobs
    for (int i = 0; i < b.nMod; ++i) {
        const SnapMod& bm = b.mod[i];
        int s = live(bm.key(), bm.type);
        if (s < 0) {
            const int occ = slotAtCell(bm.patch, bm.cell);
            if (occ >= 0) {
                if (t < 1.0f) continue;   // a different module sits there until the end
                remove(occ);
            }
            s = add(bm.type, bm.patch, bm.cell);
            if (s < 0) continue;
            mods_[s].number = bm.number;
            std::memcpy(mods_[s].ext, bm.ext, sizeof bm.ext);
            changed = true;
        }
        const int ai = a.find(bm.key());
        const bool both = ai >= 0 && a.mod[ai].type == bm.type;
        Module& m = mods_[s];
        const ModDef& d = def(m.type);
        for (int p = 0; p < d.nParam; ++p) {
            float k = bm.k[p];
            if (both) {
                if (d.p[p].kind == kInt) k = t < 0.5f ? a.mod[ai].k[p] : bm.k[p];
                else k = lerpf(a.mod[ai].k[p], bm.k[p], t);
            }
            m.k[p] = k;
            m.v[p] = mapParam(d.p[p], k);
        }
        if (both && t >= 0.5f) std::memcpy(m.ext, bm.ext, sizeof m.ext);
        else if (both) std::memcpy(m.ext, a.mod[ai].ext, sizeof m.ext);
    }
    // 3. cables: union of a and b with faded amounts
    auto amountIn = [](const Snapshot& sn, uint16_t src, uint8_t so, uint16_t dst, uint8_t di, float* amt) {
        for (int c = 0; c < sn.nCab; ++c) {
            const SnapCable& k = sn.cab[c];
            if (k.src == src && k.dst == dst && k.srcOut == so && k.dstIn == di) {
                *amt = k.amount;
                return true;
            }
        }
        return false;
    };
    auto apply = [&](const SnapCable& k) {
        const int s0 = slotAtCell(k.src / kCells, k.src % kCells), s1 = slotAtCell(k.dst / kCells, k.dst % kCells);
        if (s0 < 0 || s1 < 0) return;
        float aa = 0.0f, bb = 0.0f;
        const bool inA = amountIn(a, k.src, k.srcOut, k.dst, k.dstIn, &aa);
        const bool inB = amountIn(b, k.src, k.srcOut, k.dst, k.dstIn, &bb);
        const float amt = lerpf(inA ? aa : 0.0f, inB ? bb : 0.0f, t);
        int c = cableFind(s0, k.srcOut, s1, k.dstIn);
        if (!inB && t >= 1.0f) {
            if (c >= 0) {
                disconnect(c);
                changed = true;
            }
            return;
        }
        if (c < 0) {
            if (k.srcOut >= def(mods_[s0].type).nOut || k.dstIn >= def(mods_[s1].type).nIn) return;
            c = connect(s0, k.srcOut, s1, k.dstIn, amt);
            changed = changed || c >= 0;
        } else {
            cables_[c].amount = amt;
        }
    };
    for (int c = 0; c < a.nCab; ++c) apply(a.cab[c]);
    for (int c = 0; c < b.nCab; ++c) apply(b.cab[c]);
    for (int p = 0; p < kPatches; ++p) mix[p].level = lerpf(a.level[p], b.level[p], t);
    if (changed || dirtyPlan_) rebuild();
}

void Engine::recallSnap(int i, uint32_t ms, uint32_t nowMs) {
    if (i < 0 || i >= kSnaps || !snaps_[i].valid) return;
    captureLive(*morphFrom_);
    morphTo_ = i;
    morphStart_ = nowMs;
    morphMs_ = ms;
    morphPos_ = 0.0f;
    if (ms == 0) morphStep(nowMs);
}

void Engine::morphStep(uint32_t nowMs) {
    if (morphTo_ < 0) return;
    const float t = morphMs_ ? float(nowMs - morphStart_) / float(morphMs_) : 1.0f;
    morphPos_ = clampf(t, 0.0f, 1.0f);
    morphBetween(*morphFrom_, snaps_[morphTo_], morphPos_);
    if (t >= 1.0f) morphTo_ = -1;
}

// ------------------------------------------------------------------ song
int Engine::songBars() const {
    int n = 0;
    for (int i = 0; i < songLen; ++i) n += song[i].morphBars + song[i].holdBars;
    return n;
}

int Engine::songStepAt(double beat, float* t, double* stepStartBar) const {
    const int total = songBars();
    if (total <= 0 || songLen <= 0) return -1;
    double bar = beat * 0.25;
    bar -= double(int32_t(bar / double(total))) * double(total);   // loops
    double at = 0.0;
    for (int i = 0; i < songLen; ++i) {
        const double len = double(song[i].morphBars + song[i].holdBars);
        if (bar < at + len || i == songLen - 1) {
            const double into = bar - at;
            const float mb = float(song[i].morphBars);
            if (t) *t = mb > 0.0f ? clampf(float(into) / mb, 0.0f, 1.0f) : 1.0f;
            if (stepStartBar) *stepStartBar = at;
            return i;
        }
        at += len;
    }
    return -1;
}

void Engine::songApply(double beat) {
    float t = 1.0f;
    const int st = songStepAt(beat, &t, nullptr);
    if (st < 0) return;
    const int prev = st == 0 ? songLen - 1 : st - 1;
    const Snapshot& a = snaps_[song[prev].snap & 7];
    const Snapshot& b = snaps_[song[st].snap & 7];
    if (!b.valid) return;
    if (st == songLastStep_ && fabsf(t - songLastT_) < 0.004f && t < 1.0f) return;
    if (st == songLastStep_ && t >= 1.0f && songLastT_ >= 1.0f) return;
    songLastStep_ = st;
    songLastT_ = t;
    morphTo_ = -1;
    morphBetween(a.valid ? a : b, b, t);
}

// ------------------------------------------------------------------ samples
void Engine::setSampleFiles(const SampleFile* files, int n) {
    nFiles_ = n > kMaxSampleFiles ? kMaxSampleFiles : n;
    for (int i = 0; i < nFiles_; ++i) files_[i] = files[i];
}

void Engine::builtinSample(int16_t* dst, uint32_t n, uint32_t* frames) { makeBuiltinSample(dst, n, frames); }

bool Engine::loadSample(int slot, lab::IStorage* fs, const char* dir) {
    Module& m = mods_[slot];
    if (!m.used || m.type != kSmpl || !m.bank) return false;
    const int want = int(m.v[0]);
    sampleFrames[slot] = 0;   // silence while loading
    sampleLoaded[slot] = want;
    sampleStereo[slot] = false;
    if (want <= 0 || want > nFiles_ || !fs) {
        uint32_t fr = 0;
        makeBuiltinSample(m.bank, kBankSamples, &fr);
        lab::copyStr(sampleName[slot], sizeof sampleName[slot], "Built-in bell");
        sampleFrames[slot] = fr;
        return want <= 0;
    }
    char path[128];
    std::snprintf(path, sizeof path, "%s/%s", dir, files_[want - 1].name);
    ot::WavImportOptions opt;
    opt.stereo = true;
    ot::WavImportResult res;
    ot::FmtStatus st = ot::wavImportFile(*fs, path, opt, m.bank, kBankSamples, res);
    if (st == ot::FmtStatus::NoSpace) {   // too long: mono, then the first part
        opt.stereo = false;
        st = ot::wavImportFile(*fs, path, opt, m.bank, kBankSamples, res);
        if (st == ot::FmtStatus::NoSpace) {
            const lab::IStorage::Handle h = fs->open(path, lab::IStorage::kRead);
            uint32_t total = 0;
            if (h >= 0) {
                ot::WavInfo info;
                ot::StorageReader rd(*fs, h);
                if (ot::wavParseHeader(rd, info))
                    total = ot::wavResampledFrames(ot::wavSourceFrames(info), info.sampleRate);
                fs->close(h);
            }
            if (total > 0) {
                opt.endMarker = uint16_t(uint64_t(65535) * (kBankSamples - 64) / total);
                st = ot::wavImportFile(*fs, path, opt, m.bank, kBankSamples, res);
            }
        }
    }
    if (st != ot::FmtStatus::Ok || res.frames < 4) {
        err_ = "Cannot read that WAV";
        return false;
    }
    lab::copyStr(sampleName[slot], sizeof sampleName[slot], files_[want - 1].name);
    sampleStereo[slot] = res.stereo;
    sampleFrames[slot] = res.frames;
    return true;
}

uint32_t Engine::bankFrames(int slot) const { return sampleFrames[slot]; }

// ------------------------------------------------------------------ files
// Format "GRAF" v1, little endian:
//   magic "GRAF", u16 version, u8 rack, u8 songOn, u8 songLen
//   8 x mix strip: f32 level, f32 pan, u8 mute
//   u8 module count, per module: u8 slot, type, patch, cell, number, nParam, f32 k[nParam], ext[64],
//                                char sample[40]
//   u16 cable count, per cable: u8 src slot, srcOut, dst slot, dstIn, f32 amount
//   8 x snapshot: u8 valid; if valid: u8 nMod, u16 nCab, per mod: u8 type, patch, cell, number,
//                 nParam, f32 k[nParam], ext[64]; per cable: u16 src key, u16 dst key, u8 out,
//                 u8 in, f32 amount; f32 level[8]
//   32 x song step: u8 snap, morphBars, holdBars
//   u32 checksum (sum of all previous bytes)
namespace {
struct Writer {
    uint8_t* p;
    uint32_t cap, n = 0;
    bool ok = true;
    void bytes(const void* s, uint32_t len) {
        if (n + len > cap) {
            ok = false;
            return;
        }
        std::memcpy(p + n, s, len);
        n += len;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        bytes(b, 2);
    }
    void u32(uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        bytes(b, 4);
    }
    void f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
};
struct Reader {
    const uint8_t* p;
    uint32_t len, n = 0;
    bool ok = true;
    void bytes(void* d, uint32_t k) {
        if (n + k > len) {
            ok = false;
            std::memset(d, 0, k);
            return;
        }
        std::memcpy(d, p + n, k);
        n += k;
    }
    uint8_t u8() {
        uint8_t v = 0;
        bytes(&v, 1);
        return v;
    }
    uint16_t u16() {
        uint8_t b[2];
        bytes(b, 2);
        return uint16_t(b[0] | (b[1] << 8));
    }
    uint32_t u32() {
        uint8_t b[4];
        bytes(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }
    float f32() {
        const uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return (f == f) ? f : 0.0f;
    }
};
constexpr uint16_t kFileVersion = 1;
}  // namespace

uint32_t Engine::serialize(uint8_t* buf, uint32_t cap) const {
    Writer w{buf, cap};
    w.bytes("GRAF", 4);
    w.u16(kFileVersion);
    w.u8(rack ? 1 : 0);
    w.u8(songOn ? 1 : 0);
    w.u8(uint8_t(songLen));
    for (int p = 0; p < kPatches; ++p) {
        w.f32(mix[p].level);
        w.f32(mix[p].pan);
        w.u8(mix[p].mute ? 1 : 0);
    }
    w.u8(uint8_t(moduleCount()));
    for (int s = 0; s < kMaxSlots; ++s) {
        const Module& m = mods_[s];
        if (!m.used) continue;
        const ModDef& d = def(m.type);
        w.u8(uint8_t(s));
        w.u8(m.type);
        w.u8(m.patch);
        w.u8(m.cell);
        w.u8(m.number);
        w.u8(d.nParam);
        for (int p = 0; p < d.nParam; ++p) w.f32(m.k[p]);
        w.bytes(m.ext, kExtBytes);
        char nm[40] = {};
        if (m.type == kSmpl) lab::copyStr(nm, sizeof nm, sampleName[s]);
        w.bytes(nm, sizeof nm);
    }
    w.u16(uint16_t(cableCount()));
    for (int c = 0; c < kMaxCables; ++c) {
        const Cable& k = cables_[c];
        if (!k.used) continue;
        w.u8(k.src);
        w.u8(k.srcOut);
        w.u8(k.dst);
        w.u8(k.dstIn);
        w.f32(k.amount);
    }
    for (int i = 0; i < kSnaps; ++i) {
        const Snapshot& s = snaps_[i];
        w.u8(s.valid ? 1 : 0);
        if (!s.valid) continue;
        w.u8(s.nMod);
        w.u16(s.nCab);
        for (int k = 0; k < s.nMod; ++k) {
            const SnapMod& m = s.mod[k];
            const ModDef& d = def(m.type);
            w.u8(m.type);
            w.u8(m.patch);
            w.u8(m.cell);
            w.u8(m.number);
            w.u8(d.nParam);
            for (int p = 0; p < d.nParam; ++p) w.f32(m.k[p]);
            w.bytes(m.ext, kExtBytes);
        }
        for (int k = 0; k < s.nCab; ++k) {
            const SnapCable& c = s.cab[k];
            w.u16(c.src);
            w.u16(c.dst);
            w.u8(c.srcOut);
            w.u8(c.dstIn);
            w.f32(c.amount);
        }
        for (int p = 0; p < kPatches; ++p) w.f32(s.level[p]);
    }
    for (int i = 0; i < kSongSteps; ++i) {
        w.u8(song[i].snap);
        w.u8(song[i].morphBars);
        w.u8(song[i].holdBars);
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < w.n && w.ok; ++i) sum += buf[i];
    w.u32(sum);
    return w.ok ? w.n : 0;
}

bool Engine::deserialize(const uint8_t* buf, uint32_t len) {
    if (len < 16 || std::memcmp(buf, "GRAF", 4) != 0) {
        err_ = "Not a GRAF file";
        return false;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 4 < len; ++i) sum += buf[i];
    const uint32_t want = uint32_t(buf[len - 4]) | (uint32_t(buf[len - 3]) << 8) | (uint32_t(buf[len - 2]) << 16) |
                          (uint32_t(buf[len - 1]) << 24);
    if (sum != want) {
        err_ = "File damaged (checksum)";
        return false;
    }
    Reader r{buf, len - 4};
    r.n = 4;
    const uint16_t ver = r.u16();
    if (ver == 0 || ver > kFileVersion) {
        err_ = "File from a newer GRAF";
        return false;
    }
    clear();
    quiesce();
    allNotesOff();
    rack = r.u8() != 0;
    songOn = r.u8() != 0;
    songLen = r.u8();
    if (songLen < 1) songLen = 1;
    if (songLen > kSongSteps) songLen = kSongSteps;
    for (int p = 0; p < kPatches; ++p) {
        mix[p].level = clampf(r.f32(), 0.0f, 1.0f);
        mix[p].pan = clampf(r.f32(), -1.0f, 1.0f);
        mix[p].mute = r.u8() != 0;
    }
    const int nMod = r.u8();
    for (int i = 0; i < nMod && r.ok; ++i) {
        const int slot = r.u8(), type = r.u8(), patch = r.u8() & 7, cell = r.u8(), number = r.u8(), np = r.u8();
        float k[kMaxParam] = {};
        for (int p = 0; p < np; ++p) {
            const float v = r.f32();
            if (p < kMaxParam) k[p] = v;
        }
        uint8_t ext[kExtBytes];
        r.bytes(ext, kExtBytes);
        char nm[40];
        r.bytes(nm, sizeof nm);
        nm[39] = 0;
        if (type >= kTypeCount || slot >= kMaxSlots || cell >= kCells) continue;
        const int s = addAt(slot, type, patch, cell);
        if (s < 0) continue;
        Module& m = mods_[s];
        m.number = uint8_t(number);
        const ModDef& d = def(type);
        for (int p = 0; p < d.nParam && p < np; ++p) m.k[p] = clampf(k[p], 0.0f, 1.0f);
        refresh(s);
        std::memcpy(m.ext, ext, kExtBytes);
        if (type == kSmpl) lab::copyStr(sampleName[s], sizeof sampleName[s], nm);
    }
    const int nCab = r.u16();
    for (int i = 0; i < nCab && r.ok; ++i) {
        const int s0 = r.u8(), so = r.u8(), s1 = r.u8(), di = r.u8();
        const float a = r.f32();
        connect(s0, so, s1, di, a);
    }
    for (int i = 0; i < kSnaps && r.ok; ++i) {
        Snapshot& s = snaps_[i];
        s.valid = r.u8() != 0;
        if (!s.valid) continue;
        s.nMod = r.u8();
        s.nCab = r.u16();
        if (s.nMod > kMaxSlots) s.nMod = kMaxSlots;
        if (s.nCab > kMaxCables) s.nCab = kMaxCables;
        for (int k = 0; k < s.nMod; ++k) {
            SnapMod& m = s.mod[k];
            m.type = r.u8();
            m.patch = r.u8() & 7;
            m.cell = r.u8();
            m.number = r.u8();
            const int np = r.u8();
            for (int p = 0; p < kMaxParam; ++p) m.k[p] = 0.0f;
            for (int p = 0; p < np; ++p) {
                const float v = r.f32();
                if (p < kMaxParam) m.k[p] = clampf(v, 0.0f, 1.0f);
            }
            r.bytes(m.ext, kExtBytes);
            if (m.type >= kTypeCount || m.cell >= kCells) m.type = 0xFF;
        }
        for (int k = 0; k < s.nCab; ++k) {
            SnapCable& c = s.cab[k];
            c.src = r.u16();
            c.dst = r.u16();
            c.srcOut = r.u8();
            c.dstIn = r.u8();
            c.amount = clampf(r.f32(), -2.0f, 2.0f);
        }
        for (int p = 0; p < kPatches; ++p) s.level[p] = clampf(r.f32(), 0.0f, 1.0f);
        if (!r.ok) s.valid = false;
    }
    for (int i = 0; i < kSongSteps && r.ok; ++i) {
        song[i].snap = r.u8() & 7;
        song[i].morphBars = r.u8();
        song[i].holdBars = r.u8();
        if (song[i].morphBars > 32) song[i].morphBars = 32;
        if (song[i].holdBars > 64) song[i].holdBars = 64;
    }
    morphTo_ = -1;
    songLastStep_ = -1;
    rebuild();
    if (!r.ok) err_ = "File cut short: loaded what was there";
    return true;
}

}  // namespace graf
