#include "ftp_http_proxy.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

struct TransferContext {
  httpd_req_t *req;
  int data_sock;
  char *buffer;
  size_t buffer_size;
  bool is_media_file;
};


static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void file_transfer_task(void *param) {
  TransferContext *ctx = static_cast<TransferContext *>(param);
  char *buffer = ctx->buffer;
  size_t buffer_size = ctx->buffer_size;
  httpd_req_t *req = ctx->req;
  int data_sock = ctx->data_sock;

  TickType_t last_wdt_feed = xTaskGetTickCount();
  const TickType_t wdt_timeout = pdMS_TO_TICKS(10000);  // 10 secondes
  int total_sent = 0;

  while (true) {
    int bytes_read = recv(data_sock, buffer, buffer_size, 0);
    if (bytes_read <= 0) break;

    esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_read);
    if (err != ESP_OK) {
      ESP_LOGE("FTPHTTPProxy", "Erreur envoi HTTP: %s", esp_err_to_name(err));
      break;
    }

    total_sent += bytes_read;

    if (xTaskGetTickCount() - last_wdt_feed > wdt_timeout / 2) {
      esp_task_wdt_reset();
      last_wdt_feed = xTaskGetTickCount();
    }

    if (ctx->is_media_file) {
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      taskYIELD();  // Laisse respirer les autres tâches
    }
  }

  close(data_sock);
  httpd_resp_send_chunk(req, nullptr, 0);
  delete[] buffer;
  delete ctx;

  ESP_LOGI("FTPHTTPProxy", "Transfert terminé, %d octets envoyés.", total_sent);
  vTaskDelete(nullptr);
}


void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");

  struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  
  // Configuration du watchdog avec un délai plus long
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");

  // Aucune configuration du watchdog n'est effectuée ici
  
  this->setup_http_server();
}


void FTPHTTPProxy::loop() {}

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

  // Configuration du socket pour être plus robuste
  int flag = 1;
  setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  
  // Augmenter la taille du buffer de réception
  int rcvbuf = 32768;
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
  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
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
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  int data_sock = -1;
  bool success = false;
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2];
  int bytes_received;
  int flag = 1;
  int rcvbuf = 32768;
  int chunk_count = 0;
  size_t total_bytes_transferred = 0;
  size_t bytes_since_reset = 0;
  
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  bool wdt_initialized = false;

  if (esp_task_wdt_status(current_task) != ESP_OK) {
    if (esp_task_wdt_add(current_task) == ESP_OK) {
      wdt_initialized = true;
      ESP_LOGI(TAG, "Tâche ajoutée au watchdog");
    } else {
      ESP_LOGW(TAG, "Impossible d'ajouter la tâche au watchdog");
    }
  } else {
    wdt_initialized = true;
    ESP_LOGI(TAG, "Tâche déjà dans le watchdog");
  }

  std::string extension = "";
  size_t dot_pos = remote_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = remote_path.substr(dot_pos);
  }

  bool is_media_file = (extension == ".mp3" || extension == ".mp4" || 
                        extension == ".wav" || extension == ".ogg" || 
                        extension == ".avi" || extension == ".mov" || 
                        extension == ".flv" || extension == ".jpg" || 
                        extension == ".png" || extension == ".bmp" || 
                        extension == ".gif" || extension == ".pdf" || 
                        extension == ".txt");

  if (wdt_initialized) esp_task_wdt_reset();

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto error;
  }

  // Récupérer la taille du fichier pour décision de mode (stream ou buffer)
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "SIZE %s\r\n", remote_path.c_str());
  send(sock_, cmd, strlen(cmd), 0);
  char size_buf[128];
  int size_bytes = recv(sock_, size_buf, sizeof(size_buf) - 1, 0);
  size_buf[size_bytes] = '\0';

  size_t file_size = 0;
  if (size_bytes > 0 && strstr(size_buf, "213 ")) {
    file_size = atoi(size_buf + 4);
    ESP_LOGI(TAG, "Taille fichier détectée: %zu octets", file_size);
  }

  bool mp3_in_memory = (extension == ".mp3" && file_size > 0 && file_size < 8 * 1024 * 1024);

  if (mp3_in_memory) {
    httpd_resp_set_type(req, "audio/mpeg");
  } else if (is_media_file) {
    if (extension == ".mp3") httpd_resp_set_type(req, "audio/mpeg");
    else if (extension == ".wav") httpd_resp_set_type(req, "audio/wav");
    else if (extension == ".ogg") httpd_resp_set_type(req, "audio/ogg");
    else if (extension == ".mp4") httpd_resp_set_type(req, "video/mp4");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  }

  if (wdt_initialized) esp_task_wdt_reset();

  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, size_buf, sizeof(size_buf) - 1, 0);
  if (bytes_received <= 0 || !strstr(size_buf, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif");
    goto error;
  }
  size_buf[bytes_received] = '\0';
  pasv_start = strchr(size_buf, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect");
    goto error;
  }
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = port[0] * 256 + port[1];

  if (wdt_initialized) esp_task_wdt_reset();

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

  if (wdt_initialized) esp_task_wdt_reset();

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données");
    goto error;
  }

  snprintf(cmd, sizeof(cmd), "RETR %s\r\n", remote_path.c_str());
  send(sock_, cmd, strlen(cmd), 0);
  bytes_received = recv(sock_, size_buf, sizeof(size_buf) - 1, 0);
  if (bytes_received <= 0 || !strstr(size_buf, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto error;
  }

  int buffer_size = mp3_in_memory ? file_size + 1 : (is_media_file ? 4096 : 65535);
  char *buffer = (char *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM");
    goto error;
  }

  if (mp3_in_memory) {
    size_t offset = 0;
    while (offset < file_size) {
      int len = recv(data_sock, buffer + offset, file_size - offset, 0);
      if (len <= 0) break;
      offset += len;
      if (wdt_initialized) esp_task_wdt_reset();
    }
    httpd_resp_send(req, buffer, file_size);
    total_bytes_transferred = file_size;
  } else {
    while (true) {
      bytes_received = recv(data_sock, buffer, buffer_size, 0);
      if (bytes_received <= 0) break;

      total_bytes_transferred += bytes_received;
      bytes_since_reset += bytes_received;

      if (is_media_file && bytes_since_reset >= 102400 && wdt_initialized) {
        esp_task_wdt_reset();
        bytes_since_reset = 0;
      }

      esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec d'envoi: %d", err);
        goto error;
      }

      chunk_count++;
      if (is_media_file && chunk_count % 100 == 0) {
        ESP_LOGD(TAG, "Streaming média: %d chunks, %zu Ko", chunk_count, total_bytes_transferred / 1024);
      }

      vTaskDelay(pdMS_TO_TICKS(2));
    }

    httpd_resp_send_chunk(req, NULL, 0);
  }

  if (wdt_initialized) esp_task_wdt_reset();

  ::close(data_sock);
  data_sock = -1;

  bytes_received = recv(sock_, size_buf, sizeof(size_buf) - 1, 0);
  if (bytes_received > 0 && strstr(size_buf, "226 ")) {
    success = true;
    size_buf[bytes_received] = '\0';
    ESP_LOGD(TAG, "Transfert terminé: %s", size_buf);
  }

  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  heap_caps_free(buffer);
  ESP_LOGI(TAG, "Fichier transféré avec succès: %zu Ko, %d chunks", total_bytes_transferred / 1024, chunk_count);

  if (wdt_initialized) esp_task_wdt_delete(current_task);
  return success;

error:
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  if (wdt_initialized) esp_task_wdt_delete(current_task);
  return false;
}


esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string requested_path = req->uri;

  // Suppression du premier slash
  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  ESP_LOGI(TAG, "Requête reçue: %s", requested_path.c_str());

  // Obtenir l'extension du fichier pour déterminer le type MIME
  std::string extension = "";
  size_t dot_pos = requested_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = requested_path.substr(dot_pos);
    ESP_LOGD(TAG, "Extension détectée: %s", extension.c_str());
  }

  // Extraire le nom du fichier de requested_path pour l'en-tête Content-Disposition
  std::string filename = requested_path;
  size_t slash_pos = requested_path.find_last_of('/');
  if (slash_pos != std::string::npos) {
    filename = requested_path.substr(slash_pos + 1);
  }

  // Définir les types MIME et headers selon le type de fichier
  if (extension == ".mp3") {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement MP3");
  } else if (extension == ".wav") {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement WAV");
  } else if (extension == ".ogg") {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement OGG");
  } else if (extension == ".pdf") {
    httpd_resp_set_type(req, "application/pdf");
  } else if (extension == ".jpg" || extension == ".jpeg") {
    httpd_resp_set_type(req, "image/jpeg");
  } else if (extension == ".png") {
    httpd_resp_set_type(req, "image/png");
  } else {
    // Type par défaut pour les fichiers inconnus
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
    ESP_LOGD(TAG, "Configuré pour téléchargement générique");
  }

  // Pour traiter les gros fichiers, on ajoute des en-têtes supplémentaires
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  
  for (const auto &configured_path : proxy->remote_paths_) {
    if (requested_path == configured_path) {
      ESP_LOGI(TAG, "Téléchargement du fichier: %s", requested_path.c_str());
      if (proxy->download_file(configured_path, req)) {
        ESP_LOGI(TAG, "Téléchargement réussi");
        return ESP_OK;
      } else {
        ESP_LOGE(TAG, "Échec du téléchargement");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec du téléchargement");
        return ESP_FAIL;
      }
    }
  }

  ESP_LOGW(TAG, "Fichier non trouvé: %s", requested_path.c_str());
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  
  // Augmenter les limites pour gérer les grandes requêtes
  config.recv_wait_timeout = 240;      // Augmenté à 30 secondes
  config.send_wait_timeout = 240;      // Augmenté à 30 secondes
  config.max_uri_handlers = true;
  config.max_resp_headers = 32;
  config.stack_size = 16384;          // Augmentation de la taille de la pile

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP");
    return;
  }

  httpd_uri_t uri_proxy = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = http_req_handler,
    .user_ctx  = this
  };

  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "Serveur HTTP démarré sur le port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome



