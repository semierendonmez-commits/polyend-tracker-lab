// GRAF -- PLTS: Mutable Instruments Plaits (Emilie Gillet, MIT; the adapted
// copy in src/ws/mi, from KONSOL) as a module. A pooled voice per module (4 at
// most), two 64-frame renders per 128-frame block. CVs add to the knobs; a
// patched Trig pings Plaits' low-pass gate, a patched Level opens it like a VCA.
#include <cstring>
#include <new>

#include "core/fastrun.h"
#include "ws/graf_modules.h"
#include "ws/mi/plaits/dsp/voice.h"

namespace graf {

const char* const kPlaitsModels[24] = {"VA VCF",   "PhaseDist", "6op FM A", "6op FM B", "6op FM C", "Terrain",
                                       "StrMach",  "Chiptune",  "VirtAnlg", "Wavshapr", "2op FM",   "Formant",
                                       "Harmonic", "Wavetabl",  "Chords",   "Speech",   "Swarm",    "Noise",
                                       "Particle", "String",    "Modal",    "BassDrum", "Snare",    "HiHat"};

namespace {
// QEMU-measured M4 instructions per 128 frames (2 x 64, LPG on), per model (KONSOL's bench).
const int32_t kPlCost[24] = {38500, 51400, 32300, 31700, 31900, 41600, 57000, 18600, 41000, 33000, 54300, 53400,
                             83900, 49300, 49800, 14900, 42000, 39300, 62700, 67000, 85600, 44300, 54000, 46900};
constexpr uint32_t kPlBuf = 32768;
struct PlVoice {
    plaits::Voice* v;
    char* buf;
    plaits::Voice::Frame fr[64];
};
}  // namespace

int32_t plaitsCost(int model) {
    model = model < 0 ? 0 : (model > 23 ? 23 : model);
    return kPlCost[model] * 11 / 10 + 2500;
}

void* plaitsAlloc(lab::Arena& a) {
    void* pm = a.alloc(sizeof(PlVoice), 16);
    void* vm = a.alloc(sizeof(plaits::Voice), 16);
    char* buf = static_cast<char*>(a.alloc(kPlBuf, 16));
    if (!pm || !vm || !buf) return nullptr;
    PlVoice* p = new (pm) PlVoice();
    p->v = static_cast<plaits::Voice*>(vm);
    p->buf = buf;
    plaitsReset(p);
    return p;
}

void plaitsReset(void* voice) {
    PlVoice* p = static_cast<PlVoice*>(voice);
    if (!p) return;
    std::memset(p->buf, 0, kPlBuf);
    plaits::Voice* v = new (p->v) plaits::Voice();
    stmlib::BufferAllocator alloc(p->buf, kPlBuf);
    v->Init(&alloc);
}

OT_FASTRUN_FN void pPlts(Module& m, Proc& p) {
    PlVoice* pv = static_cast<PlVoice*>(m.big);
    float *o0 = m.out[0], *o1 = m.out[1];
    if (!pv) {
        for (uint32_t i = 0; i < kBlock; ++i) o0[i] = o1[i] = 0.0f;
        return;
    }
    const float *pit = p.in[0], *tr = p.in[1], *tim = p.in[2], *mor = p.in[3], *har = p.in[4], *lvl = p.in[5];
    plaits::Patch pa;
    pa.engine = int(m.v[0]);
    pa.harmonics = 0.0f;
    pa.frequency_modulation_amount = 0.0f;
    pa.timbre_modulation_amount = 0.0f;
    pa.morph_modulation_amount = 0.0f;
    pa.decay = m.v[6];
    pa.lpg_colour = m.v[7];
    plaits::Modulations md;
    std::memset(&md, 0, sizeof md);
    md.trigger_patched = p.has(1);
    md.level_patched = p.has(5);
    const float gain = m.v[8] * (1.0f / 32768.0f);
    for (int h = 0; h < 2; ++h) {
        const uint32_t at = uint32_t(h) * 64u;
        pa.note = 60.0f + m.v[1] * 12.0f + m.v[2] + pit[at] * 12.0f;
        pa.harmonics = clampf(m.v[3] + har[at], 0.0f, 1.0f);
        pa.timbre = clampf(m.v[4] + tim[at], 0.0f, 1.0f);
        pa.morph = clampf(m.v[5] + mor[at], 0.0f, 1.0f);
        if (md.trigger_patched) {
            float t = 0.0f;
            for (uint32_t i = at; i < at + 64; i += 4) t = tr[i] > t ? tr[i] : t;
            md.trigger = t > 0.5f ? 1.0f : 0.0f;
        }
        if (md.level_patched) md.level = clampf(lvl[at], 0.0f, 1.0f);
        pv->v->Render(pa, md, pv->fr, 64);
        for (uint32_t i = 0; i < 64; ++i) {
            o0[at + i] = float(pv->fr[i].out) * gain;
            o1[at + i] = float(pv->fr[i].aux) * gain;
        }
    }
}

}  // namespace graf
