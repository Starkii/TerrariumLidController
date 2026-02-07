#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include "DisplayConfig.h"
#include "UiState.h"

class DisplayController {
 public:
  enum class PowerMode : uint8_t {
    Auto = 0,
    ForcedDim = 1,
    ForcedOff = 2,
  };

  struct Status {
    bool present;
    bool enabled;
    bool dimmed;
    bool flipped;
    uint8_t address;
    PowerMode powerMode;
    uint16_t dimTimeoutMin;
    uint16_t offTimeoutMin;
  };

  DisplayController();
  ~DisplayController();

  bool begin(TwoWire& wire);
  void update(const UiState& state, unsigned long nowMs);
  bool isPresent() const;
  void setEnabled(bool enabled);
  void setDimMode(bool dimmed);
  void setFlip(bool flipped);
  void toggleFlip();
  void setPowerMode(PowerMode mode);
  void setTimeoutDimMinutes(uint16_t minutes);
  void setTimeoutOffMinutes(uint16_t minutes);
  void runFactoryTest(Stream& serial, unsigned long durationMs, void (*pwmWrite)(int), int maxDuty);
  Status getStatus() const;
  void setLogStream(Stream& stream);

 private:
  static constexpr unsigned long kRetryIntervalMs = 60000;
  static constexpr unsigned long kPixelShiftIntervalMs = 45000;

  bool tryDetectAt(uint8_t address);
  bool tryDetect(bool logWarning);
  void updatePixelShift(unsigned long nowMs);
  void loadFlipFromNvs();
  void saveFlipToNvs();
  bool shouldRender(const UiState& state, unsigned long nowMs);
  uint32_t computeUiHash(const UiState& state) const;
  void renderFrame(const UiState& state);
  void drawTopYellowZone(const UiState& state);
  void drawBlueZone(const UiState& state);
  const char* modeText(ControlMode mode) const;

  Adafruit_SSD1306* oled_;
  TwoWire* wire_;
  Stream* logStream_;
  bool present_;
  bool enabled_;
  bool dimmed_;
  bool flipped_;
  uint8_t address_;
  unsigned long lastRetryMs_;
  unsigned long lastRenderMs_;
  bool warnedMissing_;
  uint32_t lastUiHash_;
  Preferences prefs_;
  bool prefsOpened_;
  PowerMode powerMode_;
  uint16_t dimTimeoutMin_;
  uint16_t offTimeoutMin_;
  unsigned long dimTimeoutMs_;
  unsigned long offTimeoutMs_;
  unsigned long lastActivityMs_;
  int lastPotRaw_;
  bool havePotSample_;
  bool timeoutDimActive_;
  bool timeoutOffActive_;
  unsigned long lastPixelShiftMs_;
  int8_t pixelShiftX_;
  uint8_t pixelShiftPhase_;
  bool pixelShiftDirty_;
};
