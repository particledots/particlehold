// From Bryn 0.1.0 (itself from Hillside 0.2.0), unchanged except the namespace (bryn -> particlehold).
// The Shape curve. Every stage moves from its start level towards its target as
//     level = start + (target - start) * f(p),   p = 0 .. 1 through the stage's time.
// f(p) = p is a straight line (Shape 0%). With Shape above 0, f(p) = (1 - e^(-k p)) / (1 - e^(-k)) with k = Shape * 5:
// fast at first and slowing as it arrives, like a capacitor charging. For the attack that is the "logarithmic" rise;
// for decay and release the "exponential" fall (quick drop, long tail). f(0) = 0 and f(1) = 1 exactly at every Shape,
// so stage times never depend on Shape. Our own maths, not taken from any circuit.

#pragma once

#include <cmath>

namespace particlehold
{
    inline constexpr double kShapeMaxK = 5.0;   // Shape 100%: 92% of the way after half the stage's time

    struct Curve
    {
        double k = 0.0, scale = 1.0;
        bool linear = true;

        void setShape (double shape)
        {
            shape = shape < 0.0 ? 0.0 : (shape > 1.0 ? 1.0 : shape);
            k = shape * kShapeMaxK;
            linear = k < 1.0e-4;
            scale = linear ? 1.0 : 1.0 / (1.0 - std::exp (-k));
        }

        double operator() (double p) const
        {
            if (p <= 0.0) return 0.0;
            if (p >= 1.0) return 1.0;
            return linear ? p : (1.0 - std::exp (-k * p)) * scale;
        }
    };
}
