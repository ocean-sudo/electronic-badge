import assert from 'node:assert/strict';
import test from 'node:test';
import {
  APP_FLASH_ADDRESS,
  APP_FLASH_END,
  APP_FLASH_MAX_BYTES,
  ESP32_S3_IMAGE_CHIP_ID,
  appOnlyFlashOptions,
  readFirmwareFile,
  validateFirmwareFileSize,
  validateFirmwareImage
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
