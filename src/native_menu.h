#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum class MenuPage { Main, Playback, Display, Timeout, Animation };
constexpr int kMenuTitleY = 30;
constexpr int kMenuBatteryX = 89, kMenuBatteryY = 55;
constexpr int kMenuBatteryWidth = 288, kMenuBatteryHeight = 24;
constexpr int kMenuBatteryTextY = 57, kMenuPictureY = 82;
inline const char *menuTitle(MenuPage page) {
  switch (page) {
    case MenuPage::Playback: return "PLAYBACK";
    case MenuPage::Display: return "DISPLAY";
    case MenuPage::Timeout: return "SCREEN TIMEOUT";
    case MenuPage::Animation: return "ANIMATION";
    default: return "BADGE MENU";
  }
}
struct MenuButton {
  int16_t x, y, width, height;
  const char *action;
  const char *label;
};
constexpr MenuButton kMenuButtons[] = {
    {93, 110, 280, 44, "playback", "PLAYBACK"},
    {93, 174, 280, 44, "display", "DISPLAY"},
    {93, 238, 280, 44, "timeout", "TIMEOUT"},
    {83, 310, 142, 46, "sleep", "SLEEP"},
    {241, 310, 142, 46, "back", "BACK"},
};
constexpr MenuButton kPlaybackButtons[] = {
    {93, 110, 280, 44, "slideshow", "AUTO"},
    {93, 174, 280, 44, "shuffle", "SHUFFLE"},
    {93, 264, 58, 46, "interval_down", "-"},
    {315, 264, 58, 46, "interval_up", "+"},
    {83, 328, 142, 46, "previous", "PREVIOUS"},
    {241, 328, 142, 46, "next", "NEXT"},
    {158, 392, 150, 38, "back", "BACK"},
};
constexpr MenuButton kDisplayButtons[] = {
    {93, 148, 58, 46, "brightness_down", "-"},
    {315, 148, 58, 46, "brightness_up", "+"},
    {93, 228, 280, 46, "animation", "ANIMATION"},
    {83, 320, 142, 46, "sleep", "SLEEP"},
    {241, 320, 142, 46, "back", "BACK"},
};
constexpr MenuButton kTimeoutButtons[] = {
    {93, 130, 58, 46, "usb_sleep_down", "-"},
    {315, 130, 58, 46, "usb_sleep_up", "+"},
    {93, 230, 58, 46, "battery_sleep_down", "-"},
    {315, 230, 58, 46, "battery_sleep_up", "+"},
    {158, 330, 150, 46, "back", "BACK"},
};
constexpr MenuButton kAnimationButtons[] = {
    {93, 105, 280, 46, "transition_next", "CUT"},
    {93, 173, 280, 46, "motion_next", "MOTION"},
    {93, 273, 58, 46, "rotation_faster", "-"},
    {315, 273, 58, 46, "rotation_slower", "+"},
    {158, 354, 150, 46, "back", "BACK"},
};
inline const MenuButton *pageButtons(MenuPage page, size_t &count) {
#define PAGE_BUTTONS(array) count = sizeof(array) / sizeof(array[0]); return array
  switch (page) {
    case MenuPage::Playback: PAGE_BUTTONS(kPlaybackButtons);
    case MenuPage::Display: PAGE_BUTTONS(kDisplayButtons);
    case MenuPage::Timeout: PAGE_BUTTONS(kTimeoutButtons);
    case MenuPage::Animation: PAGE_BUTTONS(kAnimationButtons);
    default: PAGE_BUTTONS(kMenuButtons);
  }
#undef PAGE_BUTTONS
}
inline const char *menuPageName(MenuPage page) {
  switch (page) {
    case MenuPage::Playback: return "playback";
    case MenuPage::Display: return "display";
    case MenuPage::Timeout: return "timeout";
    case MenuPage::Animation: return "animation";
    default: return "main";
  }
}
inline bool menuActionAllowed(MenuPage page, const char *action, bool rotating) {
  if (!strcmp(action, "main") || !strcmp(action, "sleep") || !strcmp(action, "back")) return true;
  if ((!strcmp(action, "rotation_faster") || !strcmp(action, "rotation_slower")) && !rotating) return false;
  size_t count;
  const MenuButton *buttons = pageButtons(page, count);
  for (size_t i = 0; i < count; ++i) if (!strcmp(action, buttons[i].action)) return true;
  return false;
}
inline const char *menuButtonLabel(const MenuButton &button, bool slideshow,
                                   bool shuffle, unsigned transition, unsigned motion) {
  if (!strcmp(button.action, "slideshow")) return slideshow ? "AUTO: ON" : "AUTO: OFF";
  if (!strcmp(button.action, "shuffle")) return shuffle ? "SHUFFLE: ON" : "SHUFFLE: OFF";
  if (!strcmp(button.action, "transition_next")) {
    constexpr const char *labels[] = {"CUT: DIRECT", "CUT: FADE", "CUT: SLIDE"};
    return labels[transition];
  }
  if (!strcmp(button.action, "motion_next")) {
    constexpr const char *labels[] = {"MOTION: OFF", "MOTION: SHIFT", "MOTION: ROTATE", "MOTION: GRAVITY"};
    return labels[motion];
  }
  return button.label;
}
