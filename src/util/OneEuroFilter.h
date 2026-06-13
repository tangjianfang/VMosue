#pragma once
#include <cmath>

// MSVC doesn't expose M_PI from <cmath> unless _USE_MATH_DEFINES is set or
// <math.h> is included with that guard. Define it locally for portability.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vmosue {

class OneEuroFilter {
 public:
  // freq: estimated sampling rate; mincutoff: low-pass cutoff; beta: speed coeff; dcutoff: derivative cutoff
  OneEuroFilter(double freq, double mincutoff = 1.0, double beta = 0.0, double dcutoff = 1.0)
      : freq_(freq), mincutoff_(mincutoff), beta_(beta), dcutoff_(dcutoff),
        xPrev_(0.0), xPrevPrev_(0.0), dPrev_(0.0), initialized_(false) {}

  double Filter(double x, double dt = 0.0) {
    if (dt <= 0.0) dt = 1.0 / freq_;
    if (!initialized_) {
      xPrev_ = x;
      xPrevPrev_ = x;
      dPrev_ = 0.0;
      initialized_ = true;
      return x;
    }
    double dx = (x - xPrev_) / dt;
    double edx = lowpass(dx, dPrev_, smoothingFactor(1.0, dcutoff_, dt));
    double cutoff = mincutoff_ + beta_ * std::fabs(edx);
    double result = lowpass(x, xPrev_, smoothingFactor(1.0, cutoff, dt));
    dPrev_ = edx;
    xPrevPrev_ = xPrev_;
    xPrev_ = result;
    return result;
  }

  void Reset() { initialized_ = false; }

 private:
  static double smoothingFactor(double te, double cutoff, double dt) {
    double r = 2.0 * M_PI * cutoff * dt;
    return r / (r + 1.0);
  }
  static double lowpass(double x, double xPrev, double alpha) {
    return alpha * x + (1.0 - alpha) * xPrev;
  }
  double freq_, mincutoff_, beta_, dcutoff_;
  double xPrev_, xPrevPrev_, dPrev_;
  bool initialized_;
};

}  // namespace vmosue
