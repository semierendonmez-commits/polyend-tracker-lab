// MAKAM -- break / bass / seyir generators (gen.h).
#include "ws/gen.h"

#include "ws/usul.h"

namespace makam {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int absi(int v) { return v < 0 ? -v : v; }
}  // namespace

int phraseSteps(const Phrase& p, int usul) {
    const int n = clampi(p.cycles, 1, 4) * usulLength(usul);
    return n > kPhraseMax ? kPhraseMax : n;
}

int phraseLastDegree(const Phrase& p, int usul) {
    const int n = phraseSteps(p, usul);
    for (int i = n - 1; i >= 0; --i)
        if (p.s[i].flags & kNoteOn) return p.s[i].deg;
    return 0;
}

// ---------------------------------------------------------------------------
void genBreakVar(Song& s, int v, uint32_t seed) {
    GenRng r(seed + uint32_t(s.usul) * 131u);
    const int len = usulLength(s.usul);
    const char* st = kUsuls[s.usul].strokes;
    int starts[8];
    const int groups = usulGroups(s.usul, starts, 8);
    const int lastGroup = groups > 0 ? starts[groups - 1] : len - 2;
    const int amt = clampi(s.editDensity, 0, 100);
    BreakVar& b = s.brk[v];
    for (int i = 0; i < kSteps; ++i) {
        BreakStep& x = b.s[i];
        x = BreakStep();
        if (i >= len) continue;
        x.stroke = strokeOf(st[i]);
        if (v >= 1 && x.stroke == kRest && r.chance(25 + amt / 3)) x.stroke = kKe;   // ghost notes
        if (v >= 2 && i >= lastGroup) {                                              // group-end fills
            if (x.stroke != kRest && r.chance(30 + amt / 2)) x.roll = 2 + r.range(3);
            if (x.stroke == kRest && r.chance(40)) { x.stroke = kTek; x.stutter = 1; }
        }
        if (v >= 3) {
            if (x.stroke == kTek && r.chance(20 + amt / 4)) x.rev = 1;
            if (x.stroke != kRest && r.chance(15 + amt / 5)) x.pitch = r.chance(50) ? -5 : 7;
            if (x.stroke == kKe && r.chance(25)) x.slice = 12 + r.range(4);
        }
    }
}

void genBass(Song& s, int v) {
    const int len = usulLength(s.usul);
    const char* st = kUsuls[s.usul].strokes;
    const Makam& m = kMakams[s.makam];
    const int g = m.guclu;
    int starts[8];
    const int groups = usulGroups(s.usul, starts, 8);
    BassPat& b = s.bass[v];
    for (int i = 0; i < kSteps; ++i) b.s[i] = NoteStep();
    auto on = [&](int i, int deg, int extra = 0) {
        if (i >= 0 && i < len) b.s[i] = NoteStep{deg, kNoteOn | extra};
    };
    switch (v) {
        case 0:   // dum roots
            for (int i = 0; i < len; ++i)
                if (strokeOf(st[i]) == kDum) on(i, 0);
            break;
        case 1:   // dum karar, tek answers on the guclu
        {
            bool alt = false;
            for (int i = 0; i < len; ++i) {
                const Stroke k = strokeOf(st[i]);
                if (k == kDum) on(i, 0, kNoteAccent);
                else if (k == kTek) { on(i, alt ? g : (g > 4 ? 4 : g), 0); alt = !alt; }
            }
            break;
        }
        case 2:   // rolling: every group, karar - guclu - asma - karar
        {
            const int seq[4] = {0, g, m.asma[0] < 0 ? -1 : m.asma[0], 0};
            for (int k = 0; k < groups; ++k) on(starts[k], seq[k & 3], k ? kNoteGlide : kNoteAccent);
            break;
        }
        default:  // half-time octave: karar held, octave jump with glide at the last group
            on(0, 0, kNoteAccent);
            if (groups > 1) on(starts[groups - 1], 7, kNoteGlide);
            break;
    }
    // Hold between onsets (sustained reese), cut one unit before a gap of 3+.
    for (int i = 1; i < len; ++i)
        if (!(b.s[i].flags & kNoteOn) && (b.s[i - 1].flags & (kNoteOn | kNoteTie))) b.s[i] = NoteStep{b.s[i - 1].deg, kNoteTie};
}

// ---------------------------------------------------------------------------
void genPhrase(const Makam& m, const GenParams& p, int usul, Phrase& out, uint32_t seed) {
    GenRng r(seed * 2654435761u + 12345u);
    const int cycles = clampi(out.cycles, 1, 4);
    out.cycles = cycles;
    const int len = usulLength(usul);
    int n = cycles * len;
    if (n > kPhraseMax) n = kPhraseMax;
    for (auto& x : out.s) x = NoteStep();
    const char* st = kUsuls[usul].strokes;
    const int hi = clampi(p.range, 4, 14);
    const int guclu = m.guclu < hi ? m.guclu : hi;
    const int dens = clampi(p.density, 0, 100);
    const int orn = clampi(p.ornament, 0, 100);

    // 1) rhythm: onsets on the strokes first.
    bool onset[kPhraseMax] = {};
    for (int i = 0; i < n; ++i) {
        const Stroke k = strokeOf(st[i % len]);
        int pr = k == kDum ? 55 + dens / 2 : (k == kTek ? 25 + dens * 2 / 3 : (k == kKe ? 10 + dens / 2 : dens / 3));
        onset[i] = r.chance(pr);
    }
    // The final note (tam karar): the last dum or tek of the last cycle in its
    // second half (single-dum usuls cadence on their last tek), held to the end.
    const int lastC = (cycles - 1) * len;
    int finalAt = -1;
    for (int i = len - 1; i >= 0 && finalAt < 0; --i) {
        const Stroke k = strokeOf(st[i]);
        if ((k == kDum || k == kTek) && lastC + i < n && lastC + i >= n / 2) finalAt = lastC + i;
    }
    if (finalAt < 3) finalAt = n - 2 > 3 ? n - 2 : n - 1;
    for (int i = finalAt + 1; i < n; ++i) onset[i] = false;
    // the approach note one or two units before the final
    int approachAt = finalAt - ((finalAt >= 4 && r.chance(50)) ? 2 : 1);
    for (int i = approachAt + 1; i < finalAt; ++i) onset[i] = false;

    // 2) waypoints (degree targets at step positions) from the seyir.
    int startDeg;
    int st0 = p.start;
    if (st0 == 0) st0 = m.seyir == kSeyirAsc ? 1 : (m.seyir == kSeyirDesc ? 3 : 2);
    if (st0 == 1) startDeg = r.chance(60) ? 0 : 1 + r.range(2);
    else if (st0 == 2) startDeg = guclu;
    else startDeg = hi >= 7 ? 7 : hi;
    const int asma = clampi(m.asma[0] < 0 ? 1 : m.asma[0], 1, hi);
    struct Way { int at, deg; };
    Way way[6];
    int ways = 0;
    // positions strictly between the start (0) and the approach
    auto addWay = [&](int at, int deg) {
        const int lo = ways ? way[ways - 1].at + 1 : 1;
        if (at < lo) at = lo;
        if (at >= approachAt) return;
        way[ways++] = Way{at, deg};
    };
    // ascending makams climb past the guclu; the others reach it from above.
    const int peak = m.seyir == kSeyirAsc ? clampi(guclu + 1 + r.range(2), 1, hi) : guclu;
    addWay(n * 2 / 5, guclu);                                   // yarim karar on the guclu
    if (m.seyir == kSeyirAsc && peak != guclu) addWay(n / 2, peak);
    addWay(n * 7 / 10, asma);                                   // asma karar
    if (ways == 0) way[ways++] = Way{approachAt > 1 ? approachAt - 1 : 0, guclu};   // short cycle: guclu anyway
    const bool yeden = m.yedenCadence ? r.chance(65) : r.chance(15);
    way[ways++] = Way{approachAt, yeden ? -1 : 1};          // approach to the karar
    way[ways++] = Way{finalAt, 0};                          // tam karar
    onset[0] = true;
    for (int w = 0; w < ways; ++w) onset[way[w].at] = true;
    if (startDeg == guclu) {}   // a guclu start already emphasises it

    // 3) melody: walk toward the next waypoint, mostly by step.
    int cur = startDeg, wi = 0, last = -1;
    for (int i = 0; i < n; ++i) {
        if (!onset[i]) continue;
        while (wi < ways && way[wi].at < i) ++wi;
        int deg;
        if (i == 0) {
            deg = startDeg;
        } else if (wi < ways && way[wi].at == i) {
            deg = way[wi].deg;   // exact waypoint
        } else {
            const int target = wi < ways ? way[wi].deg : 0;
            const int stepsLeft = wi < ways ? way[wi].at - i : 1;
            int dir = target > cur ? 1 : (target < cur ? -1 : 0);
            int mv;
            const int dist = absi(target - cur);
            if (dir == 0) mv = r.chance(50) ? 1 : -1;                     // neighbour tone
            else if (dist > stepsLeft + 1) mv = dir * 2;                   // catch up by a third
            else if (r.chance(18)) mv = -dir;                              // turn back (seyir arabesque)
            else mv = r.chance(14) ? dir * 2 : dir;
            deg = cur + mv;
            deg = clampi(deg, 0, hi);   // the yeden appears only at the cadence
        }
        int flags = kNoteOn;
        if (deg == guclu && (strokeOf(st[i % len]) == kDum || (wi < ways && way[wi].at == i))) flags |= kNoteAccent;
        if (i == finalAt) flags |= kNoteAccent;
        if (last >= 0 && i != finalAt && r.chance(orn / 2)) {
            if (absi(deg - cur) >= 1 && r.chance(55)) flags |= kNoteGlide;   // glissando from the previous note
            else if (deg < hi) flags |= kNoteGrace;                            // grace from the degree above
        }
        if (i == finalAt && r.chance(orn / 3)) flags |= kNoteGlide;   // slide into the karar
        out.s[i] = NoteStep{deg, flags};
        cur = deg;
        last = i;
    }
    // 4) ties / rests between onsets: legato mostly, breaths at group ends.
    int starts[8];
    const int groups = usulGroups(usul, starts, 8);
    for (int i = 1; i < n; ++i) {
        if (out.s[i].flags & kNoteOn) continue;
        const NoteStep prev = out.s[i - 1];
        if (!(prev.flags & (kNoteOn | kNoteTie))) continue;
        bool groupStart = false;
        for (int g = 0; g < groups; ++g)
            if (starts[g] == i % len) groupStart = true;
        const bool breath = i < finalAt && groupStart && r.chance(30 + (100 - dens) / 3);
        if (!breath) out.s[i] = NoteStep{prev.deg, kNoteTie};
    }
}

}  // namespace makam
