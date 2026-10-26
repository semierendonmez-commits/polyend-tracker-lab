// A parameter-table engine base (the KONSOL-native engines): params live in a
// float array described by a PDesc table and SecDesc sections.
#pragma once

#include <cmath>
#include <cstdio>

#include "lab/dsp.h"
#include "ws/engines.h"

namespace lab {
namespace konsol {

class TableEngine : public Engine {
public:
    static constexpr int kMaxP = 28;
    int sections() const override { return nsec_; }
    const char* sectionName(int s) const override { return (s >= 0 && s < nsec_) ? secs_[s].name : ""; }
    int sectionParams(int s, uint8_t* out, int cap) const override {
        if (s < 0 || s >= nsec_) return 0;
        int n = 0;
        for (int i = 0; i < secs_[s].count && n < cap; ++i) out[n++] = uint8_t(secs_[s].first + i);
        return n;
    }
    int paramCount() const override { return np_; }
    void paramName(int i, char* out, int cap) const override {
        std::snprintf(out, size_t(cap), "%s", (i >= 0 && i < np_) ? desc_[i].name : "");
    }
    float get(int i) const override { return (i >= 0 && i < np_) ? p_[i] : 0.0f; }
    void set(int i, float v) override {
        if (i < 0 || i >= np_) return;
        const PDesc& d = desc_[i];
        p_[i] = v < d.min ? d.min : (v > d.max ? d.max : v);
        changed(i);
    }
    void stepParam(int i, int d, bool coarse) override {
        if (i < 0 || i >= np_) return;
        const PDesc& pd = desc_[i];
        float v = p_[i];
        if (pd.fmt == kFHz) {
            v = (v < 1.0f ? 1.0f : v) * exp2f(float(d) * (coarse ? 0.5f : 1.0f / 12.0f));
        } else {
            v += float(d) * pd.step * (coarse ? 10.0f : 1.0f);
        }
        set(i, v);
    }
    void format(int i, char* out, int cap) const override;
    float frac(int i) const override {
        if (i < 0 || i >= np_) return 0.0f;
        const PDesc& d = desc_[i];
        if (d.max <= d.min) return 0.0f;
        if (d.fmt == kFHz) return logf(p_[i] / d.min) / logf(d.max / d.min);
        return (p_[i] - d.min) / (d.max - d.min);
    }
    void reset() override {
        for (int i = 0; i < np_; ++i) p_[i] = desc_[i].def;
        for (int i = 0; i < np_; ++i) changed(i);
    }

protected:
    void bind(const PDesc* d, int n, const SecDesc* s, int ns) {
        desc_ = d;
        np_ = n;
        secs_ = s;
        nsec_ = ns;
        for (int i = 0; i < n; ++i) p_[i] = d[i].def;
    }
    virtual void changed(int i) { (void)i; }
    int ip(int i) const { return int(p_[i] + 0.5f); }
    const PDesc* desc_ = nullptr;
    const SecDesc* secs_ = nullptr;
    int np_ = 0, nsec_ = 0;
    float p_[kMaxP] = {};
};

inline void TableEngine::format(int i, char* out, int cap) const {
    if (i < 0 || i >= np_) {
        out[0] = 0;
        return;
    }
    const PDesc& d = desc_[i];
    const float v = p_[i];
    const size_t c = size_t(cap);
    switch (d.fmt) {
        case kFPct: std::snprintf(out, c, "%d%%", int(v * 100.0f + (v >= 0 ? 0.5f : -0.5f))); break;
        case kFBi: std::snprintf(out, c, "%+d", int(v * 100.0f + (v >= 0 ? 0.5f : -0.5f))); break;
        case kFHz:
            if (v >= 1000.0f) std::snprintf(out, c, "%d.%dk", int(v / 1000.0f), int(v / 100.0f) % 10);
            else std::snprintf(out, c, "%dHz", int(v + 0.5f));
            break;
        case kFMs:
            if (v >= 1000.0f) std::snprintf(out, c, "%d.%02ds", int(v / 1000.0f), int(v / 10.0f) % 100);
            else std::snprintf(out, c, "%dms", int(v + 0.5f));
            break;
        case kFSemi: std::snprintf(out, c, "%+dst", int(v + (v >= 0 ? 0.5f : -0.5f))); break;
        case kFdB: std::snprintf(out, c, "%+ddB", int(v + (v >= 0 ? 0.5f : -0.5f))); break;
        case kFList: {
            const int k = int(v + 0.5f);
            std::snprintf(out, c, "%s", d.labels ? d.labels[k - int(d.min)] : "?");
            break;
        }
        case kFInt: std::snprintf(out, c, "%d", int(v + (v >= 0 ? 0.5f : -0.5f))); break;
        default: {
            const int x = int(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
            std::snprintf(out, c, "%d.%02d", x / 100, (x < 0 ? -x : x) % 100);
            break;
        }
    }
}

}  // namespace konsol
}  // namespace lab
