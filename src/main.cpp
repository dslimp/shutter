#include <Arduino.h>
#include <AccelStepper.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <Updater.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include <memory>

#include "ShutterMath.h"

namespace cfg {
constexpr char kFirmwareVersion[] = "0.1.2-esp8266";
constexpr char kApSsid[] = "Shutter-Setup";
constexpr char kApPass[] = "shutter123";
constexpr uint16_t kApPortalTimeoutSec = 180;
constexpr char kDefaultWifiSsid[] = "nh";
constexpr char kDefaultWifiPass[] = "Fx110011";
constexpr uint16_t kDefaultWifiConnectTimeoutMs = 12000;
constexpr char kDefaultFirmwareRepo[] = "dslimp/shutter";
constexpr char kDefaultFirmwareAssetName[] = "firmware.bin";
constexpr char kDefaultFirmwareFsAssetName[] = "littlefs.bin";
constexpr char kStateFile[] = "/state.json";
constexpr uint32_t kSaveIntervalMs = 5000;
constexpr long kMinTravelSteps = 100;
constexpr long kMaxTravelSteps = 300000;
constexpr float kMinSpeed = 80.0f;
constexpr float kMaxSpeed = 2500.0f;
constexpr float kMinAccel = 40.0f;
constexpr float kMaxAccel = 6000.0f;
constexpr uint16_t kMaxCoilHoldMs = 10000;

// 28BYJ-48 + ULN2003 for Wemos ESP-WROOM-02 board
constexpr uint8_t kPinIn1 = 5;   // GPIO5
constexpr uint8_t kPinIn2 = 4;   // GPIO4
constexpr uint8_t kPinIn3 = 14;  // GPIO14
constexpr uint8_t kPinIn4 = 12;  // GPIO12
}  // namespace cfg

struct ControllerState {
  long travelSteps = 12000;
  long currentPosition = 0;
  bool calibrated = false;
  bool reverseDirection = false;
  bool wifiModemSleep = false;
  float maxSpeed = 700.0f;
  float acceleration = 350.0f;
  uint16_t coilHoldMs = 500;
};

ESP8266WebServer server(80);
WiFiManager wifiManager;
AccelStepper stepper(AccelStepper::HALF4WIRE, cfg::kPinIn1, cfg::kPinIn3, cfg::kPinIn2, cfg::kPinIn4);

ControllerState state;
long targetPosition = 0;
bool settingsDirty = false;
bool outputsReleased = false;
long lastSavedPosition = -1;
uint32_t lastSaveMs = 0;
uint32_t motionStoppedAtMs = 0;
String firmwareRepo = cfg::kDefaultFirmwareRepo;
String firmwareAssetName = cfg::kDefaultFirmwareAssetName;
String firmwareFsAssetName = cfg::kDefaultFirmwareFsAssetName;

bool isValidGithubRepo(const String& value) {
  const int slash = value.indexOf('/');
  const int length = static_cast<int>(value.length());
  if (slash <= 0 || slash >= length - 1) return false;
  return value.indexOf('/', slash + 1) < 0;
}

void normalizeFirmwareConfig() {
  firmwareRepo.trim();
  firmwareAssetName.trim();
  firmwareFsAssetName.trim();
  if (firmwareRepo.length() == 0) firmwareRepo = cfg::kDefaultFirmwareRepo;
  if (firmwareAssetName.length() == 0) firmwareAssetName = cfg::kDefaultFirmwareAssetName;
  if (firmwareFsAssetName.length() == 0) firmwareFsAssetName = cfg::kDefaultFirmwareFsAssetName;
}

int directionSign() { return shutter::math::directionSign(state.reverseDirection); }

long logicalToRaw(long logicalPos) {
  return shutter::math::logicalToRaw(logicalPos, state.reverseDirection);
}

long rawToLogical(long rawPos) {
  return shutter::math::rawToLogical(rawPos, state.reverseDirection);
}

long clampLogicalPosition(long pos) {
  return shutter::math::clampLong(pos, 0, state.travelSteps);
}

long currentLogicalPosition() {
  return clampLogicalPosition(rawToLogical(stepper.currentPosition()));
}

void applyStepperSettings() {
  stepper.setMaxSpeed(state.maxSpeed);
  stepper.setAcceleration(state.acceleration);
}

void applyWiFiPowerMode() {
  WiFi.setSleepMode(state.wifiModemSleep ? WIFI_MODEM_SLEEP : WIFI_NONE_SLEEP);
}

void markDirty() { settingsDirty = true; }

bool parseJsonBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) return false;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  return !err;
}

void sendJsonDocument(int code, const JsonDocument& doc) {
  String payload;
  serializeJson(doc, payload);
  server.send(code, "application/json", payload);
}

void sendError(const char* message, int code = 400) {
  StaticJsonDocument<192> doc;
  doc["ok"] = false;
  doc["error"] = message;
  sendJsonDocument(code, doc);
}

void fillStateJson(JsonObject root) {
  const long pos = currentLogicalPosition();
  const long tgt = clampLogicalPosition(targetPosition);
  const bool moving = stepper.distanceToGo() != 0;
  uint32_t adcSum = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    adcSum += analogRead(A0);
    delay(1);
  }
  const uint16_t a0Raw = static_cast<uint16_t>(adcSum / 8);

  const float posPercent = shutter::math::stepsToPercent(pos, state.travelSteps);
  const float tgtPercent = shutter::math::stepsToPercent(tgt, state.travelSteps);

  const long logicalDistanceToGo = rawToLogical(stepper.distanceToGo());
  const char* motion = "idle";
  if (moving) {
    motion = logicalDistanceToGo > 0 ? "closing" : "opening";
  }

  root["ok"] = true;
  root["version"] = cfg::kFirmwareVersion;
  root["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("0.0.0.0");
  root["ssid"] = WiFi.SSID();
  root["rssi"] = WiFi.RSSI();
  root["a0Raw"] = a0Raw;
  root["uptimeSec"] = millis() / 1000;
  root["motion"] = motion;
  root["moving"] = moving;
  root["calibrated"] = state.calibrated;
  root["positionSteps"] = pos;
  root["targetSteps"] = tgt;
  root["travelSteps"] = state.travelSteps;
  root["positionPercent"] = posPercent;
  root["targetPercent"] = tgtPercent;
  root["reverseDirection"] = state.reverseDirection;
  root["wifiModemSleep"] = state.wifiModemSleep;
  root["maxSpeed"] = state.maxSpeed;
  root["acceleration"] = state.acceleration;
  root["coilHoldMs"] = state.coilHoldMs;
  root["rawPosition"] = stepper.currentPosition();
  root["firmwareRepo"] = firmwareRepo;
  root["firmwareAssetName"] = firmwareAssetName;
  root["firmwareFsAssetName"] = firmwareFsAssetName;
}

bool loadState() {
  if (!LittleFS.exists(cfg::kStateFile)) return false;

  File file = LittleFS.open(cfg::kStateFile, "r");
  if (!file) return false;

  StaticJsonDocument<1024> doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) return false;

  state.travelSteps = shutter::math::clampLong(doc["travelSteps"] | state.travelSteps, cfg::kMinTravelSteps, cfg::kMaxTravelSteps);
  state.currentPosition = shutter::math::clampLong(doc["currentPosition"] | state.currentPosition, 0, state.travelSteps);
  state.calibrated = doc["calibrated"] | state.calibrated;
  state.reverseDirection = doc["reverseDirection"] | state.reverseDirection;
  state.wifiModemSleep = doc["wifiModemSleep"] | state.wifiModemSleep;
  state.maxSpeed = shutter::math::clampFloat(doc["maxSpeed"] | state.maxSpeed, cfg::kMinSpeed, cfg::kMaxSpeed);
  state.acceleration = shutter::math::clampFloat(doc["acceleration"] | state.acceleration, cfg::kMinAccel, cfg::kMaxAccel);
  state.coilHoldMs = static_cast<uint16_t>(shutter::math::clampLong(doc["coilHoldMs"] | state.coilHoldMs, 0, cfg::kMaxCoilHoldMs));
  firmwareRepo = String(static_cast<const char*>(doc["firmwareRepo"] | firmwareRepo.c_str()));
  firmwareAssetName = String(static_cast<const char*>(doc["firmwareAssetName"] | firmwareAssetName.c_str()));
  firmwareFsAssetName = String(static_cast<const char*>(doc["firmwareFsAssetName"] | firmwareFsAssetName.c_str()));
  normalizeFirmwareConfig();
  return true;
}

bool saveState(bool force = false) {
  const long pos = currentLogicalPosition();
  const uint32_t now = millis();

  if (!force) {
    if (!settingsDirty && pos == lastSavedPosition) return true;
    if (now - lastSaveMs < cfg::kSaveIntervalMs) return true;
  }

  StaticJsonDocument<1024> doc;
  doc["travelSteps"] = state.travelSteps;
  doc["currentPosition"] = pos;
  doc["calibrated"] = state.calibrated;
  doc["reverseDirection"] = state.reverseDirection;
  doc["wifiModemSleep"] = state.wifiModemSleep;
  doc["maxSpeed"] = state.maxSpeed;
  doc["acceleration"] = state.acceleration;
  doc["coilHoldMs"] = state.coilHoldMs;
  doc["firmwareRepo"] = firmwareRepo;
  doc["firmwareAssetName"] = firmwareAssetName;
  doc["firmwareFsAssetName"] = firmwareFsAssetName;

  File file = LittleFS.open(cfg::kStateFile, "w");
  if (!file) return false;
  if (serializeJson(doc, file) == 0) {
    file.close();
    return false;
  }

  file.close();
  lastSavedPosition = pos;
  lastSaveMs = now;
  settingsDirty = false;
  return true;
}

void enableMotorOutputs() {
  if (!outputsReleased) return;
  stepper.enableOutputs();
  outputsReleased = false;
}

void disableMotorOutputs() {
  if (outputsReleased) return;
  stepper.disableOutputs();
  outputsReleased = true;
}

void setTargetPosition(long logicalTarget) {
  targetPosition = clampLogicalPosition(logicalTarget);
  enableMotorOutputs();
  stepper.moveTo(logicalToRaw(targetPosition));
  markDirty();
}

void stopMotor() {
  const long rawNow = stepper.currentPosition();
  stepper.setCurrentPosition(rawNow);
  stepper.moveTo(rawNow);
  targetPosition = clampLogicalPosition(rawToLogical(rawNow));
  motionStoppedAtMs = millis();
  markDirty();
}

void calibrateSetTop() {
  stepper.setCurrentPosition(logicalToRaw(0));
  targetPosition = 0;
  stepper.moveTo(logicalToRaw(targetPosition));
  state.currentPosition = 0;
  markDirty();
}

bool calibrateSetBottom() {
  long measured = rawToLogical(stepper.currentPosition());
  if (measured < 0) measured = -measured;
  measured = shutter::math::clampLong(measured, cfg::kMinTravelSteps, cfg::kMaxTravelSteps);

  if (measured < cfg::kMinTravelSteps) return false;

  state.travelSteps = measured;
  stepper.setCurrentPosition(logicalToRaw(state.travelSteps));
  targetPosition = state.travelSteps;
  stepper.moveTo(logicalToRaw(targetPosition));
  state.currentPosition = state.travelSteps;
  state.calibrated = true;
  markDirty();
  return true;
}

void calibrateJog(long logicalDelta) {
  if (logicalDelta == 0) return;
  const long rawDelta = logicalDelta * directionSign();
  const long rawTarget = stepper.currentPosition() + rawDelta;
  enableMotorOutputs();
  stepper.moveTo(rawTarget);
  markDirty();
}

void handleApiState() {
  StaticJsonDocument<768> doc;
  fillStateJson(doc.to<JsonObject>());
  sendJsonDocument(200, doc);
}

void handleApiMove() {
  StaticJsonDocument<384> body;
  if (!parseJsonBody(body)) {
    sendError("invalid json");
    return;
  }

  const char* action = body["action"] | "";

  if (strcmp(action, "open") == 0) {
    setTargetPosition(0);
  } else if (strcmp(action, "close") == 0) {
    setTargetPosition(state.travelSteps);
  } else if (strcmp(action, "stop") == 0) {
    stopMotor();
  } else if (strcmp(action, "set") == 0) {
    const float percent = body["percent"] | -1.0f;
    if (percent < 0.0f || percent > 100.0f) {
      sendError("percent must be between 0 and 100");
      return;
    }
    const long tgt = shutter::math::percentToSteps(percent, state.travelSteps);
    setTargetPosition(tgt);
  } else if (strcmp(action, "jog") == 0) {
    const long delta = body["steps"] | 0;
    if (delta == 0) {
      sendError("steps must be non-zero");
      return;
    }
    setTargetPosition(currentLogicalPosition() + delta);
  } else {
    sendError("unknown action");
    return;
  }

  handleApiState();
}

void handleApiCalibrate() {
  StaticJsonDocument<256> body;
  if (!parseJsonBody(body)) {
    sendError("invalid json");
    return;
  }

  const char* action = body["action"] | "";
  if (strcmp(action, "set_top") == 0) {
    calibrateSetTop();
  } else if (strcmp(action, "set_bottom") == 0) {
    if (!calibrateSetBottom()) {
      sendError("failed to set bottom");
      return;
    }
  } else if (strcmp(action, "jog") == 0) {
    const long delta = body["steps"] | 0;
    if (delta == 0) {
      sendError("steps must be non-zero");
      return;
    }
    calibrateJog(delta);
  } else if (strcmp(action, "reset") == 0) {
    state.calibrated = false;
    markDirty();
  } else {
    sendError("unknown action");
    return;
  }

  handleApiState();
}

void handleApiSettings() {
  StaticJsonDocument<512> body;
  if (!parseJsonBody(body)) {
    sendError("invalid json");
    return;
  }

  const long logicalPosBefore = currentLogicalPosition();
  const long logicalTargetBefore = targetPosition;

  if (body.containsKey("reverseDirection")) {
    state.reverseDirection = body["reverseDirection"].as<bool>();
  }
  if (body.containsKey("wifiModemSleep")) {
    state.wifiModemSleep = body["wifiModemSleep"].as<bool>();
  }
  if (body.containsKey("maxSpeed")) {
    state.maxSpeed = shutter::math::clampFloat(body["maxSpeed"].as<float>(), cfg::kMinSpeed, cfg::kMaxSpeed);
  }
  if (body.containsKey("acceleration")) {
    state.acceleration = shutter::math::clampFloat(body["acceleration"].as<float>(), cfg::kMinAccel, cfg::kMaxAccel);
  }
  if (body.containsKey("coilHoldMs")) {
    state.coilHoldMs = static_cast<uint16_t>(shutter::math::clampLong(body["coilHoldMs"].as<long>(), 0, cfg::kMaxCoilHoldMs));
  }
  if (body.containsKey("travelSteps")) {
    state.travelSteps = shutter::math::clampLong(body["travelSteps"].as<long>(), cfg::kMinTravelSteps, cfg::kMaxTravelSteps);
  }

  applyStepperSettings();
  applyWiFiPowerMode();

  const long clampedPos = clampLogicalPosition(logicalPosBefore);
  targetPosition = clampLogicalPosition(logicalTargetBefore);

  stepper.setCurrentPosition(logicalToRaw(clampedPos));
  stepper.moveTo(logicalToRaw(targetPosition));

  markDirty();
  if (!saveState(true)) {
    sendError("failed to persist state", 500);
    return;
  }
  handleApiState();
}

void fillFirmwareConfig(JsonObject root) {
  normalizeFirmwareConfig();
  root["ok"] = true;
  root["firmwareRepo"] = firmwareRepo;
  root["firmwareAssetName"] = firmwareAssetName;
  root["firmwareFsAssetName"] = firmwareFsAssetName;
}

bool performHttpOta(const String& url, bool updateFilesystem, String* errorMessage) {
  if (errorMessage) errorMessage->clear();

  const bool isHttps = url.startsWith("https://");
  const bool isHttp = url.startsWith("http://");
  if (!isHttps && !isHttp) {
    if (errorMessage) *errorMessage = "unsupported url scheme";
    return false;
  }

  HTTPClient http;
  http.setTimeout(60000);
  WiFiClient plainClient;
  std::unique_ptr<BearSSL::WiFiClientSecure> secureClient(new BearSSL::WiFiClientSecure());
  if (isHttps) secureClient->setInsecure();

  const bool beginOk = isHttps ? http.begin(*secureClient, url) : http.begin(plainClient, url);
  if (!beginOk) {
    if (errorMessage) *errorMessage = "http begin failed";
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (errorMessage) *errorMessage = String("http code: ") + String(code);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    if (errorMessage) *errorMessage = "content length missing";
    http.end();
    return false;
  }

#if defined(U_FS)
  const int updateCommand = updateFilesystem ? U_FS : U_FLASH;
#else
  const int updateCommand = updateFilesystem ? U_SPIFFS : U_FLASH;
#endif

  if (!Update.begin(static_cast<size_t>(contentLength), updateCommand)) {
    if (errorMessage) *errorMessage = String(Update.getError());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  const size_t written = Update.writeStream(*stream);
  if (written != static_cast<size_t>(contentLength)) {
    if (errorMessage) *errorMessage = "written bytes mismatch";
    http.end();
    return false;
  }

  if (!Update.end()) {
    if (errorMessage) *errorMessage = String(Update.getError());
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    if (errorMessage) *errorMessage = "update not finished";
    http.end();
    return false;
  }

  http.end();
  return true;
}

bool probeGithubTcp(String* errorMessage) {
  if (errorMessage) errorMessage->clear();

  IPAddress ip;
  if (!WiFi.hostByName("github.com", ip)) {
    if (errorMessage) *errorMessage = "dns lookup failed";
    return false;
  }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);
  if (!client.connect(ip, 443)) {
    if (errorMessage) *errorMessage = "tcp connect failed";
    return false;
  }
  client.stop();
  return true;
}


void handleApiFirmwareConfigGet() {
  StaticJsonDocument<384> doc;
  fillFirmwareConfig(doc.to<JsonObject>());
  sendJsonDocument(200, doc);
}

void handleApiFirmwareConfigPost() {
  StaticJsonDocument<512> body;
  if (!parseJsonBody(body)) {
    sendError("invalid json");
    return;
  }

  if (body.containsKey("firmwareRepo")) {
    firmwareRepo = String(static_cast<const char*>(body["firmwareRepo"] | ""));
  }
  if (body.containsKey("firmwareAssetName")) {
    firmwareAssetName = String(static_cast<const char*>(body["firmwareAssetName"] | ""));
  }
  if (body.containsKey("firmwareFsAssetName")) {
    firmwareFsAssetName = String(static_cast<const char*>(body["firmwareFsAssetName"] | ""));
  }

  normalizeFirmwareConfig();
  if (!isValidGithubRepo(firmwareRepo)) {
    sendError("firmwareRepo must be owner/repo");
    return;
  }

  markDirty();
  if (!saveState(true)) {
    sendError("failed to persist state", 500);
    return;
  }
  handleApiFirmwareConfigGet();
}


bool runOtaUpdate(const String& firmwareUrl, const String& filesystemUrl, bool includeFilesystem, String* errorMessage) {
  if (firmwareUrl.length() == 0) {
    if (errorMessage) *errorMessage = "firmware url missing";
    return false;
  }
  if (includeFilesystem && filesystemUrl.length() == 0) {
    if (errorMessage) *errorMessage = "filesystem url missing";
    return false;
  }

  String otaErr;
  if (includeFilesystem && !performHttpOta(filesystemUrl, true, &otaErr)) {
    if (errorMessage) *errorMessage = String("filesystem update failed: ") + otaErr;
    return false;
  }
  if (!performHttpOta(firmwareUrl, false, &otaErr)) {
    if (errorMessage) *errorMessage = String("firmware update failed: ") + otaErr;
    return false;
  }
  return true;
}

void handleApiFirmwareUpdateLatest() {
  StaticJsonDocument<192> body;
  const bool hasBody = parseJsonBody(body);
  const bool includeFilesystem = hasBody ? (body["includeFilesystem"] | true) : true;

  normalizeFirmwareConfig();
  if (!isValidGithubRepo(firmwareRepo)) {
    sendError("firmwareRepo must be owner/repo");
    return;
  }
  const String firmwareUrl = "https://github.com/" + firmwareRepo + "/releases/latest/download/" + firmwareAssetName;
  const String filesystemUrl = "https://github.com/" + firmwareRepo + "/releases/latest/download/" + firmwareFsAssetName;

  String err;
  if (!runOtaUpdate(firmwareUrl, filesystemUrl, includeFilesystem, &err)) {
    sendError(err.c_str(), 502);
    return;
  }

  StaticJsonDocument<384> doc;
  doc["ok"] = true;
  doc["message"] = "ota update complete, rebooting";
  doc["firmwareUrl"] = firmwareUrl;
  doc["filesystemUrl"] = filesystemUrl;
  sendJsonDocument(200, doc);
  delay(500);
  ESP.restart();
}

void handleApiFirmwareCheckLatest() {
  normalizeFirmwareConfig();
  if (!isValidGithubRepo(firmwareRepo)) {
    sendError("firmwareRepo must be owner/repo");
    return;
  }

  StaticJsonDocument<192> body;
  const bool hasBody = parseJsonBody(body);
  const bool includeFilesystem = hasBody ? (body["includeFilesystem"] | true) : true;

  const String firmwareUrl = "https://github.com/" + firmwareRepo + "/releases/latest/download/" + firmwareAssetName;
  const String filesystemUrl = "https://github.com/" + firmwareRepo + "/releases/latest/download/" + firmwareFsAssetName;

  const bool fwUrlFormatOk = firmwareUrl.startsWith("https://");
  const bool fsUrlFormatOk = filesystemUrl.startsWith("https://");
  String netErr;
  const bool networkOk = probeGithubTcp(&netErr);

  StaticJsonDocument<1024> doc;
  doc["ok"] = fwUrlFormatOk && (!includeFilesystem || fsUrlFormatOk) && networkOk;
  doc["firmwareUrl"] = firmwareUrl;
  doc["filesystemUrl"] = filesystemUrl;
  doc["networkOk"] = networkOk;
  doc["networkError"] = netErr;
  JsonObject fw = doc.createNestedObject("firmware");
  fw["ok"] = fwUrlFormatOk && networkOk;
  fw["urlFormatOk"] = fwUrlFormatOk;
  JsonObject fs = doc.createNestedObject("filesystem");
  fs["ok"] = !includeFilesystem || (fsUrlFormatOk && networkOk);
  fs["urlFormatOk"] = fsUrlFormatOk;
  if (!(doc["ok"].as<bool>())) {
    doc["error"] = networkOk ? "invalid latest url configuration" : (netErr.length() ? netErr : "network unavailable");
  }

  if (!(doc["ok"].as<bool>())) {
    sendJsonDocument(502, doc);
    return;
  }
  sendJsonDocument(200, doc);
}


void handleApiFirmwareUpdateUrl() {
  StaticJsonDocument<768> body;
  if (!parseJsonBody(body)) {
    sendError("invalid json");
    return;
  }

  const String firmwareUrl = String(static_cast<const char*>(body["firmwareUrl"] | ""));
  const String filesystemUrl = String(static_cast<const char*>(body["filesystemUrl"] | ""));
  const bool includeFilesystem = body["includeFilesystem"] | true;

  String err;
  if (!runOtaUpdate(firmwareUrl, filesystemUrl, includeFilesystem, &err)) {
    sendError(err.c_str(), 502);
    return;
  }

  StaticJsonDocument<384> doc;
  doc["ok"] = true;
  doc["message"] = "ota update complete, rebooting";
  sendJsonDocument(200, doc);
  delay(500);
  ESP.restart();
}

void handleApiWifiReset() {
  wifiManager.resetSettings();

  StaticJsonDocument<160> doc;
  doc["ok"] = true;
  doc["message"] = "wifi settings cleared, rebooting";
  sendJsonDocument(200, doc);

  delay(500);
  ESP.restart();
}

void handleApiReboot() {
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["message"] = "rebooting";
  sendJsonDocument(200, doc);
  delay(300);
  ESP.restart();
}

void handleNotFound() {
  if (!LittleFS.exists("/index.html")) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  File file = LittleFS.open("/index.html", "r");
  server.streamFile(file, "text/html");
  file.close();
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    if (!LittleFS.exists("/index.html")) {
      server.send(500, "text/plain", "index.html missing");
      return;
    }
    File file = LittleFS.open("/index.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  });
  server.serveStatic("/app.js", LittleFS, "/app.js");
  server.serveStatic("/styles.css", LittleFS, "/styles.css");

  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/move", HTTP_POST, handleApiMove);
  server.on("/api/calibrate", HTTP_POST, handleApiCalibrate);
  server.on("/api/settings", HTTP_POST, handleApiSettings);
  server.on("/api/wifi/reset", HTTP_POST, handleApiWifiReset);
  server.on("/api/system/reboot", HTTP_POST, handleApiReboot);
  server.on("/api/firmware/config", HTTP_GET, handleApiFirmwareConfigGet);
  server.on("/api/firmware/config", HTTP_POST, handleApiFirmwareConfigPost);
  server.on("/api/firmware/check/latest", HTTP_POST, handleApiFirmwareCheckLatest);
  server.on("/api/firmware/update/latest", HTTP_POST, handleApiFirmwareUpdateLatest);
  server.on("/api/firmware/update/url", HTTP_POST, handleApiFirmwareUpdateUrl);

  server.onNotFound(handleNotFound);
  server.begin();
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  // ESP8266 core 3.x keeps persistence disabled by default.
  // Enable it so credentials configured in WiFiManager survive reboot.
  WiFi.persistent(true);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);

  WiFi.begin(cfg::kDefaultWifiSsid, cfg::kDefaultWifiPass);
  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < cfg::kDefaultWifiConnectTimeoutMs) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) return;

  wifiManager.setConfigPortalTimeout(cfg::kApPortalTimeoutSec);
  const bool connected = wifiManager.autoConnect(cfg::kApSsid, cfg::kApPass);
  if (!connected) {
    delay(1000);
    ESP.restart();
  }

  applyWiFiPowerMode();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }

  loadState();

  applyStepperSettings();
  state.currentPosition = clampLogicalPosition(state.currentPosition);
  targetPosition = state.currentPosition;
  stepper.setCurrentPosition(logicalToRaw(state.currentPosition));
  stepper.moveTo(logicalToRaw(targetPosition));
  disableMotorOutputs();

  setupWiFi();
  setupWebServer();

  saveState(true);
}

void loop() {
  server.handleClient();

  const long rawBefore = stepper.currentPosition();
  const bool wasMoving = stepper.distanceToGo() != 0;
  stepper.run();
  const bool isMoving = stepper.distanceToGo() != 0;

  if (stepper.currentPosition() != rawBefore) {
    markDirty();
  }

  if (wasMoving && !isMoving) {
    motionStoppedAtMs = millis();
    targetPosition = currentLogicalPosition();
    saveState(true);
  }

  if (!isMoving) {
    if (millis() - motionStoppedAtMs >= state.coilHoldMs) {
      disableMotorOutputs();
    }
  }

  saveState(false);
}
