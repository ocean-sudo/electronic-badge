#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "pin_config.h"
#include "XPowersLib.h"
#include "TouchDrvCSTXXX.hpp"
#include "databus/Arduino_ESP32QSPI.h"
#include "display/Arduino_CO5300.h"
#include "canvas/Arduino_Canvas.h"
#include "auto_sleep.h"
#include "picture_animation.h"
#include "gravity_sensor.h"
#include <driver/gpio.h>

namespace {
constexpr size_t kFrameBytes = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
constexpr size_t kChunkBytes = 4096;
constexpr unsigned kSlots = 50;
constexpr uint32_t kChunkTimeout = 10000;
constexpr uint32_t kTransferTimeout = 90000;
constexpr uint32_t kFileMagic = 0x31474442; // BDG1; payload is little-endian RGB565.
constexpr char kTemporary[] = "/upload.tmp";
static_assert(kFrameBytes == 434312, "Host and panel geometry must agree");
constexpr uint8_t kBootPin = 0; // ESP32-S3 BOOT is GPIO0.
constexpr uint16_t kIntervals[] = {2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
constexpr uint32_t kSleepIntervals[] = {0, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600, 7200, 14400, 28800, 43200, 86400};
constexpr uint16_t kRotationPeriods[] = {8, 12, 18, 24, 36, 60, 90, 120};
constexpr uint8_t kTearingPin = 13; // 1.75C schematic: GPIO13 -> LCD_TE.
constexpr const char *kTransitions[] = {"direct", "fade", "slide"};
constexpr const char *kMotions[] = {"off", "shift", "rotate", "gravity"};

struct ImageHeader {
  uint32_t magic;
  uint32_t length;
  uint32_t crc;
};
static_assert(sizeof(ImageHeader) == 12, "Stable on-flash header");

BadgeQSPI bus(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 gfx(&bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
PanelScan panelScan;
PictureAnimation animation(gfx, bus, panelScan);
bool animationOk = false;
TouchDrvCST92xx touch;
XPowersPMU power;
GravitySensor gravity;
Preferences preferences;

// Bind the existing PSRAM frame without allocating or initializing the panel.
class FrameCanvas : public Arduino_Canvas {
 public:
  explicit FrameCanvas(uint8_t *buffer) : Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, nullptr) {
    _framebuffer = reinterpret_cast<uint16_t *>(buffer);
  }
  ~FrameCanvas() { _framebuffer = nullptr; }
};
uint8_t *frame = nullptr;
uint8_t ioBuffer[kChunkBytes];
uint32_t crcTable[256];
bool occupied[kSlots] = {};
bool displayOk = false;
bool touchOk = false;
bool storageOk = false;
bool preferencesOk = false;
bool pmuOk = false;
bool sleeping = false;
uint8_t brightness = 80;
int current = -1;
uint32_t currentCrc = 0;
bool menuOpen = false;
enum class MenuPage { Main, Timeout, Animation };
MenuPage menuPage = MenuPage::Main;
AutoSleep autoSleep;
bool slideshowEnabled = false;
uint16_t slideshowInterval = 10;
uint32_t slideshowStarted = 0;
const char *slideshowError = "";
volatile bool touchInterrupt = false;
bool fingerDown = false;
bool gestureHandled = false;
uint32_t fingerStarted = 0;
uint32_t fingerLastEvent = 0;
int16_t fingerInitialX = 0, fingerInitialY = 0;
int16_t fingerLastX = 0, fingerLastY = 0;
bool suppressTouch = false;
uint32_t touchSuppressedAt = 0;
char commandLine[96];
size_t commandLength = 0;
bool commandInvalid = false;
uint32_t commandLastByte = 0;

void IRAM_ATTR onTouch() { touchInterrupt = true; }

void IRAM_ATTR onTearing() {
  const uint32_t now = micros();
  if (gpio_get_level(static_cast<gpio_num_t>(kTearingPin))) {
    const uint32_t period = now - panelScan.risingUs;
    const uint32_t previous = panelScan.periodUs;
    // NVS/LittleFS can mask interrupts. Missed pulses are not a slower panel:
    // never expand the scan window to two actual frames after a Flash write.
    if (panelScan.risingUs && period >= 5000U && period <= 50000U &&
        (!previous || (period * 8U >= previous * 7U && period * 8U <= previous * 9U))) {
      panelScan.periodUs = period;
    }
    panelScan.risingUs = now;
  } else {
    panelScan.fallingUs = now;
    panelScan.scans = panelScan.scans + 1U;
  }
}

void enablePanelSync() {
  bus.beginWrite();
  bus.writeC8D16(0x44, 0); // CO5300 Set Tear Scanline: vertical porch at line zero.
  bus.writeC8D8(CO5300_WC_TEARON, 0);
  bus.endWrite();
}

void resetSlideshowTimer() { slideshowStarted = millis(); }

void clearGesture() {
  if (fingerDown) {
    suppressTouch = true;
    touchSuppressedAt = millis();
  }
  fingerDown = false;
  gestureHandled = false;
  noInterrupts();
  touchInterrupt = false;
  interrupts();
}

void initCrc() {
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (unsigned bit = 0; bit < 8; ++bit) {
      c = (c >> 1) ^ ((c & 1) ? 0xEDB88320U : 0);
    }
    crcTable[n] = c;
  }
}

uint32_t updateCrc(uint32_t crc, const uint8_t *data, size_t length) {
  while (length--) crc = crcTable[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
  return crc;
}

void slotPath(unsigned slot, char *path, size_t length) {
  snprintf(path, length, "/slot%02u.rgb", slot);
}

bool readHeader(File &file, ImageHeader &header) {
  return file && file.size() == sizeof(header) + kFrameBytes &&
         file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
         header.magic == kFileMagic && header.length == kFrameBytes;
}

const char *loadPicture(unsigned slot, uint32_t &crc, uint8_t *destination = frame) {
  if (!storageOk) return "storage_unavailable";
  if (!destination) return "psram_unavailable";
  if (!occupied[slot]) return "empty_slot";
  char path[24];
  slotPath(slot, path, sizeof(path));
  File file = LittleFS.open(path, FILE_READ);
  ImageHeader header;
  if (!readHeader(file, header)) return "invalid_image";
  uint32_t sum = 0xFFFFFFFFU;
  for (size_t offset = 0; offset < kFrameBytes;) {
    const size_t count = min(kChunkBytes, kFrameBytes - offset);
    if (file.read(destination + offset, count) != count) return "storage_read";
    sum = updateCrc(sum, destination + offset, count);
    offset += count;
    delay(0);
  }
  crc = sum ^ 0xFFFFFFFFU;
  return crc == header.crc ? nullptr : "stored_crc_mismatch";
}

void centeredText(const char *text, int y, uint8_t size, uint16_t color) {
  gfx.setTextSize(size);
  gfx.setTextColor(color);
  gfx.setCursor((LCD_WIDTH - static_cast<int>(strlen(text)) * 6 * size) / 2, y);
  gfx.print(text);
}

void drawWelcome() {
  if (!displayOk || sleeping) return;
  gfx.fillScreen(0x0843);
  gfx.drawCircle(233, 233, 220, 0x2DDB);
  gfx.drawCircle(233, 233, 214, 0x1230);
  gfx.fillRoundRect(153, 84, 160, 38, 19, 0x2DDB);
  centeredText("YOUR BADGE", 96, 2, 0x0843);
  centeredText("MAKE IT", 150, 5, 0xFFFF);
  centeredText("PERSONAL", 201, 5, 0xFFFF);
  centeredText("Upload a picture over USB", 275, 2, 0xBDF7);
  centeredText("Tap: next  |  Swipe up: menu", 308, 2, 0xBDF7);
  centeredText("Hold: sleep / tap: wake", 336, 2, 0xBDF7);
  if (!storageOk || !frame || !preferencesOk) {
    centeredText("Check device status", 372, 2, 0xFC80);
  } else {
    char capacity[40];
    snprintf(capacity, sizeof(capacity), "466 x 466  |  %u pictures", kSlots);
    centeredText(capacity, 372, 2, 0x2DDB);
  }
}

struct MenuButton {
  int16_t x, y, width, height;
  const char *action;
  const char *label;
};

// All hit rectangles, including their corners, are inside the round panel.
constexpr MenuButton kMenuButtons[] = {
    {93, 96, 280, 46, "slideshow", "AUTO"},
    {93, 158, 58, 46, "interval_down", "-"},
    {315, 158, 58, 46, "interval_up", "+"},
    {93, 226, 58, 46, "brightness_down", "-"},
    {315, 226, 58, 46, "brightness_up", "+"},
    {83, 292, 142, 46, "next", "NEXT"},
    {241, 292, 142, 46, "sleep", "SLEEP"},
    {83, 354, 142, 46, "timeout", "TIMEOUT"},
    {241, 354, 142, 46, "back", "BACK"},
    {158, 408, 150, 38, "animation", "ANIMATION"},
};
constexpr MenuButton kTimeoutButtons[] = {
    {93, 130, 58, 46, "usb_sleep_down", "-"},
    {315, 130, 58, 46, "usb_sleep_up", "+"},
    {93, 230, 58, 46, "battery_sleep_down", "-"},
    {315, 230, 58, 46, "battery_sleep_up", "+"},
    {158, 330, 150, 46, "main", "BACK"},
};
constexpr MenuButton kAnimationButtons[] = {
    {93, 105, 280, 46, "transition_next", "CUT"},
    {93, 173, 280, 46, "motion_next", "MOTION"},
    {93, 273, 58, 46, "rotation_faster", "-"},
    {315, 273, 58, 46, "rotation_slower", "+"},
    {158, 354, 150, 46, "main", "BACK"},
};

const MenuButton *menuButtons(size_t &count) {
  if (menuPage == MenuPage::Timeout) {
    count = sizeof(kTimeoutButtons) / sizeof(kTimeoutButtons[0]);
    return kTimeoutButtons;
  }
  if (menuPage == MenuPage::Animation) {
    count = sizeof(kAnimationButtons) / sizeof(kAnimationButtons[0]);
    return kAnimationButtons;
  }
  count = sizeof(kMenuButtons) / sizeof(kMenuButtons[0]);
  return kMenuButtons;
}

void timeoutText(char *buffer, size_t size, uint32_t seconds) {
  if (!seconds) snprintf(buffer, size, "OFF");
  else if (seconds % 3600 == 0) snprintf(buffer, size, "%lu hr", static_cast<unsigned long>(seconds / 3600));
  else if (seconds % 60 == 0) snprintf(buffer, size, "%lu min", static_cast<unsigned long>(seconds / 60));
  else snprintf(buffer, size, "%lu sec", static_cast<unsigned long>(seconds));
}

void menuText(FrameCanvas &canvas, const char *text, int centerX, int y,
              uint8_t size, uint16_t color) {
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.setCursor(centerX - static_cast<int>(strlen(text)) * 3 * size, y);
  canvas.print(text);
}

void drawMenu() {
  if (!displayOk || !frame || sleeping || !menuOpen) return;
  FrameCanvas canvas(frame);
  canvas.fillScreen(0x0843);
  canvas.drawCircle(233, 233, 220, 0x2DDB);
  const char *title = menuPage == MenuPage::Timeout ? "SCREEN TIMEOUT"
                    : menuPage == MenuPage::Animation ? "ANIMATION" : "BADGE MENU";
  menuText(canvas, title, 233, 53, menuPage == MenuPage::Timeout ? 2 : 3, 0xFFFF);
  size_t buttonCount = 0;
  const MenuButton *buttons = menuButtons(buttonCount);
  for (size_t i = 0; i < buttonCount; ++i) {
    const auto &button = buttons[i];
    canvas.fillRoundRect(button.x, button.y, button.width, button.height, 12, 0x1230);
    const char *label = button.label;
    if (strcmp(button.action, "slideshow") == 0) {
      label = slideshowEnabled ? "AUTO: ON" : "AUTO: OFF";
    }
    if (strcmp(button.action, "transition_next") == 0) {
      static constexpr const char *labels[] = {"CUT: DIRECT", "CUT: FADE", "CUT: SLIDE"};
      label = labels[animation.transition];
    } else if (strcmp(button.action, "motion_next") == 0) {
      static constexpr const char *labels[] = {"MOTION: OFF", "MOTION: SHIFT", "MOTION: ROTATE", "MOTION: GRAVITY"};
      label = labels[animation.motion];
    }
    menuText(canvas, label, button.x + button.width / 2, button.y + 15, 2, 0xFFFF);
  }
  char value[32];
  if (menuPage == MenuPage::Timeout) {
    menuText(canvas, "USB POWER", 233, 105, 2, 0xBDF7);
    timeoutText(value, sizeof(value), autoSleep.usbSeconds);
    menuText(canvas, value, 233, 147, 2, 0xFFFF);
    menuText(canvas, "BATTERY", 233, 205, 2, 0xBDF7);
    timeoutText(value, sizeof(value), autoSleep.batterySeconds);
    menuText(canvas, value, 233, 247, 2, 0xFFFF);
    menuText(canvas, "OFF = ALWAYS ON", 233, 295, 1, 0xBDF7);
    const char *source = autoSleep.supply == AutoSleep::Supply::Usb ? "Power: USB"
                       : autoSleep.supply == AutoSleep::Supply::Battery ? "Power: Battery" : "Power: Unknown (USB policy)";
    menuText(canvas, source, 233, 395, 1, 0xBDF7);
  } else if (menuPage == MenuPage::Animation) {
    if (!gravity.ok) menuText(canvas, "GRAVITY SENSOR UNAVAILABLE", 233, 225, 1, 0xFC80);
    else if (animation.motion == 3) menuText(canvas, "FLAT: LAST POSE / FIRST: NATIVE", 233, 225, 1, 0xBDF7);
    menuText(canvas, "ROTATE SPEED (SECONDS / TURN)", 233, 247, 1, 0xBDF7);
    snprintf(value, sizeof(value), "%u sec", animation.rotationPeriod);
    menuText(canvas, value, 233, 290, 2, 0xFFFF);
    menuText(canvas, animation.motion == 3 ? "GRAVITY USES SENSOR, NOT THIS SPEED"
                                         : "- FASTER / + SLOWER", 233, 330, 1, 0xBDF7);
    menuText(canvas, "Motion never renews idle timeout", 233, 414, 1, 0xBDF7);
  } else {
    snprintf(value, sizeof(value), "%u sec", slideshowInterval);
    menuText(canvas, value, 233, 175, 2, 0xFFFF);
    menuText(canvas, "INTERVAL", 233, 147, 1, 0xBDF7);
    snprintf(value, sizeof(value), "%u", brightness);
    menuText(canvas, value, 233, 243, 2, 0xFFFF);
    menuText(canvas, "BRIGHTNESS", 233, 215, 1, 0xBDF7);
    if (current >= 0) snprintf(value, sizeof(value), "Picture %d", current + 1);
    else snprintf(value, sizeof(value), "No picture");
    menuText(canvas, value, 233, 82, 1, 0xBDF7);
  }
  gfx.draw16bitRGBBitmap(0, 0, reinterpret_cast<uint16_t *>(frame), LCD_WIDTH, LCD_HEIGHT);
}

bool rememberCurrent(int value) {
  if (!preferencesOk) return false;
  if (preferences.getInt("current", -1) == value) return true;
  if (preferences.putInt("current", value) == sizeof(int32_t)) return true;
  preferencesOk = false;
  return false;
}

void refreshGravity(bool prime = false) {
  gravity.enable(animation.motion == 3 && current >= 0 && !sleeping && !menuOpen, prime);
  gravity.poll();
  animation.gravityAngle(gravity.pose.angle);
}

void wakePanel() {
  if (sleeping) {
    if (displayOk) { gfx.displayOn(); enablePanelSync(); }
    animation.pauseClock();
    autoSleep.activity(millis());
  }
  sleeping = false;
  refreshGravity(true);
  if (displayOk) gfx.setBrightness(brightness);
  resetSlideshowTimer();
}

const char *showPicture(unsigned slot, bool remember = true) {
  if (!displayOk) return "display_unavailable";
  if (remember && !preferencesOk) return "preferences_unavailable";
  if (!animationOk) return "psram_unavailable";
  const bool animate = !sleeping && !menuOpen;
  const bool interrupted = animation.transitioning();
  animation.cancel(brightness);
  uint32_t crc;
  const char *error = loadPicture(slot, crc, reinterpret_cast<uint8_t *>(animation.nextSource()));
  if (error) {
    if (interrupted && animate && current >= 0) animation.presentCurrent();
    return error;
  }
  if (remember && !rememberCurrent(slot)) {
    if (menuOpen) drawMenu();
    return "preferences_write";
  }
  current = slot;
  currentCrc = crc;
  wakePanel();
  if (remember) autoSleep.activity(millis());
  const char *presentError = animation.selectImage(animate, brightness, !menuOpen);
  if (menuOpen) drawMenu();
  if (presentError) return presentError;
  return nullptr;
}

const char *nextPicture(bool remember = true) {
  for (unsigned step = 1; step <= kSlots; ++step) {
    const unsigned slot = (current + static_cast<int>(step)) % kSlots;
    if (occupied[slot]) return showPicture(slot, remember);
  }
  return "no_pictures";
}

const char *restorePicture() {
  if (current >= 0) return animation.presentCurrent();
  animation.invalidate(brightness);
  drawWelcome();
  return nullptr;
}

const char *setSleeping(bool value) {
  if (!displayOk) return "display_unavailable";
  autoSleep.activity(millis());
  if (sleeping == value) return nullptr;
  clearGesture();
  resetSlideshowTimer();
  if (value) {
    animation.cancel(brightness);
    menuOpen = false;
    gfx.setBrightness(0);
    gfx.displayOff();
    sleeping = true;
    refreshGravity();
    return nullptr;
  }
  wakePanel();
  return restorePicture();
}

const char *setMenu(bool open) {
  if (!displayOk) return "display_unavailable";
  if (open && !frame) return "psram_unavailable";
  clearGesture();
  autoSleep.activity(millis());
  resetSlideshowTimer();
  if (open) {
    wakePanel();
    animation.cancel(brightness);
    menuOpen = true;
    menuPage = MenuPage::Main;
    refreshGravity();
    drawMenu();
    return nullptr;
  }
  if (!menuOpen) return nullptr;
  // Do not claim to have closed the menu if the selected image cannot be read.
  menuOpen = false;
  refreshGravity(true);
  const char *error = restorePicture();
  if (error) {
    menuOpen = true;
    refreshGravity();
    drawMenu();
    return error;
  }
  menuOpen = false;
  return nullptr;
}

const char *setSlideshow(bool enabled, uint16_t interval) {
  if (!preferencesOk) return "preferences_unavailable";
  const uint32_t packed = (static_cast<uint32_t>(interval) << 1) | (enabled ? 1U : 0U);
  if (preferences.getUInt("slideshow", 10U << 1) != packed &&
      preferences.putUInt("slideshow", packed) != sizeof(uint32_t)) {
    preferencesOk = false;
    slideshowError = "preferences_write";
    return slideshowError;
  }
  slideshowEnabled = enabled;
  slideshowInterval = interval;
  slideshowError = "";
  autoSleep.activity(millis());
  resetSlideshowTimer();
  if (menuOpen) drawMenu();
  return nullptr;
}

const char *setAutoSleep(uint32_t usbSeconds, uint32_t batterySeconds) {
  if (!AutoSleep::validTimeout(usbSeconds) || !AutoSleep::validTimeout(batterySeconds)) return "invalid_autosleep_arguments";
  if (!preferencesOk) return "preferences_unavailable";
  const uint64_t packed = static_cast<uint64_t>(usbSeconds) | (static_cast<uint64_t>(batterySeconds) << 17);
  if (preferences.getULong64("auto_sleep", 15) != packed &&
      preferences.putULong64("auto_sleep", packed) != sizeof(uint64_t)) {
    preferencesOk = false;
    return "preferences_write";
  }
  autoSleep.usbSeconds = usbSeconds;
  autoSleep.batterySeconds = batterySeconds;
  autoSleep.activity(millis());
  if (menuOpen) drawMenu();
  return nullptr;
}

void pollAutoSleep() {
  const uint32_t now = millis();
  static uint32_t lastSupplyPoll = 0;
  static bool supplyPolled = false;
  if (!supplyPolled || now - lastSupplyPoll >= 250) {
    supplyPolled = true;
    lastSupplyPoll = now;
    // STATUS1 bit 5 is VBUS-good, independent of charging or USB host presence.
    const int status = pmuOk ? power.readRegister(XPOWERS_AXP2101_STATUS1) : -1;
    const auto source = status < 0 ? AutoSleep::Supply::Unknown
                      : (status & 0x20) ? AutoSleep::Supply::Usb : AutoSleep::Supply::Battery;
    if (autoSleep.updateSupply(source, now) && menuOpen) drawMenu();
  }
  if (!sleeping && !fingerDown && autoSleep.due(now)) setSleeping(true);
}

const char *setBrightness(uint8_t value) {
  if (!displayOk) return "display_unavailable";
  if (!preferencesOk) return "preferences_unavailable";
  if (brightness != value && preferences.putUChar("brightness", value) != 1) {
    preferencesOk = false;
    return "preferences_write";
  }
  brightness = value;
  const bool interrupted = animation.transitioning();
  animation.cancel(brightness);
  autoSleep.activity(millis());
  if (!sleeping) gfx.setBrightness(brightness);
  if (interrupted && !sleeping && !menuOpen && current >= 0) return animation.presentCurrent();
  if (menuOpen) drawMenu();
  return nullptr;
}

const char *setAnimation(uint8_t transition, uint8_t motion, uint16_t period) {
  if (!displayOk) return "display_unavailable";
  if (!animationOk) return "psram_unavailable";
  if (!preferencesOk) return "preferences_unavailable";
  if (motion == 3) {
    if (const char *error = gravity.available()) return error;
    // Preflight before writing preferences, including while asleep/in menus.
    gravity.enable(true, true);
    if (const char *error = gravity.available()) return error;
  }
  const uint32_t packed = transition | (motion << 2) | (period << 4);
  constexpr uint32_t defaults = 1U | (2U << 2) | (24U << 4);
  if (preferences.getUInt("animation", defaults) != packed &&
      preferences.putUInt("animation", packed) != sizeof(uint32_t)) {
    preferencesOk = false;
    refreshGravity();
    return "preferences_write";
  }
  animation.configure(transition, motion, period, brightness);
  refreshGravity(true);
  autoSleep.activity(millis());
  if (menuOpen) drawMenu();
  else if (!sleeping && current >= 0) return animation.presentCurrent();
  return nullptr;
}

const char *menuAction(const char *action) {
  if (!menuOpen || sleeping) return "menu_closed";
  if (strcmp(action, "timeout") == 0 || strcmp(action, "animation") == 0 || strcmp(action, "main") == 0) {
    menuPage = strcmp(action, "timeout") == 0 ? MenuPage::Timeout
             : strcmp(action, "animation") == 0 ? MenuPage::Animation : MenuPage::Main;
    autoSleep.activity(millis());
    drawMenu();
    return nullptr;
  }
  if (strcmp(action, "transition_next") == 0) return setAnimation((animation.transition + 1) % 3, animation.motion, animation.rotationPeriod);
  if (strcmp(action, "motion_next") == 0) {
    uint8_t next = (animation.motion + 1) % 4;
    if (next == 3 && !gravity.ok) next = 0;
    return setAnimation(animation.transition, next, animation.rotationPeriod);
  }
  if (strcmp(action, "rotation_faster") == 0 || strcmp(action, "rotation_slower") == 0) {
    const bool slower = strcmp(action, "rotation_slower") == 0;
    uint16_t period = slower ? 120 : 8;
    for (uint16_t preset : kRotationPeriods) {
      if (slower && preset > animation.rotationPeriod) { period = preset; break; }
      if (!slower && preset < animation.rotationPeriod) period = preset;
    }
    return setAnimation(animation.transition, animation.motion, period);
  }
  if (strcmp(action, "usb_sleep_down") == 0 || strcmp(action, "usb_sleep_up") == 0 ||
      strcmp(action, "battery_sleep_down") == 0 || strcmp(action, "battery_sleep_up") == 0) {
    const bool usb = strncmp(action, "usb_", 4) == 0;
    const bool up = strstr(action, "_up") != nullptr;
    const uint32_t currentTimeout = usb ? autoSleep.usbSeconds : autoSleep.batterySeconds;
    uint32_t timeout = up ? 86400 : 0;
    for (uint32_t preset : kSleepIntervals) {
      if (up && preset > currentTimeout) {
        timeout = preset;
        break;
      }
      if (!up && preset < currentTimeout) timeout = preset;
    }
    return setAutoSleep(usb ? timeout : autoSleep.usbSeconds, usb ? autoSleep.batterySeconds : timeout);
  }
  if (strcmp(action, "slideshow") == 0) return setSlideshow(!slideshowEnabled, slideshowInterval);
  if (strcmp(action, "interval_down") == 0 || strcmp(action, "interval_up") == 0) {
    const bool up = strcmp(action, "interval_up") == 0;
    uint16_t interval = up ? kIntervals[sizeof(kIntervals) / sizeof(kIntervals[0]) - 1] : kIntervals[0];
    for (uint16_t preset : kIntervals) {
      if (up && preset > slideshowInterval) {
        interval = preset;
        break;
      }
      if (!up && preset < slideshowInterval) interval = preset;
    }
    return setSlideshow(slideshowEnabled, interval);
  }
  if (strcmp(action, "brightness_down") == 0) return setBrightness(brightness > 10 ? brightness - 10 : 1);
  if (strcmp(action, "brightness_up") == 0) return setBrightness(brightness < 170 ? brightness + 10 : 180);
  if (strcmp(action, "next") == 0) return nextPicture();
  if (strcmp(action, "sleep") == 0) return setSleeping(true);
  if (strcmp(action, "back") == 0) return setMenu(false);
  return "invalid_menu_action";
}

void pollSlideshow() {
  if (!slideshowEnabled || sleeping || menuOpen) return;
  if (millis() - slideshowStarted < static_cast<uint32_t>(slideshowInterval) * 1000U) return;
  resetSlideshowTimer();
  unsigned available = 0;
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    if (occupied[slot]) ++available;
  }
  if (available < 2) return;
  const char *error = nextPicture(false);
  slideshowError = error ? error : "";
}

void reply(const char *error = nullptr) {
  if (error) {
    Serial.print("ERR ");
    Serial.print(error);
    Serial.print('\n');
  } else {
    Serial.print("OK\n");
  }
}

// Discard the remainder of an abandoned binary transfer, not as ASCII commands.
void drainAbandonedUpload() {
  const uint32_t started = millis();
  uint32_t lastByte = started;
  while (millis() - started < 1500 && millis() - lastByte < 150) {
    while (Serial.available()) {
      Serial.read();
      lastByte = millis();
    }
    delay(1);
  }
}

const char *receivePicture(unsigned slot, uint32_t expectedCrc) {
  if (!storageOk) return "storage_unavailable";
  if (!frame || !animationOk) return "psram_unavailable";
  if (!displayOk) return "display_unavailable";
  if (!preferencesOk) return "preferences_unavailable";
  Serial.print("READY\n");
  const uint32_t started = millis();
  uint32_t sum = 0xFFFFFFFFU;
  for (size_t offset = 0; offset < kFrameBytes;) {
    const size_t count = min(kChunkBytes, kFrameBytes - offset);
    const uint32_t chunkStarted = millis();
    size_t received = 0;
    while (received < count) {
      if (millis() - chunkStarted >= kChunkTimeout || millis() - started >= kTransferTimeout) {
        drainAbandonedUpload();
        return "upload_timeout";
      }
      const int available = Serial.available();
      if (available > 0) {
        const size_t wanted = min(count - received, static_cast<size_t>(available));
        received += Serial.read(frame + offset + received, wanted);
      } else {
        delay(1);
      }
    }
    sum = updateCrc(sum, frame + offset, count);
    offset += count;
    Serial.print("ACK\n");
  }
  sum ^= 0xFFFFFFFFU;
  if (sum != expectedCrc) return "upload_crc_mismatch";

  // Never remove the previous slot. LittleFS rename replaces it atomically only
  // after the complete temporary file has been closed and read back successfully.
  File file = LittleFS.open(kTemporary, FILE_WRITE);
  if (!file) return "storage_open";
  const ImageHeader header = {kFileMagic, kFrameBytes, sum};
  bool written = file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) == sizeof(header);
  for (size_t offset = 0; written && offset < kFrameBytes;) {
    const size_t count = min(kChunkBytes, kFrameBytes - offset);
    written = file.write(frame + offset, count) == count;
    offset += count;
    delay(0);
  }
  file.flush();
  file.close();
  bool verified = false;
  if (written) {
    file = LittleFS.open(kTemporary, FILE_READ);
    ImageHeader check;
    verified = readHeader(file, check) && check.crc == sum;
    uint32_t storedCrc = 0xFFFFFFFFU;
    for (size_t offset = 0; verified && offset < kFrameBytes;) {
      const size_t count = min(kChunkBytes, kFrameBytes - offset);
      verified = file.read(ioBuffer, count) == count;
      if (verified) storedCrc = updateCrc(storedCrc, ioBuffer, count);
      offset += count;
      delay(0);
    }
    verified = verified && (storedCrc ^ 0xFFFFFFFFU) == sum;
    file.close();
  }
  char path[24];
  slotPath(slot, path, sizeof(path));
  if (!verified || !LittleFS.rename(kTemporary, path)) {
    LittleFS.remove(kTemporary);
    return verified ? "storage_commit" : "storage_verify";
  }
  occupied[slot] = true;
  const bool remembered = rememberCurrent(slot);
  current = slot;
  currentCrc = sum;
  // A successful upload intentionally exits the menu to show the new picture.
  menuOpen = false;
  clearGesture();
  wakePanel();
  memcpy(animation.nextSource(), frame, kFrameBytes);
  const char *presentError = animation.selectImage(false, brightness);
  if (presentError) return presentError;
  return remembered ? nullptr : "preferences_write";
}

void sendFrame(uint32_t crc, const uint8_t *pixels = frame) {
  Serial.printf("DATA %u %lu\n", static_cast<unsigned>(kFrameBytes), static_cast<unsigned long>(crc));
  const uint32_t started = millis();
  for (size_t offset = 0; offset < kFrameBytes;) {
    const size_t count = min(kChunkBytes, kFrameBytes - offset);
    const size_t sent = Serial.write(pixels + offset, count);
    offset += sent;
    if (millis() - started >= kTransferTimeout || (!sent && !Serial.isConnected())) {
      // A truncated DATA stream cannot carry a parseable error line. The host
      // times out and reconnects; no bytes are fabricated to fill the response.
      return;
    }
    delay(0);
  }
  Serial.print("\nOK\n");
}

void sendPicture(unsigned slot) {
  uint32_t crc;
  const char *error = loadPicture(slot, crc);
  if (error) reply(error);
  else sendFrame(crc);
  // GET uses the shared frame; restore the actual menu after every outcome.
  if (menuOpen) drawMenu();
  resetSlideshowTimer();
}

void sendMenuFrame() {
  if (!menuOpen || sleeping) return reply("menu_closed");
  if (!displayOk) return reply("display_unavailable");
  if (!frame) return reply("psram_unavailable");
  drawMenu();
  sendFrame(updateCrc(0xFFFFFFFFU, frame, kFrameBytes) ^ 0xFFFFFFFFU);
}

void sendDisplayFrame() {
  if (sleeping || menuOpen || current < 0 || !animation.snapshot()) return reply("display_frame_unavailable");
  const auto *pixels = reinterpret_cast<const uint8_t *>(animation.snapshot());
  sendFrame(updateCrc(0xFFFFFFFFU, pixels, kFrameBytes) ^ 0xFFFFFFFFU, pixels);
  animation.pauseClock();
}

void status() {
  const bool batteryConnected = pmuOk && power.isBatteryConnect();
  int batteryPercent = batteryConnected ? power.getBatteryPercent() : -1;
  if (batteryPercent < 0 || batteryPercent > 100) batteryPercent = -1;
  Serial.print("{\"firmware\":\"picture-badge-1\",\"width\":466,\"height\":466,\"slots\":[");
  bool first = true;
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    if (!occupied[slot]) continue;
    if (!first) Serial.print(',');
    Serial.print(slot);
    first = false;
  }
  Serial.printf("],\"current\":%d,\"brightness\":%u,\"sleeping\":%s,\"touch_ok\":%s,\"display_ok\":%s,\"storage_ok\":%s,\"current_crc32\":%lu,\"battery_connected\":%s,\"battery_percent\":%d,\"pmu_ok\":%s,\"psram_ok\":%s,\"preferences_ok\":%s,\"slideshow_enabled\":%s,\"slideshow_interval\":%u,\"slideshow_error\":\"%s\",\"menu_open\":%s,\"menu_page\":\"%s\",\"power_source\":\"%s\",\"autosleep_usb_seconds\":%lu,\"autosleep_battery_seconds\":%lu,\"autosleep_active_seconds\":%lu,\"autosleep_remaining_seconds\":%lu,",
                current, brightness, sleeping ? "true" : "false", touchOk ? "true" : "false",
                displayOk ? "true" : "false", storageOk ? "true" : "false",
                static_cast<unsigned long>(currentCrc), batteryConnected ? "true" : "false", batteryPercent,
                pmuOk ? "true" : "false", frame && animationOk ? "true" : "false", preferencesOk ? "true" : "false",
                slideshowEnabled ? "true" : "false", slideshowInterval, slideshowError,
                menuOpen ? "true" : "false", menuPage == MenuPage::Timeout ? "timeout" : menuPage == MenuPage::Animation ? "animation" : "main",
                autoSleep.supply == AutoSleep::Supply::Usb ? "usb" : autoSleep.supply == AutoSleep::Supply::Battery ? "battery" : "unknown",
                static_cast<unsigned long>(autoSleep.usbSeconds), static_cast<unsigned long>(autoSleep.batterySeconds),
                static_cast<unsigned long>(autoSleep.timeoutSeconds()),
                static_cast<unsigned long>(sleeping ? 0 : (autoSleep.remainingMs(millis()) + 999U) / 1000U));
  Serial.printf("\"display_clock_khz\":%d,\"display_pack_us\":%lu,", bus.clockKhz, static_cast<unsigned long>(bus.packUs));
  Serial.printf("\"imu_ok\":%s,\"gravity_valid\":%s,\"gravity_error\":\"%s\",",
                gravity.ok ? "true" : "false", gravity.pose.valid ? "true" : "false", gravity.pose.error);
  if (gravity.pose.hasSample) {
    Serial.printf("\"gravity_ax\":%.4f,\"gravity_ay\":%.4f,\"gravity_az\":%.4f,",
                  gravity.pose.ax, gravity.pose.ay, gravity.pose.az);
  }
  Serial.printf("\"transition_mode\":\"%s\",\"motion_mode\":\"%s\",\"rotation_period\":%u,\"animation_active\":%s,\"animation_angle\":%u,\"animation_frame_ms\":%lu,\"animation_error\":\"%s\",\"display_te_period_us\":%lu,\"display_transfer_us\":%lu,\"display_te_scans\":%lu}\n",
                kTransitions[animation.transition], kMotions[animation.motion], animation.rotationPeriod,
                !sleeping && !menuOpen && current >= 0 && animation.active() ? "true" : "false",
                animation.angle(), static_cast<unsigned long>(animation.frameMs), animation.error,
                static_cast<unsigned long>(panelScan.periodUs), static_cast<unsigned long>(animation.transferUs),
                static_cast<unsigned long>(panelScan.scans));
}

bool unsignedArgument(const char *text, uint32_t maximum, uint32_t &value) {
  if (!text || !*text) return false;
  for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = strtoul(text, &end, 10);
  if (errno == ERANGE || *end || parsed > maximum) return false;
  value = parsed;
  return true;
}

const char *deletePicture(unsigned slot) {
  if (!storageOk) return "storage_unavailable";
  if (!preferencesOk) return "preferences_unavailable";
  if (!occupied[slot]) return "empty_slot";
  char path[24];
  slotPath(slot, path, sizeof(path));
  if (!LittleFS.remove(path)) return "storage_delete";
  occupied[slot] = false;
  autoSleep.activity(millis());
  resetSlideshowTimer();
  if (current != static_cast<int>(slot)) return nullptr;
  current = -1;
  currentCrc = 0;
  animation.invalidate(brightness);
  if (!rememberCurrent(-1)) return "preferences_write";
  const bool wasSleeping = sleeping;
  const char *error = nullptr;
  for (unsigned step = 1; step < kSlots; ++step) {
    const unsigned candidate = (slot + step) % kSlots;
    if (occupied[candidate]) {
      error = showPicture(candidate);
      break;
    }
  }
  if (current < 0) {
    if (menuOpen) drawMenu();
    else drawWelcome();
  }
  if (wasSleeping && !sleeping) setSleeping(true);
  return error;
}

void dispatch(char *line) {
  char *tokens[5] = {};
  char *save = nullptr;
  unsigned count = 0;
  for (char *token = strtok_r(line, " \t\r", &save); token && count < 5;
       token = strtok_r(nullptr, " \t\r", &save)) tokens[count++] = token;
  if (!count) return;
  uint32_t n = 0;
  if (count == 1 && strcmp(tokens[0], "STATUS") == 0) {
    status();
  } else if (count == 1 && strcmp(tokens[0], "NEXT") == 0) {
    reply(nextPicture());
  } else if (count == 1 && strcmp(tokens[0], "MENUFRAME") == 0) {
    sendMenuFrame();
  } else if (count == 1 && strcmp(tokens[0], "DISPLAYFRAME") == 0) {
    sendDisplayFrame();
  } else if (count == 2 && strcmp(tokens[0], "MENU") == 0 && unsignedArgument(tokens[1], 1, n)) {
    reply(setMenu(n != 0));
  } else if (count == 2 && strcmp(tokens[0], "MENUACT") == 0) {
    reply(menuAction(tokens[1]));
  } else if (count == 3 && strcmp(tokens[0], "SLIDESHOW") == 0) {
    uint32_t interval = 0;
    if (!unsignedArgument(tokens[1], 1, n) ||
        !unsignedArgument(tokens[2], 3600, interval) || interval < 2) {
      return reply("invalid_slideshow_arguments");
    }
    reply(setSlideshow(n != 0, interval));
  } else if (count == 4 && strcmp(tokens[0], "ANIMATION") == 0) {
    uint32_t motion = 0, period = 0;
    if (!unsignedArgument(tokens[1], 2, n) || !unsignedArgument(tokens[2], 3, motion) ||
        !unsignedArgument(tokens[3], 120, period) || period < 8) return reply("invalid_animation_arguments");
    reply(setAnimation(n, motion, period));
  } else if (count == 3 && strcmp(tokens[0], "AUTOSLEEP") == 0) {
    uint32_t batterySeconds = 0;
    if (!unsignedArgument(tokens[1], 86400, n) || !unsignedArgument(tokens[2], 86400, batterySeconds)) {
      return reply("invalid_autosleep_arguments");
    }
    reply(setAutoSleep(n, batterySeconds));
  } else if (count == 1 && strcmp(tokens[0], "REBOOT") == 0) {
    reply();
    Serial.flush();
    delay(100);
    ESP.restart();
  } else if (count == 2 && strcmp(tokens[0], "BRIGHT") == 0 && unsignedArgument(tokens[1], 180, n) && n >= 1) {
    reply(setBrightness(n));
  } else if (count == 2 && strcmp(tokens[0], "SLEEP") == 0 && unsignedArgument(tokens[1], 1, n)) {
    reply(setSleeping(n != 0));
  } else if (count == 2 && unsignedArgument(tokens[1], kSlots - 1, n)) {
    if (strcmp(tokens[0], "SHOW") == 0) reply(showPicture(n));
    else if (strcmp(tokens[0], "DELETE") == 0) reply(deletePicture(n));
    else if (strcmp(tokens[0], "GET") == 0) sendPicture(n);
    else reply("unknown_command");
  } else if (count == 4 && strcmp(tokens[0], "PUT") == 0) {
    uint32_t length = 0, crc = 0;
    if (!unsignedArgument(tokens[1], kSlots - 1, n) ||
        !unsignedArgument(tokens[2], kFrameBytes, length) || length != kFrameBytes ||
        !unsignedArgument(tokens[3], UINT32_MAX, crc)) return reply("invalid_upload_arguments");
    const char *error = receivePicture(n, crc);
    if (menuOpen) drawMenu();
    clearGesture();
    resetSlideshowTimer();
    autoSleep.activity(millis());
    reply(error);
  } else {
    reply("invalid_command");
  }
  // Only bulk image I/O pauses motion. STATUS must not change its clock.
  if (strcmp(tokens[0], "GET") == 0 || strcmp(tokens[0], "PUT") == 0) animation.pauseClock();
}

void pollSerial() {
  // Bound work so a noisy USB host cannot starve physical controls indefinitely.
  for (unsigned budget = 0; budget < 512 && Serial.available(); ++budget) {
    const int value = Serial.read();
    if (value < 0) break;
    commandLastByte = millis();
    if (value == '\n') {
      commandLine[commandLength] = '\0';
      if (commandInvalid) reply("invalid_line");
      else dispatch(commandLine);
      commandLength = 0;
      commandInvalid = false;
    } else if (value == '\r' || value == '\t' || (value >= 32 && value <= 126)) {
      if (commandLength < sizeof(commandLine) - 1 && !commandInvalid) commandLine[commandLength++] = value;
      else commandInvalid = true;
    } else {
      commandInvalid = true;
    }
  }
  if ((commandLength || commandInvalid) && millis() - commandLastByte >= 2000) {
    commandLength = 0;
    commandInvalid = false;
    reply("command_timeout");
  }
}

void pollControls() {
  const uint32_t now = millis();
  if (touchOk && touchInterrupt) {
    noInterrupts();
    touchInterrupt = false;
    interrupts();
    int16_t x[5] = {}, y[5] = {};
    const bool pressed = touch.getPoint(x, y, touch.getSupportTouchPoint()) != 0;
    fingerLastEvent = now;
    if (pressed || fingerDown) autoSleep.activity(now);
    if (suppressTouch) {
      if (!pressed) suppressTouch = false;
      else touchSuppressedAt = now;
    } else if (pressed) {
      fingerLastX = x[0];
      fingerLastY = y[0];
      if (!fingerDown) {
        fingerDown = true;
        fingerStarted = now;
        fingerInitialX = fingerLastX;
        fingerInitialY = fingerLastY;
        gestureHandled = false;
        if (sleeping) setSleeping(false);
      }
    } else if (fingerDown) {
      const uint32_t duration = now - fingerStarted;
      const int dx = fingerLastX - fingerInitialX;
      const int dy = fingerLastY - fingerInitialY;
      const bool handled = gestureHandled;
      // End this contact before acting, so changing menus does not swallow the
      // next touch. getPoint() release coordinates are not mirrored by the driver.
      fingerDown = false;
      gestureHandled = false;
      if (!handled) {
        if (!menuOpen && dy <= -80 && -dy > abs(dx)) {
          setMenu(true);
        } else if (duration >= 900) {
          setSleeping(true);
        } else if (duration >= 35) {
          if (menuOpen) {
            if (abs(dx) <= 30 && abs(dy) <= 30) {
              size_t buttonCount = 0;
              const MenuButton *buttons = menuButtons(buttonCount);
              for (size_t i = 0; i < buttonCount; ++i) {
                const auto &button = buttons[i];
                if (fingerLastX >= button.x && fingerLastX < button.x + button.width &&
                    fingerLastY >= button.y && fingerLastY < button.y + button.height) {
                  menuAction(button.action);
                  break;
                }
              }
            }
          } else {
            nextPicture();
          }
        }
      }
    }
  }
  const int upward = fingerInitialY - fingerLastY;
  const bool upwardSwipe = !menuOpen && upward >= 80 && upward > abs(fingerLastX - fingerInitialX);
  if (fingerDown && !gestureHandled && !upwardSwipe && now - fingerStarted >= 900) {
    setSleeping(true);
    gestureHandled = true;
  }
  // A missed release must not permanently disable input. IRQ reports can be
  // one second apart on CST92xx; allow more than two reports before recovery.
  if (fingerDown && now - fingerLastEvent > 2500) {
    fingerDown = false;
    gestureHandled = false;
  }
  if (suppressTouch && now - touchSuppressedAt > 2500) suppressTouch = false;

  static bool bootRaw = digitalRead(kBootPin) == LOW;
  static bool bootStable = bootRaw;
  static bool bootArmed = false;
  static uint32_t bootChanged = now;
  const bool bootPressed = digitalRead(kBootPin) == LOW;
  if (bootPressed != bootRaw) {
    bootRaw = bootPressed;
    bootChanged = now;
  }
  if (bootRaw != bootStable && now - bootChanged >= 35) {
    bootStable = bootRaw;
    autoSleep.activity(now);
    if (bootStable) bootArmed = true;
    else if (bootArmed) {
      bootArmed = false;
      setMenu(!menuOpen);
    }
  }
  static uint32_t lastPowerPoll = 0;
  if (pmuOk && now - lastPowerPoll >= 100) {
    lastPowerPoll = now;
    power.getIrqStatus();
    const bool shortPress = power.isPekeyShortPressIrq();
    power.clearIrqStatus();
    if (shortPress) setSleeping(!sleeping);
  }
}

void initStorage() {
  storageOk = LittleFS.begin(false, "/badge", 4, "badge");
  // Never auto-format an established badge filesystem on a later mount error.
  // The label limits first-install formatting to the designated data partition.
  if (!storageOk && preferencesOk && !preferences.getBool("fs_ready", false)) {
    storageOk = LittleFS.begin(true, "/badge", 4, "badge");
  }
  if (!storageOk) return;
  if (preferencesOk && !preferences.getBool("fs_ready", false)) {
    if (preferences.putBool("fs_ready", true) != 1) preferencesOk = false;
  }
  LittleFS.remove(kTemporary);
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    char path[24];
    slotPath(slot, path, sizeof(path));
    File file = LittleFS.open(path, FILE_READ);
    ImageHeader header;
    occupied[slot] = readHeader(file, header);
  }
}
} // namespace

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);
  Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(8192);
  Serial.setTxTimeoutMs(1000);
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  initCrc();
  preferencesOk = preferences.begin("picture-badge", false);
  if (preferencesOk) {
    brightness = preferences.getUChar("brightness", 80);
    if (brightness < 1 || brightness > 180) brightness = 80;
    const uint32_t packed = preferences.getUInt("slideshow", 10U << 1);
    const uint32_t interval = packed >> 1;
    if (interval >= 2 && interval <= 3600) {
      slideshowEnabled = (packed & 1U) != 0;
      slideshowInterval = interval;
    } else {
      slideshowError = "invalid_saved_slideshow";
    }
    const uint64_t sleepConfig = preferences.getULong64("auto_sleep", 15);
    const uint32_t usbSeconds = sleepConfig & 0x1FFFFU;
    const uint32_t batterySeconds = sleepConfig >> 17;
    if (AutoSleep::validTimeout(usbSeconds) && AutoSleep::validTimeout(batterySeconds) && (sleepConfig >> 34) == 0) {
      autoSleep.usbSeconds = usbSeconds;
      autoSleep.batterySeconds = batterySeconds;
    }
    const uint32_t savedAnimation = preferences.getUInt("animation", 1U | (2U << 2) | (24U << 4));
    const uint32_t period = savedAnimation >> 4;
    if ((savedAnimation & 3U) < 3U && period >= 8U && period <= 120U) {
      animation.transition = savedAnimation & 3U;
      animation.motion = (savedAnimation >> 2) & 3U;
      animation.rotationPeriod = period;
    } else {
      animation.error = "invalid_saved_animation";
    }
  }
  frame = static_cast<uint8_t *>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  animationOk = animation.begin();
  pinMode(kTearingPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(kTearingPin), onTearing, CHANGE);
  pinMode(kBootPin, INPUT_PULLUP);
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setTimeOut(50);
  gravity.begin();
  pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuOk) {
    // Keep factory rail/charging settings; only enable measurement and key IRQ.
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
  }
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(30);
  digitalWrite(TP_RST, HIGH);
  delay(50);
  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
  // Vendor CST92xx supports two contacts; reject unsupported larger reports
  // before ever passing our fixed five-element arrays to the driver.
  touchOk = touchOk && touch.getSupportTouchPoint() > 0 && touch.getSupportTouchPoint() <= 5;
  if (touchOk) {
    // begin() reads attributes in command mode; reset returns to touch reports.
    touch.reset();
    delay(50);
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setMirrorXY(true, true);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouch, FALLING);
  }
  displayOk = gfx.begin(40000000);
  if (displayOk) { gfx.setBrightness(brightness); enablePanelSync(); delay(50); }
  initStorage();
  bool shown = false;
  const int remembered = preferencesOk ? preferences.getInt("current", -1) : -1;
  if (remembered >= 0 && remembered < static_cast<int>(kSlots) && occupied[remembered]) {
    shown = showPicture(remembered) == nullptr;
  }
  if (!shown) {
    for (unsigned slot = 0; slot < kSlots && !shown; ++slot) {
      if (occupied[slot]) shown = showPicture(slot) == nullptr;
    }
  }
  if (!shown) drawWelcome();
  resetSlideshowTimer();
  autoSleep.activity(millis());
}

void loop() {
  pollSerial();
  pollControls();
  pollAutoSleep();
  pollSlideshow();
  refreshGravity();
  animation.poll(!sleeping && !menuOpen && current >= 0 && !commandLength && !fingerDown, brightness);
  delay(5);
}
