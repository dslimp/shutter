#!/usr/bin/env bash
set -euo pipefail

HOST="${1:-192.168.88.74}"
BASE_URL="http://${HOST}"

api_get() {
  local path="$1"
  curl -sS --fail --max-time 8 "${BASE_URL}${path}"
}

api_post() {
  local path="$1"
  local body="$2"
  curl -sS --fail --max-time 8 \
    -X POST "${BASE_URL}${path}" \
    -H 'Content-Type: application/json' \
    -d "${body}"
}

json_get() {
  local json="$1"
  local key="$2"
  JSON_IN="${json}" KEY_IN="${key}" python3 - << 'PY'
import json, os
obj = json.loads(os.environ['JSON_IN'])
key = os.environ['KEY_IN']
val = obj[key]
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

echo "[INFO] Reading initial state from ${BASE_URL}/api/state"
initial_state="$(api_get '/api/state')"

orig_max_speed="$(json_get "${initial_state}" 'maxSpeed')"
orig_accel="$(json_get "${initial_state}" 'acceleration')"
orig_travel="$(json_get "${initial_state}" 'travelSteps')"
orig_hold="$(json_get "${initial_state}" 'coilHoldMs')"
orig_reverse="$(json_get "${initial_state}" 'reverseDirection')"
orig_fw_repo="$(json_get "${initial_state}" 'firmwareRepo')"
orig_fw_asset="$(json_get "${initial_state}" 'firmwareAssetName')"
orig_fw_fs_asset="$(json_get "${initial_state}" 'firmwareFsAssetName')"

echo "[INFO] Original settings: maxSpeed=${orig_max_speed}, acceleration=${orig_accel}, travelSteps=${orig_travel}, coilHoldMs=${orig_hold}, reverse=${orig_reverse}"
echo "[INFO] Original OTA: repo=${orig_fw_repo}, fw=${orig_fw_asset}, fs=${orig_fw_fs_asset}"

echo "[INFO] Verifying OTA default repo fallback when empty"
ota_default_state="$(api_post '/api/firmware/config' '{"firmwareRepo":"","firmwareAssetName":"","firmwareFsAssetName":""}')"
assert_eq "dslimp/shutter" "$(json_get "${ota_default_state}" 'firmwareRepo')" "firmwareRepo default fallback"
assert_eq "firmware.bin" "$(json_get "${ota_default_state}" 'firmwareAssetName')" "firmwareAssetName default fallback"
assert_eq "littlefs.bin" "$(json_get "${ota_default_state}" 'firmwareFsAssetName')" "firmwareFsAssetName default fallback"

test_payload='{"maxSpeed":1500,"acceleration":420,"travelSteps":12000,"coilHoldMs":650,"reverseDirection":false}'
echo "[INFO] Applying test settings"
post_state="$(api_post '/api/settings' "${test_payload}")"

assert_eq "1500" "$(json_get "${post_state}" 'maxSpeed')" "maxSpeed after save"
assert_eq "420" "$(json_get "${post_state}" 'acceleration')" "acceleration after save"
assert_eq "12000" "$(json_get "${post_state}" 'travelSteps')" "travelSteps after save"
assert_eq "650" "$(json_get "${post_state}" 'coilHoldMs')" "coilHoldMs after save"
assert_eq "false" "$(json_get "${post_state}" 'reverseDirection')" "reverseDirection after save"

echo "[INFO] Rebooting device (no Wi-Fi reset)"
api_post '/api/system/reboot' '{}' >/dev/null || true

echo "[INFO] Waiting for device to return..."
for i in $(seq 1 50); do
  if state_after_reboot="$(api_get '/api/state' 2>/dev/null)"; then
    echo "[INFO] Device is online after ${i}s"
    break
  fi
  sleep 1
done

if [[ -z "${state_after_reboot:-}" ]]; then
  echo "[FAIL] Device did not return after reboot" >&2
  exit 1
fi

assert_eq "1500" "$(json_get "${state_after_reboot}" 'maxSpeed')" "maxSpeed persisted"
assert_eq "420" "$(json_get "${state_after_reboot}" 'acceleration')" "acceleration persisted"
assert_eq "12000" "$(json_get "${state_after_reboot}" 'travelSteps')" "travelSteps persisted"
assert_eq "650" "$(json_get "${state_after_reboot}" 'coilHoldMs')" "coilHoldMs persisted"
assert_eq "false" "$(json_get "${state_after_reboot}" 'reverseDirection')" "reverseDirection persisted"

echo "[INFO] Restoring original settings"
restore_payload="{\"maxSpeed\":${orig_max_speed},\"acceleration\":${orig_accel},\"travelSteps\":${orig_travel},\"coilHoldMs\":${orig_hold},\"reverseDirection\":${orig_reverse}}"
api_post '/api/settings' "${restore_payload}" >/dev/null

echo "[INFO] Restoring original OTA config"
restore_ota_payload="{\"firmwareRepo\":\"${orig_fw_repo}\",\"firmwareAssetName\":\"${orig_fw_asset}\",\"firmwareFsAssetName\":\"${orig_fw_fs_asset}\"}"
api_post '/api/firmware/config' "${restore_ota_payload}" >/dev/null

echo "[PASS] Hardware settings persistence test completed"
