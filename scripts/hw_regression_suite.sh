#!/usr/bin/env bash
set -euo pipefail

HOST="${1:-192.168.88.74}"
BASE_URL="http://${HOST}"
WORK_DIR="/tmp/shutter_hw_regression"
HTTP_PORT="18080"
LOCAL_IP="${LOCAL_IP:-}"

api_get() {
  local path="$1"
  curl -sS --fail --max-time 8 "${BASE_URL}${path}"
}

api_post() {
  local path="$1"
  local body="$2"
  curl -sS --max-time 25 -X POST "${BASE_URL}${path}" -H 'Content-Type: application/json' -d "${body}"
}

json_get() {
  local json="$1"
  local key="$2"
  JSON_IN="${json}" KEY_IN="${key}" python3 - <<'PY'
import json, os
obj = json.loads(os.environ['JSON_IN'])
val = obj[os.environ['KEY_IN']]
if isinstance(val, bool):
    print('true' if val else 'false')
else:
    print(val)
PY
}

assert_eq() {
  local expected="$1"
  local actual="$2"
  local label="$3"
  if [[ "${expected}" != "${actual}" ]]; then
    echo "[FAIL] ${label}: expected=${expected}, actual=${actual}" >&2
    exit 1
  fi
  echo "[OK] ${label}: ${actual}"
}

wait_online() {
  local timeout="${1:-120}"
  local i
  for i in $(seq 1 "${timeout}"); do
    if api_get '/api/state' >/dev/null 2>&1; then
      echo "${i}"
      return 0
    fi
    sleep 1
  done
  return 1
}

wait_idle() {
  local timeout="${1:-120}"
  local i s moving
  for i in $(seq 1 "${timeout}"); do
    s="$(api_get '/api/state' 2>/dev/null || true)"
    if [[ -n "${s}" ]]; then
      moving="$(json_get "${s}" 'moving')"
      if [[ "${moving}" == "false" ]]; then
        echo "${i}"
        return 0
      fi
    fi
    sleep 1
  done
  return 1
}

wait_version() {
  local target="$1"
  local timeout="${2:-240}"
  local i s v
  for i in $(seq 1 "${timeout}"); do
    s="$(api_get '/api/state' 2>/dev/null || true)"
    if [[ -n "${s}" ]]; then
      v="$(json_get "${s}" 'version')"
      if [[ "${v}" == "${target}" ]]; then
        echo "${i}"
        return 0
      fi
    fi
    sleep 1
  done
  return 1
}

version_to_tag() {
  local v="$1"
  echo "v${v%%-*}"
}

tag_to_version() {
  local tag="$1"
  echo "${tag#v}-esp8266"
}

choose_alt_tag() {
  local current="$1"
  for t in v0.1.7 v0.1.6 v0.1.5; do
    if [[ "${t}" != "${current}" ]]; then
      echo "${t}"
      return 0
    fi
  done
  echo "v0.1.6"
}

trigger_reboot() {
  api_post '/api/system/reboot' '{}' >/dev/null || true
  wait_online 120 >/dev/null
}

save_ota_defaults() {
  local resp
  resp="$(api_post '/api/firmware/config' '{"firmwareRepo":"","firmwareAssetName":"","firmwareFsAssetName":""}')"
  assert_eq "dslimp/shutter" "$(json_get "${resp}" 'firmwareRepo')" "OTA repo fallback"
  assert_eq "firmware.bin" "$(json_get "${resp}" 'firmwareAssetName')" "OTA firmware asset fallback"
  assert_eq "littlefs.bin" "$(json_get "${resp}" 'firmwareFsAssetName')" "OTA littlefs asset fallback"

  local check
  check="$(api_post '/api/firmware/check/latest' '{"includeFilesystem":true}')"
  assert_eq "true" "$(json_get "${check}" 'ok')" "latest URL check ok"
}

apply_test_settings() {
  local payload resp
  payload='{"maxSpeed":1500,"acceleration":420,"travelSteps":12000,"coilHoldMs":650,"reverseDirection":true,"wifiModemSleep":false,"topOverdriveEnabled":true,"topOverdrivePercent":10}'
  resp="$(api_post '/api/settings' "${payload}")"
  assert_eq "1500" "$(json_get "${resp}" 'maxSpeed')" "maxSpeed save"
  assert_eq "true" "$(json_get "${resp}" 'reverseDirection')" "reverse save"
}

assert_settings_persisted() {
  local s="$1"
  local expected_travel="${2:-12000}"
  assert_eq "1500" "$(json_get "${s}" 'maxSpeed')" "maxSpeed persisted"
  assert_eq "420" "$(json_get "${s}" 'acceleration')" "acceleration persisted"
  assert_eq "${expected_travel}" "$(json_get "${s}" 'travelSteps')" "travelSteps persisted"
  assert_eq "650" "$(json_get "${s}" 'coilHoldMs')" "coilHoldMs persisted"
  assert_eq "true" "$(json_get "${s}" 'reverseDirection')" "reverse persisted"
  assert_eq "true" "$(json_get "${s}" 'topOverdriveEnabled')" "topOverdriveEnabled persisted"
  assert_eq "10" "$(json_get "${s}" 'topOverdrivePercent')" "topOverdrivePercent persisted"
}

run_calibration_case() {
  local steps="${1:-600}"
  local s

  api_post '/api/calibrate' '{"action":"reset"}' >/dev/null
  api_post '/api/calibrate' '{"action":"set_top"}' >/dev/null
  api_post '/api/calibrate' "{\"action\":\"jog\",\"steps\":${steps}}" >/dev/null
  wait_idle 120 >/dev/null
  s="$(api_post '/api/calibrate' '{"action":"set_bottom"}')"

  assert_eq "true" "$(json_get "${s}" 'calibrated')" "calibration flag set"
  assert_eq "${steps}" "$(json_get "${s}" 'travelSteps')" "calibration travel steps"
}

run_github_ota() {
  local tag="$1"
  local expected="$2"
  echo "[INFO] GitHub OTA -> ${tag}"
  api_post '/api/firmware/update/release' "{\"tag\":\"${tag}\",\"includeFilesystem\":true}" >/dev/null || true
  local t
  t="$(wait_version "${expected}" 240 || true)"
  if [[ -z "${t}" ]]; then
    echo "[FAIL] GitHub OTA did not reach version ${expected}" >&2
    return 1
  fi
  echo "[OK] GitHub OTA reached ${expected} in ${t}s"
  return 0
}

setup_local_server() {
  mkdir -p "${WORK_DIR}"
  rm -f "${WORK_DIR}/http.pid"

  if [[ -z "${LOCAL_IP}" ]]; then
    LOCAL_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
  fi
  if [[ -z "${LOCAL_IP}" ]]; then
    echo "[FAIL] Cannot determine local IP for local OTA server" >&2
    exit 1
  fi

  python3 -m http.server "${HTTP_PORT}" --bind 0.0.0.0 --directory "${WORK_DIR}" >"${WORK_DIR}/http.log" 2>&1 &
  echo $! > "${WORK_DIR}/http.pid"
  sleep 1
  curl -sS --fail --max-time 5 "http://${LOCAL_IP}:${HTTP_PORT}/" >/dev/null
}

download_release_bins() {
  local tag="$1"
  curl -fLsS -o "${WORK_DIR}/firmware-${tag}.bin" "https://github.com/dslimp/shutter/releases/download/${tag}/firmware.bin"
  curl -fLsS -o "${WORK_DIR}/littlefs-${tag}.bin" "https://github.com/dslimp/shutter/releases/download/${tag}/littlefs.bin"
}

run_local_ota() {
  local tag="$1"
  local expected="$2"
  local fw="http://${LOCAL_IP}:${HTTP_PORT}/firmware-${tag}.bin"
  local fs="http://${LOCAL_IP}:${HTTP_PORT}/littlefs-${tag}.bin"
  echo "[INFO] Local OTA -> ${tag}"
  api_post '/api/firmware/update/url' "{\"firmwareUrl\":\"${fw}\",\"filesystemUrl\":\"${fs}\",\"includeFilesystem\":true}" >/dev/null || true
  local t
  t="$(wait_version "${expected}" 240 || true)"
  if [[ -z "${t}" ]]; then
    echo "[FAIL] Local OTA did not reach version ${expected}" >&2
    exit 1
  fi
  echo "[OK] Local OTA reached ${expected} in ${t}s"
}

cleanup() {
  if [[ -f "${WORK_DIR}/http.pid" ]]; then
    local pid
    pid="$(cat "${WORK_DIR}/http.pid" 2>/dev/null || true)"
    if [[ -n "${pid}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
    fi
  fi
}
trap cleanup EXIT

echo "[INFO] Starting hardware regression suite on ${HOST}"
state0="$(api_get '/api/state')"
current_version="$(json_get "${state0}" 'version')"
current_tag="$(version_to_tag "${current_version}")"
alt_tag="$(choose_alt_tag "${current_tag}")"
alt_version="$(tag_to_version "${alt_tag}")"

echo "[INFO] Current firmware=${current_version}, switch target=${alt_tag}"

save_ota_defaults
apply_test_settings
run_calibration_case 600

trigger_reboot
state1="$(api_get '/api/state')"
assert_settings_persisted "${state1}" "600"
assert_eq "true" "$(json_get "${state1}" 'calibrated')" "calibrated persisted after reboot"
assert_eq "600" "$(json_get "${state1}" 'travelSteps')" "travelSteps persisted after reboot"

github_ota_ok=true
if run_github_ota "${alt_tag}" "${alt_version}"; then
  state2="$(api_get '/api/state')"
  assert_settings_persisted "${state2}" "600"
  assert_eq "true" "$(json_get "${state2}" 'calibrated')" "calibrated persisted after github ota"
  assert_eq "600" "$(json_get "${state2}" 'travelSteps')" "travelSteps persisted after github ota"
else
  github_ota_ok=false
fi

setup_local_server
download_release_bins "${current_tag}"
run_local_ota "${current_tag}" "${current_version}"
state3="$(api_get '/api/state')"
assert_settings_persisted "${state3}" "600"
assert_eq "true" "$(json_get "${state3}" 'calibrated')" "calibrated persisted after local ota"
assert_eq "600" "$(json_get "${state3}" 'travelSteps')" "travelSteps persisted after local ota"

if [[ "${github_ota_ok}" != "true" ]]; then
  echo "[FAIL] Suite completed with GitHub OTA failures" >&2
  exit 1
fi
echo "[PASS] Hardware regression suite completed"
