#include <Wire.h>
#include <stdlib.h>
#include <string.h>
#include "RTClib.h"
#include "ConsoleInterface.h"
#include "SHT3xController.h"
#include "DisplayConfig.h"
#include "DisplayController.h"
#include "UiState.h"

// ====================== Pins ======================
constexpr int LED_PIN = 0;   // GPIO0 -> MOSFET gate
constexpr int POT_PIN = 1;   // GPIO1 -> potentiometer wiper (ADC)

// I2C pins for ESP32-C3 (your chosen wiring)
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;

// ====================== PWM ======================
constexpr int PWM_FREQ = 1000;        // Hz
constexpr int PWM_RESOLUTION = 10;    // bits (0..1023)
constexpr int MAX_DUTY = (1 << PWM_RESOLUTION) - 1;
constexpr int PWM_CHANNEL = 0;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define TLC_LEDC_NEW_API 1
#else
  #define TLC_LEDC_NEW_API 0
#endif

// ====================== Brightness behavior ======================
constexpr bool INVERT_KNOB = false;
constexpr float MAX_BRIGHTNESS = 0.70f;

constexpr float FILTER_ALPHA = 0.90f; // 0.85..0.95 typical
constexpr int DEADZONE_DUTY = 6;
constexpr int LOOP_DELAY_MS = 10;

// ====================== Schedule ======================
// Local schedule according to DS3231 clock time.
constexpr int ON_HOUR   = 9;          // 09:00
constexpr int ON_MINUTE = 0;
constexpr int DURATION_MINUTES = (13 * 60) + 26; // 20 hours (for testing)

// Optional: fade in/out (set to 0 for none)
constexpr int FADE_MINUTES = 10; // sunrise and sunset ramp length

RTC_DS3231 rtc;
ConsoleInterface console(Serial, rtc);
SHT3xController sht3x;
DisplayController displayController;
UiState uiState{};
static bool forceOn = false;
static unsigned long displayMaxUpdateUs = 0;
static unsigned long displayLastUpdateUs = 0;
static unsigned long displayLastTimingLogMs = 0;

// ====================== Helpers ======================

int minutesOfDay(int h, int m) {
  return h * 60 + m;
}

float cToF(float c) {
  return (c * 9.0f / 5.0f) + 32.0f;
}

void formatNextEvent(char* out, size_t outSize, bool scheduleAllowed) {
  int endMin = (minutesOfDay(ON_HOUR, ON_MINUTE) + DURATION_MINUTES) % (24 * 60);
  if (scheduleAllowed) {
    int hh = endMin / 60;
    int mm = endMin % 60;
    snprintf(out, outSize, "NEXT OFF %02d:%02d", hh, mm);
  } else {
    snprintf(out, outSize, "NEXT ON  %02d:%02d", ON_HOUR, ON_MINUTE);
  }
}

void scanI2C() {
  Serial.println("I2C scan:");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("  Found device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      found++;
    }
  }
  if (found == 0) Serial.println("  (no I2C devices found)");
}

void debugSchedule(
  Stream& serial,
  const DateTime& now,
  int nowMin,
  int startMin,
  int durationMin,
  bool inWindow
) {
  int endMin = (startMin + durationMin) % (24 * 60);

  serial.print("[RTC] ");
  serial.print(now.hour()); serial.print(":");
  if (now.minute() < 10) serial.print("0");
  serial.print(now.minute());

  serial.print(" | nowMin=");
  serial.print(nowMin);

  serial.print(" | window=");
  serial.print(startMin);
  serial.print(" -> ");
  serial.print(endMin);

  serial.print(" (");
  serial.print(durationMin);
  serial.print("m)");

  serial.print(" | inWindow=");
  serial.println(inWindow ? "YES" : "NO");
}

// Returns true if "nowMin" is inside [startMin, startMin+duration) wrapping midnight.
bool isInWindow(int nowMin, int startMin, int durationMin) {
  const int endMin = (startMin + durationMin) % (24 * 60);
  if (durationMin <= 0) return false;
  if (startMin == endMin) return true; // 24h or multiple of 24h
  if (startMin < endMin) {
    return nowMin >= startMin && nowMin < endMin;
  } else {
    // wraps midnight
    return (nowMin >= startMin) || (nowMin < endMin);
  }
}

// Compute a fade multiplier [0..1] within the window.
// Linear fade in first FADE_MINUTES and fade out last FADE_MINUTES.
float fadeMultiplier(int nowMin, int startMin, int durationMin, int fadeMin) {
  if (fadeMin <= 0) return 1.0f;
  if (fadeMin * 2 >= durationMin) return 1.0f; // avoid weird overlap

  // Compute minutes since window start in [0..durationMin)
  int sinceStart = nowMin - startMin;
  if (sinceStart < 0) sinceStart += 24 * 60; // handle wrap

  int untilEnd = durationMin - sinceStart;

  if (sinceStart < fadeMin) {
    return sinceStart / float(fadeMin);
  }
  if (untilEnd < fadeMin) {
    return untilEnd / float(fadeMin);
  }
  return 1.0f;
}

void printStatus(Stream& serial) {
  serial.print("rtc=");
  serial.print(uiState.rtcNow.year());
  serial.print('-');
  if (uiState.rtcNow.month() < 10) serial.print('0');
  serial.print(uiState.rtcNow.month());
  serial.print('-');
  if (uiState.rtcNow.day() < 10) serial.print('0');
  serial.print(uiState.rtcNow.day());
  serial.print(' ');
  if (uiState.rtcNow.hour() < 10) serial.print('0');
  serial.print(uiState.rtcNow.hour());
  serial.print(':');
  if (uiState.rtcNow.minute() < 10) serial.print('0');
  serial.print(uiState.rtcNow.minute());
  serial.print(':');
  if (uiState.rtcNow.second() < 10) serial.print('0');
  serial.print(uiState.rtcNow.second());
  serial.print(' ');
  serial.print("raw="); serial.print(uiState.rawPot);
  serial.print(" x="); serial.print(uiState.potScaled, 3);
  serial.print(" filtered="); serial.print(uiState.potFiltered, 3);
  serial.print(" allowed="); serial.print(uiState.scheduleAllowed ? "Y" : "N");
  serial.print(" forced="); serial.print(uiState.forceOn ? "Y" : "N");
  serial.print(" gate="); serial.print(uiState.gate, 3);
  serial.print(" duty="); serial.print(uiState.duty);
  serial.print(" mode=");
  if (uiState.controlMode == ControlMode::Override) serial.print("OVR");
  else if (uiState.controlMode == ControlMode::Schedule) serial.print("SCH");
  else serial.print("POT");
  serial.print(" next=");
  serial.println(uiState.nextEvent);
}

void printPot(Stream& serial) {
  serial.print("pot=");
  serial.print(uiState.rawPot);
  serial.print(" norm=");
  serial.print(uiState.potNorm, 3);
  serial.print(" scaled=");
  serial.print(uiState.potScaled, 3);
  serial.print(" filtered=");
  serial.println(uiState.potFiltered, 3);
}

void printDebug(Stream& serial) {
  DateTime now = rtc.now();
  int nowMin = minutesOfDay(now.hour(), now.minute());
  int startMin = minutesOfDay(ON_HOUR, ON_MINUTE);
  bool allowed = isInWindow(nowMin, startMin, DURATION_MINUTES);

  debugSchedule(serial, now, nowMin, startMin, DURATION_MINUTES, allowed);
}

void handleForceOn(Stream& serial) {
  forceOn = true;
  uiState.forceOn = true;
  serial.println("Force on enabled (schedule overridden). Use 'forceOff' to return to schedule.");
}

void handleForceOff(Stream& serial) {
  forceOn = false;
  uiState.forceOn = false;
  serial.println("Force on disabled. Schedule timing re-enabled.");
}

void printSht3xStatus(Stream& serial) {
  if (!sht3x.isPresent()) {
    serial.println("SHT3x: not detected");
    return;
  }

  SHT3xController::Reading last = sht3x.getLastReading();
  SHT3xController::Reading trusted = sht3x.getLastTrustedReading();
  SHT3xController::Diagnostics diag = sht3x.getDiagnostics();

  serial.print("SHT3x addr=0x");
  if (diag.address < 16) serial.print("0");
  serial.print(diag.address, HEX);
  serial.print(" heater=");
  serial.print(diag.heaterEnabled ? "Y" : "N");
  serial.print(" wetStuck=");
  serial.print(diag.wetStuck ? "Y" : "N");
  serial.print(" pulsesLastHour=");
  serial.print(diag.pulsesLastHour);
  serial.print(" condensationFault=");
  serial.println(diag.condensationFault ? "Y" : "N");

  serial.print("last valid=");
  serial.print(last.valid ? "Y" : "N");
  serial.print(" influenced=");
  serial.print(last.heaterInfluenced ? "Y" : "N");
  serial.print(" settling=");
  serial.print(last.settling ? "Y" : "N");
  serial.print(" T=");
  serial.print(cToF(last.temperatureC), 2);
  serial.print("F RH=");
  serial.print(last.humidity, 2);
  serial.println("%");

  serial.print("trusted valid=");
  serial.print(trusted.valid ? "Y" : "N");
  serial.print(" T=");
  serial.print(cToF(trusted.temperatureC), 2);
  serial.print("F RH=");
  serial.print(trusted.humidity, 2);
  serial.println("%");

  size_t count = sht3x.getHeaterEventCount();
  if (count == 0) {
    serial.println("events: (none)");
    return;
  }

  serial.println("events:");
  for (size_t i = 0; i < count; ++i) {
    SHT3xController::HeaterEvent ev = sht3x.getHeaterEvent(i);
    serial.print("  ");
    serial.print(ev.timestamp.year());
    serial.print("-");
    if (ev.timestamp.month() < 10) serial.print("0");
    serial.print(ev.timestamp.month());
    serial.print("-");
    if (ev.timestamp.day() < 10) serial.print("0");
    serial.print(ev.timestamp.day());
    serial.print(" ");
    if (ev.timestamp.hour() < 10) serial.print("0");
    serial.print(ev.timestamp.hour());
    serial.print(":");
    if (ev.timestamp.minute() < 10) serial.print("0");
    serial.print(ev.timestamp.minute());
    serial.print(":");
    if (ev.timestamp.second() < 10) serial.print("0");
    serial.print(ev.timestamp.second());
    serial.print(" dur=");
    serial.print(ev.durationMs);
    serial.print("ms reason=");
    serial.print(ev.reason ? ev.reason : "unknown");
    serial.print(" RH=");
    serial.print(ev.rhBefore, 2);
    serial.print("->");
    serial.print(ev.rhAfter, 2);
    serial.print(" T=");
    serial.print(cToF(ev.tempBeforeC), 2);
    serial.print("->");
    serial.print(cToF(ev.tempAfterC), 2);
    serial.println();
  }
}

void printDisplayStatus(Stream& serial) {
  DisplayController::Status st = displayController.getStatus();
  serial.print("Display: ");
  serial.print(st.present ? "present" : "missing");
  serial.print(" addr=");
  if (st.present) {
    serial.print("0x");
    if (st.address < 16) serial.print("0");
    serial.print(st.address, HEX);
  } else {
    serial.print("--");
  }
  serial.print(" orientation=");
  serial.print(st.flipped ? "INVERTED" : "NORMAL");
  serial.print(" mode=");
  if (st.powerMode == DisplayController::PowerMode::ForcedOff) serial.print("OFF");
  else if (st.powerMode == DisplayController::PowerMode::ForcedDim) serial.print("DIM");
  else serial.print("AUTO");
  serial.print(" enabled=");
  serial.print(st.enabled ? "on" : "off");
  serial.print(" dim=");
  serial.print(st.dimmed ? "yes" : "no");
  serial.print(" timeoutDimMin=");
  serial.print(st.dimTimeoutMin);
  serial.print(" timeoutOffMin=");
  serial.print(st.offTimeoutMin);
  serial.print(" updateUs(last/max)=");
  serial.print(displayLastUpdateUs);
  serial.print("/");
  serial.println(displayMaxUpdateUs);
}

void handleDisplayCommand(Stream& serial, const char* args) {
  if (args == nullptr || *args == '\0') {
    serial.println("Usage: display status | display on|off|dim | display flip [on|off] | display timeout dim|off <min> | display test");
    return;
  }

  if (strncmp(args, "status", 6) == 0 && (args[6] == '\0' || args[6] == ' ')) {
    printDisplayStatus(serial);
    return;
  }

  if (strncmp(args, "flip", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
    const char* p = args + 4;
    while (*p == ' ') ++p;

    if (*p == '\0') {
      displayController.toggleFlip();
      DisplayController::Status st = displayController.getStatus();
      serial.print("Display orientation: ");
      serial.print(st.flipped ? "INVERTED" : "NORMAL");
      serial.println(" (saved)");
      return;
    }

    if (strcmp(p, "on") == 0) {
      displayController.setFlip(true);
      serial.println("Display orientation: INVERTED (saved)");
      return;
    }

    if (strcmp(p, "off") == 0) {
      displayController.setFlip(false);
      serial.println("Display orientation: NORMAL (saved)");
      return;
    }
  }

  if (strcmp(args, "on") == 0) {
    displayController.setPowerMode(DisplayController::PowerMode::Auto);
    serial.println("Display mode: AUTO");
    return;
  }

  if (strcmp(args, "off") == 0) {
    displayController.setPowerMode(DisplayController::PowerMode::ForcedOff);
    serial.println("Display mode: OFF");
    return;
  }

  if (strcmp(args, "dim") == 0) {
    displayController.setPowerMode(DisplayController::PowerMode::ForcedDim);
    serial.println("Display mode: DIM");
    return;
  }

  if (strncmp(args, "timeout ", 8) == 0) {
    const char* p = args + 8;
    if (strncmp(p, "dim ", 4) == 0) {
      int mins = atoi(p + 4);
      if (mins < 0) mins = 0;
      displayController.setTimeoutDimMinutes(static_cast<uint16_t>(mins));
      serial.print("Display dim timeout set to ");
      serial.print(mins);
      serial.println(" min");
      return;
    }
    if (strncmp(p, "off ", 4) == 0) {
      int mins = atoi(p + 4);
      if (mins < 0) mins = 0;
      displayController.setTimeoutOffMinutes(static_cast<uint16_t>(mins));
      serial.print("Display off timeout set to ");
      serial.print(mins);
      serial.println(" min");
      return;
    }
  }

  if (strcmp(args, "test") == 0) {
    displayController.runFactoryTest(serial, 30000UL, writePwm, MAX_DUTY);
    return;
  }

  serial.println("Usage: display status | display on|off|dim | display flip [on|off] | display timeout dim|off <min> | display test");
}

void setupPwm() {
#if TLC_LEDC_NEW_API
  ledcAttach(LED_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(LED_PIN, 0);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(LED_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);
#endif
}

void writePwm(int duty) {
#if TLC_LEDC_NEW_API
  ledcWrite(LED_PIN, duty);
#else
  ledcWrite(PWM_CHANNEL, duty);
#endif
}

// ====================== Setup / Loop ======================

void setup() {
  // Bring up Serial FIRST so we can see failures
  Serial.begin(115200);
  delay(500);
  Serial.println("\nBOOT: starting...");

  // Ensure off at boot
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // PWM attach (compat with Arduino-ESP32 2.x/3.x)
  setupPwm();

  // Quick LED sanity test (bypasses schedule + pot)
  Serial.println("LED test: 25% for 1s");
  writePwm(MAX_DUTY / 4);
  delay(200);
  writePwm(0);
  Serial.println("LED test done");

  // ADC
  analogReadResolution(12); // 0..4095

  // I2C + RTC
  Serial.print("Wire.begin SDA="); Serial.print(I2C_SDA);
  Serial.print(" SCL="); Serial.println(I2C_SCL);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);

  scanI2C();

  Serial.println("rtc.begin()...");
  bool ok = rtc.begin();
  Serial.print("rtc.begin() -> ");
  Serial.println(ok ? "OK" : "FAIL");

  if (!ok) {
    Serial.println("FAIL-SAFE: RTC not detected. Check wiring/pins/power.");
    // Keep running so you can see logs, but keep LED off.
    while (true) {
      writePwm(0);
      delay(1000);
    }
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power; setting to compile time. Use 'settime' to update.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  DateTime now = rtc.now();
  Serial.print("RTC now: ");
  Serial.print(now.year()); Serial.print("-");
  Serial.print(now.month()); Serial.print("-");
  Serial.print(now.day()); Serial.print(" ");
  Serial.print(now.hour()); Serial.print(":");
  if (now.minute() < 10) Serial.print("0");
  Serial.println(now.minute());

  Serial.println("--- Main loop starting ---");
  bool sht3xOk = sht3x.begin(Wire);
  if (sht3xOk) {
    sht3x.setLogStream(Serial);
    Serial.println("SHT3x: detected");
  } else {
    Serial.println("SHT3x: not detected");
  }

  displayController.setLogStream(Serial);
  if (displayController.begin(Wire)) {
    Serial.println("SSD1306: detected");
  }

  console.setStatusHandler(printStatus);
  console.setPotHandler(printPot);
  console.setDebugHandler(printDebug);
  console.setForceOnHandler(handleForceOn);
  console.setForceOffHandler(handleForceOff);
  console.setSht3xHandler(printSht3xStatus);
  console.setDisplayHandler(handleDisplayCommand);
  console.begin();
}

void loop() {
  console.update();
  static float filtered = 0.0f;
  static int lastDuty = -1;
  const unsigned long nowMs = millis();

  // ---- Read pot -> normalized brightness 0..1 ----
  int raw = analogRead(POT_PIN);
  float norm = raw / 4095.0f;
  float x = norm;
  if (INVERT_KNOB) x = 1.0f - x;
  x *= MAX_BRIGHTNESS;

  // Smooth
  filtered = FILTER_ALPHA * filtered + (1.0f - FILTER_ALPHA) * x;

  // ---- RTC gating ----
  DateTime now = rtc.now();
  int nowMin = minutesOfDay(now.hour(), now.minute());

  int startMin = minutesOfDay(ON_HOUR, ON_MINUTE);
  bool scheduleAllowed = isInWindow(nowMin, startMin, DURATION_MINUTES);
  bool allowed = forceOn ? true : scheduleAllowed;

  if (sht3x.isPresent()) {
    sht3x.update(now, nowMs);
  }
  unsigned long t0 = micros();
  displayController.update(uiState, nowMs);
  unsigned long dt = micros() - t0;
  displayLastUpdateUs = dt;
  if (dt > displayMaxUpdateUs) {
    displayMaxUpdateUs = dt;
  }
  if (dt > 10000UL && (nowMs - displayLastTimingLogMs) > 10000UL) {
    displayLastTimingLogMs = nowMs;
    Serial.print("WARN: display update took ");
    Serial.print(dt);
    Serial.println(" us");
  }

  float gate = 0.0f;
  if (forceOn) {
    gate = 1.0f;
  } else if (scheduleAllowed) {
    gate = fadeMultiplier(nowMin, startMin, DURATION_MINUTES, FADE_MINUTES);
  }

  // ---- Final PWM ----
  int duty = 0;
  if (allowed) {
    duty = int(filtered * gate * MAX_DUTY + 0.5f);
    if (duty < DEADZONE_DUTY) duty = 0;
  } else {
    duty = 0;
  }

  if (duty != lastDuty) {
    writePwm(duty);
    lastDuty = duty;
  }

  uiState.rtcNow = now;
  uiState.rtcValid = true;
  uiState.rawPot = raw;
  uiState.potNorm = norm;
  uiState.potScaled = x;
  uiState.potFiltered = filtered;
  uiState.brightnessPercent = int((x / MAX_BRIGHTNESS) * 100.0f + 0.5f);
  uiState.duty = duty;
  uiState.lightOn = (duty > 0);
  uiState.scheduleAllowed = scheduleAllowed;
  uiState.forceOn = forceOn;
  uiState.gate = gate;
  uiState.controlMode = forceOn ? ControlMode::Override : ControlMode::Schedule;
  formatNextEvent(uiState.nextEvent, sizeof(uiState.nextEvent), scheduleAllowed);

  SHT3xController::Reading trusted = sht3x.getLastTrustedReading();
  uiState.hasHumidity = trusted.valid;
  uiState.humidityPercent = trusted.valid ? trusted.humidity : 0.0f;
  uiState.hasTempF = trusted.valid;
  uiState.temperatureF = trusted.valid ? cToF(trusted.temperatureC) : 0.0f;

  uiState.needsWatering = false;
  uiState.tooCold = false;
  uiState.tooHot = false;
  uiState.usbPowerLimited = false;

  delay(LOOP_DELAY_MS);
}
