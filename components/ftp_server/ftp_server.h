#pragma once

#include "esphome/core/component.h"
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace esphome {
namespace ftp_server {

enum FTPClientState {
  FTP_WAIT_LOGIN,
  FTP_LOGGED_IN
};

class FTPServer : public Component {
 public:
  FTPServer();
  void setup() override;
  void loop() override;
  void dump_config() override;

  

  // Définir une priorité d'initialisation tardive pour ESP-IDF
  float get_setup_priority() const override { return setup_priority::LATE - 1; }

  void set_port(uint16_t port) { port_ = port; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void set_root_path(const std::string &root_path) { root_path_ = root_path; }

  // Méthode pour vérifier si le serveur est en cours d'exécution
  bool is_running() const;

 protected:
  void handle_new_clients();
  void handle_ftp_client(int client_socket);
  void process_command(int client_socket, const std::string& command);
  void send_response(int client_socket, int code, const std::string& message);
  bool authenticate(const std::string& username, const std::string& password);
  void list_directory(int client_socket, const std::string& path);
  void list_names(int client_socket, const std::string& path);  // Add this line
  void start_file_upload(int client_socket, const std::string& path);
  void start_file_download(int client_socket, const std::string& path);

  uint16_t port_{21};
  std::string username_{"admin"};
  std::string password_{"admin"};
  std::string root_path_{"/sdcard"};
  std::string current_path_;
  int ftp_server_socket_{-1};
  std::vector<int> client_sockets_;
  std::vector<FTPClientState> client_states_;
  std::vector<std::string> client_usernames_;
  std::vector<std::string> client_current_paths_;

  // Variables pour le mode passif
  bool passive_mode_enabled_ = false;
  int passive_data_socket_ = -1;
  int passive_data_port_ = -1;
  std::string passive_client_ip_;

  // Variable pour la commande RNFR
  std::string rename_from_;  // Add this line

  // Méthodes pour le mode passif
  bool start_passive_mode(int client_socket);
  int open_data_connection(int client_socket);
  void close_data_connection(int client_socket);
};

}  // namespace ftp_server
}  // namespace esphome








