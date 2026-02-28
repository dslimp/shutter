let latestState = null;
let settingsDirty = false;
let firmwareReleases = [];

const MOTION_NAMES = {
  idle: 'Стоп',
  opening: 'Открывается',
  closing: 'Закрывается',
};

async function req(path, method = 'GET', body = null) {
  const response = await fetch(path, {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
  });

  let payload = {};
  try {
    payload = await response.json();
  } catch (_) {
    payload = {};
  }

  if (!response.ok || payload.ok === false) {
    const message = payload.error || `HTTP ${response.status}`;
    throw new Error(message);
  }

  return payload;
}

function setStatus(text, isError = false) {
  const el = document.getElementById('status');
  el.textContent = text;
  el.classList.toggle('error', isError);
}

function showTab(name) {
  const tabIds = ['Control', 'Calib', 'Settings'];
  tabIds.forEach((suffix) => {
    const tab = document.getElementById(`tab${suffix}`);
    const btn = document.getElementById(`tab${suffix}Btn`);
    const shouldShow = suffix.toLowerCase() === name;
    tab.classList.toggle('active', shouldShow);
    btn.classList.toggle('active', shouldShow);
  });
}

function setInputValue(id, value) {
  if (settingsDirty) return;
  const el = document.getElementById(id);
  if (!el || document.activeElement === el) return;
  el.value = value;
}

function setCheckboxValue(id, value) {
  if (settingsDirty) return;
  const el = document.getElementById(id);
  if (!el || document.activeElement === el) return;
  el.checked = !!value;
}

function setTextValue(id, value) {
  const el = document.getElementById(id);
  if (!el || document.activeElement === el) return;
  el.value = value;
}

function renderState(state) {
  latestState = state;

  const posPercent = Number(state.positionPercent || 0);
  const targetPercent = Number(state.targetPercent || 0);

  document.getElementById('chipMotion').textContent = MOTION_NAMES[state.motion] || state.motion || '-';
  document.getElementById('chipPos').textContent = `${posPercent.toFixed(1)}%`;
  document.getElementById('chipWifi').textContent = state.ssid ? `${state.ssid} (${state.rssi}dBm)` : 'offline';
  document.getElementById('chipA0').textContent = Number(state.a0Raw ?? 0).toFixed(0);

  document.getElementById('motionName').textContent = MOTION_NAMES[state.motion] || state.motion || '-';
  document.getElementById('calibName').textContent = state.calibrated ? 'Выполнена' : 'Не выполнена';
  document.getElementById('posPercent').textContent = `${posPercent.toFixed(1)}%`;
  document.getElementById('targetPercentView').textContent = `${targetPercent.toFixed(1)}%`;
  document.getElementById('stepsView').textContent = `${state.positionSteps} / ${state.travelSteps}`;
  document.getElementById('ipView').textContent = state.ip || '-';
  document.getElementById('rawState').textContent = JSON.stringify(state, null, 2);

  setInputValue('travelSteps', state.travelSteps);
  setInputValue('maxSpeed', Number(state.maxSpeed || 0).toFixed(0));
  setInputValue('acceleration', Number(state.acceleration || 0).toFixed(0));
  setInputValue('coilHoldMs', state.coilHoldMs);
  setCheckboxValue('reverseDirection', state.reverseDirection);
  setCheckboxValue('wifiModemSleep', state.wifiModemSleep);
  setCheckboxValue('topOverdriveEnabled', state.topOverdriveEnabled);
  setInputValue('topOverdrivePercent', Number(state.topOverdrivePercent ?? 10).toFixed(0));
  setTextValue('fwRepo', state.firmwareRepo || '');
  setTextValue('fwAssetName', state.firmwareAssetName || 'firmware.bin');
  setTextValue('fwFsAssetName', state.firmwareFsAssetName || 'littlefs.bin');
}

async function refresh() {
  try {
    const state = await req('/api/state');
    renderState(state);
  } catch (error) {
    setStatus(`Ошибка связи: ${error.message}`, true);
  }
}

async function moveAction(action, extra = {}) {
  try {
    const state = await req('/api/move', 'POST', { action, ...extra });
    renderState(state);
    setStatus(`Команда выполнена: ${action}`);
  } catch (error) {
    setStatus(`Ошибка движения: ${error.message}`, true);
  }
}

function moveOpen() {
  moveAction('open');
}

function moveClose() {
  moveAction('close');
}

function moveStop() {
  moveAction('stop');
}

function moveToPercent() {
  const raw = Number(document.getElementById('targetPercent').value);
  if (!Number.isFinite(raw)) {
    setStatus('Некорректный процент', true);
    return;
  }
  const percent = Math.max(0, Math.min(100, raw));
  moveAction('set', { percent });
}

function jogUp() {
  const steps = Math.abs(parseInt(document.getElementById('jogSteps').value, 10) || 0);
  if (!steps) {
    setStatus('Укажите шаги для подстройки', true);
    return;
  }
  moveAction('jog', { steps: -steps });
}

function jogDown() {
  const steps = Math.abs(parseInt(document.getElementById('jogSteps').value, 10) || 0);
  if (!steps) {
    setStatus('Укажите шаги для подстройки', true);
    return;
  }
  moveAction('jog', { steps });
}

function calJogUp() {
  const steps = Math.abs(parseInt(document.getElementById('calJogSteps').value, 10) || 0);
  if (!steps) {
    setStatus('Укажите шаги для калибровки', true);
    return;
  }
  req('/api/calibrate', 'POST', { action: 'jog', steps: -steps })
    .then((state) => {
      renderState(state);
      setStatus('Калибровочный джог выполнен');
    })
    .catch((error) => setStatus(`Ошибка калибровки: ${error.message}`, true));
}

function calJogDown() {
  const steps = Math.abs(parseInt(document.getElementById('calJogSteps').value, 10) || 0);
  if (!steps) {
    setStatus('Укажите шаги для калибровки', true);
    return;
  }
  req('/api/calibrate', 'POST', { action: 'jog', steps })
    .then((state) => {
      renderState(state);
      setStatus('Калибровочный джог выполнен');
    })
    .catch((error) => setStatus(`Ошибка калибровки: ${error.message}`, true));
}

async function setTop() {
  try {
    const state = await req('/api/calibrate', 'POST', { action: 'set_top' });
    renderState(state);
    setStatus('Верхняя точка установлена (0%)');
  } catch (error) {
    setStatus(`Ошибка калибровки: ${error.message}`, true);
  }
}

async function setBottom() {
  try {
    const state = await req('/api/calibrate', 'POST', { action: 'set_bottom' });
    renderState(state);
    setStatus('Нижняя точка установлена (100%), калибровка завершена');
  } catch (error) {
    setStatus(`Ошибка калибровки: ${error.message}`, true);
  }
}

async function resetCalibration() {
  try {
    const state = await req('/api/calibrate', 'POST', { action: 'reset' });
    renderState(state);
    setStatus('Флаг калибровки сброшен');
  } catch (error) {
    setStatus(`Ошибка: ${error.message}`, true);
  }
}

async function saveSettings() {
  const payload = {
    reverseDirection: document.getElementById('reverseDirection').checked,
    wifiModemSleep: document.getElementById('wifiModemSleep').checked,
    topOverdriveEnabled: document.getElementById('topOverdriveEnabled').checked,
    travelSteps: Number(document.getElementById('travelSteps').value),
    maxSpeed: Number(document.getElementById('maxSpeed').value),
    acceleration: Number(document.getElementById('acceleration').value),
    coilHoldMs: Number(document.getElementById('coilHoldMs').value),
    topOverdrivePercent: Number(document.getElementById('topOverdrivePercent').value),
  };

  try {
    const state = await req('/api/settings', 'POST', payload);
    settingsDirty = false;
    renderState(state);
    setStatus('Настройки сохранены');
  } catch (error) {
    setStatus(`Ошибка сохранения: ${error.message}`, true);
  }
}

async function resetWifi() {
  if (!confirm('Удалить сохраненный Wi-Fi и перезапустить устройство?')) return;

  try {
    await req('/api/wifi/reset', 'POST', {});
    setStatus('Wi-Fi сброшен, устройство перезагружается...');
  } catch (error) {
    setStatus(`Ошибка сброса Wi-Fi: ${error.message}`, true);
  }
}

function setFwStatus(text, isError = false) {
  const fw = document.getElementById('fwStatusText');
  if (fw) fw.textContent = text;
  setStatus(text, isError);
}

async function saveFirmwareConfig() {
  const payload = {
    firmwareRepo: document.getElementById('fwRepo').value.trim(),
    firmwareAssetName: document.getElementById('fwAssetName').value.trim(),
    firmwareFsAssetName: document.getElementById('fwFsAssetName').value.trim(),
  };

  try {
    const response = await req('/api/firmware/config', 'POST', payload);
    if (latestState) {
      latestState.firmwareRepo = response.firmwareRepo;
      latestState.firmwareAssetName = response.firmwareAssetName;
      latestState.firmwareFsAssetName = response.firmwareFsAssetName;
    }
    setFwStatus('OTA конфиг сохранён');
  } catch (error) {
    setFwStatus(`Ошибка OTA конфига: ${error.message}`, true);
  }
}

async function updateFirmwareLatest() {
  if (!confirm('Обновить прошивку и LittleFS до последнего релиза?')) return;
  setFwStatus('Запуск OTA latest...');
  try {
    await req('/api/firmware/update/latest', 'POST', { includeFilesystem: true });
    setFwStatus('OTA запущено, устройство перезагружается...');
  } catch (error) {
    setFwStatus(`OTA ошибка: ${error.message}`, true);
  }
}

function renderFirmwareReleases(items) {
  firmwareReleases = Array.isArray(items) ? items : [];
  const select = document.getElementById('fwReleaseSelect');
  if (!select) return;

  select.innerHTML = '';
  if (firmwareReleases.length === 0) {
    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = 'Релизы не найдены';
    select.appendChild(opt);
    return;
  }

  firmwareReleases.forEach((rel, idx) => {
    const opt = document.createElement('option');
    opt.value = String(idx);
    const tag = rel.tag || '-';
    const name = rel.name && rel.name !== tag ? ` — ${rel.name}` : '';
    const pre = rel.prerelease ? ' (pre)' : '';
    opt.textContent = `${tag}${pre}${name}`;
    select.appendChild(opt);
  });
}

async function loadFirmwareReleases() {
  const repoRaw = document.getElementById('fwRepo').value.trim();
  const repo = repoRaw || 'dslimp/shutter';
  if (!/^[^/]+\/[^/]+$/.test(repo)) {
    setFwStatus('Неверный формат repo, нужно owner/repo', true);
    return;
  }
  const fwAsset = document.getElementById('fwAssetName').value.trim() || 'firmware.bin';
  const fsAsset = document.getElementById('fwFsAssetName').value.trim() || 'littlefs.bin';

  setFwStatus('Загрузка релизов...');
  try {
    const response = await fetch(`https://api.github.com/repos/${repo}/releases?per_page=20`, {
      headers: { Accept: 'application/vnd.github+json' },
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);

    const releasesRaw = await response.json();
    const releases = (Array.isArray(releasesRaw) ? releasesRaw : []).map((item) => {
      const tag = String(item?.tag_name || '');
      const name = String(item?.name || '');
      return {
        tag,
        name: name || tag,
        prerelease: Boolean(item?.prerelease),
        draft: Boolean(item?.draft),
        publishedAt: String(item?.published_at || ''),
        firmwareUrl: `https://github.com/${repo}/releases/download/${tag}/${fwAsset}`,
        filesystemUrl: `https://github.com/${repo}/releases/download/${tag}/${fsAsset}`,
      };
    }).filter((item) => item.tag.length > 0 && !item.draft);

    renderFirmwareReleases(releases);
    setFwStatus(`Релизы загружены: ${releases.length}`);
  } catch (error) {
    setFwStatus(`Ошибка загрузки релизов: ${error.message}`, true);
  }
}

async function updateFirmwareSelectedRelease() {
  const select = document.getElementById('fwReleaseSelect');
  const idx = Number(select?.value ?? -1);
  const release = Number.isInteger(idx) && idx >= 0 ? firmwareReleases[idx] : null;
  if (!release || !release.tag) {
    setFwStatus('Выберите релиз из списка', true);
    return;
  }
  if (!confirm(`Обновить до релиза ${release.tag}?`)) return;
  setFwStatus(`Запуск OTA релиза ${release.tag}...`);
  try {
    await req('/api/firmware/update/release', 'POST', {
      tag: release.tag,
      firmwareUrl: release.firmwareUrl || '',
      filesystemUrl: release.filesystemUrl || '',
      includeFilesystem: true,
    });
    setFwStatus(`OTA ${release.tag} запущено, устройство перезагружается...`);
  } catch (error) {
    setFwStatus(`OTA ошибка: ${error.message}`, true);
  }
}

async function checkFirmwareLatestUrls() {
  setFwStatus('Проверка latest URL...');
  try {
    const res = await req('/api/firmware/check/latest', 'POST', { includeFilesystem: true });
    const fw = res.firmware || {};
    const fs = res.filesystem || {};
    setFwStatus(`OK: network=${res.networkOk}, fw=${fw.ok}, fs=${fs.ok}`);
  } catch (error) {
    setFwStatus(`Проверка latest URL: ${error.message}`, true);
  }
}

async function updateFirmwareFromUrl() {
  const firmwareUrl = document.getElementById('fwUrl').value.trim();
  const filesystemUrl = document.getElementById('fwFsUrl').value.trim();
  if (!firmwareUrl || !filesystemUrl) {
    setFwStatus('Укажите оба URL (firmware и littlefs)', true);
    return;
  }
  if (!confirm('Обновить прошивку из указанных URL?')) return;
  setFwStatus('Запуск OTA по URL...');
  try {
    await req('/api/firmware/update/url', 'POST', { firmwareUrl, filesystemUrl, includeFilesystem: true });
    setFwStatus('OTA запущено, устройство перезагружается...');
  } catch (error) {
    setFwStatus(`OTA ошибка: ${error.message}`, true);
  }
}

showTab('control');

['travelSteps', 'maxSpeed', 'acceleration', 'coilHoldMs', 'topOverdrivePercent', 'reverseDirection', 'wifiModemSleep', 'topOverdriveEnabled'].forEach((id) => {
  const el = document.getElementById(id);
  if (!el) return;
  el.addEventListener('input', () => { settingsDirty = true; });
  el.addEventListener('change', () => { settingsDirty = true; });
});

refresh();
setInterval(refresh, 800);
