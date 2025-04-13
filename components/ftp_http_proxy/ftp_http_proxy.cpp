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

  // Réduire encore plus la taille du buffer pour les fichiers média
  int buffer_size = is_media_file ? 4096 : 16384;
  
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
    // Réinitialiser le watchdog avant chaque itération pour les fichiers média
    if (is_media_file && (chunk_count % 5 == 0) && wdt_initialized) {
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
      goto error;
    }
    
    // Comptez les chunks pour les fichiers média pour surveiller la progression
    chunk_count++;
    if (is_media_file && (chunk_count % 100 == 0)) {
      ESP_LOGD(TAG, "Streaming média: %d chunks envoyés", chunk_count);
    }
    
    // Yield plus souvent pour les fichiers média
    if (is_media_file) {
      // Yield plus souvent pour les fichiers média
      vTaskDelay(pdMS_TO_TICKS(10));  // Augmenté à 10ms
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

  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  ESP_LOGI(TAG, "Requête reçue: %s", requested_path.c_str());

  std::string extension = "";
  size_t dot_pos = requested_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = requested_path.substr(dot_pos);
  }

  std::string filename = requested_path;
  size_t slash_pos = requested_path.find_last_of('/');
  if (slash_pos != std::string::npos) {
    filename = requested_path.substr(slash_pos + 1);
  }

  if (extension == ".mp3" || extension == ".wav" || extension == ".ogg") {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
  } else if (extension == ".pdf") {
    httpd_resp_set_type(req, "application/pdf");
  } else if (extension == ".jpg" || extension == ".jpeg") {
    httpd_resp_set_type(req, "image/jpeg");
  } else if (extension == ".png") {
    httpd_resp_set_type(req, "image/png");
  } else {
    httpd_resp_set_type(req, "application/octet-stream");
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", header.c_str());
  }

  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

  for (const auto &configured_path : proxy->remote_paths_) {
    if (requested_path == configured_path) {
      if (proxy->download_file(configured_path, req)) {
        return ESP_OK;
      } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec du téléchargement");
        return ESP_FAIL;
      }
    }
  }

  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé");
  return ESP_FAIL;
}

esp_err_t FTPHTTPProxy::list_files_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  if (!proxy->connect_to_ftp()) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de connexion FTP");
    return ESP_FAIL;
  }

  // Allouer le JSON en SPIRAM
  char* json_buffer = (char*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
  if (!json_buffer) {
    ESP_LOGE(TAG, "Échec d'allocation SPIRAM pour le JSON");
    return ESP_FAIL;
  }

  // Simulez une liste de fichiers distants (remplacez par une vraie implémentation)
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
  
  // Libérer la mémoire
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

  // Allouer le buffer en SPIRAM
  char* buffer = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
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

  // Libérer le buffer
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
