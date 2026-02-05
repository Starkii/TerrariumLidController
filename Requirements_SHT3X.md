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
