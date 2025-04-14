#include "ftp_server.h"
#include "../sd_mmc_card/sd_mmc_card.h"
#include "esp_log.h"
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <ctime>
#include "esp_netif.h"
#include "esp_err.h"
#include <errno.h>

namespace esphome {
namespace ftp_server {

static const char *TAG = "ftp_server";

FTPServer::FTPServer() : 
  ftp_server_socket_(-1),
  passive_data_socket_(-1),
  passive_data_port_(-1),
  passive_mode_enabled_(false),
  rename_from_("") {}

std::string normalize_path(const std::string& base_path, const std::string& path) {
  std::string result;
  
  if (path.empty() || path == ".") {
    return base_path;
  }
  
  if (path[0] == '/') {
    if (path == "/sdcard") {
      return base_path;
    }
    std::string clean_path = path;
    if (base_path.back() == '/' && path[0] == '/') {
      clean_path = path.substr(1);
    }
    result = base_path + clean_path;
  } else {
    if (base_path.back() == '/') {
      result = base_path + path;
    } else {
      result = base_path + "/" + path;
    }
  }
  
  ESP_LOGD(TAG, "Normalized path: %s (from base: %s, request: %s)", 
           result.c_str(), base_path.c_str(), path.c_str());
  
  return result;
}

void FTPServer::setup() {
  ESP_LOGI(TAG, "Setting up FTP server...");

  if (root_path_.empty()) {
    root_path_ = "/";
  }
  
  if (root_path_.back() != '/') {
    root_path_ += '/';
  }

  DIR *dir = opendir(root_path_.c_str());
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Root directory %s does not exist or is not accessible (errno: %d)", 
             root_path_.c_str(), errno);
    if (mkdir(root_path_.c_str(), 0755) != 0) {
      ESP_LOGE(TAG, "Failed to create root directory %s (errno: %d)", 
               root_path_.c_str(), errno);
    } else {
      ESP_LOGI(TAG, "Created root directory %s", root_path_.c_str());
      dir = opendir(root_path_.c_str());
    }
  }
  
  if (dir != nullptr) {
    closedir(dir);
  } else {
    ESP_LOGE(TAG, "Root directory %s still not accessible after creation attempt", 
             root_path_.c_str());
  }

  ftp_server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ftp_server_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create FTP server socket (errno: %d)", errno);
    return;
  }

  int opt = 1;
  if (setsockopt(ftp_server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    ESP_LOGE(TAG, "Failed to set socket options (errno: %d)", errno);
    close(ftp_server_socket_);
    ftp_server_socket_ = -1;
    return;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_);
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(ftp_server_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind FTP server socket (errno: %d)", errno);
    close(ftp_server_socket_);
    ftp_server_socket_ = -1;
    return;
  }

  if (listen(ftp_server_socket_, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen on FTP server socket (errno: %d)", errno);
    close(ftp_server_socket_);
    ftp_server_socket_ = -1;
    return;
  }

  fcntl(ftp_server_socket_, F_SETFL, O_NONBLOCK);

  ESP_LOGI(TAG, "FTP server started on port %d", port_);
  ESP_LOGI(TAG, "Root directory: %s", root_path_.c_str());
  current_path_ = root_path_;
}

void FTPServer::loop() {
  handle_new_clients();
  for (size_t i = 0; i < client_sockets_.size(); i++) {
    handle_ftp_client(client_sockets_[i]);
  }
}

void FTPServer::dump_config() {
  ESP_LOGI(TAG, "FTP Server:");
  ESP_LOGI(TAG, "  Port: %d", port_);
  ESP_LOGI(TAG, "  Root Path: %s", root_path_.c_str());
  ESP_LOGI(TAG, "  Username: %s", username_.c_str());
  ESP_LOGI(TAG, "  Server status: %s", is_running() ? "Running" : "Not running");
}

void FTPServer::handle_new_clients() {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_socket = accept(ftp_server_socket_, (struct sockaddr *)&client_addr, &client_len);
  if (client_socket >= 0) {
    fcntl(client_socket, F_SETFL, O_NONBLOCK);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    ESP_LOGI(TAG, "New FTP client connected from %s:%d", client_ip, ntohs(client_addr.sin_port));
    client_sockets_.push_back(client_socket);
    client_states_.push_back(FTP_WAIT_LOGIN);
    client_usernames_.push_back("");
    client_current_paths_.push_back(root_path_);
    send_response(client_socket, 220, "Welcome to ESPHome FTP Server");
  }
}

void FTPServer::handle_ftp_client(int client_socket) {
  char buffer[512];
  int len = recv(client_socket, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
  if (len > 0) {
    buffer[len] = '\0';
    std::string command(buffer);
    process_command(client_socket, command);
  } else if (len == 0) {
    ESP_LOGI(TAG, "FTP client disconnected");
    close(client_socket);
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = it - client_sockets_.begin();
      client_sockets_.erase(it);
      client_states_.erase(client_states_.begin() + index);
      client_usernames_.erase(client_usernames_.begin() + index);
      client_current_paths_.erase(client_current_paths_.begin() + index);
    }
  } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
    ESP_LOGW(TAG, "Socket error: %d", errno);
  }
}

void FTPServer::process_command(int client_socket, const std::string& command) {
  ESP_LOGI(TAG, "FTP command: %s", command.c_str());
  std::string cmd_str = command;
  size_t pos = cmd_str.find_first_of("\r\n");
  if (pos != std::string::npos) {
    cmd_str = cmd_str.substr(0, pos);
  }

  auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
  if (it == client_sockets_.end()) {
    ESP_LOGE(TAG, "Client socket not found!");
    return;
  }

  size_t client_index = it - client_sockets_.begin();

  if (cmd_str.find("USER") == 0) {
    std::string username = cmd_str.substr(5);
    client_usernames_[client_index] = username;
    send_response(client_socket, 331, "Password required for " + username);
  } else if (cmd_str.find("PASS") == 0) {
    std::string password = cmd_str.substr(5);
    if (authenticate(client_usernames_[client_index], password)) {
      client_states_[client_index] = FTP_LOGGED_IN;
      send_response(client_socket, 230, "Login successful");
    } else {
      send_response(client_socket, 530, "Login incorrect");
    }
  } else if (client_states_[client_index] != FTP_LOGGED_IN) {
    send_response(client_socket, 530, "Not logged in");
  } else if (cmd_str.find("SYST") == 0) {
    send_response(client_socket, 215, "UNIX Type: L8");
  } else if (cmd_str.find("FEAT") == 0) {
    send_response(client_socket, 211, "Features:");
    send_response(client_socket, 211, " SIZE");
    send_response(client_socket, 211, " MDTM");
    send_response(client_socket, 211, "End");
  } else if (cmd_str.find("TYPE") == 0) {
    send_response(client_socket, 200, "Type set to " + cmd_str.substr(5));
  } else if (cmd_str.find("PWD") == 0) {
    std::string current_path = client_current_paths_[client_index];
    std::string relative_path = "/";
    if (current_path.length() > root_path_.length()) {
      relative_path = current_path.substr(root_path_.length() - 1);
    }
    send_response(client_socket, 257, "\"" + relative_path + "\" is current directory");
  } else if (cmd_str.find("CWD") == 0) {
    std::string path = cmd_str.substr(4);
    size_t first_non_space = path.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      path = path.substr(first_non_space);
    }
    
    if (path.empty()) {
      send_response(client_socket, 550, "Failed to change directory - path is empty");
    } else {
      std::string current_path = client_current_paths_[client_index];
      std::string full_path;
      
      if (path == "/") {
        full_path = root_path_;
      } else {
        full_path = normalize_path(current_path, path);
      }
      
      ESP_LOGI(TAG, "Attempting to change directory to: %s", full_path.c_str());
      
      DIR *dir = opendir(full_path.c_str());
      if (dir != nullptr) {
        closedir(dir);
        client_current_paths_[client_index] = full_path;
        send_response(client_socket, 250, "Directory successfully changed");
      } else {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno: %d)", full_path.c_str(), errno);
        send_response(client_socket, 550, "Failed to change directory");
      }
    }
  } else if (cmd_str.find("CDUP") == 0) {
    std::string current = client_current_paths_[client_index];
    
    if (current == root_path_ || current.length() <= root_path_.length()) {
      send_response(client_socket, 250, "Already at root directory");
      return;
    }
    
    size_t pos = current.find_last_of('/');
    if (pos != std::string::npos && current.length() > 1) {
      if (pos == current.length() - 1) {
        std::string temp = current.substr(0, pos);
        pos = temp.find_last_of('/');
      }
      
      if (pos != std::string::npos) {
        std::string parent_dir = current.substr(0, pos + 1);
        
        if (parent_dir.length() >= root_path_.length()) {
          client_current_paths_[client_index] = parent_dir;
          send_response(client_socket, 250, "Directory successfully changed");
        } else {
          client_current_paths_[client_index] = root_path_;
          send_response(client_socket, 250, "Directory changed to root");
        }
      } else {
        send_response(client_socket, 550, "Failed to change directory");
      }
    } else {
      send_response(client_socket, 550, "Failed to change directory");
    }
  } else if (cmd_str.find("PASV") == 0) {
    if (start_passive_mode(client_socket)) {
      passive_mode_enabled_ = true;
    } else {
      send_response(client_socket, 425, "Can't open passive connection");
    }
  } else if (cmd_str.find("LIST") == 0 || cmd_str.find("NLST") == 0) {
    std::string path_arg = "";
    std::string cmd_type = cmd_str.substr(0, 4);
    
    if (cmd_str.length() > 5) {
      path_arg = cmd_str.substr(5);
      size_t first_non_space = path_arg.find_first_not_of(" \t");
      if (first_non_space != std::string::npos) {
        path_arg = path_arg.substr(first_non_space);
      }
    }
    
    std::string list_path;
    if (path_arg.empty() || path_arg == ".") {
      list_path = client_current_paths_[client_index];
    } else {
      list_path = normalize_path(client_current_paths_[client_index], path_arg);
    }
    
    ESP_LOGI(TAG, "Listing directory: %s", list_path.c_str());
    send_response(client_socket, 150, "Opening ASCII mode data connection for file list");
    
    if (cmd_type == "LIST") {
      list_directory(client_socket, list_path);
    } else {
      list_names(client_socket, list_path);
    }
  } else if (cmd_str.find("STOR") == 0) {
    std::string filename = cmd_str.substr(5);
    size_t first_non_space = filename.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      filename = filename.substr(first_non_space);
    }
    
    std::string full_path = normalize_path(client_current_paths_[client_index], filename);
    ESP_LOGI(TAG, "Starting file upload to: %s", full_path.c_str());
    send_response(client_socket, 150, "Opening connection for file upload");
    start_file_upload(client_socket, full_path);
  } else if (cmd_str.find("RETR") == 0) {
    std::string filename = cmd_str.substr(5);
    size_t first_non_space = filename.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      filename = filename.substr(first_non_space);
    }
    
    std::string full_path = normalize_path(client_current_paths_[client_index], filename);
    ESP_LOGI(TAG, "Starting file download from: %s", full_path.c_str());
    
    struct stat file_stat;
    if (stat(full_path.c_str(), &file_stat) == 0) {
      if (S_ISREG(file_stat.st_mode)) {
        std::string size_msg = "Opening connection for file download (" +
                              std::to_string(file_stat.st_size) + " bytes)";
        send_response(client_socket, 150, size_msg);
        start_file_download(client_socket, full_path);
      } else {
        send_response(client_socket, 550, "Not a regular file");
      }
    } else {
      ESP_LOGE(TAG, "File not found: %s (errno: %d)", full_path.c_str(), errno);
      send_response(client_socket, 550, "File not found");
    }
  } else if (cmd_str.find("DELE") == 0) {
    std::string filename = cmd_str.substr(5);
    size_t first_non_space = filename.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      filename = filename.substr(first_non_space);
    }
    
    std::string full_path = normalize_path(client_current_paths_[client_index], filename);
    ESP_LOGI(TAG, "Deleting file: %s", full_path.c_str());
    
    if (unlink(full_path.c_str()) == 0) {
      send_response(client_socket, 250, "File deleted successfully");
    } else {
      ESP_LOGE(TAG, "Failed to delete file: %s (errno: %d)", full_path.c_str(), errno);
      send_response(client_socket, 550, "Failed to delete file");
    }
  } else if (cmd_str.find("MKD") == 0) {
    std::string dirname = cmd_str.substr(4);
    size_t first_non_space = dirname.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      dirname = dirname.substr(first_non_space);
    }
    
    std::string full_path = normalize_path(client_current_paths_[client_index], dirname);
    ESP_LOGI(TAG, "Creating directory: %s", full_path.c_str());
    
    if (mkdir(full_path.c_str(), 0755) == 0) {
      send_response(client_socket, 257, "Directory created");
    } else {
      ESP_LOGE(TAG, "Failed to create directory: %s (errno: %d)", full_path.c_str(), errno);
      send_response(client_socket, 550, "Failed to create directory");
    }
  } else if (cmd_str.find("RMD") == 0) {
    std::string dirname = cmd_str.substr(4);
    size_t first_non_space = dirname.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      dirname = dirname.substr(first_non_space);
    }
    
    std::string full_path = normalize_path(client_current_paths_[client_index], dirname);
    ESP_LOGI(TAG, "Removing directory: %s", full_path.c_str());
    
    if (rmdir(full_path.c_str()) == 0) {
      send_response(client_socket, 250, "Directory removed");
    } else {
      ESP_LOGE(TAG, "Failed to remove directory: %s (errno: %d)", full_path.c_str(), errno);
      send_response(client_socket, 550, "Failed to remove directory");
    }
  } else if (cmd_str.find("RNFR") == 0) {
    std::string filename = cmd_str.substr(5);
    size_t first_non_space = filename.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      filename = filename.substr(first_non_space);
    }
    
    rename_from_ = normalize_path(client_current_paths_[client_index], filename);
    struct stat file_stat;
    if (stat(rename_from_.c_str(), &file_stat) == 0) {
      send_response(client_socket, 350, "Ready for RNTO");
    } else {
      ESP_LOGE(TAG, "File not found for rename: %s (errno: %d)", rename_from_.c_str(), errno);
      send_response(client_socket, 550, "File not found");
      rename_from_ = "";
    }
  } else if (cmd_str.find("RNTO") == 0) {
    if (rename_from_.empty()) {
      send_response(client_socket, 503, "RNFR required first");
    } else {
      std::string filename = cmd_str.substr(5);
      size_t first_non_space = filename.find_first_not_of(" \t");
      if (first_non_space != std::string::npos) {
        filename = filename.substr(first_non_space);
      }
      
      std::string rename_to = normalize_path(client_current_paths_[client_index], filename);
      ESP_LOGI(TAG, "Renaming from %s to %s", rename_from_.c_str(), rename_to.c_str());
      
      if (rename(rename_from_.c_str(), rename_to.c_str()) == 0) {
        send_response(client_socket, 250, "Rename successful");
      } else {
        ESP_LOGE(TAG, "Failed to rename: %s -> %s (errno: %d)", 
                 rename_from_.c_str(), rename_to.c_str(), errno);
        send_response(client_socket, 550, "Rename failed");
      }
      rename_from_ = "";
    }
  } else if (cmd_str.find("SIZE") == 0) {
    std::string filename = cmd_str.substr(5);
    size_t first_non_space = filename.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      filename = filename.substr(first_non_space);
    }
    
    std::string full_path = normalize_path(client_current_paths_[client_index], filename);
    struct stat file_stat;
    if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
      send_response(client_socket, 213, std::to_string(file_stat.st_size));
    } else {
      send_response(client_socket, 550, "File not found or not a regular file");
    }
  } else if (cmd_str.find("MDTM") == 0) {
    std::string filename = cmd_str.substr(5);
    size_t first_non_space = filename.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
      filename = filename.substr(first_non_space);
    }
    
    std::string full_path = normalize_path(client_current_paths_[client_index], filename);
    struct stat file_stat;
    if (stat(full_path.c_str(), &file_stat) == 0) {
      char mdtm_str[15];
      struct tm *tm_info = gmtime(&file_stat.st_mtime);
      strftime(mdtm_str, sizeof(mdtm_str), "%Y%m%d%H%M%S", tm_info);
      send_response(client_socket, 213, mdtm_str);
    } else {
      send_response(client_socket, 550, "File not found");
    }
  } else if (cmd_str.find("NOOP") == 0) {
    send_response(client_socket, 200, "NOOP command successful");
  } else if (cmd_str.find("QUIT") == 0) {
    send_response(client_socket, 221, "Goodbye");
    close(client_socket);
    auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
    if (it != client_sockets_.end()) {
      size_t index = it - client_sockets_.begin();
      client_sockets_.erase(it);
      client_states_.erase(client_states_.begin() + index);
      client_usernames_.erase(client_usernames_.begin() + index);
      client_current_paths_.erase(client_current_paths_.begin() + index);
    }
  } else {
    send_response(client_socket, 502, "Command not implemented");
  }
}

void FTPServer::send_response(int client_socket, int code, const std::string& message) {
  std::string response = std::to_string(code) + " " + message + "\r\n";
  send(client_socket, response.c_str(), response.length(), 0);
  ESP_LOGD(TAG, "Sent: %s", response.c_str());
}

bool FTPServer::authenticate(const std::string& username, const std::string& password) {
  return username == username_ && password == password_;
}

bool FTPServer::start_passive_mode(int client_socket) {
  if (passive_data_socket_ != -1) {
    close(passive_data_socket_);
    passive_data_socket_ = -1;
  }

  passive_data_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (passive_data_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create passive data socket (errno: %d)", errno);
    return false;
  }

  int opt = 1;
  if (setsockopt(passive_data_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    ESP_LOGE(TAG, "Failed to set socket options for passive mode (errno: %d)", errno);
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  data_addr.sin_port = htons(0);

  if (bind(passive_data_socket_, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind passive data socket (errno: %d)", errno);
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  if (listen(passive_data_socket_, 1) < 0) {
    ESP_LOGE(TAG, "Failed to listen on passive data socket (errno: %d)", errno);
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  struct sockaddr_in sin;
  socklen_t len = sizeof(sin);
  if (getsockname(passive_data_socket_, (struct sockaddr *)&sin, &len) < 0) {
    ESP_LOGE(TAG, "Failed to get socket name (errno: %d)", errno);
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  passive_data_port_ = ntohs(sin.sin_port);

  esp_netif_t *netif = esp_netif_get_default_netif();
  if (netif == nullptr) {
    ESP_LOGE(TAG, "Failed to get default netif");
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get IP info");
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    return false;
  }

  uint32_t ip = ip_info.ip.addr;
  std::string response = "Entering Passive Mode (" +
                        std::to_string((ip & 0xFF)) + "," +
                        std::to_string((ip >> 8) & 0xFF) + "," +
                        std::to_string((ip >> 16) & 0xFF) + "," +
                        std::to_string((ip >> 24) & 0xFF) + "," +
                        std::to_string(passive_data_port_ >> 8) + "," +
                        std::to_string(passive_data_port_ & 0xFF) + ")";

  send_response(client_socket, 227, response);
  return true;
}

int FTPServer::open_data_connection(int client_socket) {
  if (passive_data_socket_ == -1) {
    return -1;
  }

  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(passive_data_socket_, &readfds);

  int ret = select(passive_data_socket_ + 1, &readfds, nullptr, nullptr, &tv);
  if (ret <= 0) {
    return -1;
  }

  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int data_socket = accept(passive_data_socket_, (struct sockaddr *)&client_addr, &client_len);

  if (data_socket < 0) {
    return -1;
  }

  int flags = fcntl(data_socket, F_GETFL, 0);
  fcntl(data_socket, F_SETFL, flags & ~O_NONBLOCK);

  return data_socket;
}

void FTPServer::close_data_connection(int client_socket) {
  if (passive_data_socket_ != -1) {
    close(passive_data_socket_);
    passive_data_socket_ = -1;
    passive_data_port_ = -1;
    passive_mode_enabled_ = false;
  }
}

void FTPServer::list_directory(int client_socket, const std::string& path) {
  int data_socket = open_data_connection(client_socket);
  if (data_socket < 0) {
    send_response(client_socket, 425, "Can't open data connection");
    return;
  }

  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    close(data_socket);
    close_data_connection(client_socket);
    send_response(client_socket, 550, "Failed to open directory");
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entry_name = entry->d_name;
    if (entry_name == "." || entry_name == "..") {
      continue;
    }

    std::string full_path = path + "/" + entry_name;
    struct stat entry_stat;
    if (stat(full_path.c_str(), &entry_stat) == 0) {
      char time_str[80];
      strftime(time_str, sizeof(time_str), "%b %d %H:%M", localtime(&entry_stat.st_mtime));
      
      char perm_str[11] = "----------";
      if (S_ISDIR(entry_stat.st_mode)) perm_str[0] = 'd';
      if (entry_stat.st_mode & S_IRUSR) perm_str[1] = 'r';
      if (entry_stat.st_mode & S_IWUSR) perm_str[2] = 'w';
      if (entry_stat.st_mode & S_IXUSR) perm_str[3] = 'x';
      if (entry_stat.st_mode & S_IRGRP) perm_str[4] = 'r';
      if (entry_stat.st_mode & S_IWGRP) perm_str[5] = 'w';
      if (entry_stat.st_mode & S_IXGRP) perm_str[6] = 'x';
      if (entry_stat.st_mode & S_IROTH) perm_str[7] = 'r';
      if (entry_stat.st_mode & S_IWOTH) perm_str[8] = 'w';
      if (entry_stat.st_mode & S_IXOTH) perm_str[9] = 'x';

      char list_item[512];
      snprintf(list_item, sizeof(list_item),
               "%s 1 root root %8ld %s %s\r\n",
               perm_str, (long)entry_stat.st_size, time_str, entry_name.c_str());
      
      send(data_socket, list_item, strlen(list_item), 0);
    }
  }

  closedir(dir);
  close(data_socket);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "Directory send OK");
}

void FTPServer::list_names(int client_socket, const std::string& path) {
  int data_socket = open_data_connection(client_socket);
  if (data_socket < 0) {
    send_response(client_socket, 425, "Can't open data connection");
    return;
  }

  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    close(data_socket);
    close_data_connection(client_socket);
    send_response(client_socket, 550, "Failed to open directory");
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entry_name = entry->d_name;
    if (entry_name == "." || entry_name == "..") {
      continue;
    }

    std::string full_path = path + "/" + entry_name;
    struct stat entry_stat;
    if (stat(full_path.c_str(), &entry_stat) == 0) {
      std::string list_item = entry_name + "\r\n";
      send(data_socket, list_item.c_str(), list_item.length(), 0);
    }
  }

  closedir(dir);
  close(data_socket);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "Directory send OK");
}

void FTPServer::start_file_upload(int client_socket, const std::string& path) {
  int data_socket = open_data_connection(client_socket);
  if (data_socket < 0) {
    send_response(client_socket, 425, "Can't open data connection");
    return;
  }

  int file_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (file_fd < 0) {
    close(data_socket);
    close_data_connection(client_socket);
    send_response(client_socket, 550, "Failed to open file for writing");
    return;
  }

  char buffer[2048];
  int len;
  while ((len = recv(data_socket, buffer, sizeof(buffer), 0)) > 0) {
    write(file_fd, buffer, len);
  }

  close(file_fd);
  close(data_socket);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "Transfer complete");
}

void FTPServer::start_file_download(int client_socket, const std::string& path) {
  int data_socket = open_data_connection(client_socket);
  if (data_socket < 0) {
    send_response(client_socket, 425, "Can't open data connection");
    return;
  }

  int file_fd = open(path.c_str(), O_RDONLY);
  if (file_fd < 0) {
    close(data_socket);
    close_data_connection(client_socket);
    send_response(client_socket, 550, "Failed to open file for reading");
    return;
  }

  char buffer[2048];
  int len;
  while ((len = read(file_fd, buffer, sizeof(buffer))) > 0) {
    send(data_socket, buffer, len, 0);
  }

  close(file_fd);
  close(data_socket);
  close_data_connection(client_socket);
  send_response(client_socket, 226, "Transfer complete");
}

bool FTPServer::is_running() const {
  return ftp_server_socket_ != -1;
}

}  // namespace ftp_server
}  // namespace esphome

































