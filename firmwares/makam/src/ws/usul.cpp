// MAKAM -- usul table (usul.h). Stroke strings render each usul's kudum
// pattern with one stroke per time unit (long strokes "duum" / "teek"
// become the stroke followed by rests). Group sums equal the cycle length.
#include "ws/usul.h"

#include <cstring>

namespace makam {

const Usul kUsuls[kUsulCount] = {
    {"Aksak", "9/8", "2+2+2+3", "D.TkD.T.T"},           // Dum Te-ke Dum Tek Tek
    {"Curcuna", "10/8", "3+2+2+3", "DTkD.T.T.."},       // Dum Te Ka Dum Tek Tek
    {"Devr-i Hindi", "7/8", "3+2+2", "D.kT.T."},        // Dum Ke Tek Tek
    {"Sofyan", "4/4", "4+2+2", "D...T.T."},             // Duum Tek Tek (quarter = 2 units)
    {"Duyek", "8/8", "2+1+2+1+2", "DT.TD.T."},          // Dum Teek Tek Dum Teek
    {"Semai", "6/8", "3+3", "D.kD.T"},                  // yuruk semai: Dum Ke Dum Tek
    {"Aksak Semai", "10/8", "3+2+2+3", "D..TkD.T.T"},   // Duum Te Ka Duum Teek Tek
    {"Devr-i Turan", "7/8", "2+2+3", "D.T.Tk."},        // Dum Tek Tek-ka
    {"Turk Aksagi", "5/8", "2+3", "D.TT."},             // Dum Tek Teek
    {"Raks Aksagi", "9/8", "2+3+2+2", "D.TT.D.TT"},     // turk aksagi + sofyan (5 + 4)
    {"Musemmen", "8/8", "3+3+2", "D.kD.kT."},           // Dum Ke Dum Ke Tek
    {"Tek Vurus", "11/8", "2+2+2+2+3", "D.T.k.D.T.k"},
    {"Frenkcin", "12/8", "3+3+2+2+2", "D.kD.kT.T.k."},
};

int usulLength(int index) {
    if (index < 0 || index >= kUsulCount) return 8;
    return int(std::strlen(kUsuls[index].strokes));
}

Stroke strokeOf(char c) {
    switch (c) {
        case 'D': return kDum;
        case 'T': return kTek;
        case 'k':
        case 't': return kKe;
        default: return kRest;
    }
}

char strokeChar(Stroke s) {
    switch (s) {
        case kDum: return 'D';
        case kTek: return 'T';
        case kKe: return 'k';
        default: return '.';
    }
}

int usulGroups(int index, int* starts, int cap) {
    if (index < 0 || index >= kUsulCount) return 0;
    const char* g = kUsuls[index].groups;
    int n = 0, at = 0, v = 0;
    for (const char* p = g;; ++p) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
        } else {
            if (n < cap) starts[n] = at;
            ++n;
            at += v;
            v = 0;
            if (!*p) break;
        }
    }
    return n < cap ? n : cap;
}

}  // namespace makam
