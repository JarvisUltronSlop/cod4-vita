#pragma once
#include <cstdint>
#include <cstring>

namespace kisak::vita::skinmath {
// Inline forms of the source scalar helpers used in the skinning vertex loop.
// Keep operand order and intermediate precision, especially the asymmetric
// R_TransformSkelMat sums. No reassociation, reciprocal approximation or SIMD
// fused arithmetic. Packed/Matrix types are supplied by the source caller.
template<class Packed> inline void Vec3UnpackUnitVec(Packed in, float* out) {
    const float decodeScale = (in.array[3] - -192.0f) / 32385.0f;
    out[0] = (in.array[0] - 127.0f) * decodeScale;
    out[1] = (in.array[1] - 127.0f) * decodeScale;
    out[2] = (in.array[2] - 127.0f) * decodeScale;
}
inline void Vec3Scale(const float* v, float scale, float* result) {
    result[0] = scale * v[0];
    result[1] = scale * v[1];
    result[2] = scale * v[2];
}
inline void Vec3Mad(const float* start, float scale, const float* dir, float* result) {
    result[0] = start[0] + (scale * dir[0]);
    result[1] = start[1] + (scale * dir[1]);
    result[2] = start[2] + (scale * dir[2]);
}
inline float Vec3Dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
template<class Matrix> inline void R_TransformSkelMat(const float* origin, const Matrix* mat, float* out) {
    *out = (float)((float)(*origin * mat->axis[0][0])
        + (float)((float)(mat->axis[2][0] * origin[2]) + (float)(mat->axis[1][0] * origin[1])))
        + mat->origin[0];
    out[1] = (float)((float)(mat->axis[1][1] * origin[1])
        + (float)((float)(mat->axis[0][1] * *origin) + (float)(mat->axis[2][1] * origin[2])))
        + mat->origin[1];
    out[2] = (float)((float)(mat->axis[1][2] * origin[1])
        + (float)((float)(mat->axis[0][2] * *origin) + (float)(mat->axis[2][2] * origin[2])))
        + mat->origin[2];
}
// Same bit mapping as Vec2UnpackTexCoords, expressed without float aliasing.
inline float UnpackTexCoordHalf(uint32_t half) {
    uint32_t bits = 0;
    if (half) bits = (half << 16) & 0x80000000u
        | (((((half << 14) & 0x0fffc000u) - (~(half << 14) & 0x10000000u)) ^ 0x80000000u) >> 1);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
template<class Packed> inline void Vec2UnpackTexCoords(Packed in, float* out) {
    out[0] = UnpackTexCoordHalf(in.packed >> 16);
    out[1] = UnpackTexCoordHalf(in.packed & 0xffffu);
}
}
