#include "sd_web_server.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "SD_WEB";

namespace esphome {
namespace sd_web_server {

const char* SDWebServer::get_mime_type(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return "application/octet-stream";
  
  static const struct {
    const char *ext;
    const char *mime;
  } mime_types[] = {
    {".html", "text/html"}, {".js", "application/javascript"}, 
    {".css", "text/css"}, {".jpg", "image/jpeg"},
    {".png", "image/png"}, {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"}, {".txt", "text/plain"},
    {".json", "application/json"}
  };

  for (const auto &mt : mime_types) {
    if (strcasecmp(dot, mt.ext) == 0) return mt.mime;
  }
  return "application/octet-stream";
}

void SDWebServer::send_directory_listing(httpd_req_t *req, const char *path) {
  DIR *dir = opendir(path);
  if (!dir) {
    httpd_resp_send_404(req);
    return;
  }

  std::string html = R"(
<html><head><title>Index of )" + std::string(path) + R"(</title>
<style>
.grid { 
  display: grid; 
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 1rem;
  padding: 1rem;
}
.item {
  border: 1px solid #ddd;
  padding: 1rem;
  text-align: center;
}
img { max-width: 100%; height: auto; }
</style></head><body>
<h1>Index of )" + std::string(path) + R"(</h1><div class="grid">)";

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name == '.') continue;
    
    std::string full_path = std::string(path) + "/" + entry->d_name;
    struct stat st;
    stat(full_path.c_str(), &st);

    html += "<div class='item'>";
    html += "<a href='" + std::string(entry->d_name) + "'>";
    
    if (S_ISDIR(st.st_mode)) {
      html += "📁 <strong>" + std::string(entry->d_name) + "</strong>";
    } else {
      html += "📄 " + std::string(entry->d_name);
      if (strstr(entry->d_name, ".jpg") || strstr(entry->d_name, ".png")) {
        html += "<br><img src='" + std::string(entry->d_name) + "' loading='lazy'>";
      }
    }
    
    html += "</a><br><small>" + std::to_string(st.st_size / 1024) + " KB</small>";
    html += "</div>";
  }
  closedir(dir);
  
  html += "</div></body></html>";
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.size());
}

void SDWebServer::send_file(httpd_req_t *req, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    httpd_resp_send_404(req);
    return;
  }

  const size_t buffer_size = 4096;
  char *buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    fclose(file);
    httpd_resp_send_500(req);
    return;
  }

  httpd_resp_set_type(req, get_mime_type(path));
  
  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, buffer_size, file)) > 0) {
    if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) break;
  }

  heap_caps_free(buffer);
  fclose(file);
  httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t SDWebServer::request_handler(httpd_req_t *req) {
  char path[128];
  snprintf(path, sizeof(path), "/sd%s", req->uri);
  
  struct stat st;
  if (stat(path, &st) != 0) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  if (S_ISDIR(st.st_mode)) {
    send_directory_listing(req, path);
  } else {
    send_file(req, path);
  }
  
  return ESP_OK;
}

void SDWebServer::setup() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port_;
  config.ctrl_port = port_ + 1;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;
  config.lru_purge_enable = true;

  if (httpd_start(&server_, &config) == ESP_OK) {
    httpd_uri_t uri = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = request_handler,
      .user_ctx = this
    };
    httpd_register_uri_handler(server_, &uri);
    ESP_LOGI(TAG, "Serveur web SD démarré sur le port %d", port_);
  }
}

} // namespace sd_web_server
} // namespace esphome


