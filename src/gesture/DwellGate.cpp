#include "gesture/DwellGate.h"

namespace vmosue {

DwellGate::Kind DwellGate::PickKind(const ActionSet& local) {
  if (local.leftClick)       return Kind::LeftClick;
  if (local.leftDoubleClick) return Kind::DoubleClick;
  if (local.rightClick)      return Kind::RightClick;
  if (local.middleClick)     return Kind::MiddleClick;
  return Kind::None;
}

ActionSet DwellGate::Process(const ActionSet& local, int64_t ts) {
  ActionSet out = local;
  // Out is the pass-through. We zero the four one-shot action
  // flags here and re-set them ONLY when the dwell commits. The
  // other ActionSet fields (cursorX/Y, leftDown/leftUp, wheel,
  // hWheel, safeRelease) are forwarded unchanged.
  out.leftClick = false;
  out.leftDoubleClick = false;
  out.rightClick = false;
  out.middleClick = false;

  // Disabled gate: behave like the legacy code path and let
  // everything through.
  if (cfg_.dwellMs <= 0) {
    out.leftClick       = local.leftClick;
    out.leftDoubleClick = local.leftDoubleClick;
    out.rightClick      = local.rightClick;
    out.middleClick     = local.middleClick;
    return out;
  }

  // Update the per-kind slot. A kind is "asserted" this frame if
  // its action is in `local`; otherwise the slot is reset so the
  // next assertion has to re-accumulate the full dwell window.
  for (int k = 1; k < kSlotCount; ++k) {
    Kind kind = static_cast<Kind>(k);
    bool asserted = false;
    switch (kind) {
      case Kind::LeftClick:   asserted = local.leftClick;       break;
      case Kind::RightClick:  asserted = local.rightClick;      break;
      case Kind::MiddleClick: asserted = local.middleClick;     break;
      case Kind::DoubleClick: asserted = local.leftDoubleClick; break;
      default: break;
    }
    Slot& s = slots_[k];
    if (asserted) {
      if (!s.active) {
        s.active = true;
        s.startMs = ts;
      }
    } else {
      // The gesture was released (or never started) — cancel the
      // dwell so the next gesture has to re-accumulate the full
      // dwell window. The committedMs cooldown is preserved so a
      // brief re-grab within the cooldown still respects the gap.
      s.active = false;
    }
  }

  // Commit the highest-priority kind whose dwell has elapsed and
  // whose cooldown is also clear. Only one kind fires per frame
  // because the priority list is the same one arbitration uses.
  Kind pick = PickKind(local);
  if (pick != Kind::None) {
    Slot& s = slot_(pick);
    if (s.active) {
      int64_t dwell = ts - s.startMs;
      int64_t sinceCommit = ts - s.committedMs;
      if (dwell >= cfg_.dwellMs &&
          (s.committedMs == 0 || sinceCommit >= cfg_.cooldownMs)) {
        s.committedMs = ts;
        // Force a re-dwell: clear active so the next frame's
        // (still-asserted) input re-arms a fresh dwell window.
        s.active = false;
        switch (pick) {
          case Kind::LeftClick:   out.leftClick = true;       break;
          case Kind::DoubleClick: out.leftDoubleClick = true; break;
          case Kind::RightClick:  out.rightClick = true;      break;
          case Kind::MiddleClick: out.middleClick = true;     break;
          default: break;
        }
      }
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
