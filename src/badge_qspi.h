#pragma once

#include "databus/Arduino_ESP32QSPI.h"

// Keep the vendor command/init path; pipeline larger RGB565 DMA tiles locally.
// The inherited bus is shared so its device lock can be handed to the pixel
// device. No vendor-private state or unsupported display clock is required.
class BadgeQSPI final : public Arduino_ESP32QSPI {
 public:
  BadgeQSPI(int8_t cs, int8_t clock, int8_t data0, int8_t data1, int8_t data2, int8_t data3)
      : Arduino_ESP32QSPI(cs, clock, data0, data1, data2, data3, true), cs_(cs) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED, int8_t mode = GFX_NOT_DEFINED) override {
    if (!Arduino_ESP32QSPI::begin(speed, mode)) return false;
    for (auto &buffer : buffers_) {
      buffer = static_cast<uint32_t *>(heap_caps_aligned_alloc(16, kTilePixels * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
      if (!buffer) return false;
    }
    spi_device_interface_config_t config = {};
    config.command_bits = 8;
    config.address_bits = 24;
    config.mode = mode == GFX_NOT_DEFINED ? ESP32QSPI_SPI_MODE : mode;
    config.clock_speed_hz = speed <= GFX_NOT_DEFINED ? ESP32QSPI_FREQUENCY : speed;
    config.spics_io_num = -1;
    config.flags = SPI_DEVICE_HALFDUPLEX;
    config.queue_size = 1;
    if (spi_bus_add_device(ESP32QSPI_SPI_HOST, &config, &pixels_) != ESP_OK) return false;
    return spi_device_get_actual_freq(pixels_, &clockKhz) == ESP_OK;
  }

  void writePixels(uint16_t *data, uint32_t count) override {
    packUs = 0;
    // Arduino_TFT already owns the inherited command device here.
    Arduino_ESP32QSPI::endWrite();
    pixelOk = spi_device_acquire_bus(pixels_, portMAX_DELAY) == ESP_OK;
    if (!pixelOk) {
      Arduino_ESP32QSPI::beginWrite();
      return;
    }
    uint32_t remaining = count;
    unsigned buffer = 0;
    uint32_t length = remaining < kTilePixels ? remaining : kTilePixels;
    pack(data, buffers_[buffer], length);
    data += length;
    remaining -= length;
    digitalWrite(cs_, LOW);
    bool first = true;
    while (length) {
      spi_transaction_ext_t transaction = {};
      transaction.base.flags = SPI_TRANS_MODE_QIO;
      if (first) {
        transaction.base.cmd = 0x32;
        transaction.base.addr = 0x003C00; // CO5300 RAMWRC, as in vendor writePixels.
        first = false;
      } else {
        transaction.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
      }
      transaction.base.tx_buffer = buffers_[buffer];
      transaction.base.length = length * 16;
      if (spi_device_polling_start(pixels_, &transaction.base, portMAX_DELAY) != ESP_OK) {
        pixelOk = false;
        break;
      }
      const uint32_t nextLength = remaining < kTilePixels ? remaining : kTilePixels;
      const unsigned nextBuffer = buffer ^ 1U;
      // Convert the next tile while DMA sends this one. Never modify an in-flight tile.
      if (nextLength) pack(data, buffers_[nextBuffer], nextLength);
      const esp_err_t result = spi_device_polling_end(pixels_, portMAX_DELAY);
      if (result != ESP_OK) {
        pixelOk = false;
        break;
      }
      data += nextLength;
      remaining -= nextLength;
      buffer = nextBuffer;
      length = nextLength;
    }
    digitalWrite(cs_, HIGH);
    spi_device_release_bus(pixels_);
    Arduino_ESP32QSPI::beginWrite();
  }

  bool pixelOk = true;
  int clockKhz = 0;
  uint32_t packUs = 0;

 private:
  static constexpr uint32_t kTilePixels = 8192; // <= vendor SPI bus max_transfer_sz (16392 bytes).
  void pack(const uint16_t *source, uint32_t *destination, uint32_t count) {
    const uint32_t started = micros();
    const uint32_t pairs = count / 2;
    if ((reinterpret_cast<uintptr_t>(source) & 3U) == 0) {
      typedef uint32_t PixelPair __attribute__((may_alias));
      const auto *words = reinterpret_cast<const PixelPair *>(source);
      for (uint32_t i = 0; i < pairs; ++i) {
        const uint32_t pair = words[i];
        destination[i] = ((pair & 0x00FF00FFU) << 8) | ((pair & 0xFF00FF00U) >> 8);
      }
    } else {
      for (uint32_t i = 0; i < pairs; ++i) {
        const uint32_t pair = source[i * 2] | (static_cast<uint32_t>(source[i * 2 + 1]) << 16);
        destination[i] = ((pair & 0x00FF00FFU) << 8) | ((pair & 0xFF00FF00U) >> 8);
      }
    }
    if (count & 1U) {
      const uint16_t pixel = source[count - 1];
      reinterpret_cast<uint16_t *>(destination)[count - 1] = static_cast<uint16_t>((pixel << 8) | (pixel >> 8));
    }
    packUs += micros() - started;
  }
  int8_t cs_;
  spi_device_handle_t pixels_ = nullptr;
  uint32_t *buffers_[2] = {};
};
