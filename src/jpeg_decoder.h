#pragma once

#include <FS.h>
#include <esp32s3/rom/tjpgd.h>
#include "jpeg_image.h"

// ROM software TJpgDec. Every File byte (including skipped APP/COM segments and
// unread entropy/tail) passes through both CRCs exactly once. Only the 19-byte
// prefix is reread; JPEG output goes straight to the inactive canonical source.
class BadgeJpegDecoder {
 public:
  static constexpr size_t kWorkspaceBytes = 4096;
  uint32_t decodeUs = 0;
  const char* decode(File& file, uint8_t* pixels, uint8_t* scratch, size_t scratchBytes,
                     const uint32_t* crcTable, uint32_t& storedCrc, uint32_t& transferCrc) {
    uint8_t prefix[BadgeJpeg::kPrefixBytes];
    if (!file || !pixels || !file.seek(0) || file.read(prefix, sizeof(prefix)) != sizeof(prefix)) return "storage_read";
    uint32_t expected;
    if (!BadgeJpeg::header(prefix, sizeof(prefix), file.size(), expected)) return "jpeg_metadata";
    if (!file.seek(0)) return "storage_read";
    Session session{&file, pixels, scratch, scratchBytes, BadgeJpeg::Integrity(crcTable)};
    JDEC decoder{};
    const uint32_t started = micros();
    const JRESULT prepared = jd_prepare(&decoder, input, workspace_, sizeof(workspace_), &session);
    if (prepared != JDR_OK) return "jpeg_unsupported_or_invalid";
    if (decoder.width != 466 || decoder.height != 466) return "jpeg_dimensions";
    const JRESULT decoded = jd_decomp(&decoder, output, 0);
    decodeUs = micros() - started;
    if (decoded != JDR_OK || !session.coverage.complete()) return "jpeg_decode";
    while (session.integrity.bytes < file.size()) {
      const size_t remaining = file.size() - session.integrity.bytes;
      const size_t wanted = remaining < scratchBytes ? remaining : scratchBytes;
      if (read(session, scratch, wanted) != wanted) return "storage_read";
    }
    const char* error = session.integrity.finish(file.size(), expected);
    if (error) return error;
    storedCrc = session.integrity.storedCrc();
    transferCrc = session.integrity.transferCrc();
    return nullptr;
  }
 private:
  struct Session {
    File* file;
    uint8_t* pixels;
    uint8_t* scratch;
    size_t scratchBytes;
    BadgeJpeg::Integrity integrity;
    BadgeJpeg::Coverage coverage;
  };
  alignas(4) uint8_t workspace_[kWorkspaceBytes];
  static size_t read(Session& session, uint8_t* data, size_t wanted) {
    const size_t count = session.file->read(data, wanted);
    session.integrity.append(data, count);
    return count;
  }
  static UINT input(JDEC* decoder, BYTE* data, UINT count) {
    auto& session = *static_cast<Session*>(decoder->device);
    if (data) return read(session, data, count);
    size_t consumed = 0;
    while (consumed < count) {
      const size_t remaining = count - consumed;
      const size_t wanted = remaining < session.scratchBytes ? remaining : session.scratchBytes;
      const size_t got = read(session, session.scratch, wanted);
      consumed += got;
      if (got != wanted) break;
    }
    return consumed;
  }
  static UINT output(JDEC* decoder, void* bitmap, JRECT* rect) {
    auto& session = *static_cast<Session*>(decoder->device);
    if (!session.coverage.accept(rect->left, rect->right, rect->top, rect->bottom)) return 0;
    BadgeJpeg::writePackedRect(session.pixels, static_cast<const uint8_t*>(bitmap),
        rect->left, rect->right, rect->top, rect->bottom);
    return 1;
  }
};
