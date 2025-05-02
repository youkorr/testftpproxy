#pragma once

#include "esphome.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include "../sd_mmc_card/sd_mmc_card.h"
#include "esp_http_server.h"

namespace esphome {
namespace sd_web_server {

class SDWebServer : public Component {
 public:
  // Setters pour le port et le répertoire SD
  void set_port(uint16_t port) { port_ = port; }
  void set_sd_directory(const std::string &path) { sd_dir_ = path; }

  // Méthode setup qui est appelée lors de l'initialisation
  void setup() override;

 private:
  // Membres privés
  uint16_t port_{8080};                   // Port sur lequel le serveur écoute
  std::string sd_dir_ {"/sdcard"};        // Répertoire de la carte SD
  TaskHandle_t server_task_{nullptr};     // Handle pour la tâche serveur

  // Fonctions statiques utilisées dans les tâches serveur
  static void server_task(void *pv);
  static void handle_client(int client_sock, const std::string &sd_dir);
  static std::string build_http_response(const std::string &status, const std::string &content_type, const std::string &body);
  static std::string get_mime_type(const std::string &filename);
  static void send_directory_listing(int client_sock, const std::string &path);
  static void send_file(int client_sock, const std::string &path);
  static esp_err_t request_handler(httpd_req_t *req);

  // Membres pour gérer le serveur HTTP
  httpd_handle_t server_; // Handle du serveur HTTP
};

}  // namespace sd_web_server
}  // namespace esphome



