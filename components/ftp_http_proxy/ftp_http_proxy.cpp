#include "ftp_http_proxy.h"
#include "ftp_web.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");

  // Configuration du watchdog timer
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = 30000,  // 30 secondes
      .idle_core_mask = (1 << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&twdt_config);
  esp_task_wdt_add(NULL);  // Ajouter la tâche actuelle

  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Réinitialiser le watchdog régulièrement
  esp_task_wdt_reset();
}

int FTPHTTPProxy::recv_with_timeout(int sock, void *buffer, size_t size, int timeout_ms) {
  fd_set readfds;
  struct timeval tv;
  int ret;

  FD_ZERO(&readfds);
  FD_SET(sock, &readfds);

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  ret = select(sock + 1, &readfds, NULL, NULL, &tv);
  if (ret > 0) {
    return recv(sock, (char *)buffer, size, 0);
  }
  return ret;  // 0 for timeout, -1 for error
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "Échec de la résolution DNS");
    return false;
  }

  sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Échec de création du socket : %d", errno);
    return false;
  }

  // Configurer le socket en mode non bloquant
  int flags = fcntl(sock_, F_GETFL, 0);
  fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

  // Configuration du socket pour être plus robuste
  int flag = 1;
  setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  int rcvbuf = 16384;
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (::connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion FTP : %d", errno);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int bytes_received = recv_with_timeout(sock_, buffer, sizeof(buffer) - 1, 5000);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Message de bienvenue FTP non reçu");
    ::close(sock_);
    sock_ = -1;
    return false;
  }
  buffer[bytes_received] = '\0';

  // Authentification
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv_with_timeout(sock_, buffer, sizeof(buffer) - 1, 5000);
  buffer[bytes_received] = '\0';

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv_with_timeout(sock_, buffer, sizeof(buffer) - 1, 5000);
  buffer[bytes_received] = '\0';

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv_with_timeout(sock_, buffer, sizeof(buffer) - 1, 5000);
  buffer[bytes_received] = '\0';

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2];
  char buffer[8192];
  int bytes_received;
  int flag = 1;
  int rcvbuf = 16384;

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto error;
  }

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv_with_timeout(sock_, buffer, sizeof(buffer) - 1, 5000);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif");
    goto error;
  }
  buffer[bytes_received] = '\0';
  ESP_LOGD(TAG, "Réponse PASV: %s", buffer);

  pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect");
    goto error;
  }

  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = port[0] * 256 + port[1];
  ESP_LOGD(TAG, "Port de données: %d", data_port);

  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données");
    goto error;
  }

  setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données");
    goto error;
  }

  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv_with_timeout(sock_, buffer, sizeof(buffer) - 1, 5000);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto error;
  }
  buffer[bytes_received] = '\0';

  unsigned long transfer_start_time = esp_timer_get_time() / 1000;
  size_t bytes_transferred = 0;
  size_t last_progress_time = transfer_start_time;

  while (true) {
    bytes_received = recv_with_timeout(data_sock, buffer, sizeof(buffer), 5000);
    if (bytes_received <= 0) {
      if (bytes_received < 0) {
        ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
      }
      break;
    }

    esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
      goto error;
    }

    bytes_transferred += bytes_received;
    unsigned long current_time = esp_timer_get_time() / 1000;

    // Log progress every 5 seconds
    if (current_time - last_progress_time > 5000) {
      ESP_LOGI(TAG, "Transfert en cours: %zu octets transférés", bytes_transferred);
      last_progress_time = current_time;
    }

    // Check for stalled transfer (no progress for 30 seconds)
    if (current_time - last_progress_time > 30000) {
      ESP_LOGE(TAG, "Transfert bloqué, abandon");
      goto error;
    }

    // Yield to the system
    if (bytes_received >= 4096) {
      vTaskDelay(pdMS_TO_TICKS(5));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Reset watchdog timer explicitly
    esp_task_wdt_reset();
  }

  ::close(data_sock);
  data_sock = -1;
  bytes_received = recv_with_timeout(sock_, buffer, sizeof(buffer) - 1, 5000);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
    buffer[bytes_received] = '\0';
    ESP_LOGD(TAG, "Transfert terminé: %s", buffer);
  }

  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;
  httpd_resp_send_chunk(req, NULL, 0);
  return success;

error:
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  return false;
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;

  // Augmenter les timeouts pour les gros fichiers
  config.recv_wait_timeout = 60;
  config.send_wait_timeout = 60;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 20;
  config.stack_size = 16384;
  config.task_priority = tskIDLE_PRIORITY + 5;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP");
    return;
  }

  httpd_uri_t uri_proxy = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = static_http_req_handler,
      .user_ctx = this};
  httpd_register_uri_handler(server_, &uri_proxy);

  httpd_uri_t uri_list = {
      .uri = "/list",
      .method = HTTP_GET,
      .handler = static_list_files_handler,
      .user_ctx = this};
  httpd_register_uri_handler(server_, &uri_list);

  httpd_uri_t uri_delete = {
      .uri = "/delete/*",
      .method = HTTP_DELETE,
      .handler = static_delete_file_handler,
      .user_ctx = this};
  httpd_register_uri_handler(server_, &uri_delete);

  httpd_uri_t uri_upload = {
      .uri = "/upload",
      .method = HTTP_POST,
      .handler = static_upload_file_handler,
      .user_ctx = this};
  httpd_register_uri_handler(server_, &uri_upload);

  ESP_LOGI(TAG, "Serveur HTTP démarré sur le port %d", local_port_);
}

} // namespace ftp_http_proxy
} // namespace esphome
