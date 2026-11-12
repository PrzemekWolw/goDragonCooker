#include "model/ts/ts_math.h"

#include <algorithm>

namespace ts {

Quat normalize(const Quat& q)
{
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-20f) {
        return {};
    }
    const float s = 1.0f / l;
    return {q.x * s, q.y * s, q.z * s, q.w * s};
}

Quat slerp(Quat a, Quat b, float t)
{
    float c = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (c < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        c = -c;
    }
    float s0 = 1 - t, s1 = t;
    if (c < 0.9995f) {
        const float om = std::acos(std::min(c, 1.0f));
        const float so = std::sin(om);
        s0 = std::sin((1 - t) * om) / so;
        s1 = std::sin(t * om) / so;
    }
    return normalize({a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1, a.w * s0 + b.w * s1});
}

Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) {
                s += a.at(i, k) * b.at(k, j);
            }
            r.at(i, j) = s;
        }
    }
    return r;
}

Vec3 mulPoint(const Mat4& m, const Vec3& p)
{
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
            m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

Vec3 mulVector(const Mat4& m, const Vec3& v)
{
    return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z,
            m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z,
            m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z};
}

float determinant3(const Mat4& m)
{
    return m.at(0, 0) * (m.at(1, 1) * m.at(2, 2) - m.at(1, 2) * m.at(2, 1)) -
           m.at(0, 1) * (m.at(1, 0) * m.at(2, 2) - m.at(1, 2) * m.at(2, 0)) +
           m.at(0, 2) * (m.at(1, 0) * m.at(2, 1) - m.at(1, 1) * m.at(2, 0));
}

Mat4 inverse(const Mat4& src)
{
    // Gauss-Jordan with partial pivoting; general 4x4 so skin bind matrices with shear survive.
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = src.at(r, c);
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::abs(a[r][c]) > std::abs(a[pivot][c])) {
                pivot = r;
            }
        }
        if (std::abs(a[pivot][c]) < 1e-30) {
            return {};
        }
        if (pivot != c) {
            for (int k = 0; k < 8; ++k) {
                std::swap(a[c][k], a[pivot][k]);
            }
        }
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) {
            a[c][k] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != c && a[r][c] != 0.0) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    Mat4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.at(r, c) = float(a[r][c + 4]);
        }
    }
    return out;
}

Mat4 fromQuat(const Quat& q)
{
    Mat4 m;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m.at(0, 0) = 1 - 2 * (yy + zz);
    m.at(0, 1) = 2 * (xy - wz);
    m.at(0, 2) = 2 * (xz + wy);
    m.at(1, 0) = 2 * (xy + wz);
    m.at(1, 1) = 1 - 2 * (xx + zz);
    m.at(1, 2) = 2 * (yz - wx);
    m.at(2, 0) = 2 * (xz - wy);
    m.at(2, 1) = 2 * (yz + wx);
    m.at(2, 2) = 1 - 2 * (xx + yy);
    return m;
}

Mat4 composeTRS(const Vec3& t, const Quat& r, const Vec3& s)
{
    Mat4 m = fromQuat(r);
    for (int row = 0; row < 3; ++row) {
        m.at(row, 0) *= s.x;
        m.at(row, 1) *= s.y;
        m.at(row, 2) *= s.z;
    }
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}

Mat4 rigidPart(const Mat4& world)
{
    Vec3 t, s;
    Quat r;
    decompose(world, t, r, s);
    return composeTRS(t, r, {1, 1, 1});
}

Mat4 normalMatrix(const Mat4& m)
{
    const Mat4 inv = inverse(m);
    Mat4 t;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            t.at(r, c) = inv.at(c, r);
        }
    }
    return t;
}

Quat quatFromMatrix(const Mat4& m)
{
    Quat q;
    const float tr = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q.w = 0.25f * s;
        q.x = (m.at(2, 1) - m.at(1, 2)) / s;
        q.y = (m.at(0, 2) - m.at(2, 0)) / s;
        q.z = (m.at(1, 0) - m.at(0, 1)) / s;
    } else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
        const float s = std::sqrt(1.0f + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2;
        q.w = (m.at(2, 1) - m.at(1, 2)) / s;
        q.x = 0.25f * s;
        q.y = (m.at(0, 1) + m.at(1, 0)) / s;
        q.z = (m.at(0, 2) + m.at(2, 0)) / s;
    } else if (m.at(1, 1) > m.at(2, 2)) {
        const float s = std::sqrt(1.0f + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2;
        q.w = (m.at(0, 2) - m.at(2, 0)) / s;
        q.x = (m.at(0, 1) + m.at(1, 0)) / s;
        q.y = 0.25f * s;
        q.z = (m.at(1, 2) + m.at(2, 1)) / s;
    } else {
        const float s = std::sqrt(1.0f + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2;
        q.w = (m.at(1, 0) - m.at(0, 1)) / s;
        q.x = (m.at(0, 2) + m.at(2, 0)) / s;
        q.y = (m.at(1, 2) + m.at(2, 1)) / s;
        q.z = 0.25f * s;
    }
    return normalize(q);
}

void decompose(const Mat4& m, Vec3& t, Quat& r, Vec3& s)
{
    t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    Vec3 c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    Vec3 c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    s = {length(c0), length(c1), length(c2)};
    if (determinant3(m) < 0) {
        s.z = -s.z;
    }
    Mat4 rot;
    for (int row = 0; row < 3; ++row) {
        rot.at(row, 0) = s.x != 0 ? m.at(row, 0) / s.x : 0;
        rot.at(row, 1) = s.y != 0 ? m.at(row, 1) / s.y : 0;
        rot.at(row, 2) = s.z != 0 ? m.at(row, 2) / s.z : 0;
    }
    r = quatFromMatrix(rot);
}

Quat toQuat(const Quat16& q)
{
    const float k = 1.0f / 32767.0f;
    return normalize(Quat{-q.x * k, -q.y * k, -q.z * k, q.w * k});
}

Quat16 toQuat16(const Quat& in)
{
    Quat q = normalize(in);
    // keep w positive so the stored form is canonical across tools
    if (q.w < 0) {
        q = {-q.x, -q.y, -q.z, -q.w};
    }
    auto pack = [](float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); };
    return {pack(-q.x), pack(-q.y), pack(-q.z), pack(q.w)};
}

} // namespace ts
