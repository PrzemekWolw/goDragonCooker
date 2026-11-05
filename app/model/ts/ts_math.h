#pragma once

#include <cmath>
#include <cstdint>

namespace ts {

struct Vec2 {
    float x = 0, y = 0;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v)
{
    const float l = length(v);
    return l > 1e-20f ? v * (1.0f / l) : Vec3{0, 0, 0};
}

// Standard Hamilton quaternion (w + xi + yj + zk), rotating column vectors.
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

Quat normalize(const Quat& q);
Quat mul(const Quat& a, const Quat& b);
Quat conjugate(const Quat& q);
Quat slerp(Quat a, Quat b, float t);
Vec3 rotate(const Quat& q, const Vec3& v);

// Row-major 4x4 acting on column vectors (p' = M * p), same element layout as the engine's MatrixF.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float& at(int r, int c) { return m[r * 4 + c]; }
    float at(int r, int c) const { return m[r * 4 + c]; }
};

Mat4 operator*(const Mat4& a, const Mat4& b);
Vec3 mulPoint(const Mat4& m, const Vec3& p);
Vec3 mulVector(const Mat4& m, const Vec3& v);
Mat4 inverse(const Mat4& m);
float determinant3(const Mat4& m);
Mat4 composeTRS(const Vec3& t, const Quat& r, const Vec3& s);
Mat4 fromQuat(const Quat& q);
// Splits an affine matrix into translation, rotation and per-axis scale (negative determinant goes to scale.z).
void decompose(const Mat4& m, Vec3& t, Quat& r, Vec3& s);
Quat quatFromMatrix(const Mat4& m);

// Engine Quat16: 4 x int16 scaled by 0x7fff. The engine's QuatF builds the transpose of the
// standard rotation matrix, so a stored (x,y,z,w) is the standard quaternion (-x,-y,-z,w).
struct Quat16 {
    int16_t x = 0, y = 0, z = 0, w = 0x7fff;
    bool operator==(const Quat16& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
};

Quat toQuat(const Quat16& q);
Quat16 toQuat16(const Quat& q);

} // namespace ts
