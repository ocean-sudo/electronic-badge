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
#include "power_off.h"
#include "picture_animation.h"
#include "gravity_sensor.h"
#include "boot_animation.h"
#include "native_menu.h"
#include "battery_status.h"
#include "picture_player.h"
#include "touch_gesture.h"
#include <driver/gpio.h>
#include <algorithm>
#include <vector>
#include "jpeg_decoder.h"

namespace {
constexpr size_t kFrameBytes = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
constexpr size_t kChunkBytes = 4096;
constexpr uint32_t kMaxSlot = BadgeJpeg::kMaxSlot;
constexpr uint32_t kChunkTimeout = 10000;
constexpr uint32_t kTransferTimeout = 90000;
constexpr char kTemporary[] = "/upload.tmp";
static_assert(kFrameBytes == 434312, "Host and panel geometry must agree");
constexpr uint8_t kBootPin = 0; // ESP32-S3 BOOT is GPIO0.
constexpr uint16_t kIntervals[] = {2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
constexpr uint32_t kSleepIntervals[] = {0, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600, 7200, 14400, 28800, 43200, 86400};
constexpr uint16_t kRotationPeriods[] = {8, 12, 18, 24, 36, 60, 90, 120};
constexpr uint8_t kTearingPin = 13; // 1.75C schematic: GPIO13 -> LCD_TE.
constexpr const char *kTransitions[] = {"direct", "fade", "slide", "ripple"};
constexpr const char *kMotions[] = {"off", "shift", "rotate", "gravity"};


BadgeQSPI bus(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 gfx(&bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
PanelScan panelScan;
PictureAnimation animation(gfx, bus, panelScan);
badge_boot::Animation bootAnimation;
bool animationOk = false;
// getPoint() maps I2C/ACK errors to zero points; zero is not proof of release.
class SleepAwareTouch final : public TouchDrvCST92xx {
 public:
  bool confirmedRelease() {
    uint8_t report[CST92XX_MAX_FINGER_NUM * 5 + 5];
    uint8_t command[] = {highByte(CST92XX_READ_COMMAND), lowByte(CST92XX_READ_COMMAND), CST92XX_ACK};
    if (comm->writeThenRead(command, 2, report, sizeof(report)) != 0 ||
        comm->writeBuffer(command, sizeof(command)) != 0 || report[6] != CST92XX_ACK) return false;
    const uint8_t count = report[5] & 0x7f;
    if (count > CST92XX_MAX_FINGER_NUM) return false;
    for (uint8_t i = 0; i < count; ++i) {
      const uint8_t contact = report[i * 5 + (i ? 2 : 0)];
      // Vendor parseFingerData uses event 0x06 for pressed; only an explicit
      // release (0x00) with a valid finger ID can clear the sleep gate.
      if ((contact >> 4) >= CST92XX_MAX_FINGER_NUM || (contact & 0x0f) != 0) return false;
    }
    return true;
  }
};
SleepAwareTouch touch;
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
struct SlotEntry { int32_t slot; uint32_t bytes; uint32_t storedCrc; uint32_t transferCrc; };
std::vector<SlotEntry> catalog;
std::vector<int32_t> catalogIds;
uint32_t catalogRevision = 0;
uint64_t catalogBytes = 0;
String catalogJson;
uint32_t catalogJsonCrc = 0;
bool catalogOk = false;
BadgeJpegDecoder jpegDecoder;
uint32_t loadUs = 0, decodeUs = 0;
size_t storageTotal = 0, storageUsed = 0;
int64_t nextSlotId = 0;
bool displayOk = false;
bool touchOk = false;
bool storageOk = false;
bool preferencesOk = false;
bool pmuOk = false;
bool pmicPowerKeyActive = false;
bool pmicPowerOffReady = false;
enum class PowerOffReason { None, ManualSleep, AutomaticTimeout, ScreenOffBattery };
PowerOffReason pendingPowerOff = PowerOffReason::None;
bool autoPowerOffBlocked = false;
bool sleeping = false;
uint8_t brightness = 80;
int current = -1;
uint32_t currentCrc = 0;
bool menuOpen = false;
PicturePlayer player;
TouchGesture gesture;
BatteryStatus menuBattery;
MenuPage menuPage = MenuPage::Main;
AutoSleep autoSleep;
bool slideshowEnabled = false;
uint16_t slideshowInterval = 10;
uint32_t slideshowStarted = 0;
const char *slideshowError = "";
volatile bool touchInterrupt = false;
bool fingerDown = false;
uint32_t fingerLastEvent = 0;
int16_t fingerLastX = 0, fingerLastY = 0;
bool suppressTouch = false;
uint32_t touchSuppressedAt = 0;
char commandLine[96];
size_t commandLength = 0;
bool commandInvalid = false;
uint32_t commandLastByte = 0;

const char *requestPmicPowerOff(bool automatic);
void executePmicPowerOff();

bool shutdownInputsIdle() {
  return PmicPowerOff::inputsIdle({fingerDown, suppressTouch || touchInterrupt,
      pmicPowerKeyActive, digitalRead(kBootPin) == LOW,
      commandLength != 0, commandInvalid, Serial.available() != 0});
}

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

void resetTouchReports() {
  touch.reset();
  delay(50);
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);
  pinMode(TP_INT, INPUT);
  attachInterrupt(digitalPinToInterrupt(TP_INT), onTouch, FALLING);
}

void resetSlideshowTimer() { slideshowStarted = millis(); }

void clearGesture() {
  if (fingerDown) {
    suppressTouch = true;
    touchSuppressedAt = millis();
  }
  fingerDown = false;
  gesture.cancel();
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

void tickBootAnimation() {
  if (!displayOk || !bootAnimation.active() || !bootAnimation.tick(millis())) return;
  auto *pixels = reinterpret_cast<uint16_t *>(frame);
  for (uint8_t patch = 0; patch < badge_boot::PatchCount; ++patch) {
    int16_t x, y;
    if (bootAnimation.renderPatch(patch, pixels, x, y))
      gfx.draw16bitRGBBitmap(x, y, pixels, badge_boot::PatchExtent, badge_boot::PatchExtent);
  }
}


void slotPath(unsigned slot, char *path, size_t length) {
  snprintf(path, length, "/slot%u.jpg", slot);
}

auto findSlot(unsigned slot) {
  return std::lower_bound(catalog.begin(), catalog.end(), slot,
      [](const SlotEntry& entry, unsigned id) { return static_cast<unsigned>(entry.slot) < id; });
}
bool hasSlot(unsigned slot) {
  const auto entry = findSlot(slot);
  return entry != catalog.end() && static_cast<unsigned>(entry->slot) == slot;
}
int64_t nextSlot() {
  uint32_t candidate = 0;
  for (const auto& entry : catalog) {
    if (static_cast<uint32_t>(entry.slot) != candidate) break;
    if (candidate == kMaxSlot) return -1;
    ++candidate;
  }
  return candidate;
}

void publishCatalog() {
  ++catalogRevision;
  storageTotal = LittleFS.totalBytes();
  storageUsed = LittleFS.usedBytes();
  nextSlotId = nextSlot();
  catalogBytes = 0;
  catalogIds.clear();
  catalogIds.reserve(catalog.size());
  catalogOk = catalogJson.reserve(64U + catalog.size() * 64U);
  catalogJson = "";
  if (catalogOk) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "{\"revision\":%lu,\"images\":[", static_cast<unsigned long>(catalogRevision));
    catalogJson += prefix;
  }
  bool first = true;
  for (const auto& entry : catalog) {
    catalogIds.push_back(entry.slot);
    catalogBytes += entry.bytes;
    if (!catalogOk) continue;
    if (!first) catalogJson += ',';
    first = false;
    char item[112];
    snprintf(item, sizeof(item), "{\"slot\":%ld,\"bytes\":%lu,\"crc32\":%lu}",
             static_cast<long>(entry.slot), static_cast<unsigned long>(entry.bytes), static_cast<unsigned long>(entry.transferCrc));
    catalogJson += item;
  }
  if (catalogOk) catalogJson += "]}";
  catalogJsonCrc = updateCrc(0xffffffffU, reinterpret_cast<const uint8_t*>(catalogJson.c_str()), catalogJson.length()) ^ 0xffffffffU;
  player.membership(catalogIds.data(), catalogIds.size());
}

const char *loadJpeg(File& file, uint32_t& crc, uint8_t* destination, uint32_t* transfer = nullptr) {
  if (!destination) return "psram_unavailable";
  const uint32_t started = micros();
  uint32_t exactCrc = 0;
  const char* error = jpegDecoder.decode(file, destination, ioBuffer, sizeof(ioBuffer), crcTable, crc, exactCrc);
  if (!error) {
    loadUs = micros() - started;
    decodeUs = jpegDecoder.decodeUs;
    if (transfer) *transfer = exactCrc;
  }
  return error;
}

// Catalog discovery validates every persistent byte without IDCT or touching
// canonical image buffers. Only the selected image is decoded at boot.
const char* inspectJpeg(File& file, uint32_t& storedCrc, uint32_t& transferCrc) {
  if (!file || !file.seek(0)) return "storage_read";
  const size_t length = file.size();
  BadgeJpeg::Integrity integrity(crcTable);
  uint32_t expected = 0;
  while (integrity.bytes < length) {
    const size_t count = min(sizeof(ioBuffer), length - integrity.bytes);
    if (file.read(ioBuffer, count) != count) return "storage_read";
    if (!integrity.bytes && !BadgeJpeg::header(ioBuffer, count, length, expected)) return "jpeg_metadata";
    integrity.append(ioBuffer, count); delay(0); tickBootAnimation();
  }
  if (const char* error = integrity.finish(length, expected)) return error;
  storedCrc = integrity.storedCrc(); transferCrc = integrity.transferCrc();
  return nullptr;
}

const char *loadPicture(unsigned slot, uint32_t &crc, uint8_t *destination = frame) {
  if (!storageOk) return "storage_unavailable";
  if (!hasSlot(slot)) return "empty_slot";
  char path[24];
  slotPath(slot, path, sizeof(path));
  File file = LittleFS.open(path, FILE_READ);
  uint32_t normalizedCrc = 0;
  return loadJpeg(file, normalizedCrc, destination, &crc);
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
  centeredText("Hold: sleep  |  PWR: toggle/start", 336, 2, 0xBDF7);
  if (!storageOk || !frame || !preferencesOk) {
    centeredText("Check device status", 372, 2, 0xFC80);
  } else {
    char capacity[40];
    snprintf(capacity, sizeof(capacity), "466 x 466  |  %u pictures", static_cast<unsigned>(catalog.size()));
    centeredText(capacity, 372, 2, 0x2DDB);
  }
}

const MenuButton *menuButtons(size_t &count) {
  return pageButtons(menuPage, count);
}

int hitMenuButton(int x, int y) {
  size_t count;
  const MenuButton *buttons = menuButtons(count);
  for (size_t i = 0; i < count; ++i) {
    const auto &button = buttons[i];
    if (!menuActionAllowed(menuPage, button.action, animation.motion == 2)) continue;
    if (x >= button.x && x < button.x + button.width &&
        y >= button.y && y < button.y + button.height) return static_cast<int>(i);
  }
  return -1;
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

BatteryStatus readBatteryStatus(int reg00) {
  const int percent = pmuOk && reg00 >= 0 && (reg00 & 0x08)
                    ? power.readRegister(XPOWERS_AXP2101_BAT_PERCENT_DATA) : -1;
  const int reg01 = pmuOk ? power.readRegister(XPOWERS_AXP2101_STATUS2) : -1;
  return BatteryStatus::decode(reg00, percent, reg01);
}

void drawBatteryRow(FrameCanvas &canvas, const BatteryStatus &battery) {
  char text[32];
  battery.text(text, sizeof(text));
  canvas.fillRect(kMenuBatteryX, kMenuBatteryY, kMenuBatteryWidth, kMenuBatteryHeight, 0x0843);
  menuText(canvas, text, 233, kMenuBatteryTextY, 2, 0xBDF7);
}

void updateMenuBattery(int reg00) {
  if (!menuOpen || sleeping || !displayOk || !frame) return;
  const BatteryStatus battery = readBatteryStatus(reg00);
  if (battery == menuBattery) return;
  menuBattery = battery;
  FrameCanvas canvas(frame);
  drawBatteryRow(canvas, battery);
  gfx.startWrite();
  gfx.writeAddrWindow(kMenuBatteryX, kMenuBatteryY, kMenuBatteryWidth, kMenuBatteryHeight);
  auto *pixels = reinterpret_cast<uint16_t *>(frame);
  for (int y = kMenuBatteryY; y < kMenuBatteryY + kMenuBatteryHeight; ++y)
    gfx.writePixels(pixels + y * LCD_WIDTH + kMenuBatteryX, kMenuBatteryWidth);
  gfx.endWrite();
}

void drawMenu() {
  if (!displayOk || !frame || sleeping || !menuOpen) return;
  FrameCanvas canvas(frame);
  canvas.fillScreen(0x0843);
  menuText(canvas, menuTitle(menuPage), 233, kMenuTitleY, 2, 0xFFFF);
  menuBattery = readBatteryStatus(pmuOk ? power.readRegister(XPOWERS_AXP2101_STATUS1) : -1);
  drawBatteryRow(canvas, menuBattery);
  char picture[24];
  if (current >= 0) snprintf(picture, sizeof(picture), "Picture %d", current + 1);
  else snprintf(picture, sizeof(picture), "No picture");
  menuText(canvas, picture, 233, kMenuPictureY, 1, 0xBDF7);
  size_t buttonCount = 0;
  const MenuButton *buttons = menuButtons(buttonCount);
  for (size_t i = 0; i < buttonCount; ++i) {
    const auto &button = buttons[i];
    canvas.fillRoundRect(button.x, button.y, button.width, button.height, 12, 0x1230);
    const char *label = menuButtonLabel(button, slideshowEnabled, player.shuffle(),
                                      animation.transition, animation.motion);
    const bool enabled = menuActionAllowed(menuPage, button.action, animation.motion == 2) &&
                         (strcmp(button.action, "previous") != 0 || player.previousAvailable());
    menuText(canvas, label, button.x + button.width / 2,
             button.y + (button.height - 16) / 2, 2, enabled ? 0xFFFF : 0x7BEF);
  }
  char value[32];
  if (menuPage == MenuPage::Timeout) {
    menuText(canvas, "USB SCREEN", 233, 105, 2, 0xBDF7);
    timeoutText(value, sizeof(value), autoSleep.usbSeconds);
    menuText(canvas, value, 233, 147, 2, 0xFFFF);
    menuText(canvas, "BATTERY OFF", 233, 205, 2, 0xBDF7);
    timeoutText(value, sizeof(value), autoSleep.batterySeconds);
    menuText(canvas, value, 233, 247, 2, 0xFFFF);
    menuText(canvas, pmicPowerOffReady ? "PWR HOLD 2 SEC TO TURN ON" : "PMU POWER-OFF UNAVAILABLE",
             233, 295, 1, pmicPowerOffReady ? 0xBDF7 : 0xFC80);
  } else if (menuPage == MenuPage::Animation) {
    if (!gravity.ok) menuText(canvas, "GRAVITY SENSOR UNAVAILABLE", 233, 225, 1, 0xFC80);
    else if (animation.motion == 3) menuText(canvas, "FLAT: LAST POSE / FIRST: NATIVE", 233, 225, 1, 0xBDF7);
    menuText(canvas, "ROTATE SPEED (SECONDS / TURN)", 233, 247, 1, 0xBDF7);
    snprintf(value, sizeof(value), "%u sec", animation.rotationPeriod);
    menuText(canvas, value, 233, 290, 2, 0xFFFF);
    menuText(canvas, animation.motion == 3 ? "GRAVITY USES SENSOR, NOT THIS SPEED"
                                         : "- FASTER / + SLOWER", 233, 330, 1, 0xBDF7);
    menuText(canvas, "Motion never renews idle timeout", 233, 414, 1, 0xBDF7);
  } else if (menuPage == MenuPage::Playback) {
    snprintf(value, sizeof(value), "%u sec", slideshowInterval);
    menuText(canvas, value, 233, 281, 2, 0xFFFF);
    menuText(canvas, "INTERVAL", 233, 236, 1, 0xBDF7);
  } else if (menuPage == MenuPage::Display) {
    snprintf(value, sizeof(value), "%u", brightness);
    menuText(canvas, value, 233, 165, 2, 0xFFFF);
    menuText(canvas, "BRIGHTNESS", 233, 110, 1, 0xBDF7);
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

const char *showPicture(unsigned slot, bool remember = true, bool manual = true, bool closeMenu = false) {
  if (!displayOk) return "display_unavailable";
  if (remember && !preferencesOk) return "preferences_unavailable";
  if (!animationOk) return "psram_unavailable";
  const bool animate = !sleeping && !menuOpen;
  animation.cancel(brightness);
  uint32_t crc;
  const char *error = loadPicture(slot, crc, reinterpret_cast<uint8_t *>(animation.nextSource()));
  if (error) return error;
  const bool wasSleeping = sleeping;
  if (wasSleeping && displayOk) { gfx.displayOn(); enablePanelSync(); gfx.setBrightness(brightness); }
  error = animation.prepareImage(animate, brightness, !menuOpen || closeMenu);
  if (!error && remember && !rememberCurrent(slot)) {
    animation.rejectImage(!menuOpen);
    error = "preferences_write";
  }
  if (error) {
    if (menuOpen) drawMenu();
    if (wasSleeping && displayOk) { gfx.setBrightness(0); gfx.displayOff(); }
    return error;
  }
  animation.commitImage();
  current = slot;
  currentCrc = crc;
  if (manual) player.commitManual(slot);
  else player.commit(slot);
  if (closeMenu && menuOpen) {
    menuOpen = false;
    clearGesture();
  }
  wakePanel();
  if (remember) autoSleep.activity(millis());
  if (menuOpen) drawMenu();
  return nullptr;
}

const char *nextPicture(bool remember = true) {
  const int slot = player.prepareNext(current);
  if (slot < 0) return "no_next_picture";
  return showPicture(static_cast<unsigned>(slot), remember, false);
}

const char *previousPicture() {
  const int slot = player.preparePrevious();
  if (slot < 0) return "previous_unavailable";
  return showPicture(static_cast<unsigned>(slot), true, false);
}

const char *setShuffle(bool enabled) {
  if (!preferencesOk) return "preferences_unavailable";
  if (enabled == player.shuffle()) return nullptr;
  if (preferences.getBool("shuffle", false) != enabled &&
      preferences.putBool("shuffle", enabled) != 1) {
    preferencesOk = false;
    return "preferences_write";
  }
  player.setShuffle(enabled, current);
  autoSleep.activity(millis());
  resetSlideshowTimer();
  if (menuOpen) drawMenu();
  return nullptr;
}

const char *restorePicture() {
  if (current >= 0) return animation.presentCurrent();
  animation.invalidate(brightness);
  drawWelcome();
  return nullptr;
}

const char *setSleeping(bool value) {
  if (!displayOk) return "display_unavailable";
  const uint32_t now = millis();
  autoSleep.activity(now);
  if (value) {
    const auto source = AutoSleep::decodeSupply(pmuOk ? power.readRegister(XPOWERS_AXP2101_STATUS1) : -1);
    if (autoSleep.updateSupply(source, now)) autoPowerOffBlocked = false;
    if (source == AutoSleep::Supply::Battery) {
      const char *error = requestPmicPowerOff(false);
      if (error) return error;
      menuOpen = false;
      clearGesture();
      resetSlideshowTimer();
      return nullptr;
    }
  }
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
  if (pendingPowerOff == PowerOffReason::ScreenOffBattery) {
    pendingPowerOff = PowerOffReason::None;
    autoPowerOffBlocked = false;
  }
  wakePanel();
  return restorePicture();
}
const char *requestPmicPowerOff(bool automatic) {
  if (!pmuOk) return "pmu_unavailable";
  if (!pmicPowerOffReady) return "poweroff_unavailable";
  const uint32_t now = millis();
  const auto source = AutoSleep::decodeSupply(power.readRegister(XPOWERS_AXP2101_STATUS1));
  if (autoSleep.updateSupply(source, now)) autoPowerOffBlocked = false;
  if (source != AutoSleep::Supply::Battery) return "battery_power_required";
  if (automatic && !autoSleep.due(now)) return "idle_timeout_reset";
  pendingPowerOff = automatic ? PowerOffReason::AutomaticTimeout : PowerOffReason::ManualSleep;
  return nullptr;
}
void executePmicPowerOff() {
  if (pendingPowerOff == PowerOffReason::None || !shutdownInputsIdle()) return;
  if (pendingPowerOff == PowerOffReason::AutomaticTimeout && !autoSleep.due(millis())) {
    pendingPowerOff = PowerOffReason::None;
    return;
  }
  pendingPowerOff = PowerOffReason::None;
  Serial.flush(); // Complete the request acknowledgement before the final source check.
  const bool shutdownIssued = PmicPowerOff::shutdownOnBattery(
      power, pmuOk, pmicPowerOffReady, XPOWERS_AXP2101_STATUS1, [&] {
        // The preceding STATUS1 read gates every peripheral change; abort leaves them untouched.
        animation.cancel(brightness);
        if (displayOk) {
          gfx.setBrightness(0);
          gfx.displayOff();
        }
        if (touchOk) {
          detachInterrupt(digitalPinToInterrupt(TP_INT));
          touch.sleep();
        }
        gravity.enable(false);
      });
  if (!shutdownIssued) {
    autoPowerOffBlocked = true;
    return;
  }
  for (;;) delay(1000); // Never resume application work after the PMIC shutdown command.
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
    const auto source = AutoSleep::decodeSupply(status);
    if (autoSleep.updateSupply(source, now)) autoPowerOffBlocked = false;
    updateMenuBattery(status);
  }
  if (sleeping && autoSleep.supply == AutoSleep::Supply::Battery) {
    if (pendingPowerOff == PowerOffReason::None && !autoPowerOffBlocked) {
      if (requestPmicPowerOff(false)) autoPowerOffBlocked = true;
      else pendingPowerOff = PowerOffReason::ScreenOffBattery;
    }
    return;
  }
  if (pendingPowerOff != PowerOffReason::None) return;
  if (sleeping || fingerDown || suppressTouch || touchInterrupt || commandLength || commandInvalid ||
      Serial.available() || pmicPowerKeyActive || digitalRead(kBootPin) == LOW) {
    autoPowerOffBlocked = false;
    return;
  }
  AutoSleep::TimeoutAction action = autoSleep.timeoutAction(now);
  if (action == AutoSleep::TimeoutAction::None) {
    autoPowerOffBlocked = false;
    return;
  }
  // Refresh supply at the timeout boundary so a newly attached USB source
  // cannot be treated as a battery timeout (or vice versa).
  const auto supply = AutoSleep::decodeSupply(pmuOk ? power.readRegister(XPOWERS_AXP2101_STATUS1) : -1);
  if (autoSleep.updateSupply(supply, millis())) {
    autoPowerOffBlocked = false;
    return;
  }
  action = autoSleep.timeoutAction(millis());
  if (action == AutoSleep::TimeoutAction::None) return;
  if (action == AutoSleep::TimeoutAction::ScreenOff) {
    setSleeping(true);
    return;
  }
  if (autoPowerOffBlocked || !pmicPowerOffReady) {
    autoPowerOffBlocked = true;
    return;
  }
  if (requestPmicPowerOff(true)) autoPowerOffBlocked = true;
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
  if (transition >= 4) return "invalid_animation";
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
  if (!menuActionAllowed(menuPage, action, animation.motion == 2)) return "invalid_menu_action";
  if (strcmp(action, "back") == 0 && menuPage == MenuPage::Main) return setMenu(false);
  if (strcmp(action, "playback") == 0 || strcmp(action, "display") == 0 ||
      strcmp(action, "timeout") == 0 || strcmp(action, "animation") == 0 ||
      strcmp(action, "main") == 0 || strcmp(action, "back") == 0) {
    const MenuPage previousPage = menuPage;
    menuPage = strcmp(action, "playback") == 0 ? MenuPage::Playback
             : strcmp(action, "display") == 0 ? MenuPage::Display
             : strcmp(action, "timeout") == 0 ? MenuPage::Timeout
             : strcmp(action, "animation") == 0 ? MenuPage::Animation
             : strcmp(action, "back") == 0 && previousPage == MenuPage::Animation ? MenuPage::Display
             : MenuPage::Main;
    clearGesture();
    autoSleep.activity(millis());
    drawMenu();
    return nullptr;
  }
  if (strcmp(action, "transition_next") == 0) return setAnimation((animation.transition + 1) % 4, animation.motion, animation.rotationPeriod);
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
  if (strcmp(action, "shuffle") == 0) return setShuffle(!player.shuffle());
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
  if (strcmp(action, "previous") == 0) return previousPicture();
  if (strcmp(action, "sleep") == 0) return setSleeping(true);
  return "invalid_menu_action";
}

void pollSlideshow() {
  if (!slideshowEnabled || sleeping || menuOpen) return;
  if (millis() - slideshowStarted < static_cast<uint32_t>(slideshowInterval) * 1000U) return;
  resetSlideshowTimer();
  if (catalog.size() < 2) return;
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

const char *receivePicture(unsigned slot, size_t length, uint32_t expectedCrc) {
  if (!storageOk) return "storage_unavailable";
  if (!frame || !animationOk) return "psram_unavailable";
  if (!displayOk) return "display_unavailable";
  if (!preferencesOk) return "preferences_unavailable";
  const size_t total = storageTotal, used = storageUsed;
  if (used > total || length > total - used || total - used - length < BadgeJpeg::kStorageReserve) return "storage_full";
  File file = LittleFS.open(kTemporary, FILE_WRITE);
  if (!file) return "storage_open";
  auto abort = [&](const char* error) { file.close(); LittleFS.remove(kTemporary); storageUsed = LittleFS.usedBytes(); return error; };
  Serial.print("READY\n");
  const uint32_t started = millis();
  uint32_t sum = 0xffffffffU;
  for (size_t offset = 0; offset < length;) {
    const size_t count = min(kChunkBytes, length - offset);
    const uint32_t chunkStarted = millis();
    size_t received = 0;
    while (received < count) {
      if (millis() - chunkStarted >= kChunkTimeout || millis() - started >= kTransferTimeout) {
        drainAbandonedUpload(); return abort("upload_timeout");
      }
      const int available = Serial.available();
      if (available > 0) {
        const size_t wanted = min(count - received, static_cast<size_t>(available));
        received += Serial.read(ioBuffer + received, wanted);
      } else delay(1);
    }
    sum = updateCrc(sum, ioBuffer, count);
    if (file.write(ioBuffer, count) != count) { drainAbandonedUpload(); return abort("storage_write"); }
    offset += count; Serial.print("ACK\n");
  }
  if ((sum ^ 0xffffffffU) != expectedCrc) return abort("upload_crc_mismatch");
  file.flush(); file.close();
  animation.cancel(brightness); // The inactive bank can still hold an outgoing slide.
  file = LittleFS.open(kTemporary, FILE_READ);
  uint32_t storedCrc = 0, transferCrc = 0;
  const char* error = loadJpeg(file, storedCrc, reinterpret_cast<uint8_t*>(animation.nextSource()), &transferCrc);
  if (error || transferCrc != expectedCrc) return abort(error ? error : "storage_verify");
  file.close();
  char path[24]; slotPath(slot, path, sizeof(path));
  if (!LittleFS.rename(kTemporary, path)) return abort("storage_commit");
  auto entry = findSlot(slot);
  const SlotEntry updated{static_cast<int32_t>(slot), static_cast<uint32_t>(length), storedCrc, transferCrc};
  if (entry != catalog.end() && entry->slot == static_cast<int32_t>(slot)) { *entry = updated; player.invalidate(slot); }
  else catalog.insert(entry, updated);
  publishCatalog();
  const bool wasSleeping = sleeping;
  if (wasSleeping) { gfx.displayOn(); enablePanelSync(); gfx.setBrightness(brightness); }
  error = animation.prepareImage(false, brightness);
  if (!error && !rememberCurrent(slot)) { animation.rejectImage(!menuOpen); error = "preferences_write"; }
  if (error) { if (wasSleeping) { gfx.setBrightness(0); gfx.displayOff(); } return error; }
  animation.commitImage();
  current = slot; currentCrc = transferCrc; player.commitManual(slot);
  menuOpen = false; clearGesture(); wakePanel();
  return nullptr;
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
  if (!storageOk) return reply("storage_unavailable");
  const auto entry = findSlot(slot);
  if (entry == catalog.end() || entry->slot != static_cast<int32_t>(slot)) return reply("empty_slot");
  char path[24]; slotPath(slot, path, sizeof(path));
  File file = LittleFS.open(path, FILE_READ);
  if (!file || file.size() != entry->bytes) return reply("storage_read");
  Serial.printf("DATA %lu %lu\n", static_cast<unsigned long>(entry->bytes), static_cast<unsigned long>(entry->transferCrc));
  uint32_t sum = 0xffffffffU;
  const uint32_t started = millis();
  for (size_t offset = 0; offset < file.size();) {
    const size_t wanted = min(sizeof(ioBuffer), file.size() - offset);
    if (file.read(ioBuffer, wanted) != wanted) return;
    sum = updateCrc(sum, ioBuffer, wanted);
    size_t sent = 0;
    while (sent < wanted) {
      sent += Serial.write(ioBuffer + sent, wanted - sent);
      if (millis() - started >= kTransferTimeout || !Serial.isConnected()) return;
      delay(0);
    }
    offset += wanted;
  }
  Serial.print("\n");
  reply((sum ^ 0xffffffffU) == entry->transferCrc ? nullptr : "stored_crc_mismatch");
  resetSlideshowTimer();
}

void sendCatalog() {
  if (!storageOk) return reply("storage_unavailable");
  if (!catalogOk) return reply("catalog_unavailable");
  Serial.printf("DATA %u %lu\n", static_cast<unsigned>(catalogJson.length()), static_cast<unsigned long>(catalogJsonCrc));
  const uint32_t started = millis();
  for (size_t offset = 0; offset < catalogJson.length();) {
    const size_t wanted = min(kChunkBytes, catalogJson.length() - offset);
    offset += Serial.write(reinterpret_cast<const uint8_t*>(catalogJson.c_str()) + offset, wanted);
    if (millis() - started >= kTransferTimeout || !Serial.isConnected()) return;
    delay(0);
  }
  Serial.print("\nOK\n");
}

void sendMenuFrame() {
  if (!menuOpen || sleeping) return reply("menu_closed");
  if (!displayOk) return reply("display_unavailable");
  if (!frame) return reply("psram_unavailable");
  sendFrame(updateCrc(0xFFFFFFFFU, frame, kFrameBytes) ^ 0xFFFFFFFFU);
}

void sendDisplayFrame() {
  if (sleeping || menuOpen || current < 0 || !animation.snapshot()) return reply("display_frame_unavailable");
  const auto *pixels = reinterpret_cast<const uint8_t *>(animation.snapshot());
  sendFrame(updateCrc(0xFFFFFFFFU, pixels, kFrameBytes) ^ 0xFFFFFFFFU, pixels);
  animation.pauseClock();
}

void status() {
  const BatteryStatus battery = readBatteryStatus(pmuOk ? power.readRegister(XPOWERS_AXP2101_STATUS1) : -1);
  const char *batteryCharging = battery.charging < 0 ? "null" : battery.charging ? "true" : "false";
  const size_t total = storageTotal, used = storageUsed;
  Serial.printf("{\"firmware\":\"picture-badge-2\",\"width\":466,\"height\":466,\"catalog_revision\":%lu,\"image_count\":%u,\"catalog_bytes\":%llu,\"next_slot\":",
                static_cast<unsigned long>(catalogRevision), static_cast<unsigned>(catalog.size()), static_cast<unsigned long long>(catalogBytes));
  const int64_t next = nextSlotId;
  if (next < 0) Serial.print("null"); else Serial.print(static_cast<unsigned long>(next));
  Serial.printf(",\"storage_total_bytes\":%u,\"storage_used_bytes\":%u,\"storage_free_bytes\":%u,\"storage_reserve_bytes\":%u,\"load_us\":%lu,\"decode_us\":%lu,",
                static_cast<unsigned>(total), static_cast<unsigned>(used), static_cast<unsigned>(total >= used ? total - used : 0),
                static_cast<unsigned>(BadgeJpeg::kStorageReserve), static_cast<unsigned long>(loadUs), static_cast<unsigned long>(decodeUs));
  Serial.printf("\"current\":%d,\"brightness\":%u,\"sleeping\":%s,\"touch_ok\":%s,\"display_ok\":%s,\"storage_ok\":%s,\"current_crc32\":%lu,\"battery_connected\":%s,\"battery_percent\":%d,\"pmu_ok\":%s,\"psram_ok\":%s,\"preferences_ok\":%s,\"slideshow_enabled\":%s,\"slideshow_interval\":%u,\"slideshow_error\":\"%s\",\"menu_open\":%s,\"menu_page\":\"%s\",\"power_source\":\"%s\",\"autosleep_usb_seconds\":%lu,\"autosleep_battery_seconds\":%lu,\"autosleep_active_seconds\":%lu,\"autosleep_remaining_seconds\":%lu,",
                current, brightness, sleeping ? "true" : "false", touchOk ? "true" : "false",
                displayOk ? "true" : "false", storageOk ? "true" : "false",
                static_cast<unsigned long>(currentCrc), battery.presence < 0 ? "null" : battery.presence ? "true" : "false", battery.percent,
                pmuOk ? "true" : "false", frame && animationOk ? "true" : "false", preferencesOk ? "true" : "false",
                slideshowEnabled ? "true" : "false", slideshowInterval, slideshowError,
                menuOpen ? "true" : "false", menuPageName(menuPage),
                autoSleep.supply == AutoSleep::Supply::Usb ? "usb" : autoSleep.supply == AutoSleep::Supply::Battery ? "battery" : "unknown",
                static_cast<unsigned long>(autoSleep.usbSeconds), static_cast<unsigned long>(autoSleep.batterySeconds),
                static_cast<unsigned long>(autoSleep.timeoutSeconds()),
                static_cast<unsigned long>(sleeping ? 0 : (autoSleep.remainingMs(millis()) + 999U) / 1000U));
  Serial.printf("\"pmic_poweroff_ready\":%s,\"pmic_on_hold_ms\":%u,",
                pmicPowerOffReady ? "true" : "false",
                pmicPowerOffReady ? PmicPowerOff::kPowerOnHoldMilliseconds : 0);
  Serial.printf("\"battery_charging\":%s,", batteryCharging);
  Serial.printf("\"shuffle_enabled\":%s,\"previous_available\":%s,",
                player.shuffle() ? "true" : "false", player.previousAvailable() ? "true" : "false");
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
  if (!hasSlot(slot)) return "empty_slot";
  char path[24];
  slotPath(slot, path, sizeof(path));
  if (!LittleFS.remove(path)) return "storage_delete";
  catalog.erase(findSlot(slot));
  publishCatalog();
  autoSleep.activity(millis());
  resetSlideshowTimer();
  if (current != static_cast<int>(slot)) {
    if (menuOpen) drawMenu();
    return nullptr;
  }
  current = -1;
  currentCrc = 0;
  animation.invalidate(brightness);
  if (!rememberCurrent(-1)) return "preferences_write";
  const bool wasSleeping = sleeping;
  const char *error = nullptr;
  if (!catalog.empty()) {
    auto entry = findSlot(slot);
    if (entry == catalog.end()) entry = catalog.begin();
    error = showPicture(entry->slot);
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
  } else if (count == 1 && strcmp(tokens[0], "LIST") == 0) {
    sendCatalog();
  } else if (count == 1 && strcmp(tokens[0], "NEXT") == 0) {
    reply(nextPicture());
  } else if (count == 1 && strcmp(tokens[0], "PREVIOUS") == 0) {
    reply(previousPicture());
  } else if (count == 2 && strcmp(tokens[0], "SHUFFLE") == 0 && unsignedArgument(tokens[1], 1, n)) {
    reply(setShuffle(n != 0));
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
    if (!unsignedArgument(tokens[1], 3, n) || !unsignedArgument(tokens[2], 3, motion) ||
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
  } else if (count == 2 && unsignedArgument(tokens[1], kMaxSlot, n)) {
    if (strcmp(tokens[0], "SHOW") == 0) reply(showPicture(n, true, true, true));
    else if (strcmp(tokens[0], "DELETE") == 0) reply(deletePicture(n));
    else if (strcmp(tokens[0], "GET") == 0) sendPicture(n);
    else reply("unknown_command");
  } else if (count == 4 && strcmp(tokens[0], "PUT") == 0) {
    uint32_t length = 0, crc = 0;
    const bool automatic = strcmp(tokens[1], "AUTO") == 0;
    if ((!automatic && !unsignedArgument(tokens[1], kMaxSlot, n)) ||
        !unsignedArgument(tokens[2], UINT32_MAX, length) || length < BadgeJpeg::kPrefixBytes + 2 ||
        !unsignedArgument(tokens[3], UINT32_MAX, crc)) return reply("invalid_upload_arguments");
    if (automatic) { if (nextSlotId < 0) return reply("slot_ids_exhausted"); n = nextSlotId; }
    const char *error = receivePicture(n, length, crc);
    if (menuOpen) drawMenu();
    clearGesture();
    resetSlideshowTimer();
    autoSleep.activity(millis());
    if (error) reply(error); else Serial.printf("OK %lu\n", static_cast<unsigned long>(n));
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

void applyGesture(GestureAction action, int capturedButton = -1) {
  switch (action) {
    case GestureAction::Next: nextPicture(); break;
    case GestureAction::Previous: previousPicture(); break;
    case GestureAction::OpenMenu: setMenu(true); break;
    case GestureAction::CloseMenu: setMenu(false); break;
    case GestureAction::Back: menuAction("back"); break;
    case GestureAction::Sleep: setSleeping(true); break;
    case GestureAction::Button: {
      size_t count;
      const MenuButton *buttons = menuButtons(count);
      if (capturedButton >= 0 && static_cast<size_t>(capturedButton) < count)
        menuAction(buttons[capturedButton].action);
      break;
    }
    default: break;
  }
}

void pollControls() {
  const uint32_t now = millis();
  static uint32_t lastSleepTouchPoll = 0;
  if (touchOk) AutoSleep::pollTouchRelease(
      sleeping || pendingPowerOff != PowerOffReason::None, suppressTouch, now, lastSleepTouchPoll, [] {
    noInterrupts();
    touchInterrupt = false;
    interrupts();
    return touch.confirmedRelease();
  });
  if (touchOk && touchInterrupt && !(sleeping && suppressTouch)) {
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
        gesture.begin(fingerLastX, fingerLastY, now, menuOpen,
                      menuOpen ? hitMenuButton(fingerLastX, fingerLastY) : -1, sleeping);
        if (sleeping) setSleeping(false);
      } else {
        gesture.move(fingerLastX, fingerLastY);
      }
    } else if (fingerDown) {
      // CST92xx release coordinates are not mirrored: use the last pressed
      // coordinate, but require the same captured control at both endpoints.
      const GestureAction action = gesture.release(now, menuOpen ? hitMenuButton(fingerLastX, fingerLastY) : -1);
      const int captured = gesture.button();
      fingerDown = false;
      applyGesture(action, captured);
    }
  }
  if (fingerDown) applyGesture(gesture.hold(now));
  if (fingerDown && now - fingerLastEvent > 2500) {
    fingerDown = false;
    gesture.cancel();
  }
  if (suppressTouch && !sleeping && pendingPowerOff == PowerOffReason::None &&
      now - touchSuppressedAt > 2500) suppressTouch = false;

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
    const bool keyPressEdge = power.isPekeyNegativeIrq();
    const bool keyReleaseEdge = power.isPekeyPositiveIrq();
    power.clearIrqStatus();
    if (keyPressEdge && !keyReleaseEdge) pmicPowerKeyActive = true;
    if (keyReleaseEdge) pmicPowerKeyActive = false;
    if (shortPress) setSleeping(!sleeping);
  }
}

void initStorage() {
  storageOk = LittleFS.begin(false, "/badge", 4, "badge");
  if (!storageOk) return; // Existing data is never automatically formatted.
  LittleFS.remove(kTemporary);
  catalogRevision = esp_random();
  File directory = LittleFS.open("/");
  if (!directory || !directory.isDirectory()) { storageOk = false; return; }
  for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
    uint32_t slot, storedCrc = 0, transferCrc = 0;
    if (!file.isDirectory() && BadgeJpeg::slotName(file.name(), slot) &&
        !inspectJpeg(file, storedCrc, transferCrc)) {
      catalog.push_back({static_cast<int32_t>(slot), static_cast<uint32_t>(file.size()), storedCrc, transferCrc});
    }
    file.close(); delay(0); tickBootAnimation();
  }
  std::sort(catalog.begin(), catalog.end(), [](const SlotEntry& left, const SlotEntry& right) { return left.slot < right.slot; });
  publishCatalog();
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
    if ((savedAnimation & 3U) < 4U && period >= 8U && period <= 120U) {
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
    pmicPowerOffReady = PmicPowerOff::configureOnLevel(
        power, XPOWERS_AXP2101_IRQ_OFF_ON_LEVEL_CTRL, XPOWERS_POWERON_2S);
    // Keep factory rail/charging settings; only enable measurement and key IRQ.
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    pmicPowerOffReady = power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                                        XPOWERS_AXP2101_PKEY_POSITIVE_IRQ |
                                        XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ) &&
                        pmicPowerOffReady;
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
    resetTouchReports();
  }
  displayOk = gfx.begin(40000000);
  if (displayOk) { gfx.setBrightness(brightness); enablePanelSync(); delay(50); }
  if (displayOk && frame) {
    bootAnimation.start(millis(), reinterpret_cast<uint16_t *>(frame));
    gfx.draw16bitRGBBitmap(0, 0, reinterpret_cast<uint16_t *>(frame), LCD_WIDTH, LCD_HEIGHT);
  }
  initStorage();
  player.reset(preferencesOk && preferences.getBool("shuffle", false), -1, catalogIds.data(), catalogIds.size(), esp_random());
  bool shown = false;
  const int remembered = preferencesOk ? preferences.getInt("current", -1) : -1;
  if (remembered >= 0 && hasSlot(remembered)) {
    shown = showPicture(remembered) == nullptr;
  }
  if (!shown) {
    for (const auto& entry : catalog) {
      if ((shown = showPicture(entry.slot) == nullptr)) break;
    }
  }
  bootAnimation.stop();
  if (!shown) drawWelcome();
  resetSlideshowTimer();
  autoSleep.activity(millis());
}

void loop() {
  pollSerial();
  executePmicPowerOff();
  pollControls();
  executePmicPowerOff();
  pollAutoSleep();
  executePmicPowerOff();
  pollSlideshow();
  refreshGravity();
  animation.poll(!sleeping && !menuOpen && current >= 0 && !commandLength && !fingerDown, brightness);
  delay(5);
}
