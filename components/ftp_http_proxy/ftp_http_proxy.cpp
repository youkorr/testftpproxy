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

  struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  
  // Configuration du watchdog avec un délai plus long
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 10000,  // Augmenter à 10 secondes (défaut est 5s)
    .idle_core_mask = 0,  // Sans surveillance des cœurs inactifs
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Impossible de configurer le watchdog: %d", err);
  } else {
    ESP_LOGI(TAG, "Watchdog configuré avec timeout de 10s");
  }
  
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
  size_t total_bytes_transferred = 0;
  size_t bytes_since_reset = 0;
  
  // Obtenir le handle de la tâche actuelle pour le watchdog
  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  bool wdt_initialized = false;
  
  // Essayer d'ajouter la tâche au WDT si elle n'y est pas déjà
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

  // Détecter si c'est un fichier média
  std::string extension = "";
  size_t dot_pos = remote_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = remote_path.substr(dot_pos);
  }

  bool is_media_file = (extension == ".mp3" || extension == ".mp4" || 
                        extension == ".wav" || extension == ".ogg");

  // Ajuster la taille du buffer pour optimiser les performances
  // Pour les fichiers média, utiliser un buffer plus petit pour des réponses plus fréquentes
  int buffer_size = is_media_file ? 4096 : 8192;
  
  // Allouer le buffer en SPIRAM
  char* buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le buffer");
    if (wdt_initialized) esp_task_wdt_delete(current_task);
    return false;
  }

  // Réinitialiser le watchdog avant des opérations potentiellement longues
  if (wdt_initialized) esp_task_wdt_reset();

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto error;
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

  // Réinitialiser le watchdog avant des opérations de communication
  if (wdt_initialized) esp_task_wdt_reset();

  // Mode passif
  send(sock_, "PASV\r\n", 6, 0);
  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
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

  // Réinitialiser le watchdog avant la création du socket
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

  // Réinitialiser le watchdog avant la connexion
  if (wdt_initialized) esp_task_wdt_reset();

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données");
    goto error;
  }

  snprintf(buffer, buffer_size, "RETR %s\r\n", remote_path.c_str());
  send(sock_, buffer, strlen(buffer), 0);

  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto error;
  }
  buffer[bytes_received] = '\0';

  // Pour les fichiers média, envoyer en plus petits chunks avec plus de yields
  while (true) {
    // Réinitialiser le watchdog plus fréquemment pour les fichiers volumineux
    // Basé sur le volume de données transférées pour s'adapter à différentes vitesses
    bytes_received = recv(data_sock, buffer, buffer_size, 0);
    if (bytes_received <= 0) {
      if (bytes_received < 0) {
        ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
      }
      break;
    }
    
    // Mise à jour des compteurs
    total_bytes_transferred += bytes_received;
    bytes_since_reset += bytes_received;
    
    // Pour les fichiers média, réinitialiser le watchdog tous les ~100Ko
    if (is_media_file && bytes_since_reset >= 102400 && wdt_initialized) {
      esp_task_wdt_reset();
      bytes_since_reset = 0;
      ESP_LOGD(TAG, "WDT reset après ~100 Ko, total transféré: %zu Ko", total_bytes_transferred / 1024);
    }
    
    esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
      goto error;
    }
    
    // Comptez les chunks pour les fichiers média pour surveiller la progression
    chunk_count++;
    if (is_media_file && (chunk_count % 100 == 0)) {
      ESP_LOGD(TAG, "Streaming média: %d chunks envoyés, %zu Ko", chunk_count, total_bytes_transferred / 1024);
    }
    
    // Yield plus souvent pour les fichiers média, mais avec un délai plus court
    if (is_media_file) {
      vTaskDelay(pdMS_TO_TICKS(1));  // Délai minimal
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  // Réinitialiser le watchdog après la boucle principale
  if (wdt_initialized) esp_task_wdt_reset();

  ::close(data_sock);
  data_sock = -1;

  bytes_received = recv(sock_, buffer, buffer_size - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
    buffer[bytes_received] = '\0';
    ESP_LOGD(TAG, "Transfert terminé: %s", buffer);
  }

  send(sock_, "QUIT\r\n", 6, 0);
  ::close(sock_);
  sock_ = -1;

  // Libérer le buffer SPIRAM
  heap_caps_free(buffer);
  
  httpd_resp_send_chunk(req, NULL, 0);
  
  // Statistiques finales
  ESP_LOGI(TAG, "Fichier transféré avec succès: %zu Ko, %d chunks", total_bytes_transferred / 1024, chunk_count);
  
  // Retirer la tâche du watchdog à la fin
  if (wdt_initialized) {
    esp_task_wdt_delete(current_task);
  }
  
  return success;

error:
  if (buffer) heap_caps_free(buffer);
  if (data_sock != -1) ::close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    ::close(sock_);
    sock_ = -1;
  }
  
  // Retirer la tâche du watchdog en cas d'erreur
  if (wdt_initialized) {
    esp_task_wdt_delete(current_task);
  }
  
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



