// ============================================================================
//  SRO Asset Studio - fx/FxWindow.h
//
//  When an emitter switches on and off.
//
//  This is NOT simply the object's own command window. If the object carries a
//  StaticEmit command with a real range, THAT range decides the emitter's
//  timeline and the object's union is only the fallback. Getting it from the
//  object alone left 5500 emitters reporting the wrong end time - and an end
//  time is what stops an extinct emitter drawing, so a choreographed effect
//  ran its stages for the wrong length.
//
//  Both values are then offset by the start times accumulated down the tree.
// ============================================================================
#pragma once

#include "../Common.h"
#include "../formats/Efp.h"

namespace sro {
namespace fx {

struct FxWindow { f32 startMs = 0.0f; f32 endMs = 0.0f; };

/// `cefStartMs` is the offset inherited from the ancestors.
///
/// The whole window is resolved in the OBJECT's own time first, and only then
/// shifted by the inherited offset. Comparing an already-shifted end against an
/// already-shifted start picks the wrong branch of the fallback whenever an
/// ancestor contributed an offset.
/// The reference derives this from the StaticEmit command's own time triple,
/// falling back to the object's union and finally to start + 1000.
///
/// The end is REPORTED faithfully but is not a hard gate, and that is not a
/// compromise - it is what the reference actually does. Its viewer sets
/// `emitter.steadyState = playback.looping()` for every emitter, looping is on
/// by default, and a steady-state emitter's only condition is `elapsed >= 0`.
/// The end gate is dormant in the preview. Treating it as a stop condition here
/// switched 80 effects off entirely.
inline FxWindow EmitterWindow(const EfpNode& n, f32 cefStartMs) {
    FxWindow w;
    f32 localStart, localEnd;
    if (n.hasEmitRange && n.emitEndSec > n.emitStartSec) {
        localStart = n.emitStartSec * 1000.0f;
        localEnd   = n.emitEndSec   * 1000.0f;
    } else {
        localStart = n.startSec * 1000.0f;
        const f32 objectEnd = n.endSec * 1000.0f;
        localEnd = objectEnd > localStart ? objectEnd : localStart + 1000.0f;
    }
    w.startMs = cefStartMs + localStart;
    w.endMs   = cefStartMs + localEnd;
    return w;
}

} // namespace fx
} // namespace sro
