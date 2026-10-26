// DATAMATICS -- reading modes (dm_data.h).
#include "ws/dm_data.h"

#include <cstring>

#include "formats/wav.h"

namespace dm {

namespace {

const char* const kModeNames[kModeCount] = {"AUTO", "RAW", "BITS", "IMAGE", "WAV", "MIDI", "TEXT"};

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool extIs(const char* name, const char* ext) {
    const char* dot = nullptr;
    for (const char* p = name; *p; ++p)
        if (*p == '.') dot = p;
    if (!dot) return false;
    ++dot;
    for (; *dot && *ext; ++dot, ++ext)
        if (lower(*dot) != *ext) return false;
    return *dot == 0 && *ext == 0;
}

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint8_t luma(uint32_t r, uint32_t g, uint32_t b) { return uint8_t((r * 77u + g * 150u + b * 29u) >> 8); }

// Integer downsampling factor so that w*h/f^2 fits cap.
uint32_t fitFactor(uint32_t w, uint32_t h, uint32_t cap) {
    uint32_t f = 1;
    while ((uint64_t(w / f) * (h / f)) > cap) ++f;
    return f;
}

// Pixel source for the image decoders: returns luminance of (x, y), y = 0 top.
struct PixelFn {
    virtual uint8_t at(uint32_t x, uint32_t y) const = 0;
};

uint32_t rasterize(const PixelFn& px, uint32_t w, uint32_t h, DecodeOut& o) {
    if (!w || !h) return 0;
    const uint32_t f = fitFactor(w, h, o.cap);
    const uint32_t ow = w / f, oh = h / f;
    if (!ow || !oh) return 0;
    uint32_t n = 0;
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const uint8_t v = px.at(x * f, y * f);
            o.v[n] = v;
            if (o.aux) o.aux[n] = v;
            ++n;
        }
    o.n = n;
    o.width = uint16_t(ow > 65535 ? 65535 : ow);
    o.srcW = uint16_t(w > 65535 ? 65535 : w);
    o.srcH = uint16_t(h > 65535 ? 65535 : h);
    o.mode = kImage;
    return n;
}

// ---- BMP ----------------------------------------------------------------------
struct BmpPx : PixelFn {
    const uint8_t* f = nullptr;
    uint32_t len = 0, off = 0, stride = 0, h = 0;
    uint16_t bpp = 0;
    bool topDown = false;
    const uint8_t* pal = nullptr;
    uint32_t palN = 0;
    uint32_t mR = 0, mG = 0, mB = 0;
    static uint32_t chan(uint32_t v, uint32_t m) {
        if (!m) return 0;
        uint32_t s = 0;
        while (!((m >> s) & 1u)) ++s;
        const uint32_t mm = m >> s;
        return ((v >> s) & mm) * 255u / mm;
    }
    uint8_t at(uint32_t x, uint32_t y) const override {
        const uint32_t row = topDown ? y : (h - 1 - y);
        const uint32_t base = off + row * stride;
        uint32_t r = 0, g = 0, b = 0;
        switch (bpp) {
            case 1: case 4: case 8: {
                const uint32_t bit = x * bpp;
                const uint32_t p = base + bit / 8;
                if (p >= len) return 0;
                const uint32_t idx = (f[p] >> (8 - bpp - (bit % 8))) & ((1u << bpp) - 1u);
                if (!pal || idx >= palN) return uint8_t(idx * 255u / ((1u << bpp) - 1u));
                b = pal[idx * 4];
                g = pal[idx * 4 + 1];
                r = pal[idx * 4 + 2];
                break;
            }
            case 16: {
                const uint32_t p = base + x * 2;
                if (p + 2 > len) return 0;
                const uint32_t v = rd16(f + p);
                r = chan(v, mR); g = chan(v, mG); b = chan(v, mB);
                break;
            }
            case 24: case 32: {
                const uint32_t p = base + x * (bpp / 8);
                if (p + 3 > len) return 0;
                if (bpp == 32 && (mR || mG || mB)) {
                    if (p + 4 > len) return 0;
                    const uint32_t v = rd32(f + p);
                    r = chan(v, mR); g = chan(v, mG); b = chan(v, mB);
                } else {
                    b = f[p]; g = f[p + 1]; r = f[p + 2];
                }
                break;
            }
            default: return 0;
        }
        return luma(r, g, b);
    }
};

uint32_t decodeBmp(const uint8_t* f, uint32_t len, DecodeOut& o) {
    if (len < 54 || f[0] != 'B' || f[1] != 'M') return 0;
    BmpPx p;
    p.f = f;
    p.len = len;
    p.off = rd32(f + 10);
    const uint32_t dib = rd32(f + 14);
    if (dib < 40 || 14 + dib > len) return 0;
    const int32_t w = int32_t(rd32(f + 18));
    int32_t h = int32_t(rd32(f + 22));
    p.bpp = rd16(f + 28);
    const uint32_t comp = rd32(f + 30);
    if (w <= 0 || h == 0 || w > 16384 || h > 16384 || h < -16384) return 0;
    if (comp != 0 && comp != 3) return 0;   // RLE / JPEG / PNG inside BMP: not decoded
    p.topDown = h < 0;
    if (h < 0) h = -h;
    p.h = uint32_t(h);
    p.stride = ((uint32_t(w) * p.bpp + 31u) / 32u) * 4u;
    if (p.bpp <= 8) {
        uint32_t used = rd32(f + 46);
        if (!used || used > 256u) used = 1u << p.bpp;
        p.pal = f + 14 + dib;
        p.palN = used;
        if (14 + dib + used * 4 > len) p.pal = nullptr;
    } else if (p.bpp == 16) {
        p.mR = 0x7C00; p.mG = 0x03E0; p.mB = 0x001F;
        if (comp == 3) {
            const uint32_t mo = dib >= 52 ? 14 + 40 : 14 + dib;
            if (mo + 12 <= len) { p.mR = rd32(f + mo); p.mG = rd32(f + mo + 4); p.mB = rd32(f + mo + 8); }
        }
    } else if (p.bpp == 32 && comp == 3) {
        const uint32_t mo = dib >= 52 ? 14 + 40 : 14 + dib;
        if (mo + 12 <= len) { p.mR = rd32(f + mo); p.mG = rd32(f + mo + 4); p.mB = rd32(f + mo + 8); }
    } else if (p.bpp != 24 && p.bpp != 32) {
        return 0;
    }
    if (p.off >= len) return 0;
    return rasterize(p, uint32_t(w), p.h, o);
}

// ---- PNM (P1..P6) ----------------------------------------------------------------
struct PnmReader {
    const uint8_t* f;
    uint32_t len, pos;
    void skipWs() {
        while (pos < len) {
            const uint8_t c = f[pos];
            if (c == '#') {
                while (pos < len && f[pos] != '\n') ++pos;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else {
                break;
            }
        }
    }
    bool num(uint32_t& v) {
        skipWs();
        if (pos >= len || f[pos] < '0' || f[pos] > '9') return false;
        v = 0;
        while (pos < len && f[pos] >= '0' && f[pos] <= '9') {
            v = v * 10u + uint32_t(f[pos] - '0');
            if (v > 100000000u) return false;
            ++pos;
        }
        return true;
    }
};

struct PnmPx : PixelFn {
    const uint8_t* data = nullptr;
    uint32_t avail = 0, w = 0, maxv = 255;
    int kind = 5;   // 4 bits, 5 gray, 6 rgb
    bool wide = false;
    uint8_t at(uint32_t x, uint32_t y) const override {
        if (kind == 4) {
            const uint32_t rowBytes = (w + 7) / 8;
            const uint32_t p = y * rowBytes + x / 8;
            if (p >= avail) return 0;
            return ((data[p] >> (7 - (x % 8))) & 1u) ? 0 : 255;   // PBM: 1 = black
        }
        const uint32_t ch = kind == 6 ? 3 : 1, bps = wide ? 2 : 1;
        const uint32_t p = (y * w + x) * ch * bps;
        if (p + ch * bps > avail) return 0;
        auto s = [&](uint32_t i) -> uint32_t {
            const uint32_t raw = wide ? be16(data + p + i * 2) : data[p + i];
            return raw * 255u / (maxv ? maxv : 1);
        };
        if (kind == 6) return luma(s(0), s(1), s(2));
        return uint8_t(s(0));
    }
};

uint32_t decodePnm(const uint8_t* f, uint32_t len, DecodeOut& o) {
    if (len < 3 || f[0] != 'P' || f[1] < '1' || f[1] > '6') return 0;
    const int kind = f[1] - '0';
    PnmReader r{f, len, 2};
    uint32_t w = 0, h = 0, maxv = 1;
    if (!r.num(w) || !r.num(h) || !w || !h || w > 16384 || h > 16384) return 0;
    if (kind != 1 && kind != 4 && (!r.num(maxv) || !maxv || maxv > 65535)) return 0;
    if (kind <= 3) {   // ASCII: parse into the output directly (downsampled by f)
        const uint32_t ch = kind == 3 ? 3 : 1;
        const uint32_t fac = fitFactor(w, h, o.cap);
        const uint32_t ow = w / fac;
        uint32_t n = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t c[3] = {0, 0, 0};
                for (uint32_t k = 0; k < ch; ++k) {
                    if (kind == 1) {   // P1 digits may be packed without spaces
                        r.skipWs();
                        if (r.pos >= len) goto done;
                        c[k] = f[r.pos++] == '1' ? 0 : 255;
                    } else {
                        if (!r.num(c[k])) goto done;
                        c[k] = c[k] * 255u / maxv;
                    }
                }
                if ((y % fac) || (x % fac) || x / fac >= ow || n >= o.cap) continue;
                const uint8_t v = ch == 3 ? luma(c[0], c[1], c[2]) : uint8_t(c[0] > 255 ? 255 : c[0]);
                o.v[n] = v;
                if (o.aux) o.aux[n] = v;
                ++n;
            }
    done:
        if (!n) return 0;
        o.n = n;
        o.width = uint16_t(ow ? ow : 1);
        o.srcW = uint16_t(w);
        o.srcH = uint16_t(h);
        o.mode = kImage;
        return n;
    }
    if (r.pos >= len) return 0;
    ++r.pos;   // the single whitespace before the raster
    PnmPx p;
    p.data = f + r.pos;
    p.avail = len - r.pos;
    p.w = w;
    p.maxv = maxv;
    p.kind = kind;
    p.wide = maxv > 255;
    return rasterize(p, w, h, o);
}

// ---- MIDI ----------------------------------------------------------------------------
uint32_t readVlq(const uint8_t* f, uint32_t end, uint32_t& pos) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && pos < end; ++i) {
        const uint8_t b = f[pos++];
        v = (v << 7) | (b & 0x7Fu);
        if (!(b & 0x80)) break;
    }
    return v;
}

}  // namespace

const char* modeName(int m) { return (m >= 0 && m < kModeCount) ? kModeNames[m] : "?"; }

Mode detectMode(const char* name, const uint8_t* head, uint32_t len) {
    if (name) {
        if (extIs(name, "bmp") || extIs(name, "pgm") || extIs(name, "ppm") || extIs(name, "pbm") || extIs(name, "pnm"))
            return kImage;
        if (extIs(name, "wav") || extIs(name, "wave")) return kWav;
        if (extIs(name, "mid") || extIs(name, "midi") || extIs(name, "smf")) return kMidi;
        static const char* const kTextExt[] = {"txt", "md", "csv", "json", "xml", "html", "htm", "c", "h", "cpp",
                                            "py", "lua", "ini", "log", "cfg", "yml", "yaml", "js", "tsv", "nfo"};
        for (const char* e : kTextExt)
            if (extIs(name, e)) return kText;
    }
    if (len >= 12 && !std::memcmp(head, "RIFF", 4) && !std::memcmp(head + 8, "WAVE", 4)) return kWav;
    if (len >= 4 && !std::memcmp(head, "MThd", 4)) return kMidi;
    if (len >= 2 && head[0] == 'B' && head[1] == 'M') return kImage;
    if (len >= 3 && head[0] == 'P' && head[1] >= '1' && head[1] <= '6' &&
        (head[2] == ' ' || head[2] == '\n' || head[2] == '\r' || head[2] == '\t'))
        return kImage;
    if (len) {
        const uint32_t m = len < 512 ? len : 512;
        uint32_t printable = 0;
        for (uint32_t i = 0; i < m; ++i) {
            const uint8_t c = head[i];
            if ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t') ++printable;
        }
        if (printable * 100u >= m * 95u) return kText;
    }
    return kRaw;
}

uint32_t decodeRaw(const uint8_t* f, uint32_t len, DecodeOut& o) {
    const uint32_t n = len < o.cap ? len : o.cap;
    std::memcpy(o.v, f, n);
    if (o.aux) std::memcpy(o.aux, f, n);
    o.n = n;
    o.width = 256;
    o.mode = kRaw;
    return n;
}

uint32_t decodeBits(const uint8_t* f, uint32_t len, DecodeOut& o) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n + 8 <= o.cap; ++i)
        for (int b = 7; b >= 0; --b) {
            o.v[n] = ((f[i] >> b) & 1u) ? 255 : 0;
            if (o.aux) o.aux[n] = f[i];
            ++n;
        }
    o.n = n;
    o.width = 256;
    o.mode = kBits;
    return n;
}

uint32_t decodeText(const uint8_t* f, uint32_t len, DecodeOut& o) {
    // Printable characters spread over 1..255 ('!' = 3 .. '~' = 255), white space
    // (and anything else) = 0, so words become bursts and spaces become rests.
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n < o.cap; ++i) {
        const uint8_t c = f[i];
        if (c == '\r') continue;
        const uint8_t v = (c > 32 && c < 127) ? uint8_t(1 + (uint32_t(c - 32) * 254u) / 94u) : 0;
        o.v[n] = v;
        if (o.aux) o.aux[n] = c;
        ++n;
    }
    o.n = n;
    o.width = 80;
    o.mode = kText;
    return n;
}

uint32_t decodeImage(const uint8_t* f, uint32_t len, DecodeOut& o) {
    o.n = 0;
    if (len >= 2 && f[0] == 'B' && f[1] == 'M') return decodeBmp(f, len, o);
    if (len >= 2 && f[0] == 'P') return decodePnm(f, len, o);
    return 0;
}

uint32_t decodeWav(const uint8_t* f, uint32_t len, DecodeOut& o) {
    // The recording as an amplitude envelope: one value per 64-sample frame
    // (1.45 ms at 44.1 kHz; longer files use longer frames to fit): v = peak,
    // aux = zero-crossing density of the frame (a brightness measure).
    o.n = 0;
    ot::WavInfo info;
    if (!ot::wavParseHeader(f, len, info) || !info.valid || !ot::wavIsSupported(info)) return 0;
    if (info.dataOffset >= len) return 0;
    const uint32_t bpf = ot::wavBytesPerFrame(info);
    if (!bpf) return 0;
    uint32_t avail = len - info.dataOffset;
    if (info.dataSize && info.dataSize < avail) avail = info.dataSize;
    const uint32_t frames = avail / bpf;
    if (!frames) return 0;
    uint32_t win = 64;
    while (frames / win > o.cap) win *= 2;
    const uint8_t* d = f + info.dataOffset;
    const uint32_t bytesPerSample = info.blockAlign / (info.channels ? info.channels : 1);
    uint32_t n = 0;
    for (uint32_t s = 0; s < frames && n < o.cap; s += win) {
        int peak = 0, zc = 0;
        int prev = 0;
        const uint32_t e = s + win < frames ? s + win : frames;
        for (uint32_t i = s; i < e; ++i) {
            int x = ot::wavDecodeSample(d + i * bpf, info.format, info.bitsPerSample);
            if (info.channels > 1) {
                x = (x + ot::wavDecodeSample(d + i * bpf + bytesPerSample, info.format, info.bitsPerSample)) / 2;
            }
            const int a = x < 0 ? -x : x;
            if (a > peak) peak = a;
            if (i > s && ((x < 0) != (prev < 0))) ++zc;
            prev = x;
        }
        const uint32_t len2 = e - s;
        o.v[n] = uint8_t(peak >= 32767 ? 255 : (peak * 256) / 32768);
        if (o.aux) o.aux[n] = uint8_t(len2 > 1 ? (uint32_t(zc) * 255u) / (len2 - 1) : 0);
        ++n;
    }
    o.n = n;
    o.width = 256;
    o.mode = kWav;
    return n;
}

uint32_t decodeMidi(const uint8_t* f, uint32_t len, DecodeOut& o, MidiEv* ev) {
    // Note-ons of every track merged in time; v = note * 2, aux = velocity * 2.
    // Rests between notes become zero values on a sixteenth grid (max 16 per gap).
    o.n = 0;
    if (!ev || len < 14 || std::memcmp(f, "MThd", 4) != 0) return 0;
    const uint32_t hl = be32(f + 4);
    if (hl < 6 || 8 + hl > len) return 0;
    const uint16_t ntr = be16(f + 10);
    uint16_t div = be16(f + 12);
    if (div & 0x8000) div = 96;   // SMPTE time: treat as 96 ppq
    if (!div) div = 96;
    uint32_t pos = 8 + hl, nev = 0;
    constexpr int kMaxTracks = 64;
    uint32_t trStart[kMaxTracks + 1];
    int tracks = 0;
    for (uint16_t t = 0; t < ntr && pos + 8 <= len && tracks < kMaxTracks; ++t) {
        const uint32_t clen = be32(f + pos + 4);
        const bool isTrack = !std::memcmp(f + pos, "MTrk", 4);
        uint32_t p = pos + 8;
        const uint32_t end = (clen > len - p) ? len : p + clen;
        pos = end;
        if (!isTrack) { --t; continue; }
        trStart[tracks] = nev;
        uint32_t tick = 0;
        uint8_t run = 0;
        while (p < end && nev < kMaxMidiEvents) {
            tick += readVlq(f, end, p);
            if (p >= end) break;
            uint8_t st = f[p];
            if (st == 0xFF) {
                if (p + 2 > end) break;
                const uint8_t metaType = f[p + 1];
                p += 2;
                const uint32_t l = readVlq(f, end, p);
                if (metaType == 0x2F) break;   // end of track
                p += l;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {
                ++p;
                const uint32_t l = readVlq(f, end, p);
                p += l;
                continue;
            }
            if (st & 0x80) {
                run = st;
                ++p;
            } else if (!run) {
                ++p;
                continue;
            }
            const uint8_t ty = run & 0xF0;
            const uint32_t need = (ty == 0xC0 || ty == 0xD0) ? 1 : 2;
            if (p + need > end) break;
            if (ty == 0x90 && f[p + 1] > 0) {
                ev[nev].tick = tick;
                ev[nev].note = f[p] & 0x7F;
                ev[nev].vel = f[p + 1] & 0x7F;
                ev[nev].track = uint8_t(tracks);
                ++nev;
            }
            p += need;
        }
        ++tracks;
    }
    if (!nev) return 0;
    trStart[tracks] = nev;
    uint32_t cur[kMaxTracks];
    for (int t = 0; t < tracks; ++t) cur[t] = trStart[t];
    const uint32_t sixteenth = div / 4 ? div / 4 : 1;
    uint32_t n = 0;
    uint32_t lastTick = 0;
    bool first = true;
    for (;;) {
        int best = -1;
        for (int t = 0; t < tracks; ++t)
            if (cur[t] < trStart[t + 1] && (best < 0 || ev[cur[t]].tick < ev[cur[best]].tick)) best = t;
        if (best < 0 || n >= o.cap) break;
        const MidiEv& e = ev[cur[best]++];
        if (!first && e.tick > lastTick) {
            uint32_t rests = (e.tick - lastTick) / sixteenth;
            rests = rests ? rests - 1 : 0;
            if (rests > 16) rests = 16;
            for (uint32_t r = 0; r < rests && n < o.cap; ++r) {
                o.v[n] = 0;
                if (o.aux) o.aux[n] = 0;
                ++n;
            }
        }
        if (n >= o.cap) break;
        first = false;
        lastTick = e.tick;
        o.v[n] = uint8_t(e.note * 2);
        if (o.aux) o.aux[n] = uint8_t(e.vel * 2);
        ++n;
    }
    o.n = n;
    o.width = 64;
    o.mode = kMidi;
    return n;
}

bool decode(const char* name, const uint8_t* file, uint32_t len, Mode mode, DecodeOut& out, MidiEv* midiScratch) {
    out.n = 0;
    out.srcW = out.srcH = 0;
    if (!file || !len) return false;
    if (mode == kAuto || mode >= kModeCount) mode = detectMode(name, file, len);
    uint32_t n = 0;
    switch (mode) {
        case kBits: n = decodeBits(file, len, out); break;
        case kImage: n = decodeImage(file, len, out); break;
        case kWav: n = decodeWav(file, len, out); break;
        case kMidi: n = decodeMidi(file, len, out, midiScratch); break;
        case kText: n = decodeText(file, len, out); break;
        default: break;
    }
    if (!n) n = decodeRaw(file, len, out);   // a mode that does not fit falls back to raw bytes
    return n > 0;
}

// ---- built-in data sets ----------------------------------------------------------
namespace {
const char* const kBuiltins[kBuiltinCount] = {"@PRIMES", "@TESTCARD", "@LFSR", "@CHIRP", "@FIBONACCI"};
}

const char* builtinName(int i) { return (i >= 0 && i < kBuiltinCount) ? kBuiltins[i] : ""; }

int builtinIndex(const char* path) {
    if (!path || path[0] != '@') return -1;
    for (int i = 0; i < kBuiltinCount; ++i)
        if (!std::strcmp(path, kBuiltins[i])) return i;
    return -1;
}

uint32_t makeBuiltin(int i, DecodeOut& o) {
    uint32_t n = 0;
    o.srcW = o.srcH = 0;
    o.width = 256;
    o.mode = kRaw;
    switch (i) {
        case 0: {   // primes: 255 on a prime index, 0 elsewhere (65,536 values)
            n = 65536 < o.cap ? 65536 : o.cap;
            for (uint32_t k = 0; k < n; ++k) o.v[k] = 255;
            o.v[0] = 0;
            if (n > 1) o.v[1] = 0;
            for (uint32_t p = 2; p * p < n; ++p)
                if (o.v[p])
                    for (uint32_t q = p * p; q < n; q += p) o.v[q] = 0;
            for (uint32_t k = 0; k < n; ++k)
                if (o.aux) o.aux[k] = uint8_t(k);
            o.mode = kBits;
            break;
        }
        case 1: {   // a 256 x 256 test card
            n = 65536 < o.cap ? 65536 : o.cap;
            for (uint32_t k = 0; k < n; ++k) {
                const int x = int(k & 255), y = int(k >> 8);
                uint8_t v;
                if (y < 32) {   // barcode: bars of width 1..8 alternating
                    int acc = 0, w = 1, on = 1;
                    while (acc + w <= x) { acc += w; w = w % 8 + 1; on ^= 1; }
                    v = on ? 255 : 0;
                } else if (y < 64) {
                    v = uint8_t(x);   // ramp
                } else if (y < 128) {   // checkerboards of growing size
                    const int s = 1 << (x >> 6);
                    v = (((x / s) ^ (y / s)) & 1) ? 255 : 0;
                } else if (y < 224) {   // zone plate rings
                    const int dx = x - 128, dy = y - 176;
                    v = (((dx * dx + dy * dy) >> 5) & 1) ? 255 : 0;
                } else {   // fine grid
                    v = ((x % 8) == 0 || (y % 8) == 0) ? 255 : 0;
                }
                o.v[k] = v;
                if (o.aux) o.aux[k] = v;
            }
            o.srcW = 256;
            o.srcH = 256;
            o.mode = kImage;
            break;
        }
        case 2: {   // 16-bit maximal LFSR bytes
            n = 65536 < o.cap ? 65536 : o.cap;
            uint16_t s = 0xACE1u;
            for (uint32_t k = 0; k < n; ++k) {
                uint8_t b = 0;
                for (int j = 0; j < 8; ++j) {
                    const uint16_t bit = uint16_t(((s >> 0) ^ (s >> 2) ^ (s >> 3) ^ (s >> 5)) & 1u);
                    s = uint16_t((s >> 1) | (bit << 15));
                    b = uint8_t((b << 1) | (s & 1u));
                }
                o.v[k] = b;
                if (o.aux) o.aux[k] = b;
            }
            break;
        }
        case 3: {   // chirp: a triangle whose period shrinks from 256 to 2 values
            n = 32768 < o.cap ? 32768 : o.cap;
            uint32_t ph = 0;
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t inc = 256u + (k * 32767u) / (n ? n : 1);   // 1/256 .. 1/2 cycle per value (Q16)
                ph += inc;
                const uint32_t p = ph & 0xFFFFu;
                const uint8_t v = uint8_t(p < 0x8000u ? (p >> 7) : ((0xFFFFu - p) >> 7));
                o.v[k] = v;
                if (o.aux) o.aux[k] = v;
            }
            break;
        }
        case 4: {   // Fibonacci word (0/255), quasi-periodic
            n = 65536 < o.cap ? 65536 : o.cap;
            // s(k) = floor((k+2)/phi) - floor((k+1)/phi), phi^-1 ~ 0.6180339887 in Q32
            const uint64_t inv = 2654435769ull;   // 2^32 / phi
            for (uint32_t k = 0; k < n; ++k) {
                const uint64_t a = ((uint64_t(k) + 2) * inv) >> 32, b = ((uint64_t(k) + 1) * inv) >> 32;
                o.v[k] = (a - b) ? 255 : 0;
                if (o.aux) o.aux[k] = o.v[k];
            }
            o.mode = kBits;
            break;
        }
        default: return 0;
    }
    o.n = n;
    return n;
}

}  // namespace dm
