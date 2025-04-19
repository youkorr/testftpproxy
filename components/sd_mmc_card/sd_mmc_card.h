#pragma once

#include "esphome/core/component.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"

namespace esphome {
namespace sd_mmc {

class SdMmc : public Component {
 public:
  void setup() override;
  void dump_config() override;

  int clk_pin_{-1};
  int cmd_pin_{-1};
  int data0_pin_{-1};
  int data1_pin_{GPIO_NUM_NC};
  int data2_pin_{GPIO_NUM_NC};
  int data3_pin_{GPIO_NUM_NC};
  int ctrl_power_{GPIO_NUM_NC};

  bool is_mounted_{false};
  sdmmc_card_t *card_{nullptr};

  esp_vfs_fat_mount_config_t mount_config_{};

 protected:
};

}  // namespace sd_mmc
}  // namespace esphome









