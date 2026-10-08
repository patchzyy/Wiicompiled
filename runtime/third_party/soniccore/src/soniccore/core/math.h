#pragma once
#include <cmath>
#include <algorithm>

namespace sonic {

constexpr float PI = 3.14159265358979323846f;

// Ninja "BAMS" angle: 0x10000 = 360 degrees
inline float bamsToRad(int a) { return float(a) * (2.0f * PI / 65536.0f); }
inline int radToBams(float r) { return int(r * (65536.0f / (2.0f * PI))); }

struct Vec2 {
    float x = 0, y = 0;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline float length2(const Vec3& a) { return dot(a, a); }
inline Vec3 normalize(const Vec3& a, const Vec3& fallback = Vec3(0, 1, 0)) {
    float l = length(a);
    return l > 1e-8f ? a / l : fallback;
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Project v onto the plane with normal n
inline Vec3 projectOnPlane(const Vec3& v, const Vec3& n) { return v - n * dot(v, n); }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};

// Column-major 4x4 matrix (OpenGL convention, column vectors).
struct Mat4 {
    float m[16];
    Mat4() { identity(); }
    void identity() {
        for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.f : 0.f;
    }
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += at(rr, k) * b.at(k, c);
                r.at(rr, c) = s;
            }
        return r;
    }
    Vec3 transformPoint(const Vec3& v) const {
        return {at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3),
                at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3),
                at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3)};
    }
    Vec3 transformDir(const Vec3& v) const {
        return {at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z,
                at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z,
                at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z};
    }
    Vec3 translation() const { return {at(0, 3), at(1, 3), at(2, 3)}; }

    static Mat4 translate(const Vec3& t) {
        Mat4 r;
        r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
        return r;
    }
    static Mat4 rotX(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(1, 1) = c; r.at(1, 2) = -s; r.at(2, 1) = s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotY(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(0, 0) = c; r.at(0, 2) = s; r.at(2, 0) = -s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotZ(float a) {
        Mat4 r; float c = std::cos(a), s = std::sin(a);
        r.at(0, 0) = c; r.at(0, 1) = -s; r.at(1, 0) = s; r.at(1, 1) = c;
        return r;
    }
    // Build a rotation whose columns are the given basis vectors.
    static Mat4 basis(const Vec3& x, const Vec3& y, const Vec3& z) {
        Mat4 r;
        r.at(0, 0) = x.x; r.at(1, 0) = x.y; r.at(2, 0) = x.z;
        r.at(0, 1) = y.x; r.at(1, 1) = y.y; r.at(2, 1) = y.z;
        r.at(0, 2) = z.x; r.at(1, 2) = z.y; r.at(2, 2) = z.z;
        return r;
    }
    static Mat4 perspective(float fovy, float aspect, float zn, float zf) {
        Mat4 r;
        float f = 1.0f / std::tan(fovy * 0.5f);
        for (float& v : r.m) v = 0;
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        r.at(2, 2) = (zf + zn) / (zn - zf);
        r.at(2, 3) = (2 * zf * zn) / (zn - zf);
        r.at(3, 2) = -1;
        return r;
    }
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = normalize(target - eye, Vec3(0, 0, -1));
        Vec3 s = normalize(cross(f, up), Vec3(1, 0, 0));
        Vec3 u = cross(s, f);
        Mat4 r;
        r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z;
        r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z;
        r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z;
        r.at(0, 3) = -dot(s, eye);
        r.at(1, 3) = -dot(u, eye);
        r.at(2, 3) = dot(f, eye);
        return r;
    }
    // Inverse of an affine (rotation/scale + translation) matrix.
    Mat4 affineInverse() const {
        // general 3x3 inverse
        float a = at(0, 0), b = at(0, 1), c = at(0, 2);
        float d = at(1, 0), e = at(1, 1), f = at(1, 2);
        float g = at(2, 0), h = at(2, 1), i = at(2, 2);
        float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        float det = a * A + b * B + c * C;
        if (std::fabs(det) < 1e-12f) return Mat4();
        float id = 1.0f / det;
        Mat4 r;
        r.at(0, 0) = A * id; r.at(0, 1) = -(b * i - c * h) * id; r.at(0, 2) = (b * f - c * e) * id;
        r.at(1, 0) = B * id; r.at(1, 1) = (a * i - c * g) * id; r.at(1, 2) = -(a * f - c * d) * id;
        r.at(2, 0) = C * id; r.at(2, 1) = -(a * h - b * g) * id; r.at(2, 2) = (a * e - b * d) * id;
        Vec3 t = translation();
        Vec3 nt = r.transformDir(t);
        r.at(0, 3) = -nt.x; r.at(1, 3) = -nt.y; r.at(2, 3) = -nt.z;
        return r;
    }
    // Normal matrix (inverse-transpose of upper 3x3), returned as Mat4
    Mat4 normalMatrix() const {
        Mat4 inv = affineInverse();
        Mat4 r;
        for (int rr = 0; rr < 3; rr++)
            for (int cc = 0; cc < 3; cc++) r.at(rr, cc) = inv.at(cc, rr);
        return r;
    }
};

// Ninja object transform: T * R * S with rotation order X,Y,Z (or Z,X,Y).
// Ninja object transform. With column vectors the rotation is applied X first,
// then Y, then Z (M = T * Rz * Ry * Rx * S); objects flagged NJD_EVAL_ZXY_ANG
// rotate Z first, then X, then Y (M = T * Ry * Rx * Rz * S).
inline Mat4 ninjaTransform(const Vec3& pos, const int ang[3], const Vec3& scl, bool zxy) {
    Mat4 X = Mat4::rotX(bamsToRad(ang[0])), Y = Mat4::rotY(bamsToRad(ang[1])), Z = Mat4::rotZ(bamsToRad(ang[2]));
    Mat4 r = zxy ? Y * X * Z : Z * Y * X;
    return Mat4::translate(pos) * r * Mat4::scale(scl);
}

}  // namespace sonic
