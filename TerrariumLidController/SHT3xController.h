#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "RTClib.h"
#include "Adafruit_SHT31.h"

class SHT3xController {
 public:
  struct Reading {
    bool valid;
    bool heaterInfluenced;
    bool settling;
    float temperatureC;
    float humidity;
    DateTime timestamp;
  };

  struct Diagnostics {
    bool present;
    uint8_t address;
    bool heaterEnabled;
    unsigned long lastHeaterMs;
    bool wetStuck;
    unsigned int pulsesLastHour;
    bool condensationFault;
  };

  struct HeaterEvent {
    DateTime timestamp;
    unsigned long durationMs;
    const char* reason;
    float rhBefore;
    float tempBeforeC;
    float rhAfter;
    float tempAfterC;
  };

  SHT3xController();

  bool begin(TwoWire& wire, uint8_t primaryAddr = 0x44, uint8_t fallbackAddr = 0x45);
  void setLogStream(Stream& stream);
  bool isPresent() const;
  void update(const DateTime& now, unsigned long nowMs);
  Reading getLastReading() const;
  Reading getLastTrustedReading() const;
  Diagnostics getDiagnostics() const;
  size_t getHeaterEventCount() const;
  HeaterEvent getHeaterEvent(size_t index) const;

 private:
  static constexpr unsigned long kSampleIntervalMs = 2000;
  static constexpr size_t kHistorySize = 4;
  static constexpr size_t kWetStuckSamples = 2;
  static constexpr float kWetStuckRhThreshold = 99.5f;
  static constexpr float kWetStuckDeltaC = 0.2f;
  static constexpr unsigned long kHeaterPulseMs = 500;
  static constexpr unsigned long kHeaterCooldownMs = 5000;
  static constexpr unsigned long kPulseWindowMs = 60UL * 60UL * 1000UL;
  static constexpr unsigned int kMaxPulsesPerHour = 12;
  static constexpr size_t kHeaterEventBufferSize = 8;
  static constexpr unsigned int kCondensationHours = 2;

  bool tryBegin(uint8_t addr);

  Adafruit_SHT31 sensor_;
  TwoWire* wire_;
  Stream* logStream_;
  bool present_;
  uint8_t address_;
  Reading lastReading_;
  Diagnostics diagnostics_;
  Reading history_[kHistorySize];
  size_t historyCount_;
  size_t historyIndex_;
  unsigned long lastSampleMs_;
  bool heaterEnabled_;
  bool wetStuck_;
  unsigned long heaterStartMs_;
  unsigned long settleUntilMs_;
  unsigned long lastPulseMs_;
  unsigned long pulseTimestamps_[kMaxPulsesPerHour];
  size_t pulseCount_;
  size_t pulseIndex_;
  unsigned int hourlyPulseCounts_[kCondensationHours];
  size_t hourlyIndex_;
  unsigned long hourlyWindowStartMs_;
  size_t hoursFilled_;
  bool condensationFault_;
  HeaterEvent heaterEvents_[kHeaterEventBufferSize];
  size_t heaterEventCount_;
  size_t heaterEventIndex_;
  struct PendingEvent {
    bool active;
    bool awaitingAfter;
    DateTime timestamp;
    unsigned long durationMs;
    const char* reason;
    float rhBefore;
    float tempBeforeC;
  };
  PendingEvent pendingEvent_;

  void updateHeaterState(const DateTime& now, unsigned long nowMs);
  void maybeStartHeaterPulse(const DateTime& now, unsigned long nowMs, const Reading& current);
  void recordHeaterEvent(const DateTime& ts, unsigned long durationMs, const char* reason,
                         float rhBefore, float tempBeforeC, float rhAfter, float tempAfterC);
  void updateCondensationFault(unsigned long nowMs);
  unsigned int countPulsesInWindow(unsigned long nowMs) const;
  bool canPulse(unsigned long nowMs) const;
};
