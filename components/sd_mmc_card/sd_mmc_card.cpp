#include "sd_mmc_card.h"
#include "esphome/core/log.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_task_wdt.h"

namespace esphome {
namespace sd_mmc_card {

static const char *const TAG = "sd_mmc";

void SdMmc::setup() {
  ESP_LOGCONFIG(TAG, "Setting up SD card...");

  if (this->power_ctrl_pin_ != nullptr) {
    this->power_ctrl_pin_->setup();
    this->power_ctrl_pin_->digital_write(true);
    delay(100);
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  slot_config.clk = this->clk_pin_;
  slot_config.cmd = this->cmd_pin_;
  slot_config.d0 = this->data0_pin_;

  if (!this->mode_1bit_) {
    slot_config.width = 4;
    slot_config.d1 = this->data1_pin_;
    slot_config.d2 = this->data2_pin_;
    slot_config.d3 = this->data3_pin_;
  } else {
    slot_config.width = 1;
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
  };

  sdmmc_card_t *card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "SD card mounted at /sd");
}

void SdMmc::dump_config() {
  ESP_LOGCONFIG(TAG, "SDMMC Card:");
  LOG_PIN("  CLK Pin: ", this->clk_pin_);
  LOG_PIN("  CMD Pin: ", this->cmd_pin_);
  LOG_PIN("  D0 Pin: ", this->data0_pin_);
  if (!this->mode_1bit_) {
    LOG_PIN("  D1 Pin: ", this->data1_pin_);
    LOG_PIN("  D2 Pin: ", this->data2_pin_);
    LOG_PIN("  D3 Pin: ", this->data3_pin_);
  }
  ESP_LOGCONFIG(TAG, "  Bus width: %s", this->mode_1bit_ ? "1-bit" : "4-bit");
}

float SdMmc::get_setup_priority() const { return setup_priority::HARDWARE; }

void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file_with_wdt(path, buffer, len, "w");
}

void SdMmc::append_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file_with_wdt(path, buffer, len, "a");
}

void SdMmc::write_file_with_wdt(const char *path, const uint8_t *buffer, size_t len, const char *mode) {
  FILE *file = fopen(path, mode);
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file %s for writing", path);
    return;
  }

  const size_t chunk_size = 512;
  size_t written = 0;

  while (written < len) {
    size_t to_write = std::min(chunk_size, len - written);
    size_t result = fwrite(buffer + written, 1, to_write, file);

    if (result != to_write) {
      ESP_LOGE(TAG, "Failed to write to file %s", path);
      fclose(file);
      return;
    }

    written += result;
    esp_task_wdt_reset();  // Feed the watchdog
  }

  fflush(file);
  fclose(file);
  ESP_LOGD(TAG, "File written: %s (%u bytes)", path, (unsigned int) len);
}

void SdMmc::read_file(const char *path, std::string &content) {
  FILE *file = fopen(path, "r");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file %s for reading", path);
    return;
  }

  char buffer[512];
  content.clear();

  while (true) {
    size_t len = fread(buffer, 1, sizeof(buffer), file);
    if (len == 0)
      break;
    content.append(buffer, len);
  }

  fclose(file);
}

void SdMmc::read_file_stream(const char *path, size_t offset, size_t chunk_size,
                              std::function<void(const uint8_t *, size_t)> callback) {
  FILE *file = fopen(path, "r");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file %s for streaming", path);
    return;
  }

  if (offset > 0)
    fseek(file, offset, SEEK_SET);

  std::vector<uint8_t> buffer(chunk_size);

  while (true) {
    size_t len = fread(buffer.data(), 1, chunk_size, file);
    if (len == 0)
      break;
    callback(buffer.data(), len);
    esp_task_wdt_reset();  // Feed the watchdog while streaming
  }

  fclose(file);
}

#ifdef USE_SENSOR
void SdMmc::add_file_size_sensor(sensor::Sensor *sensor, std::string const &path) {
  FILE *file = fopen(path.c_str(), "r");
  if (!file) {
    sensor->publish_state(0);
    return;
  }

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fclose(file);
  sensor->publish_state(static_cast<float>(size));
}
#endif

void SdMmc::set_clk_pin(uint8_t pin) { this->clk_pin_ = pin; }
void SdMmc::set_cmd_pin(uint8_t pin) { this->cmd_pin_ = pin; }
void SdMmc::set_data0_pin(uint8_t pin) { this->data0_pin_ = pin; }
void SdMmc::set_data1_pin(uint8_t pin) { this->data1_pin_ = pin; }
void SdMmc::set_data2_pin(uint8_t pin) { this->data2_pin_ = pin; }
void SdMmc::set_data3_pin(uint8_t pin) { this->data3_pin_ = pin; }
void SdMmc::set_mode_1bit(bool mode) { this->mode_1bit_ = mode; }
void SdMmc::set_power_ctrl_pin(GPIOPin *pin) { this->power_ctrl_pin_ = pin; }

}  // namespace sd_mmc_card
}  // namespace esphome


