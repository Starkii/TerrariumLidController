#pragma once

#include <Arduino.h>
#include "RTClib.h"

enum class ControlMode : uint8_t {
  Pot = 0,
  Schedule = 1,
  Override = 2,
};

struct UiState {
  DateTime rtcNow;
  bool rtcValid;

  int rawPot;
  float potNorm;
  float potScaled;
  float potFiltered;

  int brightnessPercent;
  int duty;
  bool lightOn;

  bool scheduleAllowed;
  bool forceOn;
  float gate;
  ControlMode controlMode;

  char nextEvent[16];

  bool hasHumidity;
  float humidityPercent;
  bool hasTempF;
  float temperatureF;

  bool needsWatering;
  bool tooCold;
  bool tooHot;
  bool usbPowerLimited;
};
