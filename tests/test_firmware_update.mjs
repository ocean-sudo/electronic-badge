import assert from 'node:assert/strict';
import test from 'node:test';
import { webcrypto } from 'node:crypto';
import {
  APP_FLASH_ADDRESS,
  APP_FLASH_END,
  APP_FLASH_MAX_BYTES,
  ESP32_S3_IMAGE_CHIP_ID,
  appOnlyFlashOptions,
  readFirmwareFile,
  validateFirmwareFileSize,
  validateFirmwareImage,
  LATEST_FIRMWARE_RELEASE_API,
  RELEASE_FIRMWARE_BASE_URL,
  fetchLatestFirmware,
  parseFirmwareChecksum
} from '../docs/console/firmware_update.mjs';

function validImage(size = 34) {
  const bytes = new Uint8Array(size);
  bytes[0] = 0xe9;
  bytes[1] = 1;
  bytes[12] = ESP32_S3_IMAGE_CHIP_ID;
  if (size >= 32) new DataView(bytes.buffer).setUint32(28, 1, true);
  return bytes;
}

function fileFor(bytes) {
  return {
    size: bytes.byteLength,
    async arrayBuffer() { return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength); }
  };
}

test('rejects empty, short, and oversized files before reading their buffers', async () => {
  for (const size of [0, 31, APP_FLASH_MAX_BYTES + 1]) {
    let read = false;
    await assert.rejects(readFirmwareFile({
      size,
      async arrayBuffer() { read = true; throw new Error('must not read'); }
    }));
    assert.equal(read, false, 'arrayBuffer called for rejected size ' + size);
  }
});

test('accepts the exact APP maximum and rejects one byte over', () => {
  assert.equal(APP_FLASH_END - APP_FLASH_ADDRESS, 0x300000);
  assert.equal(APP_FLASH_MAX_BYTES, 0x300000);
  assert.doesNotThrow(() => validateFirmwareFileSize(APP_FLASH_MAX_BYTES));
  assert.throws(() => validateFirmwareFileSize(APP_FLASH_MAX_BYTES + 1));
  assert.doesNotThrow(() => validateFirmwareImage(validImage(APP_FLASH_MAX_BYTES)));
});

test('rejects malformed and truncated ESP images and non-S3 chip IDs', () => {
  const badMagic = validImage();
  badMagic[0] = 0;
  assert.throws(() => validateFirmwareImage(badMagic), /magic/);

  const badSegmentCount = validImage();
  badSegmentCount[1] = 0;
  assert.throws(() => validateFirmwareImage(badSegmentCount), /段数/);

  const wrongChip = validImage();
  wrongChip[12] = 5;
  assert.throws(() => validateFirmwareImage(wrongChip), /chip_id/);

  const truncatedHeader = validImage(31);
  assert.throws(() => validateFirmwareImage(truncatedHeader), /太短/);

  const truncatedSegment = validImage(32);
  assert.throws(() => validateFirmwareImage(truncatedSegment), /长度/);

  const missingChecksum = validImage(33);
  assert.throws(() => validateFirmwareImage(missingChecksum), /校验/);

  const segmentPastEnd = validImage();
  new DataView(segmentPastEnd.buffer).setUint32(28, 99, true);
  assert.throws(() => validateFirmwareImage(segmentPastEnd), /长度/);
});

test('reads and validates the declared image after checking file size', async () => {
  const bytes = validImage();
  assert.deepEqual(await readFirmwareFile(fileFor(bytes)), bytes);
  await assert.rejects(readFirmwareFile({ ...fileFor(bytes), size: bytes.byteLength + 1 }), /文件大小不一致/);
});

test('builds an APP-only, non-erasing write request', () => {
  const bytes = validImage();
  const reportProgress = () => {};
  const options = appOnlyFlashOptions(bytes, reportProgress);
  assert.deepEqual(options.fileArray, [{ data: bytes, address: 0x10000 }]);
  assert.equal(options.fileArray.length, 1);
  assert.equal(options.fileArray[0].address, APP_FLASH_ADDRESS);
  assert.equal(options.eraseAll, false);
  assert.equal(options.reportProgress, reportProgress);
});


if (!globalThis.crypto?.subtle) globalThis.crypto = webcrypto;

test('parses only one SHA256SUMS entry for firmware.bin', () => {
  const hash = 'a'.repeat(64);
  assert.equal(parseFirmwareChecksum(hash + '  firmware.bin\n'), hash);
  assert.equal(parseFirmwareChecksum(hash.toUpperCase() + ' *firmware.bin'), hash);
  assert.throws(() => parseFirmwareChecksum(hash + '  firmware.bin\n' + hash + '  other.bin'), /格式/);
  assert.throws(() => parseFirmwareChecksum(hash + '  other.bin'), /格式/);
  assert.throws(() => parseFirmwareChecksum('bad  firmware.bin'), /格式/);
});

function releaseFixture(bytes, checksum) {
  return {
    tag_name: 'v2.0.0',
    draft: false,
    prerelease: false,
    assets: [
      { name: 'firmware.bin', size: bytes.byteLength, browser_download_url: 'https://untrusted.invalid/firmware.bin' },
      { name: 'SHA256SUMS', size: checksum.length, browser_download_url: 'https://untrusted.invalid/SHA256SUMS' },
      { name: 'BUILD-INFO.txt', size: 10, browser_download_url: 'https://untrusted.invalid/BUILD-INFO.txt' }
    ]
  };
}

async function sha256(bytes) {
  const digest = await webcrypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, '0')).join('');
}

function response(body) {
  return { ok: true, async json() { return body; } };
}

test('downloads the same-origin Pages mirror and verifies release bytes before image validation', async () => {
  const bytes = validImage();
  const checksum = await sha256(bytes);
  const urls = [];
  const fetchStub = async url => {
    urls.push(url);
    if (url === LATEST_FIRMWARE_RELEASE_API) return response(releaseFixture(bytes, checksum + '  firmware.bin\n'));
    if (url.endsWith('/firmware.bin')) return { ok: true, async arrayBuffer() { return bytes.buffer; } };
    if (url.endsWith('/SHA256SUMS')) return { ok: true, async text() { return checksum + '  firmware.bin\n'; } };
    throw new Error('unexpected URL');
  };
  const result = await fetchLatestFirmware(fetchStub);
  assert.equal(result.tag, 'v2.0.0');
  assert.equal(result.sha256, checksum);
  assert.deepEqual(result.bytes, bytes);
  assert.equal(urls[0], LATEST_FIRMWARE_RELEASE_API);
  assert(urls.every(url => url === LATEST_FIRMWARE_RELEASE_API || url.startsWith(new URL('v2.0.0/', RELEASE_FIRMWARE_BASE_URL).href)));
  assert.equal(urls.some(url => url.includes('untrusted.invalid')), false);
});

test('rejects changed bytes, non-stable tags, and oversized release assets', async () => {
  const bytes = validImage();
  const checksum = await sha256(bytes);
  const tagStub = async url => url === LATEST_FIRMWARE_RELEASE_API ? response({ ...releaseFixture(bytes, checksum + '  firmware.bin\n'), tag_name: 'v2.0.0-rc1' }) : assert.fail('download must not start');
  await assert.rejects(fetchLatestFirmware(tagStub), /稳定/);

  const mismatchStub = async url => {
    if (url === LATEST_FIRMWARE_RELEASE_API) return response(releaseFixture(bytes, checksum + '  firmware.bin\n'));
    if (url.endsWith('/firmware.bin')) return { ok: true, async arrayBuffer() { const changed = bytes.slice(); changed[33] ^= 1; return changed.buffer; } };
    if (url.endsWith('/SHA256SUMS')) return { ok: true, async text() { return checksum + '  firmware.bin\n'; } };
    throw new Error('unexpected URL');
  };
  await assert.rejects(fetchLatestFirmware(mismatchStub), /SHA256SUMS 不匹配/);

  const oversizedStub = async url => url === LATEST_FIRMWARE_RELEASE_API ? response({
    ...releaseFixture(bytes, checksum + '  firmware.bin\n'),
    assets: releaseFixture(bytes, checksum + '  firmware.bin\n').assets.map(asset => asset.name === 'firmware.bin' ? { ...asset, size: APP_FLASH_MAX_BYTES + 1 } : asset)
  }) : assert.fail('oversized image must be rejected before download');
  await assert.rejects(fetchLatestFirmware(oversizedStub), /APP 分区/);
});

test('reports latest-release HTTP and network/CORS failures', async () => {
  await assert.rejects(fetchLatestFirmware(async () => { throw new TypeError('cross-origin request blocked'); }), /网络或 CORS/);
  await assert.rejects(fetchLatestFirmware(async () => ({ ok: false, status: 404 })), /HTTP 404/);
});
