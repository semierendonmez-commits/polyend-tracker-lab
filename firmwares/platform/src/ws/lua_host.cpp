// PLATFORM -- Lua script host (lua_host.h, MANUAL.md "Lua API").
#include "ws/lua_host.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "ws/lua_config.h"
#include "ws/lua_prelude.h"

#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
#include "../../third_party/lua/lualib.h"

namespace {
plat::LuaSession* gPrint = nullptr;
uint32_t gSeed = 0x2545F491u;
}  // namespace

extern "C" void platLuaWrite(const char* s, size_t n) {
    if (!gPrint || !s) return;
    if (n == 0) n = std::strlen(s);
    gPrint->print(s, uint32_t(n));
}
extern "C" void platLuaWriteLine(void) {
    if (gPrint) gPrint->printEnd();
}
extern "C" unsigned int platLuaSeed(void) {
    gSeed ^= gSeed << 13;
    gSeed ^= gSeed >> 17;
    gSeed ^= gSeed << 5;
    return gSeed;
}

namespace plat {

namespace {

enum DrawCode : uint8_t {
    dColor = 1, dAlpha, dLineWidth, dPointSize, dClip, dNoClip, dFill, dRound, dFrame, dLine, dPoint, dRing, dArc,
    dText, dBatch, dNumber
};

struct KeyName {
    uint8_t id;
    const char* name;
};
const KeyName kKeys[] = {
    {lab::btn::F1, "F1"}, {lab::btn::F2, "F2"}, {lab::btn::F3, "F3"}, {lab::btn::F4, "F4"},
    {lab::btn::F5, "F5"}, {lab::btn::F6, "F6"}, {lab::btn::F7, "F7"}, {lab::btn::F8, "F8"},
    {lab::btn::Up, "UP"}, {lab::btn::Down, "DOWN"}, {lab::btn::Left, "LEFT"}, {lab::btn::Right, "RIGHT"},
    {lab::btn::Shift, "SHIFT"},
    {lab::btn::Copy, "COPY"}, {lab::btn::Insert, "INSERT"}, {lab::btn::Delete, "DELETE"}, {lab::btn::Rec, "REC"},
    {lab::btn::Note, "NOTE"}, {lab::btn::Instr, "INSTR"}, {lab::btn::Fx1, "FX1"}, {lab::btn::Fx2, "FX2"},
    {lab::btn::Pattern, "PATTERN"}, {lab::btn::Song, "SONG"}, {lab::btn::Performance, "PERFORMANCE"},
    {lab::btn::File, "FILE"}, {lab::btn::Master, "MASTER"}, {lab::btn::InstrParams, "INSTRPARAMS"},
    {lab::btn::SampleLoader, "SAMPLELOADER"}, {lab::btn::SamplePlayback, "SAMPLEPLAYBACK"},
    {lab::btn::SampleEditor, "SAMPLEEDITOR"}, {lab::btn::SampleRecorder, "SAMPLERECORDER"},
};

LuaSession* S(lua_State* L) { return LuaSession::from(L); }

void* luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)osize;
    Heap* h = static_cast<Heap*>(ud);
    if (nsize == 0) {
        h->free(ptr);
        return nullptr;
    }
    return h->realloc(ptr, uint32_t(nsize));
}

void hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    LuaSession* s = S(L);
    s->insn += 1000;
    if (s->insnLimit && s->insn > s->insnLimit)
        luaL_error(L, "script ran too long (over %d VM instructions in one call) - endless loop?", int(s->insnLimit));
}

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) msg = luaL_typename(L, 1);
    // Without the debug library: walk the stack for "file:line" frames.
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, msg);
    lua_Debug ar;
    int lvl = 1, n = 0;
    while (n < 6 && lua_getstack(L, lvl++, &ar)) {
        if (!lua_getinfo(L, "Sln", &ar)) break;
        if (ar.currentline <= 0) continue;
        char line[96];
        std::snprintf(line, sizeof line, "\n  at %s:%d%s%s", ar.short_src, ar.currentline, ar.name ? " in " : "",
                      ar.name ? ar.name : "");
        luaL_addstring(&b, line);
        ++n;
    }
    luaL_pushresult(&b);
    return 1;
}

double numArg(lua_State* L, int i, double def) { return lua_isnoneornil(L, i) ? def : double(luaL_checknumber(L, i)); }

uint32_t colorArgs(lua_State* L, int first) {
    if (lua_gettop(L) >= first + 2) {
        const int r = int(luaL_checknumber(L, first)), g = int(luaL_checknumber(L, first + 1)),
                  b = int(luaL_checknumber(L, first + 2));
        auto c = [](int v) { return uint32_t(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        return (c(r) << 16) | (c(g) << 8) | c(b);
    }
    return uint32_t(luaL_checkinteger(L, first)) & 0xFFFFFFu;
}

// ---- screen -------------------------------------------------------------------
DrawOp* addOp(lua_State* L, uint8_t code) {
    DrawList& d = S(L)->back();
    if (d.n >= uint32_t(LuaSession::kMaxOps)) {
        d.overflow = true;
        return nullptr;
    }
    DrawOp* o = &d.ops[d.n++];
    *o = DrawOp();
    o->op = code;
    return o;
}

int s_color(lua_State* L) {
    if (DrawOp* o = addOp(L, dColor)) o->u = colorArgs(L, 1);
    return 0;
}
int s_level(lua_State* L) {   // norns-style grey level 0..15
    int l = int(luaL_checknumber(L, 1));
    l = l < 0 ? 0 : (l > 15 ? 15 : l);
    const uint32_t v = uint32_t(l * 17);
    if (DrawOp* o = addOp(L, dColor)) o->u = (v << 16) | (v << 8) | v;
    return 0;
}
int s_alpha(lua_State* L) {
    if (DrawOp* o = addOp(L, dAlpha)) o->u = uint32_t(luaL_checknumber(L, 1)) & 255u;
    return 0;
}
int s_lineWidth(lua_State* L) {
    if (DrawOp* o = addOp(L, dLineWidth)) o->a = float(luaL_checknumber(L, 1));
    return 0;
}
int s_clip(lua_State* L) {
    if (DrawOp* o = addOp(L, dClip)) {
        o->a = float(luaL_checknumber(L, 1));
        o->b = float(luaL_checknumber(L, 2));
        o->c = float(luaL_checknumber(L, 3));
        o->d = float(luaL_checknumber(L, 4));
    }
    return 0;
}
int s_noclip(lua_State* L) {
    addOp(L, dNoClip);
    return 0;
}
int rect4(lua_State* L, uint8_t code) {
    if (DrawOp* o = addOp(L, code)) {
        o->a = float(luaL_checknumber(L, 1));
        o->b = float(luaL_checknumber(L, 2));
        o->c = float(luaL_checknumber(L, 3));
        o->d = float(luaL_checknumber(L, 4));
        o->e = float(numArg(L, 5, 6.0));
    }
    return 0;
}
int s_rect(lua_State* L) { return rect4(L, dFill); }
int s_frame(lua_State* L) { return rect4(L, dFrame); }
int s_round(lua_State* L) { return rect4(L, dRound); }
int s_line(lua_State* L) { return rect4(L, dLine); }
int s_circle(lua_State* L) {
    if (DrawOp* o = addOp(L, dPoint)) {
        o->a = float(luaL_checknumber(L, 1));
        o->b = float(luaL_checknumber(L, 2));
        o->c = float(luaL_checknumber(L, 3));
    }
    return 0;
}
int s_ring(lua_State* L) {
    if (DrawOp* o = addOp(L, dRing)) {
        o->a = float(luaL_checknumber(L, 1));
        o->b = float(luaL_checknumber(L, 2));
        o->c = float(luaL_checknumber(L, 3));
        o->u = uint32_t(numArg(L, 4, 24));
    }
    return 0;
}
int s_arc(lua_State* L) {
    if (DrawOp* o = addOp(L, dArc)) {
        o->a = float(luaL_checknumber(L, 1));
        o->b = float(luaL_checknumber(L, 2));
        o->c = float(luaL_checknumber(L, 3));
        o->d = float(luaL_checknumber(L, 4));
        o->e = float(luaL_checknumber(L, 5));
        o->u = uint32_t(numArg(L, 6, 16));
    }
    return 0;
}
int textAt(lua_State* L, uint16_t align) {
    LuaSession* s = S(L);
    const float x = float(luaL_checknumber(L, 1)), y = float(luaL_checknumber(L, 2));
    size_t n = 0;
    const char* str = luaL_tolstring(L, 3, &n);
    if (n > 120) n = 120;
    DrawList& d = s->back();
    if (d.nt + n + 1 > uint32_t(LuaSession::kMaxText)) {
        d.overflow = true;
        return 0;
    }
    DrawOp* o = addOp(L, dText);
    if (!o) return 0;
    o->a = x;
    o->b = y;
    o->font = s->curFont;
    o->opt = uint16_t(align | s->curAlign);
    o->u = d.nt;
    std::memcpy(d.text + d.nt, str, n);
    d.text[d.nt + n] = 0;
    d.nt += uint32_t(n) + 1;
    return 0;
}
int s_text(lua_State* L) { return textAt(L, 0); }
int s_textCenter(lua_State* L) { return textAt(L, lab::Canvas::kCenter); }
int s_textRight(lua_State* L) { return textAt(L, lab::Canvas::kRight); }
int s_font(lua_State* L) {
    LuaSession* s = S(L);
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* f = lua_tostring(L, 1);
        static const char* const names[] = {"tiny", "small", "body", "large", "title", "huge"};
        for (int i = 0; i < 6; ++i)
            if (!std::strcmp(f, names[i])) s->curFont = uint8_t(26 + i);
    } else {
        int f = int(luaL_checknumber(L, 1));
        if (f >= 1 && f <= 6) f += 25;   // 1..6 = tiny..huge
        s->curFont = uint8_t(f < 16 ? 16 : (f > 31 ? 31 : f));
    }
    return 0;
}
int s_valign(lua_State* L) {   // screen.middle(true): text y is the vertical centre
    S(L)->curAlign = lua_toboolean(L, 1) ? lab::Canvas::kCenterY : 0;
    return 0;
}
int batch(lua_State* L, int kind) {
    luaL_checktype(L, 1, LUA_TTABLE);
    LuaSession* s = S(L);
    DrawList& d = s->back();
    const lua_Integer n = luaL_len(L, 1) / 2;
    if (n <= 0) return 0;
    if (d.nv + uint32_t(n) * 2 > uint32_t(LuaSession::kMaxVerts) * 2) {
        d.overflow = true;
        return 0;
    }
    DrawOp* o = addOp(L, dBatch);
    if (!o) return 0;
    o->font = uint8_t(kind);
    o->u = d.nv;
    o->opt = uint16_t(n);
    o->a = float(numArg(L, 2, kind == 1 ? 2.0 : 1.0));
    for (lua_Integer i = 1; i <= n * 2; ++i) {
        lua_geti(L, 1, i);
        d.verts[d.nv++] = float(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return 0;
}
int s_lines(lua_State* L) { return batch(L, 0); }
int s_points(lua_State* L) { return batch(L, 1); }
int s_rects(lua_State* L) { return batch(L, 2); }
int s_polyline(lua_State* L) { return batch(L, 3); }
int s_number(lua_State* L) {
    LuaSession* s = S(L);
    if (DrawOp* o = addOp(L, dNumber)) {
        o->a = float(luaL_checknumber(L, 1));
        o->b = float(luaL_checknumber(L, 2));
        o->u = uint32_t(int32_t(luaL_checknumber(L, 3)));
        o->font = s->curFont;
        o->opt = s->curAlign;
    }
    return 0;
}
int s_update(lua_State* L) {
    S(L)->commit();
    return 0;
}
int s_fps(lua_State* L) {
    float f = float(luaL_checknumber(L, 1));
    S(L)->fps = f < 0.0f ? 0.0f : (f > 30.0f ? 30.0f : f);
    return 0;
}
int s_dirty(lua_State* L) {
    S(L)->redrawRequested = true;
    return 0;
}
int s_textWidth(lua_State* L) {   // estimate: ROM font advance ~ 0.55 x height
    size_t n = 0;
    luaL_checklstring(L, 1, &n);
    static const float kH[] = {12, 14, 17, 20, 24, 30};
    const int f = S(L)->curFont;
    const float h = (f >= 26 && f <= 31) ? kH[f - 26] : 16.0f;
    lua_pushinteger(L, lua_Integer(float(n) * h * 0.55f + 0.5f));
    return 1;
}

// ---- grid / pads ------------------------------------------------------------------
void setPad(LuaSession* s, int pad, uint32_t rgb) {
    if (pad < 0 || pad >= lab::kPads) return;
    s->padRgb[pad][0] = uint8_t(rgb >> 16);
    s->padRgb[pad][1] = uint8_t(rgb >> 8);
    s->padRgb[pad][2] = uint8_t(rgb);
}
uint32_t levelColor(uint32_t base, int level) {
    level = level < 0 ? 0 : (level > 15 ? 15 : level);
    auto ch = [&](int sh) { return ((((base >> sh) & 255u) * uint32_t(level)) / 15u) << sh; };
    return ch(16) | ch(8) | ch(0);
}
int g_led(lua_State* L) {
    LuaSession* s = S(L);
    const int x = int(luaL_checkinteger(L, 1)), y = int(luaL_checkinteger(L, 2));
    if (x < 1 || x > lab::kPadCols || y < 1 || y > lab::kPadRows) return 0;
    uint32_t c;
    if (lua_gettop(L) >= 5) c = colorArgs(L, 3);
    else c = levelColor(s->gridColor, int(luaL_checknumber(L, 3)));
    setPad(s, (y - 1) * lab::kPadCols + (x - 1), c);
    return 0;
}
int g_all(lua_State* L) {
    LuaSession* s = S(L);
    const uint32_t c = lua_gettop(L) >= 3 ? colorArgs(L, 1) : levelColor(s->gridColor, int(numArg(L, 1, 0)));
    for (int i = 0; i < lab::kPads; ++i) setPad(s, i, c);
    return 0;
}
int g_color(lua_State* L) {
    S(L)->gridColor = colorArgs(L, 1);
    return 0;
}
int g_nop(lua_State*) { return 0; }
int p_led(lua_State* L) {
    const int n = int(luaL_checkinteger(L, 1));
    setPad(S(L), n - 1, lua_gettop(L) >= 4 ? colorArgs(L, 2) : uint32_t(luaL_checkinteger(L, 2)));
    return 0;
}

// ---- engine -----------------------------------------------------------------------
int voiceArg(lua_State* L, int i) {
    const int v = int(luaL_checkinteger(L, i));
    luaL_argcheck(L, v >= 1 && v <= kVoices, i, "voice must be 1..16");
    return v - 1;
}
double when(LuaSession* s) { return s->schedBeat; }
float velArg(lua_State* L, int i) {
    float v = float(numArg(L, i, 1.0));
    if (v > 1.0f) v *= 1.0f / 127.0f;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
int e_note(lua_State* L) {
    LuaSession* s = S(L);
    const int v = voiceArg(L, 1);
    s->engine()->noteOn(v, float(luaL_checknumber(L, 2)), velArg(L, 3), when(s), false);
    return 0;
}
int e_hz(lua_State* L) {
    LuaSession* s = S(L);
    const int v = voiceArg(L, 1);
    s->engine()->noteOn(v, float(luaL_checknumber(L, 2)), velArg(L, 3), when(s), true);
    return 0;
}
int e_off(lua_State* L) {
    LuaSession* s = S(L);
    s->engine()->noteOff(voiceArg(L, 1), when(s));
    return 0;
}
int e_trig(lua_State* L) {   // note on now (or at the clock time) and off `dur` seconds later
    LuaSession* s = S(L);
    const int v = voiceArg(L, 1);
    const float note = float(luaL_checknumber(L, 2));
    const float vel = velArg(L, 3);
    const double dur = numArg(L, 4, 0.1);
    double w = when(s);
    s->engine()->noteOn(v, note, vel, w, false);
    if (w < 0.0) w = s->beatNow();
    s->engine()->noteOff(v, w + dur * s->bps());
    return 0;
}
int paramArg(lua_State* L, int i) {
    if (lua_type(L, i) == LUA_TNUMBER) {
        const int p = int(lua_tointeger(L, i));
        luaL_argcheck(L, p >= 0 && p < pParamCount, i, "unknown parameter");
        return p;
    }
    const int p = paramIndex(luaL_checkstring(L, i));
    if (p < 0) luaL_error(L, "engine: unknown parameter '%s'", lua_tostring(L, i));
    return p;
}
float paramValueArg(lua_State* L, int p, int i) {
    if (p == pWave && lua_type(L, i) == LUA_TSTRING) {
        const int w = waveIndex(lua_tostring(L, i));
        if (w < 0) luaL_error(L, "engine: unknown wave '%s'", lua_tostring(L, i));
        return float(w);
    }
    if ((p == pFtype || p == pEnvMode) && lua_type(L, i) == LUA_TSTRING) {
        const char* t = lua_tostring(L, i);
        static const char* const ft[] = {"lp", "bp", "hp", "off"};
        static const char* const em[] = {"adsr", "perc"};
        const char* const* list = p == pFtype ? ft : em;
        const int n = p == pFtype ? 4 : 2;
        for (int k = 0; k < n; ++k)
            if (!std::strcmp(t, list[k])) return float(k);
        luaL_error(L, "engine: bad value '%s'", t);
    }
    if (lua_type(L, i) == LUA_TBOOLEAN) return lua_toboolean(L, i) ? 1.0f : 0.0f;
    if (p == pSample) return float(luaL_checknumber(L, i)) - 1.0f;   // slots 1..32 in Lua
    return float(luaL_checknumber(L, i));
}
int e_set(lua_State* L) {
    LuaSession* s = S(L);
    const int v = voiceArg(L, 1);
    const int p = paramArg(L, 2);
    s->engine()->set(v, p, paramValueArg(L, p, 3), when(s));
    return 0;
}
int e_setAll(lua_State* L) {   // engine.set_all("param", value): every voice
    LuaSession* s = S(L);
    const int p = paramArg(L, 1);
    const float val = paramValueArg(L, p, 2);
    for (int v = 0; v < kVoices; ++v) s->engine()->set(v, p, val, when(s));
    return 0;
}
int e_voice(lua_State* L) {   // engine.voice(v, {wave="saw", cutoff=800, ...})
    LuaSession* s = S(L);
    const int v = voiceArg(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const int p = paramIndex(lua_tostring(L, -2));
            if (p < 0) luaL_error(L, "engine.voice: unknown parameter '%s'", lua_tostring(L, -2));
            s->engine()->set(v, p, paramValueArg(L, p, lua_gettop(L)), when(s));
        }
        lua_pop(L, 1);
    }
    return 0;
}
int e_fx(lua_State* L) {
    LuaSession* s = S(L);
    const int p = fxIndex(luaL_checkstring(L, 1));
    if (p < 0) return luaL_error(L, "engine: unknown fx parameter '%s'", lua_tostring(L, 1));
    s->engine()->fx(p, float(luaL_checknumber(L, 2)), when(s));
    return 0;
}
int e_panic(lua_State* L) {
    S(L)->engine()->panic();
    return 0;
}
int e_get(lua_State* L) {
    LuaSession* s = S(L);
    lua_pushnumber(L, s->engine()->meter(voiceArg(L, 1)));
    return 1;
}
bool safeName(const char* n) {
    if (!n || !*n || n[0] == '/' || std::strstr(n, "..")) return false;
    for (const char* p = n; *p; ++p)
        if (*p == '\\' || *p == ':') return false;
    return true;
}
int e_load(lua_State* L) {   // engine.load(slot, "file.wav") -> frames | nil, err
    LuaSession* s = S(L);
    const int slot = int(luaL_checkinteger(L, 1)) - 1;
    luaL_argcheck(L, slot >= 0 && slot < kSlots, 1, "slot must be 1..32");
    const char* name = luaL_checkstring(L, 2);
    lab::Context* c = s->ctx();
    if (!c->fs) {
        lua_pushnil(L);
        lua_pushstring(L, "no card");
        return 2;
    }
    if (!safeName(name)) return luaL_error(L, "engine.load: bad file name '%s'", name);
    char path[160];
    const char* dirs[2] = {s->dataDir, s->scriptDir};
    bool found = false;
    for (const char* d : dirs) {
        std::snprintf(path, sizeof path, "%s/%s", d, name);
        if (c->fs->exists(path)) {
            found = true;
            break;
        }
    }
    if (!found) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s not found", name);
        return 2;
    }
    // header first, to size the buffer
    const ot::IStorage::Handle h = c->fs->open(path, ot::IStorage::kRead);
    if (h < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "cannot open");
        return 2;
    }
    uint8_t hdr[512];
    const int32_t got = c->fs->read(h, hdr, sizeof hdr);
    c->fs->close(h);
    ot::WavInfo info;
    if (got < 44 || !ot::wavParseHeader(hdr, uint32_t(got), info) || !ot::wavIsSupported(info)) {
        lua_pushnil(L);
        lua_pushstring(L, "not a supported WAV");
        return 2;
    }
    ot::WavImportOptions opt;
    opt.stereo = info.channels == 2;
    const ot::WavImportPlan plan = ot::wavPlanImport(info, opt);
    const uint32_t maxFrames = 44100u * 60u;
    if (plan.outFrames == 0 || plan.outFrames > maxFrames) {
        lua_pushnil(L);
        lua_pushstring(L, "sample too long (max 60 s)");
        return 2;
    }
    const uint32_t samples = plan.outFrames * plan.outChannels;
    int16_t* buf = static_cast<int16_t*>(s->pool()->alloc(samples * 2u, 16));
    if (!buf) {
        lua_pushnil(L);
        lua_pushstring(L, "out of sample memory");
        return 2;
    }
    ot::WavImportResult res;
    if (ot::wavImportFile(*c->fs, path, opt, buf, samples, res) != ot::FmtStatus::Ok) {
        lua_pushnil(L);
        lua_pushstring(L, "read error");
        return 2;
    }
    s->addSampleBytes(samples * 2u);
    s->engine()->publishSlot(slot, buf, res.frames, res.stereo ? 2 : 1);
    lua_pushinteger(L, lua_Integer(res.frames));
    return 1;
}

// ---- clock primitives (the scheduler itself is Lua, below) -----------------------------
int c_now(lua_State* L) {
    lua_pushnumber(L, lua_Number(S(L)->beatNow()));
    return 1;
}
int c_bps(lua_State* L) {
    lua_pushnumber(L, lua_Number(S(L)->bps()));
    return 1;
}
int c_setSched(lua_State* L) {
    LuaSession* s = S(L);
    s->schedBeat = lua_isnil(L, 1) ? -1.0 : double(lua_tonumber(L, 1));
    return 0;
}
int c_tempo(lua_State* L) {
    lua_pushnumber(L, lua_Number(S(L)->ctx()->bpm()));
    return 1;
}
int c_setTempo(lua_State* L) {
    S(L)->ctx()->setBpm(float(luaL_checknumber(L, 1)));
    return 0;
}
int c_start(lua_State* L) {
    S(L)->ctx()->setPlaying(true);
    return 0;
}
int c_stop(lua_State* L) {
    S(L)->ctx()->setPlaying(false);
    return 0;
}
int c_playing(lua_State* L) {
    lua_pushboolean(L, S(L)->ctx()->playing());
    return 1;
}
int c_millis(lua_State* L) {
    lua_pushinteger(L, lua_Integer(S(L)->ctx()->millis()));
    return 1;
}

// ---- midi -----------------------------------------------------------------------------
uint8_t chArg(lua_State* L, LuaSession* s, int i) {
    if (lua_isnoneornil(L, i)) return s->ctx()->midiChannel();
    const int c = int(luaL_checkinteger(L, i));
    luaL_argcheck(L, c >= 1 && c <= 16, i, "channel must be 1..16");
    return uint8_t(c - 1);
}
void sendMidi(LuaSession* s, uint8_t st, uint8_t d1, uint8_t d2) {
    lab::MidiMsg m;
    m.status = st;
    m.d1 = d1 & 127;
    m.d2 = d2 & 127;
    if (s->schedBeat >= 0.0) s->midiAt(m);
    else s->ctx()->midiOut(m);
}
int m_noteOn(lua_State* L) {
    LuaSession* s = S(L);
    const int n = int(luaL_checkinteger(L, 1));
    int v = int(numArg(L, 2, 100));
    sendMidi(s, uint8_t(0x90 | chArg(L, s, 3)), uint8_t(n), uint8_t(v < 1 ? 1 : v));
    return 0;
}
int m_noteOff(lua_State* L) {
    LuaSession* s = S(L);
    sendMidi(s, uint8_t(0x80 | chArg(L, s, 3)), uint8_t(luaL_checkinteger(L, 1)), uint8_t(numArg(L, 2, 0)));
    return 0;
}
int m_cc(lua_State* L) {
    LuaSession* s = S(L);
    sendMidi(s, uint8_t(0xB0 | chArg(L, s, 3)), uint8_t(luaL_checkinteger(L, 1)), uint8_t(luaL_checkinteger(L, 2)));
    return 0;
}
int m_send(lua_State* L) {
    LuaSession* s = S(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    uint8_t b[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        lua_geti(L, 1, i + 1);
        b[i] = uint8_t(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    if (!(b[0] & 0x80)) return luaL_error(L, "midi.send: first byte must be a status byte");
    sendMidi(s, b[0], b[1], b[2]);
    return 0;
}

// ---- files (in the script's data folder) ------------------------------------------------
bool pathFor(lua_State* L, char* out, size_t cap, int arg) {
    LuaSession* s = S(L);
    const char* n = luaL_checkstring(L, arg);
    if (!safeName(n)) luaL_error(L, "file: bad name '%s' (no '..', no absolute paths)", n);
    std::snprintf(out, cap, "%s/%s", s->dataDir, n);
    return s->ctx()->fs != nullptr;
}
int f_read(lua_State* L) {
    char p[160];
    if (!pathFor(L, p, sizeof p, 1)) return 0;
    ot::IStorage* fs = S(L)->ctx()->fs;
    const ot::IStorage::Handle h = fs->open(p, ot::IStorage::kRead);
    if (h < 0) return 0;
    const int32_t sz = fs->size(h);
    if (sz < 0 || sz > 256 * 1024) {
        fs->close(h);
        return luaL_error(L, "file.read: %s is too big (max 256 KB)", p);
    }
    luaL_Buffer b;
    char* dst = luaL_buffinitsize(L, &b, size_t(sz));
    int32_t got = 0;
    while (got < sz) {
        const int32_t n = fs->read(h, dst + got, uint32_t(sz - got));
        if (n <= 0) break;
        got += n;
    }
    fs->close(h);
    luaL_pushresultsize(&b, size_t(got));
    return 1;
}
int writeImpl(lua_State* L, bool append) {
    char p[160];
    if (!pathFor(L, p, sizeof p, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    size_t n = 0;
    const char* data = luaL_checklstring(L, 2, &n);
    ot::IStorage* fs = S(L)->ctx()->fs;
    const ot::IStorage::Handle h = fs->open(p, append ? ot::IStorage::kReadWrite : ot::IStorage::kWrite);
    if (h < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (append) fs->seek(h, uint32_t(fs->size(h) > 0 ? fs->size(h) : 0));
    const bool ok = fs->write(h, data, uint32_t(n)) == int32_t(n);
    fs->close(h);
    lua_pushboolean(L, ok);
    return 1;
}
int f_write(lua_State* L) { return writeImpl(L, false); }
int f_append(lua_State* L) { return writeImpl(L, true); }
int f_exists(lua_State* L) {
    char p[160];
    lua_pushboolean(L, pathFor(L, p, sizeof p, 1) && S(L)->ctx()->fs->exists(p));
    return 1;
}
int f_remove(lua_State* L) {
    char p[160];
    lua_pushboolean(L, pathFor(L, p, sizeof p, 1) && S(L)->ctx()->fs->remove(p));
    return 1;
}
int f_list(lua_State* L) {
    LuaSession* s = S(L);
    char p[160];
    if (lua_isnoneornil(L, 1)) std::snprintf(p, sizeof p, "%s", s->dataDir);
    else pathFor(L, p, sizeof p, 1);
    lua_newtable(L);
    if (!s->ctx()->fs) return 1;
    struct Ctx {
        lua_State* L;
        int n;
    } c{L, 0};
    s->ctx()->fs->listDir(
        p,
        [](const ot::IStorage::DirEntry& e, void* u) {
            Ctx* c = static_cast<Ctx*>(u);
            if (c->n >= 256) return false;
            lua_pushstring(c->L, e.name);
            lua_rawseti(c->L, -2, ++c->n);
            return true;
        },
        &c);
    return 1;
}

// ---- misc --------------------------------------------------------------------------------
int x_toast(lua_State* L) {
    S(L)->ctx()->toast(luaL_checkstring(L, 1));
    return 0;
}
int x_memory(lua_State* L) {
    LuaSession* s = S(L);
    lua_pushinteger(L, lua_Integer(s->heapUsed()));
    lua_pushinteger(L, lua_Integer(s->heapCap()));
    return 2;
}

void reg(lua_State* L, const char* table, const luaL_Reg* fns) {
    lua_getglobal(L, table);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, table);
    }
    luaL_setfuncs(L, fns, 0);
    lua_pop(L, 1);
}

constexpr const char kClockLua[] = R"LUA(
local co, nextid = {}, 1
clock = clock or {}
clock.transport = {}
local now, bps, setsched = _plat_now, _plat_bps, _plat_setsched
function clock.run(f, ...)
  local args = table.pack(...)
  local id = nextid
  nextid = nextid + 1
  co[id] = {co = coroutine.create(function() f(table.unpack(args, 1, args.n)) end), wake = now()}
  return id
end
function clock.cancel(id) co[id] = nil end
function clock.sync(b, off) return coroutine.yield("sync", b or 1, off or 0) end
function clock.sleep(s) return coroutine.yield("sleep", s or 0) end
function clock.get_beats() return now() end
function clock.get_beat_sec() return 1 / bps() end
function clock.get_tempo() return _plat_tempo() end
function clock.set_tempo(t) _plat_set_tempo(t) end
function clock.start() _plat_start() end
function clock.stop() _plat_stop() end
function clock.playing() return _plat_playing() end
local function tick(look)
  for id, c in pairs(co) do
    local guard = 0
    while co[id] and c.wake <= look and guard < 64 do
      guard = guard + 1
      setsched(c.wake)
      local ok, kind, a, off = coroutine.resume(c.co)
      setsched(nil)
      if not ok then co[id] = nil error("clock coroutine: " .. tostring(kind), 0) end
      if coroutine.status(c.co) == "dead" then co[id] = nil break end
      if kind == "sync" then
        local w = (math.floor(c.wake / a + 1e-3) + 1) * a + off
        if w <= c.wake then w = w + a end
        c.wake = w
      elseif kind == "sleep" then
        c.wake = c.wake + math.max(a, 0.001) * bps()
      else
        c.wake = c.wake + 1
      end
    end
  end
end
local function rebase(n) for _, c in pairs(co) do c.wake = n end end
local function count() local n = 0 for _ in pairs(co) do n = n + 1 end return n end
return tick, rebase, count
)LUA";

// ---- LABKIT: params, mod sources, routes, macros ------------------------------------
const lab::Rgb kPagePalette[] = {lab::Rgb(0x39E6B4), lab::Rgb(0xFFB347), lab::Rgb(0x4DA3FF), lab::Rgb(0xFF6FA8),
                                 lab::Rgb(0xB48CFF), lab::Rgb(0xFFE14D), lab::Rgb(0x5CF07A), lab::Rgb(0xFF7A45)};

const char* fieldStr(lua_State* L, int t, const char* k, const char* def) {
    lua_getfield(L, t, k);
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return s;
}
bool fieldNum(lua_State* L, int t, const char* k, float& out) {
    lua_getfield(L, t, k);
    const bool ok = lua_isnumber(L, -1) != 0;
    if (ok) out = float(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return ok;
}

int k_paramAdd(lua_State* L) {
    LuaSession* s = S(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    lab::kit::Registry& R = s->ctx()->params();
    const char* id = fieldStr(L, 1, "id", nullptr);
    if (!id) return luaL_error(L, "params: id missing");
    const char* name = fieldStr(L, 1, "name", id);
    const char* kind = fieldStr(L, 1, "kind", "control");
    const char* unit = fieldStr(L, 1, "unit", nullptr);
    // page: a name ("FILTER") or a number (1..); default the first page
    int page = 0;
    lua_getfield(L, 1, "page");
    if (lua_type(L, -1) == LUA_TSTRING) page = s->pageOf(lua_tostring(L, -1));
    else if (lua_isnumber(L, -1)) {
        char nm[16];
        const int want = int(lua_tointeger(L, -1));
        std::snprintf(nm, sizeof nm, "PAGE %d", want < 1 ? 1 : want);
        while (s->pageCount() < want && s->pageCount() < LuaSession::kMaxPages) {
            char n2[16];
            std::snprintf(n2, sizeof n2, "PAGE %d", s->pageCount() + 1);
            s->pageOf(n2);
        }
        page = want - 1;
    } else page = s->pageCount() ? 0 : s->pageOf("PARAMS");
    lua_pop(L, 1);
    if (page < 0) page = 0;
    if (page >= s->pageCount()) page = s->pageCount() - 1;
    float lo = 0.0f, hi = 1.0f, def = 0.0f, step = 0.0f;
    fieldNum(L, 1, "min", lo);
    fieldNum(L, 1, "max", hi);
    if (!fieldNum(L, 1, "default", def)) def = lo;
    const bool hasStep = fieldNum(L, 1, "step", step);
    if (!(hi > lo)) hi = lo + 1.0f;
    char key[48];
    std::snprintf(key, sizeof key, "lua.%s", id);
    float* slot = static_cast<float*>(s->pool()->alloc(sizeof(float), 4));
    if (!slot) return luaL_error(L, "params: out of session memory");
    R.setPage(page);
    R.setGroup(s->pageName(page), kPagePalette[page % 8]);
    const bool isOption = !std::strcmp(kind, "option");
    lab::kit::ParamId rid = lab::kit::kNoParam;
    if (isOption) {
        lua_getfield(L, 1, "options");
        const int n = lua_istable(L, -1) ? int(luaL_len(L, -1)) : 0;
        if (n <= 0) {
            lua_pop(L, 1);
            return luaL_error(L, "params: option '%s' has no options", id);
        }
        const char** names = static_cast<const char**>(s->pool()->alloc(sizeof(char*) * uint32_t(n), 8));
        if (!names) return luaL_error(L, "params: out of session memory");
        for (int i = 0; i < n; ++i) {
            lua_geti(L, -1, i + 1);
            names[i] = s->keep(luaL_tolstring(L, -1, nullptr));
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        *slot = def - 1.0f;   // Lua options are 1..n, the registry's 0..n-1
        rid = R.add(key, s->keep(name), slot).names(names, n).def(def - 1.0f);
        if (rid != lab::kit::kNoParam) s->markEnum(rid);
    } else {
        *slot = def;
        lab::kit::Param& P = R.add(key, s->keep(name), slot).range(lo, hi).def(def);
        rid = P;
        if (rid != lab::kit::kNoParam) {
            if (!std::strcmp(kind, "number")) P.integer();
            if (hasStep && step > 0.0f) P.step(step);
            if (unit) P.unit(s->keep(unit));
            if (lo < 0.0f && hi > 0.0f) P.bipolar();
        }
    }
    if (rid == lab::kit::kNoParam) return luaL_error(L, "params: the registry is full (512 params)");
    lua_pushinteger(L, rid);
    return 1;
}
int k_paramGet(lua_State* L) {
    LuaSession* s = S(L);
    const lab::kit::Registry& R = s->ctx()->params();
    const int rid = int(luaL_checkinteger(L, 1));
    if (!R.valid(lab::kit::ParamId(rid))) return 0;
    float v = lua_toboolean(L, 2) ? R.get(lab::kit::ParamId(rid)) : R.effective(lab::kit::ParamId(rid));
    if (s->isEnumParam(rid)) {
        lua_pushinteger(L, lua_Integer(v + 0.5f) + 1);
        return 1;
    }
    if (R.curve(lab::kit::ParamId(rid)) == lab::kit::Curve::kInt) lua_pushinteger(L, lua_Integer(floorf(v + 0.5f)));
    else lua_pushnumber(L, lua_Number(v));
    return 1;
}
int k_paramSet(lua_State* L) {
    LuaSession* s = S(L);
    lab::kit::Registry& R = s->ctx()->params();
    const int rid = int(luaL_checkinteger(L, 1));
    float v = float(luaL_checknumber(L, 2));
    if (!R.valid(lab::kit::ParamId(rid))) return 0;
    if (s->isEnumParam(rid)) v -= 1.0f;
    R.set(lab::kit::ParamId(rid), v);
    return 0;
}
int k_paramString(lua_State* L) {
    LuaSession* s = S(L);
    const lab::kit::Registry& R = s->ctx()->params();
    const int rid = int(luaL_checkinteger(L, 1));
    char buf[48] = {};
    if (R.valid(lab::kit::ParamId(rid))) R.format(lab::kit::ParamId(rid), buf, sizeof buf, true);
    lua_pushstring(L, buf);
    return 1;
}
int k_paramNudge(lua_State* L) {
    LuaSession* s = S(L);
    lab::kit::Registry& R = s->ctx()->params();
    const int rid = int(luaL_checkinteger(L, 1));
    if (R.valid(lab::kit::ParamId(rid))) R.nudge(lab::kit::ParamId(rid), int(luaL_checkinteger(L, 2)));
    return 0;
}
int k_kitSave(lua_State* L) {
    LuaSession* s = S(L);
    char path[96];
    lua_pushboolean(L, s->kitPath(path, sizeof path) && s->ctx()->params().saveFile(s->ctx()->fs, path));
    return 1;
}
int k_kitLoad(lua_State* L) {
    LuaSession* s = S(L);
    char path[96];
    bool ok = s->kitPath(path, sizeof path) && s->ctx()->fs && s->ctx()->fs->exists(path);
    if (ok) ok = s->ctx()->params().loadFile(s->ctx()->fs, path);
    lua_pushboolean(L, ok);
    return 1;
}
int k_modSource(lua_State* L) {
    LuaSession* s = S(L);
    const char* name = luaL_checkstring(L, 1);
    const bool bip = lua_toboolean(L, 2) != 0;
    const int slot = s->addModSlot();
    float* v = s->modSlot(slot);
    if (!v) return luaL_error(L, "mod.source: at most %d sources per script", LuaSession::kMaxSources);
    *v = 0.0f;
    const int id = s->ctx()->mod().addSource(s->keep(name), v, bip);
    if (id < 0) return luaL_error(L, "mod.source: no room for another source");
    lua_pushinteger(L, slot);
    lua_pushinteger(L, id);
    return 2;
}
int k_modSet(lua_State* L) {
    float* v = S(L)->modSlot(int(luaL_checkinteger(L, 1)));
    float x = float(luaL_checknumber(L, 2));
    if (v) *v = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    return 0;
}
int k_modFind(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    int id = -1;
    if (!std::strncmp(n, "LFO", 3) && n[3] >= '1' && n[3] <= '4' && !n[4]) id = lab::kit::kSrcLfo + (n[3] - '1');
    else if (!std::strncmp(n, "RND", 3) && n[3] >= '1' && n[3] <= '2' && !n[4]) id = lab::kit::kSrcRnd + (n[3] - '1');
    else if (n[0] == 'M' && n[1] >= '1' && n[1] <= '8' && !n[2]) id = lab::kit::kSrcMacro + (n[1] - '1');
    else id = S(L)->ctx()->mod().findSource(n);
    if (id < 0) return 0;
    lua_pushinteger(L, id);
    return 1;
}
int k_modRoute(lua_State* L) {
    LuaSession* s = S(L);
    const int r = s->ctx()->mod().route(int(luaL_checkinteger(L, 1)), lab::kit::ParamId(luaL_checkinteger(L, 2)),
                                        float(luaL_checknumber(L, 3)));
    lua_pushboolean(L, r >= 0);
    return 1;
}
int k_modUnroute(lua_State* L) {
    lua_pushboolean(L, S(L)->ctx()->mod().unroute(int(luaL_checkinteger(L, 1)),
                                                  lab::kit::ParamId(luaL_checkinteger(L, 2))));
    return 1;
}
int k_modClear(lua_State* L) {
    S(L)->ctx()->mod().clearRoutes();
    return 0;
}
int k_modCount(lua_State* L) {
    lua_pushinteger(L, S(L)->ctx()->mod().routeCount());
    return 1;
}
int macroIndex(lua_State* L) {
    const int i = int(luaL_checkinteger(L, 1));
    luaL_argcheck(L, i >= 1 && i <= lab::kit::kMacros, 1, "macro 1..8");
    return i - 1;
}
int k_macroGet(lua_State* L) {
    lua_pushnumber(L, lua_Number(S(L)->ctx()->mod().macro(macroIndex(L))));
    return 1;
}
int k_macroSet(lua_State* L) {
    S(L)->ctx()->mod().setMacro(macroIndex(L), float(luaL_checknumber(L, 2)));
    return 0;
}
int k_macroName(lua_State* L) {
    S(L)->ctx()->mod().setMacroName(macroIndex(L), luaL_checkstring(L, 2));
    return 0;
}
int k_lfo(lua_State* L) {
    LuaSession* s = S(L);
    const int i = int(luaL_checkinteger(L, 1));
    luaL_argcheck(L, i >= 1 && i <= lab::kit::kLfos, 1, "LFO 1..4");
    static const char* const kFields[] = {"shape", "rate", "sync", "phase", "retrig"};
    const char* f = luaL_checkstring(L, 2);
    int field = -1;
    for (int k = 0; k < 5; ++k)
        if (!std::strcmp(f, kFields[k])) field = k;
    if (field < 0) return luaL_error(L, "mod.lfo: field shape | rate | sync | phase | retrig");
    lab::kit::Registry& R = s->ctx()->params();
    const lab::kit::ParamId p = s->ctx()->mod().lfoParam(i - 1, field);
    if (!R.valid(p)) return 0;
    if (!lua_isnoneornil(L, 3)) {
        float v = 0.0f;
        if (field == 0 && lua_type(L, 3) == LUA_TSTRING) {
            static const char* const kShapes[] = {"sine", "tri", "saw", "square", "sh", "smooth"};
            for (int k = 0; k < 6; ++k)
                if (!std::strcmp(lua_tostring(L, 3), kShapes[k])) v = float(k);
        } else {
            v = float(luaL_checknumber(L, 3));
        }
        R.set(p, v);
    }
    lua_pushnumber(L, lua_Number(R.get(p)));
    return 1;
}

int openLibs(lua_State* L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_settop(L, 0);
    // no file loading from Lua itself
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    static const luaL_Reg screen[] = {
        {"color", s_color}, {"level", s_level}, {"alpha", s_alpha}, {"line_width", s_lineWidth},
        {"clip", s_clip}, {"noclip", s_noclip}, {"rect", s_rect}, {"frame", s_frame}, {"round", s_round},
        {"line", s_line}, {"circle", s_circle}, {"ring", s_ring}, {"arc", s_arc}, {"text", s_text},
        {"text_center", s_textCenter}, {"text_right", s_textRight}, {"font", s_font}, {"middle", s_valign},
        {"lines", s_lines}, {"points", s_points}, {"rects", s_rects}, {"polyline", s_polyline},
        {"number", s_number}, {"update", s_update}, {"fps", s_fps}, {"dirty", s_dirty},
        {"text_width", s_textWidth}, {nullptr, nullptr}};
    reg(L, "screen", screen);
    lua_getglobal(L, "screen");
    lua_pushinteger(L, lab::kScreenW);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, lab::kScreenH - 30);
    lua_setfield(L, -2, "height");
    lua_pop(L, 1);
    static const luaL_Reg grid[] = {{"led", g_led}, {"all", g_all}, {"color", g_color}, {"refresh", g_nop},
                                    {nullptr, nullptr}};
    reg(L, "grid", grid);
    lua_getglobal(L, "grid");
    lua_pushinteger(L, lab::kPadCols);
    lua_setfield(L, -2, "cols");
    lua_pushinteger(L, lab::kPadRows);
    lua_setfield(L, -2, "rows");
    lua_pop(L, 1);
    static const luaL_Reg pads[] = {{"led", p_led}, {"all", g_all}, {nullptr, nullptr}};
    reg(L, "pads", pads);
    static const luaL_Reg eng[] = {{"note", e_note}, {"hz", e_hz}, {"off", e_off}, {"trig", e_trig},
                                   {"set", e_set}, {"set_all", e_setAll}, {"voice", e_voice}, {"fx", e_fx},
                                   {"panic", e_panic}, {"level", e_get}, {"load", e_load}, {nullptr, nullptr}};
    reg(L, "engine", eng);
    lua_getglobal(L, "engine");
    lua_pushinteger(L, kVoices);
    lua_setfield(L, -2, "voices");
    lua_pop(L, 1);
    static const luaL_Reg midi[] = {{"note_on", m_noteOn}, {"note_off", m_noteOff}, {"cc", m_cc},
                                    {"send", m_send}, {nullptr, nullptr}};
    reg(L, "midi", midi);
    static const luaL_Reg file[] = {{"read", f_read}, {"write", f_write}, {"append", f_append},
                                    {"exists", f_exists}, {"remove", f_remove}, {"list", f_list},
                                    {nullptr, nullptr}};
    reg(L, "file", file);
    static const luaL_Reg sys[] = {{"toast", x_toast}, {"memory", x_memory}, {nullptr, nullptr}};
    reg(L, "sys", sys);
    lua_register(L, "_plat_now", c_now);
    lua_register(L, "_plat_bps", c_bps);
    lua_register(L, "_plat_setsched", c_setSched);
    lua_register(L, "_plat_tempo", c_tempo);
    lua_register(L, "_plat_set_tempo", c_setTempo);
    lua_register(L, "_plat_start", c_start);
    lua_register(L, "_plat_stop", c_stop);
    lua_register(L, "_plat_playing", c_playing);
    lua_register(L, "_plat_millis", c_millis);
    static const luaL_Reg kit[] = {
        {"_plat_param_add", k_paramAdd}, {"_plat_param_get", k_paramGet}, {"_plat_param_set", k_paramSet},
        {"_plat_param_string", k_paramString}, {"_plat_param_nudge", k_paramNudge}, {"_plat_kit_save", k_kitSave},
        {"_plat_kit_load", k_kitLoad}, {"_plat_mod_source", k_modSource}, {"_plat_mod_set", k_modSet},
        {"_plat_mod_find", k_modFind}, {"_plat_mod_route", k_modRoute}, {"_plat_mod_unroute", k_modUnroute},
        {"_plat_mod_clear", k_modClear}, {"_plat_mod_count", k_modCount}, {"_plat_macro_get", k_macroGet},
        {"_plat_macro_set", k_macroSet}, {"_plat_macro_name", k_macroName}, {"_plat_lfo", k_lfo},
        {nullptr, nullptr}};
    for (const luaL_Reg* r = kit; r->name; ++r) lua_register(L, r->name, r->func);
    // clock scheduler -> registry
    if (luaL_loadbufferx(L, kClockLua, sizeof kClockLua - 1, "=clock", "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 3);
    lua_setfield(L, LUA_REGISTRYINDEX, "plat.clock_count");
    lua_setfield(L, LUA_REGISTRYINDEX, "plat.clock_rebase");
    lua_setfield(L, LUA_REGISTRYINDEX, "plat.clock_tick");
    // params / metro / util / music
    if (luaL_loadbufferx(L, kLuaPrelude, sizeof kLuaPrelude - 1, "=prelude", "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "plat.host");
    return 0;
}

}  // namespace

const char* keyName(uint8_t id) {
    for (const KeyName& k : kKeys)
        if (k.id == id) return k.name;
    return "?";
}

void parseScriptHeader(const char* text, uint32_t len, const char* fileName, ScriptMeta& m) {
    m = ScriptMeta();
    // default name: file name without extension
    uint32_t i = 0;
    for (; fileName[i] && fileName[i] != '.' && i + 1 < sizeof m.name; ++i) m.name[i] = fileName[i];
    m.name[i] = 0;
    lab::copyStr(m.icon, sizeof m.icon, "script");
    uint32_t pos = 0;
    for (int line = 0; line < 24 && pos < len; ++line) {
        uint32_t end = pos;
        while (end < len && text[end] != '\n') ++end;
        const char* l = text + pos;
        const uint32_t n = end - pos;
        pos = end + 1;
        if (n < 2 || l[0] != '-' || l[1] != '-') {
            if (n == 0 || (n == 1 && l[0] == '\r')) continue;
            break;
        }
        uint32_t k = 2;
        while (k < n && l[k] == ' ') ++k;
        const char* keys[] = {"name:", "author:", "desc:", "description:", "icon:"};
        for (int ki = 0; ki < 5; ++ki) {
            const uint32_t kl = uint32_t(std::strlen(keys[ki]));
            if (n - k >= kl && !std::strncmp(l + k, keys[ki], kl)) {
                uint32_t v = k + kl;
                while (v < n && l[v] == ' ') ++v;
                uint32_t e = n;
                while (e > v && (l[e - 1] == '\r' || l[e - 1] == ' ')) --e;
                char* dst = ki == 0 ? m.name : ki == 1 ? m.author : ki == 4 ? m.icon : m.desc;
                const uint32_t cap = ki == 0 ? sizeof m.name : ki == 1 ? sizeof m.author : ki == 4 ? sizeof m.icon : sizeof m.desc;
                uint32_t o = 0;
                for (uint32_t c = v; c < e && o + 1 < cap; ++c) {
                    const unsigned char ch = static_cast<unsigned char>(l[c]);
                    dst[o++] = (ch >= 32 && ch <= 126) ? char(ch) : '?';
                }
                dst[o] = 0;
            }
        }
    }
}

int LuaSession::pageOf(const char* name) {
    for (int i = 0; i < nPages_; ++i)
        if (!std::strcmp(pageNames_[i], name)) return i;
    if (nPages_ >= kMaxPages) return -1;
    int o = 0;
    for (const char* p = name; *p && o < 15; ++p) {
        const char c = *p;
        pageNames_[nPages_][o++] = (c >= 'a' && c <= 'z') ? char(c - 32) : ((c >= 32 && c <= 126) ? c : '?');
    }
    pageNames_[nPages_][o] = 0;
    return nPages_++;
}

const char* LuaSession::keep(const char* str) {
    if (!str) return "";
    const uint32_t n = uint32_t(std::strlen(str));
    char* d = pool_ ? static_cast<char*>(pool_->alloc(n + 1, 1)) : nullptr;
    if (!d) return "";
    std::memcpy(d, str, n);
    d[n] = 0;
    return d;
}

bool LuaSession::kitPath(char* out, uint32_t cap) const {
    if (!ctx_ || !ctx_->fs || !dataDir[0]) return false;
    std::snprintf(out, cap, "%s/labkit.lkc", dataDir);
    return true;
}

LuaSession* LuaSession::from(lua_State* L) {
    // LUA_EXTRASPACE holds the session pointer (copied into every coroutine).
    LuaSession* s;
    std::memcpy(&s, lua_getextraspace(L), sizeof s);
    return s;
}

void LuaSession::print(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (consoleN_ == 0) consoleN_ = 1;
        char* line = console_[(consoleN_ - 1) % kConsoleLines];
        if (c == '\n' || consoleCol_ >= kConsoleCols - 1) {
            ++consoleN_;
            console_[(consoleN_ - 1) % kConsoleLines][0] = 0;
            consoleCol_ = 0;
            if (c == '\n') continue;
            line = console_[(consoleN_ - 1) % kConsoleLines];
        }
        const unsigned char u = static_cast<unsigned char>(c);
        line[consoleCol_++] = (u >= 32 && u <= 126) ? c : (c == '\t' ? ' ' : '?');
        line[consoleCol_] = 0;
    }
}
void LuaSession::printEnd() {
    if (consoleN_ == 0) consoleN_ = 1;
    ++consoleN_;
    console_[(consoleN_ - 1) % kConsoleLines][0] = 0;
    consoleCol_ = 0;
}
const char* LuaSession::consoleLine(int i) const {
    const int shown = consoleCount();
    const int first = consoleN_ - shown;
    return console_[(first + i) % kConsoleLines];
}

void LuaSession::fail(const char* msg) {
    failed_ = true;
    lab::copyStr(error_, sizeof error_, msg ? msg : "unknown error");
    if (engine_) engine_->panic();
}

bool LuaSession::call(int nargs, uint32_t limit, bool fatal) {
    // stack: ... fn args  ->  insert the traceback handler below fn
    const int base = lua_gettop(L_) - nargs;
    lua_pushcfunction(L_, traceback);
    lua_insert(L_, base);
    insn = 0;
    insnLimit = limit;
    LuaSession* prev = gPrint;
    gPrint = this;
    const int r = lua_pcall(L_, nargs, 0, base);
    gPrint = prev;
    insnLimit = 0;
    schedBeat = -1.0;
    if (r != LUA_OK) {
        const char* m = lua_tostring(L_, -1);
        const char* msg = m ? m : (r == LUA_ERRMEM ? "out of memory (script heap is full)" : "error");
        // Crash-safe runner: an error in a callback is logged and the script keeps
        // running (like norns); loading / init errors, out of memory, or more than
        // kMaxErrors errors in kErrorWindowMs stop it with the error page.
        if (fatal || r == LUA_ERRMEM) {
            fail(msg);
        } else {
            const uint32_t now = ctx_ ? ctx_->millis() : 0;
            if (now - errWindowStart_ > kErrorWindowMs) {
                errWindowStart_ = now;
                errInWindow_ = 0;
            }
            ++errInWindow_;
            ++errorsTotal_;
            lab::copyStr(lastError_, sizeof lastError_, msg);
            lastErrorMs_ = now;
            print("! ", 2);
            // first line of the message only into the console
            uint32_t n = 0;
            while (msg[n] && msg[n] != '\n') ++n;
            print(msg, n);
            printEnd();
            if (errInWindow_ > kMaxErrors) {
                char m2[640];
                std::snprintf(m2, sizeof m2, "%d errors in %u s - stopped. Last one:\n%s", errInWindow_,
                              unsigned(kErrorWindowMs / 1000), msg);
                fail(m2);
            }
        }
        lua_settop(L_, base - 1);
        return false;
    }
    lua_settop(L_, base - 1);
    return true;
}

bool LuaSession::eval(const char* line) {
    if (!running() || !line) return false;
    print("> ", 2);
    print(line, uint32_t(std::strlen(line)));
    printEnd();
    // an expression first ("1 + 2", "params:get('cut')"), else a statement
    char buf[300];
    std::snprintf(buf, sizeof buf, "return %s", line);
    if (luaL_loadbufferx(L_, buf, std::strlen(buf), "=repl", "t") != LUA_OK) {
        lua_pop(L_, 1);
        if (luaL_loadbufferx(L_, line, std::strlen(line), "=repl", "t") != LUA_OK) {
            const char* m = lua_tostring(L_, -1);
            print("! ", 2);
            print(m ? m : "syntax error", uint32_t(std::strlen(m ? m : "syntax error")));
            printEnd();
            lua_pop(L_, 1);
            return false;
        }
    }
    // results printed through print(...)
    lua_getglobal(L_, "print");
    lua_insert(L_, -2);
    lua_pushcfunction(L_, [](lua_State* L) -> int {
        // stack: print, chunk -> call the chunk, then print its results (if any)
        const int top0 = lua_gettop(L);
        lua_call(L, 0, LUA_MULTRET);
        const int n = lua_gettop(L) - top0 + 1;
        if (n > 0) {
            lua_pushvalue(L, top0 - 1);   // print
            lua_insert(L, -n - 1);
            lua_call(L, n, 0);
        }
        return 0;
    });
    lua_insert(L_, -3);
    return call(2, cfg_.callInsnLimit, false);
}

bool LuaSession::callGlobal(const char* fn, int nargs) {
    if (!running()) return false;
    lua_getglobal(L_, fn);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1 + nargs);
        return false;
    }
    if (nargs) lua_insert(L_, -1 - nargs);
    return call(nargs, cfg_.callInsnLimit, false);
}

bool LuaSession::open(Pool& pool, lab::Context& ctx, Engine& engine, const char* scriptPath, const char* name,
                      const LuaConfig& cfg) {
    cfg_ = cfg;
    engine_ = &engine;
    ctx_ = &ctx;
    pool_ = &pool;
    failed_ = false;
    error_[0] = 0;
    consoleN_ = consoleCol_ = 0;
    nPages_ = 0;
    nModSlots_ = 0;
    std::memset(enumBits_, 0, sizeof enumBits_);
    errInWindow_ = errorsTotal_ = 0;
    errWindowStart_ = lastErrorMs_ = lastPollMs_ = 0;
    lastError_[0] = 0;
    modSlots_ = static_cast<float*>(pool.alloc(sizeof(float) * kMaxSources, 16));
    nTimed_ = 0;
    gridColor = 0x40C0FF;
    lab::copyStr(name_, sizeof name_, name);
    lab::copyStr(path_, sizeof path_, scriptPath);
    std::memset(padRgb, 0, sizeof padRgb);
    std::snprintf(dataDir, sizeof dataDir, "%s/Data/%s", ctx.home, name);
    // the script's folder (for samples next to it)
    lab::copyStr(scriptDir, sizeof scriptDir, scriptPath);
    if (char* sl = std::strrchr(scriptDir, '/')) *sl = 0;
    if (ctx.fs) {
        char d[80];
        std::snprintf(d, sizeof d, "%s/Data", ctx.home);
        ctx.fs->mkdir(d);
        ctx.fs->mkdir(dataDir);
    }

    void* heapMem = pool.alloc(cfg.heapBytes, 16);
    for (auto& l : lists_) {
        l.ops = static_cast<DrawOp*>(pool.alloc(sizeof(DrawOp) * kMaxOps, 16));
        l.verts = static_cast<float*>(pool.alloc(sizeof(float) * kMaxVerts * 2, 16));
        l.text = static_cast<char*>(pool.alloc(kMaxText, 16));
        l.clear();
    }
    if (!heapMem || !heap_.init(heapMem, cfg.heapBytes) || !lists_[0].ops || !lists_[1].text) {
        fail("not enough memory for the script");
        return false;
    }
    L_ = lua_newstate(luaAlloc, &heap_);
    if (!L_) {
        fail("cannot create the Lua state");
        return false;
    }
    {
        LuaSession* self = this;
        std::memcpy(lua_getextraspace(L_), &self, sizeof self);
    }
    lua_sethook(L_, hook, LUA_MASKCOUNT, 1000);
    lua_gc(L_, LUA_GCGEN, 0, 0);

    // libraries + bindings inside a protected call
    lua_pushcfunction(L_, openLibs);
    engine_->setStampEpoch(engine_->epoch());
    snapshot();
    if (!call(0, cfg.initInsnLimit)) return false;

    if (!scriptPath || !*scriptPath) return true;   // openSource() continues
    // load the script text into the script heap (from the card)
    if (!ctx.fs) {
        fail("no SD card");
        return false;
    }
    const ot::IStorage::Handle h = ctx.fs->open(scriptPath, ot::IStorage::kRead);
    if (h < 0) {
        fail("cannot open the script file");
        return false;
    }
    const int32_t sz = ctx.fs->size(h);
    if (sz <= 0 || sz > 512 * 1024) {
        ctx.fs->close(h);
        fail(sz <= 0 ? "empty script file" : "script file too big (max 512 KB)");
        return false;
    }
    char* src = static_cast<char*>(heap_.alloc(uint32_t(sz)));
    if (!src) {
        ctx.fs->close(h);
        fail("script too big for the script heap");
        return false;
    }
    int32_t got = 0;
    while (got < sz) {
        const int32_t n = ctx.fs->read(h, src + got, uint32_t(sz - got));
        if (n <= 0) break;
        got += n;
    }
    ctx.fs->close(h);
    const bool ok = run(src, uint32_t(got));
    heap_.free(src);
    return ok && start();
}

bool LuaSession::openSource(Pool& pool, lab::Context& ctx, Engine& engine, const char* name, const char* src,
                            uint32_t len, const LuaConfig& cfg) {
    if (!open(pool, ctx, engine, "", name, cfg)) return false;
    return run(src, len) && start();
}

bool LuaSession::run(const char* src, uint32_t len) {
    if (!L_ || failed_) return false;
    char chunk[48];
    std::snprintf(chunk, sizeof chunk, "@%s.lua", name_);
    const int lr = luaL_loadbufferx(L_, src, size_t(len), chunk, "t");
    if (lr != LUA_OK) {
        fail(lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return call(0, cfg_.initInsnLimit);
}

bool LuaSession::start() {
    if (!L_ || failed_) return false;
    hasRedraw_ = false;
    lua_getglobal(L_, "redraw");
    hasRedraw_ = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    lua_getglobal(L_, "init");
    if (lua_isfunction(L_, -1)) {
        if (!call(0, cfg_.initInsnLimit)) return false;
    } else {
        lua_pop(L_, 1);
    }
    // params: read the saved values, then run every action once
    lua_getfield(L_, LUA_REGISTRYINDEX, "plat.host");
    lua_getfield(L_, -1, "after_init");
    lua_remove(L_, -2);
    if (!call(0, cfg_.initInsnLimit)) return false;
    redrawRequested = true;
    return true;
}

void LuaSession::close() {
    if (L_) {
        if (!failed_) {
            callGlobal("cleanup", 0);
            save();
        }
        lua_close(L_);
        L_ = nullptr;
    }
    if (engine_) engine_->panic();
}

void LuaSession::save() {
    if (!running()) return;
    lua_getfield(L_, LUA_REGISTRYINDEX, "plat.host");
    lua_getfield(L_, -1, "save");
    lua_remove(L_, -2);
    call(0, cfg_.callInsnLimit, false);
}

void LuaSession::commit() {
    lastDropped_ = 0;
    back_ ^= 1;
    lists_[back_].clear();
}

void LuaSession::snapshot() {
    double b = 0.0, bps = 0.0;
    uint8_t ep = 0;
    uint32_t blocks = 0;
    engine_->clock(b, bps, ep, blocks);
    snapBeat_ = b;
    snapBps_ = bps;
    snapEpoch_ = ep;
    snapBlocks_ = blocks;
    engine_->setStampEpoch(ep);
}

void LuaSession::midiAt(const lab::MidiMsg& m) {
    if (nTimed_ >= kTimedMidi) {
        ctx_->midiOut(m);
        return;
    }
    timed_[nTimed_++] = TimedMidi{schedBeat, snapEpoch_, m};
}

void LuaSession::flushMidi() {
    int keep = 0;
    for (int i = 0; i < nTimed_; ++i) {
        const TimedMidi& t = timed_[i];
        if (t.epoch != snapEpoch_ || t.beat <= snapBeat_) ctx_->midiOut(t.m);
        else timed_[keep++] = t;
    }
    nTimed_ = keep;
}

void LuaSession::runClock() {
    // look 60 ms ahead of the audio: commands carry exact beats, so they still
    // land on their sample even though they are sent early.
    const double look = snapBeat_ + snapBps_ * 44100.0 * 0.06;
    lua_getfield(L_, LUA_REGISTRYINDEX, "plat.clock_tick");
    lua_pushnumber(L_, lua_Number(look));
    call(1, cfg_.callInsnLimit, false);
}

void LuaSession::update(uint32_t ms) {
    if (!running()) return;
    const uint8_t oldEpoch = snapEpoch_;
    snapshot();
    if (snapEpoch_ != oldEpoch) {   // transport restarted: every clock coroutine resumes at the new beat
        lua_getfield(L_, LUA_REGISTRYINDEX, "plat.clock_rebase");
        lua_pushnumber(L_, lua_Number(snapBeat_));
        call(1, cfg_.callInsnLimit, false);
        nTimed_ = 0;
    }
    if (running()) runClock();
    if (running()) flushMidi();
    // params changed by the LABKIT views (pads, MAP, macros, routes, MIDI learn)
    // and the script's own mod sources: ~100 times a second
    if (running() && ms - lastPollMs_ >= 10) {
        lastPollMs_ = ms;
        lua_getfield(L_, LUA_REGISTRYINDEX, "plat.host");
        lua_getfield(L_, -1, "poll");
        lua_remove(L_, -2);
        call(0, cfg_.callInsnLimit, false);
    }
    // redraw at the script's frame rate (or when asked)
    if (running() && hasRedraw_) {
        const bool due = fps > 0.0f && float(ms - lastRedrawMs_) >= 1000.0f / fps;
        if (due || redrawRequested) {
            lastRedrawMs_ = ms;
            redrawRequested = false;
            lists_[back_].clear();
            if (callGlobal("redraw", 0)) commit();
        }
    }
    if (running()) {
        lua_gc(L_, LUA_GCSTEP, 0);
    }
}

void LuaSession::event(const lab::Event& e) {
    if (!running()) return;
    switch (e.type) {
        case lab::Event::kPress:
        case lab::Event::kRelease:
        case lab::Event::kHold:
            lua_pushstring(L_, keyName(e.id));
            lua_pushinteger(L_, e.type == lab::Event::kPress ? 1 : (e.type == lab::Event::kHold ? 2 : 0));
            lua_pushboolean(L_, e.shift);
            callGlobal("key", 3);
            break;
        case lab::Event::kPadDown:
        case lab::Event::kPadUp: {
            const bool down = e.type == lab::Event::kPadDown;
            lua_pushinteger(L_, e.id + 1);
            lua_pushinteger(L_, down ? e.velocity : 0);
            lua_pushinteger(L_, down ? 1 : 0);
            callGlobal("pad", 3);
            if (running()) {
                lua_pushinteger(L_, lab::padCol(e.id) + 1);
                lua_pushinteger(L_, lab::padRow(e.id) + 1);
                lua_pushinteger(L_, down ? 1 : 0);
                callGlobal("grid_key", 3);
            }
            break;
        }
        case lab::Event::kEncoder:
            lua_pushinteger(L_, e.delta);
            lua_pushboolean(L_, e.shift);
            callGlobal("enc", 2);
            break;
        default: break;
    }
    redrawRequested = true;
}

void LuaSession::midi(const lab::MidiMsg& m) {
    if (!running()) return;
    lua_getglobal(L_, "midi");
    lua_getfield(L_, -1, "event");
    lua_remove(L_, -2);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return;
    }
    lua_createtable(L_, 3, 0);
    lua_pushinteger(L_, m.status);
    lua_rawseti(L_, -2, 1);
    lua_pushinteger(L_, m.d1);
    lua_rawseti(L_, -2, 2);
    lua_pushinteger(L_, m.d2);
    lua_rawseti(L_, -2, 3);
    call(1, cfg_.callInsnLimit, false);
}

void LuaSession::transport(bool playing) {
    if (!running()) return;
    lua_getglobal(L_, "clock");
    lua_getfield(L_, -1, "transport");
    lua_remove(L_, -2);
    if (lua_istable(L_, -1)) {
        lua_getfield(L_, -1, playing ? "start" : "stop");
        lua_remove(L_, -2);
        if (lua_isfunction(L_, -1)) call(0, cfg_.callInsnLimit, false);
        else lua_pop(L_, 1);
    } else {
        lua_pop(L_, 1);
    }
}

void LuaSession::draw(BudgetCanvas& c) {
    // replay the committed list
    const DrawList& d = lists_[back_ ^ 1];
    c.color(0xFFFFFF);
    for (uint32_t i = 0; i < d.n; ++i) {
        const DrawOp& o = d.ops[i];
        switch (o.op) {
            case dColor: c.color(o.u); break;
            case dAlpha: c.alpha(uint8_t(o.u)); break;
            case dLineWidth: c.lineWidth(o.a); break;
            case dClip: c.clip(int(o.a), int(o.b), int(o.c), int(o.d)); break;
            case dNoClip: c.noClip(); break;
            case dFill: c.fill(int(o.a), int(o.b), int(o.c), int(o.d)); break;
            case dRound: c.fillRound(int(o.a), int(o.b), int(o.c), int(o.d), o.e); break;
            case dFrame: c.frame(int(o.a), int(o.b), int(o.c), int(o.d)); break;
            case dLine: c.line(o.a, o.b, o.c, o.d); break;
            case dPoint: c.point(o.a, o.b, o.c); break;
            case dRing: c.ring(o.a, o.b, o.c, int(o.u)); break;
            case dArc: c.arc(o.a, o.b, o.c, o.d, o.e, int(o.u)); break;
            case dText: c.text(int(o.a), int(o.b), o.font, d.text + o.u, o.opt); break;
            case dNumber: c.number(int(o.a), int(o.b), o.font, int32_t(o.u), o.opt); break;
            case dBatch:
                if (o.font == 1) c.pointSize(o.a);
                else if (o.font == 0 || o.font == 3) c.lineWidth(o.a);
                if (c.beginBatch(o.font, o.opt)) {
                    for (uint32_t k = 0; k < o.opt; ++k) c.vtx(d.verts[o.u + 2 * k], d.verts[o.u + 2 * k + 1]);
                    c.endBatch();
                }
                if (o.font == 0 || o.font == 3) c.lineWidth(1.0f);
                break;
            default: break;
        }
    }
    c.noClip();
    c.alpha(255);
    lastDropped_ = c.dropped() + (d.overflow ? 1u : 0u);
}

void LuaSession::leds(lab::Rgb* pads) {
    for (int i = 0; i < lab::kPads; ++i) pads[i] = lab::Rgb(padRgb[i][0], padRgb[i][1], padRgb[i][2]);
}

}  // namespace plat
