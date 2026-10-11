#include <DHT.h>
#include <EEPROM.h>

// -------------------- DHT11 --------------------

#define DHTPIN 5
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

float temperatureC = NAN;
float humidity = NAN;

unsigned long lastDhtReadTime = 0;
const unsigned long DHT_READ_INTERVAL_MS = 3000UL;

// -------------------- Timers used by EEPROM --------------------
//
// Must be declared before eepromSaveIdle(), because that function uses it.

unsigned long lastCycleStartTime = 0;

// -------------------- EEPROM --------------------
//
// 2 bytes:
//   Address 0: state IDLE or IN_PROGRESS
//   Address 1: elapsed time in minutes since lastCycleStartTime
//
// Boot logic:
//   IDLE        -> restore timer position
//   IN_PROGRESS -> previous cycle was interrupted -> run soon
//   other/0xFF  -> first boot -> run soon
//
// EEPROM writes are minimized: byte is written only if value changed.

#define EEPROM_ADDR_STATE   0
#define EEPROM_ADDR_ELAPSED 1

const uint8_t EEPROM_STATE_IDLE        = 0x01;
const uint8_t EEPROM_STATE_IN_PROGRESS = 0x02;

void eepromWriteByte(int addr, uint8_t value) {
  uint8_t current;
  EEPROM.get(addr, current);

  if (current != value) {
    EEPROM.put(addr, value);
  }
}

// Save IDLE state and elapsed timer position.
void eepromSaveIdle() {
  unsigned long elapsedMs = millis() - lastCycleStartTime;
  uint8_t elapsedMin = (uint8_t)min(elapsedMs / 60000UL, 254UL);

  eepromWriteByte(EEPROM_ADDR_STATE, EEPROM_STATE_IDLE);
  eepromWriteByte(EEPROM_ADDR_ELAPSED, elapsedMin);
}

// Save cycle-in-progress state.
void eepromSaveInProgress() {
  eepromWriteByte(EEPROM_ADDR_STATE, EEPROM_STATE_IN_PROGRESS);
  eepromWriteByte(EEPROM_ADDR_ELAPSED, 0);
}

// -------------------- Pins --------------------

const int RELAY_PUMP_1 = 8;
const int RELAY_PUMP_2 = 9;

const int RELAY_3 = 10;
const int RELAY_4 = 11;

const int BUTTON_STOP = 2;
const int BUTTON_START_CYCLE = 3;
const int BUTTON_PUMP_2 = 4;

const int FLOAT_UP_PIN = A1;
const int FLOAT_DOWN_PIN = A2;

// Active LOW relay board:
// LOW  = relay ON
// HIGH = relay OFF
const bool RELAY_ACTIVE_LOW = true;

// -------------------- Timings --------------------

const unsigned long CYCLE_INTERVAL_MS = 4UL * 60UL * 60UL * 1000UL;

const unsigned long FILL_MAX_RUNTIME_MS  = 10UL * 60UL * 1000UL;
const unsigned long DRAIN_MAX_RUNTIME_MS = 10UL * 60UL * 1000UL;

const unsigned long BUTTON_DEBOUNCE_MS = 50UL;
const unsigned long FLOAT_DEBOUNCE_MS = 500UL;

const unsigned long STATUS_PRINT_INTERVAL_MS = 5000UL;

// Save IDLE timer position to EEPROM max once per minute.
const unsigned long EEPROM_IDLE_SAVE_INTERVAL_MS = 60000UL;

// -------------------- State --------------------

enum SystemState {
  IDLE,
  FILLING,
  DRAINING
};

SystemState state = IDLE;

unsigned long currentPhaseStartTime = 0;
unsigned long lastStatusPrintTime = 0;
unsigned long lastIdleEepromSaveTime = 0;

// -------------------- Button debounce state --------------------

bool lastStopReading = HIGH;
bool lastCycleReading = HIGH;
bool lastPump2Reading = HIGH;

bool stableStopState = HIGH;
bool stableCycleState = HIGH;
bool stablePump2State = HIGH;

unsigned long lastStopChange = 0;
unsigned long lastCycleChange = 0;
unsigned long lastPump2Change = 0;

// -------------------- Float debounce state --------------------

bool lastFloatUpReading = HIGH;
bool lastFloatDownReading = HIGH;

bool stableFloatUpState = HIGH;
bool stableFloatDownState = HIGH;

unsigned long lastFloatUpChange = 0;
unsigned long lastFloatDownChange = 0;

// -------------------- Relay helpers --------------------

void setRelay(int relayPin, bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(relayPin, on ? LOW : HIGH);
  } else {
    digitalWrite(relayPin, on ? HIGH : LOW);
  }
}

void pumpsOff() {
  setRelay(RELAY_PUMP_1, false);
  setRelay(RELAY_PUMP_2, false);
}

void allRelaysOff() {
  setRelay(RELAY_PUMP_1, false);
  setRelay(RELAY_PUMP_2, false);
  setRelay(RELAY_3, false);
  setRelay(RELAY_4, false);
}

bool pump1On() {
  return state == FILLING;
}

bool pump2On() {
  return state == DRAINING;
}

// -------------------- Input debounce --------------------

bool readDebouncedButton(
  int pin,
  bool &lastReading,
  bool &stableState,
  unsigned long &lastChangeTime
) {
  bool currentReading = digitalRead(pin);

  if (currentReading != lastReading) {
    lastChangeTime = millis();
    lastReading = currentReading;
  }

  if ((millis() - lastChangeTime) > BUTTON_DEBOUNCE_MS) {
    if (stableState != currentReading) {
      stableState = currentReading;

      // Buttons use INPUT_PULLUP:
      // HIGH = not pressed
      // LOW  = pressed
      if (stableState == LOW) {
        return true;
      }
    }
  }

  return false;
}

// -------------------- Float debounce --------------------

void updateFloatDebounce() {
  unsigned long now = millis();

  bool currentUp = digitalRead(FLOAT_UP_PIN);
  bool currentDown = digitalRead(FLOAT_DOWN_PIN);

  if (currentUp != lastFloatUpReading) {
    lastFloatUpChange = now;
    lastFloatUpReading = currentUp;
  }

  if ((now - lastFloatUpChange) > FLOAT_DEBOUNCE_MS) {
    stableFloatUpState = currentUp;
  }

  if (currentDown != lastFloatDownReading) {
    lastFloatDownChange = now;
    lastFloatDownReading = currentDown;
  }

  if ((now - lastFloatDownChange) > FLOAT_DEBOUNCE_MS) {
    stableFloatDownState = currentDown;
  }
}

// Float switches use INPUT_PULLUP:
// HIGH = inactive/open
// LOW  = active/closed
bool floatUpActive() {
  return stableFloatUpState == LOW;
}

bool floatDownActive() {
  return stableFloatDownState == LOW;
}

// -------------------- DHT --------------------

void readDhtIfNeeded() {
  unsigned long now = millis();

  if (now - lastDhtReadTime < DHT_READ_INTERVAL_MS) {
    return;
  }

  lastDhtReadTime = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && !isnan(t)) {
    // Ignore obviously broken DHT11 startup glitches.
    if (t > -10.0 && t < 60.0 && h >= 0.0 && h <= 100.0) {
      humidity = h;
      temperatureC = t;
    }
  } else {
    Serial.println("DHT11 read failed");
  }
}

// -------------------- State text --------------------

const char* stateName() {
  switch (state) {
    case IDLE:
      return "IDLE";
    case FILLING:
      return "FILLING";
    case DRAINING:
      return "DRAINING";
    default:
      return "?";
  }
}

unsigned long minutesUntilNextCycle() {
  unsigned long now = millis();

  if (state != IDLE) {
    return 0;
  }

  unsigned long elapsed = now - lastCycleStartTime;

  if (elapsed >= CYCLE_INTERVAL_MS) {
    return 0;
  }

  unsigned long remainingMs = CYCLE_INTERVAL_MS - elapsed;

  // Round up, so it does not show 0 min when there are still seconds left.
  return (remainingMs + 59999UL) / 60000UL;
}

// -------------------- Cycle control --------------------

void startFillCycle(const char* reason) {
  if (floatUpActive()) {
    Serial.println("Cycle not started: A1 float-up is already active");
    state = IDLE;
    pumpsOff();
    return;
  }

  unsigned long now = millis();

  currentPhaseStartTime = now;
  lastCycleStartTime = now;

  state = FILLING;

  setRelay(RELAY_PUMP_1, true);
  setRelay(RELAY_PUMP_2, false);

  // EEPROM: mark cycle as running.
  eepromSaveInProgress();

  Serial.print("Cycle started, FILLING: ");
  Serial.println(reason);
}

void startDrainPhase(const char* reason) {
  // Important:
  // Do NOT block drain startup based on A2.
  // A2 is used only to stop the drain pump when the low water level is reached.

  unsigned long now = millis();

  currentPhaseStartTime = now;
  lastCycleStartTime = now;

  state = DRAINING;

  setRelay(RELAY_PUMP_1, false);
  setRelay(RELAY_PUMP_2, true);

  // EEPROM: drain is also an in-progress cycle state.
  eepromSaveInProgress();

  Serial.print("DRAINING started: ");
  Serial.println(reason);
}

void stopAll(const char* reason) {
  state = IDLE;
  pumpsOff();

  // Reset cycle timer so automatic cycle does not start immediately.
  lastCycleStartTime = millis();

  // EEPROM: save fresh IDLE timer.
  eepromSaveIdle();

  Serial.print("All pumps stopped, returning to IDLE: ");
  Serial.println(reason);
}

void finishCycle(const char* reason) {
  state = IDLE;
  pumpsOff();

  // Reset cycle timer after completed fill+drain cycle.
  // Next automatic cycle will happen 4 hours from now.
  lastCycleStartTime = millis();

  // EEPROM: save fresh IDLE timer.
  eepromSaveIdle();

  Serial.print("Cycle finished: ");
  Serial.println(reason);
}

// -------------------- Serial status --------------------

void printStatusIfNeeded() {
  unsigned long now = millis();

  if (now - lastStatusPrintTime < STATUS_PRINT_INTERVAL_MS) {
    return;
  }

  lastStatusPrintTime = now;

  Serial.print("State: ");
  Serial.print(stateName());

  Serial.print(" | P1: ");
  Serial.print(pump1On() ? "ON" : "OFF");

  Serial.print(" | P2: ");
  Serial.print(pump2On() ? "ON" : "OFF");

  Serial.print(" | A1: ");
  Serial.print(floatUpActive() ? "ACTIVE" : "inactive");

  Serial.print(" | A2: ");
  Serial.print(floatDownActive() ? "ACTIVE" : "inactive");

  Serial.print(" | raw A1: ");
  Serial.print(digitalRead(FLOAT_UP_PIN) == LOW ? "LOW" : "HIGH");

  Serial.print(" | raw A2: ");
  Serial.print(digitalRead(FLOAT_DOWN_PIN) == LOW ? "LOW" : "HIGH");

  Serial.print(" | T: ");
  if (isnan(temperatureC)) {
    Serial.print("--");
  } else {
    Serial.print(temperatureC);
  }

  Serial.print(" C | H: ");
  if (isnan(humidity)) {
    Serial.print("--");
  } else {
    Serial.print(humidity);
  }

  Serial.print(" % | Next cycle: ");
  Serial.print(minutesUntilNextCycle());
  Serial.println(" min");
}

// -------------------- EEPROM idle periodic save --------------------

void saveIdleTimerPeriodically() {
  unsigned long now = millis();

  if (state != IDLE) {
    return;
  }

  if (now - lastIdleEepromSaveTime < EEPROM_IDLE_SAVE_INTERVAL_MS) {
    return;
  }

  lastIdleEepromSaveTime = now;
  eepromSaveIdle();
}

// -------------------- Setup --------------------

void setup() {
  Serial.begin(9600);

  // Set relay pins HIGH before OUTPUT mode to reduce relay flicker on boot.
  digitalWrite(RELAY_PUMP_1, HIGH);
  digitalWrite(RELAY_PUMP_2, HIGH);
  digitalWrite(RELAY_3, HIGH);
  digitalWrite(RELAY_4, HIGH);

  pinMode(RELAY_PUMP_1, OUTPUT);
  pinMode(RELAY_PUMP_2, OUTPUT);
  pinMode(RELAY_3, OUTPUT);
  pinMode(RELAY_4, OUTPUT);

  allRelaysOff();

  pinMode(BUTTON_STOP, INPUT_PULLUP);
  pinMode(BUTTON_START_CYCLE, INPUT_PULLUP);
  pinMode(BUTTON_PUMP_2, INPUT_PULLUP);

  pinMode(FLOAT_UP_PIN, INPUT_PULLUP);
  pinMode(FLOAT_DOWN_PIN, INPUT_PULLUP);

  // Initialize float states.
  lastFloatUpReading = digitalRead(FLOAT_UP_PIN);
  lastFloatDownReading = digitalRead(FLOAT_DOWN_PIN);

  stableFloatUpState = lastFloatUpReading;
  stableFloatDownState = lastFloatDownReading;

  lastFloatUpChange = millis();
  lastFloatDownChange = millis();

  dht.begin();

  // EEPROM restore:
  //
  // IDLE:
  //   restore elapsed timer position.
  //
  // IN_PROGRESS or first boot:
  //   force automatic cycle soon by making timer already expired.
  {
    uint8_t savedState;
    uint8_t savedElapsed;

    EEPROM.get(EEPROM_ADDR_STATE, savedState);
    EEPROM.get(EEPROM_ADDR_ELAPSED, savedElapsed);

    if (savedState == EEPROM_STATE_IDLE) {
      lastCycleStartTime = millis() - (unsigned long)savedElapsed * 60000UL;

      Serial.print("EEPROM: restored timer, ");
      Serial.print(savedElapsed);
      Serial.println(" min elapsed before reboot");
    } else {
      lastCycleStartTime = millis() - CYCLE_INTERVAL_MS;

      Serial.println("EEPROM: cycle was in progress or first boot, starting cycle soon");
    }
  }

  state = IDLE;
  lastIdleEepromSaveTime = millis();

  Serial.println("Ebb & Flow production controller started");
  Serial.println("D2=STOP ALL PUMPS, D3=START CYCLE, D4=MANUAL DRAIN");
  Serial.println("A1=float-up, A2=float-down");
  Serial.println("Float debounce enabled");
  Serial.println("Relay logic: active LOW");
}

// -------------------- Main loop --------------------

void loop() {
  unsigned long now = millis();

  updateFloatDebounce();
  readDhtIfNeeded();

  bool stopPressed = readDebouncedButton(
    BUTTON_STOP,
    lastStopReading,
    stableStopState,
    lastStopChange
  );

  bool cyclePressed = readDebouncedButton(
    BUTTON_START_CYCLE,
    lastCycleReading,
    stableCycleState,
    lastCycleChange
  );

  bool pump2Pressed = readDebouncedButton(
    BUTTON_PUMP_2,
    lastPump2Reading,
    stablePump2State,
    lastPump2Change
  );

  if (stopPressed) {
    if (state == IDLE) {
      // Pumps are already off.
      // Save current timer position manually before reboot/debug.
      eepromSaveIdle();
      Serial.println("EEPROM: timer position saved manually");
    } else {
      stopAll("manual stop button");
    }
  }

  if (cyclePressed) {
    startFillCycle("manual button");
  }

  if (pump2Pressed) {
    startDrainPhase("manual Pump 2 button");
  }

  if (state == IDLE && (now - lastCycleStartTime >= CYCLE_INTERVAL_MS)) {
    startFillCycle("automatic 4-hour interval");
  }

  switch (state) {
    case IDLE:
      pumpsOff();
      break;

    case FILLING:
      if (floatUpActive()) {
        Serial.println("A1 float-up detected: stopping Pump 1");
        startDrainPhase("Pump 1 fill completed");
      } else if (now - currentPhaseStartTime >= FILL_MAX_RUNTIME_MS) {
        stopAll("fill safety timeout - A1 was not reached");
      }
      break;

    case DRAINING:
      if (floatDownActive()) {
        finishCycle("A2 float-down detected");
      } else if (now - currentPhaseStartTime >= DRAIN_MAX_RUNTIME_MS) {
        stopAll("drain safety timeout - A2 was not reached");
      }
      break;
  }

  saveIdleTimerPeriodically();
  printStatusIfNeeded();
}