#pragma once

#include <cstdint>

enum class GestureAction {
  None,
  Next,
  Previous,
  OpenMenu,
  CloseMenu,
  Back,
  Sleep,
  Button
};

// One physical contact, with start/last coordinates supplied by the touch driver.
// Swipe actions are emitted only at release; hold() never consumes a moving
// contact. Firmware owns wake/menu transitions and may cancel a contact anytime.
class TouchGesture {
 public:
  void begin(int x, int y, uint32_t now, bool menu, int originButton, bool waking) {
    startX_ = lastX_ = x;
    startY_ = lastY_ = y;
    started_ = now;
    menu_ = menu;
    button_ = originButton;
    tapCancelled_ = false;
    active_ = !waking && insideCircle(x, y);
  }

  void move(int x, int y) {
    if (!active_) return;
    lastX_ = x;
    lastY_ = y;
    // Latch maximum excursion, not merely the final displacement. In
    // particular, moving away and returning must not become a button tap.
    const int64_t dx = static_cast<int64_t>(x) - startX_;
    const int64_t dy = static_cast<int64_t>(y) - startY_;
    if (dx < -30 || dx > 30 || dy < -30 || dy > 30 ||
        dx * dx + dy * dy > 30 * 30 || !insideCircle(x, y)) {
      tapCancelled_ = true;
    }
  }

  GestureAction hold(uint32_t now) {
    if (!active_ || tapCancelled_ || now - started_ < 900) {
      return GestureAction::None;
    }
    active_ = false;
    return GestureAction::Sleep;
  }

  GestureAction release(uint32_t now, int releasedButton) {
    if (!active_) return GestureAction::None;
    active_ = false;
    if (!insideCircle(lastX_, lastY_)) return GestureAction::None;

    const int dx = lastX_ - startX_;
    const int dy = lastY_ - startY_;
    const int ax = dx < 0 ? -dx : dx;
    const int ay = dy < 0 ? -dy : dy;
    // Inclusive 5:4 dominance leaves ambiguous diagonals unclassified.
    if (ax >= 80 && ax * 4 >= ay * 5) {
      if (!menu_) return dx < 0 ? GestureAction::Next : GestureAction::Previous;
      if (dx > 0 && startX_ <= 40 && startY_ >= 110 && startY_ <= 355 && button_ < 0) {
        return GestureAction::Back;
      }
    }
    if (ay >= 80 && ay * 4 >= ax * 5) {
      if (!menu_ && dy < 0) return GestureAction::OpenMenu;
      if (menu_ && dy > 0) return GestureAction::CloseMenu;
    }

    if (tapCancelled_) return GestureAction::None;
    const uint32_t duration = now - started_;
    if (duration >= 900) return GestureAction::Sleep;
    if (duration < 35) return GestureAction::None;
    if (!menu_) return GestureAction::Next;
    if (button_ >= 0 && releasedButton == button_) return GestureAction::Button;
    return GestureAction::None;
  }

  void cancel() {
    active_ = false;
    button_ = -1;
  }

  // Keep the capture available after release() so the consumer can dispatch it.
  int button() const { return button_; }

 private:
  static bool insideCircle(int x, int y) {
    // Center (232.5, 232.5), radius 233: doubled integer coordinates avoid
    // floating-point work and preserve the physical circle at pixel edges.
    if (x < 0 || x > 465 || y < 0 || y > 465) return false;
    const int dx = 2 * x - 465;
    const int dy = 2 * y - 465;
    return dx * dx + dy * dy <= 466 * 466;
  }

  int startX_ = 0, startY_ = 0;
  int lastX_ = 0, lastY_ = 0;
  uint32_t started_ = 0;
  int button_ = -1;
  bool menu_ = false;
  bool active_ = false;
  bool tapCancelled_ = false;
};
