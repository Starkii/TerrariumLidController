#pragma once

#include <Arduino.h>
#include "RTClib.h"

class ConsoleInterface {
 public:
  using StatusHandler = void (*)(Stream& serial);
  using PotHandler = void (*)(Stream& serial);
  using DebugHandler = void (*)(Stream& serial);
  using ForceOnHandler = void (*)(Stream& serial);
  using ForceOffHandler = void (*)(Stream& serial);
  using Sht3xHandler = void (*)(Stream& serial);

  ConsoleInterface(Stream& serial, RTC_DS3231& rtc);

  void begin();
  void update();
  void setStatusHandler(StatusHandler handler);
  void setPotHandler(PotHandler handler);
  void setDebugHandler(DebugHandler handler);
  void setForceOnHandler(ForceOnHandler handler);
  void setForceOffHandler(ForceOffHandler handler);
  void setSht3xHandler(Sht3xHandler handler);

 private:
  static constexpr size_t kBufferSize = 64;

  Stream& serial_;
  RTC_DS3231& rtc_;
  char inputBuffer_[kBufferSize];
  size_t inputLength_;
  StatusHandler statusHandler_;
  PotHandler potHandler_;
  DebugHandler debugHandler_;
  ForceOnHandler forceOnHandler_;
  ForceOffHandler forceOffHandler_;
  Sht3xHandler sht3xHandler_;
  void printPrompt();
  void printHelp();
  void handleCommand(const char* command);
  void printDateTime();
  void printSetTimeUsage();
  bool parseDateTime(const char* args, DateTime& out);
};
