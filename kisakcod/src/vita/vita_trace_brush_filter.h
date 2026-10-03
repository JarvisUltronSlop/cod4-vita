#pragma once

namespace kisak::vita {
// The reconstructed source leaf contains a flat, ordered brush list. Reject
// distant brushes before entering the original (large) narrow-phase routine.
// No fraction clipping: even brushes after an earlier hit retain source order.
struct TraceBrushFilter {
    float low[3], high[3];
    TraceBrushFilter(const float* start, const float* end) {
        for (int axis = 0; axis != 3; ++axis) {
            low[axis] = start[axis] < end[axis] ? start[axis] : end[axis];
            high[axis] = start[axis] > end[axis] ? start[axis] : end[axis];
        }
    }
    bool Reject(const float* mins, const float* maxs, const float* radiusOffset) const {
        for (int axis = 0; axis != 3; ++axis) {
            // Keep the subtraction/sign/radius order of CM_TraceThroughBrush.
            // If the nearest endpoint is strictly beyond its 0.125 tolerance,
            // both source distances are beyond it and that routine returns
            // without changing any trace field. Equality deliberately passes.
            if ((high[axis] - mins[axis]) * -1.0f - radiusOffset[axis] > 0.125f ||
                (low[axis] - maxs[axis]) * 1.0f - radiusOffset[axis] > 0.125f)
                return true;
        }
        return false;
    }
};
}
