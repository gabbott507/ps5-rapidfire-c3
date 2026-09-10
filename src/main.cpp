#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include "web_ui.h"

namespace {
constexpr uint8_t R2_PIN = 0;
constexpr char FW_VERSION[] = "0.4.9";
constexpr char AP_SSID[] = "R2-RapidFire";
constexpr char AP_PASSWORD[] = "12345678";
constexpr uint16_t DNS_PORT = 53;
constexpr uint32_t SAMPLE_INTERVAL_US = 2000;
constexpr uint32_t RELEASE_KICK_US = 15000;
constexpr uint32_t BURST_RELEASE_KICK_US = 35000;
constexpr uint32_t FLOAT_SAMPLE_US = 2500;
constexpr uint32_t RELEASE_PROBE_US = 10000;
constexpr uint64_t SLEEP_POLL_US = 250000;
// While asleep the board reboots on every timer wake just to sample the R2 ADC.
// When the controller signal is absent (controller off) that reboot is wasted
// energy, so back off the poll interval until a valid signal returns.
constexpr uint8_t SLEEP_BACKOFF_STEPS = 6;
constexpr uint64_t SLEEP_BACKOFF_MAX_US = 30000000;  // 30 s between idle polls
constexpr int MIN_CAL_SPAN = 150;

enum class Mode : uint8_t { Off, Continuous, Burst };
enum class Phase : uint8_t { Listening, PressPulse, ReleaseKick, FloatSample, WaitCycle, WaitPhysicalRelease };

struct Settings {
  uint32_t magic = 0x52324333;
  uint16_t released = 3000;
  uint16_t pressed = 500;
  uint16_t sps = 10;
  uint16_t burstSps = 8;
  uint16_t burst = 3;
  uint16_t pulseMs = 25;
  uint16_t debounceMs = 15;
  uint8_t pressPoint = 85;
  uint8_t hysteresis = 10;
  uint8_t mode = static_cast<uint8_t>(Mode::Off);
  uint8_t sleepMinutes = 5;  // Zero disables automatic deep sleep.
};

Settings cfg;
Preferences prefs;
WebServer server(80);
DNSServer dns;
Phase phase = Phase::Listening;
uint32_t phaseStartedUs = 0;
uint32_t cycleStartedUs = 0;
uint32_t burstWaitStartedMs = 0;
uint32_t lastSampleUs = 0;
uint32_t lastReleaseProbeUs = 0;
uint32_t candidateSinceMs = 0;
uint16_t adcValue = 0;
uint16_t capturedReleased = 0;
uint16_t capturedPressed = 0;
uint16_t shotsRemaining = 0;
bool triggerPressed = false;
bool candidatePressed = false;
bool sleepRequested = false;
bool otaInProgress = false;
bool rebootRequested = false;
RTC_DATA_ATTR uint8_t sleepPressPolls = 0;
RTC_DATA_ATTR uint8_t sleepBackoffLevel = 0;
uint32_t rebootRequestedMs = 0;
uint32_t sleepRequestedMs = 0;
uint32_t lastActivityMs = 0;
uint16_t previousActivityAdc = 0;
int previousActivityTravel = 0;

void resetEngine();

Mode mode() { return static_cast<Mode>(cfg.mode); }
bool decreasing() { return cfg.pressed < cfg.released; }
const char* modeName() {
  switch (mode()) { case Mode::Continuous: return "continuous"; case Mode::Burst: return "burst"; default: return "off"; }
}

void floatTrigger() { pinMode(R2_PIN, INPUT); }
void drivePressed() { pinMode(R2_PIN, OUTPUT); digitalWrite(R2_PIN, decreasing() ? LOW : HIGH); }
void driveReleased() { pinMode(R2_PIN, OUTPUT); digitalWrite(R2_PIN, decreasing() ? HIGH : LOW); }

uint16_t readMedianAdc(uint8_t count = 5) {
  floatTrigger();
  uint16_t values[9];
  count = constrain(count, 1, 9);
  for (uint8_t i = 0; i < count; ++i) { values[i] = analogRead(R2_PIN); delayMicroseconds(180); }
  for (uint8_t i = 1; i < count; ++i) {
    uint16_t value = values[i]; int8_t j = i - 1;
    while (j >= 0 && values[j] > value) { values[j + 1] = values[j]; --j; }
    values[j + 1] = value;
  }
  return values[count / 2];
}

int travelPercent(uint16_t value) {
  int32_t span = static_cast<int32_t>(cfg.pressed) - cfg.released;
  if (abs(span) < MIN_CAL_SPAN) return 0;
  return constrain(static_cast<int>((static_cast<int32_t>(value) - cfg.released) * 100 / span), 0, 100);
}

void updatePhysicalTrigger(uint16_t value) {
  int currentTravel = travelPercent(value);
  // Normal ADC noise must not continually postpone automatic sleep.
  if (abs(currentTravel - previousActivityTravel) >= 5) lastActivityMs = millis();
  previousActivityTravel = currentTravel;
  previousActivityAdc = value;
  int pct = currentTravel;
  int releasePoint = max(0, static_cast<int>(cfg.pressPoint) - cfg.hysteresis);
  bool raw = triggerPressed ? pct > releasePoint : pct >= cfg.pressPoint;
  uint32_t now = millis();
  if (raw != candidatePressed) { candidatePressed = raw; candidateSinceMs = now; }
  // Releasing should always feel immediate, even if the user selected a
  // larger press debounce to suppress noisy activation.
  uint16_t debounce = raw ? cfg.debounceMs : min<uint16_t>(cfg.debounceMs, 15);
  if (mode() == Mode::Burst && raw) debounce = min<uint16_t>(debounce, 10);
  if (raw != triggerPressed && now - candidateSinceMs >= debounce) triggerPressed = raw;
}

uint64_t backoffIntervalUs(uint8_t level) {
  uint64_t interval = SLEEP_POLL_US;
  for (uint8_t i = 0; i < level; ++i) interval *= 2;
  return interval > SLEEP_BACKOFF_MAX_US ? SLEEP_BACKOFF_MAX_US : interval;
}

void configureWakeSources(uint64_t pollUs = SLEEP_POLL_US) {
  floatTrigger();
  delay(2);
  // R2 is analog and can immediately retrigger digital GPIO wake, so waking
  // is timer/ADC-only. RESET remains the immediate hardware fallback.
  esp_sleep_enable_timer_wakeup(pollUs);
}

void enterDeepSleep() {
  resetEngine();
  sleepPressPolls = 0;
  sleepBackoffLevel = 0;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(30);
  configureWakeSources();
  esp_deep_sleep_start();
}

void runPowerManager() {
  if (otaInProgress) return;
  if (rebootRequested && millis() - rebootRequestedMs >= 750) ESP.restart();
  if (sleepRequested && millis() - sleepRequestedMs >= 250) enterDeepSleep();
  if (!cfg.sleepMinutes || triggerPressed || phase != Phase::Listening) return;
  uint32_t timeoutMs = static_cast<uint32_t>(cfg.sleepMinutes) * 60000UL;
  if (millis() - lastActivityMs < timeoutMs) return;
  // Do not sleep if GPIO already reads at the configured wake level, which
  // would cause an immediate wake loop on an unusual sensor voltage.
  floatTrigger();
  bool wakeLevel = decreasing() ? LOW : HIGH;
  if (digitalRead(R2_PIN) == wakeLevel) { lastActivityMs = millis(); return; }
  enterDeepSleep();
}

void saveSettings() {
  prefs.begin("rapidfire", false); prefs.putBytes("settings", &cfg, sizeof(cfg)); prefs.end();
}

void loadSettings() {
  prefs.begin("rapidfire", true);
  if (prefs.getBytesLength("settings") == sizeof(cfg)) prefs.getBytes("settings", &cfg, sizeof(cfg));
  prefs.end();
  if (cfg.magic != 0x52324333 || cfg.sps < 1 || cfg.sps > 40 || cfg.mode > 2) cfg = Settings{};
  cfg.burstSps = constrain(cfg.burstSps, 1, 12);
  cfg.burst = constrain(cfg.burst, 1, 10);
}

void resetEngine() {
  floatTrigger(); phase = Phase::Listening; triggerPressed = false; candidatePressed = false;
  candidateSinceMs = millis(); shotsRemaining = 0;
}

void beginShot() {
  cycleStartedUs = micros(); drivePressed(); phase = Phase::PressPulse; phaseStartedUs = cycleStartedUs;
}

uint32_t selectedCycleUs() {
  uint16_t rate = mode() == Mode::Burst ? cfg.burstSps : cfg.sps;
  return 1000000UL / constrain(rate, 1, 40);
}
uint32_t releaseKickUs() { return mode() == Mode::Burst ? BURST_RELEASE_KICK_US : RELEASE_KICK_US; }
uint32_t cycleUs() {
  uint32_t requested = selectedCycleUs();
  if (mode() != Mode::Burst) return requested;
  uint32_t safeMinimum = static_cast<uint32_t>(cfg.pulseMs) * 1000UL + releaseKickUs() + FLOAT_SAMPLE_US;
  return max(requested, safeMinimum);
}
uint32_t pressUs() {
  uint32_t requested = static_cast<uint32_t>(cfg.pulseMs) * 1000UL;
  if (mode() == Mode::Burst) return requested;
  uint32_t reserved = releaseKickUs() + FLOAT_SAMPLE_US;
  uint32_t available = cycleUs() > reserved + 5000 ? cycleUs() - reserved : 5000;
  return min(requested, available);
}

void runEngine() {
  uint32_t now = micros();
  if (mode() == Mode::Off) {
    if (phase != Phase::Listening) resetEngine();
    if (now - lastSampleUs >= SAMPLE_INTERVAL_US) { lastSampleUs = now; adcValue = readMedianAdc(3); updatePhysicalTrigger(adcValue); }
    return;
  }
  switch (phase) {
    case Phase::Listening:
      if (now - lastSampleUs >= SAMPLE_INTERVAL_US) {
        lastSampleUs = now; adcValue = readMedianAdc(3); updatePhysicalTrigger(adcValue);
        if (triggerPressed) {
          if (mode() == Mode::Burst && cfg.burst <= 1) {
            // The physical trigger edge is the requested single shot.
            burstWaitStartedMs = millis(); phase = Phase::WaitPhysicalRelease;
          } else {
            // The physical edge is already shot one; generate only the
            // remaining burst transitions.
            shotsRemaining = mode() == Mode::Burst ? cfg.burst - 1 : 0;
            beginShot();
          }
        }
      }
      break;
    case Phase::PressPulse:
      if (now - phaseStartedUs >= pressUs()) {
        driveReleased(); phase = Phase::ReleaseKick; phaseStartedUs = now;
        if (mode() == Mode::Burst && shotsRemaining) --shotsRemaining;
      }
      break;
    case Phase::ReleaseKick:
      if (now - phaseStartedUs >= releaseKickUs()) {
        floatTrigger(); phase = Phase::FloatSample; phaseStartedUs = now;
      }
      break;
    case Phase::FloatSample:
      if (now - phaseStartedUs >= FLOAT_SAMPLE_US) {
        adcValue = readMedianAdc(3); updatePhysicalTrigger(adcValue);
        if (mode() == Mode::Burst && shotsRemaining == 0) {
          burstWaitStartedMs = millis(); phase = Phase::WaitPhysicalRelease;
        }
        else if (mode() == Mode::Continuous && !triggerPressed) resetEngine();
        else { driveReleased(); lastReleaseProbeUs = now; phase = Phase::WaitCycle; }
      }
      break;
    case Phase::WaitCycle:
      if (now - cycleStartedUs >= cycleUs()) {
        // Sample before committing the next continuous shot. If R2 was
        // released during this cycle, stop without producing a trailing edge.
        adcValue = readMedianAdc(3);
        updatePhysicalTrigger(adcValue);
        int releasePoint = max(0, static_cast<int>(cfg.pressPoint) - cfg.hysteresis);
        if (mode() == Mode::Continuous && travelPercent(adcValue) <= releasePoint) resetEngine();
        else beginShot();
      } else if (mode() == Mode::Continuous && now - lastReleaseProbeUs >= RELEASE_PROBE_US) {
        // Briefly float the shared signal to sample the physical trigger,
        // then return to the simulated released level between shots.
        lastReleaseProbeUs = now;
        adcValue = readMedianAdc(3);
        updatePhysicalTrigger(adcValue);
        if (!triggerPressed) resetEngine();
        else driveReleased();
      }
      break;
    case Phase::WaitPhysicalRelease:
      if (now - lastSampleUs >= SAMPLE_INTERVAL_US) {
        lastSampleUs = now; adcValue = readMedianAdc(3); updatePhysicalTrigger(adcValue);
        if (!triggerPressed) resetEngine();
      }
      break;
  }
}

void jsonMessage(const String& message, int code = 200) {
  server.send(code, "application/json", String("{\"message\":\"") + message + "\"}");
}

void setupRoutes() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", WEB_UI); });
  server.on("/api/status", HTTP_GET, [] {
    String out = "{\"version\":\"" + String(FW_VERSION) + "\",\"mode\":\"" + String(modeName()) + "\",\"travel\":" + travelPercent(adcValue) +
      ",\"adc\":" + adcValue + ",\"sps\":" + cfg.sps + ",\"burstSps\":" + cfg.burstSps + ",\"burst\":" + cfg.burst +
      ",\"pulse\":" + cfg.pulseMs + ",\"point\":" + cfg.pressPoint + ",\"hyst\":" + cfg.hysteresis +
      ",\"deb\":" + cfg.debounceMs + ",\"sleep\":" + cfg.sleepMinutes +
      ",\"released\":" + cfg.released + ",\"pressed\":" + cfg.pressed +
      ",\"polarity\":\"" + (decreasing() ? "decreasing" : "increasing") + "\"}";
    server.send(200, "application/json", out);
  });
  server.on("/api/mode", HTTP_POST, [] {
    String value = server.arg("value");
    if (value == "continuous") cfg.mode = static_cast<uint8_t>(Mode::Continuous);
    else if (value == "burst") cfg.mode = static_cast<uint8_t>(Mode::Burst);
    else cfg.mode = static_cast<uint8_t>(Mode::Off);
    resetEngine(); saveSettings(); jsonMessage(String("Mode: ") + modeName());
  });
  server.on("/api/set", HTTP_POST, [] {
    if (server.hasArg("sps")) cfg.sps = constrain(server.arg("sps").toInt(), 1, 40);
    if (server.hasArg("burstSps")) cfg.burstSps = constrain(server.arg("burstSps").toInt(), 1, 12);
    if (server.hasArg("burst")) cfg.burst = constrain(server.arg("burst").toInt(), 1, 10);
    if (server.hasArg("pulse")) cfg.pulseMs = constrain(server.arg("pulse").toInt(), 5, 200);
    if (server.hasArg("point")) cfg.pressPoint = constrain(server.arg("point").toInt(), 5, 100);
    if (server.hasArg("hyst")) cfg.hysteresis = constrain(server.arg("hyst").toInt(), 0, 30);
    if (server.hasArg("debounce")) cfg.debounceMs = constrain(server.arg("debounce").toInt(), 0, 500);
    if (server.hasArg("sleep")) cfg.sleepMinutes = constrain(server.arg("sleep").toInt(), 0, 30);
    lastActivityMs = millis();
    saveSettings(); jsonMessage("Setting saved");
  });
  server.on("/api/cal/released", HTTP_POST, [] { capturedReleased = readMedianAdc(9); jsonMessage(String("Released captured: ") + capturedReleased); });
  server.on("/api/cal/pressed", HTTP_POST, [] { capturedPressed = readMedianAdc(9); jsonMessage(String("Pressed captured: ") + capturedPressed); });
  server.on("/api/cal/apply", HTTP_POST, [] {
    if (abs(static_cast<int>(capturedReleased) - capturedPressed) < MIN_CAL_SPAN) { jsonMessage("Calibration span is too small", 400); return; }
    cfg.released = capturedReleased; cfg.pressed = capturedPressed; saveSettings(); resetEngine(); jsonMessage("Calibration applied");
  });
  server.on("/api/reset-trigger", HTTP_POST, [] { resetEngine(); jsonMessage("Trigger reset"); });
  server.on("/api/defaults", HTTP_POST, [] { cfg = Settings{}; saveSettings(); resetEngine(); jsonMessage("Defaults restored"); });
  server.on("/api/sleep", HTTP_POST, [] {
    jsonMessage("Going to sleep; pull R2 to wake");
    sleepRequested = true; sleepRequestedMs = millis();
  });
  server.on("/api/update", HTTP_POST, [] {
    bool ok = !Update.hasError() && Update.isFinished();
    otaInProgress = false;
    if (ok) {
      server.send(200, "application/json", "{\"message\":\"Update installed; rebooting\"}");
      rebootRequested = true; rebootRequestedMs = millis();
    } else {
      server.send(500, "application/json", String("{\"message\":\"Update failed: ") + Update.errorString() + "\"}");
    }
  }, [] {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaInProgress = true; resetEngine();
      Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (!Update.hasError()) Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!Update.hasError()) Update.end(true);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.abort(); otaInProgress = false;
    }
  });
  server.onNotFound([] { server.sendHeader("Location", "http://192.168.4.1/", true); server.send(302, "text/plain", ""); });
}
}  // namespace

void setup() {
#ifdef R2_WIFI_DIAGNOSTIC
  delay(500);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("R2-Diagnostic", "12345678");
  return;
#endif
  // Give marginal USB supplies time to settle, then use a modest AP transmit
  // level to reduce the current spike that occurs when Wi-Fi starts.
  delay(500);
  analogReadResolution(12);
  analogSetPinAttenuation(R2_PIN, ADC_11db);
  floatTrigger(); loadSettings(); adcValue = readMedianAdc(9);
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    int lower = min(cfg.released, cfg.pressed) - 150;
    int upper = max(cfg.released, cfg.pressed) + 150;
    bool validSignal = adcValue >= max(0, lower) && adcValue <= min(4095, upper);
    bool pressed = validSignal && travelPercent(adcValue) >= cfg.pressPoint;
    if (pressed) {
      sleepPressPolls = min<uint8_t>(sleepPressPolls + 1, 2);
      sleepBackoffLevel = 0;
    } else {
      // No press detected. When the controller signal is also absent (controller
      // off) back off the timer wake so the board stops rebooting every 250 ms
      // indefinitely on battery power; any valid signal resets it immediately.
      sleepPressPolls = 0;
      sleepBackoffLevel = validSignal ? 0 : min<uint8_t>(sleepBackoffLevel + 1, SLEEP_BACKOFF_STEPS);
    }
    if (sleepPressPolls < 2) {
      // Require two valid pressed samples before starting Wi-Fi. This rejects
      // ADC noise and an unpowered controller sensor.
      configureWakeSources(backoffIntervalUs(sleepBackoffLevel));
      esp_deep_sleep_start();
    }
    sleepPressPolls = 0;
  }
  previousActivityAdc = adcValue; previousActivityTravel = travelPercent(adcValue); lastActivityMs = millis();
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dns.start(DNS_PORT, "*", WiFi.softAPIP()); setupRoutes(); server.begin();
}

void loop() {
#ifdef R2_WIFI_DIAGNOSTIC
  delay(10);
  return;
#endif
  runEngine(); dns.processNextRequest(); server.handleClient(); runPowerManager(); delay(0);
}
