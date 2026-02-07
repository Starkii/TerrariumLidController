## SSD1306 display requirements (terrarium controller)

### 1) Display hardware + electrical

* Use **SSD1306 OLED over I²C** on the existing I²C bus (shared with DS3231).
* Default resolution support: **128×64** (also allow 128×32 via config/compile flag).
* Panel color zones are fixed:
  * **Top 16 rows (y=0..15)** are **yellow**
  * **Bottom 48 rows (y=16..63)** are **blue**
* I²C address support: **0x3C** (primary) and **0x3D** (fallback).
* Provide a dedicated **display enable GPIO** (power gate or reset) so firmware can fully shut it down for power/EMI reasons.
* Display update must not disrupt USB-CDC or PWM output.

### 2) Boot + fault behavior

* On boot, show a **splash screen** for 5 seconds: product name + firmware version.
* If display not detected on I²C at boot, firmware must:

  * continue normal operation (headless mode),
  * log a single warning on USB-CDC,
  * retry detection periodically (e.g., every 60s) without blocking.

### 3) User-visible “home” screen (always available)

Must show at minimum:

* **Current brightness** (0–100%) and whether it’s **manual (pot)** or **scheduled**.
* **Light state** (ON/OFF) and **next event** (next on/off time).
* **Current time** (from DS3231) and a small **RTC-valid** icon/state.
* **Current humidity** if available
* **Current temperature** if available
* **Needs watering** Alert
* **Too Cold** Alert
* **Too Hot** Alert

Refresh behavior:

* Max refresh rate: **2 Hz** (every 500 ms).
* Display operations must not block PWM or USB-CDC; if your SSD1306 lib is slow, update via buffered framebuffer and send once per refresh.
* Brightness value should respond to pot changes with **<250 ms perceived latency**.

### 4) Additional screens (rotating or user-selectable)

Navigation requirement (pick one implementation):

* **Auto-rotate** pages every N seconds (default 8s),

### 5) Alerts & notifications (high value)

* If the controller detects any issues, it must flash a persistent banner/icon and disable powering down the screen:

  * Needs Watering
  * Temperature too high or too low

### 6) Display layout + readability

* Must remain legible at **30–60 cm viewing distance**.
* Use a **large font** for the primary number on the home screen (brightness or time).
* Use only monochrome-safe UI elements: icons + 1–2 lines of text.
* Avoid full-screen inversion or rapid blinking (no strobing).
* Place **icons, alerts, and warning banners** in the **yellow zone** (`y=0..15`) whenever possible.
* Place **less critical / routine information** in the **blue zone** (`y=16..63`) whenever possible.

### 7) Burn-in mitigation

* Implement **pixel shifting** (e.g., ±1–2 px) on a slow interval (e.g., every 30–60s) for static screens.
* Provide a **screen dim/off timeout** when idle dim after 2 min, off after 5 min) configurable via CLI.
* When off, the waking criteria is a change in the brightness knob

### 8) Firmware + performance constraints

* Display rendering must be non-blocking or bounded: worst-case render/update must complete in **<10 ms** or be chunked.
* I²C transactions must tolerate bus sharing with DS3231 (no starvation).
* All display strings must be sourced from the same state model used by USB-CDC `status` output (no duplicated logic).


### 9) Configuration + CLI integration

Add CLI commands:

* `display on|off|dim`
* `display flip on|off`
* `display timeout dim <min>` / `display timeout off <min>`

### 10) Testability

Factory test firmware must:

* Detect the SSD1306 on I²C and print address.
* Show a simple **test pattern + text**.
* Verify update loop stability for 30 seconds while PWM is active.

Ok — **one screen only**, with an **orientation (normal/upside-down) setting** that’s **persistent across reboot**.

## Single-screen spec (SSD1306 128×64)

### Orientation

* Two modes: **NORMAL** (0°) and **INVERTED** (180° / upside-down).
* Orientation must be applied to the entire framebuffer (no per-element transforms).
* Orientation setting is stored in **ESP32 NVS** and loaded on boot.
* Default orientation on first boot (no NVS key): **NORMAL**.

### Flip control (no button)

You need a way to toggle it without adding hardware:

**Requirement: CLI-only**

* Add CLI command:

  * `display flip` → toggles NORMAL ↔ INVERTED, saves to NVS immediately
  * `display flip on|off` (optional aliases)
  * `display status` prints current orientation and I²C detect status
* After a flip command, the device must:

  * apply the new orientation within **≤250 ms**
  * print `Display orientation: NORMAL|INVERTED (saved)` to USB-CDC

### Screen layout (pixel-precise)

**Top bar (y=0..7)**

* **(0,0)** USB icon 8×8

  * off = outline, on = solid
* **(10,0)** RTC icon 8×8

  * ok = clock, fault = “!” icon
* **(20,0)** Mode indicator 8×8

  * Pot/manual = `M` icon, Schedule = `S` icon, Override = `!`
* **Right-aligned time** at **(128 - 5*6 = 98, 0)** using 6×8 font

  * format `HH:MM` (always 5 chars)

**Primary value block**

* **Brightness percent (big)** using 8×16 font:

  * Text at **(0, 12)**: `NNN%` (pad to 4 chars, e.g. `" 75%"`, `"100%"`)
* **Bar graph**

  * Outline rect at **(56, 14)** size **68×12**
  * Fill width = `round(66 * brightness/100)` inside outline

**Status lines (6×8 font)**

* Line 1 at **(0, 34)**: light state + source

  * Examples: `ON  POT`, `OFF SCH`, `ON  OVR`
* Line 2 at **(0, 44)**: next event

  * Scheduled: `NEXT OFF 18:30` or `NEXT ON  07:00`
  * Manual: `NEXT --`
* Line 3 at **(0, 54)**: single highest-priority banner text (max 21 chars)
  Priority order:

  1. `BROWNOUT` / `UNDERVOLT`
  2. `RTC MISSING`
  3. `USB POWER LIMITED`
  4. `OK`

### Rendering + update rules

* Home screen refresh at **2 Hz max** (every 500 ms).
* Only redraw if any displayed field changes (time tick counts as a change once per minute if you want to reduce I²C traffic).
* Must not block PWM timing or USB-CDC; worst-case display update should stay **<10 ms** worth of work (chunk I²C if needed).

## NVS keys (explicit)

* Namespace: `ui`
* Key: `oled_flip`
* Type: `u8` (0 = NORMAL, 1 = INVERTED)

## SSD1306 command requirement (implementation detail, but concrete)

* NORMAL uses “COM scan direction” + “segment remap” default
* INVERTED sets:

  * segment remap reversed
  * COM scan direction reversed
    (Every SSD1306 lib exposes this as “rotate 180” / “flip” / “setRotation(2)” equivalent.)

---------

## SSD1306 128×64 — Single Screen Spec (with persistent 180° flip)

### 0) Display modes

* **Orientation**: `NORMAL` (0°) or `INVERTED` (180°).
* Orientation is global (applies to the entire screen).
* Stored in **ESP32 NVS**: namespace `ui`, key `oled_flip` (`u8`: 0=normal, 1=inverted).
* Default if missing: `NORMAL`.

---

# 1) Layout contract (pixel coordinates)

Coordinate system: `(0,0)` top-left, X→right, Y→down.
Font assumptions:

Font L (big numbers): FreeSans24pt7b
Font M (everything else): FreeSans12pt7b

Color zone note for this specific OLED:
* **Yellow region:** `y=0..15` (top 16 px)
* **Blue region:** `y=16..63` (bottom 48 px)
* Prioritization rule:
  * **Yellow:** attention items (status icons, warnings, faults, urgent banners)
  * **Blue:** normal operational data (time, brightness, next event, steady-state info)

### A) Top status bar (Y = 0..7)

**Icons are 8×8 monochrome bitmaps.**

1. **USB icon**

* Position: **(0,0)** size 8×8
* States:

  * `USB_OFF`: outline-only
  * `USB_ON`: filled/solid

2. **RTC icon**

* Position: **(10,0)** size 8×8
* States:

  * `RTC_OK`: clock icon
  * `RTC_FAULT`: `!` icon (or clock with “!” overlay)

3. **MODE icon**

* Position: **(20,0)** size 8×8
* States:

  * `MODE_POT`: letter “M” icon
  * `MODE_SCHED`: letter “S” icon
  * `MODE_OVRD`: `!` icon

4. **Time (HH:MM)**

* Font: **Font M**
* Position: **right-aligned** to x=127 at y=0
* Render at: **(97,0)** because 5 chars × 6 px = 30 px; 128-30=98 (use 97 or 98 consistently; pick **98** if your font is exact 6 px)
* String format: `"HH:MM"` fixed 5 chars

---

### B) Primary brightness readout (Y = 12..31)

1. **Brightness percent (NNN%)**

* Font: **Font L**
* Position: **(0,12)**
* String: **4 chars** exactly, left-padded:

  * `"  0%"`, `" 75%"`, `"100%"` (always width 4)

2. **Brightness bar graph**

* Outline rectangle:

  * Top-left: **(56,14)**
  * Size: **68×12**
  * Border: 1 px
* Fill area:

  * Inside rect: width **66**, height **10**
  * Fill width formula: `fill = round(66 * brightness / 100)`
  * Fill origin: **(57,15)** size `(fill × 10)`

---

### C) Status text lines (Font M)

All lines are **max 21 chars** (21×6=126 px). Hard truncate beyond 21 chars.

1. **Line 1: Light state + control source**

* Position: **(0,34)**
* Format (fixed-ish):

  * `"ON  POT"` / `"OFF POT"`
  * `"ON  SCH"` / `"OFF SCH"`
  * `"ON  OVR"` / `"OFF OVR"`
* Pad to keep it stable if you want: `"ON  POT         "` etc (optional).

2. **Line 2: Next event**

* Position: **(0,44)**
* If scheduled:

  * `"NEXT OFF HH:MM"` or `"NEXT ON  HH:MM"` (note two spaces before HH:MM for ON)
* If manual/pot:

  * `"NEXT --"`
---

# 2) Flip control (no button)

**CLI requirements**

* `display flip`

  * toggles orientation
  * applies within **≤250 ms**
  * persists immediately (write `oled_flip` to NVS)
* `display status` prints:

  * detected address (0x3C/0x3D or “none”)
  * orientation (NORMAL/INVERTED)

---

# 3) Visual flip implementation requirement

* `INVERTED` must be a **true 180° flip** (segment remap + COM scan direction reversal), not “invert pixels.”
* Entire screen content must be readable upside down (icons, text, bar, everything).



// USB icon (8x8)
static const uint8_t ICON_USB[8] = {
  0b00111000,
  0b00101000,
  0b11111110,
  0b00101000,
  0b00111000,
  0b00010000,
  0b00010000,
  0b00010000
};


// RTC / Clock icon (8x8)
static const uint8_t ICON_RTC[8] = {
  0b00111100,
  0b01000010,
  0b10011001,
  0b10001001,
  0b10000001,
  0b10000001,
  0b01000010,
  0b00111100
};

---

## Implementation Plan (Step-by-Step)

- [x] **Lock dependencies and compile-time config**
   - Add SSD1306 display libraries to `TerrariumLidController/sketch.yaml` (e.g., Adafruit SSD1306 + GFX).
   - Add compile-time display config in a new header (`DisplayConfig.h`): width/height (`128x64` default, `128x32` optional), addresses (`0x3C` primary, `0x3D` fallback), refresh interval (`500 ms`), rotation default.
   - Add constants for the two-tone panel zones: yellow `y=0..15`, blue `y=16..63`.

- [x] **Create optional display module**
   - Add `DisplayController.h/.cpp` with a minimal API:
     - `begin(TwoWire&)`, `update(const UiState&, unsigned long nowMs)`, `isPresent()`, `setEnabled(bool)`, `setDimMode(bool)`, `setFlip(bool)`, `toggleFlip()`, `getStatus()`.
   - Boot-time detection tries `0x3C`, then `0x3D`; if missing, remain headless and non-blocking.
   - Retry detect every `60s` while headless.

- [x] **Define shared UI state model**
   - Add a `UiState` struct in a shared header used by both console and display.
   - Include: brightness %, control mode (POT/SCH/OVR), light on/off, next event string, RTC state, humidity/temperature optional values, watering/temperature alerts, USB power status.
   - Refactor existing console status output to source data from this single state model.

- [x] **Implement rendering pipeline**
   - Implement full-screen buffered render with dirty-checking (only push to display when content changes or minute tick changes).
   - Enforce max refresh rate of `2 Hz`.
   - Keep rendering work bounded; no blocking delays in render path.
   - Apply color-priority layout rule:
     - Yellow zone (`y=0..15`): icons + warnings/fault banners.
     - Blue zone (`y=16..63`): normal operational information.

- [x] **Implement single-screen layout contract**
   - Render top status bar icons and right-aligned time in yellow zone.
   - Render primary brightness block and bar graph in blue zone.
   - Render status lines and next-event text in blue zone.
   - Add hard truncation for text fields to avoid overflow.

- [x] **Add persistent orientation (flip)**
   - Add NVS storage: namespace `ui`, key `oled_flip` (`u8`, 0 normal / 1 inverted).
   - Load on boot and apply immediately.
   - Implement `display flip` command with immediate apply (`<=250 ms`) and persisted write.
   - Add `display status` command to report orientation and detect status/address.

- [x] **Add display power/dim behavior**
   - Implement `display on|off|dim`.
   - Implement idle timeout config:
     - `display timeout dim <min>`
     - `display timeout off <min>`
   - Wake from off/dim on potentiometer movement threshold.
   - If active alerts exist, keep display awake (override power-down).

- [x] **Add burn-in mitigation**
   - Implement periodic pixel shift (e.g., `+/-1 px` every `30-60s`) for static content.
   - Keep shifts bounded to avoid clipping key UI elements.

- [x] **Integrate with main loop timing**
   - Instantiate `DisplayController` in `TerrariumLidController.ino`.
   - Call `display.update(uiState, millis())` in loop.
   - Verify no impact on PWM behavior and serial command responsiveness.

- [x] **Factory/self-test mode**
   - Add a test routine/command to:
     - Detect and print display address.
     - Render test pattern + text.
     - Run 30s stability check while PWM remains active.

- [X] **Validation and acceptance checks**
   - Confirm headless fallback works when display absent.
   - Confirm retry detection recovers display if connected after boot.
   - Confirm layout and warning placement follow yellow/blue zone rules.
   - Confirm CLI commands function and persist across reboot.
   - Confirm no regression in RTC, console, SHT3x, and lighting behavior.




