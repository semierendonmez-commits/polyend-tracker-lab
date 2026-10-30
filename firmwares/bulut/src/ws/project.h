// BULUT projects on the card and the gesture recorder (main loop only).
//
// Project file: "BLUT" magic, format version, then tagged chunks
// [tag 4 B][length u32][data]. Unknown chunks are skipped, so later versions
// can add chunks without breaking older files; a newer *major* version is
// refused.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "lab/lab.h"
#include "ws/bulut.h"

namespace bulut {

constexpr uint16_t kProjectVersion = 2;   // 2: four channels + the LABKIT registry chunk
// Minor 1: TAPE FILE (the TAPE chunk; scenes' SOURCE values count 5 sources).
// A minor-0 file's scenes are mapped from 4 sources on load.
constexpr uint16_t kProjectMinor = 1;

// Everything a project holds (pointers into the workstation's memory).
struct ProjectRefs {
    ChState* states = nullptr;          // kCh (switches, every engine's knobs)
    ChSource* sources = nullptr;        // kCh (sample paths, labels)
    Gesture* gestures[kCh] = {};        // the playing gesture of each channel
    lab::kit::Registry* reg = nullptr;  // every parameter, routes, macros, MIDI learn
    uint8_t* scratch = nullptr;         // for the registry chunk
    uint32_t scratchCap = 0;
    Scene* scenes = nullptr;            // kScenes
    TimeEntry* timeline = nullptr;      // kTimeline
    int* timelineLen = nullptr;
    FreezeSlot* freezes = nullptr;      // kFreezeSlots
    float* bpm = nullptr;
    int* songMode = nullptr;
    TapeSave* tapes = nullptr;          // kCh (optional)
};

enum class ProjResult : uint8_t { kOk, kNoCard, kIoError, kBadFile, kTooNew, kPartial, kTooOld };
ProjResult saveProject(ot::IStorage* fs, const char* path, const ProjectRefs& p);
ProjResult loadProject(ot::IStorage* fs, const char* path, const ProjectRefs& p);
const char* projResultText(ProjResult r);

// ---- gesture recorder -------------------------------------------------------
// A gesture records one channel's pad presses and parameter moves with sample
// timestamps. New: the loop length is the time from the first movement to
// Stop (rounded to beats or bars when quantised). Overdub: new movements are
// merged into the playing loop at the loop position they happened.
class GestureRecorder {
public:
    enum State : uint8_t { kIdle = 0, kArmed, kRecording };
    void arm(int cloud, bool overdub);
    // Overdub needs the playing loop and its position (samples).
    void start(uint32_t nowSamples, const Gesture* live, uint32_t livePhase, float liveSpeed = 1.0f);
    void pad(bool down, int pad, int vel, uint32_t nowSamples);
    void param(int index, float norm, uint32_t nowSamples);   // channel param index 0..35
    // Finish into `out` (the idle buffer). quantSamples 0 = free length.
    // Returns false when nothing was recorded (out untouched).
    bool finish(uint32_t nowSamples, uint32_t quantSamples, Gesture& out, const Gesture* live);
    void cancel() { state_ = kIdle; }
    State state() const { return state_; }
    int cloud() const { return cloud_; }
    bool overdub() const { return overdub_; }
    uint32_t elapsed(uint32_t nowSamples) const { return state_ == kRecording ? nowSamples - t0_ : 0; }
    int count() const { return n_; }

private:
    uint32_t stamp(uint32_t now) const;
    void add(uint8_t type, uint8_t a, uint16_t v, uint32_t now);
    State state_ = kIdle;
    bool overdub_ = false;
    int cloud_ = 0;
    uint32_t t0_ = 0, base_ = 0, loopLen_ = 0;
    float speed_ = 1.0f;
    uint64_t held_ = 0;
    int n_ = 0;
    GestEvent ev_[kGestEvents];
};

// Sort events by time (stable insertion sort; counts are small).
void sortEvents(GestEvent* ev, int n);

}  // namespace bulut
