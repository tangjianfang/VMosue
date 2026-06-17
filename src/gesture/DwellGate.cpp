#include "gesture/DwellGate.h"

#include <array>

namespace vmosue {

DwellGate::Kind DwellGate::PickKind(const ActionSet& local) {
  if (local.leftClick)       return Kind::LeftClick;
  if (local.leftDoubleClick) return Kind::DoubleClick;
  if (local.rightClick)      return Kind::RightClick;
  if (local.middleClick)     return Kind::MiddleClick;
  return Kind::None;
}

// v0.6.2: mapping from Kind to the two per-frame signals that
// drive the slot — (a) the "currently held" signal that keeps the
// slot armed across consecutive frames, and (b) the "release event"
// that gates the actual commit. Each Kind has its own (held, event)
// pair; this helper centralizes the lookup so Process() stays
// readable.
static void HeldAndEvent(DwellGate::Kind kind, const ActionSet& local,
                         bool& heldOut, bool& eventOut) {
  switch (kind) {
    case DwellGate::Kind::LeftClick:
      heldOut  = local.leftPinchHeld;
      eventOut = local.leftClick;
      break;
    case DwellGate::Kind::MiddleClick:
      heldOut  = local.middlePinchHeld;
      eventOut = local.middleClick;
      break;
    case DwellGate::Kind::RightClick:
      heldOut  = local.rightPushHeld;
      eventOut = local.rightClick;
      break;
    case DwellGate::Kind::DoubleClick:
      // Double-click is two consecutive single-click releases within
      // doubleClickWindowMs. Dwell-wise we treat it identically to
      // LeftClick: it dwells on the same pinch-held signal, and the
      // "release event" is leftDoubleClick instead of leftClick.
      heldOut  = local.leftPinchHeld;
      eventOut = local.leftDoubleClick;
      break;
    default:
      heldOut = false;
      eventOut = false;
      break;
  }
}

ActionSet DwellGate::Process(const ActionSet& local, int64_t ts) {
  ActionSet out = local;
  // Out is the pass-through. We zero the four one-shot action
  // flags here and re-set them ONLY when the dwell commits. The
  // other ActionSet fields (cursorX/Y, leftDown/leftUp, wheel,
  // hWheel, safeRelease) are forwarded unchanged. The *Held
  // signals are also forwarded so downstream consumers (e.g. the
  // overlay) can show live state.
  out.leftClick = false;
  out.leftDoubleClick = false;
  out.rightClick = false;
  out.middleClick = false;

  // Disabled gate: behave like the legacy code path and let
  // everything through (every release event fires immediately).
  if (cfg_.dwellMs <= 0) {
    out.leftClick       = local.leftClick;
    out.leftDoubleClick = local.leftDoubleClick;
    out.rightClick      = local.rightClick;
    out.middleClick     = local.middleClick;
    return out;
  }

  // Compute per-kind (held, event) once. The commit check below
  // runs BEFORE the disarm pass so a release event arriving in
  // the same frame as the gesture ending can still fire — we
  // need s.active=true at the moment of the event, even though
  // we will set s.active=false on the disarm pass.
  struct KindState { bool held; bool event; };
  std::array<KindState, kSlotCount> states{};
  for (int k = 1; k < kSlotCount; ++k) {
    HeldAndEvent(static_cast<Kind>(k), local, states[k].held, states[k].event);
  }

  // Commit each kind whose release event fires AND whose dwell
  // elapsed AND whose cooldown is clear. We commit in priority
  // order (LeftClick > RightClick > MiddleClick > DoubleClick)
  // and short-circuit once one kind fires (the GestureStateMachine
  // already arbitrates so usually only one event arrives per
  // frame, but if two slip through we want the left-wins behavior
  // here too).
  bool fired = false;
  for (int k = 1; k < kSlotCount && !fired; ++k) {
    Kind kind = static_cast<Kind>(k);
    if (!states[k].event) continue;  // no release event this frame
    Slot& s = slot_(kind);
    if (!s.active) continue;         // never held → never armed
    int64_t dwell = ts - s.startMs;
    int64_t sinceCommit = ts - s.committedMs;
    if (dwell < cfg_.dwellMs) continue;
    if (s.committedMs != 0 && sinceCommit < cfg_.cooldownMs) continue;
    s.committedMs = ts;
    // Force a re-dwell: clear active so a continued pinch
    // (without release) has to re-accumulate the full dwell window
    // before the NEXT release event can fire.
    s.active = false;
    switch (kind) {
      case Kind::LeftClick:   out.leftClick = true;       break;
      case Kind::DoubleClick: out.leftDoubleClick = true; break;
      case Kind::RightClick:  out.rightClick = true;      break;
      case Kind::MiddleClick: out.middleClick = true;     break;
      default: break;
    }
    fired = true;
  }

  // Disarm / arm each slot based on the held signal. Done AFTER
  // the commit pass so the release frame can still see s.active
  // and commit. The committedMs cooldown is preserved across
  // disarm so a brief re-grab within the cooldown still respects
  // the gap.
  for (int k = 1; k < kSlotCount; ++k) {
    Slot& s = slots_[k];
    if (states[k].held) {
      if (!s.active) {
        s.active = true;
        s.startMs = ts;
      }
    } else {
      s.active = false;
    }
  }

  // Cache the most-progressed candidate as the preview. We pick
  // the kind with the largest `dwell / dwellMs` so the overlay
  // shows what is *closest* to firing (i.e. what the user is
  // about to do) rather than the most-recently-started.
  float best = 0.0f;
  Kind bestKind = Kind::None;
  int64_t bestStart = 0;
  for (int k = 1; k < kSlotCount; ++k) {
    const Slot& s = slots_[k];
    if (!s.active) continue;
    int64_t elapsed = ts - s.startMs;
    float p = static_cast<float>(elapsed) /
              static_cast<float>(cfg_.dwellMs);
    if (p > best) {
      best = p;
      bestKind = static_cast<Kind>(k);
      bestStart = s.startMs;
    }
  }
  if (bestKind == Kind::None || best <= 0.0f) {
    lastPreview_ = Preview{};
  } else {
    if (best > 1.0f) best = 1.0f;
    lastPreview_.kind = bestKind;
    lastPreview_.progress = best;
    lastPreview_.totalMs = cfg_.dwellMs;
    int64_t rem = (ts - bestStart) - cfg_.dwellMs;
    lastPreview_.remainingMs = rem > 0 ? 0 : -static_cast<int>(rem);
  }
  return out;
}

void DwellGate::Reset() {
  for (auto& s : slots_) {
    s.active = false;
    s.startMs = 0;
    s.committedMs = 0;
  }
  lastPreview_ = Preview{};
}

DwellGate::Preview DwellGate::CurrentPreview(int64_t nowMs) const {
  // Recompute the progress on read so the overlay shows a live
  // countdown even if the gesture thread is stalled. We use
  // lastPreview_'s startMs (not slots_[k].startMs) so the answer
  // is stable for the action that *was* the most-progressed at
  // the last Process() call — switching kinds mid-read would
  // look glitchy.
  if (lastPreview_.kind == Kind::None || lastPreview_.totalMs <= 0) {
    return Preview{};
  }
  int idx = index_(lastPreview_.kind);
  const Slot& s = slots_[idx];
  if (!s.active) {
    // The action was released between the last Process() and
    // now; show a fresh "None" rather than a stale countdown.
    return Preview{};
  }
  int64_t elapsed = nowMs - s.startMs;
  float p = static_cast<float>(elapsed) /
            static_cast<float>(lastPreview_.totalMs);
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  Preview out = lastPreview_;
  out.progress = p;
  out.remainingMs = static_cast<int>(lastPreview_.totalMs - elapsed);
  if (out.remainingMs < 0) out.remainingMs = 0;
  return out;
}

}  // namespace vmosue
