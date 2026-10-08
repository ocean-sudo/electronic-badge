#include "native_menu.h"
#include "battery_status.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {
constexpr MenuPage pages[] = {MenuPage::Main, MenuPage::Playback, MenuPage::Display,
                              MenuPage::Timeout, MenuPage::Animation};
double maximumRadius = 0;
void point(double x, double y) {
  const double radius = std::hypot(x - 232.5, y - 232.5);
  assert(radius <= 233);
  if (radius > maximumRadius) maximumRadius = radius;
}
void rectangle(int x, int y, int width, int height) {
  // Include the complete font-cell/rectangle envelope, not just lit pixels.
  point(x, y); point(x + width, y); point(x, y + height); point(x + width, y + height);
}
void text(const char *value, int center, int y, int size) {
  const int width = std::strlen(value) * 6 * size;
  rectangle(center - width / 2, y, width, 8 * size);
}
void layout() {
  for (MenuPage page : pages) {
    text(menuTitle(page), 233, kMenuTitleY, 2);
    size_t count;
    const MenuButton *buttons = pageButtons(page, count);
    for (size_t i = 0; i < count; ++i) {
      const auto &button = buttons[i];
      rectangle(button.x, button.y, button.width, button.height);
      const bool automatic = !std::strcmp(button.action, "slideshow");
      const bool shuffle = !std::strcmp(button.action, "shuffle");
      const bool cut = !std::strcmp(button.action, "transition_next");
      const bool motion = !std::strcmp(button.action, "motion_next");
      const unsigned states = motion ? 4 : cut ? 4 : automatic || shuffle ? 2 : 1;
      for (unsigned state = 0; state < states; ++state) {
        const char *label = menuButtonLabel(button, automatic && state, shuffle && state,
                                           cut ? state : 0, motion ? state : 0);
        if (!std::strcmp(button.action, "sleep")) assert(!std::strcmp(label, "SLEEP/OFF"));
        assert(std::strlen(label) * 12 + 16 <= static_cast<unsigned>(button.width));
        const int y = button.y + (button.height - 16) / 2;
        assert(y >= button.y + 8 && y + 16 <= button.y + button.height - 8);
        text(label, button.x + button.width / 2, y, 2);
      }
    }
  }
  rectangle(kMenuBatteryX, kMenuBatteryY, kMenuBatteryWidth, kMenuBatteryHeight);
  for (int presence : {-1, 0, 1}) for (int percent : {-1, 0, 9, 99, 100})
    for (int charging : {-1, 0, 1}) {
      BatteryStatus battery;
      battery.presence = presence; battery.percent = percent; battery.charging = charging;
      char value[32]; battery.text(value, sizeof(value));
      assert(std::strlen(value) <= 24);
      assert(233 - static_cast<int>(std::strlen(value)) * 6 >= kMenuBatteryX);
      assert(233 + static_cast<int>(std::strlen(value)) * 6 <= kMenuBatteryX + kMenuBatteryWidth);
      text(value, 233, kMenuBatteryTextY, 2);
    }
  std::cout << "native complete-cell/rectangle max radius " << maximumRadius << " < 233\n";
}
void matrix() {
  const char *all[] = {"main", "back", "sleep", "power_off", "playback", "display", "timeout", "slideshow",
      "shuffle", "interval_down", "interval_up", "previous", "next", "brightness_down",
      "brightness_up", "animation", "usb_sleep_down", "usb_sleep_up", "battery_sleep_down",
      "battery_sleep_up", "transition_next", "motion_next", "rotation_faster", "rotation_slower"};
  const char *legal[] = {
      " playback display timeout ",
      " slideshow shuffle interval_down interval_up previous next ",
      " brightness_down brightness_up animation ",
      " usb_sleep_down usb_sleep_up battery_sleep_down battery_sleep_up ",
      " transition_next motion_next rotation_faster rotation_slower "};
  for (size_t p = 0; p < sizeof(pages) / sizeof(pages[0]); ++p) {
    for (const char *action : all) {
      const bool global = !std::strcmp(action, "main") || !std::strcmp(action, "back") || !std::strcmp(action, "sleep");
      char needle[40]; std::snprintf(needle, sizeof(needle), " %s ", action);
      const bool expected = global || std::strstr(legal[p], needle);
      assert(menuActionAllowed(pages[p], action, true) == expected);
      const bool speed = !std::strcmp(action, "rotation_faster") || !std::strcmp(action, "rotation_slower");
      assert(menuActionAllowed(pages[p], action, false) == (expected && !speed));
    }
    assert(!menuActionAllowed(pages[p], "obsolete_action", true));
  }
}
void transitionLabels() {
  const char *expected[] = {"CUT: DIRECT", "CUT: FADE", "CUT: SLIDE", "CUT: RIPPLE"};
  for (unsigned transition = 0; transition < 4; ++transition) {
    assert(std::strcmp(menuButtonLabel(kAnimationButtons[0], false, false, transition, 0),
                       expected[transition]) == 0);
  }
}
void battery() {
  for (int unrelated = 0; unrelated < 256; ++unrelated) {
    const int state = (unrelated >> 5) & 3;
    const auto value = BatteryStatus::decode(0x28, 100, unrelated);
    assert(value.presence == 1 && value.percent == 100);
    assert(value.charging == (state == 3 ? -1 : state == 1));
  }
  assert(BatteryStatus::decode(-1, 50, -1).presence == -1);
  assert(BatteryStatus::decode(-1, 50, -1).percent == -1);
  assert(BatteryStatus::decode(-1, 50, -1).charging == -1);
  assert(BatteryStatus::decode(0x20, 100, 0).percent == -1);
  assert(BatteryStatus::decode(0x08, 101, 0x80).percent == -1);
  assert(BatteryStatus::decode(0x08, -1, 0).percent == -1);
  assert(BatteryStatus::decode(0x28, 100, 0x80).charging == 0);
  assert(BatteryStatus::decode(0x08, 0, 0xA0).charging == 1);
}
}
int main() { layout(); matrix(); transitionLabels(); battery(); std::cout << "native menu/battery tests passed\n"; }
