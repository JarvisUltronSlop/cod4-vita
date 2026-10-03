#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace kisak::vita {
// Only finite, bounded vertex components reach the integer conversion. Keep
// ties away from zero, as lround does. Adding 0.5 before truncating would round
// some floats just below a tie incorrectly, so compare the exact remainder.
inline int RoundVertexComponent(float scaled) {
    const int integral = static_cast<int>(scaled);
    const float remainder = scaled - static_cast<float>(integral);
    return integral + (remainder >= 0.5f) - (remainder <= -0.5f);
}
// The packed attributes are finite and clamped before scaling, so their
// magnitude is at most 255. Decode that rounded float's exact binary fraction
// instead of converting it to int, back to float, and comparing a remainder.
// The half-unit is added to the integer significand, never to the float: this
// preserves values immediately below a tie and ties away from zero.
inline uint32_t RoundPackedVertexMagnitude(uint32_t magnitudeBits) {
    if (magnitudeBits < 0x3f000000u) return 0; // magnitude < 0.5, includes zero
    const uint32_t shift = 150u - (magnitudeBits >> 23); // 16..24 for [0.5,255]
    const uint32_t significand = (magnitudeBits & 0x7fffffu) | 0x800000u;
    return (significand + (1u << (shift - 1))) >> shift;
}
inline int8_t PackVertexSnorm(float value) {
    uint32_t input;std::memcpy(&input,&value,sizeof(input));
    const uint32_t magnitude = input & 0x7fffffffu;
    // Positive binary32 bit order is magnitude order. Classify before any FP
    // comparison: NaN/infinity map to zero, finite saturation maps directly to
    // its final byte. This also avoids multiplying/rounding already saturated
    // attributes. For the interior retain the original multiply and rounding.
    if (magnitude >= 0x7f800000u) return 0;
    if (magnitude >= 0x3f800000u) return input >> 31 ? -127 : 127;
    const float scaled = value * 127.0f;
    uint32_t bits;std::memcpy(&bits,&scaled,sizeof(bits));
    const int rounded = int(RoundPackedVertexMagnitude(bits & 0x7fffffffu));
    return static_cast<int8_t>(bits >> 31 ? -rounded : rounded);
}
inline uint8_t PackVertexUnorm8(float value) {
    uint32_t input;std::memcpy(&input,&value,sizeof(input));
    // Negative values (including -0 and negative NaNs) and nonfinite positive
    // values produce zero. Finite values >= 1 produce 255 without FP work.
    if (input >= 0x7f800000u) return 0;
    if (input >= 0x3f800000u) return 255;
    const float scaled = value * 255.0f;
    uint32_t bits;std::memcpy(&bits,&scaled,sizeof(bits));
    return static_cast<uint8_t>(RoundPackedVertexMagnitude(bits & 0x7fffffffu));
}
}
