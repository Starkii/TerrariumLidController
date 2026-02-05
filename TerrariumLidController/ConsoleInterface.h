#pragma once

#include <Arduino.h>
#include "RTClib.h"

class ConsoleInterface {
 public:
  ConsoleInterface(Stream& serial, RTC_DS3231& rtc);

  void begin();
  void update();

 private:
  static constexpr size_t kBufferSize = 64;
  static constexpr unsigned long kCommandIdleTimeoutMs = 250;

  Stream& serial_;
  RTC_DS3231& rtc_;
  char inputBuffer_[kBufferSize];
  size_t inputLength_;
  unsigned long lastInputMs_;

  void processBufferedCommand();
  void printPrompt();
  void printHelp();
  void handleCommand(const char* command);
  void printDateTime();
};
