## Product Requirement: SHT3x Heater Control

### Purpose

1. The system **shall use the SHT3x internal heater only as a recovery / plausibility tool**, not as a steady-state operating mode. ([Sensirion AG][1])
2. The system **shall keep the heater disabled by default after boot/reset**. ([Sensirion AG][1])

### Activation Criteria

3. The system **shall consider the sensor “wet/stuck”** when:

   * Relative humidity reports **≥ 99.5% RH for N consecutive samples**, where **N ≥ 2**, and
   * Absolute temperature change is small (e.g., **|ΔT| ≤ 0.2°C** across those N samples), to avoid falsely triggering during real rapid environmental change.
4. The system **shall activate the heater only when in “wet/stuck” state** or during an explicit diagnostic routine.

### Heater Pulse Behavior

5. When activated, the system **shall enable the heater via the SHT3x heater-enable command** and disable it afterward. ([Sensirion AG][2])
6. Each heater activation **shall be time-bounded**:

   * **Pulse duration:** **0.1–1.0 seconds** per activation. ([Sensirion AG][3])
   * **Cooldown / settle time:** wait **≥ 5 seconds** after disabling heater before taking the first “trusted” measurement (to reduce thermal bias).
7. The system **shall limit heater duty cycle** to prevent biased readings and unnecessary power draw:

   * No more than **1 pulse per sample interval**, and
   * No more than **M pulses per hour** (recommend **M = 12** unless you have a reason to be more aggressive).

### Measurement Validity Rules

8. Any temperature/RH measurement taken while the heater is enabled **shall be flagged as “heater-influenced” and shall not be used** for control decisions (lighting/watering/etc.).
9. The first measurement after heater disable **shall be treated as “settling”** and may be discarded or flagged, depending on your control loop sensitivity.

### Observability & Diagnostics

10. The system **shall record heater events** (timestamp, duration, trigger reason, RH/T before and after).
11. If heater activations exceed a threshold (e.g., **> M/hour for 2 consecutive hours**), the system **shall raise a “condensation/placement fault” diagnostic** (likely airflow/membrane/placement issue).

### Interface Requirements

12. The firmware **shall implement heater control using the SHT3x I²C commands**:

* Heater enable: **0x30 0x6D**
* Heater disable: **0x30 0x66** ([Sensirion AG][2])

### Non-Goals / Explicit Constraints

13. The heater **shall not be used** to “dry the enclosure,” “control humidity,” or improve normal accuracy—its intent is plausibility checking / recovery only. ([Sensirion AG][1])

[1]: https://sensirion.com/media/documents/051DF50B/65537B73/Sensirion_Humidity_and_Temperature_Sensors_Datasheet_SHT33.pdf?utm_source=chatgpt.com "SHT33-DIS"
[2]: https://sensirion.com/media/documents/213E6A3B/63A5A569/Datasheet_SHT3x_DIS.pdf?utm_source=chatgpt.com "Datasheet SHT3x-DIS"
[3]: https://sensirion.com/media/documents/9B40ED17/664309C8/HT_Transition_Guide_SHT3x_SHT4x.pdf?utm_source=chatgpt.com "SHT3x – SHT4x Transition Guide"

---

## Implementation Plan (Step-by-Step)

1. **Add dependency and wiring assumptions**
   - Add the SHT3x library to `TerrariumLidController/sketch.yaml` (e.g., `Adafruit SHT31 Library`) or document if we will use raw I2C commands.
   - Decide on primary I2C address (0x44) with fallback to 0x45.
   - Decision: use 0x44 primary, 0x45 fallback.

2. **Create optional SHT3x module**
   - Add `TerrariumLidController/SHT3xController.h/.cpp` with a small API: `begin(Wire, addr)`, `isPresent()`, `update(now, millis)`, `getLastReading()`, `getDiag()`.
   - Ensure `begin()` disables the heater on boot and returns `false` if the sensor is not detected.

3. **Define sampling cadence and storage**
   - Add a configurable sample interval (e.g., 2s) and store the last N readings (N>=2).
   - Track `heaterEnabled`, `heaterInfluenced`, and `settling` flags per sample.
   - Decision: sample interval = 2000 ms; history size = 4 readings.

4. **Wet/stuck detection**
   - Implement the trigger: RH >= 99.5% for N consecutive samples and |dT| <= 0.2C across those samples.
   - Require consecutive samples to be non-heater-influenced.
   - Decision: N = 2 consecutive samples.

5. **Heater pulse state machine**
   - Implement a non-blocking heater pulse: enable (0x30 0x6D), run 0.1–1.0s, disable (0x30 0x66).
   - Enforce >=5s cooldown/settle time before the first trusted measurement.
   - Enforce: at most one pulse per sample interval and at most M pulses/hour (default M=12).
   - Decision: pulse duration = 500 ms; cooldown = 5000 ms; M = 12 pulses/hour.

6. **Measurement validity rules**
   - Mark measurements during heater-enabled as "heater-influenced" and exclude from control logic.
   - Mark the first measurement after heater disable as "settling" and discard or flag it.
   - Implementation: expose `getLastTrustedReading()` that returns the most recent non-heater-influenced, non-settling valid sample (or `valid=false` if none).

7. **Event logging and diagnostics**
   - Record heater events (timestamp, duration, trigger reason, RH/T before/after) in a small ring buffer.
   - Detect >M/hour for 2 consecutive hours and raise a `condensationFault` flag.
   - Emit serial logs when heater activates, deactivates, and when a fault is raised.
   - Implementation: ring buffer size = 8 events; timestamp is heater pulse start time; condensation check uses rolling 2-hour window; serial logging uses an optional stream via `setLogStream()`.

8. **Integrate into main loop**
   - Instantiate the SHT3x controller in `TerrariumLidController.ino`, call `begin()` in `setup()`, and `update()` in `loop()`.
   - If the sensor is not present at boot, skip all SHT3x actions with zero overhead beyond a single `isPresent()` check.

9. **Expose status (optional but recommended)**
   - Add a console command (e.g., `sht3x`) to print last reading, wet/stuck state, heater flags, and any active diagnostics.
