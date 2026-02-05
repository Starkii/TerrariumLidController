#include <Wire.h>
#include "RTClib.h"

// ====================== Debug ======================
constexpr bool DEBUG_SCHEDULE = true;
constexpr unsigned long DEBUG_INTERVAL_MS = 5000;  // schedule print every 5s
constexpr unsigned long STATUS_INTERVAL_MS = 2000; // pot/gate/duty print every 2s

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

// ====================== Helpers ======================

int minutesOfDay(int h, int m) {
  return h * 60 + m;
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
  const DateTime& now,
  int nowMin,
  int startMin,
  int durationMin,
  bool inWindow
) {
  static unsigned long lastPrint = 0;
  unsigned long nowMs = millis();

  if (!DEBUG_SCHEDULE) return;
  if (nowMs - lastPrint < DEBUG_INTERVAL_MS) return;
  lastPrint = nowMs;

  int endMin = (startMin + durationMin) % (24 * 60);

  Serial.print("[RTC] ");
  Serial.print(now.hour()); Serial.print(":");
  if (now.minute() < 10) Serial.print("0");
  Serial.print(now.minute());

  Serial.print(" | nowMin=");
  Serial.print(nowMin);

  Serial.print(" | window=");
  Serial.print(startMin);
  Serial.print(" -> ");
  Serial.print(endMin);

  Serial.print(" (");
  Serial.print(durationMin);
  Serial.print("m)");

  Serial.print(" | inWindow=");
  Serial.println(inWindow ? "YES" : "NO");
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

// ====================== Setup / Loop ======================

void setup() {
  // Bring up Serial FIRST so we can see failures
  Serial.begin(115200);
  delay(500);
  Serial.println("\nBOOT: starting...");

  // Ensure off at boot
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // PWM attach (new LEDC API)
  ledcAttach(LED_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(LED_PIN, 0);

  // Quick LED sanity test (bypasses schedule + pot)
  Serial.println("LED test: 25% for 1s");
  ledcWrite(LED_PIN, MAX_DUTY / 4);
  delay(1000);
  ledcWrite(LED_PIN, 0);
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
      ledcWrite(LED_PIN, 0);
      delay(1000);
    }
  }

  // Optional: if RTC lost power, set time once (uncomment for first-time setup)
  //if (rtc.lostPower()) {
    // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.print("date:");
    Serial.print(__DATE__);
    Serial.print("time:");
    Serial.print(__TIME__);
    Serial.print("\n");
  //}

  DateTime now = rtc.now();
  Serial.print("RTC now: ");
  Serial.print(now.year()); Serial.print("-");
  Serial.print(now.month()); Serial.print("-");
  Serial.print(now.day()); Serial.print(" ");
  Serial.print(now.hour()); Serial.print(":");
  if (now.minute() < 10) Serial.print("0");
  Serial.println(now.minute());

  Serial.println("--- Main loop starting ---");
}

void loop() {
  static float filtered = 0.0f;
  static int lastDuty = -1;
  static unsigned long lastStatus = 0;

  // ---- Read pot -> normalized brightness 0..1 ----
  int raw = analogRead(POT_PIN);
  float x = raw / 4095.0f;
  if (INVERT_KNOB) x = 1.0f - x;
  x *= MAX_BRIGHTNESS;

  // Smooth
  filtered = FILTER_ALPHA * filtered + (1.0f - FILTER_ALPHA) * x;

  // ---- RTC gating ----
  DateTime now = rtc.now();
  int nowMin = minutesOfDay(now.hour(), now.minute());

  int startMin = minutesOfDay(ON_HOUR, ON_MINUTE);
  bool allowed = isInWindow(nowMin, startMin, DURATION_MINUTES);

  debugSchedule(now, nowMin, startMin, DURATION_MINUTES, allowed);

  float gate = 0.0f;
  if (allowed) {
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
    ledcWrite(LED_PIN, duty);
    lastDuty = duty;
  }

  // Periodic status line (pot + duty) so you can see what's happening
  if (DEBUG_SCHEDULE && (millis() - lastStatus > STATUS_INTERVAL_MS)) {
    lastStatus = millis();
    Serial.print("raw="); Serial.print(raw);
    Serial.print(" x="); Serial.print(x, 3);
    Serial.print(" filtered="); Serial.print(filtered, 3);
    Serial.print(" allowed="); Serial.print(allowed ? "Y" : "N");
    Serial.print(" gate="); Serial.print(gate, 3);
    Serial.print(" duty="); Serial.println(duty);
  }

  delay(LOOP_DELAY_MS);
}
