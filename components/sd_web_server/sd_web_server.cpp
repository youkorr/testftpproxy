#include "sd_web_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"

namespace esphome {
namespace sd_web_server {

static const char *TAG = "sd_webdav";

void SDWebServer::setup() {
  xTaskCreate(&SDWebServer::server_task, "webdav_server", 8192, this, 5, &server_task_);
}

void SDWebServer::server_task(void *pv) {
  SDWebServer *self = static_cast<SDWebServer *>(pv);

  // Création du socket
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    ESP_LOGE(TAG, "Socket creation failed");
    vTaskDelete(nullptr);
    return;
  }

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(self->port_);
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  // Lier le socket à l'adresse IP et au port
  if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Socket bind failed");
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  // Ecouter les connexions entrantes
  if (listen(sock, 2) < 0) {
    ESP_LOGE(TAG, "Listen failed");
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "WebDAV server running on port %d", self->port_);

  while (true) {
    // Attendre une connexion entrante
    int client_sock = accept(sock, nullptr, nullptr);
    if (client_sock >= 0) {
      // Traiter la requête client
      handle_client(client_sock, self->sd_dir_);
      close(client_sock);
    }
  }
}


void SDWebServer::handle_client(int client_sock, const std::string &sd_dir) {
  char buffer[1024];
  int len = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0) return;
  buffer[len] = 0;

  std::string request(buffer);
  std::string method, path;
  size_t m_end = request.find(' ');
  size_t p_end = request.find(' ', m_end + 1);

  if (m_end == std::string::npos || p_end == std::string::npos) return;
  method = request.substr(0, m_end);
  path = request.substr(m_end + 1, p_end - m_end - 1);
  std::string fs_path = sd_dir + path;

  ESP_LOGI(TAG, "Method: %s, Path: %s", method.c_str(), path.c_str());

  struct stat st;
  std::string response;

  if (method == "GET") {
    if (stat(fs_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      FILE *f = fopen(fs_path.c_str(), "rb");
      if (f) {
        std::string header = build_http_response("200 OK", get_mime_type(fs_path), "");
        send(client_sock, header.c_str(), header.size(), 0);
        char filebuf[512];
        size_t r;
        while ((r = fread(filebuf, 1, sizeof(filebuf), f)) > 0) {
          send(client_sock, filebuf, r, 0);
        }
        fclose(f);
        return;
      }
    }
    response = build_http_response("404 Not Found", "text/plain", "File not found");
  }

  else if (method == "PUT") {
    FILE *f = fopen(fs_path.c_str(), "wb");
    if (!f) {
      response = build_http_response("500 Internal Server Error", "text/plain", "Cannot create file");
    } else {
      const char *body = strstr(buffer, "\r\n\r\n");
      if (body) {
        body += 4;
        fwrite(body, 1, len - (body - buffer), f);
        // Could also read more from recv if needed
      }
      fclose(f);
      response = build_http_response("201 Created", "text/plain", "File written");
    }
  }

  else if (method == "PROPFIND") {
    if (stat(fs_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      response = build_http_response("404 Not Found", "text/plain", "Not a directory");
    } else {
      std::string xml = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">";
      DIR *dir = opendir(fs_path.c_str());
      if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
          std::string name(entry->d_name);
          if (name == "." || name == "..") continue;
          xml += "<d:response><d:href>" + path + "/" + name + "</d:href><d:propstat><d:prop><d:resourcetype/>";
          xml += "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
        }
        closedir(dir);
      }
      xml += "</d:multistatus>";
      response = build_http_response("207 Multi-Status", "application/xml", xml);
    }
  }

  else if (method == "DELETE") {
    if (remove(fs_path.c_str()) == 0) {
      response = build_http_response("200 OK", "text/plain", "Deleted");
    } else {
      response = build_http_response("404 Not Found", "text/plain", "Delete failed");
    }
  }

  else {
    response = build_http_response("405 Method Not Allowed", "text/plain", "Unsupported method");
  }

  send(client_sock, response.c_str(), response.size(), 0);
}

std::string SDWebServer::build_http_response(const std::string &status, const std::string &content_type, const std::string &body) {
  char header[256];
  snprintf(header, sizeof(header),
           "HTTP/1.1 %s\r\n"
           "Content-Type: %s\r\n"
           "Content-Length: %d\r\n"
           "Connection: close\r\n\r\n",
           status.c_str(), content_type.c_str(), (int)body.size());
  return std::string(header) + body;
}

std::string SDWebServer::get_mime_type(const std::string &filename) {
  if (filename.ends_with(".html")) return "text/html";
  if (filename.ends_with(".jpg")) return "image/jpeg";
  if (filename.ends_with(".png")) return "image/png";
  if (filename.ends_with(".json")) return "application/json";
  if (filename.ends_with(".txt")) return "text/plain";
  return "application/octet-stream";
}

}  // namespace sd_web_server
}  // namespace esphome

