#include "ConsoleInterface.h"

#include <ctype.h>
#include <string.h>

ConsoleInterface::ConsoleInterface(Stream& serial, RTC_DS3231& rtc)
    : serial_(serial), rtc_(rtc), inputBuffer_{0}, inputLength_(0) {}

void ConsoleInterface::begin() {
  serial_.println("Console ready. Type 'help' for commands.");
  printPrompt();
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
}

void ConsoleInterface::handleCommand(const char* command) {
  while (*command != '\0' && isspace(*command)) {
    ++command;
  }

  if (*command == '\0') {
    return;
  }

  if (strcmp(command, "help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(command, "now") == 0 || strcmp(command, "time") == 0 || strcmp(command, "datetime") == 0) {
    printDateTime();
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
