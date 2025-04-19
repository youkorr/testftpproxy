#include "sd_mmc_card.h"
#include "esphome/core/log.h"

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace esphome {
namespace sd_mmc {

static const char *const TAG = "sd_mmc_card";

void SdMmc::setup() {
  ESP_LOGCONFIG(TAG, "Setting up SD_MMC card...");

  if (this->ctrl_power_ != GPIO_NUM_NC) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << this->ctrl_power_,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(this->ctrl_power_, 1);
    ESP_LOGD(TAG, "Power pin %d set to HIGH", this->ctrl_power_);
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.clk = (gpio_num_t)this->clk_pin_;
  slot_config.cmd = (gpio_num_t)this->cmd_pin_;
  slot_config.d0 = (gpio_num_t)this->data0_pin_;
  slot_config.width = 1;

  if (this->data1_pin_ != GPIO_NUM_NC && this->data2_pin_ != GPIO_NUM_NC && this->data3_pin_ != GPIO_NUM_NC) {
    slot_config.d1 = (gpio_num_t)this->data1_pin_;
    slot_config.d2 = (gpio_num_t)this->data2_pin_;
    slot_config.d3 = (gpio_num_t)this->data3_pin_;
    slot_config.width = 4;
  }

  esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &this->mount_config_, &this->card_);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
    this->status_set_warning();
    return;
  }

  this->is_mounted_ = true;
  this->status_clear_warning();
  ESP_LOGI(TAG, "SD card mounted successfully.");
}

void SdMmc::dump_config() {
  ESP_LOGCONFIG(TAG, "SD_MMC Card:");
  ESP_LOGCONFIG(TAG, "  CLK Pin: %d", this->clk_pin_);
  ESP_LOGCONFIG(TAG, "  CMD Pin: %d", this->cmd_pin_);
  ESP_LOGCONFIG(TAG, "  Data0 Pin: %d", this->data0_pin_);
  if (this->data1_pin_ != GPIO_NUM_NC)
    ESP_LOGCONFIG(TAG, "  Data1 Pin: %d", this->data1_pin_);
  if (this->data2_pin_ != GPIO_NUM_NC)
    ESP_LOGCONFIG(TAG, "  Data2 Pin: %d", this->data2_pin_);
  if (this->data3_pin_ != GPIO_NUM_NC)
    ESP_LOGCONFIG(TAG, "  Data3 Pin: %d", this->data3_pin_);
  ESP_LOGCONFIG(TAG, "  Mount path: /sdcard");
  ESP_LOGCONFIG(TAG, "  Card mounted: %s", YESNO(this->is_mounted_));
}

}  // namespace sd_mmc
}  // namespace esphome





