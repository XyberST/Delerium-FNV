// Small vector / quaternion / 3x3 helpers. Matrices are row-major float[9], applied as v' = R * v (Gamebryo's NiMatrix3 convention).
#pragma once
#include <cmath>
struct V3 { float x, y, z; };
struct Q { float x, y, z, w; };
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline Q conj(Q q) { return { -q.x, -q.y, -q.z, q.w }; }
inline Q mul(Q a, Q b) { return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                                   a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z }; }
inline V3 rot(Q q, V3 v) { V3 u{ q.x, q.y, q.z }; return u * (2 * dot(u, v)) + v * (q.w * q.w - dot(u, u)) + cross(u, v) * (2 * q.w); }
inline Q norm(Q q) { float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); return n > 0 ? Q{ q.x / n, q.y / n, q.z / n, q.w / n } : Q{ 0, 0, 0, 1 }; }
inline V3 mulMV(const float* R, V3 v) { return { R[0] * v.x + R[1] * v.y + R[2] * v.z, R[3] * v.x + R[4] * v.y + R[5] * v.z, R[6] * v.x + R[7] * v.y + R[8] * v.z }; }
inline V3 mulMTV(const float* R, V3 v) { return { R[0] * v.x + R[3] * v.y + R[6] * v.z, R[1] * v.x + R[4] * v.y + R[7] * v.z, R[2] * v.x + R[5] * v.y + R[8] * v.z }; }
inline void mulMM(const float* A, const float* B, float* C) { for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) C[r * 3 + c] = A[r * 3] * B[c] + A[r * 3 + 1] * B[3 + c] + A[r * 3 + 2] * B[6 + c]; }
inline void mulMTM(const float* A, const float* B, float* C) { for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) C[r * 3 + c] = A[r] * B[c] + A[3 + r] * B[3 + c] + A[6 + r] * B[6 + c]; }   // A^T * B
inline void normaliseCols(float* R) { for (int c = 0; c < 3; c++) { float n = std::sqrt(R[c] * R[c] + R[3 + c] * R[3 + c] + R[6 + c] * R[6 + c]); if (n > 1e-6f) { R[c] /= n; R[3 + c] /= n; R[6 + c] /= n; } } }
inline Q fromMat(const float* R) {
	float t = R[0] + R[4] + R[8]; Q q;
	if (t > 0) { float s = std::sqrt(t + 1) * 2; q = { (R[7] - R[5]) / s, (R[2] - R[6]) / s, (R[3] - R[1]) / s, s / 4 }; }
	else if (R[0] >= R[4] && R[0] >= R[8]) { float s = std::sqrt(1 + R[0] - R[4] - R[8]) * 2; q = { s / 4, (R[1] + R[3]) / s, (R[2] + R[6]) / s, (R[7] - R[5]) / s }; }
	else if (R[4] >= R[8]) { float s = std::sqrt(1 + R[4] - R[0] - R[8]) * 2; q = { (R[1] + R[3]) / s, s / 4, (R[5] + R[7]) / s, (R[2] - R[6]) / s }; }
	else { float s = std::sqrt(1 + R[8] - R[0] - R[4]) * 2; q = { (R[2] + R[6]) / s, (R[5] + R[7]) / s, s / 4, (R[3] - R[1]) / s }; }
	return norm(q);
}
inline void toMat(Q q, float* R) {
	float x = q.x, y = q.y, z = q.z, w = q.w;
	R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
	R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
	R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
}
