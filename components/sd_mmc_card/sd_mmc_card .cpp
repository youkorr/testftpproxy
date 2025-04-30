#include "sd_mmc_card.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // For delay() function

#include <string>
#include <vector>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_mmc_card";
static const std::string MOUNT_POINT = "/sdcard";

namespace esphome {
namespace sd_mmc_card {

long double convertBytes(uint64_t value, MemoryUnits unit) {
  switch (unit) {
    case BYTES:
      return static_cast<long double>(value);
    case KILOBYTES:
      return static_cast<long double>(value) / 1024.0;
    case MEGABYTES:
      return static_cast<long double>(value) / (1024.0 * 1024.0);
    case GIGABYTES:
      return static_cast<long double>(value) / (1024.0 * 1024.0 * 1024.0);
    default:
      return static_cast<long double>(value);
  }
}

FileInfo::FileInfo(std::string const &path, size_t size, bool is_directory)
    : path(path), size(size), is_directory(is_directory) {}

void SdMmc::setup() {
  ESP_LOGCONFIG(TAG, "Setting up SD MMC...");
  
  // Power control
  if (this->power_ctrl_pin_ != nullptr) {
    this->power_ctrl_pin_->setup();
    this->power_ctrl_pin_->digital_write(false);
    delay(100);  // Wait for card to discharge
    this->power_ctrl_pin_->digital_write(true);
    delay(100);  // Wait for card to initialize
  }
  
  // Explicitly disable pull-up resistor on GPIO12 (DATA3) before anything else
  if (!this->mode_1bit_ && this->data3_pin_ == 12) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << 12);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;  // Fixed: use proper enum value
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;      // Fixed: use proper enum value
    gpio_config(&io_conf);
    ESP_LOGD(TAG, "GPIO12 pull-up disabled for SD card DATA3");
  }
  
  // Optimal configuration
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 16,
    .allocation_unit_size = 64 * 1024  // 64KB optimizes file writing
  };
  
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 50MHz
  
  // Enable DMA for optimal performance
  #ifdef SDMMC_HOST_FLAG_DMA
  host.flags |= SDMMC_HOST_FLAG_DMA;
  #endif
  
  // Enable DDR only in 4-bit mode
  if (!this->mode_1bit_) {
    host.flags |= SDMMC_HOST_FLAG_DDR;
  } else {
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
  }
  
  // Disable internal pull-up resistors of the SDMMC host
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.flags &= ~SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  
  // Bus width configuration
  slot_config.width = this->mode_1bit_ ? 1 : 4;
  
  // Pin configuration
  #ifdef SOC_SDMMC_USE_GPIO_MATRIX
  slot_config.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot_config.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot_config.d0 = static_cast<gpio_num_t>(this->data0_pin_);
  
  if (!this->mode_1bit_) {
    slot_config.d1 = static_cast<gpio_num_t>(this->data1_pin_);
    slot_config.d2 = static_cast<gpio_num_t>(this->data2_pin_);
    slot_config.d3 = static_cast<gpio_num_t>(this->data3_pin_);
  }
  #endif
  
  // Card mounting attempts
  const int max_attempts = 3;
  for (int attempt = 1; attempt <= max_attempts; attempt++) {
    ESP_LOGI(TAG, "Mounting SD Card (attempt %d/%d)...", attempt, max_attempts);
    
    // Reset GPIO12 again before new attempt if it's DATA3
    if (!this->mode_1bit_ && this->data3_pin_ == 12) {
      gpio_pullup_dis(static_cast<gpio_num_t>(12));
      gpio_pulldown_dis(static_cast<gpio_num_t>(12));
      delay(50);
    }
    
    // Fixed: Convert std::string to const char* using c_str()
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT.c_str(), &host, &slot_config, &mount_config, &this->card_);
    
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "SD Card mounted successfully!");
      break;
    }
    
    ESP_LOGW(TAG, "SD Card mount failed (attempt %d/%d): %s", attempt, max_attempts, esp_err_to_name(ret));
    
    if (attempt < max_attempts) {
      delay(500);  // Wait before next attempt
    } else {
      this->init_error_ = static_cast<ErrorCode>(ret);  // Fixed: cast esp_err_t to ErrorCode
      this->mark_failed();
      return;
    }
  }
  
  if (this->is_failed()) {
    return;
  }
  
  // Information about the mounted card
  sdmmc_card_t *card = this->card_;
  ESP_LOGI(TAG, "SD Card Information:");
  ESP_LOGI(TAG, "  Name: %s", card->cid.name);
  ESP_LOGI(TAG, "  Type: %s", (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC");
  ESP_LOGI(TAG, "  Speed: %s", (card->csd.tr_speed > 25000000) ? "High Speed" : "Default Speed");
  ESP_LOGI(TAG, "  Size: %lluMB", ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
  ESP_LOGI(TAG, "  CSD Version: %s", (card->csd.csd_ver == 0) ? "1.0" : "2.0");
  ESP_LOGI(TAG, "  Freq: %ukHz", card->max_freq_khz);
  
  card_->real_freq_khz = card->max_freq_khz;
  card_->max_freq_khz = card->max_freq_khz;
  card_->is_ddr = !!(host.flags & SDMMC_HOST_FLAG_DDR);
  
  // Update sensors if enabled
  #ifdef USE_SENSOR
  if (this->total_space_sensor_ != nullptr || this->free_space_sensor_ != nullptr || this->used_space_sensor_ != nullptr ||
      !this->file_size_sensors_.empty()) {
    this->set_interval(this->interval_);  // Fixed: using set_interval instead of set_update_interval
  }
  #endif
}

void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file(path, buffer, len, "w");
}

void SdMmc::append_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file(path, buffer, len, "a");
}

void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len, const char *mode) {
  if (this->is_failed())
    return;
    
  std::string full_path = MOUNT_POINT + path;
  
  FILE *f = fopen(full_path.c_str(), mode);
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path.c_str());
    return;
  }
  
  size_t written = fwrite(buffer, 1, len, f);
  if (written != len) {
    ESP_LOGE(TAG, "Failed to write data to file: %s (wrote %d of %d bytes)", full_path.c_str(), written, len);
  }
  
  fclose(f);
}

void SdMmc::write_file_chunked(const char *path, const uint8_t *buffer, size_t len, size_t chunk_size) {
  if (this->is_failed())
    return;
    
  std::string full_path = MOUNT_POINT + path;
  
  FILE *f = fopen(full_path.c_str(), "w");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path.c_str());
    return;
  }
  
  size_t total_written = 0;
  size_t remaining = len;
  
  while (remaining > 0) {
    size_t to_write = (remaining > chunk_size) ? chunk_size : remaining;
    size_t written = fwrite(buffer + total_written, 1, to_write, f);
    
    if (written != to_write) {
      ESP_LOGE(TAG, "Failed to write chunk to file: %s (wrote %d of %d bytes)", full_path.c_str(), written, to_write);
      fclose(f);
      return;
    }
    
    total_written += written;
    remaining -= written;
    
    // Flush periodically to avoid data loss
    fflush(f);
    
    // Allow other tasks to run
    delay(1);
  }
  
  fclose(f);
}

std::vector<std::string> SdMmc::list_directory(const char *path, uint8_t depth) {
  std::vector<FileInfo> file_infos = this->list_directory_file_info(path, depth);
  std::vector<std::string> result;
  
  for (const auto &info : file_infos) {
    result.push_back(info.path);
  }
  
  return result;
}

std::vector<std::string> SdMmc::list_directory(std::string path, uint8_t depth) {
  return this->list_directory(path.c_str(), depth);
}

std::vector<FileInfo> SdMmc::list_directory_file_info(const char *path, uint8_t depth) {
  std::vector<FileInfo> result;
  
  this->list_directory_file_info_rec(path, depth, result);
  
  return result;
}

std::vector<FileInfo> SdMmc::list_directory_file_info(std::string path, uint8_t depth) {
  return this->list_directory_file_info(path.c_str(), depth);
}

std::vector<FileInfo> &SdMmc::list_directory_file_info_rec(const char *path, uint8_t depth,
                                                        std::vector<FileInfo> &result) {
  if (this->is_failed())
    return result;
  
  std::string dir_path = MOUNT_POINT + path;
  DIR *dir = opendir(dir_path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory: %s", dir_path.c_str());
    return result;
  }
  
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    
    std::string entry_path = std::string(path) + "/" + entry->d_name;
    if (entry_path.front() == '/')
      entry_path = entry_path.substr(1);
    
    std::string full_path = MOUNT_POINT + "/" + entry_path;
    
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0) {
      bool is_dir = S_ISDIR(st.st_mode);
      result.push_back(FileInfo(entry_path, is_dir ? 0 : st.st_size, is_dir));
      
      if (is_dir && depth > 0) {
        this->list_directory_file_info_rec(entry_path.c_str(), depth - 1, result);
      }
    }
  }
  
  closedir(dir);
  return result;
}

bool SdMmc::is_directory(const char *path) {
  if (this->is_failed())
    return false;
  
  std::string full_path = MOUNT_POINT + path;
  
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat path: %s", full_path.c_str());
    return false;
  }
  
  return S_ISDIR(st.st_mode);
}

size_t SdMmc::file_size(const char *path) {
  if (this->is_failed())
    return 0;
  
  std::string full_path = MOUNT_POINT + path;
  
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat file: %s", full_path.c_str());
    return 0;
  }
  
  if (S_ISDIR(st.st_mode)) {
    ESP_LOGE(TAG, "Path is a directory, not a file: %s", full_path.c_str());
    return 0;
  }
  
  return st.st_size;
}

std::string SdMmc::sd_card_type() const {
  if (this->is_failed() || this->card_ == nullptr)
    return "Unknown";
  
  sdmmc_card_t *card = this->card_;
  
  if (card->is_sdio)
    return "SDIO";
  else if (card->is_mmc)
    return "MMC";
  else if (card->ocr & SD_OCR_SDHC_CAP)
    return "SDHC/SDXC";
  else
    return "SDSC";
}

void SdMmc::update_sensors() {
  #ifdef USE_SENSOR
  if (this->is_failed())
    return;
  
  SDFS sd;
  uint64_t total_bytes = sd.totalBytes();
  uint64_t used_bytes = sd.usedBytes();
  uint64_t free_bytes = total_bytes - used_bytes;
  
  ESP_LOGV(TAG, "SD Card space - Total: %llu bytes, Used: %llu bytes, Free: %llu bytes",
          total_bytes, used_bytes, free_bytes);
  
  if (this->total_space_sensor_ != nullptr) {
    float value = convertBytes(total_bytes, this->total_space_sensor_unit_);
    this->total_space_sensor_->publish_state(value);
  }
  
  if (this->used_space_sensor_ != nullptr) {
    float value = convertBytes(used_bytes, this->used_space_sensor_unit_);
    this->used_space_sensor_->publish_state(value);
  }
  
  if (this->free_space_sensor_ != nullptr) {
    float value = convertBytes(free_bytes, this->free_space_sensor_unit_);
    this->free_space_sensor_->publish_state(value);
  }
  
  // Update file size sensors
  for (const auto &sensor_info : this->file_size_sensors_) {
    size_t size = this->file_size(sensor_info.path.c_str());
    float value = convertBytes(size, sensor_info.unit);
    sensor_info.sensor->publish_state(value);
  }
  #endif
}

bool SdMmc::create_directory(const char *path) {
  if (this->is_failed())
    return false;
  
  std::string full_path = MOUNT_POINT + path;
  
  if (mkdir(full_path.c_str(), 0755) != 0) {
    ESP_LOGE(TAG, "Failed to create directory: %s (errno: %d)", full_path.c_str(), errno);
    return false;
  }
  
  return true;
}

bool SdMmc::remove_directory(const char *path) {
  if (this->is_failed())
    return false;
  
  std::string full_path = MOUNT_POINT + path;
  
  // First, check if directory exists
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Directory not found: %s", full_path.c_str());
    return false;
  }
  
  if (!S_ISDIR(st.st_mode)) {
    ESP_LOGE(TAG, "Path is not a directory: %s", full_path.c_str());
    return false;
  }
  
  if (rmdir(full_path.c_str()) != 0) {
    ESP_LOGE(TAG, "Failed to remove directory: %s (errno: %d)", full_path.c_str(), errno);
    return false;
  }
  
  return true;
}

bool SdMmc::delete_file(const char *path) {
  if (this->is_failed())
    return false;
  
  std::string full_path = MOUNT_POINT + path;
  
  // First, check if file exists
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "File not found: %s", full_path.c_str());
    return false;
  }
  
  if (S_ISDIR(st.st_mode)) {
    ESP_LOGE(TAG, "Path is a directory, not a file: %s", full_path.c_str());
    return false;
  }
  
  if (unlink(full_path.c_str()) != 0) {
    ESP_LOGE(TAG, "Failed to delete file: %s (errno: %d)", full_path.c_str(), errno);
    return false;
  }
  
  return true;
}

std::vector<uint8_t> SdMmc::read_file(const char *path) {
  std::vector<uint8_t> data;
  if (this->is_failed())
    return data;
  
  std::string full_path = MOUNT_POINT + path;
  
  // Get file size
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat file: %s", full_path.c_str());
    return data;
  }
  
  if (S_ISDIR(st.st_mode)) {
    ESP_LOGE(TAG, "Path is a directory, not a file: %s", full_path.c_str());
    return data;
  }
  
  // Allocate buffer for file content
  data.resize(st.st_size);
  
  // Open and read file
  FILE *f = fopen(full_path.c_str(), "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path.c_str());
    data.clear();
    return data;
  }
  
  size_t read = fread(data.data(), 1, data.size(), f);
  fclose(f);
  
  if (read != data.size()) {
    ESP_LOGE(TAG, "Failed to read entire file: %s (read %d of %d bytes)", full_path.c_str(), read, data.size());
    data.resize(read);  // Resize to actual bytes read
  }
  
  return data;
}

void SdMmc::read_file_stream(const char *path, size_t offset, size_t chunk_size,
                           std::function<bool(std::vector<uint8_t> &)> callback) {
  if (this->is_failed())
    return;
  
  std::string full_path = MOUNT_POINT + path;
  
  // Get file size
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat file: %s", full_path.c_str());
    return;
  }
  
  if (S_ISDIR(st.st_mode)) {
    ESP_LOGE(TAG, "Path is a directory, not a file: %s", full_path.c_str());
    return;
  }
  
  // Check offset
  if (offset >= static_cast<size_t>(st.st_size)) {
    ESP_LOGE(TAG, "Offset is beyond file size: %s (offset: %d, size: %d)", full_path.c_str(), offset, st.st_size);
    return;
  }
  
  // Open file
  FILE *f = fopen(full_path.c_str(), "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path.c_str());
    return;
  }
  
  // Seek to offset
  if (fseek(f, offset, SEEK_SET) != 0) {
    ESP_LOGE(TAG, "Failed to seek to offset in file: %s (offset: %d)", full_path.c_str(), offset);
    fclose(f);
    return;
  }
  
  // Read and process in chunks
  std::vector<uint8_t> buffer(chunk_size);
  size_t remaining = st.st_size - offset;
  bool continue_reading = true;
  
  while (remaining > 0 && continue_reading) {
    size_t to_read = (remaining > chunk_size) ? chunk_size : remaining;
    size_t read = fread(buffer.data(), 1, to_read, f);
    
    if (read > 0) {
      buffer.resize(read);  // Resize to actual bytes read
      continue_reading = callback(buffer);
      remaining -= read;
    } else {
      break;  // Error or EOF
    }
  }
  
  fclose(f);
}

// Convenience overloads
size_t SdMmc::file_size(std::string const &path) { return this->file_size(path.c_str()); }

bool SdMmc::is_directory(std::string const &path) { return this->is_directory(path.c_str()); }

bool SdMmc::delete_file(std::string const &path) { return this->delete_file(path.c_str()); }

std::vector<uint8_t> SdMmc::read_file(std::string const &path) { return this->read_file(path.c_str()); }

std::vector<uint8_t> SdMmc::read_file_chunked(std::string const &path, size_t offset, size_t chunk_size) {
  std::vector<uint8_t> result;
  this->read_file_stream(path.c_str(), offset, chunk_size, [&result](std::vector<uint8_t> &chunk) {
    result.insert(result.end(), chunk.begin(), chunk.end());
    return true;
  });
  return result;
}

void SdMmc::add_file_size_sensor(sensor::Sensor *sensor, std::string const &path) {
  #ifdef USE_SENSOR
  this->file_size_sensors_.push_back({sensor, path, BYTES});
  #endif
}

void SdMmc::set_clk_pin(uint8_t pin) { this->clk_pin_ = pin; }

void SdMmc::set_cmd_pin(uint8_t pin) { this->cmd_pin_ = pin; }

void SdMmc::set_data0_pin(uint8_t pin) { this->data0_pin_ = pin; }

void SdMmc::set_data1_pin(uint8_t pin) { this->data1_pin_ = pin; }

void SdMmc::set_data2_pin(uint8_t pin) { this->data2_pin_ = pin; }

void SdMmc::set_data3_pin(uint8_t pin) { this->data3_pin_ = pin; }

void SdMmc::set_mode_1bit(bool b) { this->mode_1bit_ = b; }

void SdMmc::set_power_ctrl_pin(GPIOPin *pin) { this->power_ctrl_pin_ = pin; }

std::string SdMmc::error_code_to_string(SdMmc::ErrorCode code) {
  switch (code) {
    case ErrorCode::NONE:
      return "No Error";
    case ErrorCode::MOUNT_FAILED:
      return "Mount Failed";
    case ErrorCode::INIT_FAILED:
      return "Initialization Failed";
    case ErrorCode::CARD_NOT_FOUND:
      return "Card Not Found";
    default:
      return "Unknown Error (" + std::to_string(static_cast<int>(code)) + ")";
  }
}

}  // namespace sd_mmc_card
}  // namespace esphome

