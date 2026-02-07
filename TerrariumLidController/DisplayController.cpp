#include "DisplayController.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace {
void copyTrunc21(char* out, const char* in) {
  size_t i = 0;
  while (i < 21 && in[i] != '\0') {
    out[i] = in[i];
    ++i;
  }
  out[i] = '\0';
}
}

DisplayController::DisplayController()
    : oled_(nullptr),
      wire_(nullptr),
      logStream_(nullptr),
      present_(false),
      enabled_(true),
      dimmed_(false),
      flipped_(DISPLAY_ROTATION_DEFAULT == 2),
      address_(0),
      lastRetryMs_(0),
      lastRenderMs_(0),
      warnedMissing_(false),
      lastUiHash_(0),
      prefsOpened_(false),
      powerMode_(PowerMode::Auto),
      dimTimeoutMin_(2),
      offTimeoutMin_(5),
      dimTimeoutMs_(2UL * 60UL * 1000UL),
      offTimeoutMs_(5UL * 60UL * 1000UL),
      lastActivityMs_(0),
      lastPotRaw_(0),
      havePotSample_(false),
      timeoutDimActive_(false),
      timeoutOffActive_(false),
      lastPixelShiftMs_(0),
      pixelShiftX_(0),
      pixelShiftPhase_(0),
      pixelShiftDirty_(false) {}

DisplayController::~DisplayController() {
  if (oled_ != nullptr) {
    delete oled_;
    oled_ = nullptr;
  }
  if (prefsOpened_) {
    prefs_.end();
    prefsOpened_ = false;
  }
}

bool DisplayController::begin(TwoWire& wire) {
  wire_ = &wire;
  warnedMissing_ = false;
  lastRetryMs_ = 0;
  lastActivityMs_ = millis();
  loadFlipFromNvs();
  return tryDetect(true);
}

void DisplayController::update(const UiState& state, unsigned long nowMs) {
  const bool hasAlert = state.needsWatering || state.tooCold || state.tooHot || !state.rtcValid || state.usbPowerLimited;
  if (!havePotSample_) {
    lastPotRaw_ = state.rawPot;
    havePotSample_ = true;
  } else if (abs(state.rawPot - lastPotRaw_) >= 12) {
    lastActivityMs_ = nowMs;
    lastPotRaw_ = state.rawPot;
    if (powerMode_ == PowerMode::Auto && (timeoutDimActive_ || timeoutOffActive_)) {
      timeoutDimActive_ = false;
      timeoutOffActive_ = false;
      setEnabled(true);
      setDimMode(false);
    }
  }

  if (powerMode_ == PowerMode::Auto) {
    if (hasAlert) {
      timeoutDimActive_ = false;
      timeoutOffActive_ = false;
      setEnabled(true);
      setDimMode(false);
    } else {
      const unsigned long idleMs = nowMs - lastActivityMs_;
      if (offTimeoutMs_ > 0 && idleMs >= offTimeoutMs_) {
        timeoutOffActive_ = true;
        timeoutDimActive_ = false;
        setEnabled(false);
        setDimMode(false);
      } else if (dimTimeoutMs_ > 0 && idleMs >= dimTimeoutMs_) {
        timeoutDimActive_ = true;
        timeoutOffActive_ = false;
        setEnabled(true);
        setDimMode(true);
      } else {
        if (timeoutDimActive_ || timeoutOffActive_) {
          timeoutDimActive_ = false;
          timeoutOffActive_ = false;
          setEnabled(true);
          setDimMode(false);
        }
      }
    }
  } else if (hasAlert) {
    setEnabled(true);
    setDimMode(false);
  } else if (powerMode_ == PowerMode::ForcedDim) {
    setEnabled(true);
    setDimMode(true);
  } else {
    setEnabled(false);
    setDimMode(false);
  }

  if (present_) {
    updatePixelShift(nowMs);
    if (!enabled_ || oled_ == nullptr) {
      return;
    }
    if (!shouldRender(state, nowMs)) {
      return;
    }
    renderFrame(state);
    oled_->display();
    lastRenderMs_ = nowMs;
    lastUiHash_ = computeUiHash(state);
    return;
  }
  if (lastRetryMs_ != 0 && (nowMs - lastRetryMs_) < kRetryIntervalMs) {
    return;
  }
  lastRetryMs_ = nowMs;
  tryDetect(false);
}

bool DisplayController::isPresent() const {
  return present_;
}

void DisplayController::setEnabled(bool enabled) {
  enabled_ = enabled;
  if (!present_) {
    return;
  }
  if (enabled_) {
    oled_->ssd1306_command(SSD1306_DISPLAYON);
  } else {
    oled_->ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

void DisplayController::setDimMode(bool dimmed) {
  dimmed_ = dimmed;
  if (!present_) {
    return;
  }
  oled_->dim(dimmed_);
}

void DisplayController::setFlip(bool flipped) {
  if (flipped_ != flipped) {
    flipped_ = flipped;
    saveFlipToNvs();
  }
  if (!present_) {
    return;
  }
  oled_->setRotation(flipped_ ? 2 : 0);
}

void DisplayController::toggleFlip() {
  setFlip(!flipped_);
}

DisplayController::Status DisplayController::getStatus() const {
  return Status{present_, enabled_, dimmed_, flipped_, address_, powerMode_, dimTimeoutMin_, offTimeoutMin_};
}

void DisplayController::setLogStream(Stream& stream) {
  logStream_ = &stream;
}

bool DisplayController::tryDetectAt(uint8_t address) {
  if (wire_ == nullptr) {
    return false;
  }

  Adafruit_SSD1306* probe = new Adafruit_SSD1306(DISPLAY_ACTIVE_WIDTH, DISPLAY_ACTIVE_HEIGHT, wire_, -1);
  if (!probe->begin(SSD1306_SWITCHCAPVCC, address, false, false)) {
    delete probe;
    return false;
  }

  if (oled_ != nullptr) {
    delete oled_;
  }
  oled_ = probe;
  oled_->clearDisplay();
  oled_->display();
  lastRenderMs_ = 0;
  lastUiHash_ = 0;
  return true;
}

bool DisplayController::tryDetect(bool logWarning) {
  present_ = false;
  address_ = 0;

  if (tryDetectAt(DISPLAY_I2C_ADDR_PRIMARY)) {
    present_ = true;
    address_ = DISPLAY_I2C_ADDR_PRIMARY;
  } else if (tryDetectAt(DISPLAY_I2C_ADDR_FALLBACK)) {
    present_ = true;
    address_ = DISPLAY_I2C_ADDR_FALLBACK;
  }

  if (present_) {
    warnedMissing_ = false;
    setFlip(flipped_);
    setDimMode(dimmed_);
    setEnabled(enabled_);
    if (logStream_ != nullptr) {
      logStream_->print("SSD1306 detected at 0x");
      if (address_ < 16) logStream_->print("0");
      logStream_->println(address_, HEX);
    }
    return true;
  }

  if (logWarning && !warnedMissing_ && logStream_ != nullptr) {
    logStream_->println("SSD1306 not detected. Running headless.");
    warnedMissing_ = true;
  }
  return false;
}

void DisplayController::loadFlipFromNvs() {
  if (!prefsOpened_) {
    prefsOpened_ = prefs_.begin("ui", false);
  }
  if (!prefsOpened_) {
    return;
  }
  uint8_t value = prefs_.getUChar("oled_flip", flipped_ ? 1 : 0);
  flipped_ = (value != 0);
}

void DisplayController::saveFlipToNvs() {
  if (!prefsOpened_) {
    prefsOpened_ = prefs_.begin("ui", false);
  }
  if (!prefsOpened_) {
    return;
  }
  prefs_.putUChar("oled_flip", flipped_ ? 1 : 0);
}

void DisplayController::setPowerMode(PowerMode mode) {
  powerMode_ = mode;
  if (powerMode_ == PowerMode::Auto) {
    timeoutDimActive_ = false;
    timeoutOffActive_ = false;
    lastActivityMs_ = millis();
    setEnabled(true);
    setDimMode(false);
  } else if (powerMode_ == PowerMode::ForcedDim) {
    setEnabled(true);
    setDimMode(true);
  } else {
    setEnabled(false);
    setDimMode(false);
  }
}

void DisplayController::setTimeoutDimMinutes(uint16_t minutes) {
  dimTimeoutMin_ = minutes;
  dimTimeoutMs_ = static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

void DisplayController::setTimeoutOffMinutes(uint16_t minutes) {
  offTimeoutMin_ = minutes;
  offTimeoutMs_ = static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

void DisplayController::runFactoryTest(Stream& serial, unsigned long durationMs, void (*pwmWrite)(int), int maxDuty) {
  if (!present_) {
    tryDetect(true);
  }
  if (!present_ || oled_ == nullptr) {
    serial.println("Display test: SSD1306 not detected.");
    return;
  }

  const PowerMode prevMode = powerMode_;
  setPowerMode(PowerMode::Auto);
  setEnabled(true);
  setDimMode(false);

  serial.print("Display test: SSD1306 at 0x");
  if (address_ < 16) serial.print("0");
  serial.println(address_, HEX);

  const unsigned long startMs = millis();
  unsigned long frames = 0;
  unsigned long maxFrameUs = 0;
  unsigned long lastPwmToggleMs = startMs;
  bool pwmHigh = false;

  while ((millis() - startMs) < durationMs) {
    const unsigned long nowMs = millis();
    const unsigned long t0 = micros();

    oled_->clearDisplay();
    for (int x = 0; x < DISPLAY_ACTIVE_WIDTH; x += 8) {
      if (((x / 8) + (nowMs / 250)) % 2 == 0) {
        oled_->drawFastVLine(x, 0, DISPLAY_ACTIVE_HEIGHT, SSD1306_WHITE);
      }
    }
    oled_->drawRect(0, 0, DISPLAY_ACTIVE_WIDTH, DISPLAY_ACTIVE_HEIGHT, SSD1306_WHITE);
    oled_->setTextSize(1);
    oled_->setTextColor(SSD1306_WHITE);
    oled_->setCursor(4, 4);
    oled_->print("OLED FACTORY TEST");
    oled_->setCursor(4, 16);
    oled_->print("addr 0x");
    if (address_ < 16) oled_->print("0");
    oled_->print(address_, HEX);
    oled_->setCursor(4, 28);
    oled_->print("sec ");
    oled_->print((nowMs - startMs) / 1000UL);
    oled_->setCursor(4, 40);
    oled_->print("pwm ");
    oled_->print(pwmHigh ? "HIGH" : "LOW");
    oled_->display();

    const unsigned long frameUs = micros() - t0;
    if (frameUs > maxFrameUs) {
      maxFrameUs = frameUs;
    }
    frames++;

    if (pwmWrite != nullptr && (nowMs - lastPwmToggleMs) >= 500) {
      lastPwmToggleMs = nowMs;
      pwmHigh = !pwmHigh;
      pwmWrite(pwmHigh ? (maxDuty / 2) : (maxDuty / 4));
    }

    delay(40);
  }

  if (pwmWrite != nullptr) {
    pwmWrite(0);
  }
  setPowerMode(prevMode);
  serial.print("Display test done: frames=");
  serial.print(frames);
  serial.print(" maxFrameUs=");
  serial.println(maxFrameUs);
}

void DisplayController::updatePixelShift(unsigned long nowMs) {
  if (lastPixelShiftMs_ != 0 && (nowMs - lastPixelShiftMs_) < kPixelShiftIntervalMs) {
    return;
  }
  lastPixelShiftMs_ = nowMs;

  // Shift only blue-zone content by -1/0/+1 px on X.
  // Blue content uses a +1 base margin so -1 does not clip left edge.
  pixelShiftPhase_ = static_cast<uint8_t>((pixelShiftPhase_ + 1) % 3);
  if (pixelShiftPhase_ == 0) pixelShiftX_ = -1;
  if (pixelShiftPhase_ == 1) pixelShiftX_ = 0;
  if (pixelShiftPhase_ == 2) pixelShiftX_ = 1;
  pixelShiftDirty_ = true;
}

bool DisplayController::shouldRender(const UiState& state, unsigned long nowMs) {
  if (lastRenderMs_ != 0 && (nowMs - lastRenderMs_) < DISPLAY_REFRESH_INTERVAL_MS) {
    return false;
  }
  if (pixelShiftDirty_) {
    return true;
  }
  return computeUiHash(state) != lastUiHash_;
}

uint32_t DisplayController::computeUiHash(const UiState& state) const {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t v) {
    h ^= v;
    h *= 16777619u;
  };

  mix(static_cast<uint32_t>(state.rtcNow.hour()));
  mix(static_cast<uint32_t>(state.rtcNow.minute()));
  mix(static_cast<uint32_t>(state.brightnessPercent));
  mix(static_cast<uint32_t>(state.duty));
  mix(static_cast<uint32_t>(state.lightOn));
  mix(static_cast<uint32_t>(state.scheduleAllowed));
  mix(static_cast<uint32_t>(state.forceOn));
  mix(static_cast<uint32_t>(state.controlMode));
  mix(static_cast<uint32_t>(state.needsWatering));
  mix(static_cast<uint32_t>(state.tooCold));
  mix(static_cast<uint32_t>(state.tooHot));
  mix(static_cast<uint32_t>(state.usbPowerLimited));
  for (size_t i = 0; i < sizeof(state.nextEvent) && state.nextEvent[i] != '\0'; ++i) {
    mix(static_cast<uint8_t>(state.nextEvent[i]));
  }
  return h;
}

void DisplayController::renderFrame(const UiState& state) {
  oled_->clearDisplay();
  drawTopYellowZone(state);
  drawBlueZone(state);
  pixelShiftDirty_ = false;
}

void DisplayController::drawTopYellowZone(const UiState& state) {
  // Top bar contract: three 8x8 icons and right-aligned HH:MM.
  oled_->drawRect(0, 0, 8, 8, SSD1306_WHITE);   // USB icon placeholder
  oled_->drawRect(10, 0, 8, 8, SSD1306_WHITE);  // RTC icon placeholder
  oled_->drawRect(20, 0, 8, 8, SSD1306_WHITE);  // Mode icon placeholder

  char timeBuf[6];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", state.rtcNow.hour(), state.rtcNow.minute());
  oled_->setTextSize(1);
  oled_->setTextColor(SSD1306_WHITE);
  oled_->setCursor(98, 0);
  oled_->print(timeBuf);
}

void DisplayController::drawBlueZone(const UiState& state) {
  // Blue zone: primary value, bar, and three status lines.
  const int sx = pixelShiftX_;
  oled_->setTextSize(2);  // Approximates 8x16 primary font.
  oled_->setTextColor(SSD1306_WHITE);
  oled_->setCursor(1 + sx, 12);
  char brightnessBuf[6];
  snprintf(brightnessBuf, sizeof(brightnessBuf), "%3d%%", state.brightnessPercent);
  oled_->print(brightnessBuf);

  int barX = 56;  // Layout contract.
  int barY = 14;
  int barW = 68;
  int barH = 12;
  oled_->drawRect(barX + sx, barY, barW, barH, SSD1306_WHITE);
  int fill = (66 * state.brightnessPercent + 50) / 100;  // round(66 * pct / 100)
  if (fill > 66) fill = 66;
  if (fill < 0) fill = 0;
  if (fill > 0) {
    oled_->fillRect(57 + sx, 15, fill, 10, SSD1306_WHITE);
  }

  char lineBuf[22];
  oled_->setTextSize(1);
  snprintf(lineBuf, sizeof(lineBuf), "%s %s", state.lightOn ? "ON " : "OFF", modeText(state.controlMode));
  if (state.lightOn) {
    lineBuf[2] = ' ';
    lineBuf[3] = ' ';
  }
  oled_->setCursor(1 + sx, 34);
  oled_->print(lineBuf);

  copyTrunc21(lineBuf, state.nextEvent);
  oled_->setCursor(1 + sx, 44);
  oled_->print(lineBuf);

  const char* banner = "OK";
  if (state.needsWatering) banner = "NEEDS WATERING";
  if (state.tooCold) banner = "TOO COLD";
  if (state.tooHot) banner = "TOO HOT";
  if (!state.rtcValid) banner = "RTC MISSING";
  if (state.usbPowerLimited) banner = "USB POWER LIMITED";
  copyTrunc21(lineBuf, banner);
  oled_->setCursor(1 + sx, 54);
  oled_->print(lineBuf);
}

const char* DisplayController::modeText(ControlMode mode) const {
  if (mode == ControlMode::Override) return "OVR";
  if (mode == ControlMode::Schedule) return "SCH";
  return "POT";
}
