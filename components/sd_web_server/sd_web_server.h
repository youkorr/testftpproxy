#pragma once
#include "esphome.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include "../sd_mmc_card/sd_mmc_card.h"

namespace esphome {
namespace sd_web_server {

class SDWebServer : public Component {
 public:
  void set_port(uint16_t port) { port_ = port; }
  void set_sd_directory(const std::string &path) { sd_dir_ = path; }
  void setup() override;

 private:
  uint16_t port_{8080};
  std::string sd_dir_{"/sdcard"};
  TaskHandle_t server_task_{nullptr};

  static void server_task(void *pv);
  static void handle_client(int client_sock, const std::string &sd_dir);
  static std::string build_http_response(const std::string &status, const std::string &content_type, const std::string &body);
  static std::string get_mime_type(const std::string &filename);
};

}  // namespace sd_web_server
}  // namespace esphome


