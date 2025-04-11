#pragma once

#include <cstddef>
#include <cstring>
#include <algorithm>

namespace esphome {
namespace ftp_http_proxy {

/**
 * @brief Class implementing a circular buffer for efficient data streaming
 * 
 * This buffer allows continuous writing and reading of data without having
 * to move memory around, which is particularly useful for streaming applications.
 */
class CircularBuffer {
public:
    /**
     * @brief Construct a new Circular Buffer with the specified size
     * 
     * @param size Size of the buffer in bytes
     */
    CircularBuffer(size_t size) 
        : buffer_(new uint8_t[size]), 
          size_(size),
          read_pos_(0),
          write_pos_(0),
          full_(false) {}

    /**
     * @brief Destroy the Circular Buffer and free allocated memory
     */
    ~CircularBuffer() {
        delete[] buffer_;
    }

    /**
     * @brief Get the free space available in the buffer
     * 
     * @return size_t Free space in bytes
     */
    size_t freeSpace() const {
        return available_for_write();
    }    /**
     * @brief Write data to the buffer
     * 
     * @param data Pointer to the data to write
     * @param len Length of the data to write
     * @return size_t Number of bytes actually written
     */
    size_t write(const void* data, size_t len) {
        if (isFull()) {
            return 0;
        }

        const uint8_t* input = static_cast<const uint8_t*>(data);
        size_t bytes_to_write = std::min(len, available_for_write());

        // Write in two steps if wrapping around the buffer end
        if (write_pos_ + bytes_to_write > size_) {
            // First part: from write_pos to end of buffer
            size_t first_part = size_ - write_pos_;
            std::memcpy(buffer_ + write_pos_, input, first_part);
            
            // Second part: from beginning of buffer
            size_t second_part = bytes_to_write - first_part;
            std::memcpy(buffer_, input + first_part, second_part);
            
            write_pos_ = second_part;
        } else {
            // No wrap-around needed
            std::memcpy(buffer_ + write_pos_, input, bytes_to_write);
            write_pos_ = (write_pos_ + bytes_to_write) % size_;
        }

        // Check if buffer became full after this write
        if (write_pos_ == read_pos_) {
            full_ = true;
        }

        return bytes_to_write;
    }

    /**
     * @brief Read data from the buffer
     * 
     * @param data Pointer to where the data should be stored
     * @param len Maximum number of bytes to read
     * @return size_t Number of bytes actually read
     */
    size_t read(void* data, size_t len) {
        if (isEmpty()) {
            return 0;
        }

        uint8_t* output = static_cast<uint8_t*>(data);
        size_t bytes_to_read = std::min(len, available());

        // Read in two steps if wrapping around the buffer end
        if (read_pos_ + bytes_to_read > size_) {
            // First part: from read_pos to end of buffer
            size_t first_part = size_ - read_pos_;
            std::memcpy(output, buffer_ + read_pos_, first_part);
            
            // Second part: from beginning of buffer
            size_t second_part = bytes_to_read - first_part;
            std::memcpy(output + first_part, buffer_, second_part);
            
            read_pos_ = second_part;
        } else {
            // No wrap-around needed
            std::memcpy(output, buffer_ + read_pos_, bytes_to_read);
            read_pos_ = (read_pos_ + bytes_to_read) % size_;
        }

        // Buffer is no longer full after reading
        full_ = false;

        return bytes_to_read;
    }

    /**
     * @brief Check if the buffer is empty
     * 
     * @return true Buffer is empty
     * @return false Buffer contains data
     */
    bool isEmpty() const {
        return !full_ && (read_pos_ == write_pos_);
    }

    /**
     * @brief Check if the buffer is full
     * 
     * @return true Buffer is full
     * @return false Buffer has space available
     */
    bool isFull() const {
        return full_;
    }

    /**
     * @brief Get number of bytes available for reading
     * 
     * @return size_t Bytes available to read
     */
    size_t available() const {
        if (full_) {
            return size_;
        }
        
        if (write_pos_ >= read_pos_) {
            return write_pos_ - read_pos_;
        } else {
            return size_ - (read_pos_ - write_pos_);
        }
    }

    /**
     * @brief Get number of bytes available for writing
     * 
     * @return size_t Bytes available to write
     */
    size_t available_for_write() const {
        return size_ - available();
    }

    /**
     * @brief Get total capacity of the buffer
     * 
     * @return size_t Total buffer capacity in bytes
     */
    size_t capacity() const {
        return size_;
    }

    /**
     * @brief Clear all data from the buffer
     */
    void clear() {
        read_pos_ = 0;
        write_pos_ = 0;
        full_ = false;
    }

private:
    uint8_t* buffer_;   // Buffer memory
    size_t size_;       // Buffer size
    size_t read_pos_;   // Current read position
    size_t write_pos_;  // Current write position
    bool full_;         // Flag indicating if buffer is full
};

}  // namespace ftp_http_proxy
}  // namespace esphome
