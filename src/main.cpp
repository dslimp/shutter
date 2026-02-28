#include <Arduino.h>
#include <AccelStepper.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <Updater.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include <WiFiManager.h>
#include <time.h>
#include <math.h>
#include <memory>

#include "ShutterMath.h"

namespace cfg {
constexpr char kFirmwareVersion[] = "0.1.10-esp8266";
constexpr char kApSsid[] = "Shutter-Setup";
constexpr char kApPass[] = "shutter123";
constexpr uint16_t kApPortalTimeoutSec = 180;
constexpr char kDefaultWifiSsid[] = "nh";
constexpr char kDefaultWifiPass[] = "Fx110011";
constexpr uint16_t kDefaultWifiConnectTimeoutMs = 12000;
constexpr char kDefaultFirmwareRepo[] = "dslimp/shutter";
constexpr char kDefaultFirmwareAssetName[] = "firmware.bin";
constexpr char kDefaultFirmwareFsAssetName[] = "littlefs.bin";
constexpr char kDefaultTimezone[] = "MSK-3";
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";
constexpr uint8_t kOtaMaxAttempts = 4;
constexpr uint16_t kOtaClientTimeoutMs = 20000;
constexpr uint16_t kOtaRetryDelayMs = 2500;
constexpr uint16_t kOtaQueueStartDelayMs = 400;
constexpr char kStateFile[] = "/state.json";
constexpr uint16_t kEepromSize = 512;
constexpr uint32_t kStateMagic = 0x53485452;  // "SHTR"
constexpr uint16_t kStateSchemaVersion = 2;
constexpr uint32_t kSaveIntervalMs = 5000;
constexpr long kMinTravelSteps = 100;
constexpr long kMaxTravelSteps = 300000;
constexpr float kMinSpeed = 80.0f;
constexpr float kMaxSpeed = 2500.0f;
constexpr float kMinAccel = 40.0f;
constexpr float kMaxAccel = 6000.0f;
constexpr uint16_t kMaxCoilHoldMs = 10000;
constexpr float kMinTopOverdrivePercent = 0.0f;
constexpr float kMaxTopOverdrivePercent = 50.0f;
constexpr float kMinLatitude = -90.0f;
constexpr float kMaxLatitude = 90.0f;
constexpr float kMinLongitude = -180.0f;
constexpr float kMaxLongitude = 180.0f;
constexpr int16_t kMinScheduleOffsetMinutes = -300;
constexpr int16_t kMaxScheduleOffsetMinutes = 300;

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
  bool topOverdriveEnabled = true;
  bool sunScheduleEnabled = false;
  float maxSpeed = 700.0f;
  float acceleration = 350.0f;
  float topOverdrivePercent = 10.0f;
  float latitude = 55.7558f;
  float longitude = 37.6173f;
  uint16_t coilHoldMs = 500;
  int16_t sunriseOffsetMinutes = 0;
  int16_t sunsetOffsetMinutes = 0;
  uint8_t sunriseTargetPercent = 0;
  uint8_t sunsetTargetPercent = 100;
};

struct PersistedStateBlob {
  uint32_t magic;
  uint16_t schemaVersion;
  uint16_t structSize;
  int32_t travelSteps;
  int32_t currentPosition;
  uint8_t calibrated;
  uint8_t reverseDirection;
  uint8_t wifiModemSleep;
  uint8_t topOverdriveEnabled;
  uint8_t sunScheduleEnabled;
  float maxSpeed;
  float acceleration;
  float topOverdrivePercent;
  float latitude;
  float longitude;
  uint16_t coilHoldMs;
  int16_t sunriseOffsetMinutes;
  int16_t sunsetOffsetMinutes;
  uint8_t sunriseTargetPercent;
  uint8_t sunsetTargetPercent;
  char timezone[40];
  char firmwareRepo[64];
  char firmwareAssetName[32];
  char firmwareFsAssetName[32];
  uint32_t checksum;
};

// Legacy schema v1 used by releases before sun schedule/timezone fields.
struct PersistedStateBlobV1 {
  uint32_t magic;
  uint16_t schemaVersion;
  uint16_t structSize;
  int32_t travelSteps;
  int32_t currentPosition;
  uint8_t calibrated;
  uint8_t reverseDirection;
  uint8_t wifiModemSleep;
  uint8_t topOverdriveEnabled;
  float maxSpeed;
  float acceleration;
  float topOverdrivePercent;
  uint16_t coilHoldMs;
  char firmwareRepo[64];
  char firmwareAssetName[32];
  char firmwareFsAssetName[32];
  uint32_t checksum;
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
bool resetTopReferenceWhenStopped = false;
String firmwareRepo = cfg::kDefaultFirmwareRepo;
String firmwareAssetName = cfg::kDefaultFirmwareAssetName;
String firmwareFsAssetName = cfg::kDefaultFirmwareFsAssetName;
String timezoneSpec = cfg::kDefaultTimezone;
bool eepromReady = false;

struct SunRuntimeState {
  bool valid = false;
  int year = -1;
  int yday = -1;
  time_t sunriseEpoch = 0;
  time_t sunsetEpoch = 0;
  bool sunriseDone = false;
  bool sunsetDone = false;
};

SunRuntimeState sunRuntime;

struct OtaJobState {
  bool pending = false;
  bool running = false;
  bool rebootScheduled = false;
  bool includeFilesystem = true;
  String firmwareUrl;
  String filesystemUrl;
  String source;
  String tag;
  String phase;
  String lastError;
  uint32_t queuedAtMs = 0;
  uint32_t startedAtMs = 0;
};

OtaJobState otaJob;

#if defined(OTA_FW_PAD_BYTES) && (OTA_FW_PAD_BYTES > 0)
__attribute__((used)) const uint8_t kOtaFirmwarePad[OTA_FW_PAD_BYTES] PROGMEM = {0xA5};
#endif

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

void normalizeTimezoneSpec() {
  timezoneSpec.trim();
  if (timezoneSpec.length() == 0) timezoneSpec = cfg::kDefaultTimezone;
  if (timezoneSpec.length() > 39) timezoneSpec = timezoneSpec.substring(0, 39);
}

void resetSunRuntime() {
  sunRuntime.valid = false;
  sunRuntime.year = -1;
  sunRuntime.yday = -1;
  sunRuntime.sunriseEpoch = 0;
  sunRuntime.sunsetEpoch = 0;
  sunRuntime.sunriseDone = false;
  sunRuntime.sunsetDone = false;
}

void configureTimekeeping() {
  normalizeTimezoneSpec();
  configTime(timezoneSpec.c_str(), cfg::kNtpServer1, cfg::kNtpServer2);
}

bool isClockSynchronized() {
  return time(nullptr) > 1704067200;  // 2024-01-01 UTC
}

double normalizeDegrees(double degrees) {
  while (degrees < 0.0) degrees += 360.0;
  while (degrees >= 360.0) degrees -= 360.0;
  return degrees;
}

double julianDayFromDate(int year, int month, int day) {
  const int a = (14 - month) / 12;
  const int y = year + 4800 - a;
  const int m = month + 12 * a - 3;
  const int jdn = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
  return static_cast<double>(jdn);
}

time_t julianToUnix(double julianDay) {
  const double unixSeconds = (julianDay - 2440587.5) * 86400.0;
  return static_cast<time_t>(llround(unixSeconds));
}

bool computeSunEventsUtcForDate(
    int year,
    int month,
    int day,
    float latitude,
    float longitude,
    time_t* sunriseEpoch,
    time_t* sunsetEpoch) {
  if (!sunriseEpoch || !sunsetEpoch) return false;
  if (latitude < cfg::kMinLatitude || latitude > cfg::kMaxLatitude) return false;
  if (longitude < cfg::kMinLongitude || longitude > cfg::kMaxLongitude) return false;

  const double degToRad = 3.14159265358979323846 / 180.0;
  const double julianDay = julianDayFromDate(year, month, day);
  // Input longitude is conventional east-positive (e.g. Moscow +37.6).
  // Sunrise equation here uses west-positive convention, so invert sign.
  const double westLongitude = -static_cast<double>(longitude);
  const double n = round(julianDay - 2451545.0009 - westLongitude / 360.0);
  const double jStar = 2451545.0009 + westLongitude / 360.0 + n;

  const double meanAnomaly = normalizeDegrees(357.5291 + 0.98560028 * (jStar - 2451545.0));
  const double meanAnomalyRad = meanAnomaly * degToRad;
  const double center = 1.9148 * sin(meanAnomalyRad) + 0.0200 * sin(2.0 * meanAnomalyRad) + 0.0003 * sin(3.0 * meanAnomalyRad);
  const double eclipticLongitude = normalizeDegrees(meanAnomaly + center + 180.0 + 102.9372);
  const double eclipticLongitudeRad = eclipticLongitude * degToRad;
  const double transit = jStar + 0.0053 * sin(meanAnomalyRad) - 0.0069 * sin(2.0 * eclipticLongitudeRad);

  const double declination = asin(sin(eclipticLongitudeRad) * sin(23.44 * degToRad));
  const double latitudeRad = static_cast<double>(latitude) * degToRad;
  const double cosHourAngle =
      (sin(-0.833 * degToRad) - sin(latitudeRad) * sin(declination)) / (cos(latitudeRad) * cos(declination));
  if (cosHourAngle < -1.0 || cosHourAngle > 1.0) return false;

  const double hourAngle = acos(cosHourAngle);
  const double julianSunset = transit + hourAngle / (2.0 * 3.14159265358979323846);
  const double julianSunrise = transit - hourAngle / (2.0 * 3.14159265358979323846);

  *sunriseEpoch = julianToUnix(julianSunrise);
  *sunsetEpoch = julianToUnix(julianSunset);
  return true;
}

String formatLocalEpoch(time_t epoch) {
  if (epoch <= 0) return String("-");
  struct tm info;
  if (!localtime_r(&epoch, &info)) return String("-");
  char buffer[24];
  if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &info) == 0) return String("-");
  return String(buffer);
}

void refreshSunScheduleForNow(time_t nowEpoch) {
  if (!state.sunScheduleEnabled || !isClockSynchronized()) {
    resetSunRuntime();
    return;
  }

  struct tm localNow;
  if (!localtime_r(&nowEpoch, &localNow)) {
    resetSunRuntime();
    return;
  }

  const int year = localNow.tm_year;
  const int yday = localNow.tm_yday;
  if (sunRuntime.valid && sunRuntime.year == year && sunRuntime.yday == yday) return;

  time_t sunriseEpoch = 0;
  time_t sunsetEpoch = 0;
  const bool ok = computeSunEventsUtcForDate(
      localNow.tm_year + 1900,
      localNow.tm_mon + 1,
      localNow.tm_mday,
      state.latitude,
      state.longitude,
      &sunriseEpoch,
      &sunsetEpoch);
  if (!ok) {
    resetSunRuntime();
    return;
  }

  sunriseEpoch += static_cast<time_t>(state.sunriseOffsetMinutes) * 60;
  sunsetEpoch += static_cast<time_t>(state.sunsetOffsetMinutes) * 60;

  sunRuntime.valid = true;
  sunRuntime.year = year;
  sunRuntime.yday = yday;
  sunRuntime.sunriseEpoch = sunriseEpoch;
  sunRuntime.sunsetEpoch = sunsetEpoch;
  sunRuntime.sunriseDone = nowEpoch >= sunriseEpoch;
  sunRuntime.sunsetDone = nowEpoch >= sunsetEpoch;
}

uint32_t computeChecksum(const uint8_t* data, size_t length) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

void copyStringField(char* dst, size_t dstSize, const String& src) {
  if (dstSize == 0) return;
  memset(dst, 0, dstSize);
  src.substring(0, dstSize - 1).toCharArray(dst, dstSize);
}

String parseStringField(const char* src, size_t srcSize) {
  size_t len = 0;
  while (len < srcSize && src[len] != '\0') ++len;
  return String(src).substring(0, len);
}

void fillPersistedBlob(PersistedStateBlob* blob, long pos) {
  normalizeTimezoneSpec();
  memset(blob, 0, sizeof(PersistedStateBlob));
  blob->magic = cfg::kStateMagic;
  blob->schemaVersion = cfg::kStateSchemaVersion;
  blob->structSize = sizeof(PersistedStateBlob);
  blob->travelSteps = static_cast<int32_t>(state.travelSteps);
  blob->currentPosition = static_cast<int32_t>(pos);
  blob->calibrated = state.calibrated ? 1 : 0;
  blob->reverseDirection = state.reverseDirection ? 1 : 0;
  blob->wifiModemSleep = state.wifiModemSleep ? 1 : 0;
  blob->topOverdriveEnabled = state.topOverdriveEnabled ? 1 : 0;
  blob->sunScheduleEnabled = state.sunScheduleEnabled ? 1 : 0;
  blob->maxSpeed = state.maxSpeed;
  blob->acceleration = state.acceleration;
  blob->topOverdrivePercent = state.topOverdrivePercent;
  blob->latitude = state.latitude;
  blob->longitude = state.longitude;
  blob->coilHoldMs = state.coilHoldMs;
  blob->sunriseOffsetMinutes = state.sunriseOffsetMinutes;
  blob->sunsetOffsetMinutes = state.sunsetOffsetMinutes;
  blob->sunriseTargetPercent = state.sunriseTargetPercent;
  blob->sunsetTargetPercent = state.sunsetTargetPercent;
  copyStringField(blob->timezone, sizeof(blob->timezone), timezoneSpec);
  copyStringField(blob->firmwareRepo, sizeof(blob->firmwareRepo), firmwareRepo);
  copyStringField(blob->firmwareAssetName, sizeof(blob->firmwareAssetName), firmwareAssetName);
  copyStringField(blob->firmwareFsAssetName, sizeof(blob->firmwareFsAssetName), firmwareFsAssetName);
  blob->checksum = computeChecksum(reinterpret_cast<const uint8_t*>(blob), sizeof(PersistedStateBlob) - sizeof(uint32_t));
}

bool applyPersistedBlob(const PersistedStateBlob& blob) {
  if (blob.magic != cfg::kStateMagic) return false;
  if (blob.schemaVersion != cfg::kStateSchemaVersion) return false;
  if (blob.structSize != sizeof(PersistedStateBlob)) return false;
  const uint32_t expected = computeChecksum(reinterpret_cast<const uint8_t*>(&blob), sizeof(PersistedStateBlob) - sizeof(uint32_t));
  if (blob.checksum != expected) return false;

  state.travelSteps = shutter::math::clampLong(blob.travelSteps, cfg::kMinTravelSteps, cfg::kMaxTravelSteps);
  state.currentPosition = shutter::math::clampLong(blob.currentPosition, 0, state.travelSteps);
  state.calibrated = blob.calibrated != 0;
  state.reverseDirection = blob.reverseDirection != 0;
  state.wifiModemSleep = blob.wifiModemSleep != 0;
  state.topOverdriveEnabled = blob.topOverdriveEnabled != 0;
  state.sunScheduleEnabled = blob.sunScheduleEnabled != 0;
  state.maxSpeed = shutter::math::clampFloat(blob.maxSpeed, cfg::kMinSpeed, cfg::kMaxSpeed);
  state.acceleration = shutter::math::clampFloat(blob.acceleration, cfg::kMinAccel, cfg::kMaxAccel);
  state.topOverdrivePercent =
      shutter::math::clampFloat(blob.topOverdrivePercent, cfg::kMinTopOverdrivePercent, cfg::kMaxTopOverdrivePercent);
  state.latitude = shutter::math::clampFloat(blob.latitude, cfg::kMinLatitude, cfg::kMaxLatitude);
  state.longitude = shutter::math::clampFloat(blob.longitude, cfg::kMinLongitude, cfg::kMaxLongitude);
  state.coilHoldMs = static_cast<uint16_t>(shutter::math::clampLong(blob.coilHoldMs, 0, cfg::kMaxCoilHoldMs));
  state.sunriseOffsetMinutes =
      static_cast<int16_t>(shutter::math::clampLong(blob.sunriseOffsetMinutes, cfg::kMinScheduleOffsetMinutes, cfg::kMaxScheduleOffsetMinutes));
  state.sunsetOffsetMinutes =
      static_cast<int16_t>(shutter::math::clampLong(blob.sunsetOffsetMinutes, cfg::kMinScheduleOffsetMinutes, cfg::kMaxScheduleOffsetMinutes));
  state.sunriseTargetPercent = static_cast<uint8_t>(shutter::math::clampLong(blob.sunriseTargetPercent, 0, 100));
  state.sunsetTargetPercent = static_cast<uint8_t>(shutter::math::clampLong(blob.sunsetTargetPercent, 0, 100));
  timezoneSpec = parseStringField(blob.timezone, sizeof(blob.timezone));
  firmwareRepo = parseStringField(blob.firmwareRepo, sizeof(blob.firmwareRepo));
  firmwareAssetName = parseStringField(blob.firmwareAssetName, sizeof(blob.firmwareAssetName));
  firmwareFsAssetName = parseStringField(blob.firmwareFsAssetName, sizeof(blob.firmwareFsAssetName));
  normalizeTimezoneSpec();
  normalizeFirmwareConfig();
  resetSunRuntime();
  return true;
}

bool applyPersistedBlobV1(const PersistedStateBlobV1& blob) {
  if (blob.magic != cfg::kStateMagic) return false;
  if (blob.schemaVersion != 1) return false;
  if (blob.structSize != sizeof(PersistedStateBlobV1)) return false;
  const uint32_t expected =
      computeChecksum(reinterpret_cast<const uint8_t*>(&blob), sizeof(PersistedStateBlobV1) - sizeof(uint32_t));
  if (blob.checksum != expected) return false;

  state.travelSteps = shutter::math::clampLong(blob.travelSteps, cfg::kMinTravelSteps, cfg::kMaxTravelSteps);
  state.currentPosition = shutter::math::clampLong(blob.currentPosition, 0, state.travelSteps);
  state.calibrated = blob.calibrated != 0;
  state.reverseDirection = blob.reverseDirection != 0;
  state.wifiModemSleep = blob.wifiModemSleep != 0;
  state.topOverdriveEnabled = blob.topOverdriveEnabled != 0;
  state.maxSpeed = shutter::math::clampFloat(blob.maxSpeed, cfg::kMinSpeed, cfg::kMaxSpeed);
  state.acceleration = shutter::math::clampFloat(blob.acceleration, cfg::kMinAccel, cfg::kMaxAccel);
  state.topOverdrivePercent =
      shutter::math::clampFloat(blob.topOverdrivePercent, cfg::kMinTopOverdrivePercent, cfg::kMaxTopOverdrivePercent);
  state.coilHoldMs = static_cast<uint16_t>(shutter::math::clampLong(blob.coilHoldMs, 0, cfg::kMaxCoilHoldMs));
  firmwareRepo = parseStringField(blob.firmwareRepo, sizeof(blob.firmwareRepo));
  firmwareAssetName = parseStringField(blob.firmwareAssetName, sizeof(blob.firmwareAssetName));
  firmwareFsAssetName = parseStringField(blob.firmwareFsAssetName, sizeof(blob.firmwareFsAssetName));
  normalizeFirmwareConfig();
  normalizeTimezoneSpec();
  resetSunRuntime();
  return true;
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
  const uint32_t nowMs = millis();
  const time_t nowEpoch = time(nullptr);
  const bool timeSynced = isClockSynchronized();
  if (timeSynced) {
    refreshSunScheduleForNow(nowEpoch);
  } else {
    resetSunRuntime();
  }
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
  root["unixTime"] = static_cast<int64_t>(nowEpoch);
  root["timeSynced"] = timeSynced;
  root["localTime"] = timeSynced ? formatLocalEpoch(nowEpoch) : String("-");
  root["timezone"] = timezoneSpec;
  root["ntpServer1"] = cfg::kNtpServer1;
  root["ntpServer2"] = cfg::kNtpServer2;
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
  root["topOverdriveEnabled"] = state.topOverdriveEnabled;
  root["topOverdrivePercent"] = state.topOverdrivePercent;
  root["sunScheduleEnabled"] = state.sunScheduleEnabled;
  root["latitude"] = state.latitude;
  root["longitude"] = state.longitude;
  root["sunriseOffsetMinutes"] = state.sunriseOffsetMinutes;
  root["sunsetOffsetMinutes"] = state.sunsetOffsetMinutes;
  root["sunriseTargetPercent"] = state.sunriseTargetPercent;
  root["sunsetTargetPercent"] = state.sunsetTargetPercent;
  root["sunScheduleReady"] = sunRuntime.valid;
  root["sunriseEpoch"] = static_cast<int64_t>(sunRuntime.sunriseEpoch);
  root["sunsetEpoch"] = static_cast<int64_t>(sunRuntime.sunsetEpoch);
  root["sunriseTime"] = sunRuntime.valid ? formatLocalEpoch(sunRuntime.sunriseEpoch) : String("-");
  root["sunsetTime"] = sunRuntime.valid ? formatLocalEpoch(sunRuntime.sunsetEpoch) : String("-");
  root["sunriseDoneToday"] = sunRuntime.sunriseDone;
  root["sunsetDoneToday"] = sunRuntime.sunsetDone;
  root["maxSpeed"] = state.maxSpeed;
  root["acceleration"] = state.acceleration;
  root["coilHoldMs"] = state.coilHoldMs;
  root["rawPosition"] = stepper.currentPosition();
  root["firmwareRepo"] = firmwareRepo;
  root["firmwareAssetName"] = firmwareAssetName;
  root["firmwareFsAssetName"] = firmwareFsAssetName;
  root["otaPending"] = otaJob.pending;
  root["otaRunning"] = otaJob.running;
  root["otaSource"] = otaJob.source;
  root["otaTag"] = otaJob.tag;
  root["otaPhase"] = otaJob.phase;
  root["otaLastError"] = otaJob.lastError;
  root["otaQueuedSec"] = otaJob.queuedAtMs > 0 ? (nowMs - otaJob.queuedAtMs) / 1000 : 0;
  root["otaRunningSec"] = otaJob.startedAtMs > 0 ? (nowMs - otaJob.startedAtMs) / 1000 : 0;
}

bool loadStateFromLegacyFs() {
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
  state.topOverdriveEnabled = doc["topOverdriveEnabled"] | state.topOverdriveEnabled;
  state.sunScheduleEnabled = doc["sunScheduleEnabled"] | state.sunScheduleEnabled;
  state.maxSpeed = shutter::math::clampFloat(doc["maxSpeed"] | state.maxSpeed, cfg::kMinSpeed, cfg::kMaxSpeed);
  state.acceleration = shutter::math::clampFloat(doc["acceleration"] | state.acceleration, cfg::kMinAccel, cfg::kMaxAccel);
  state.topOverdrivePercent = shutter::math::clampFloat(
      doc["topOverdrivePercent"] | state.topOverdrivePercent, cfg::kMinTopOverdrivePercent, cfg::kMaxTopOverdrivePercent);
  state.latitude = shutter::math::clampFloat(doc["latitude"] | state.latitude, cfg::kMinLatitude, cfg::kMaxLatitude);
  state.longitude = shutter::math::clampFloat(doc["longitude"] | state.longitude, cfg::kMinLongitude, cfg::kMaxLongitude);
  state.coilHoldMs = static_cast<uint16_t>(shutter::math::clampLong(doc["coilHoldMs"] | state.coilHoldMs, 0, cfg::kMaxCoilHoldMs));
  state.sunriseOffsetMinutes = static_cast<int16_t>(shutter::math::clampLong(
      doc["sunriseOffsetMinutes"] | state.sunriseOffsetMinutes, cfg::kMinScheduleOffsetMinutes, cfg::kMaxScheduleOffsetMinutes));
  state.sunsetOffsetMinutes = static_cast<int16_t>(shutter::math::clampLong(
      doc["sunsetOffsetMinutes"] | state.sunsetOffsetMinutes, cfg::kMinScheduleOffsetMinutes, cfg::kMaxScheduleOffsetMinutes));
  state.sunriseTargetPercent = static_cast<uint8_t>(shutter::math::clampLong(doc["sunriseTargetPercent"] | state.sunriseTargetPercent, 0, 100));
  state.sunsetTargetPercent = static_cast<uint8_t>(shutter::math::clampLong(doc["sunsetTargetPercent"] | state.sunsetTargetPercent, 0, 100));
  timezoneSpec = String(static_cast<const char*>(doc["timezone"] | timezoneSpec.c_str()));
  firmwareRepo = String(static_cast<const char*>(doc["firmwareRepo"] | firmwareRepo.c_str()));
  firmwareAssetName = String(static_cast<const char*>(doc["firmwareAssetName"] | firmwareAssetName.c_str()));
  firmwareFsAssetName = String(static_cast<const char*>(doc["firmwareFsAssetName"] | firmwareFsAssetName.c_str()));
  normalizeTimezoneSpec();
  normalizeFirmwareConfig();
  resetSunRuntime();
  return true;
}

bool loadStateFromEeprom(bool* migratedToCurrent = nullptr) {
  if (migratedToCurrent) *migratedToCurrent = false;
  if (!eepromReady) return false;
  PersistedStateBlob blob;
  EEPROM.get(0, blob);
  if (applyPersistedBlob(blob)) return true;

  PersistedStateBlobV1 legacyBlob;
  EEPROM.get(0, legacyBlob);
  if (!applyPersistedBlobV1(legacyBlob)) return false;
  if (migratedToCurrent) *migratedToCurrent = true;
  return true;
}

bool saveStateToEeprom(long pos) {
  if (!eepromReady) return false;
  PersistedStateBlob blob;
  fillPersistedBlob(&blob, pos);
  EEPROM.put(0, blob);
  return EEPROM.commit();
}

bool saveStateToLegacyFs(long pos) {
  StaticJsonDocument<1024> doc;
  doc["travelSteps"] = state.travelSteps;
  doc["currentPosition"] = pos;
  doc["calibrated"] = state.calibrated;
  doc["reverseDirection"] = state.reverseDirection;
  doc["wifiModemSleep"] = state.wifiModemSleep;
  doc["topOverdriveEnabled"] = state.topOverdriveEnabled;
  doc["sunScheduleEnabled"] = state.sunScheduleEnabled;
  doc["maxSpeed"] = state.maxSpeed;
  doc["acceleration"] = state.acceleration;
  doc["topOverdrivePercent"] = state.topOverdrivePercent;
  doc["latitude"] = state.latitude;
  doc["longitude"] = state.longitude;
  doc["coilHoldMs"] = state.coilHoldMs;
  doc["sunriseOffsetMinutes"] = state.sunriseOffsetMinutes;
  doc["sunsetOffsetMinutes"] = state.sunsetOffsetMinutes;
  doc["sunriseTargetPercent"] = state.sunriseTargetPercent;
  doc["sunsetTargetPercent"] = state.sunsetTargetPercent;
  doc["timezone"] = timezoneSpec;
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
  return true;
}

bool loadState() {
  bool migratedToCurrent = false;
  if (loadStateFromEeprom(&migratedToCurrent)) {
    if (migratedToCurrent) {
      const long pos = shutter::math::clampLong(state.currentPosition, 0, state.travelSteps);
      saveStateToEeprom(pos);
      saveStateToLegacyFs(pos);
      lastSavedPosition = pos;
      lastSaveMs = millis();
      settingsDirty = false;
    }
    return true;
  }
  if (!loadStateFromLegacyFs()) return false;
  const long pos = shutter::math::clampLong(state.currentPosition, 0, state.travelSteps);
  saveStateToEeprom(pos);
  return true;
}

bool saveState(bool force = false) {
  const long pos = currentLogicalPosition();
  const uint32_t now = millis();

  if (!force) {
    if (!settingsDirty && pos == lastSavedPosition) return true;
    if (now - lastSaveMs < cfg::kSaveIntervalMs) return true;
  }

  if (!saveStateToEeprom(pos)) return false;
  saveStateToLegacyFs(pos);
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
  resetTopReferenceWhenStopped = false;
  targetPosition = clampLogicalPosition(logicalTarget);
  enableMotorOutputs();
  stepper.moveTo(logicalToRaw(targetPosition));
  markDirty();
}

void setTargetRawWithLogical(long rawTarget, long logicalTarget) {
  targetPosition = clampLogicalPosition(logicalTarget);
  enableMotorOutputs();
  stepper.moveTo(rawTarget);
  markDirty();
}

void startOpenMotion() {
  if (!state.topOverdriveEnabled || state.topOverdrivePercent <= 0.0f) {
    setTargetPosition(0);
    return;
  }

  const float clampedPercent =
      shutter::math::clampFloat(state.topOverdrivePercent, cfg::kMinTopOverdrivePercent, cfg::kMaxTopOverdrivePercent);
  const long extraSteps = static_cast<long>(lroundf((clampedPercent / 100.0f) * static_cast<float>(state.travelSteps)));
  if (extraSteps <= 0) {
    setTargetPosition(0);
    return;
  }

  resetTopReferenceWhenStopped = true;
  const long rawTarget = logicalToRaw(-extraSteps);
  setTargetRawWithLogical(rawTarget, 0);
}

void setTargetByPercent(uint8_t percentValue) {
  const float percent = shutter::math::clampFloat(static_cast<float>(percentValue), 0.0f, 100.0f);
  if (percent <= 0.0f) {
    startOpenMotion();
    return;
  }
  const long target = shutter::math::percentToSteps(percent, state.travelSteps);
  setTargetPosition(target);
}

void processSunSchedule() {
  if (!state.sunScheduleEnabled || !state.calibrated) return;
  if (otaJob.pending || otaJob.running || otaJob.rebootScheduled) return;
  if (!isClockSynchronized()) return;

  const time_t nowEpoch = time(nullptr);
  refreshSunScheduleForNow(nowEpoch);
  if (!sunRuntime.valid) return;

  if (!sunRuntime.sunriseDone && nowEpoch >= sunRuntime.sunriseEpoch) {
    sunRuntime.sunriseDone = true;
    Serial.printf("[SUN] sunrise trigger: target=%u%%\n", static_cast<unsigned>(state.sunriseTargetPercent));
    setTargetByPercent(state.sunriseTargetPercent);
  }

  if (!sunRuntime.sunsetDone && nowEpoch >= sunRuntime.sunsetEpoch) {
    sunRuntime.sunsetDone = true;
    Serial.printf("[SUN] sunset trigger: target=%u%%\n", static_cast<unsigned>(state.sunsetTargetPercent));
    setTargetByPercent(state.sunsetTargetPercent);
  }
}

void stopMotor() {
  resetTopReferenceWhenStopped = false;
  const long rawNow = stepper.currentPosition();
  stepper.setCurrentPosition(rawNow);
  stepper.moveTo(rawNow);
  targetPosition = clampLogicalPosition(rawToLogical(rawNow));
  motionStoppedAtMs = millis();
  markDirty();
}

void calibrateSetTop() {
  resetTopReferenceWhenStopped = false;
  stepper.setCurrentPosition(logicalToRaw(0));
  targetPosition = 0;
  stepper.moveTo(logicalToRaw(targetPosition));
  state.currentPosition = 0;
  markDirty();
}

bool calibrateSetBottom() {
  resetTopReferenceWhenStopped = false;
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
  resetTopReferenceWhenStopped = false;
  const long rawDelta = logicalDelta * directionSign();
  const long rawTarget = stepper.currentPosition() + rawDelta;
  enableMotorOutputs();
  stepper.moveTo(rawTarget);
  markDirty();
}

void handleApiState() {
  // Keep state payload document in static storage to avoid large per-request stack usage.
  static StaticJsonDocument<2304> doc;
  doc.clear();
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
    startOpenMotion();
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
  bool shouldPersistNow = false;
  if (strcmp(action, "set_top") == 0) {
    calibrateSetTop();
    shouldPersistNow = true;
  } else if (strcmp(action, "set_bottom") == 0) {
    if (!calibrateSetBottom()) {
      sendError("failed to set bottom");
      return;
    }
    shouldPersistNow = true;
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
    shouldPersistNow = true;
  } else {
    sendError("unknown action");
    return;
  }

  if (shouldPersistNow && !saveState(true)) {
    sendError("failed to persist state", 500);
    return;
  }

  handleApiState();
}

void handleApiSettings() {
  StaticJsonDocument<768> body;
  if (!parseJsonBody(body)) {
    sendError("invalid json");
    return;
  }

  const long logicalPosBefore = currentLogicalPosition();
  const long logicalTargetBefore = targetPosition;
  const String oldTimezoneSpec = timezoneSpec;
  bool scheduleConfigChanged = false;

  if (body.containsKey("reverseDirection")) {
    state.reverseDirection = body["reverseDirection"].as<bool>();
  }
  if (body.containsKey("wifiModemSleep")) {
    state.wifiModemSleep = body["wifiModemSleep"].as<bool>();
  }
  if (body.containsKey("topOverdriveEnabled")) {
    state.topOverdriveEnabled = body["topOverdriveEnabled"].as<bool>();
  }
  if (body.containsKey("sunScheduleEnabled")) {
    state.sunScheduleEnabled = body["sunScheduleEnabled"].as<bool>();
    scheduleConfigChanged = true;
  }
  if (body.containsKey("maxSpeed")) {
    state.maxSpeed = shutter::math::clampFloat(body["maxSpeed"].as<float>(), cfg::kMinSpeed, cfg::kMaxSpeed);
  }
  if (body.containsKey("acceleration")) {
    state.acceleration = shutter::math::clampFloat(body["acceleration"].as<float>(), cfg::kMinAccel, cfg::kMaxAccel);
  }
  if (body.containsKey("topOverdrivePercent")) {
    state.topOverdrivePercent =
        shutter::math::clampFloat(body["topOverdrivePercent"].as<float>(), cfg::kMinTopOverdrivePercent, cfg::kMaxTopOverdrivePercent);
  }
  if (body.containsKey("latitude")) {
    state.latitude = shutter::math::clampFloat(body["latitude"].as<float>(), cfg::kMinLatitude, cfg::kMaxLatitude);
    scheduleConfigChanged = true;
  }
  if (body.containsKey("longitude")) {
    state.longitude = shutter::math::clampFloat(body["longitude"].as<float>(), cfg::kMinLongitude, cfg::kMaxLongitude);
    scheduleConfigChanged = true;
  }
  if (body.containsKey("coilHoldMs")) {
    state.coilHoldMs = static_cast<uint16_t>(shutter::math::clampLong(body["coilHoldMs"].as<long>(), 0, cfg::kMaxCoilHoldMs));
  }
  if (body.containsKey("sunriseOffsetMinutes")) {
    state.sunriseOffsetMinutes = static_cast<int16_t>(shutter::math::clampLong(
        body["sunriseOffsetMinutes"].as<long>(), cfg::kMinScheduleOffsetMinutes, cfg::kMaxScheduleOffsetMinutes));
    scheduleConfigChanged = true;
  }
  if (body.containsKey("sunsetOffsetMinutes")) {
    state.sunsetOffsetMinutes = static_cast<int16_t>(shutter::math::clampLong(
        body["sunsetOffsetMinutes"].as<long>(), cfg::kMinScheduleOffsetMinutes, cfg::kMaxScheduleOffsetMinutes));
    scheduleConfigChanged = true;
  }
  if (body.containsKey("sunriseTargetPercent")) {
    state.sunriseTargetPercent = static_cast<uint8_t>(shutter::math::clampLong(body["sunriseTargetPercent"].as<long>(), 0, 100));
    scheduleConfigChanged = true;
  }
  if (body.containsKey("sunsetTargetPercent")) {
    state.sunsetTargetPercent = static_cast<uint8_t>(shutter::math::clampLong(body["sunsetTargetPercent"].as<long>(), 0, 100));
    scheduleConfigChanged = true;
  }
  if (body.containsKey("travelSteps")) {
    state.travelSteps = shutter::math::clampLong(body["travelSteps"].as<long>(), cfg::kMinTravelSteps, cfg::kMaxTravelSteps);
  }
  if (body.containsKey("timezone")) {
    timezoneSpec = String(static_cast<const char*>(body["timezone"] | timezoneSpec.c_str()));
    normalizeTimezoneSpec();
  }

  if (timezoneSpec != oldTimezoneSpec) {
    configureTimekeeping();
    scheduleConfigChanged = true;
  }
  if (scheduleConfigChanged) {
    resetSunRuntime();
  }

  applyStepperSettings();
  applyWiFiPowerMode();

  const long clampedPos = clampLogicalPosition(logicalPosBefore);
  targetPosition = clampLogicalPosition(logicalTargetBefore);

  stepper.setCurrentPosition(logicalToRaw(clampedPos));
  stepper.moveTo(logicalToRaw(targetPosition));
  resetTopReferenceWhenStopped = false;

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
  ESPhttpUpdate.rebootOnUpdate(false);
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  const char* targetName = updateFilesystem ? "filesystem" : "firmware";
  int lastErr = 0;
  String lastErrText = "unknown";

  for (uint8_t attempt = 1; attempt <= cfg::kOtaMaxAttempts; ++attempt) {
    delay(1);
    yield();
    if (WiFi.status() != WL_CONNECTED) {
      if (errorMessage) *errorMessage = "wifi disconnected before OTA";
      Serial.printf("[OTA] %s attempt %u/%u skipped: wifi disconnected\n",
                    targetName, attempt, cfg::kOtaMaxAttempts);
      return false;
    }

    Serial.printf("[OTA] %s attempt %u/%u, timeout=%ums, rssi=%d, heap=%u, url=%s\n",
                  targetName, attempt, cfg::kOtaMaxAttempts, cfg::kOtaClientTimeoutMs,
                  WiFi.RSSI(), ESP.getFreeHeap(), url.c_str());

    t_httpUpdate_return result = HTTP_UPDATE_FAILED;
    if (isHttps) {
      std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
      client->setInsecure();
      client->setTimeout(cfg::kOtaClientTimeoutMs);
      if (updateFilesystem) {
        result = ESPhttpUpdate.updateFS(*client, url);
      } else {
        result = ESPhttpUpdate.update(*client, url);
      }
    } else {
      WiFiClient client;
      client.setTimeout(cfg::kOtaClientTimeoutMs);
      if (updateFilesystem) {
        result = ESPhttpUpdate.updateFS(client, url);
      } else {
        result = ESPhttpUpdate.update(client, url);
      }
    }

    if (result == HTTP_UPDATE_OK) {
      Serial.printf("[OTA] %s attempt %u/%u result=OK\n", targetName, attempt, cfg::kOtaMaxAttempts);
      return true;
    }

    lastErr = ESPhttpUpdate.getLastError();
    lastErrText = ESPhttpUpdate.getLastErrorString();
    Serial.printf("[OTA] %s attempt %u/%u failed: code=%d msg=%s\n",
                  targetName, attempt, cfg::kOtaMaxAttempts, lastErr, lastErrText.c_str());

    if (attempt < cfg::kOtaMaxAttempts) {
      delay(cfg::kOtaRetryDelayMs);
      yield();
    }
  }

  if (errorMessage) {
    *errorMessage = String(lastErr) + ": " + lastErrText + " (attempts=" + String(cfg::kOtaMaxAttempts) + ")";
  }
  return false;
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

String githubReleaseAssetUrl(const String& tag, const String& assetName) {
  return "https://github.com/" + firmwareRepo + "/releases/download/" + tag + "/" + assetName;
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
  const bool restoreModemSleep = state.wifiModemSleep;
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  bool ok = true;
  if (includeFilesystem && !performHttpOta(filesystemUrl, true, &otaErr)) {
    if (errorMessage) *errorMessage = String("filesystem update failed: ") + otaErr;
    ok = false;
  }
  if (ok && !performHttpOta(firmwareUrl, false, &otaErr)) {
    if (errorMessage) *errorMessage = String("firmware update failed: ") + otaErr;
    ok = false;
  }

  WiFi.setSleepMode(restoreModemSleep ? WIFI_MODEM_SLEEP : WIFI_NONE_SLEEP);
  return ok;
}

bool queueOtaJob(
    const String& firmwareUrl,
    const String& filesystemUrl,
    bool includeFilesystem,
    const String& source,
    const String& tag,
    String* errorMessage) {
  if (errorMessage) errorMessage->clear();
  if (otaJob.pending || otaJob.running || otaJob.rebootScheduled) {
    if (errorMessage) *errorMessage = "ota already in progress";
    return false;
  }
  if (firmwareUrl.length() == 0) {
    if (errorMessage) *errorMessage = "firmware url missing";
    return false;
  }
  if (includeFilesystem && filesystemUrl.length() == 0) {
    if (errorMessage) *errorMessage = "filesystem url missing";
    return false;
  }

  otaJob.pending = true;
  otaJob.running = false;
  otaJob.rebootScheduled = false;
  otaJob.includeFilesystem = includeFilesystem;
  otaJob.firmwareUrl = firmwareUrl;
  otaJob.filesystemUrl = filesystemUrl;
  otaJob.source = source;
  otaJob.tag = tag;
  otaJob.phase = "queued";
  otaJob.lastError = "";
  otaJob.queuedAtMs = millis();
  otaJob.startedAtMs = 0;
  return true;
}

void processOtaJob() {
  if (!otaJob.pending || otaJob.running || otaJob.rebootScheduled) return;
  if (millis() - otaJob.queuedAtMs < cfg::kOtaQueueStartDelayMs) return;

  otaJob.pending = false;
  otaJob.running = true;
  otaJob.startedAtMs = millis();
  otaJob.phase = "starting";
  otaJob.lastError = "";

  // OTA blocks the main loop for a while; stop motion first to avoid leaving active drive.
  targetPosition = currentLogicalPosition();
  stepper.moveTo(logicalToRaw(targetPosition));
  resetTopReferenceWhenStopped = false;
  disableMotorOutputs();
  saveState(true);

  Serial.printf(
      "[OTA] job start source=%s tag=%s includeFS=%u queuedFor=%lus fw=%s fs=%s\n",
      otaJob.source.c_str(),
      otaJob.tag.c_str(),
      static_cast<unsigned>(otaJob.includeFilesystem),
      static_cast<unsigned long>((millis() - otaJob.queuedAtMs) / 1000),
      otaJob.firmwareUrl.c_str(),
      otaJob.filesystemUrl.c_str());

  otaJob.phase = "updating";
  String err;
  const bool ok = runOtaUpdate(otaJob.firmwareUrl, otaJob.filesystemUrl, otaJob.includeFilesystem, &err);
  otaJob.running = false;

  if (!ok) {
    otaJob.phase = "failed";
    otaJob.lastError = err;
    Serial.printf("[OTA] job failed: %s\n", err.c_str());
    return;
  }

  otaJob.rebootScheduled = true;
  otaJob.phase = "completed";
  otaJob.lastError = "";
  Serial.println("[OTA] job complete, rebooting");
  delay(300);
  ESP.restart();
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

  String queueErr;
  if (!queueOtaJob(firmwareUrl, filesystemUrl, includeFilesystem, "latest", "", &queueErr)) {
    sendError(queueErr.c_str(), 409);
    return;
  }

  StaticJsonDocument<384> doc;
  doc["ok"] = true;
  doc["message"] = "ota job queued";
  doc["queued"] = true;
  doc["source"] = "latest";
  doc["firmwareUrl"] = firmwareUrl;
  doc["filesystemUrl"] = filesystemUrl;
  sendJsonDocument(202, doc);
}

void handleApiFirmwareUpdateRelease() {
  StaticJsonDocument<768> body;
  if (!parseJsonBody(body)) {
    sendError("invalid json");
    return;
  }

  normalizeFirmwareConfig();
  if (!isValidGithubRepo(firmwareRepo)) {
    sendError("firmwareRepo must be owner/repo");
    return;
  }

  const String tag = String(static_cast<const char*>(body["tag"] | ""));
  if (tag.length() == 0) {
    sendError("release tag missing");
    return;
  }
  const bool includeFilesystem = body["includeFilesystem"] | true;

  String firmwareUrl = String(static_cast<const char*>(body["firmwareUrl"] | ""));
  String filesystemUrl = String(static_cast<const char*>(body["filesystemUrl"] | ""));
  if (firmwareUrl.length() == 0) firmwareUrl = githubReleaseAssetUrl(tag, firmwareAssetName);
  if (filesystemUrl.length() == 0) filesystemUrl = githubReleaseAssetUrl(tag, firmwareFsAssetName);

  String queueErr;
  if (!queueOtaJob(firmwareUrl, filesystemUrl, includeFilesystem, "release", tag, &queueErr)) {
    sendError(queueErr.c_str(), 409);
    return;
  }

  StaticJsonDocument<448> doc;
  doc["ok"] = true;
  doc["message"] = "ota release job queued";
  doc["queued"] = true;
  doc["source"] = "release";
  doc["tag"] = tag;
  doc["firmwareUrl"] = firmwareUrl;
  doc["filesystemUrl"] = filesystemUrl;
  sendJsonDocument(202, doc);
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

  String queueErr;
  if (!queueOtaJob(firmwareUrl, filesystemUrl, includeFilesystem, "url", "", &queueErr)) {
    sendError(queueErr.c_str(), 409);
    return;
  }

  StaticJsonDocument<384> doc;
  doc["ok"] = true;
  doc["message"] = "ota url job queued";
  doc["queued"] = true;
  doc["source"] = "url";
  sendJsonDocument(202, doc);
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
  server.on("/api/firmware/update/release", HTTP_POST, handleApiFirmwareUpdateRelease);
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

  EEPROM.begin(cfg::kEepromSize);
  eepromReady = true;

  loadState();

  applyStepperSettings();
  state.currentPosition = clampLogicalPosition(state.currentPosition);
  targetPosition = state.currentPosition;
  stepper.setCurrentPosition(logicalToRaw(state.currentPosition));
  stepper.moveTo(logicalToRaw(targetPosition));
  disableMotorOutputs();

  setupWiFi();
  configureTimekeeping();
  setupWebServer();

  saveState(true);
}

void loop() {
  server.handleClient();
  processOtaJob();
  processSunSchedule();

  const long rawBefore = stepper.currentPosition();
  const bool wasMoving = stepper.distanceToGo() != 0;
  stepper.run();
  const bool isMoving = stepper.distanceToGo() != 0;

  if (stepper.currentPosition() != rawBefore) {
    markDirty();
  }

  if (wasMoving && !isMoving) {
    motionStoppedAtMs = millis();
    if (resetTopReferenceWhenStopped) {
      stepper.setCurrentPosition(logicalToRaw(0));
      stepper.moveTo(logicalToRaw(0));
      resetTopReferenceWhenStopped = false;
    }
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
