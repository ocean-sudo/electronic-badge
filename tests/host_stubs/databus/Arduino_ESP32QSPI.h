#pragma once
#include <cstdint>
#include <cstddef>

constexpr int32_t GFX_NOT_DEFINED = -1;
constexpr int ESP32QSPI_SPI_MODE = 0;
constexpr int ESP32QSPI_FREQUENCY = 40000000;
constexpr int ESP32QSPI_SPI_HOST = 1;
constexpr int SPI_DEVICE_HALFDUPLEX = 1;
constexpr int SPI_TRANS_MODE_QIO = 1;
constexpr int SPI_TRANS_VARIABLE_CMD = 2;
constexpr int SPI_TRANS_VARIABLE_ADDR = 4;
constexpr int SPI_TRANS_VARIABLE_DUMMY = 8;
constexpr int portMAX_DELAY = -1;
constexpr int ESP_OK = 0;
constexpr int LOW = 0;
constexpr int HIGH = 1;
using esp_err_t = int;
using spi_device_handle_t = void *;

struct spi_device_interface_config_t {
  int command_bits = 0;
  int address_bits = 0;
  int mode = 0;
  int clock_speed_hz = 0;
  int spics_io_num = 0;
  int flags = 0;
  int queue_size = 0;
};
struct spi_transaction_t {
  uint32_t flags = 0;
  uint32_t cmd = 0;
  uint64_t addr = 0;
  const void *tx_buffer = nullptr;
  std::size_t length = 0;
};
struct spi_transaction_ext_t { spi_transaction_t base; };

inline esp_err_t spi_bus_add_device(int, const spi_device_interface_config_t *, spi_device_handle_t *out) {
  *out = reinterpret_cast<void *>(1);
  return ESP_OK;
}
inline esp_err_t spi_device_get_actual_freq(spi_device_handle_t, int *out) { *out = 40000; return ESP_OK; }
inline esp_err_t spi_device_acquire_bus(spi_device_handle_t, int) { return ESP_OK; }
inline esp_err_t spi_device_polling_start(spi_device_handle_t, spi_transaction_t *, int) { return ESP_OK; }
inline esp_err_t spi_device_polling_end(spi_device_handle_t, int) { return ESP_OK; }
inline void spi_device_release_bus(spi_device_handle_t) {}
inline void digitalWrite(int8_t, int) {}

class Arduino_ESP32QSPI {
 public:
  Arduino_ESP32QSPI(int8_t, int8_t, int8_t, int8_t, int8_t, int8_t, bool) {}
  virtual ~Arduino_ESP32QSPI() = default;
  virtual bool begin(int32_t = GFX_NOT_DEFINED, int8_t = GFX_NOT_DEFINED) { return true; }
  virtual void writePixels(uint16_t *, uint32_t) {}
  void beginWrite() {}
  void endWrite() {}
};
