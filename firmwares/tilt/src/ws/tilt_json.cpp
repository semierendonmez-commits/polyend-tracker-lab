// TILT -- patterns as JSON (tilt_json.h). Main loop only.
#include "ws/tilt_json.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tilt {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
const char* const kDrainJ[2] = {"REST", "HOLD"};
const char* const kTiltJ[3] = {"FREEZE", "SHAKE", "BOTH"};
const char* const kGridJ[3] = {"BAR", "HALF", "BEAT"};

// ---- writer ---------------------------------------------------------------------------
struct Out {
    char* p;
    uint32_t cap, n = 0;
    bool ok = true;
    void put(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
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
    void str(const char* s) {   // a JSON string (ASCII; quotes and backslashes escaped)
        put("\"");
        for (; s && *s && ok; ++s) {
            const char c = *s;
            if (c == '"' || c == '\\') put("\\%c", c);
            else if (uint8_t(c) < 0x20) put(" ");
            else put("%c", c);
        }
        put("\"");
    }
};
const char* tf(bool b) { return b ? "true" : "false"; }
}  // namespace

uint32_t patternToJson(const Pattern& p, char* out, uint32_t cap) {
    Out o{out, cap};
    o.put("{\n  \"format\": \"tilt-pattern\",\n  \"version\": 1,\n  \"name\": ");
    o.str(p.name);
    o.put(",\n  \"bpm\": %.1f,\n  \"page\": %d,\n", double(p.bpmX10) * 0.1, p.ev.page);
    const Table& t = p.table;
    o.put("  \"clock\": {\"rate\": \"%s\", \"reset\": %d, \"start\": %d, \"swing\": %d},\n",
          kRates[clampi(t.rate, 0, kRateCount - 1)].name, t.resetBars, t.startStep, t.swing);
    // steps: delay and length in moves (Pinball's 1/16 units at Rate 1/16)
    o.put("  \"steps\": [\n");
    for (int s = 0; s < kSteps; ++s) {
        const Step& st = p.steps[s];
        o.put("    {\"trig\": %d, \"trigProb\": %d, \"delay\": %.4g, \"delayProb\": %d, \"velocity\": %d, "
              "\"length\": %.4g, \"bumper\": %d, \"ramp\": %d, \"degree\": %d, \"lock\": %s}%s\n",
              st.trig == kTrigMute ? -1 : st.trig, st.trigP, double(st.delay) / 12.0, st.delayP, st.vel,
              double(st.len) / 4.0, st.bumper, st.ramp, st.degree, tf(st.lock), s + 1 < kSteps ? "," : "");
    }
    o.put("  ],\n  \"pages\": [\n");
    for (int pg = 0; pg < kPages; ++pg) {
        o.put("    {\"name\": \"%c\", \"matrix\": [\n", 'A' + pg);
        for (int r = 0; r < kStates; ++r) {
            o.put("      [");
            for (int c = 0; c < kStates; ++c) o.put("%d%s", p.w[pg][r][c], c + 1 < kStates ? "," : "");
            o.put("]%s\n", r + 1 < kStates ? "," : "");
        }
        o.put("    ], \"edges\": [");
        bool first = true;
        for (int r = 0; r < kStates; ++r)
            for (int c = 0; c < kStates; ++c) {
                const Edge& e = p.e[pg][r][c];
                if (!e.tr && !e.dly && !e.vel) continue;
                o.put("%s\n      {\"from\": %d, \"to\": %d, \"transpose\": %d, \"delay\": %.4g, \"velocity\": %d}",
                      first ? "" : ",", r, c, e.tr, double(e.dly) / 12.0, e.vel);
                first = false;
            }
        o.put("%s]}%s\n", first ? "" : "\n    ", pg + 1 < kPages ? "," : "");
    }
    o.put("  ],\n  \"flippers\": [");
    for (int i = 0; i < kFlips; ++i)
        o.put("{\"step\": %d, \"note\": %d}%s", p.flips[i].step, p.flips[i].note, i + 1 < kFlips ? ", " : "");
    o.put("],\n  \"combos\": [\n");
    for (int i = 0; i < kCombos; ++i) {
        const Combo& c = p.combos[i];
        o.put("    {\"on\": %s, \"path\": [", tf(c.on));
        for (int k = 0; k < c.len; ++k) o.put("%d%s", c.path[k], k + 1 < c.len ? ", " : "");
        o.put("], \"action\": \"%s\", \"a\": %d, \"b\": %d}%s\n", kComboActionNames[clampi(c.action, 0, kComboActions - 1)],
              c.a, c.b, i + 1 < kCombos ? "," : "");
    }
    o.put("  ],\n");
    o.put("  \"tilt\": {\"maxFlips\": %d, \"hardVelocity\": %d, \"action\": \"%s\", \"beats\": %d},\n", t.tiltFlips,
          t.tiltVel, kTiltJ[clampi(t.tiltAction, 0, 2)], t.tiltBeats);
    o.put("  \"ball\": {\"drain\": \"%s\", \"save\": %d, \"gravity\": %d, \"gravityGrid\": \"%s\", \"maxBalls\": %d, "
          "\"life\": %d, \"stepNotes\": %d},\n",
          kDrainJ[clampi(t.drain, 0, 1)], t.saveBars, t.gravity, kGridJ[clampi(t.gravityGrid, 0, 2)], t.mbMax, t.mbLife,
          t.stepNoteBase);
    const Evolve& ev = p.ev;
    o.put("  \"memory\": {\"fatigue\": %d, \"reinforce\": %d, \"recover\": %d},\n", ev.fatigue, ev.reinforce, ev.recover);
    o.put("  \"mutation\": {\"amount\": %d, \"home\": %d, \"spread\": %d},\n", ev.mutate, ev.mutHome, ev.mutSpread);
    o.put("  \"meta\": {\"on\": %s, \"bars\": %d, \"matrix\": [", tf(ev.metaOn), ev.metaBars);
    for (int r = 0; r < kPages; ++r)
        o.put("[%d, %d, %d, %d]%s", ev.meta[r][0], ev.meta[r][1], ev.meta[r][2], ev.meta[r][3], r + 1 < kPages ? ", " : "");
    o.put("]},\n");
    o.put("  \"rules\": {\"anchor\": %d, \"anchorEvery\": %d, \"maxRepeat\": %d, \"noBounce\": %s},\n", ev.anchorStep,
          ev.anchorEvery, ev.maxRepeat, tf(ev.noBounce));
    o.put("  \"melody\": {\"on\": %s, \"scale\": \"%s\", \"root\": \"%s\", \"octaveUp\": %d},\n", tf(p.mel.on),
          kScales[clampi(p.mel.scale, 0, kScaleCount - 1)].name, kNoteNames[clampi(p.mel.root, 0, 11)], p.mel.octUp);
    const RandCfg& rn = p.rnd;
    o.put("  \"randomizer\": {\"amount\": %d, \"density\": %d, \"matrix\": %s, \"steps\": %s, \"lanes\": %s, "
          "\"rules\": %s, \"sound\": %s, \"rowLocks\": %d},\n",
          rn.amount, rn.density, tf(rn.tMatrix), tf(rn.tSteps), tf(rn.tLanes), tf(rn.tRules), tf(rn.tSound), rn.rowLocks);
    o.put("  \"lanes\": [\n");
    for (int i = 0; i < kCh; ++i) {
        const Channel& c = p.ch[i];
        o.put("    {\"note\": %d, \"midiChannel\": %d, \"choke\": %d, \"mute\": %s}%s\n", c.note, c.midiCh, c.choke,
              tf(c.mute), i + 1 < kCh ? "," : "");
    }
    o.put("  ],\n  \"tracker\": {\n    \"channels\": [\n");
    for (int i = 0; i < kCh; ++i) {
        const Channel& c = p.ch[i];
        o.put("      {\"engine\": \"%s\", \"params\": [", kEngineNames[clampi(c.type, 0, kEngines - 1)]);
        for (int k = 0; k < kParamsPerEngine; ++k) o.put("%d%s", c.p[k], k + 1 < kParamsPerEngine ? ", " : "");
        o.put("], \"level\": %d, \"pan\": %d, \"sendA\": %d, \"sendB\": %d, \"stem\": %d, \"solo\": %s, "
              "\"follow\": %s, \"legato\": %s, \"sample\": ",
              c.level, c.pan - 64, c.sendA, c.sendB, c.stem + 1, tf(c.solo), tf(c.follow), tf(c.legato));
        o.str(c.sample);
        o.put("}%s\n", i + 1 < kCh ? "," : "");
    }
    const Fx& fx = p.fx;
    o.put("    ],\n    \"fx\": {\"delayTime\": \"%s\", \"delayFeedback\": %d, \"delayTone\": %d, \"delayWidth\": %d, "
          "\"reverbSize\": %d, \"reverbDamp\": %d, \"reverbReturn\": %d, \"drive\": %d, \"master\": %d}\n  }\n}\n",
          kDelayTimeNames[clampi(fx.dlyTime, 0, kDelayTimeCount - 1)], fx.dlyFb, fx.dlyTone, fx.dlyWidth, fx.revSize,
          fx.revDamp, fx.revLevel, fx.drive, fx.master);
    return o.ok ? o.n : 0;
}

// ---- reader ----------------------------------------------------------------------------
namespace {

struct Json {
    const char* s;
    JsonTok* t;
    int n = 0;
};

// Tokenizer (in the spirit of jsmn): objects, arrays, strings, primitives.
int tokenize(const char* s, uint32_t len, JsonTok* tok, int cap) {
    int n = 0;
    int stack[64];
    int depth = 0;
    for (uint32_t i = 0; i < len; ++i) {
        const char c = s[i];
        switch (c) {
            case '{':
            case '[': {
                if (n >= cap || depth >= 64) return -1;
                if (depth && tok[stack[depth - 1]].type == 2) ++tok[stack[depth - 1]].size;   // objects count keys
                tok[n].type = c == '{' ? 1 : 2;
                tok[n].start = int32_t(i);
                tok[n].size = 0;
                stack[depth++] = n++;
                break;
            }
            case '}':
            case ']':
                if (!depth) return -1;
                tok[stack[depth - 1]].end = int32_t(i + 1);
                --depth;
                break;
            case '"': {
                if (n >= cap) return -1;
                const uint32_t st = ++i;
                while (i < len && s[i] != '"') {
                    if (s[i] == '\\') ++i;
                    ++i;
                }
                if (i >= len) return -1;
                tok[n].type = 3;
                tok[n].start = int32_t(st);
                tok[n].end = int32_t(i);
                tok[n].size = 0;
                // a key's value is not counted again: an object counts its keys only
                if (depth) {
                    JsonTok& par = tok[stack[depth - 1]];
                    bool isKey = false;
                    if (par.type == 1) {
                        uint32_t j = i + 1;
                        while (j < len && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) ++j;
                        isKey = j < len && s[j] == ':';
                    }
                    if (par.type == 2 || isKey) ++par.size;
                }
                ++n;
                break;
            }
            case ' ':
            case '\t':
            case '\r':
            case '\n':
            case ',':
            case ':': break;
            default: {
                if (n >= cap) return -1;
                const uint32_t st = i;
                while (i < len && s[i] != ',' && s[i] != ']' && s[i] != '}' && s[i] != ' ' && s[i] != '\n' &&
                       s[i] != '\r' && s[i] != '\t')
                    ++i;
                tok[n].type = 4;
                tok[n].start = int32_t(st);
                tok[n].end = int32_t(i);
                tok[n].size = 0;
                if (depth && tok[stack[depth - 1]].type == 2) ++tok[stack[depth - 1]].size;
                ++n;
                --i;
                break;
            }
        }
    }
    return depth ? -1 : n;
}

// Index of the token after the subtree at i.
int skip(const Json& J, int i) {
    if (i < 0 || i >= J.n) return J.n;
    const JsonTok& t = J.t[i];
    int j = i + 1;
    if (t.type == 1)
        for (int k = 0; k < t.size; ++k) j = skip(J, skip(J, j));
    else if (t.type == 2)
        for (int k = 0; k < t.size; ++k) j = skip(J, j);
    return j;
}

bool keyIs(const Json& J, int i, const char* key) {
    const JsonTok& t = J.t[i];
    const int l = t.end - t.start;
    return t.type == 3 && int(std::strlen(key)) == l && std::strncmp(J.s + t.start, key, size_t(l)) == 0;
}

// The value of `key` in the object at o, or -1.
int get(const Json& J, int o, const char* key) {
    if (o < 0 || o >= J.n || J.t[o].type != 1) return -1;
    int j = o + 1;
    for (int k = 0; k < J.t[o].size && j < J.n; ++k) {
        if (keyIs(J, j, key)) return j + 1;
        j = skip(J, j + 1);
    }
    return -1;
}

// Item i of the array at a, or -1.
int item(const Json& J, int a, int i) {
    if (a < 0 || a >= J.n || J.t[a].type != 2 || i >= J.t[a].size) return -1;
    int j = a + 1;
    for (int k = 0; k < i; ++k) j = skip(J, j);
    return j;
}

bool num(const Json& J, int i, float& v) {
    if (i < 0 || J.t[i].type != 4) return false;
    char b[24];
    const int l = J.t[i].end - J.t[i].start;
    if (l <= 0 || l >= int(sizeof b)) return false;
    std::memcpy(b, J.s + J.t[i].start, size_t(l));
    b[l] = 0;
    if (!std::strcmp(b, "true")) { v = 1.0f; return true; }
    if (!std::strcmp(b, "false") || !std::strcmp(b, "null")) { v = 0.0f; return true; }
    char* end = nullptr;
    v = std::strtof(b, &end);
    return end && end != b;
}

bool text(const Json& J, int i, char* buf, int cap) {
    if (i < 0 || J.t[i].type != 3 || cap <= 0) return false;
    int o = 0;
    for (int k = J.t[i].start; k < J.t[i].end && o + 1 < cap; ++k) {
        char c = J.s[k];
        if (c == '\\' && k + 1 < J.t[i].end) c = J.s[++k];
        buf[o++] = c;
    }
    buf[o] = 0;
    return true;
}

// Setters that keep the default when the key is missing.
template <class T>
void setInt(const Json& J, int o, const char* key, T& field, int lo, int hi, float scale = 1.0f) {
    float v;
    if (num(J, get(J, o, key), v)) {
        const float x = v * scale;
        field = T(clampi(int(x < 0.0f ? x - 0.5f : x + 0.5f), lo, hi));
    }
}
template <class T>
void setName(const Json& J, int o, const char* key, T& field, const char* const* names, int count) {
    char b[24];
    const int i = get(J, o, key);
    if (text(J, i, b, sizeof b)) {
        for (int k = 0; k < count; ++k)
            if (!std::strcmp(b, names[k])) {
                field = T(k);
                return;
            }
        return;
    }
    float v;   // a number works too
    if (num(J, i, v)) field = T(clampi(int(v), 0, count - 1));
}

}  // namespace

bool patternFromJson(Pattern& p, const char* textIn, uint32_t len, JsonTok* toks, int maxToks, char* err, int errCap) {
    auto fail = [&](const char* m) {
        if (err && errCap > 0) std::snprintf(err, size_t(errCap), "%s", m);
        return false;
    };
    Json J{textIn, toks};
    J.n = tokenize(textIn, len, toks, maxToks);
    if (J.n <= 0) return fail("not JSON (or too big)");
    if (toks[0].type != 1) return fail("not a JSON object");
    char b[64];
    const int fmt = get(J, 0, "format");
    if (fmt >= 0 && text(J, fmt, b, sizeof b) && std::strcmp(b, "tilt-pattern") != 0) return fail("not a tilt-pattern");
    const int pages = get(J, 0, "pages");
    const int single = get(J, 0, "matrix");
    if (pages < 0 && single < 0 && get(J, 0, "steps") < 0) return fail("no steps / matrix");

    defaultPattern(p);
    if (text(J, get(J, 0, "name"), b, sizeof b)) {
        std::snprintf(p.name, sizeof p.name, "%s", b);
    }
    float v;
    if (num(J, get(J, 0, "bpm"), v)) p.bpmX10 = uint16_t(clampi(int(v * 10.0f + 0.5f), 200, 4000));
    setInt(J, 0, "page", p.ev.page, 0, kPages - 1);
    const int clk = get(J, 0, "clock");
    if (clk >= 0) {
        static const char* rn[kRateCount];
        for (int i = 0; i < kRateCount; ++i) rn[i] = kRates[i].name;
        setName(J, clk, "rate", p.table.rate, rn, kRateCount);
        setInt(J, clk, "reset", p.table.resetBars, 0, 64);
        setInt(J, clk, "start", p.table.startStep, 1, 16);
        setInt(J, clk, "swing", p.table.swing, 0, 75);
    }
    const int steps = get(J, 0, "steps");
    for (int s = 0; s < kSteps; ++s) {
        const int o = item(J, steps, s);
        if (o < 0) break;
        Step& st = p.steps[s];
        st = Step();
        float t;
        if (num(J, get(J, o, "trig"), t)) st.trig = uint8_t(t < 0.0f ? kTrigMute : clampi(int(t), 0, 9));
        setInt(J, o, "trigProb", st.trigP, 0, 100);
        setInt(J, o, "delay", st.delay, 0, 23, 12.0f);
        setInt(J, o, "delayProb", st.delayP, 0, 100);
        setInt(J, o, "velocity", st.vel, 1, 127);
        setInt(J, o, "length", st.len, 1, 64, 4.0f);
        setInt(J, o, "bumper", st.bumper, 0, 4);
        setInt(J, o, "ramp", st.ramp, 0, 16);
        setInt(J, o, "degree", st.degree, -7, 21);
        setInt(J, o, "lock", st.lock, 0, 1);
    }
    auto readPage = [&](int po, int pg, int matrix) {
        if (matrix >= 0) {
            std::memset(p.w[pg], 0, sizeof p.w[pg]);
            for (int r = 0; r < kStates; ++r) {
                const int row = item(J, matrix, r);
                for (int c = 0; c < kStates; ++c) {
                    float w;
                    if (num(J, item(J, row, c), w)) p.w[pg][r][c] = uint8_t(clampi(int(w + 0.5f), 0, 100));
                }
            }
        }
        const int edges = po >= 0 ? get(J, po, "edges") : -1;
        if (edges >= 0) {
            std::memset(p.e[pg], 0, sizeof p.e[pg]);
            for (int k = 0; k < J.t[edges].size; ++k) {
                const int e = item(J, edges, k);
                float fr = -1.0f, to = -1.0f;
                if (!num(J, get(J, e, "from"), fr) || !num(J, get(J, e, "to"), to)) continue;
                const int r = int(fr), c = int(to);
                if (r < 0 || r >= kStates || c < 0 || c >= kStates) continue;
                Edge& ed = p.e[pg][r][c];
                setInt(J, e, "transpose", ed.tr, -12, 12);
                setInt(J, e, "delay", ed.dly, 0, 11, 12.0f);
                setInt(J, e, "velocity", ed.vel, -64, 63);
            }
        }
    };
    if (pages >= 0) {
        for (int pg = 0; pg < kPages; ++pg) {
            const int po = item(J, pages, pg);
            if (po < 0) break;
            readPage(po, pg, get(J, po, "matrix"));
        }
    } else if (single >= 0) {   // one matrix: every page plays it
        readPage(-1, 0, single);
        for (int pg = 1; pg < kPages; ++pg) std::memcpy(p.w[pg], p.w[0], sizeof p.w[0]);
    }
    const int fl = get(J, 0, "flippers");
    for (int i = 0; i < kFlips; ++i) {
        const int o = item(J, fl, i);
        if (o < 0) break;
        setInt(J, o, "step", p.flips[i].step, 1, 16);
        setInt(J, o, "note", p.flips[i].note, 0, 127);
    }
    const int cb = get(J, 0, "combos");
    for (int i = 0; i < kCombos; ++i) {
        const int o = item(J, cb, i);
        if (o < 0) break;
        Combo& c = p.combos[i];
        c = Combo();
        setInt(J, o, "on", c.on, 0, 1);
        const int path = get(J, o, "path");
        for (int k = 0; k < kComboLen; ++k) {
            float s;
            c.path[k] = num(J, item(J, path, k), s) ? uint8_t(clampi(int(s), 0, 16)) : 0;
        }
        comboFixLen(c);
        setName(J, o, "action", c.action, kComboActionNames, kComboActions);
        setInt(J, o, "a", c.a, 0, 127);
        setInt(J, o, "b", c.b, 0, 127);
    }
    const int tl = get(J, 0, "tilt");
    if (tl >= 0) {
        setInt(J, tl, "maxFlips", p.table.tiltFlips, 0, 16);
        setInt(J, tl, "hardVelocity", p.table.tiltVel, 0, 127);
        setName(J, tl, "action", p.table.tiltAction, kTiltJ, 3);
        setInt(J, tl, "beats", p.table.tiltBeats, 1, 16);
    }
    const int bl = get(J, 0, "ball");
    if (bl >= 0) {
        setName(J, bl, "drain", p.table.drain, kDrainJ, 2);
        setInt(J, bl, "save", p.table.saveBars, 0, 8);
        setInt(J, bl, "gravity", p.table.gravity, 0, 100);
        setName(J, bl, "gravityGrid", p.table.gravityGrid, kGridJ, 3);
        setInt(J, bl, "maxBalls", p.table.mbMax, 1, 4);
        setInt(J, bl, "life", p.table.mbLife, 4, 64);
        setInt(J, bl, "stepNotes", p.table.stepNoteBase, 0, 112);
    }
    const int me = get(J, 0, "memory");
    if (me >= 0) {
        setInt(J, me, "fatigue", p.ev.fatigue, 0, 100);
        setInt(J, me, "reinforce", p.ev.reinforce, 0, 100);
        setInt(J, me, "recover", p.ev.recover, 0, 100);
    }
    const int mu = get(J, 0, "mutation");
    if (mu >= 0) {
        setInt(J, mu, "amount", p.ev.mutate, 0, 100);
        setInt(J, mu, "home", p.ev.mutHome, 0, 100);
        setInt(J, mu, "spread", p.ev.mutSpread, 0, 100);
    }
    const int mt = get(J, 0, "meta");
    if (mt >= 0) {
        setInt(J, mt, "on", p.ev.metaOn, 0, 1);
        setInt(J, mt, "bars", p.ev.metaBars, 1, 16);
        const int m = get(J, mt, "matrix");
        for (int r = 0; r < kPages; ++r)
            for (int c = 0; c < kPages; ++c) {
                float w;
                if (num(J, item(J, item(J, m, r), c), w)) p.ev.meta[r][c] = uint8_t(clampi(int(w + 0.5f), 0, 100));
            }
    }
    const int ru = get(J, 0, "rules");
    if (ru >= 0) {
        setInt(J, ru, "anchor", p.ev.anchorStep, 0, 16);
        setInt(J, ru, "anchorEvery", p.ev.anchorEvery, 1, 64);
        setInt(J, ru, "maxRepeat", p.ev.maxRepeat, 0, 8);
        setInt(J, ru, "noBounce", p.ev.noBounce, 0, 1);
    }
    const int ml = get(J, 0, "melody");
    if (ml >= 0) {
        static const char* sn[kScaleCount];
        for (int i = 0; i < kScaleCount; ++i) sn[i] = kScales[i].name;
        setInt(J, ml, "on", p.mel.on, 0, 1);
        setName(J, ml, "scale", p.mel.scale, sn, kScaleCount);
        setName(J, ml, "root", p.mel.root, kNoteNames, 12);
        setInt(J, ml, "octaveUp", p.mel.octUp, 0, 100);
    }
    const int rd = get(J, 0, "randomizer");
    if (rd >= 0) {
        setInt(J, rd, "amount", p.rnd.amount, 0, 100);
        setInt(J, rd, "density", p.rnd.density, 0, 100);
        setInt(J, rd, "matrix", p.rnd.tMatrix, 0, 1);
        setInt(J, rd, "steps", p.rnd.tSteps, 0, 1);
        setInt(J, rd, "lanes", p.rnd.tLanes, 0, 1);
        setInt(J, rd, "rules", p.rnd.tRules, 0, 1);
        setInt(J, rd, "sound", p.rnd.tSound, 0, 1);
        setInt(J, rd, "rowLocks", p.rnd.rowLocks, 0, (1 << kStepRows) - 1);
    }
    const int ln = get(J, 0, "lanes");
    for (int i = 0; i < kCh; ++i) {
        const int o = item(J, ln, i);
        if (o < 0) break;
        setInt(J, o, "note", p.ch[i].note, 0, 127);
        setInt(J, o, "midiChannel", p.ch[i].midiCh, 0, 16);
        setInt(J, o, "choke", p.ch[i].choke, 0, 4);
        setInt(J, o, "mute", p.ch[i].mute, 0, 1);
    }
    const int tr = get(J, 0, "tracker");
    const int chs = get(J, tr, "channels");
    for (int i = 0; i < kCh; ++i) {
        const int o = item(J, chs, i);
        if (o < 0) break;
        Channel& c = p.ch[i];
        uint8_t type = c.type;
        setName(J, o, "engine", type, kEngineNames, kEngines);
        if (type != c.type) engineDefaults(c, type);
        const int pr = get(J, o, "params");
        for (int k = 0; k < kParamsPerEngine; ++k) {
            float x;
            if (num(J, item(J, pr, k), x)) c.p[k] = uint8_t(clampi(int(x), 0, 127));
        }
        setInt(J, o, "level", c.level, 0, 127);
        float pan;
        if (num(J, get(J, o, "pan"), pan)) c.pan = uint8_t(clampi(int(pan) + 64, 0, 127));
        setInt(J, o, "sendA", c.sendA, 0, 127);
        setInt(J, o, "sendB", c.sendB, 0, 127);
        float stem;
        if (num(J, get(J, o, "stem"), stem)) c.stem = uint8_t(clampi(int(stem) - 1, 0, 3));
        setInt(J, o, "solo", c.solo, 0, 1);
        setInt(J, o, "follow", c.follow, 0, 1);
        setInt(J, o, "legato", c.legato, 0, 1);
        char path[kPathLen];
        if (text(J, get(J, o, "sample"), path, sizeof path)) std::snprintf(c.sample, sizeof c.sample, "%s", path);
    }
    const int fx = get(J, tr, "fx");
    if (fx >= 0) {
        setName(J, fx, "delayTime", p.fx.dlyTime, kDelayTimeNames, kDelayTimeCount);
        setInt(J, fx, "delayFeedback", p.fx.dlyFb, 0, 95);
        setInt(J, fx, "delayTone", p.fx.dlyTone, 0, 100);
        setInt(J, fx, "delayWidth", p.fx.dlyWidth, 0, 100);
        setInt(J, fx, "reverbSize", p.fx.revSize, 0, 100);
        setInt(J, fx, "reverbDamp", p.fx.revDamp, 0, 100);
        setInt(J, fx, "reverbReturn", p.fx.revLevel, 0, 100);
        setInt(J, fx, "drive", p.fx.drive, 0, 100);
        setInt(J, fx, "master", p.fx.master, 0, 127);
    }
    sanitize(p);
    return true;
}

}  // namespace tilt
