// DATAMATICS -- performance: the PERFORM page and pads, throws, fills, scenes
// with morph (SETBOOK-style), and the performance recorder (BLACKBOX-style).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/dm_ws.h"

namespace dm {

using namespace lab;
using kit::ParamId;

// ---- live actions -------------------------------------------------------------------
void Datamatics::setThrows(uint32_t held, uint32_t latch) {
    thHeld_ = held;
    thLatch_ = latch;
    eng_.throws = held | latch;
    perfLog(PE_THROWS, held | latch, 0);
}

void Datamatics::setFill(uint32_t mask) {
    fill_ = mask & 0xFFu;
    eng_.fillMask = fill_;
    perfLog(PE_FILL, fill_, 0);
}

void Datamatics::setSlotSolo(uint32_t mask) {
    slotSoloMask_ = mask & 0xFu;
    eng_.slotSolo = slotSoloMask_;
    perfLog(PE_SLOTSOLO, slotSoloMask_, 0);
}

void Datamatics::launchSection(int i) {
    if (i < 0 || i >= eng_.secCount) return;
    if (ctx_->playing()) eng_.jumpReq = i;
    else eng_.startSec = i;
    secSel_ = i;
    perfLog(PE_SECTION, 0, 0, uint8_t(i));
}

void Datamatics::panic() {
    setThrows(0, 0);
    setFill(0);
    setSlotSolo(0);
    for (int s = 0; s < kStreams; ++s)
        if (eng_.st[s].p[P_SOLO]) setParam(pid_[s][P_SOLO], 0.0f);
    ctx_->toast("Throws, fills and solos released");
}

// ---- PERFORM page ----------------------------------------------------------------------
void Datamatics::performEvent(const Event& e) {
    // Note / Instr / Fx1 / Fx2 hold the first four throws (CUT, FREEZE, STUTTER, DENSE).
    auto keyThrow = [](uint8_t id) {
        switch (id) {
            case btn::Note: return 0;
            case btn::Instr: return 1;
            case btn::Fx1: return 2;
            case btn::Fx2: return 3;
            default: return -1;
        }
    };
    if (e.type == Event::kPress || e.type == Event::kRelease) {
        const int t = keyThrow(e.id);
        if (t >= 0) {
            const uint32_t b = 1u << t;
            if (e.type == Event::kPress && e.shift) setThrows(thHeld_, thLatch_ ^ b);
            else if (e.type == Event::kPress) setThrows(thHeld_ | b, thLatch_);
            else setThrows(thHeld_ & ~b, thLatch_);
            return;
        }
    }
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Left: perfField_ = (perfField_ + 5) % 6; break;
            case btn::Right: perfField_ = (perfField_ + 1) % 6; break;
            case btn::Up: sceneSel_ = (sceneSel_ + kScenes - 1) % kScenes; break;
            case btn::Down: sceneSel_ = (sceneSel_ + 1) % kScenes; break;
            case btn::Insert: recallScene(sceneSel_, true); break;
            case btn::Copy: storeScene(sceneSel_); break;
            case btn::Delete:
                scene_[sceneSel_].used = false;
                ctx_->toast("Scene cleared");
                break;
            case btn::F8:
                perfReplay_ = !perfReplay_;
                ctx_->toast(perfReplay_ ? (perfN_ ? "Replay on: Play replays the take" : "Replay on (no take yet)")
                                        : "Replay off");
                break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        if (slotHeld_ >= 0) {   // hold a slot pad + turn: scrub that slot's data
            nudge(scrubId_[slotHeld_], e.delta, e.shift);
            slotUsed_ = true;
            return;
        }
        static const int kFieldIds = 6;
        (void)kFieldIds;
        ParamId id = kit::kNoParam;
        switch (perfField_) {
            case 0: id = quantId_; break;
            case 1: id = morphId_; break;
            case 2: id = stutId_; break;
            case 3: id = freezeId_; break;
            case 4: id = depthId_; break;
            default: id = scrubId_[scrubSlot_]; break;
        }
        nudge(id, e.delta, e.shift);
    }
}

void Datamatics::performPad(int row, int col, bool down, uint8_t vel, bool shift) {
    (void)vel;
    if (row == 0) {
        if (col < 8) {   // stream mutes (with a slot pad held: patch the stream to that slot)
            if (!down) return;
            const int s = col;
            if (slotHeld_ >= 0) {
                assign(s, slotHeld_);
                slotUsed_ = true;
            } else if (shift) {
                setParam(pid_[s][P_SOLO], float(eng_.st[s].p[P_SOLO] ^ 1));
            } else {
                setParam(pid_[s][P_MUTE], float(eng_.st[s].p[P_MUTE] ^ 1));
            }
            sel_ = s;
            return;
        }
        const int sl = col - 8;   // slot pads: hold = scrub (encoder) / patch (stream pads)
        if (down) {
            slotHeld_ = sl;
            slotUsed_ = false;
        } else if (slotHeld_ == sl) {
            if (!slotUsed_) {
                if (shift) setSlotSolo(slotSoloMask_ ^ (1u << sl));
                else {
                    scrubSlot_ = sl;
                    perfField_ = 5;
                }
            }
            slotHeld_ = -1;
        }
        return;
    }
    if (row == 1 && col < 8) {   // fills: the stream plays every unit while held
        const uint32_t b = 1u << col;
        if (down && shift) setParam(pid_[col][P_SOLO], float(eng_.st[col].p[P_SOLO] ^ 1));
        else if (down) setFill(fill_ | b);
        else setFill(fill_ & ~b);
        return;
    }
    if ((row == 1 || row == 2) && col >= 8) {   // throws (Shift: latch)
        const int t = (row - 1) * 4 + (col - 8);
        const uint32_t b = 1u << t;
        if (down && shift) setThrows(thHeld_, thLatch_ ^ b);
        else if (down) setThrows(thHeld_ | b, thLatch_);
        else setThrows(thHeld_ & ~b, thLatch_);
        return;
    }
    if (!down) return;
    if (row == 2) {   // sections of the current bank
        launchSection(secBank_ * 8 + col);
        return;
    }
    // row 3
    if (col < 8) {
        if (shift) storeScene(col);
        else recallScene(col, true);
        return;
    }
    switch (col) {
        case 8:
            if (perfRec_) perfStop();
            else {
                perfArm_ = !perfArm_;
                ctx_->toast(perfArm_ ? "Performance armed: Play records" : "Performance recording off");
            }
            break;
        case 9:
            perfReplay_ = !perfReplay_;
            ctx_->toast(perfReplay_ ? "Replay on" : "Replay off");
            break;
        case 10: {
            const int banks = (eng_.secCount + 7) / 8;
            secBank_ = banks ? (secBank_ + 1) % banks : 0;
            break;
        }
        default: panic(); break;
    }
}

// ---- scenes ---------------------------------------------------------------------------
void Datamatics::captureScene(Scene& sc) {
    kit::Registry& P = ctx_->params();
    for (int k = 0; k < sceneN_; ++k) sc.v[k] = P.norm(sceneIds_[k]);
}

void Datamatics::applySceneNow(const Scene& sc) {
    kit::Registry& P = ctx_->params();
    for (int k = 0; k < sceneN_; ++k)
        if (fabsf(P.norm(sceneIds_[k]) - sc.v[k]) > 1e-5f) P.setNorm(sceneIds_[k], sc.v[k], false);
}

void Datamatics::storeScene(int i) {
    if (i < 0 || i >= kScenes) return;
    captureScene(scene_[i]);
    scene_[i].used = true;
    sceneSel_ = i;
    char m[32];
    std::snprintf(m, sizeof m, "Scene %d stored", i + 1);
    ctx_->toast(m);
}

void Datamatics::recallScene(int i, bool quantised) {
    if (i < 0 || i >= kScenes) return;
    sceneSel_ = i;
    if (!scene_[i].used) {
        if (!inReplay_) ctx_->toast("Empty scene: Shift + pad stores");
        return;
    }
    perfLog(PE_SCENE, 0, 0, uint8_t(i));
    if (!quantised || !ctx_->playing() || eng_.quant == Q_NOW) {
        startMorph(i);
        morphPending_ = -1;
    } else {
        morphPending_ = i;
        morphAt_ = eng_.boundaryAfter(eng_.quant);
    }
}

void Datamatics::startMorph(int i) {
    kit::Registry& P = ctx_->params();
    for (int k = 0; k < sceneN_; ++k) morphFrom_[k] = P.norm(sceneIds_[k]);
    morphTo_ = i;
    morphStart_ = eng_.clock;
    morphLen_ = morphBars_ > 0 ? uint32_t(beatsQ(uint32_t(4 * morphBars_), 1, ctx_->bpm()) >> 16) : 0;
    if (!morphLen_ || !ctx_->playing()) {
        applySceneNow(scene_[i]);
        morphing_ = false;
        srcMorph_ = 1.0f;
    } else {
        morphing_ = true;
        srcMorph_ = 0.0f;
    }
}

void Datamatics::stepMorph() {
    if (morphPending_ >= 0 && ctx_->playing() && eng_.clock >= morphAt_) {
        const int i = morphPending_;
        morphPending_ = -1;
        startMorph(i);
    }
    if (!morphing_ || morphTo_ < 0) return;
    kit::Registry& P = ctx_->params();
    float t = 1.0f;
    if (ctx_->playing() && morphLen_) t = float(eng_.clock - morphStart_) / float(morphLen_);
    if (t > 1.0f) t = 1.0f;
    const Scene& to = scene_[morphTo_];
    for (int k = 0; k < sceneN_; ++k) {
        const ParamId id = sceneIds_[k];
        const kit::Curve c = P.curve(id);
        float v;
        if (c == kit::Curve::kEnum) v = t >= 0.5f ? to.v[k] : morphFrom_[k];   // switches half way
        else v = morphFrom_[k] + (to.v[k] - morphFrom_[k]) * t;
        if (fabsf(P.norm(id) - v) > 1e-4f) P.setNorm(id, v, false);
    }
    srcMorph_ = t;
    morphProg_ = t;
    if (t >= 1.0f) morphing_ = false;
}

void Datamatics::firstBootScenes() {
    // Four scenes made from the demo: BASE, SPARSE, DENSE, ULTRA.
    kit::Registry& P = ctx_->params();
    for (int i = 0; i <= kScenes; ++i) scene_[i].used = false;
    P.applyNow();
    captureScene(scene_[0]);
    scene_[0].used = true;
    auto variant = [&](int idx, int kind) {
        for (int s = 0; s < kStreams; ++s) {
            const int* p = eng_.st[s].p;
            if (kind == 1) P.set(pid_[s][P_THR], float(p[P_THR] + (255 - p[P_THR]) * 2 / 3), false);
            if (kind == 2) {
                P.set(pid_[s][P_THR], float(p[P_THR] / 4), false);
                P.set(pid_[s][P_LEN], 8.0f, false);   // 8-unit loops: dense, repeating
            }
            if (kind == 3 && p[P_SOUND] == S_SINE) {
                P.set(pid_[s][P_LO], 9000.0f, false);
                P.set(pid_[s][P_HI], 19000.0f, false);
            }
            if (kind == 3 && p[P_SOUND] == S_CLICK) P.set(pid_[s][P_DUR], 0.1f, false);
        }
        P.applyNow();
        captureScene(scene_[idx]);
        scene_[idx].used = true;
        applySceneNow(scene_[0]);
        P.applyNow();
    };
    variant(1, 1);
    variant(2, 2);
    variant(3, 3);
}

// ---- recorder -----------------------------------------------------------------------------
void Datamatics::perfLog(uint8_t type, uint32_t key, uint16_t val, uint8_t a) {
    if (!perfRec_ || inReplay_ || perfN_ >= kPerfMax) return;
    PerfEv& e = perf_[perfN_++];
    e.t = eng_.clock;
    e.type = type;
    e.key = key;
    e.val = val;
    e.a = a;
}

void Datamatics::perfStart() {
    perfN_ = 0;
    perfPos_ = 0;
    captureScene(scene_[kScenes]);
    scene_[kScenes].used = true;
    perfStartSec_ = eng_.startSec;
    perfRec_ = true;
    perfArm_ = false;
    perfReplaying_ = false;
    if (eng_.throws) perfLog(PE_THROWS, eng_.throws, 0);
    if (fill_) perfLog(PE_FILL, fill_, 0);
}

void Datamatics::perfStop() {
    if (!perfRec_) return;
    perfRec_ = false;
    perfLen_ = eng_.clock;
    char m[48];
    std::snprintf(m, sizeof m, "Take: %d events, %lu s", perfN_, static_cast<unsigned long>(perfLen_ / 44100u));
    ctx_->toast(m);
}

void Datamatics::onPlayStarted() {
    if (perfArm_) {
        perfStart();
    } else if (perfReplay_ && perfN_ && scene_[kScenes].used) {
        inReplay_ = true;
        applySceneNow(scene_[kScenes]);
        setThrows(0, 0);
        setFill(0);
        inReplay_ = false;
        eng_.jumpQuantOnce = Q_NOW;
        eng_.jumpReq = perfStartSec_ < eng_.secCount ? perfStartSec_ : 0;
        perfPos_ = 0;
        perfReplaying_ = true;
    }
}

void Datamatics::perfDispatch(const PerfEv& e) {
    kit::Registry& P = ctx_->params();
    inReplay_ = true;
    switch (e.type) {
        case PE_PARAM: {
            const ParamId id = P.findHash(e.key);
            if (P.valid(id)) P.setNorm(id, float(e.val) * (1.0f / 65535.0f), false);
            break;
        }
        case PE_THROWS: setThrows(0, e.key); break;
        case PE_FILL: setFill(e.key); break;
        case PE_SLOTSOLO: setSlotSolo(e.key); break;
        case PE_SECTION: launchSection(e.a); break;
        case PE_SCENE: recallScene(e.a, true); break;
        default: break;
    }
    inReplay_ = false;
}

void Datamatics::perfReplayStep() {
    const uint32_t ser = eng_.playSerial;
    if (ser != playSeen_) {
        playSeen_ = ser;
        onPlayStarted();
    }
    if (!perfReplaying_ || !ctx_->playing()) return;
    const uint32_t now = eng_.clock;
    while (perfPos_ < perfN_ && perf_[perfPos_].t <= now) perfDispatch(perf_[perfPos_++]);
    if (perfPos_ >= perfN_ && now >= perfLen_) {
        perfReplaying_ = false;
        setThrows(0, 0);
        setFill(0);
    }
}

}  // namespace dm
