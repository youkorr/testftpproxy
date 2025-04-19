#pragma once

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/sensor/sensor.h"
#include <vector>

namespace esphome {
namespace sd_mmc_card {

class SdMmc : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void write_file(const char *path, const uint8_t *buffer, size_t len);
  void append_file(const char *path, const uint8_t *buffer, size_t len);
  void read_file(const char *path, std::string &content);
  void read_file_stream(const char *path, size_t offset, size_t chunk_size, std::function<void(const uint8_t*, size_t)> callback);

#ifdef USE_SENSOR
  void add_file_size_sensor(sensor::Sensor *, std::string const &path);
#endif

  void set_clk_pin(uint8_t);
  void set_cmd_pin(uint8_t);
  void set_data0_pin(uint8_t);
  void set_data1_pin(uint8_t);
  void set_data2_pin(uint8_t);
  void set_data3_pin(uint8_t);
  void set_mode_1bit(bool);
  void set_power_ctrl_pin(GPIOPin *);

  void write_file_with_wdt(const char *path, const uint8_t *buffer, size_t len, const char *mode);  // 🔧 Ajout de la déclaration

 protected:
  uint8_t clk_pin_{};
  uint8_t cmd_pin_{};
  uint8_t data0_pin_{};
  uint8_t data1_pin_{};
  uint8_t data2_pin_{};
  uint8_t data3_pin_{};
  bool mode_1bit_{true};
  GPIOPin *power_ctrl_pin_{nullptr};
};

}  // namespace sd_mmc_card
}  // namespace esphome








