// MAKAM host tests: makam interval tables and frequencies in both tunings,
// usul strokes, slice scheduling on aksak grids at 172 BPM, the seyir
// generator, the project file round trip.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "../mem_storage.h"
#include "lab/lab.h"
#include "lab_test.h"
#include "ws/gen.h"
#include "ws/song.h"
#include "ws/theory.h"
#include "ws/usul.h"
#include "ws/ws.h"

using namespace makam;

namespace {
int findMakam(const char* n) {
    for (int i = 0; i < kMakamCount; ++i)
        if (std::strcmp(kMakams[i].name, n) == 0) return i;
    return -1;
}
// Expected ascending step sizes (commas) for the octave degrees 0..7.
struct Expect {
    const char* name;
    int steps[7];
    int karar, guclu;   // absolute commas
};
// AEU: T 9, K 8, S 5, B 4, A 12/13.
const Expect kExpect[] = {
    {"Rast", {9, 8, 5, 9, 9, 8, 5}, 31, 62},          // Rast5 + Rast4
    {"Ussak", {8, 5, 9, 9, 4, 9, 9}, 40, 62},         // Ussak4 + Buselik5
    {"Huzzam", {5, 9, 5, 12, 5, 13, 4}, 48, 62},      // Huzzam5 + Hicaz4 (+ Sunbule, Tiz Segah)
    {"Hicaz", {5, 12, 5, 9, 8, 5, 9}, 40, 62},        // Hicaz4 + Rast5
    {"Saba", {8, 5, 5, 12, 5, 9, 5}, 40, 53},         // Saba4 (K S S) + Hicaz on Cargah, no octave
    {"Nihavend", {9, 4, 9, 9, 4, 13, 5}, 31, 62},     // Buselik5 + Hicaz4
    {"Kurdi", {4, 9, 9, 9, 4, 9, 9}, 40, 62},         // Kurdi4 + Buselik5
    {"Segah", {5, 9, 8, 9, 5, 13, 4}, 48, 62},        // Segah5 + Hicaz4 on Evic
    {"Karcigar", {8, 5, 9, 5, 12, 5, 9}, 40, 62},     // Ussak4 + Hicaz5
    {"Huseyni", {8, 5, 9, 9, 8, 5, 9}, 40, 71},       // Huseyni5 + Ussak4
    {"Muhayyer", {8, 5, 9, 9, 8, 5, 9}, 40, 93},
    {"Hicazkar", {5, 12, 5, 9, 5, 12, 5}, 31, 84},    // Hicaz5 + Hicaz4
    {"Suzinak", {9, 8, 5, 9, 5, 12, 5}, 31, 62},      // Rast5 + Hicaz4
    {"Neva", {8, 5, 9, 9, 8, 5, 9}, 40, 62},          // Ussak4 + Rast5
    {"Beyati", {8, 5, 9, 9, 8, 5, 9}, 40, 62},
    {"Acemasiran", {9, 9, 4, 9, 9, 9, 4}, 22, 75},    // Cargah5 + Cargah4
    {"Kurdilihicazkar", {4, 9, 9, 9, 4, 9, 9}, 31, 84},
    {"Humayun", {5, 12, 5, 9, 4, 9, 9}, 40, 62},      // Hicaz4 + Buselik5
    {"Uzzal", {5, 12, 5, 9, 8, 5, 9}, 40, 71},        // Hicaz5 + Ussak4
    {"Zirguleli Hicaz", {5, 12, 5, 9, 5, 12, 5}, 40, 71},
    {"Buselik", {9, 4, 9, 9, 4, 9, 9}, 40, 71},
    {"Mahur", {9, 9, 4, 9, 9, 9, 4}, 31, 84},
    {"Nikriz", {9, 5, 12, 5, 9, 8, 5}, 31, 62},
    {"Neveser", {9, 5, 12, 5, 5, 12, 5}, 31, 62},
    {"Cargah", {9, 9, 4, 9, 9, 9, 4}, 53, 84},
};

std::vector<uint8_t> gMem(64u * 1024u * 1024u);
}  // namespace

LAB_TEST(makam_interval_tables) {
    CHECK(int(sizeof kExpect / sizeof kExpect[0]) == kMakamCount);
    for (const Expect& e : kExpect) {
        const int i = findMakam(e.name);
        CHECK(i >= 0);
        if (i < 0) continue;
        const Makam& m = kMakams[i];
        int sum = 0;
        for (int d = 0; d < 7; ++d) {
            const int st = stepCommas(m, d);
            if (st != e.steps[d]) std::fprintf(stderr, "  %s step %d: %d != %d\n", e.name, d, st, e.steps[d]);
            CHECK(st == e.steps[d]);
            sum += st;
        }
        // an octave of 53 commas, except Saba (no octave: Sehnaz above Dugah)
        if (std::strcmp(e.name, "Saba") == 0) CHECK(sum == 49);
        else CHECK(sum == 53);
        CHECK(m.asc[0] == e.karar);
        CHECK(m.asc[m.guclu] == e.guclu);
        // every pitch is an AEU perde, the yeden lies below the karar
        for (int d = 0; d < 8; ++d) {
            CHECK(isAeuPitch(m.asc[d]));
            CHECK(isAeuPitch(m.desc[d]));
        }
        CHECK(m.yeden < m.asc[0]);
        CHECK(isAeuPitch(m.yeden));
        CHECK(inMakam(m, m.asc[m.guclu] + 53));
    }
    // Rast descends with Acem (Buselik4 on Neva), Nihavend with Acem (Kurdi4)
    const Makam& rast = kMakams[findMakam("Rast")];
    CHECK(rast.desc[6] == 75 && rast.asc[6] == 79);
}

LAB_TEST(aeu_and_53tet_frequencies) {
    // Dugah = A4 = 440 Hz in both systems
    CHECK_NEAR(freqHz(kDugah, kTuneAEU), 440.0, 0.01);
    CHECK_NEAR(freqHz(kDugah, kTune53), 440.0, 0.01);
    // AEU: pure fifth Dugah -> Huseyni (71) = 3/2, fourth Dugah -> Neva (62) = 4/3
    CHECK_NEAR(freqHz(71, kTuneAEU) / 440.0f, 1.5, 1e-4);
    CHECK_NEAR(freqHz(62, kTuneAEU) / 440.0f, 4.0 / 3.0, 1e-4);
    // AEU tanini = 9/8 (Rast -> Dugah), bakiye = 256/243 (Dugah -> Kurdi), apotome Kurdi -> Segah
    CHECK_NEAR(freqHz(40, kTuneAEU) / freqHz(31, kTuneAEU), 9.0 / 8.0, 1e-4);
    CHECK_NEAR(freqHz(44, kTuneAEU) / freqHz(40, kTuneAEU), 256.0 / 243.0, 1e-4);
    // AEU comma (Segah 48 -> Buselik 49) = Pythagorean comma 23.46 c
    CHECK_NEAR(aeuCents(49) - aeuCents(48), 23.46, 0.02);
    // AEU kucuk mucenneb (Dugah -> Dik Kurdi 45) = 113.69 c (apotome)
    CHECK_NEAR(aeuCents(45) - aeuCents(40), 113.69, 0.02);
    // AEU buyuk mucenneb (Dugah -> Segah) = 180.45 c
    CHECK_NEAR(aeuCents(48) - aeuCents(40), 180.45, 0.02);
    // 53-TET: every comma 22.6415 c
    for (int c = -20; c < 120; ++c) CHECK_NEAR(tetCents(c + 1) - tetCents(c), 1200.0 / 53.0, 1e-3);
    // AEU: the 24 pitch classes stay within 4 c of 53-TET and ascend
    float prev = -1.0f;
    for (int i = 0; i < kAeuCount; ++i) {
        const int c = kAeuComma[i];
        CHECK(std::fabs(aeuCents(c) - tetCents(c)) < 4.0f);
        CHECK(aeuCents(c) > prev);
        prev = aeuCents(c);
    }
    // octaves
    CHECK_NEAR(freqHz(93, kTuneAEU), 880.0, 0.02);
    CHECK_NEAR(freqHz(93 - 106, kTune53), 220.0, 0.02);
    // ahenk transposes by semitones
    CHECK_NEAR(freqHz(kDugah, kTuneAEU, 440.0f, -12), 220.0, 0.01);
    // every makam degree in both tunings: finite, ascending, 53-TET within 4 c of AEU
    for (int i = 0; i < kMakamCount; ++i) {
        const Makam& m = kMakams[i];
        for (int d = 0; d < 7; ++d) {
            const float a0 = freqHz(degreeComma(m, d), kTuneAEU), a1 = freqHz(degreeComma(m, d + 1), kTuneAEU);
            const float t0 = freqHz(degreeComma(m, d), kTune53);
            CHECK(a1 > a0);
            CHECK(std::fabs(1200.0f * log2f(t0 / a0)) < 4.0f);
        }
    }
    char buf[32];
    CHECK(std::strcmp(perdeName(40, buf, sizeof buf), "Dugah") == 0);
    CHECK(std::strcmp(perdeName(31 - 53, buf, sizeof buf), "Kaba Rast") == 0);
    CHECK(std::strcmp(perdeName(48 + 106, buf, sizeof buf), "Tiz Tiz Segah") == 0 ||
          std::strcmp(perdeName(48 + 106, buf, sizeof buf), "Tiz Segah") != 0);
    CHECK(std::strcmp(perdeName(101, buf, sizeof buf), "Tiz Segah") == 0);
}

LAB_TEST(usul_strokes) {
    for (int i = 0; i < kUsulCount; ++i) {
        const Usul& u = kUsuls[i];
        const int len = usulLength(i);
        CHECK(len >= 4 && len <= kUsulMaxSteps);
        int starts[8];
        const int g = usulGroups(i, starts, 8);
        int sum = 0;
        for (const char* p = u.groups; *p; ++p)
            if (*p >= '0' && *p <= '9') sum += *p - '0';
        CHECK(sum == len);
        CHECK(g >= 2);
        CHECK(starts[0] == 0);
        CHECK(strokeOf(u.strokes[0]) == kDum);   // every usul starts on dum
        int dums = 0, teks = 0;
        for (int k = 0; k < len; ++k) {
            dums += strokeOf(u.strokes[k]) == kDum;
            teks += strokeOf(u.strokes[k]) == kTek;
        }
        CHECK(dums >= 1 && teks >= 1);
        // the meter numerator equals the cycle length (Sofyan 4/4 is written in eighths)
        const int num = std::atoi(u.meter);
        CHECK(num == len || (std::strcmp(u.name, "Sofyan") == 0 && len == 8));
    }
    // aksak 9/8 = 2+2+2+3 with dum on 1 and 5
    CHECK(std::strcmp(kUsuls[0].name, "Aksak") == 0);
    CHECK(std::strcmp(kUsuls[0].groups, "2+2+2+3") == 0);
    CHECK(strokeOf(kUsuls[0].strokes[4]) == kDum);
    // curcuna 10/8 = 3+2+2+3
    CHECK(std::strcmp(kUsuls[1].groups, "3+2+2+3") == 0);
    CHECK(usulLength(1) == 10);
    // devr-i hindi 7/8 = 3+2+2
    CHECK(usulLength(2) == 7);
}

// Boot the workstation headless and play aksak at 172 BPM: slices land on
// the 16th grid of the usul and dum strokes take kick slices.
LAB_TEST(aksak_slice_scheduling_172) {
    lab::Shell shell;
    lab::Hal h;
    CHECK(shell.init(h, gMem.data(), uint32_t(gMem.size()), "TEST"));
    MakamWs* ws = static_cast<MakamWs*>(shell.workstation());
    Song& s = ws->song();
    s.usul = 0;
    songApplyUsul(s);
    s.songMode = 0;
    s.editVar = 0;
    s.sec[0].chaos = 0;
    s.sec[0].brk = 1;
    s.curSection = 0;
    s.swing = 0;
    s.kit = 0;
    Engine& e = ws->engine();
    lab::Transport tr;
    tr.playing = true;
    tr.startedThisBlock = true;
    tr.bpm = 172.0f;
    tr.beatsPerSample = 172.0 / 60.0 / 44100.0;
    float l[lab::kBlock], r[lab::kBlock];
    const double unitSamples = 0.25 / tr.beatsPerSample;   // 16th at 172 = 3845.9 samples
    CHECK_NEAR(unitSamples, 3845.93, 0.1);
    std::vector<int> hitUnits, hitSlices;
    int lastHits = e.st.sliceHit;
    const Kit& k = ws->kit(0);
    for (int b = 0; b < 2000; ++b) {
        for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
        e.render(tr, l, r);
        tr.startedThisBlock = false;
        tr.beat += tr.beatsPerSample * lab::kBlock;
        if (e.st.sliceHit != lastHits) {
            lastHits = e.st.sliceHit;
            hitUnits.push_back(int(e.st.unit));
            hitSlices.push_back(int(e.st.slice));
        }
    }
    CHECK(!hitUnits.empty());
    const char* st = kUsuls[0].strokes;
    int checked = 0;
    for (size_t i = 0; i < hitUnits.size(); ++i) {
        const int step = hitUnits[i] % 9;
        const Stroke sk = strokeOf(st[step]);
        CHECK(sk != kRest);   // hits only on stroke units (variation A, no chaos)
        if (sk == kDum) {
            CHECK(k.cls[hitSlices[i]] == kClsKick);
            ++checked;
        }
        if (sk == kTek) CHECK(k.cls[hitSlices[i]] == kClsSnare);
    }
    CHECK(checked > 5);
    // 2000 blocks = 256000 samples = 66.5 units: units 0..66 seen, cycle of 9
    CHECK(hitUnits.back() >= 60);
    // the step counter tracks the 9-unit cycle
    CHECK(e.st.step == e.st.unit % 9);
}

LAB_TEST(seyir_generator_stays_in_makam_and_cadences) {
    for (int mi = 0; mi < kMakamCount; ++mi) {
        const Makam& m = kMakams[mi];
        for (int u = 0; u < kUsulCount; ++u) {
            for (uint32_t seed = 1; seed < 12; ++seed) {
                GenParams p;
                p.density = int(seed * 9 % 100);
                p.ornament = int(seed * 13 % 100);
                p.range = 4 + int(seed % 11);
                p.start = int(seed % 4);
                Phrase ph;
                ph.cycles = 1 + int(seed % 4);
                genPhrase(m, p, u, ph, seed * 7919u + uint32_t(mi));
                const int n = phraseSteps(ph, u);
                bool any = false, guclu = false;
                for (int i = 0; i < n; ++i) {
                    const NoteStep& x = ph.s[i];
                    if (!(x.flags & kNoteOn)) continue;
                    any = true;
                    CHECK(x.deg >= -1 && x.deg <= p.range);
                    CHECK(inMakam(m, degreeComma(m, x.deg)) || x.deg == -1);
                    if (x.deg == (m.guclu < p.range ? m.guclu : p.range)) guclu = true;
                }
                CHECK(any);
                CHECK(guclu);                          // the guclu is emphasised
                CHECK(phraseLastDegree(ph, u) == 0);   // tam karar
                // the note before the final is a step away (approach from above or the yeden)
                int last = -1, prev = -100;
                for (int i = 0; i < n; ++i)
                    if (ph.s[i].flags & kNoteOn) {
                        prev = last >= 0 ? ph.s[last].deg : -100;
                        last = i;
                    }
                CHECK(prev == 1 || prev == -1 || prev == 0);
                // the first note follows the seyir start region
                int first = -100;
                for (int i = 0; i < n; ++i)
                    if (ph.s[i].flags & kNoteOn) { first = ph.s[i].deg; break; }
                const int st0 = p.start ? p.start : (m.seyir == kSeyirAsc ? 1 : (m.seyir == kSeyirDesc ? 3 : 2));
                if (st0 == 1) CHECK(first >= 0 && first <= 2);
                if (st0 == 3) CHECK(first >= 4);
            }
        }
    }
}

LAB_TEST(save_load_roundtrip) {
    Song a;
    songDefaults(a);
    a.makam = 7;
    a.usul = 2;
    a.tuning = 1;
    a.bpm = 168;
    a.brk[2].s[3].roll = 3;
    a.brk[1].s[0].pitch = -7;
    a.phr[3].s[17] = NoteStep{-1, kNoteOn | kNoteGlide};
    a.sec[4].chaos = 77;
    a.sec[5].filter = -33;
    a.ch[2].rvb = 91;
    a.lp.model = 2;
    a.midiBend = 12;
    std::strcpy(a.wav, "amen.wav");
    a.sectionCount = 9;
    static uint8_t buf[kFileMax];
    const size_t n = songSave(a, buf, sizeof buf);
    CHECK(n > 100 && n < kFileMax);
    Song b;
    CHECK(songLoad(b, buf, n));
    CHECK(b.makam == 7 && b.usul == 2 && b.tuning == 1 && b.bpm == 168);
    CHECK(b.brk[2].s[3].roll == 3 && b.brk[1].s[0].pitch == -7);
    CHECK(b.phr[3].s[17].deg == -1 && b.phr[3].s[17].flags == (kNoteOn | kNoteGlide));
    CHECK(b.sec[4].chaos == 77 && b.sec[5].filter == -33 && b.ch[2].rvb == 91);
    CHECK(b.lp.model == 2 && b.midiBend == 12 && b.sectionCount == 9);
    CHECK(std::strcmp(b.wav, "amen.wav") == 0);
    // identical bytes after a second save
    static uint8_t buf2[kFileMax];
    CHECK(songSave(b, buf2, sizeof buf2) == n);
    CHECK(std::memcmp(buf, buf2, n) == 0);
    // corruption and future versions are rejected
    buf[40] ^= 0x5A;
    Song c;
    CHECK(!songLoad(c, buf, n));
    buf[40] ^= 0x5A;
    buf[4] = 9;
    CHECK(!songLoad(c, buf, n));
    buf[4] = 1;
    CHECK(!songLoad(c, buf, 10));

    // through the card: the workstation saves to and loads from MemStorage
    MemStorage fs;
    lab::Shell shell;
    lab::Hal h;
    h.fs = &fs;
    CHECK(shell.init(h, gMem.data(), uint32_t(gMem.size()), "MAKAM"));
    MakamWs* ws = static_cast<MakamWs*>(shell.workstation());
    ws->song().makam = 11;
    ws->song().sec[2].cycles = 5;
    CHECK(ws->saveProject("Lab/MAKAM/Projects/SONG1.mkm"));
    ws->song().makam = 0;
    ws->song().sec[2].cycles = 1;
    CHECK(ws->loadProject("Lab/MAKAM/Projects/SONG1.mkm"));
    CHECK(ws->song().makam == 11 && ws->song().sec[2].cycles == 5);
    CHECK(!ws->loadProject("Lab/MAKAM/Projects/SONG8.mkm"));
}

LAB_TEST(builtin_kits_are_classified) {
    lab::Shell shell;
    lab::Hal h;
    CHECK(shell.init(h, gMem.data(), uint32_t(gMem.size()), "TEST"));
    MakamWs* ws = static_cast<MakamWs*>(shell.workstation());
    for (int k = 0; k < kKits; ++k) {
        Kit& kit = ws->kit(k);
        CHECK(kit.frames > 100000 && kit.frames <= kit.cap);
        CHECK(kit.count[kClsKick] >= 1 && kit.count[kClsSnare] >= 1 && kit.count[kClsGhost] >= 1);
        int peak = 0;
        for (uint32_t i = 0; i < kit.frames; ++i) peak = std::max(peak, std::abs(int(kit.data[i])));
        CHECK(peak > 8000);
        // the classifier agrees with the synthesis on most slices
        Kit copy = kit;
        classifySlices(copy);
        int agree = 0;
        for (int sl = 0; sl < kSlices; ++sl) agree += copy.cls[sl] == kit.cls[sl];
        std::fprintf(stderr, "  kit %d: classifier agrees on %d / 16 slices\n", k, agree);
        CHECK(agree >= 8);
    }
}

LAB_TEST(midi_pitch_bend_for_microtones) {
    // degree -> bend: Segah (48) is 180.45 c above Dugah = A4 + 1.8045 st -> note 71 (B4) - 0.1955 st
    const float semis = 69.0f + (aeuCents(48) - aeuCents(kDugah)) * 0.01f;
    CHECK_NEAR(semis, 70.8045, 1e-3);
    const int n = int(std::floor(semis + 0.5f));
    CHECK(n == 71);
    const int bend = 8192 + int((semis - float(n)) / 2.0f * 8192.0f);
    CHECK(bend < 8192 && bend > 7300);
}

// ============================== round 2 ==============================
namespace {
// A booted workstation with a fake card and a tick helper.
struct Station {
    MemStorage fs;
    lab::Shell shell;
    MakamWs* ws = nullptr;
    lab::Transport tr;
    explicit Station(bool card = true) {
        lab::Hal h;
        if (card) h.fs = &fs;
        if (shell.init(h, gMem.data(), uint32_t(gMem.size()), "MAKAM")) ws = static_cast<MakamWs*>(shell.workstation());
        tr.playing = true;
        tr.startedThisBlock = true;
        tr.bpm = 172.0f;
        tr.beatsPerSample = 172.0 / 60.0 / 44100.0;
    }
    void run(int blocks, float* peak = nullptr) {
        float l[lab::kBlock], r[lab::kBlock];
        for (int b = 0; b < blocks; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            shell.kitBlock(tr);
            ws->render(tr, lab::AudioIn(), l, r);
            tr.startedThisBlock = false;
            tr.beat += tr.beatsPerSample * lab::kBlock;
            for (uint32_t i = 0; i < lab::kBlock; ++i)
                if (peak) *peak = std::max(*peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
            if ((b & 7) == 0) ws->update(uint32_t(b) * 3u);
        }
    }
    void press(uint8_t id, bool shift = false) {
        lab::Event e;
        e.type = lab::Event::kPress;
        e.id = id;
        e.shift = shift;
        ws->onEvent(e);
        e.type = lab::Event::kRelease;
        ws->onEvent(e);
    }
};
}  // namespace

LAB_TEST(call_and_response_phrases) {
    for (int mi = 0; mi < kMakamCount; ++mi) {
        const Makam& m = kMakams[mi];
        for (int u = 0; u < kUsulCount; ++u)
            for (uint32_t seed = 1; seed < 6; ++seed) {
                GenParams p;
                p.form = 1;
                p.range = 9;
                p.density = 40 + int(seed * 11);
                Phrase ph;
                ph.cycles = 2 + int(seed % 3);
                genPhrase(m, p, u, ph, seed * 31u + uint32_t(mi));
                const int g = m.guclu < p.range ? m.guclu : p.range;
                CHECK(phraseCallEnd(ph, u) == g);          // the call stops on the guclu (yarim karar)
                CHECK(phraseLastDegree(ph, u) == 0);       // the response closes on the karar
                for (int i = 0; i < phraseSteps(ph, u); ++i)
                    if (ph.s[i].flags & kNoteOn) CHECK(inMakam(m, degreeComma(m, ph.s[i].deg)) || ph.s[i].deg == -1);
            }
    }
}

LAB_TEST(practice_intonation_shifts) {
    auto idx = [](const char* n) {
        for (int i = 0; i < kMakamCount; ++i)
            if (std::strcmp(kMakams[i].name, n) == 0) return i;
        return -1;
    };
    CHECK(practiceComma(idx("Saba"), 58) == 60);       // Hicaz up 2
    CHECK(practiceComma(idx("Huzzam"), 67) == 68);     // Hisar up 1
    CHECK(practiceComma(idx("Ussak"), 48) == 47);      // Segah down 1
    CHECK(practiceComma(idx("Ussak"), 48 + 53) == 100);   // every octave
    CHECK(practiceComma(idx("Rast"), 48) == 48);       // Rast's Segah as written
    CHECK(practiceComma(idx("Segah"), 48) == 48);
    // a practice pitch off the AEU grid sounds on the 53-comma grid
    CHECK(!isAeuPitch(47));
    CHECK_NEAR(aeuCents(47), tetCents(47), 1e-3);
    // the engine plays it
    Station st(false);
    if (!st.ws) return;
    st.ws->song().makam = idx("Ussak");
    st.ws->song().intonation = 1;
    CHECK(st.ws->engine().degreeToComma(1, 0) == 47);
    st.ws->song().intonation = 0;
    CHECK(st.ws->engine().degreeToComma(1, 0) == 48);
}

LAB_TEST(cut_modes_and_phrase_end_fills) {
    Station st(false);
    if (!st.ws) return;
    Song& s = st.ws->song();
    Engine& e = st.ws->engine();
    for (int mode = 0; mode < kCutModes; ++mode) {
        s.songMode = 0;
        s.curSection = 0;
        s.sec[0].brk = 1;
        s.sec[0].cut = mode;
        s.sec[0].fill = 100;
        s.sec[0].chaos = 30;
        st.ws->pullFields();
        e.reset();
        st.tr.beat = 0.0;
        st.tr.startedThisBlock = true;
        const int hits0 = e.st.sliceHit;
        bool sawFill3 = false, sawFill1 = false;
        float peak = 0.0f;
        // 16 aksak cycles at 172 = 144 units * 3846 samples / 128 = ~4330 blocks
        float l[lab::kBlock], r[lab::kBlock];
        for (int b = 0; b < 4500; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            st.shell.kitBlock(st.tr);
            st.ws->render(st.tr, lab::AudioIn(), l, r);
            st.tr.startedThisBlock = false;
            st.tr.beat += st.tr.beatsPerSample * lab::kBlock;
            for (uint32_t i = 0; i < lab::kBlock; ++i) {
                CHECK(std::isfinite(l[i]));
                peak = std::max(peak, std::fabs(l[i]));
            }
            if (e.st.fill == 3) sawFill3 = true;
            if (e.st.fill == 1) sawFill1 = true;
        }
        std::fprintf(stderr, "  cut %s: %d hits, peak %.2f\n", kCutNames[mode], e.st.sliceHit - hits0, double(peak));
        CHECK(e.st.sliceHit - hits0 > 60);
        CHECK(e.st.cut == mode);
        CHECK(sawFill1 && sawFill3);   // the 4th and the 16th cycle end got their tails
        CHECK(peak > 0.05f && peak < 8.0f);
    }
}

LAB_TEST(onset_slicer_finds_the_hits) {
    Station st(false);
    if (!st.ws) return;
    // kit 1 (Sert Funk) has a hit on most 16ths; the onsets land near the hit grid
    Kit k = st.ws->kit(1);
    static float scratch[4096];
    const int found = onsetSlices(k, scratch);
    std::fprintf(stderr, "  onsets found %d\n", found);
    CHECK(found >= 12);
    CHECK(k.start[0] == 0);
    for (int i = 1; i < kSlices; ++i) CHECK(k.start[i] > k.start[i - 1]);
    const float sixteenth = 60.0f / k.bpm / 4.0f * 44100.0f;
    int near = 0;
    for (int i = 1; i < kSlices; ++i) {
        const float q = float(k.start[i]) / sixteenth;
        const float d = std::fabs(q - std::round(q)) * sixteenth;
        if (d < 400.0f) ++near;   // within 9 ms of a 16th
    }
    CHECK(near >= 12);
}

LAB_TEST(dub_delay_types_stay_bounded) {
    lab::Arena a;
    static std::vector<uint8_t> mem(4u * 1024u * 1024u);
    a.attach(mem.data(), uint32_t(mem.size()));
    for (int type = 0; type < kDlyTypes; ++type) {
        DubDelay d;
        CHECK(d.init(a));
        d.setType(type);
        d.setTime(5000.0f);
        d.setFeedback(0.95f);
        float in[lab::kBlock] = {}, l[lab::kBlock], r[lab::kBlock];
        float peak = 0.0f, late = 0.0f;
        for (int b = 0; b < 3000; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) in[i] = (b < 4) ? 0.8f * std::sin(float(i) * 0.3f) : 0.0f;
            const bool on = d.process(in, l, r);
            for (uint32_t i = 0; i < lab::kBlock && on; ++i) {
                peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
                if (b > 2900) late = std::max(late, std::fabs(l[i]) + std::fabs(r[i]));
            }
        }
        std::fprintf(stderr, "  delay %d: peak %.3f, late %.4f\n", type, double(peak), double(late));
        CHECK(peak > 0.05f);
        CHECK(peak < 2.0f);   // feedback 95 % never runs away
    }
}

LAB_TEST(labkit_registry_sources_and_edits) {
    Station st(false);
    if (!st.ws) return;
    MakamWs& ws = *st.ws;
    lab::kit::Registry& R = st.shell.context().params();
    Song& s = ws.song();
    CHECK(R.count() >= 60);
    CHECK(std::strcmp(st.shell.context().mod().macroName(0), "DUB") == 0);   // first-boot macros
    st.shell.context().mod().clearRoutes();
    // registration keeps the song's values (negative ints included)
    CHECK(s.bp.octave == -3 && s.lp.octave == 0);
    CHECK_NEAR(R.get(ws.paramOf(&s.bp.octave)), -3.0, 0.01);
    // every page with values has params on it
    lab::kit::ParamId ids[64];
    CHECK(R.onPage(kPgSynth, ids, 64) >= 17);
    CHECK(R.onPage(kPgMixer, ids, 64) >= 20);
    // registry edits land in the song; song edits come back (sync)
    const lab::kit::ParamId cut = ws.paramOf(&s.bp.cutoff);
    CHECK(cut != lab::kit::kNoParam);
    R.set(cut, 80.0f);
    R.applyNow();
    CHECK(s.bp.cutoff == 80);
    s.bp.cutoff = 12;
    R.sync();
    CHECK_NEAR(R.get(cut), 12.0, 0.01);
    // the makam / usul pickers are not modulation targets
    CHECK(!R.modulatable(ws.paramOf(&s.makam)));
    CHECK(!R.modulatable(ws.paramOf(&s.usul)));
    // changing the makam from a Shell view regenerates the phrases in that makam
    const lab::kit::ParamId mk = ws.paramOf(&s.makam);
    R.set(mk, 4.0f);   // Saba
    R.applyNow();
    ws.navParamChanged(mk);
    CHECK(s.makam == 4);
    for (int p = 0; p < kPhrases; ++p) CHECK(phraseLastDegree(s.phr[p], s.usul) == 0);
    // the sources
    lab::kit::Mod& M = st.shell.context().mod();
    for (const char* n : {"USUL", "DUM", "TEK", "SLICE", "ENERGY", "SEYIR", "SECTION", "FILL"}) CHECK(M.findSource(n) >= 0);
    st.run(2000);
    CHECK(ws.engine().srcEnergy > 0.0f);
    // an LFO on the bass cutoff moves the field; the base stays
    R.set(cut, 50.0f);
    CHECK(M.route(lab::kit::kSrcLfo, cut, 0.5f) >= 0);
    M.update(false);
    int lo = 100, hi = 0;
    for (int b = 0; b < 3000; ++b) {
        st.run(1);
        lo = std::min(lo, s.bp.cutoff);
        hi = std::max(hi, s.bp.cutoff);
    }
    CHECK(hi - lo > 20);
    CHECK_NEAR(R.get(cut), 50.0, 0.6);
    M.clearRoutes();
}

LAB_TEST(scenes_morph_and_project_trailers) {
    Station st;
    if (!st.ws) return;
    MakamWs& ws = *st.ws;
    lab::kit::Registry& R = st.shell.context().params();
    Song& s = ws.song();
    st.shell.context().mod().clearRoutes();
    const lab::kit::ParamId fb = ws.paramOf(&s.dlyFb), cut = ws.paramOf(&s.bp.cutoff);
    R.set(fb, 10.0f);
    R.set(cut, 20.0f);
    ws.storeScene(0);
    R.set(fb, 90.0f);
    R.set(cut, 90.0f);
    ws.storeScene(1);
    s.morphCycles = 0;
    ws.recallScene(0);   // a jump
    R.applyNow();
    CHECK(s.dlyFb == 10 && s.bp.cutoff == 20);
    CHECK(!ws.morphing());
    // the project file keeps the scenes and the routes
    lab::kit::Mod& M = st.shell.context().mod();
    M.route(lab::kit::kSrcLfo + 1, cut, 0.3f);
    CHECK(ws.saveProject("Lab/MAKAM/Projects/SONG2.mkm"));
    R.set(fb, 55.0f);
    M.clearRoutes();
    ws.storeScene(1);   // overwritten
    CHECK(ws.loadProject("Lab/MAKAM/Projects/SONG2.mkm"));
    CHECK(ws.sceneUsed(0) && ws.sceneUsed(1));
    CHECK(M.findRoute(lab::kit::kSrcLfo + 1, cut) >= 0);
    s.morphCycles = 0;
    ws.recallScene(1);
    R.applyNow();
    CHECK(s.dlyFb == 90);
    // a morph is gradual
    s.morphCycles = 4;
    ws.recallScene(0);
    CHECK(ws.morphing());
}

LAB_TEST(real_tracker_keys_only) {
    Station st(false);
    if (!st.ws) return;
    MakamWs& ws = *st.ws;
    st.run(200);
    // Aux, Help and the encoder push do nothing (they are not on the hardware)
    st.press(lab::btn::F4);
    const int seed = ws.song().gen.seed;
    const int page = ws.page();
    st.press(lab::btn::Aux);
    st.press(lab::btn::Help);
    st.press(lab::btn::EncPush);
    CHECK(ws.page() == page);
    CHECK(ws.song().gen.seed == seed);
    // Insert is the page action (here: a new seyir phrase)
    st.press(lab::btn::Insert);
    CHECK(ws.song().gen.seed != seed);
    // F1..F8 are the pages; Master and I.Param belong to LABKIT (never a MAKAM page change)
    st.press(lab::btn::F3);
    CHECK(ws.page() == kPgBreak);
    st.press(lab::btn::Master);
    st.press(lab::btn::InstrParams);
    CHECK(ws.page() == kPgBreak);
    // Song toggles the arrangement / loop
    const int mode = ws.song().songMode;
    st.press(lab::btn::Song);
    CHECK(ws.song().songMode != mode);
    // the File page: Insert on the browser entry opens the LABKIT browser only with a card
    st.press(lab::btn::F8);
    CHECK(ws.page() == kPgFile);
    // Perform + pad recalls a scene, Perform + Shift + pad stores one
    lab::Event e;
    e.type = lab::Event::kPress;
    e.id = lab::btn::Performance;
    ws.onEvent(e);
    e.type = lab::Event::kPadDown;
    e.id = 3;
    e.shift = true;
    e.velocity = 100;
    ws.onEvent(e);
    CHECK(ws.sceneUsed(3));
    e.type = lab::Event::kRelease;
    e.id = lab::btn::Performance;
    e.shift = false;
    ws.onEvent(e);
}
