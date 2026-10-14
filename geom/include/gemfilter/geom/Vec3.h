// gemfilter / geom : minimal 3-vector and rotation helpers (no ROOT dependency).
#pragma once
#include <cmath>

namespace gemfilter {
namespace geom {

struct Vec3 {
  double x = 0, y = 0, z = 0;
  constexpr Vec3() = default;
  constexpr Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
  double mag() const { return std::sqrt(dot(*this)); }
  Vec3 unit() const { double m = mag(); return m > 0 ? *this / m : *this; }
};
inline Vec3 operator*(double s, const Vec3& v) { return v * s; }

// 3x3 rotation matrix, row-major m[row][col].
struct Rot3 {
  double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  Vec3 operator*(const Vec3& v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
  }
  Rot3 operator*(const Rot3& o) const {
    Rot3 r;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        r.m[i][j] = 0;
        for (int k = 0; k < 3; ++k) r.m[i][j] += m[i][k] * o.m[k][j];
      }
    return r;
  }
  Vec3 column(int j) const { return {m[0][j], m[1][j], m[2][j]}; }
  static Rot3 aboutX(double a) {
    Rot3 r; double c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r;
  }
  static Rot3 aboutY(double a) {
    Rot3 r; double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; return r;
  }
  static Rot3 aboutZ(double a) {
    Rot3 r; double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; return r;
  }
};

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

}  // namespace geom
}  // namespace gemfilter
