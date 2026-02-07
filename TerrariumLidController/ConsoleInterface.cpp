#include "ConsoleInterface.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

ConsoleInterface::ConsoleInterface(Stream& serial, RTC_DS3231& rtc)
    : serial_(serial),
      rtc_(rtc),
      inputBuffer_{0},
      inputLength_(0),
      statusHandler_(nullptr),
      potHandler_(nullptr),
      debugHandler_(nullptr),
      forceOnHandler_(nullptr),
      forceOffHandler_(nullptr),
      sht3xHandler_(nullptr),
      displayHandler_(nullptr) {}

void ConsoleInterface::begin() {
  serial_.println("Console ready. Type 'help' for commands.");
  printPrompt();
}

void ConsoleInterface::setStatusHandler(StatusHandler handler) {
  statusHandler_ = handler;
}

void ConsoleInterface::setPotHandler(PotHandler handler) {
  potHandler_ = handler;
}

void ConsoleInterface::setDebugHandler(DebugHandler handler) {
  debugHandler_ = handler;
}

void ConsoleInterface::setForceOnHandler(ForceOnHandler handler) {
  forceOnHandler_ = handler;
}

void ConsoleInterface::setForceOffHandler(ForceOffHandler handler) {
  forceOffHandler_ = handler;
}

void ConsoleInterface::setSht3xHandler(Sht3xHandler handler) {
  sht3xHandler_ = handler;
}

void ConsoleInterface::setDisplayHandler(DisplayHandler handler) {
  displayHandler_ = handler;
}

void ConsoleInterface::update() {
  while (serial_.available() > 0) {
    int incoming = serial_.read();
    if (incoming < 0) {
      return;
    }

    char c = static_cast<char>(incoming);
    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      inputBuffer_[inputLength_] = '\0';
      handleCommand(inputBuffer_);
      inputLength_ = 0;
      inputBuffer_[0] = '\0';
      printPrompt();

      continue;
    }

    if (inputLength_ < (kBufferSize - 1)) {
      inputBuffer_[inputLength_++] = c;
    }
  }
}

void ConsoleInterface::printPrompt() {
  serial_.print("> ");
}

void ConsoleInterface::printHelp() {
  serial_.println("Commands:");
  serial_.println("  help            Show available commands");
  serial_.println("  now             Read date/time from DS3231");
  serial_.println("  settime         Set RTC (YYYY-MM-DD HH:MM:SS)");
  serial_.println("  status          Show current pot/gate/duty");
  serial_.println("  pot             Read current potentiometer value");
  serial_.println("  debug           Print schedule debug line");
  serial_.println("  forceOn         Force LED on (override schedule)");
  serial_.println("  forceOff        Return to schedule timing");
  serial_.println("  sht3x           Show SHT3x status and recent events");
  serial_.println("  display ...     Display commands (status/on/off/dim/flip/timeout/test)");
}

void ConsoleInterface::handleCommand(const char* command) {
  while (*command != '\0' && isspace(static_cast<unsigned char>(*command))) {
    ++command;
  }

  if (*command == '\0') {
    return;
  }

  const char* end = command;
  while (*end != '\0' && !isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  size_t len = static_cast<size_t>(end - command);

  if (len == 4 && strncmp(command, "help", len) == 0) {
    printHelp();
    return;
  }

  if ((len == 3 && strncmp(command, "now", len) == 0) ||
      (len == 4 && strncmp(command, "time", len) == 0) ||
      (len == 8 && strncmp(command, "datetime", len) == 0)) {
    printDateTime();
    return;
  }

  if (len == 7 && strncmp(command, "settime", len) == 0) {
    const char* args = end;
    while (*args != '\0' && isspace(static_cast<unsigned char>(*args))) {
      ++args;
    }
    if (*args == '\0') {
      printSetTimeUsage();
      return;
    }

    DateTime dt;
    if (!parseDateTime(args, dt)) {
      printSetTimeUsage();
      return;
    }

    rtc_.adjust(dt);
    serial_.println("RTC updated.");
    printDateTime();
    return;
  }

  if (len == 6 && strncmp(command, "status", len) == 0) {
    if (statusHandler_ != nullptr) {
      statusHandler_(serial_);
    } else {
      serial_.println("Status not available.");
    }
    return;
  }

  if (len == 3 && strncmp(command, "pot", len) == 0) {
    if (potHandler_ != nullptr) {
      potHandler_(serial_);
    } else {
      serial_.println("Pot not available.");
    }
    return;
  }

  if (len == 5 && strncmp(command, "debug", len) == 0) {
    if (debugHandler_ != nullptr) {
      debugHandler_(serial_);
    } else {
      serial_.println("Debug not available.");
    }
    return;
  }

  if ((len == 7 && strncmp(command, "forceOn", len) == 0) ||
      (len == 7 && strncmp(command, "forceon", len) == 0)) {
    if (forceOnHandler_ != nullptr) {
      forceOnHandler_(serial_);
    } else {
      serial_.println("Force-on not available.");
    }
    return;
  }

  if ((len == 8 && strncmp(command, "forceOff", len) == 0) ||
      (len == 8 && strncmp(command, "forceoff", len) == 0)) {
    if (forceOffHandler_ != nullptr) {
      forceOffHandler_(serial_);
    } else {
      serial_.println("Force-off not available.");
    }
    return;
  }

  if (len == 5 && strncmp(command, "sht3x", len) == 0) {
    if (sht3xHandler_ != nullptr) {
      sht3xHandler_(serial_);
    } else {
      serial_.println("SHT3x not available.");
    }
    return;
  }

  if (len == 7 && strncmp(command, "display", len) == 0) {
    if (displayHandler_ != nullptr) {
      const char* args = end;
      while (*args != '\0' && isspace(static_cast<unsigned char>(*args))) {
        ++args;
      }
      displayHandler_(serial_, args);
    } else {
      serial_.println("Display commands not available.");
    }
    return;
  }

  serial_.print("Unknown command: ");
  serial_.println(command);
  serial_.println("Type 'help' to list supported commands.");
}

void ConsoleInterface::printDateTime() {
  DateTime now = rtc_.now();

  serial_.print("DS3231 datetime: ");
  serial_.print(now.year());
  serial_.print('-');
  if (now.month() < 10) serial_.print('0');
  serial_.print(now.month());
  serial_.print('-');
  if (now.day() < 10) serial_.print('0');
  serial_.print(now.day());
  serial_.print(' ');
  if (now.hour() < 10) serial_.print('0');
  serial_.print(now.hour());
  serial_.print(':');
  if (now.minute() < 10) serial_.print('0');
  serial_.print(now.minute());
  serial_.print(':');
  if (now.second() < 10) serial_.print('0');
  serial_.println(now.second());
}

void ConsoleInterface::printSetTimeUsage() {
  serial_.println("Usage:");
  serial_.println("  settime YYYY-MM-DD HH:MM:SS");
  serial_.println("  settime YYYY-MM-DDTHH:MM:SS");
}

bool ConsoleInterface::parseDateTime(const char* args, DateTime& out) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  int parsed = sscanf(args, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  if (parsed != 6) {
    parsed = sscanf(args, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  }
  if (parsed != 6) {
    return false;
  }

  if (year < 2000 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;
  if (second < 0 || second > 59) return false;

  out = DateTime(year, month, day, hour, minute, second);
  return true;
}
