// PLATFORM native app example: STEPSEQ -- 16-step, 4-track MIDI + internal
// percussion sequencer on the transport clock, sample-accurate in render().
#include <math.h>

#include "platform_app.hpp"

PLAT_APP_INFO("StepSeq", "PLATFORM SDK", "Native C++ 16-step drum sequencer: 4 tracks, MIDI out, swing.", "seq", 32, 0);

class StepSeq : public plat::App {
public:
    static constexpr int kTracks = 4, kSteps = 16;
    volatile uint8_t pat_[kTracks][kSteps] = {};
    volatile int page_ = 0;      // steps 1-12 / 5-16 view on the 12 pad columns
    volatile int playStep_ = -1;
    volatile float swing_ = 0.0f;
    uint8_t notes_[kTracks] = {36, 38, 42, 46};
    // audio-side state
    float env_[kTracks] = {}, ph_[kTracks] = {}, pitch_[kTracks] = {};
    uint32_t rng_ = 12345;
    int lastStep_ = -1;
    // MIDI notes from render() go through a tiny ring to the main loop
    volatile uint32_t mHead_ = 0;
    uint32_t mTail_ = 0;
    uint8_t mNote_[64] = {};

    bool init() override {
        const uint8_t def[kTracks][kSteps] = {
            {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1}, {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0},
            {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1}, {0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0}};
        for (int t = 0; t < kTracks; ++t)
            for (int s = 0; s < kSteps; ++s) pat_[t][s] = def[t][s];
        return true;
    }
    void event(const PlatEvent& e) override {
        if (e.type == PLAT_EV_PAD_DOWN) {
            const int t = e.id / 12, c = e.id % 12;
            const int s = c + (page_ ? 4 : 0);
            pat_[t][s] = pat_[t][s] ? 0 : 1;
        }
        if (e.type == PLAT_EV_PRESS && (e.id == PLAT_BTN_LEFT || e.id == PLAT_BTN_RIGHT)) page_ = e.id == PLAT_BTN_RIGHT;
        if (e.type == PLAT_EV_PRESS && e.id == PLAT_BTN_ENCPUSH) host->setPlaying(host->ctx, !host->playing(host->ctx));
        if (e.type == PLAT_EV_ENCODER) {
            if (e.shift) {
                float s = swing_ + float(e.delta) * 0.02f;
                swing_ = s < 0.0f ? 0.0f : (s > 0.5f ? 0.5f : s);
            } else {
                host->setBpm(host->ctx, host->bpm(host->ctx) + float(e.delta));
            }
        }
    }
    void update(uint32_t) override {
        while (mTail_ != mHead_) {
            const uint8_t n = mNote_[mTail_ & 63];
            host->midiNote(host->ctx, true, n, 110, 9);   // channel 10 relative to the MIDI out channel? (offset 9)
            host->midiNote(host->ctx, false, n, 0, 9);
            ++mTail_;
        }
    }
    // the sounding part: four tiny synthetic drums
    void trigger(int t) {
        env_[t] = 1.0f;
        ph_[t] = 0.0f;
        pitch_[t] = t == 0 ? 1.0f : 0.0f;
        const uint32_t h = mHead_;
        if (h - mTail_ < 64) {
            mNote_[h & 63] = notes_[t];
            mHead_ = h + 1;
        }
    }
    void render(const PlatTransport& tr, const int16_t*, float* l, float* r) override {
        if (tr.startedThisBlock) lastStep_ = -1;
        for (uint32_t i = 0; i < PLAT_BLOCK; ++i) {
            if (tr.playing) {
                // 16th notes with swing on the odd steps
                const double beat = tr.beat + tr.beatsPerSample * double(i);
                const double sixteenth = beat * 4.0;
                const int n = int(sixteenth);
                const double frac = sixteenth - double(n);
                const double delay = (n & 1) ? double(swing_) : 0.0;
                if (n != lastStep_ && frac >= delay) {
                    lastStep_ = n;
                    const int s = n % kSteps;
                    playStep_ = s;
                    for (int t = 0; t < kTracks; ++t)
                        if (pat_[t][s]) trigger(t);
                }
            }
            float out = 0.0f;
            // kick
            if (env_[0] > 1e-4f) {
                pitch_[0] *= 0.9993f;
                ph_[0] += (45.0f + 140.0f * pitch_[0]) / 44100.0f;
                if (ph_[0] >= 1.0f) ph_[0] -= 1.0f;
                out += sinf(6.2831853f * ph_[0]) * env_[0] * 0.8f;
                env_[0] *= 0.99975f;
            }
            for (int t = 1; t < kTracks; ++t) {
                if (env_[t] < 1e-4f) continue;
                rng_ ^= rng_ << 13;
                rng_ ^= rng_ >> 17;
                rng_ ^= rng_ << 5;
                const float nz = float(int32_t(rng_)) * (1.0f / 2147483648.0f);
                const float decay = t == 1 ? 0.9992f : (t == 2 ? 0.996f : 0.9985f);
                out += nz * env_[t] * (t == 1 ? 0.5f : 0.22f);
                env_[t] *= decay;
            }
            l[i] += out * 0.5f;
            r[i] += out * 0.5f;
        }
    }
    void draw() override {
        color(0x39E6B4);
        text(30, 34, PLAT_FONT_HUGE, "STEPSEQ");
        color(0x808890);
        text(32, 92, PLAT_FONT_SMALL, host->playing(host->ctx) ? "playing - Play or encoder push to stop"
                                                                : "press Play (or push the encoder)");
        static const char* const names[kTracks] = {"KICK", "SNARE", "HAT", "OPEN"};
        const int ps = playStep_;
        for (int t = 0; t < kTracks; ++t) {
            const int y = 120 + t * 70;
            color(0xE8ECF0);
            text(30, y + 24, PLAT_FONT_BODY, names[t], PLAT_TEXT_CENTERY);
            for (int s = 0; s < kSteps; ++s) {
                const int x = 120 + s * 42;
                const bool inPage = page_ ? s >= 4 : s < 12;
                if (pat_[t][s]) {
                    color(s == ps ? 0xFFFFFF : (inPage ? 0xFFB347 : 0x805A24));
                    fill(x, y + 4, 36, 40);
                } else {
                    color(s == ps ? 0x8090A0 : (inPage ? 0x2A323C : 0x161B22));
                    frame(x, y + 4, 36, 40);
                }
            }
        }
        color(0x66707C);
        text(400, 440, PLAT_FONT_TINY, "pads: steps (Left/Right: steps 1-12 / 5-16)   enc: tempo   Shift+enc: swing",
             PLAT_TEXT_CENTER);
    }
    void leds(uint8_t* rgb) override {
        const int ps = playStep_;
        for (int t = 0; t < kTracks; ++t)
            for (int c = 0; c < 12; ++c) {
                const int s = c + (page_ ? 4 : 0);
                uint8_t* p = rgb + 3 * (t * 12 + c);
                const bool on = pat_[t][s];
                if (s == ps) { p[0] = 255; p[1] = 255; p[2] = 255; }
                else if (on) { p[0] = 255; p[1] = 150; p[2] = 40; }
                else { p[0] = 6; p[1] = 8; p[2] = 12; }
            }
    }
};

PLAT_APP(StepSeq)
