#pragma once
#include "esphome.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include <dirent.h>

namespace esphome {
namespace sd_web_server {

class SDWebServer : public Component {
 public:
  void set_port(uint16_t port) { port_ = port; }
  void setup() override;
  void loop() override {}

 private:
  uint16_t port_;
  httpd_handle_t server_ = nullptr;
  
  static esp_err_t request_handler(httpd_req_t *req);
  static void send_directory_listing(httpd_req_t *req, const char *path);
  static void send_file(httpd_req_t *req, const char *path);
  static const char* get_mime_type(const char *filename);
};

} // namespace sd_web_server
} // namespace esphome
