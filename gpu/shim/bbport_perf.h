// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// SacredFire: frame timing histogram. Every presented frame's time goes into a
// histogram; every 5 seconds one line goes to the game's log:
//
//   Perf: frames <n> ms <sum> worst <ms> long <n> h <count per bucket, comma separated>
//
// Frames of 250 ms or more (loading screens) are counted as "long" and left out of the histogram.

namespace BbPerf {

// The histogram's upper bucket edges in milliseconds; the last bucket is 100-250 ms.
inline constexpr float Edges[] = {5.0f,  7.0f,  8.5f,  10.0f, 12.0f, 14.0f, 16.0f, 17.5f,
                                  20.0f, 25.0f, 33.4f, 50.0f, 100.0f, 250.0f};
inline constexpr int Buckets = sizeof(Edges) / sizeof(Edges[0]);

// One presented frame, `ms` after the previous one (the present thread).
void Frame(float ms);

} // namespace BbPerf
