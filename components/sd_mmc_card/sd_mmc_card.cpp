#include "sd_mmc_card.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace sd_mmc {

static const char *const TAG = "sd_mmc";

void SdMmc::setup() {
  // Alimentation de la carte SD si contrôle disponible
  if (this->ctrl_power_ != GPIO_NUM_NC) {
    pinMode((gpio_num_t) this->ctrl_power_, OUTPUT);
    digitalWrite((gpio_num_t) this->ctrl_power_, true);
    esphome::delay(100);
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.clk = (gpio_num_t) this->clk_pin_;
  slot_config.cmd = (gpio_num_t) this->cmd_pin_;
  slot_config.d0  = (gpio_num_t) this->data0_pin_;

  if (this->data1_pin_ != GPIO_NUM_NC && this->data2_pin_ != GPIO_NUM_NC && this->data3_pin_ != GPIO_NUM_NC) {
    slot_config.width = 4;
    slot_config.d1 = (gpio_num_t) this->data1_pin_;
    slot_config.d2 = (gpio_num_t) this->data2_pin_;
    slot_config.d3 = (gpio_num_t) this->data3_pin_;
  } else {
    slot_config.width = 1;
  }

  esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &this->mount_config_, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD card (%s)", esp_err_to_name(err));
    this->status_set_warning();
    return;
  }

  this->is_mounted_ = true;
  this->status_clear_warning();
  ESP_LOGI(TAG, "SD card mounted.");
}

void SdMmc::dump_config() {
  ESP_LOGCONFIG(TAG, "SDMMC Card:");
  ESP_LOGCONFIG(TAG, "  CLK Pin: %d", this->clk_pin_);
  ESP_LOGCONFIG(TAG, "  CMD Pin: %d", this->cmd_pin_);
  ESP_LOGCONFIG(TAG, "  Data0 Pin: %d", this->data0_pin_);
  if (this->data1_pin_ != GPIO_NUM_NC)
    ESP_LOGCONFIG(TAG, "  Data1 Pin: %d", this->data1_pin_);
  if (this->data2_pin_ != GPIO_NUM_NC)
    ESP_LOGCONFIG(TAG, "  Data2 Pin: %d", this->data2_pin_);
  if (this->data3_pin_ != GPIO_NUM_NC)
    ESP_LOGCONFIG(TAG, "  Data3 Pin: %d", this->data3_pin_);
  if (this->ctrl_power_ != GPIO_NUM_NC)
    ESP_LOGCONFIG(TAG, "  Power Control Pin: %d", this->ctrl_power_);
  ESP_LOGCONFIG(TAG, "  Mounted: %s", YESNO(this->is_mounted_));
}

void SdMmc::loop() {
  // Optionnel : surveiller la carte
}

void SdMmc::end() {
  if (!this->is_mounted_)
    return;

  esp_vfs_fat_sdcard_unmount("/sdcard", this->card_);
  this->is_mounted_ = false;

  if (this->ctrl_power_ != GPIO_NUM_NC) {
    digitalWrite((gpio_num_t) this->ctrl_power_, false);
  }

  ESP_LOGI(TAG, "SD card unmounted.");
}

}  // namespace sd_mmc
}  // namespace esphome




