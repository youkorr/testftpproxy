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

  // Vérifier que la SPIRAM est disponible et afficher la mémoire disponible
  size_t spiram_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  ESP_LOGI(TAG, "SPIRAM disponible: %d octets", spiram_size);

  // Initialize Task Watchdog Timer with new API
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 30000,  // 30 second timeout
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,  // Check all cores
    .trigger_panic = true  // Panic on timeout
  };
  esp_task_wdt_init(&twdt_config);
  esp_task_wdt_add(NULL); // Add current task
  
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  esp_task_wdt_reset();  
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

  // Configuration du socket pour être plus robuste
  int flag = 1;
  setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

  // Augmenter la taille du buffer de réception
  int rcvbuf = 32768;  // Augmenté car SPIRAM a plus d'espace
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

  // Allouer le buffer en SPIRAM
  char* buffer = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le buffer");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  int bytes_received = recv(sock_, buffer, 255, 0);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Message de bienvenue FTP non reçu");
    heap_caps_free(buffer);
    ::close(sock_);
    sock_ = -1;
    return false;
  }
  buffer[bytes_received] = '\0';

  // Authentification
  snprintf(buffer, 256, "USER %s\r\n", username_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, 255, 0);
  buffer[bytes_received] = '\0';

  snprintf(buffer, 256, "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  bytes_received = recv(sock_, buffer, 255, 0);
  buffer[bytes_received] = '\0';

  // Mode binaire
  send(sock_, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock_, buffer, 255, 0);
  buffer[bytes_received] = '\0';

  // Libérer le buffer
  heap_caps_free(buffer);
  
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

esp_err_t FTPHTTPProxy::list_files_handler(httpd_req_t *req) {
    auto *proxy = (FTPHTTPProxy *)req->user_ctx;
    if (!proxy->connect_to_ftp()) {
        ESP_LOGE(TAG, "Échec de connexion FTP");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de connexion FTP");
        return ESP_FAIL;
    }

    char *json_buffer = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!json_buffer) {
        ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le JSON");
        return ESP_FAIL;
    }

    std::vector<std::string> files = {"file1.txt", "image.jpg", "audio.mp3"};
    strcpy(json_buffer, "[");
    for (size_t i = 0; i < files.size(); ++i) {
        strcat(json_buffer, "{\"name\":\"");
        strcat(json_buffer, files[i].c_str());
        strcat(json_buffer, "\"}");
        if (i < files.size() - 1) strcat(json_buffer, ",");
    }
    strcat(json_buffer, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buffer, strlen(json_buffer));
    heap_caps_free(json_buffer);
    return ESP_OK;
}

esp_err_t FTPHTTPProxy::delete_file_handler(httpd_req_t *req) {
    auto *proxy = (FTPHTTPProxy *)req->user_ctx;
    std::string file_name = req->uri + strlen("/delete/");
    ESP_LOGI(TAG, "Suppression du fichier: %s", file_name.c_str());

    if (!proxy->connect_to_ftp()) {
        ESP_LOGE(TAG, "Échec de connexion FTP");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de connexion FTP");
        return ESP_FAIL;
    }

    char *buffer = (char *)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le buffer");
        return ESP_FAIL;
    }

    snprintf(buffer, 256, "DELE %s\r\n", file_name.c_str());
    send(proxy->sock_, buffer, strlen(buffer), 0);

    int bytes_received = recv(proxy->sock_, buffer, 255, 0);
    buffer[bytes_received] = '\0';

    if (strstr(buffer, "250 ")) {
        httpd_resp_send(req, "File deleted", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de suppression");
    }

    send(proxy->sock_, "QUIT\r\n", 6, 0);
    ::close(proxy->sock_);
    proxy->sock_ = -1;
    heap_caps_free(buffer);
    return ESP_OK;
}

esp_err_t FTPHTTPProxy::upload_file_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  if (!proxy->connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de connexion FTP");
    return ESP_FAIL;
  }

  // Allouer les buffers en SPIRAM
  char* boundary = (char*)heap_caps_malloc(100, MALLOC_CAP_SPIRAM);
  if (!boundary) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le boundary");
    return ESP_FAIL;
  }

  size_t boundary_len = httpd_req_get_hdr_value_len(req, "Content-Type");
  if (boundary_len <= 0) {
    heap_caps_free(boundary);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Content-Type");
    return ESP_FAIL;
  }

  // Allouer content_type en SPIRAM via std::string
  std::string content_type;
  content_type.resize(boundary_len);
  httpd_req_get_hdr_value_str(req, "Content-Type", &content_type[0], boundary_len);

  size_t pos = content_type.find("boundary=");
  if (pos == std::string::npos) {
    heap_caps_free(boundary);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Boundary not found");
    return ESP_FAIL;
  }

  std::string boundary_str = content_type.substr(pos + 9);
  std::string file_name;
  
  // Allouer le buffer de données en SPIRAM
  char* buffer = (char*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le buffer de données");
    heap_caps_free(boundary);
    return ESP_FAIL;
  }
  
  // Utiliser SPIRAM pour stocker les données du fichier
  void* file_data_ptr = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
  if (!file_data_ptr) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour les données du fichier");
    heap_caps_free(buffer);
    heap_caps_free(boundary);
    return ESP_FAIL;
  }
  
  char* file_data = (char*)file_data_ptr;
  int file_data_len = 0;
  
  int ret, remaining = req->content_len;

  while (remaining > 0) {
    ret = httpd_req_recv(req, buffer, std::min(remaining, 1024));
    if (ret <= 0) {
      heap_caps_free(file_data);
      heap_caps_free(buffer);
      heap_caps_free(boundary);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error receiving data");
      return ESP_FAIL;
    }

    // Copier les données dans le buffer SPIRAM
    memcpy(file_data + file_data_len, buffer, ret);
    file_data_len += ret;
    remaining -= ret;
  }

  // Analyser pour trouver le nom de fichier
  std::string file_data_str(file_data, file_data_len);
  pos = file_data_str.find("filename=\"");
  if (pos == std::string::npos) {
    heap_caps_free(file_data);
    heap_caps_free(buffer);
    heap_caps_free(boundary);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename not found");
    return ESP_FAIL;
  }

  size_t end_pos = file_data_str.find("\"", pos + 10);
  file_name = file_data_str.substr(pos + 10, end_pos - pos - 10);

  pos = file_data_str.find("\r\n\r\n");
  if (pos == std::string::npos) {
    heap_caps_free(file_data);
    heap_caps_free(buffer);
    heap_caps_free(boundary);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File content not found");
    return ESP_FAIL;
  }

  file_data_str = file_data_str.substr(pos + 4);
  end_pos = file_data_str.rfind("--" + boundary_str + "--");
  if (end_pos == std::string::npos) {
    heap_caps_free(file_data);
    heap_caps_free(buffer);
    heap_caps_free(boundary);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file content");
    return ESP_FAIL;
  }

  file_data_str = file_data_str.substr(0, end_pos);

  char buffer_cmd[256];
  snprintf(buffer_cmd, sizeof(buffer_cmd), "STOR %s\r\n", file_name.c_str());
  send(proxy->sock_, buffer_cmd, strlen(buffer_cmd), 0);

  int bytes_sent = send(proxy->sock_, file_data_str.c_str(), file_data_str.length(), 0);
  if (bytes_sent <= 0) {
    heap_caps_free(file_data);
    heap_caps_free(buffer);
    heap_caps_free(boundary);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error uploading file");
    return ESP_FAIL;
  }

  httpd_resp_send(req, "File uploaded", HTTPD_RESP_USE_STRLEN);

  send(proxy->sock_, "QUIT\r\n", 6, 0);
  ::close(proxy->sock_);
  proxy->sock_ = -1;

  // Libérer toutes les allocations SPIRAM
  heap_caps_free(file_data);
  heap_caps_free(buffer);
  heap_caps_free(boundary);

  return ESP_OK;
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;

  config.recv_wait_timeout = 20;
  config.send_wait_timeout = 20;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 20;
  
  // Augmenter la taille de la pile, en utilisant SPIRAM si disponible
  config.stack_size = 16384;  // Augmenter la taille
  
  // Définir une priorité de tâche plus élevée
  config.task_priority = tskIDLE_PRIORITY + 5;
  
  // Activer la purge LRU pour économiser de la mémoire
  config.lru_purge_enable = true;

  // Utiliser plus de SPIRAM avec plus de connexions simultanées
  config.max_open_sockets = 7;  // Valeur par défaut: 7

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP");
    return;
  }

  httpd_uri_t uri_proxy = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = static_http_req_handler,  // Utilisation du wrapper statique
    .user_ctx  = this
  };
  httpd_register_uri_handler(server_, &uri_proxy);

  httpd_uri_t uri_list = {
    .uri       = "/list",
    .method    = HTTP_GET,
    .handler   = static_list_files_handler,  // Utilisation du wrapper statique
    .user_ctx  = this
  };
  httpd_register_uri_handler(server_, &uri_list);

  httpd_uri_t uri_delete = {
    .uri       = "/delete/*",
    .method    = HTTP_DELETE,
    .handler   = static_delete_file_handler,  // Utilisation du wrapper statique
    .user_ctx  = this
  };
  httpd_register_uri_handler(server_, &uri_delete);

  httpd_uri_t uri_upload = {
    .uri       = "/upload",
    .method    = HTTP_POST,
    .handler   = static_upload_file_handler,  // Utilisation du wrapper statique
    .user_ctx  = this
  };
  httpd_register_uri_handler(server_, &uri_upload);

  ESP_LOGI(TAG, "Serveur HTTP démarré sur le port %d", local_port_);
}

// Ces méthodes statiques sont uniquement des wrappers pour les méthodes membres
esp_err_t FTPHTTPProxy::static_http_req_handler(httpd_req_t *req) {
  return ((FTPHTTPProxy *)req->user_ctx)->http_req_handler(req);
}

esp_err_t FTPHTTPProxy::static_list_files_handler(httpd_req_t *req) {
  return ((FTPHTTPProxy *)req->user_ctx)->list_files_handler(req);
}

esp_err_t FTPHTTPProxy::static_delete_file_handler(httpd_req_t *req) {
  return ((FTPHTTPProxy *)req->user_ctx)->delete_file_handler(req);
}

esp_err_t FTPHTTPProxy::static_upload_file_handler(httpd_req_t *req) {
  return ((FTPHTTPProxy *)req->user_ctx)->upload_file_handler(req);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
