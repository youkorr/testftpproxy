#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");
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
  
  // Déterminer si c'est un fichier média
  std::string extension = "";
  size_t dot_pos = remote_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = remote_path.substr(dot_pos);
  }
  
  bool is_media_file = (extension == ".mp3" || extension == ".mp4" || 
                        extension == ".wav" || extension == ".ogg");

  // Réduire encore plus la taille du buffer pour les fichiers média
  int buffer_size = is_media_file ? 1024 : 4096;
  
  // Allouer le buffer en SPIRAM
  char* buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le buffer");
    return false;
  }

  // Ajouter ce handler au watchdog
  esp_task_wdt_reset();  // Réinitialiser le watchdog global
  
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    heap_caps_free(buffer);
    return false;
  }

  // Configuration spéciale pour les fichiers média
  if (is_media_file) {
    // Configuration correcte du type MIME
    if (extension == ".mp3") {
      httpd_resp_set_type(req, "audio/mpeg");
    } else if (extension == ".wav") {
      httpd_resp_set_type(req, "audio/wav");
    } else if (extension == ".ogg") {
      httpd_resp_set_type(req, "audio/ogg");
    } else if (extension == ".mp4") {
      httpd_resp_set_type(req, "video/mp4");
    }
    // Permet la mise en mémoire tampon côté client
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  }

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif");
    goto cleanup;
  }
  buffer[bytes_received] = '\0';

  pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect");
    goto cleanup;
  }
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = port[0] * 256 + port[1];

  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données");
    goto cleanup;
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
    goto cleanup;
  }

  // Avant de demander le fichier, réinitialisez le watchdog
  esp_task_wdt_reset();
  
  snprintf(buffer, buffer_size, "RETR %s\r\n", remote_path.c_str());
  send(sock_, buffer, strlen(buffer), 0);

  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto cleanup;
  }
  buffer[bytes_received] = '\0';

  // *** APPROCHE SPÉCIALE POUR LES FICHIERS MÉDIA ***
  if (is_media_file) {
    // Pour permettre au loopTask de s'exécuter, on utilise une approche avec yielding intensif
    httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked"); // Utiliser l'encodage par morceaux
    
    // Initialiser timers pour céder régulièrement le contrôle
    uint32_t last_wdt_reset = xTaskGetTickCount();
    uint32_t last_log_time = xTaskGetTickCount();
    const uint32_t YIELD_INTERVAL_MS = 25;         // Céder toutes les 25ms
    const uint32_t WDT_RESET_INTERVAL_MS = 1000;   // Réinitialiser le watchdog toutes les 1s
    const uint32_t LOG_INTERVAL_MS = 5000;         // Log toutes les 5s

    // Configuration du socket en mode non-bloquant pour éviter le blocage sur recv
    fcntl(data_sock, F_SETFL, O_NONBLOCK);
    
    // Boucle de streaming avec abandons réguliers du CPU
    bool transfer_active = true;
    while (transfer_active) {
      // Vérifier si on doit céder le contrôle
      uint32_t now = xTaskGetTickCount();
      
      // Céder le contrôle au système régulièrement
      if ((now - last_wdt_reset) >= pdMS_TO_TICKS(YIELD_INTERVAL_MS)) {
        vTaskDelay(pdMS_TO_TICKS(5));  // Céder au moins 5ms
      }
      
      // Réinitialiser le watchdog périodiquement
      if ((now - last_wdt_reset) >= pdMS_TO_TICKS(WDT_RESET_INTERVAL_MS)) {
        esp_task_wdt_reset();
        last_wdt_reset = now;
      }
      
      // Log périodique
      if ((now - last_log_time) >= pdMS_TO_TICKS(LOG_INTERVAL_MS)) {
        ESP_LOGI(TAG, "Streaming média: %d chunks envoyés (environ %.1f kB)", 
                chunk_count, (float)chunk_count * buffer_size / 1024);
        last_log_time = now;
      }
      
      // Essayer de recevoir des données (non-bloquant)
      bytes_received = recv(data_sock, buffer, buffer_size - 16, MSG_DONTWAIT);  // Laisser une marge
      
      if (bytes_received > 0) {
        // Données reçues, les envoyer au client
        esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
          break;
        }
        chunk_count++;
      } 
      else if (bytes_received == 0) {
        // Fin de fichier
        transfer_active = false;
      }
      else {
        // -1 avec errno = EAGAIN ou EWOULDBLOCK signifie qu'il n'y a pas de données disponibles maintenant
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGE(TAG, "Erreur socket: %d", errno);
          transfer_active = false;
        }
        // Attendre un peu avant de réessayer pour éviter une utilisation CPU intensive
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    
    // Envoyer un chunk vide pour terminer l'encodage chunked
    httpd_resp_send_chunk(req, NULL, 0);
  } 
  else {
    // Pour les fichiers non-média, utiliser l'approche standard mais avec des réinitialisations watchdog
    while (true) {
      // Réinitialiser le watchdog périodiquement
      if (chunk_count % 10 == 0) {
        esp_task_wdt_reset();
      }
      
      bytes_received = recv(data_sock, buffer, buffer_size, 0);
      if (bytes_received <= 0) {
        if (bytes_received < 0) {
          ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
        }
        break;
      }
      
      esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
        goto cleanup;
      }
      
      chunk_count++;
      vTaskDelay(pdMS_TO_TICKS(1)); // Petit yield même pour les fichiers non-média
    }
    
    httpd_resp_send_chunk(req, NULL, 0);
  }

  // Récupérer le message de fin de transfert
  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
    buffer[bytes_received] = '\0';
    ESP_LOGI(TAG, "Transfert terminé: %s", buffer);
  }

cleanup:
  ESP_LOGI(TAG, "Nettoyage des ressources téléchargement");
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  if (buffer) heap_caps_free(buffer);
  
  ESP_LOGI(TAG, "Fin du téléchargement, %s", success ? "succès" : "échec");
  return success;
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
  config.recv_wait_timeout = 20;
  config.send_wait_timeout = 20;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 20;
  config.stack_size = 12288;

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



