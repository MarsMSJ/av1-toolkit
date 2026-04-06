#pragma once

// [MMSJ] §7.18.3.2 — Gaussian sequence / AOM: av1/decoder/grain_synthesis.c

namespace av1::fg {

inline constexpr int kGaussBits = 11;

extern const int kGaussianSequence[2048];

}  // namespace av1::fg