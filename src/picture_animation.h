#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "display/Arduino_CO5300.h"
#include "animation_renderer.h"
#include "badge_qspi.h"

struct PanelScan {
  volatile uint32_t risingUs = 0;
  volatile uint32_t fallingUs = 0;
  volatile uint32_t periodUs = 0;
  volatile uint32_t scans = 0;
};

// Two immutable image buffers plus two output buffers. USB/menu scratch is
// separate: GET/failed PUT/menu drawing cannot change the rotating source.
class PictureAnimation {
 public:
  PictureAnimation(Arduino_CO5300 &display, BadgeQSPI &bus, PanelScan &scan)
      : display_(display), bus_(bus), scan_(scan) {}

  bool begin() {
    badge_animation::initialize();
    for (auto &image : images_) {
      image = static_cast<uint16_t *>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!image) return false;
    }
    for (auto &output : outputs_) {
      output = static_cast<uint16_t *>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!output) return false;
    }
    ready_ = true;
    pauseClock();
    return true;
  }

  uint16_t *nextSource() { return ready_ ? images_[imageIndex_ ^ 1U] : nullptr; }
  const uint16_t *snapshot() const { return hasFrame_ ? outputs_[front_] : nullptr; }
  bool active() const { return hasImage_ && !*error && (phase_ != Idle || motion != 0); }
  bool transitioning() const { return phase_ != Idle; }
  uint16_t angle() const {
    return motion == 3 ? presentedGravityAngle_ : motion == 2 ? static_cast<uint16_t>(motionElapsed_ * 36U / rotationPeriod) : 0;
  }

  void gravityAngle(uint16_t value) { gravityAngle_ = value; }

  void configure(uint8_t newTransition, uint8_t newMotion, uint16_t period, uint8_t brightness) {
    cancel(brightness);
    transition = newTransition;
    motion = newMotion;
    rotationPeriod = period;
    motionElapsed_ = 0;
    error = "";
    pauseClock();
  }

  void cancel(uint8_t brightness) {
    if (phase_ == FadeOut || phase_ == FadeIn) display_.setBrightness(brightness);
    phase_ = Idle;
    pauseClock();
  }

  void invalidate(uint8_t brightness) {
    cancel(brightness);
    hasImage_ = hasFrame_ = false;
    error = "";
  }

  void pauseClock() { lastTick_ = millis(); }

  // Stage without discarding the old image/pose. The caller commits only after
  // its preference write succeeds; neither immediate failure nor rejected NVS
  // updates require an additional image buffer.
  const char *prepareImage(bool animate, uint8_t brightness, bool displayNow = true) {
    if (!ready_) return "psram_unavailable";
    const bool previous = hasImage_ && hasFrame_;
    cancel(brightness);
    selection_ = {imageIndex_, front_, shiftPose_, motionElapsed_,
                  presentedGravityAngle_, hasImage_, hasFrame_};
    selectionPending_ = true;
    selectionPhase_ = Idle;
    if (animate && previous && displayNow) {
      switch (transition) {
        case 1: selectionPhase_ = FadeOut; break;
        case 2: selectionPhase_ = Slide; break;
        case 3: selectionPhase_ = Ripple; break;
        default: break;
      }
    }
    imageIndex_ ^= 1U;
    hasImage_ = true;
    motionElapsed_ = 0;
    error = "";
    started_ = millis();
    lastFrame_ = started_ - kFrameInterval;
    if (!displayNow) { hasFrame_ = false; return nullptr; }
    if (selectionPhase_ != Idle) return nullptr;
    const char *failure = presentCurrent();
    if (failure) rejectImage(true);
    return failure;
  }

  void commitImage() {
    if (!selectionPending_) return;
    // This is the asynchronous acceptance boundary. Later DMA errors do not
    // undo an accepted player step. Slide freezes the outgoing presented pose.
    if (selectionPhase_ == Slide || selectionPhase_ == Ripple)
      memcpy(images_[selection_.image], outputs_[selection_.front], kBytes);
    phase_ = selectionPhase_;
    started_ = millis();
    pauseClock();
    selectionPending_ = false;
  }

  void rejectImage(bool restorePanel) {
    if (!selectionPending_) return;
    imageIndex_ = selection_.image;
    front_ = selection_.front;
    shiftPose_ = selection_.shift;
    motionElapsed_ = selection_.motion;
    presentedGravityAngle_ = selection_.gravity;
    hasImage_ = selection_.hasImage;
    hasFrame_ = selection_.hasFrame;
    phase_ = Idle;
    selectionPending_ = false;
    if (restorePanel && hasFrame_)
      display_.draw16bitRGBBitmap(0, 0, outputs_[front_], 466, 466);
    pauseClock();
  }

  const char *presentCurrent() {
    if (!ready_ || !hasImage_) return "no_pictures";
    const uint32_t started = micros();
    renderCurrent(outputs_[front_ ^ 1U]);
    // A static image remains usable if TE is missing; continuous motion reports
    // the synchronization failure rather than repeatedly tearing the panel.
    const bool synced = validScan();
    if (!present(synced)) return error;
    frameMs = (micros() - started + 999U) / 1000U;
    lastFrame_ = millis();
    pauseClock();
    return nullptr;
  }

  void poll(bool enabled, uint8_t brightness) {
    const uint32_t now = millis();
    const uint32_t elapsed = now - lastTick_;
    lastTick_ = now;
    if (!enabled || !ready_ || !hasImage_ || *error) return;
    if (phase_ == FadeOut || phase_ == FadeIn) {
      const uint32_t duration = phase_ == FadeOut ? 130U : 170U;
      const uint32_t time = now - started_;
      const uint32_t value = ease(time < duration ? time * 255U / duration : 255U);
      display_.setBrightness(phase_ == FadeOut ? brightness * (255U - value) / 255U : brightness * value / 255U);
      if (time >= duration) {
        if (phase_ == FadeOut) {
          const uint32_t rendering = micros();
          renderCurrent(outputs_[front_ ^ 1U]);
          if (!present(false)) { cancel(brightness); return; }
          frameMs = (micros() - rendering + 999U) / 1000U;
          phase_ = FadeIn;
          started_ = millis();
        } else {
          phase_ = Idle;
          display_.setBrightness(brightness);
          lastFrame_ = now;
        }
      }
      return;
    }
    if (phase_ == Slide) {
      if (now - lastFrame_ < (motion == 3 ? kFrameInterval : 35U)) return;
      const uint32_t time = now - started_;
      const unsigned offset = time >= 300U ? 466U : ease(time * 255U / 300U) * 466U / 255U;
      const uint32_t rendering = micros();
      // Sample the incoming canonical source directly at its gravity pose.
      // Never use the published front buffer as unsubmitted render scratch.
      badge_animation::renderSlide(images_[imageIndex_ ^ 1U], images_[imageIndex_],
                                  outputs_[front_ ^ 1U], offset, motion == 3, gravityAngle_);
      if (!present(true)) { cancel(brightness); return; }
      frameMs = (micros() - rendering + 999U) / 1000U;
      lastFrame_ = now;
      if (offset == 466U) { phase_ = Idle; pauseClock(); }
      return;
    }
    if (phase_ == Ripple) {
      constexpr uint32_t duration = 400U;
      const uint32_t time = now - started_;
      if (now - lastFrame_ < kFrameInterval && time < duration) return;
      const uint8_t progress = time >= duration ? 255U : static_cast<uint8_t>(time * 255U / duration);
      const uint32_t rendering = micros();
      badge_animation::renderRipple(images_[imageIndex_ ^ 1U], images_[imageIndex_],
                                    outputs_[front_ ^ 1U], progress);
      if (!present(true)) { cancel(brightness); return; }
      frameMs = (micros() - rendering + 999U) / 1000U;
      lastFrame_ = now;
      if (progress == 255U) { phase_ = Idle; pauseClock(); }
      return;
    }
    if (!motion) return;
    if (motion != 3) {
      const uint32_t period = motion == 2 ? rotationPeriod * 1000U : 60000U;
      motionElapsed_ = (motionElapsed_ + elapsed) % period;
    }
    if (now - lastFrame_ < kFrameInterval) return;
    if (motion == 3 && hasFrame_ && gravityAngle_ == presentedGravityAngle_) return;
    if (motion == 1) {
      const unsigned pose = motionElapsed_ / 7500U;
      if (pose == shiftPose_) return;
      shiftPose_ = pose;
    }
    const uint32_t rendering = micros();
    renderCurrent(outputs_[front_ ^ 1U]);
    if (!present(true)) return;
    frameMs = (micros() - rendering + 999U) / 1000U;
    lastFrame_ = now;
  }

  uint8_t transition = 1; // fade through black
  uint8_t motion = 2;     // clockwise rotation
  uint16_t rotationPeriod = 24;
  uint32_t frameMs = 0;
  uint32_t transferUs = 0;
  const char *error = "";

 private:
  enum Phase { Idle, FadeOut, FadeIn, Slide, Ripple };
  static constexpr size_t kBytes = 466U * 466U * sizeof(uint16_t);
  static constexpr uint32_t kFrameInterval = 100; // Target 10fps; late frames are skipped, never queued.
  // Payload floor at 4-bit 40MHz. Never exceed CO5300's documented 50MHz limit.
  static constexpr uint32_t kWireUs = (kBytes + 19U) / 20U;
  static uint32_t ease(uint32_t x) { return (x * x * (765U - 2U * x) + 32512U) / 65025U; }

  bool validScan() const {
    const uint32_t period = scan_.periodUs;
    return period >= 5000U && period <= 50000U && micros() - scan_.fallingUs < period * 3U;
  }

  void renderCurrent(uint16_t *output) {
    if (motion == 2 || motion == 3) {
      badge_animation::renderRotation(images_[imageIndex_], output, motion == 3 ? gravityAngle_ : angle());
    } else if (motion == 1) {
      static constexpr int8_t positions[8][2] = {{0, 0}, {1, 0}, {2, 1}, {1, 2}, {0, 1}, {-1, 0}, {-2, -1}, {-1, -2}};
      const unsigned pose = motionElapsed_ / 7500U;
      shiftPose_ = pose;
      badge_animation::renderShift(images_[imageIndex_], output, positions[pose][0], positions[pose][1]);
    } else {
      badge_animation::renderShift(images_[imageIndex_], output, 0, 0);
    }
  }

  bool present(bool synchronize) {
    uint32_t phaseUs = 0;
    uint32_t period = 0;
    if (synchronize) {
      if (!validScan()) { error = "display_sync_unavailable"; return false; }
      period = scan_.periodUs;
      // Write behind the current scan, ahead of its next pass. A slower-than-
      // scan transfer needs >0 phase; a faster transfer needs period-wire time.
      phaseUs = period > kWireUs ? period - kWireUs + 700U : 700U;
      const uint32_t expected = transferUs ? transferUs : kWireUs + 3000U;
      if (phaseUs + expected + 1500U >= period * 2U) { error = "transfer_exceeds_scan_window"; return false; }
      const uint32_t scanCount = scan_.scans;
      const uint32_t waiting = micros();
      while (scan_.scans == scanCount) {
        if (micros() - waiting >= period * 3U) { error = "display_sync_timeout"; return false; }
        delay(1);
      }
      const uint32_t falling = scan_.fallingUs;
      const uint32_t since = micros() - falling;
      if (since < phaseUs) delayMicroseconds(phaseUs - since);
      phaseUs = micros() - falling;
      if (phaseUs + expected + 1000U >= period * 2U) { error = "display_sync_late"; return false; }
    }
    const uint32_t sending = micros();
    display_.draw16bitRGBBitmap(0, 0, outputs_[front_ ^ 1U], 466, 466);
    transferUs = micros() - sending;
    if (!bus_.pixelOk) { error = "display_transfer"; return false; }
    front_ ^= 1U;
    presentedGravityAngle_ = gravityAngle_;
    hasFrame_ = true;
    if (synchronize && phaseUs + transferUs + 1000U >= period * 2U) {
      error = "transfer_exceeds_scan_window";
      return false;
    }
    return true;
  }

  Arduino_CO5300 &display_;
  BadgeQSPI &bus_;
  PanelScan &scan_;
  uint16_t *images_[2] = {};
  uint16_t *outputs_[2] = {};
  unsigned imageIndex_ = 0, front_ = 0, shiftPose_ = 0;
  bool ready_ = false, hasImage_ = false, hasFrame_ = false;
  Phase phase_ = Idle;
  uint32_t started_ = 0, lastFrame_ = 0, lastTick_ = 0, motionElapsed_ = 0;
  uint16_t gravityAngle_ = 0, presentedGravityAngle_ = 0;
  struct SelectionState {
    unsigned image, front, shift;
    uint32_t motion;
    uint16_t gravity;
    bool hasImage, hasFrame;
  } selection_ = {};
  bool selectionPending_ = false;
  Phase selectionPhase_ = Idle;
};
