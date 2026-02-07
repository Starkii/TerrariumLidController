#include "SHT3xController.h"

#include <math.h>

SHT3xController::SHT3xController()
    : wire_(nullptr),
      logStream_(nullptr),
      present_(false),
      address_(0),
      lastReading_{false, false, false, 0.0f, 0.0f, DateTime(2000, 1, 1, 0, 0, 0)},
      diagnostics_{false, 0, false, 0, false, 0, false},
      history_{},
      historyCount_(0),
      historyIndex_(0),
      lastSampleMs_(0),
      heaterEnabled_(false),
      wetStuck_(false),
      heaterStartMs_(0),
      settleUntilMs_(0),
      lastPulseMs_(0),
      pulseTimestamps_{},
      pulseCount_(0),
      pulseIndex_(0),
      hourlyPulseCounts_{},
      hourlyIndex_(0),
      hourlyWindowStartMs_(0),
      hoursFilled_(0),
      condensationFault_(false),
      heaterEvents_{},
      heaterEventCount_(0),
      heaterEventIndex_(0),
      pendingEvent_{false, false, DateTime(2000, 1, 1, 0, 0, 0), 0, nullptr, 0.0f, 0.0f} {}

bool SHT3xController::begin(TwoWire& wire, uint8_t primaryAddr, uint8_t fallbackAddr) {
  wire_ = &wire;
  present_ = false;
  address_ = 0;
  diagnostics_ = {false, 0, false, 0, false, 0, false};

  if (tryBegin(primaryAddr)) {
    address_ = primaryAddr;
    present_ = true;
  } else if (fallbackAddr != primaryAddr && tryBegin(fallbackAddr)) {
    address_ = fallbackAddr;
    present_ = true;
  }

  diagnostics_.present = present_;
  diagnostics_.address = address_;
  diagnostics_.heaterEnabled = heaterEnabled_;
  diagnostics_.lastHeaterMs = 0;
  diagnostics_.wetStuck = false;
  diagnostics_.pulsesLastHour = 0;
  diagnostics_.condensationFault = false;
  historyCount_ = 0;
  historyIndex_ = 0;
  lastSampleMs_ = 0;
  heaterEnabled_ = false;
  wetStuck_ = false;
  heaterStartMs_ = 0;
  settleUntilMs_ = 0;
  lastPulseMs_ = 0;
  pulseCount_ = 0;
  pulseIndex_ = 0;
  hourlyPulseCounts_[0] = 0;
  hourlyPulseCounts_[1] = 0;
  hourlyIndex_ = 0;
  hourlyWindowStartMs_ = 0;
  hoursFilled_ = 0;
  condensationFault_ = false;
  heaterEventCount_ = 0;
  heaterEventIndex_ = 0;
  pendingEvent_ = {false, false, DateTime(2000, 1, 1, 0, 0, 0), 0, nullptr, 0.0f, 0.0f};

  if (present_) {
    sensor_.heater(false);
  }

  return present_;
}

void SHT3xController::setLogStream(Stream& stream) {
  logStream_ = &stream;
}

bool SHT3xController::tryBegin(uint8_t addr) {
  (void)wire_;
  return sensor_.begin(addr);
}

bool SHT3xController::isPresent() const {
  return present_;
}

void SHT3xController::update(const DateTime& now, unsigned long nowMs) {
  if (!present_) {
    return;
  }

  updateHeaterState(now, nowMs);

  if (lastSampleMs_ != 0 && (nowMs - lastSampleMs_) < kSampleIntervalMs) {
    return;
  }
  lastSampleMs_ = nowMs;

  float temperature = sensor_.readTemperature();
  float humidity = sensor_.readHumidity();

  diagnostics_.heaterEnabled = heaterEnabled_;

  bool valid = !(isnan(temperature) || isnan(humidity));
  bool settling = (!heaterEnabled_ && settleUntilMs_ != 0 && nowMs < settleUntilMs_);
  Reading reading{valid, heaterEnabled_, settling, temperature, humidity, now};
  lastReading_ = reading;

  history_[historyIndex_] = reading;
  historyIndex_ = (historyIndex_ + 1) % kHistorySize;
  if (historyCount_ < kHistorySize) {
    historyCount_++;
  }

  wetStuck_ = false;
  if (historyCount_ >= kWetStuckSamples) {
    float minTemp = 0.0f;
    float maxTemp = 0.0f;
    bool tempsInitialized = false;
    bool allHighRh = true;
    bool allValid = true;

    for (size_t i = 0; i < kWetStuckSamples; ++i) {
      size_t idx = (historyIndex_ + kHistorySize - 1 - i) % kHistorySize;
      const Reading& sample = history_[idx];
      if (!sample.valid || sample.heaterInfluenced || sample.settling) {
        allValid = false;
        break;
      }
      if (sample.humidity < kWetStuckRhThreshold) {
        allHighRh = false;
        break;
      }
      if (!tempsInitialized) {
        minTemp = sample.temperatureC;
        maxTemp = sample.temperatureC;
        tempsInitialized = true;
      } else {
        if (sample.temperatureC < minTemp) minTemp = sample.temperatureC;
        if (sample.temperatureC > maxTemp) maxTemp = sample.temperatureC;
      }
    }

    if (allValid && allHighRh && tempsInitialized) {
      wetStuck_ = (fabsf(maxTemp - minTemp) <= kWetStuckDeltaC);
    }
  }

  diagnostics_.wetStuck = wetStuck_;

  if (wetStuck_) {
    maybeStartHeaterPulse(now, nowMs, reading);
  }

  diagnostics_.pulsesLastHour = countPulsesInWindow(nowMs);

  if (pendingEvent_.awaitingAfter) {
    float rhAfter = reading.valid ? reading.humidity : NAN;
    float tempAfter = reading.valid ? reading.temperatureC : NAN;
    recordHeaterEvent(pendingEvent_.timestamp, pendingEvent_.durationMs, pendingEvent_.reason,
                      pendingEvent_.rhBefore, pendingEvent_.tempBeforeC, rhAfter, tempAfter);
    pendingEvent_.awaitingAfter = false;
  }

  updateCondensationFault(nowMs);
}

void SHT3xController::updateHeaterState(const DateTime& now, unsigned long nowMs) {
  (void)now;
  if (!heaterEnabled_) {
    return;
  }
  if (nowMs - heaterStartMs_ < kHeaterPulseMs) {
    return;
  }

  sensor_.heater(false);
  heaterEnabled_ = false;
  diagnostics_.heaterEnabled = false;
  diagnostics_.lastHeaterMs = nowMs;
  settleUntilMs_ = nowMs + kHeaterCooldownMs;

  if (logStream_ != nullptr) {
    logStream_->println("SHT3x: heater disabled (cooldown)");
  }

  if (pendingEvent_.active) {
    pendingEvent_.durationMs = nowMs - heaterStartMs_;
    pendingEvent_.awaitingAfter = true;
    pendingEvent_.active = false;
  }
}

void SHT3xController::maybeStartHeaterPulse(const DateTime& now, unsigned long nowMs, const Reading& current) {
  if (!canPulse(nowMs)) {
    return;
  }

  pendingEvent_.active = true;
  pendingEvent_.awaitingAfter = false;
  pendingEvent_.timestamp = now;
  pendingEvent_.reason = "wet/stuck";
  pendingEvent_.rhBefore = current.humidity;
  pendingEvent_.tempBeforeC = current.temperatureC;

  sensor_.heater(true);
  heaterEnabled_ = true;
  heaterStartMs_ = nowMs;
  diagnostics_.heaterEnabled = true;
  diagnostics_.lastHeaterMs = nowMs;

  if (logStream_ != nullptr) {
    logStream_->println("SHT3x: heater enabled (wet/stuck)");
  }

  lastPulseMs_ = nowMs;
  pulseTimestamps_[pulseIndex_] = nowMs;
  pulseIndex_ = (pulseIndex_ + 1) % kMaxPulsesPerHour;
  if (pulseCount_ < kMaxPulsesPerHour) {
    pulseCount_++;
  }

  if (hourlyWindowStartMs_ == 0) {
    hourlyWindowStartMs_ = nowMs;
    if (hoursFilled_ == 0) {
      hoursFilled_ = 1;
    }
  }
  while (nowMs - hourlyWindowStartMs_ >= kPulseWindowMs) {
    hourlyWindowStartMs_ += kPulseWindowMs;
    hourlyIndex_ = (hourlyIndex_ + 1) % kCondensationHours;
    hourlyPulseCounts_[hourlyIndex_] = 0;
    if (hoursFilled_ < kCondensationHours) {
      hoursFilled_++;
    }
  }
  hourlyPulseCounts_[hourlyIndex_]++;
}

void SHT3xController::recordHeaterEvent(const DateTime& ts, unsigned long durationMs, const char* reason,
                                        float rhBefore, float tempBeforeC, float rhAfter, float tempAfterC) {
  HeaterEvent event{ts, durationMs, reason, rhBefore, tempBeforeC, rhAfter, tempAfterC};
  heaterEvents_[heaterEventIndex_] = event;
  heaterEventIndex_ = (heaterEventIndex_ + 1) % kHeaterEventBufferSize;
  if (heaterEventCount_ < kHeaterEventBufferSize) {
    heaterEventCount_++;
  }
}

void SHT3xController::updateCondensationFault(unsigned long nowMs) {
  if (hourlyWindowStartMs_ == 0) {
    return;
  }
  while (nowMs - hourlyWindowStartMs_ >= kPulseWindowMs) {
    hourlyWindowStartMs_ += kPulseWindowMs;
    hourlyIndex_ = (hourlyIndex_ + 1) % kCondensationHours;
    hourlyPulseCounts_[hourlyIndex_] = 0;
    if (hoursFilled_ < kCondensationHours) {
      hoursFilled_++;
    }
  }

  if (hoursFilled_ >= kCondensationHours) {
    size_t prevIndex = (hourlyIndex_ + kCondensationHours - 1) % kCondensationHours;
    if (hourlyPulseCounts_[hourlyIndex_] > kMaxPulsesPerHour &&
        hourlyPulseCounts_[prevIndex] > kMaxPulsesPerHour) {
      if (!condensationFault_) {
        condensationFault_ = true;
        if (logStream_ != nullptr) {
          logStream_->println("SHT3x: condensation fault detected");
        }
      }
    }
  }
  diagnostics_.condensationFault = condensationFault_;
}

unsigned int SHT3xController::countPulsesInWindow(unsigned long nowMs) const {
  unsigned int count = 0;
  for (size_t i = 0; i < pulseCount_; ++i) {
    unsigned long ts = pulseTimestamps_[i];
    if ((nowMs - ts) <= kPulseWindowMs) {
      count++;
    }
  }
  return count;
}

bool SHT3xController::canPulse(unsigned long nowMs) const {
  if (heaterEnabled_) {
    return false;
  }
  if (settleUntilMs_ != 0 && nowMs < settleUntilMs_) {
    return false;
  }
  if (lastPulseMs_ != 0 && (nowMs - lastPulseMs_) < kSampleIntervalMs) {
    return false;
  }
  if (countPulsesInWindow(nowMs) >= kMaxPulsesPerHour) {
    return false;
  }
  return true;
}

SHT3xController::Reading SHT3xController::getLastReading() const {
  return lastReading_;
}

SHT3xController::Reading SHT3xController::getLastTrustedReading() const {
  for (size_t i = 0; i < historyCount_; ++i) {
    size_t idx = (historyIndex_ + kHistorySize - 1 - i) % kHistorySize;
    const Reading& sample = history_[idx];
    if (sample.valid && !sample.heaterInfluenced && !sample.settling) {
      return sample;
    }
  }
  return Reading{false, false, false, 0.0f, 0.0f, DateTime(2000, 1, 1, 0, 0, 0)};
}

SHT3xController::Diagnostics SHT3xController::getDiagnostics() const {
  return diagnostics_;
}

size_t SHT3xController::getHeaterEventCount() const {
  return heaterEventCount_;
}

SHT3xController::HeaterEvent SHT3xController::getHeaterEvent(size_t index) const {
  if (index >= heaterEventCount_) {
    return HeaterEvent{DateTime(2000, 1, 1, 0, 0, 0), 0, nullptr, 0.0f, 0.0f, 0.0f, 0.0f};
  }
  size_t base = (heaterEventIndex_ + kHeaterEventBufferSize - heaterEventCount_) % kHeaterEventBufferSize;
  size_t idx = (base + index) % kHeaterEventBufferSize;
  return heaterEvents_[idx];
}
