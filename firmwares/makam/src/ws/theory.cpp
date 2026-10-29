// MAKAM -- makam theory tables (theory.h).
//
// Sources for the scales, karar, guclu, yeden and seyir: TDV Islam
// Ansiklopedisi makam articles (I. H. Ozkan), the AEU perde table (comma
// positions above Kaba Cargah) and the AEU dortlu / besli definitions:
//   Cargah 4 = T T B   (9 9 4)      Cargah 5 = T T B T
//   Buselik 4 = T B T  (9 4 9)      Buselik 5 = T B T T
//   Kurdi 4  = B T T   (4 9 9)      Kurdi 5   = B T T T
//   Ussak 4  = K S T   (8 5 9)      Huseyni 5 = K S T T
//   Hicaz 4  = S A S   (5 12 5)     Hicaz 5   = S A S T
//   Rast 4   = T K S   (9 8 5)      Rast 5    = T K S T
//   Segah 5  = S T K T (5 9 8 9)    Huzzam 5  = S T S A (5 9 5 12)
//   Nikriz 5 = T S A S (9 5 12 5)   Saba 4    = K S S (8 5 5, "eksik dortlu")
// (T tanini 9, K buyuk mucenneb 8, S kucuk mucenneb 5, B bakiye 4, A artik ikili 12/13.)
#include "ws/theory.h"

#include <cmath>
#include <cstring>

namespace makam {

// Chain of fifths: position n -> 31 n mod 53 commas. n = -12 .. +11.
const uint8_t kAeuComma[kAeuCount] = {0, 4, 5, 8, 9, 13, 14, 17, 18, 22, 23, 26,
                                      27, 30, 31, 35, 36, 39, 40, 44, 45, 48, 49, 52};
const int8_t kAeuFifths[kAeuCount] = {0, -5, 7, -10, 2, -3, 9, -8, 4, -1, 11, -6,
                                      6, -11, 1, -4, 8, -9, 3, -2, 10, -7, 5, -12};
// Ring labels: the pitch class names of the octave Rast..Gerdaniye.
const char* const kAeuShort[kAeuCount] = {"Crg", "NHc", "Hcz", "DHc", "Nev", "NHs", "His", "DHs",
                                          "Hus", "Acm", "DAc", "Evc", "Mhr", "DMh", "Rst", "NZr",
                                          "Zrg", "DZr", "Dug", "Krd", "DKr", "Seg", "Bus", "DBs"};

namespace {
inline int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Perde names of the two central octaves, absolute commas 0..105 (AEU table).
struct Named {
    uint8_t c;
    const char* n;
};
const Named kNames[] = {
    {0, "Kaba Cargah"}, {4, "Kaba Nim Hicaz"}, {5, "Kaba Hicaz"}, {8, "Kaba Dik Hicaz"}, {9, "Yegah"},
    {13, "Kaba Nim Hisar"}, {14, "Kaba Hisar"}, {17, "Kaba Dik Hisar"}, {18, "Huseyni Asiran"},
    {22, "Acem Asiran"}, {23, "Dik Acem Asiran"}, {26, "Irak"}, {27, "Gevest"}, {30, "Dik Gevest"},
    {31, "Rast"}, {35, "Nim Zirgule"}, {36, "Zirgule"}, {39, "Dik Zirgule"}, {40, "Dugah"}, {44, "Kurdi"},
    {45, "Dik Kurdi"}, {48, "Segah"}, {49, "Buselik"}, {52, "Dik Buselik"}, {53, "Cargah"},
    {57, "Nim Hicaz"}, {58, "Hicaz"}, {61, "Dik Hicaz"}, {62, "Neva"}, {66, "Nim Hisar"}, {67, "Hisar"},
    {70, "Dik Hisar"}, {71, "Huseyni"}, {75, "Acem"}, {76, "Dik Acem"}, {79, "Evic"}, {80, "Mahur"},
    {83, "Dik Mahur"}, {84, "Gerdaniye"}, {88, "Nim Sehnaz"}, {89, "Sehnaz"}, {92, "Dik Sehnaz"},
    {93, "Muhayyer"}, {97, "Sunbule"}, {98, "Dik Sunbule"}, {101, "Tiz Segah"}, {102, "Tiz Buselik"},
    {105, "Tiz Dik Buselik"},
};
const char* nameAt(int c) {
    for (const Named& n : kNames)
        if (n.c == c) return n.n;
    return nullptr;
}
void put(char* buf, int cap, int& at, const char* s) {
    while (*s && at + 1 < cap) buf[at++] = *s++;
    buf[at] = 0;
}
}  // namespace

int aeuIndex(int comma) {
    const int pc = comma - floorDiv(comma, kCommasPerOctave) * kCommasPerOctave;
    for (int i = 0; i < kAeuCount; ++i)
        if (kAeuComma[i] == pc) return i;
    return -1;
}
bool isAeuPitch(int comma) { return aeuIndex(comma) >= 0; }

float aeuCents(int comma) {
    const int oct = floorDiv(comma, kCommasPerOctave);
    const int idx = aeuIndex(comma);
    if (idx < 0) return tetCents(comma);   // not an AEU pitch: fall back to the comma grid
    // n fifths of 701.955 c, folded into [0, 1200).
    float c = float(kAeuFifths[idx]) * 701.955001f;
    c -= 1200.0f * floorf(c / 1200.0f);
    // Folding can land a pitch class near 1200 (Dik Buselik 52 = 1176.5 c): keep it in its octave.
    return c + 1200.0f * float(oct);
}

float tetCents(int comma) { return float(comma) * (1200.0f / 53.0f); }

float cents(int comma, Tuning t) { return t == kTune53 ? tetCents(comma) : aeuCents(comma); }

float freqHz(int comma, Tuning t, float refHz, int ahenk) {
    const float rel = cents(comma, t) - cents(kDugah, t) + 100.0f * float(ahenk);
    return refHz * exp2f(rel * (1.0f / 1200.0f));
}

const char* perdeName(int comma, char* buf, int cap) {
    int at = 0;
    if (cap <= 0) return buf;
    buf[0] = 0;
    // Below the table: "Kaba" + the name an octave up; above: "Tiz" + an octave down.
    int c = comma, kaba = 0, tiz = 0;
    while (c < 0) { c += 53; ++kaba; }
    while (c > 105) { c -= 53; ++tiz; }
    const char* n = nameAt(c);
    if (!n) {   // a comma between AEU perdes (53-TET only)
        put(buf, cap, at, "comma ");
        char d[8];
        int v = comma, k = 0;
        bool neg = v < 0;
        if (neg) v = -v;
        do { d[k++] = char('0' + v % 10); v /= 10; } while (v && k < 7);
        if (neg) put(buf, cap, at, "-");
        while (k) { char s[2] = {d[--k], 0}; put(buf, cap, at, s); }
        return buf;
    }
    for (int i = 0; i < kaba; ++i)
        if (!(i == 0 && std::strncmp(n, "Kaba ", 5) == 0)) put(buf, cap, at, "Kaba ");
    for (int i = 0; i < tiz; ++i) put(buf, cap, at, "Tiz ");
    put(buf, cap, at, n);
    return buf;
}

// ---------------------------------------------------------------------------
// The makam table. asc / desc: absolute commas of degrees 0..7.
// Perdes: Yegah 9, H.Asiran 18, A.Asiran 22, Irak 26, Rast 31, N.Zirgule 35,
// Zirgule 36, Dugah 40, Kurdi 44, D.Kurdi 45, Segah 48, Buselik 49, Cargah 53,
// N.Hicaz 57, Hicaz 58, Neva 62, N.Hisar 66, Hisar 67, D.Hisar 70, Huseyni 71,
// Acem 75, D.Acem 76, Evic 79, Mahur 80, Gerdaniye 84, N.Sehnaz 88,
// Sehnaz 89, Muhayyer 93, Sunbule 97, Tiz Segah 101, Tiz Buselik 102.
const Makam kMakams[kMakamCount] = {
    // name, parts, asc, desc, guclu, yeden, seyir, asma, yedenCadence
    {"Rast", "Rast5 + Rast4@Neva (desc Buselik4)", {31, 40, 48, 53, 62, 71, 79, 84},
     {31, 40, 48, 53, 62, 71, 75, 84}, 4, 26, kSeyirAsc, {1, 2}, true},
    {"Ussak", "Ussak4 + Buselik5@Neva", {40, 48, 53, 62, 71, 75, 84, 93},
     {40, 48, 53, 62, 71, 75, 84, 93}, 3, 31, kSeyirAsc, {2, 1}, false},
    {"Huzzam", "Huzzam5 + Hicaz4@Evic", {48, 53, 62, 67, 79, 84, 97, 101},
     {48, 53, 62, 67, 79, 84, 97, 101}, 2, 44, kSeyirDescAsc, {3, -2}, true},
    {"Hicaz", "Hicaz4 + Rast5@Neva", {40, 45, 57, 62, 71, 79, 84, 93},
     {40, 45, 57, 62, 71, 75, 84, 93}, 3, 31, kSeyirDescAsc, {-1, 4}, false},
    {"Saba", "Saba4 + Hicaz@Cargah (no octave)", {40, 48, 53, 58, 70, 75, 84, 89},
     {40, 48, 53, 58, 70, 75, 84, 89}, 2, 31, kSeyirAsc, {1, 5}, false},
    {"Nihavend", "Buselik5 + Hicaz4@Neva (desc Kurdi4)", {31, 40, 44, 53, 62, 66, 79, 84},
     {31, 40, 44, 53, 62, 66, 75, 84}, 4, 26, kSeyirDescAsc, {3, 1}, true},
    {"Kurdi", "Kurdi4 + Buselik5@Neva", {40, 44, 53, 62, 71, 75, 84, 93},
     {40, 44, 53, 62, 71, 75, 84, 93}, 3, 31, kSeyirAsc, {2, 1}, false},
    {"Segah", "Segah5 + Hicaz4@Evic (desc Ussak@Neva)", {48, 53, 62, 70, 79, 84, 97, 101},
     {48, 53, 62, 70, 75, 84, 93, 101}, 2, 44, kSeyirAsc, {3, 1}, true},
    {"Karcigar", "Ussak4 + Hicaz5@Neva", {40, 48, 53, 62, 67, 79, 84, 93},
     {40, 48, 53, 62, 67, 79, 84, 93}, 3, 31, kSeyirDescAsc, {2, 1}, false},
    {"Huseyni", "Huseyni5 + Ussak4@Huseyni", {40, 48, 53, 62, 71, 79, 84, 93},
     {40, 48, 53, 62, 71, 75, 84, 93}, 4, 31, kSeyirDescAsc, {3, 2}, false},
    {"Muhayyer", "Huseyni5 + Ussak4@Huseyni", {40, 48, 53, 62, 71, 79, 84, 93},
     {40, 48, 53, 62, 71, 75, 84, 93}, 7, 31, kSeyirDesc, {3, 4}, false},
    {"Hicazkar", "Hicaz5@Rast + Hicaz4@Neva", {31, 36, 48, 53, 62, 67, 79, 84},
     {31, 36, 48, 53, 62, 67, 75, 84}, 7, 26, kSeyirDesc, {4, 3}, true},
    {"Suzinak", "Rast5 + Hicaz4@Neva", {31, 40, 48, 53, 62, 67, 79, 84},
     {31, 40, 48, 53, 62, 67, 79, 84}, 4, 26, kSeyirDescAsc, {3, 1}, false},
    {"Neva", "Ussak4 + Rast5@Neva", {40, 48, 53, 62, 71, 79, 84, 93},
     {40, 48, 53, 62, 71, 75, 84, 93}, 3, 31, kSeyirDescAsc, {5, 2}, false},
    {"Beyati", "Ussak4 + Buselik5@Neva (Evic up)", {40, 48, 53, 62, 71, 79, 84, 93},
     {40, 48, 53, 62, 71, 75, 84, 93}, 3, 31, kSeyirDescAsc, {2, 4}, false},
    {"Acemasiran", "Cargah5@A.Asiran + Cargah4@Cargah", {22, 31, 40, 44, 53, 62, 71, 75},
     {22, 31, 40, 44, 53, 62, 71, 75}, 7, 18, kSeyirDesc, {4, 2}, false},
    {"Kurdilihicazkar", "Kurdi4@Rast + Buselik5@Cargah", {31, 35, 44, 53, 62, 66, 75, 84},
     {31, 35, 44, 53, 62, 66, 75, 84}, 7, 22, kSeyirDesc, {4, 3}, false},
    {"Humayun", "Hicaz4 + Buselik5@Neva", {40, 45, 57, 62, 71, 75, 84, 93},
     {40, 45, 57, 62, 71, 75, 84, 93}, 3, 31, kSeyirDescAsc, {-1, 4}, false},
    {"Uzzal", "Hicaz5 + Ussak4@Huseyni", {40, 45, 57, 62, 71, 79, 84, 93},
     {40, 45, 57, 62, 71, 79, 84, 93}, 4, 31, kSeyirDescAsc, {3, 2}, false},
    {"Zirguleli Hicaz", "Hicaz5 + Hicaz4@Huseyni", {40, 45, 57, 62, 71, 76, 88, 93},
     {40, 45, 57, 62, 71, 76, 88, 93}, 4, 31, kSeyirDescAsc, {3, -1}, false},
    {"Buselik", "Buselik5 + Kurdi4@Huseyni", {40, 49, 53, 62, 71, 75, 84, 93},
     {40, 49, 53, 62, 71, 75, 84, 93}, 4, 35, kSeyirAsc, {2, 3}, true},
    {"Mahur", "Cargah5@Rast + Cargah4@Neva (desc Acem)", {31, 40, 49, 53, 62, 71, 80, 84},
     {31, 40, 49, 53, 62, 71, 75, 84}, 7, 26, kSeyirDesc, {4, 1}, true},
    {"Nikriz", "Nikriz5@Rast + Rast4@Neva (desc Buselik4)", {31, 40, 45, 57, 62, 71, 79, 84},
     {31, 40, 45, 57, 62, 71, 75, 84}, 4, 26, kSeyirAsc, {3, 1}, false},
    {"Neveser", "Nikriz5@Rast + Hicaz4@Neva", {31, 40, 45, 57, 62, 67, 79, 84},
     {31, 40, 45, 57, 62, 67, 79, 84}, 4, 26, kSeyirDescAsc, {3, 1}, false},
    {"Cargah", "Cargah5 + Cargah4@Gerdaniye", {53, 62, 71, 75, 84, 93, 102, 106},
     {53, 62, 71, 75, 84, 93, 102, 106}, 4, 49, kSeyirAsc, {2, 5}, true},
};

const PracticeShift kPractice[kPracticeShifts] = {
    {"Saba", 5, 2},          // Hicaz -> about Dik Hicaz (TDV: 2-2.5 commas sharper)
    {"Huzzam", 14, 1},       // Hisar 1 comma sharp (TDV: 1-1.5)
    {"Karcigar", 14, 1},     // the same Hisar in the hicaz pentachord on Neva
    {"Suzinak", 14, 0},      // (written: the TDV raises it only for a huzzam cadence)
    {"Ussak", 48, -1},       // Segah about a comma low in the Ussak family
    {"Beyati", 48, -1},
    {"Huseyni", 48, -1},
    {"Muhayyer", 48, -1},
    {"Neva", 48, -1},
    {"Karcigar", 48, -1},
    {"Saba", 48, -1},
    {"Segah", 48, 0},        // Segah's own karar stays where it is written
};

int practiceComma(int makamIndex, int comma) {
    if (makamIndex < 0 || makamIndex >= kMakamCount) return comma;
    const char* n = kMakams[makamIndex].name;
    const int pc = comma - floorDiv(comma, kCommasPerOctave) * kCommasPerOctave;
    for (const PracticeShift& p : kPractice)
        if (p.pitchClass == pc && std::strcmp(p.makam, n) == 0) return comma + p.delta;
    return comma;
}

const char* seyirName(uint8_t s) {
    switch (s) {
        case kSeyirAsc: return "Ascending";
        case kSeyirDesc: return "Descending";
        default: return "Desc-Asc";
    }
}

int degreeComma(const Makam& m, int degree, bool descending) {
    const int8_t* t = descending ? m.desc : m.asc;
    // degrees 0..7 from the table, then period 7 with +53 commas.
    if (degree >= 0 && degree <= 7) return t[degree];
    int oct = 0;
    int d = degree;
    while (d > 7) { d -= 7; ++oct; }
    if (degree == -1) return m.yeden;   // below the karar: the yeden (Saba has no octave)
    while (d < 0) { d += 7; --oct; }
    return t[d] + 53 * oct;
}

int stepCommas(const Makam& m, int i, bool descending) {
    return degreeComma(m, i + 1, descending) - degreeComma(m, i, descending);
}

bool inMakam(const Makam& m, int comma) {
    for (int d = 0; d < 7; ++d) {
        const int a = m.asc[d], b = m.desc[d];
        if ((comma - a) % 53 == 0 || (comma - b) % 53 == 0) return true;
    }
    // Saba: degree 7 is not the octave of the karar.
    if ((comma - m.asc[7]) % 53 == 0 || (comma - m.desc[7]) % 53 == 0) return true;
    return false;
}

int nearestDegree(const Makam& m, int comma) {
    int best = 0, bestD = 1 << 20;
    for (int d = -14; d <= 21; ++d) {
        int diff = degreeComma(m, d) - comma;
        if (diff < 0) diff = -diff;
        if (diff < bestD) { bestD = diff; best = d; }
    }
    return best;
}

}  // namespace makam
