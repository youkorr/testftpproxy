#pragma once

#include "esphome.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include <dirent.h>
#include <string>

namespace esphome {
namespace sd_web_server {

class SDWebServer : public Component {
 public:
  // Setter du port
  void set_port(uint16_t port) { this->port_ = port; }

  // Setter du répertoire SD (ex: "/sdcard")
  void set_sd_directory(const std::string &dir) { this->sd_dir_ = dir; }

  void setup() override;
  void loop() override {}

 protected:
  uint16_t port_{8080};  // Port par défaut
  std::string sd_dir_{"/sdcard"};  // Répertoire SD par défaut

  httpd_handle_t server_ = nullptr;

  // Méthodes de traitement des requêtes HTTP
  static esp_err_t request_handler(httpd_req_t *req);
  static void send_directory_listing(httpd_req_t *req, const std::string &path);
  static void send_file(httpd_req_t *req, const std::string &path);
  static const char* get_mime_type(const char *filename);
};

}  // namespace sd_web_server
}  // namespace esphome

