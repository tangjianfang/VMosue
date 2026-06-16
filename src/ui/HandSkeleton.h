#pragma once
#include <array>
#include <utility>

namespace vmosue {

// MediaPipe Hands canonical 21-landmark skeleton topology.
//
// Landmark index reference (per the model):
//   0  = wrist
//   1..4  = thumb   (CMC, MCP, IP, TIP)
//   5..8  = index   (MCP, PIP, DIP, TIP)
//   9..12 = middle  (MCP, PIP, DIP, TIP)
//   13..16= ring    (MCP, PIP, DIP, TIP)
//   17..20= pinky   (MCP, PIP, DIP, TIP)
//
// 23 bones total: 5 fingers x 4 segments (= 20, includes the
// wrist-to-finger-base) plus 3 palm connectors (index -> middle ->
// ring -> pinky bases). The renderer iterates this table by
// std::size(); do not hard-code 23 elsewhere.
inline constexpr std::array<std::pair<int, int>, 23> kHandBones = {{
    // Thumb
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    // Index
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    // Middle
    {0, 9}, {9, 10}, {10, 11}, {11, 12},
    // Ring
    {0, 13}, {13, 14}, {14, 15}, {15, 16},
    // Pinky
    {0, 17}, {17, 18}, {18, 19}, {19, 20},
    // Palm
    {5, 9}, {9, 13}, {13, 17},
}};

}  // namespace vmosue