// Guest-visible main screen in UIKit points at scale 3. The Android host matches the height to its window's aspect
// ratio and reports the system-bar and display-cutout areas as safe-area insets; the command-line build keeps
// 360x780 points (1080x2340 pixels) and no insets.
#pragma once

#include <atomic>

namespace screen {

constexpr double kScale = 3.0;
constexpr double kWidth = 360;
inline double g_height = 780;                   // set before the loader thread starts
inline std::atomic<double> g_insets[4];         // top, left, bottom, right in points; updated from the UI thread

}  // namespace screen

extern "C" void host_set_screen_size(int width_px, int height_px) {
    if (width_px > 0 && height_px > 0) screen::g_height = screen::kWidth * height_px / width_px;
}

extern "C" void host_set_safe_area(int top_px, int left_px, int bottom_px, int right_px, int width_px) {
    if (width_px <= 0) return;
    const double pt = screen::kWidth / width_px;
    const int px[4] = {top_px, left_px, bottom_px, right_px};
    for (int i = 0; i < 4; ++i) screen::g_insets[i].store(px[i] * pt, std::memory_order_relaxed);
}
