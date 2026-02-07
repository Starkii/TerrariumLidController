#include <Wire.h>
#include "RTClib.h"
#include "ConsoleInterface.h"
#include "SHT3xController.h"

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

static int statusRaw = 0;
static float statusNorm = 0.0f;
static float statusX = 0.0f;
static float statusFiltered = 0.0f;
static bool statusAllowed = false;
static float statusGate = 0.0f;
static int statusDuty = 0;
static bool statusForced = false;
static bool forceOn = false;

// ====================== Helpers ======================

int minutesOfDay(int h, int m) {
  return h * 60 + m;
}

float cToF(float c) {
  return (c * 9.0f / 5.0f) + 32.0f;
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
  DateTime now = rtc.now();
  serial.print("rtc=");
  serial.print(now.year());
  serial.print('-');
  if (now.month() < 10) serial.print('0');
  serial.print(now.month());
  serial.print('-');
  if (now.day() < 10) serial.print('0');
  serial.print(now.day());
  serial.print(' ');
  if (now.hour() < 10) serial.print('0');
  serial.print(now.hour());
  serial.print(':');
  if (now.minute() < 10) serial.print('0');
  serial.print(now.minute());
  serial.print(':');
  if (now.second() < 10) serial.print('0');
  serial.print(now.second());
  serial.print(' ');
  serial.print("raw="); serial.print(statusRaw);
  serial.print(" x="); serial.print(statusX, 3);
  serial.print(" filtered="); serial.print(statusFiltered, 3);
  serial.print(" allowed="); serial.print(statusAllowed ? "Y" : "N");
  serial.print(" forced="); serial.print(statusForced ? "Y" : "N");
  serial.print(" gate="); serial.print(statusGate, 3);
  serial.print(" duty="); serial.println(statusDuty);
}

void printPot(Stream& serial) {
  serial.print("pot=");
  serial.print(statusRaw);
  serial.print(" norm=");
  serial.print(statusNorm, 3);
  serial.print(" scaled=");
  serial.print(statusX, 3);
  serial.print(" filtered=");
  serial.println(statusFiltered, 3);
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
  serial.println("Force on enabled (schedule overridden). Use 'forceOff' to return to schedule.");
}

void handleForceOff(Stream& serial) {
  forceOn = false;
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
  console.setStatusHandler(printStatus);
  console.setPotHandler(printPot);
  console.setDebugHandler(printDebug);
  console.setForceOnHandler(handleForceOn);
  console.setForceOffHandler(handleForceOff);
  console.setSht3xHandler(printSht3xStatus);
  console.begin();
}

void loop() {
  console.update();
  static float filtered = 0.0f;
  static int lastDuty = -1;

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
    sht3x.update(now, millis());
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

  statusRaw = raw;
  statusNorm = norm;
  statusX = x;
  statusFiltered = filtered;
  statusAllowed = allowed;
  statusGate = gate;
  statusDuty = duty;
  statusForced = forceOn;

  delay(LOOP_DELAY_MS);
}
