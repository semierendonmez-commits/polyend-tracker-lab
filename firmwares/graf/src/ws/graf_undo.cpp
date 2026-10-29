// GRAF -- undo / redo, auto-layout of a canvas, follow-the-signal.
//
// Undo keeps the patch structure (modules, knobs, sequences, cables, strips;
// not the snapshots) before every edit, 24 deep. Restoring keeps the DSP state
// of every module that is still the same module in the same slot, so undoing a
// knob or a cable does not reset a sequencer or clear a delay. Knob edits of
// one parameter within 2 s are one step.
#include <cstring>

#include "ws/graf_ws.h"

namespace graf {

void Graf::checkpoint(int kind, int target) {
    if (!undoMem_) return;
    // kind 2 = a knob: one step per knob gesture
    if (kind == 2 && kind == lastCpKind_ && target == lastCpTarget_ && now_ - lastCpMs_ < 2000) {
        lastCpMs_ = now_;
        return;
    }
    lastCpKind_ = kind;
    lastCpTarget_ = target;
    lastCpMs_ = now_;
    const uint32_t n = eng_.serialize(undoMem_ + uint32_t(undoTop_) * kUndoBytes, kUndoBytes, false);
    if (!n) return;   // too big for a slot (cannot happen with 64 modules / 160 cables)
    undoLen_[undoTop_] = n;
    undoTop_ = (undoTop_ + 1) % kUndo;
    if (nUndo_ < kUndo) ++nUndo_;
    nRedo_ = 0;
}

namespace {
bool restore(Engine& e, const uint8_t* buf, uint32_t len) { return e.deserialize(buf, len, true); }
}  // namespace

bool Graf::undo() {
    if (!nUndo_) return false;
    // the present goes onto the redo stack
    const uint32_t n = eng_.serialize(redoMem_ + uint32_t(redoTop_) * kUndoBytes, kUndoBytes, false);
    if (n) {
        redoLen_[redoTop_] = n;
        redoTop_ = (redoTop_ + 1) % kUndo;
        if (nRedo_ < kUndo) ++nRedo_;
    }
    undoTop_ = (undoTop_ + kUndo - 1) % kUndo;
    --nUndo_;
    std::memcpy(stateBuf_, undoMem_ + uint32_t(undoTop_) * kUndoBytes, undoLen_[undoTop_]);
    restore(eng_, stateBuf_, undoLen_[undoTop_]);
    lastCpKind_ = -1;
    if (sel_ >= 0 && !eng_.mod(sel_).used) sel_ = -1;
    rebuildJacks();
    pagesDirty_ = true;
    return true;
}

bool Graf::redo() {
    if (!nRedo_) return false;
    const uint32_t n = eng_.serialize(undoMem_ + uint32_t(undoTop_) * kUndoBytes, kUndoBytes, false);
    if (n) {
        undoLen_[undoTop_] = n;
        undoTop_ = (undoTop_ + 1) % kUndo;
        if (nUndo_ < kUndo) ++nUndo_;
    }
    redoTop_ = (redoTop_ + kUndo - 1) % kUndo;
    --nRedo_;
    std::memcpy(stateBuf_, redoMem_ + uint32_t(redoTop_) * kUndoBytes, redoLen_[redoTop_]);
    restore(eng_, stateBuf_, redoLen_[redoTop_]);
    lastCpKind_ = -1;
    if (sel_ >= 0 && !eng_.mod(sel_).used) sel_ = -1;
    rebuildJacks();
    pagesDirty_ = true;
    return true;
}

// Auto-layout: columns by signal depth (the longest chain of cables from a
// module with no inputs on this canvas), rows in signal order; a column that
// fills up spills into the next one. Outputs (OUT) go to the far right.
void Graf::tidy(int patch) {
    int mods[kCells], n = 0;
    for (int c = 0; c < kCells; ++c) {
        const int s = eng_.slotAtCell(patch, c);
        if (s >= 0) mods[n++] = s;
    }
    if (n == 0) return;
    checkpoint(1);
    // depth by relaxation over the canvas's cables (cycles: capped)
    int depth[kMaxSlots] = {};
    for (int it = 0; it < n; ++it) {
        bool moved = false;
        for (int c = 0; c < kMaxCables; ++c) {
            const Cable& k = eng_.cable(c);
            if (!k.used || k.src == k.dst) continue;
            const Module& a = eng_.mod(k.src);
            const Module& b = eng_.mod(k.dst);
            if (a.patch != patch || b.patch != patch) continue;
            if (depth[k.dst] < depth[k.src] + 1 && depth[k.src] + 1 < 12) {
                depth[k.dst] = depth[k.src] + 1;
                moved = true;
            }
        }
        if (!moved) break;
    }
    int maxD = 0;
    for (int i = 0; i < n; ++i)
        if (depth[mods[i]] > maxD) maxD = depth[mods[i]];
    for (int i = 0; i < n; ++i)
        if (eng_.mod(mods[i]).type == kOut) depth[mods[i]] = maxD + 1;
    // order: by depth, then signal order
    int order[kCells], no = 0;
    int sig[kMaxSlots];
    const int ns = eng_.signalOrder(patch, sig, kMaxSlots);
    for (int d = 0; d <= maxD + 1; ++d)
        for (int i = 0; i < ns; ++i)
            if (depth[sig[i]] == d) order[no++] = sig[i];
    // columns: spread the depths over the 12 columns
    const int levels = maxD + 2;
    const int step = levels <= 6 ? 2 : 1;
    int target[kMaxSlots];
    bool taken[kCells] = {};
    int col = 0, lastD = -1, row = 0;
    for (int i = 0; i < no; ++i) {
        const int s = order[i];
        if (depth[s] != lastD) {
            if (lastD >= 0) col += step;
            lastD = depth[s];
            row = 0;
        }
        if (row >= 4) {
            ++col;
            row = 0;
        }
        if (col > 11) col = 11;
        int cell = row * lab::kPadCols + col;
        while (taken[cell]) cell = (cell + 1) % kCells;
        taken[cell] = true;
        target[s] = cell;
        ++row;
    }
    // move: park every module on a free high cell first would need room; swap instead
    for (int i = 0; i < no; ++i) eng_.move(order[i], target[order[i]]);
    // a swap may have moved an earlier one: repeat until stable
    for (int pass = 0; pass < 4; ++pass)
        for (int i = 0; i < no; ++i)
            if (eng_.mod(order[i]).cell != target[order[i]]) eng_.move(order[i], target[order[i]]);
    if (sel_ >= 0 && eng_.mod(sel_).used) cursor_ = eng_.mod(sel_).cell;
    rebuildJacks();
    pagesDirty_ = true;
}

// Note / Instr: the selection walks against / along the signal; repeated
// presses cycle through the branches.
void Graf::follow(bool down) {
    if (sel_ < 0 || !eng_.mod(sel_).used) return;
    int list[16];
    const int n = down ? eng_.downstream(sel_, list, 16) : eng_.upstream(sel_, list, 16);
    if (n <= 0) {
        ctx_->toast(down ? "Nothing patched from here" : "Nothing patched into here");
        return;
    }
    const int s = list[followIdx_ % n];
    followIdx_ = 0;
    if (eng_.mod(s).patch != patch_) {
        patch_ = eng_.mod(s).patch;
        rebuildJacks();
    }
    select(s);
}

}  // namespace graf
