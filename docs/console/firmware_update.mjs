export const APP_FLASH_ADDRESS = 0x10000;
export const APP_FLASH_END = 0x310000;
export const APP_FLASH_MAX_BYTES = APP_FLASH_END - APP_FLASH_ADDRESS;
export const ESP32_S3_IMAGE_CHIP_ID = 9;
const ESP_IMAGE_HEADER_BYTES = 24;
const ESP_IMAGE_MIN_BYTES = 32;

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
