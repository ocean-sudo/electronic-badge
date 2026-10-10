export const APP_FLASH_ADDRESS = 0x10000;
export const APP_FLASH_END = 0x310000;
export const APP_FLASH_MAX_BYTES = APP_FLASH_END - APP_FLASH_ADDRESS;
export const ESP32_S3_IMAGE_CHIP_ID = 9;
export const LATEST_FIRMWARE_RELEASE_API = 'https://api.github.com/repos/ocean-sudo/electronic-badge/releases/latest';
const RELEASE_DOWNLOAD_BASE = 'https://github.com/ocean-sudo/electronic-badge/releases/download';
const ESP_IMAGE_HEADER_BYTES = 24;
const ESP_IMAGE_MIN_BYTES = 32;
const RELEASE_ASSET_NAMES = ['firmware.bin', 'SHA256SUMS', 'BUILD-INFO.txt'];

export function validateFirmwareFileSize(size) {
  if (!Number.isSafeInteger(size) || size <= 0) {
    throw new Error('固件文件为空或大小无效。');
  }
  if (size < ESP_IMAGE_MIN_BYTES) {
    throw new Error('固件文件太短，不是完整的 ESP 应用镜像。');
  }
  if (size > APP_FLASH_MAX_BYTES) {
    throw new Error('固件超出 APP 分区范围 0x10000–0x310000；拒绝写入。');
  }
}

export function validateFirmwareImage(bytes) {
  if (!(bytes instanceof Uint8Array)) throw new TypeError('固件必须是 Uint8Array。');
  validateFirmwareFileSize(bytes.byteLength);
  if (bytes[0] !== 0xe9) throw new Error('ESP 应用镜像头无效（magic 必须为 0xE9）。');
  const segments = bytes[1];
  if (segments < 1 || segments > 16) throw new Error('ESP 镜像段数无效。');
  const chipId = bytes[12] | (bytes[13] << 8);
  if (chipId !== ESP32_S3_IMAGE_CHIP_ID) {
    throw new Error('ESP 镜像头声明的 chip_id 不是 ESP32-S3；拒绝写入。');
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let offset = ESP_IMAGE_HEADER_BYTES;
  for (let i = 0; i < segments; i++) {
    if (offset + 8 > bytes.byteLength) throw new Error('ESP 镜像段头超出文件边界。');
    const length = view.getUint32(offset + 4, true);
    offset += 8;
    if (!length || offset + length > bytes.byteLength) {
      throw new Error('ESP 镜像段长度无效或超出文件边界。');
    }
    offset += length;
  }
  if (offset >= bytes.byteLength) throw new Error('ESP 镜像缺少段校验数据。');
}

export async function readFirmwareFile(file) {
  validateFirmwareFileSize(file.size);
  const bytes = new Uint8Array(await file.arrayBuffer());
  if (bytes.byteLength !== file.size) throw new Error('固件文件读取长度与文件大小不一致。');
  validateFirmwareImage(bytes);
  return bytes;
}

export function parseFirmwareChecksum(manifest) {
  if (typeof manifest !== 'string') throw new TypeError('SHA256SUMS 必须是文本。');
  const match = manifest.match(/^([a-f\d]{64})[ \t]+\*?firmware\.bin\r?\n?$/i);
  if (!match) throw new Error('SHA256SUMS 格式无效或未唯一声明 firmware.bin。');
  return match[1].toLowerCase();
}

async function readOkResponse(response, assetName) {
  if (!response?.ok) {
    const status = Number.isInteger(response?.status) ? ` HTTP ${response.status}` : '';
    throw new Error(`固定仓库的 ${assetName} 下载失败${status}。`);
  }
  return response;
}

export async function fetchLatestFirmware(fetchImpl = globalThis.fetch) {
  if (typeof fetchImpl !== 'function') throw new Error('当前浏览器不支持固件下载。');
  let releaseResponse;
  try {
    releaseResponse = await fetchImpl(LATEST_FIRMWARE_RELEASE_API, {
      headers: { Accept: 'application/vnd.github+json' },
      cache: 'no-store'
    });
  } catch {
    throw new Error('无法访问固定仓库的最新 Release（网络或 CORS 错误）；可改用本机固件文件。');
  }
  await readOkResponse(releaseResponse, 'latest Release');

  let release;
  try {
    release = await releaseResponse.json();
  } catch {
    throw new Error('固定仓库的 latest Release 响应不是有效 JSON。');
  }
  if (release?.draft === true || release?.prerelease === true || !/^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(release?.tag_name ?? '')) {
    throw new Error('固定仓库 latest Release 不是稳定的 vMAJOR.MINOR.PATCH 版本。');
  }
  const assets = Array.isArray(release.assets) ? release.assets : [];
  const selected = Object.fromEntries(RELEASE_ASSET_NAMES.map(name => {
    const matching = assets.filter(asset => asset?.name === name);
    if (matching.length !== 1 || !Number.isSafeInteger(matching[0]?.size) || matching[0].size < 0) {
      throw new Error(`Release 缺少唯一有效的 ${name} 资产。`);
    }
    return [name, matching[0]];
  }));
  validateFirmwareFileSize(selected['firmware.bin'].size);
  if (selected.SHA256SUMS.size > 1024 || selected['BUILD-INFO.txt'].size > 16384) {
    throw new Error('Release 校验清单或构建信息文件超出允许大小。');
  }

  const download = async name => {
    const url = `${RELEASE_DOWNLOAD_BASE}/${encodeURIComponent(release.tag_name)}/${name}`;
    let response;
    try {
      response = await fetchImpl(url, { cache: 'no-store' });
    } catch {
      throw new Error(`无法下载固定仓库的 ${name}（网络或 CORS 错误）；可改用本机固件文件。`);
    }
    return readOkResponse(response, name);
  };

  const [firmwareResponse, sumsResponse] = await Promise.all([
    download('firmware.bin'),
    download('SHA256SUMS')
  ]);
  let manifest;
  try {
    manifest = await sumsResponse.text();
  } catch {
    throw new Error('无法读取 SHA256SUMS 文本。');
  }
  const expectedSha256 = parseFirmwareChecksum(manifest);
  let bytes;
  try {
    bytes = new Uint8Array(await firmwareResponse.arrayBuffer());
  } catch {
    throw new Error('无法读取固件二进制资产。');
  }
  if (bytes.byteLength !== selected['firmware.bin'].size) {
    throw new Error('固件下载长度与 Release 资产大小不一致。');
  }
  validateFirmwareFileSize(bytes.byteLength);
  let actualSha256;
  try {
    const digest = await globalThis.crypto.subtle.digest('SHA-256', bytes);
    actualSha256 = Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, '0')).join('');
  } catch {
    throw new Error('当前浏览器无法计算 SHA-256；拒绝启用固件刷写。');
  }
  if (actualSha256 !== expectedSha256) throw new Error('firmware.bin 的 SHA-256 与 SHA256SUMS 不匹配；拒绝写入。');
  validateFirmwareImage(bytes);
  return { bytes, tag: release.tag_name, sha256: actualSha256 };
}

export function appOnlyFlashOptions(bytes, reportProgress) {
  validateFirmwareImage(bytes);
  return {
    fileArray: [{ data: bytes, address: APP_FLASH_ADDRESS }],
    flashMode: 'qio',
    flashFreq: '80m',
    flashSize: '32MB',
    eraseAll: false,
    compress: true,
    reportProgress
  };
}
