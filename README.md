# Ebb & Flow Controller

Arduino firmware for a two-pump ebb-and-flow irrigation system, with float-switch control, manual buttons, DHT11 monitoring, and EEPROM timer persistence.

## Operation

- Automatically starts a fill cycle after four hours of idle time.
- Pump 1 fills until the upper float activates, then Pump 2 drains immediately (no soak delay).
- Pump 2 stops when the lower float activates.
- Each pump phase has a 10-minute safety timeout.
- Temperature and humidity are read every 3 seconds; serial status is printed every 5 seconds at **9600 baud**.
- Relays 3 and 4 remain off.

## Wiring

| Component | Arduino pin | Function |
| --- | --- | --- |
| Stop button | D2 | Stop active pumps; while idle, save timer position |
| Start button | D3 | Start a fill cycle |
| Manual drain button | D4 | Start Pump 2 |
| DHT11 data | D5 | Temperature and humidity |
| Pump 1 relay | D8 | Fill pump |
| Pump 2 relay | D9 | Drain pump |
| Spare relays | D10, D11 | Unused, held off |
| Upper float switch | A1 | LOW indicates upper level reached |
| Lower float switch | A2 | LOW indicates lower level reached |

Buttons and float switches use `INPUT_PULLUP`: connect them between their input pin and GND. Inputs are active LOW. Button debounce is 50 ms; float debounce is 500 ms.

The firmware assumes an **active-LOW relay board** (LOW = on, HIGH = off). Use appropriately rated relay hardware and pump power supplies; do not power pumps directly from Arduino pins.

## Setup and upload

1. Use an Arduino board with the listed pins and support for `EEPROM.h` (for example, an Uno).
2. Install **DHT sensor library by Adafruit** and its **Adafruit Unified Sensor** dependency through the Arduino IDE Library Manager. `EEPROM.h` is supplied by compatible board cores.
3. The sketch is currently named `ebb_flow_2026_season.ino`. Arduino expects the primary sketch filename to match its containing folder. Rename it to `ebb_flow_controller.ino` before opening it in this folder, or place it in a folder named `ebb_flow_2026_season`.
4. Select your board and port, then compile and upload.
5. Open Serial Monitor at **9600 baud** to check inputs and operating state.

## Power-loss recovery

EEPROM addresses 0 and 1 store the cycle state and elapsed idle minutes. During idle, the timer is saved once per minute, with writes skipped when values are unchanged.

- An idle reboot restores the saved timer position; time spent powered off is not counted.
- First boot or an interrupted cycle makes an automatic fill cycle due immediately, unless the upper float is already active.
- Completing a cycle, stopping an active cycle, or reaching a safety timeout resets the four-hour timer.

## Configuration and safety

Edit the pin assignments, timing constants, and `RELAY_ACTIVE_LOW` in the sketch to suit your hardware. DHT11 readings are informational and do not control irrigation.

Test relay polarity, float-switch orientation, and timeout behavior with pumps disconnected before operating with water. A fill is refused if the upper float is already active. Manual draining is not blocked at startup by the lower float, but stops when that float is active.

The stop button is a software control, **not a latched emergency stop**: subsequent button presses or automatic scheduling can start pumps again. Provide independent electrical protection and an emergency disconnect appropriate to your installation.
