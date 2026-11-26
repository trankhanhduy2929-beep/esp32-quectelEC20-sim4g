#include "ec20.h"

#include "esphome/core/log.h"

#include <usb/usb_host.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>

namespace esphome::ec20 {

static const char *const TAG = "ec20";
static constexpr uint16_t EC20_VID = 0x2C7C;
static constexpr uint16_t EC20_PID = 0x0125;
static constexpr uint8_t EC20_NMEA_INTERFACE = 1;

static std::vector<std::string> split_nmea_fields(const std::string &line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t comma = line.find(',', start);
    if (comma == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, comma - start));
    start = comma + 1;
  }
  if (!fields.empty()) {
    const size_t star = fields.back().find('*');
    if (star != std::string::npos) {
      fields.back().resize(star);
    }
  }
  return fields;
}

static bool parse_nmea_float(const std::string &text, float &value) {
  if (text.empty()) {
    return false;
  }
  char *end = nullptr;
  value = std::strtof(text.c_str(), &end);
  return end != text.c_str();
}

static bool parse_nmea_coord(const std::string &raw, const std::string &hemisphere, float &value) {
  float packed = 0.0f;
  if (!parse_nmea_float(raw, packed)) {
    return false;
  }
  const int degrees = static_cast<int>(packed / 100.0f);
  const float minutes = packed - static_cast<float>(degrees) * 100.0f;
  value = static_cast<float>(degrees) + minutes / 60.0f;
  if (hemisphere == "S" || hemisphere == "W") {
    value = -value;
  }
  return true;
}

void EC20Component::setup() {
  usb_host_config_t host_config = {};
  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  err = cdc_acm_host_install(nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  if (xTaskCreate(task_fn, "ec20", 4096, this, 5, &this->task_handle_) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create EC20 task");
    this->mark_failed();
  }

  this->ensure_listener_();
}

void EC20Component::loop() {
  uint32_t event_flags = 0;
  usb_host_lib_handle_events(0, &event_flags);

  this->ensure_listener_();
  if (this->disconnected_ && this->client_fd_ >= 0) {
    this->close_client_();
  }
  if (this->nmea_disconnected_) {
    this->nmea_disconnected_ = false;
    this->close_nmea_();
  }
  this->ensure_nmea_port_();
  std::string nmea_data;
  {
    std::lock_guard<std::mutex> lock(this->nmea_mutex_);
    nmea_data.swap(this->nmea_rx_buffer_);
  }
  if (!nmea_data.empty()) {
    this->feed_nmea_(reinterpret_cast<const uint8_t *>(nmea_data.data()), nmea_data.size());
  }
  this->handle_tcp_();

  if (this->has_pending_response_) {
    std::string response;
    {
      std::lock_guard<std::mutex> lock(this->pending_mutex_);
      response.swap(this->pending_response_);
      this->has_pending_response_ = false;
    }
    ESP_LOGI(TAG, "AT response: %s", response.c_str());
    if (this->at_response_sensor_ != nullptr) {
      this->at_response_sensor_->publish_state(response);
    }
  }
}

void EC20Component::dump_config() {
  ESP_LOGCONFIG(TAG, "EC20 Quectel modem:");
  ESP_LOGCONFIG(TAG, "  VID: 0x%04X, PID: 0x%04X", EC20_VID, EC20_PID);
  ESP_LOGCONFIG(TAG, "  AT interface: %u", this->at_interfaces_[this->at_index_]);
  ESP_LOGCONFIG(TAG, "  Device open: %s", YESNO(this->handle_ != nullptr));
}

void EC20Component::test_at() {
  if (this->tcp_active_) {
    if (this->at_response_sensor_ != nullptr) {
      this->at_response_sensor_->publish_state("Add-on đang giữ kết nối TCP");
    }
    return;
  }
  this->force_at_ = true;
}

void EC20Component::task_fn(void *arg) { static_cast<EC20Component *>(arg)->task_loop_(); }

void EC20Component::task_loop_() {
  while (true) {
    if (this->handle_ == nullptr) {
      if (!this->open_device_()) {
        this->at_index_ = (this->at_index_ + 1) % this->at_interfaces_.size();
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      this->configure_device_();
      this->last_at_ = 0;
      this->force_at_ = true;
    }

    if (this->disconnected_) {
      this->disconnected_ = false;
      this->close_device_();
      continue;
    }

    if (this->tcp_active_) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (this->force_at_) {
      this->force_at_ = false;
      this->send_at_();
      this->last_at_ = millis();
      vTaskDelay(pdMS_TO_TICKS(2000));
      if (this->tcp_active_) {
        continue;
      }
      const bool ok = this->response_has_ok_();
      this->store_response_();
      if (!ok) {
        if (this->tcp_active_) {
          continue;
        }
        this->note_at_failure_("Không nhận OK từ AT");
        ESP_LOGW(TAG, "No OK on interface %u, trying next", this->at_interfaces_[this->at_index_]);
        this->close_device_();
        this->at_index_ = (this->at_index_ + 1) % this->at_interfaces_.size();
        vTaskDelay(pdMS_TO_TICKS(500));
      } else {
        this->note_at_success_();
        if (!this->gps_init_attempted_) {
          this->gps_init_attempted_ = true;
          if (!this->tcp_active_) {
            ESP_LOGI(TAG, "Enabling EC20 GNSS");
            const uint8_t gps_on[] = {'A', 'T', '+', 'Q', 'G', 'P', 'S', '=', '1', '\r', '\n'};
            this->send_raw_(gps_on, sizeof(gps_on));
            vTaskDelay(pdMS_TO_TICKS(300));
            if (!this->tcp_active_) {
              std::lock_guard<std::mutex> lock(this->rx_mutex_);
              this->rx_buffer_.clear();
            }
          }
        }
      }
    } else {
      const uint32_t now = millis();
      if (now - this->last_at_ >= 20000) {
        this->last_at_ = now;
        this->send_at_();
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (this->tcp_active_) {
          continue;
        }
        if (!this->response_has_ok_()) {
          this->keepalive_failures_++;
          this->note_at_failure_("Keepalive AT không phản hồi");
          ESP_LOGW(TAG, "Idle AT keepalive failed (%d/3) on interface %u", this->keepalive_failures_,
                   this->at_interfaces_[this->at_index_]);
          if (this->keepalive_failures_ >= 3) {
            this->keepalive_failures_ = 0;
            this->force_at_ = true;
          }
        } else {
          this->note_at_success_();
        }
        {
          std::lock_guard<std::mutex> lock(this->rx_mutex_);
          this->rx_buffer_.clear();
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

bool EC20Component::open_device_() {
  cdc_acm_host_device_config_t dev_config = {};
  dev_config.connection_timeout_ms = 1000;
  dev_config.out_buffer_size = 256;
  dev_config.in_buffer_size = 256;
  dev_config.event_cb = event_cb_;
  dev_config.data_cb = data_cb_;
  dev_config.user_arg = this;

  const uint8_t interface = this->at_interfaces_[this->at_index_];
  const esp_err_t err = cdc_acm_host_open_vendor_specific(EC20_VID, EC20_PID, interface, &dev_config,
                                                          &this->handle_);
  if (err != ESP_OK) {
    this->handle_ = nullptr;
    ESP_LOGD(TAG, "Open EC20 interface %u failed: %s", interface, esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(TAG, "Opened EC20 interface %u", interface);
  if (this->ever_opened_) {
    this->reconnect_count_++;
  }
  this->ever_opened_ = true;
  if (this->reconnects_ != nullptr) {
    this->reconnects_->publish_state(this->reconnect_count_);
  }
  if (this->usb_connected_ != nullptr) {
    this->usb_connected_->publish_state(true);
  }
  return true;
}

void EC20Component::close_device_() {
  this->publish_at_ready_(false);
  this->close_nmea_();
  if (this->handle_ != nullptr) {
    cdc_acm_host_close(this->handle_);
    this->handle_ = nullptr;
  }
  if (this->usb_connected_ != nullptr) {
    this->usb_connected_->publish_state(false);
  }
}

void EC20Component::ensure_nmea_port_() {
  if (!this->at_ready_ || this->nmea_handle_ != nullptr) {
    return;
  }
  const uint32_t now = millis();
  if (now < this->nmea_retry_at_) {
    return;
  }
  this->nmea_retry_at_ = now + 5000;

  cdc_acm_host_device_config_t dev_config = {};
  dev_config.connection_timeout_ms = 500;
  dev_config.out_buffer_size = 256;
  dev_config.in_buffer_size = 512;
  dev_config.event_cb = nmea_event_cb_;
  dev_config.data_cb = nmea_data_cb_;
  dev_config.user_arg = this;

  const esp_err_t err = cdc_acm_host_open_vendor_specific(EC20_VID, EC20_PID, EC20_NMEA_INTERFACE, &dev_config,
                                                          &this->nmea_handle_);
  if (err != ESP_OK) {
    this->nmea_handle_ = nullptr;
    ESP_LOGD(TAG, "NMEA interface %u not ready: %s", EC20_NMEA_INTERFACE, esp_err_to_name(err));
    return;
  }

  cdc_acm_line_coding_t line_coding = {};
  line_coding.dwDTERate = 115200;
  line_coding.bCharFormat = 0;
  line_coding.bParityType = 0;
  line_coding.bDataBits = 8;
  cdc_acm_host_line_coding_set(this->nmea_handle_, &line_coding);
  cdc_acm_host_set_control_line_state(this->nmea_handle_, true, true);
  ESP_LOGI(TAG, "Opened EC20 NMEA interface %u", EC20_NMEA_INTERFACE);
}

void EC20Component::close_nmea_() {
  if (this->nmea_handle_ != nullptr) {
    cdc_acm_host_close(this->nmea_handle_);
    this->nmea_handle_ = nullptr;
  }
}

bool EC20Component::configure_device_() {
  cdc_acm_line_coding_t line_coding = {};
  line_coding.dwDTERate = 115200;
  line_coding.bCharFormat = 0;
  line_coding.bParityType = 0;
  line_coding.bDataBits = 8;

  esp_err_t err = cdc_acm_host_line_coding_set(this->handle_, &line_coding);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SET_LINE_CODING failed: %s", esp_err_to_name(err));
  }

  err = cdc_acm_host_set_control_line_state(this->handle_, true, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SET_CONTROL_LINE_STATE(DTR/RTS) failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "DTR/RTS enabled");
  }

  vTaskDelay(pdMS_TO_TICKS(300));

  {
    const uint8_t no_sleep[] = {'A', 'T', '+', 'Q', 'S', 'C', 'L', 'K', '=', '0', '\r'};
    std::lock_guard<std::mutex> tx_lock(this->tx_mutex_);
    cdc_acm_host_data_tx_blocking(this->handle_, no_sleep, sizeof(no_sleep), 500);
  }
  vTaskDelay(pdMS_TO_TICKS(200));
  return true;
}

bool EC20Component::send_at_() {
  if (this->handle_ == nullptr) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(this->rx_mutex_);
    this->rx_buffer_.clear();
  }
  const uint8_t command[] = {'A', 'T', '\r', '\n'};
  std::lock_guard<std::mutex> tx_lock(this->tx_mutex_);
  const esp_err_t err = cdc_acm_host_data_tx_blocking(this->handle_, command, sizeof(command), 1000);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "AT TX failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "TX: AT\\r\\n");
  return true;
}

bool EC20Component::response_has_ok_() {
  std::lock_guard<std::mutex> lock(this->rx_mutex_);
  return this->rx_buffer_.find("OK") != std::string::npos;
}

void EC20Component::store_response_() {
  if (this->tcp_active_) {
    return;
  }
  std::string response;
  {
    std::lock_guard<std::mutex> lock(this->rx_mutex_);
    response.swap(this->rx_buffer_);
  }
  if (response.empty()) {
    ESP_LOGW(TAG, "No response to AT");
    this->publish_last_error_("Không có phản hồi AT");
    return;
  }
  std::string clean;
  clean.reserve(response.size());
  for (char c : response) {
    const uint8_t u = static_cast<uint8_t>(c);
    if (c == '\n' || c == '\t' || (u >= 0x20 && u < 0x7F)) {
      clean.push_back(c);
    }
  }
  if (clean.size() > 200) {
    clean.resize(200);
  }
  if (clean.find("OK") == std::string::npos) {
    this->publish_last_error_(clean);
  }
  {
    std::lock_guard<std::mutex> lock(this->pending_mutex_);
    this->pending_response_ = std::move(clean);
    this->has_pending_response_ = true;
  }
}

void EC20Component::publish_at_ready_(bool ready) {
  this->at_ready_ = ready;
  if (this->at_ready_sensor_ != nullptr) {
    this->at_ready_sensor_->publish_state(ready);
  }
}

void EC20Component::publish_last_error_(const std::string &error) {
  std::string clean = error;
  if (clean.size() > 200) {
    clean.resize(200);
  }
  if (clean.empty() || clean == this->last_error_text_) {
    return;
  }
  this->last_error_text_ = std::move(clean);
  if (this->last_error_sensor_ != nullptr) {
    this->last_error_sensor_->publish_state(this->last_error_text_);
  }
}

void EC20Component::note_at_failure_(const std::string &error) {
  this->at_failures_++;
  if (this->at_failures_sensor_ != nullptr) {
    this->at_failures_sensor_->publish_state(static_cast<float>(this->at_failures_));
  }
  this->publish_last_error_(error);
}

void EC20Component::note_at_success_() {
  this->keepalive_failures_ = 0;
  this->at_failures_ = 0;
  if (this->at_failures_sensor_ != nullptr) {
    this->at_failures_sensor_->publish_state(0.0f);
  }
  this->publish_at_ready_(true);
}

bool EC20Component::data_cb_(const uint8_t *data, size_t len, void *arg) {
  auto *self = static_cast<EC20Component *>(arg);
  {
    std::lock_guard<std::mutex> lock(self->rx_mutex_);
    self->rx_buffer_.append(reinterpret_cast<const char *>(data), len);
    if (self->rx_buffer_.size() > 2048) {
      self->rx_buffer_.erase(0, self->rx_buffer_.size() - 2048);
    }
  }
  ESP_LOGV(TAG, "RX: %.*s", static_cast<int>(len), reinterpret_cast<const char *>(data));
  return true;
}

bool EC20Component::nmea_data_cb_(const uint8_t *data, size_t len, void *arg) {
  auto *self = static_cast<EC20Component *>(arg);
  std::lock_guard<std::mutex> lock(self->nmea_mutex_);
  if (self->nmea_rx_buffer_.size() + len > 4096) {
    return true;
  }
  self->nmea_rx_buffer_.append(reinterpret_cast<const char *>(data), len);
  return true;
}

void EC20Component::feed_nmea_(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = static_cast<char>(data[i]);
    if (c == '\n') {
      if (this->nmea_buffer_.size() >= 7 && this->nmea_buffer_[0] == '$') {
        this->parse_nmea_(this->nmea_buffer_);
      }
      this->nmea_buffer_.clear();
    } else if (c != '\r') {
      if (this->nmea_buffer_.size() < 120) {
        this->nmea_buffer_.push_back(c);
      } else {
        this->nmea_buffer_.clear();
      }
    }
  }
}

void EC20Component::publish_gnss_fix_(const std::string &fix) {
  if (this->gnss_fix_text_ == fix) {
    return;
  }
  this->gnss_fix_text_ = fix;
  if (this->gnss_fix_ != nullptr) {
    this->gnss_fix_->publish_state(fix);
  }
}

void EC20Component::clear_gnss_() {
  if (this->gnss_has_fix_) {
    this->gnss_has_fix_ = false;
    if (this->gnss_latitude_ != nullptr) {
      this->gnss_latitude_->publish_state(NAN);
    }
    if (this->gnss_longitude_ != nullptr) {
      this->gnss_longitude_->publish_state(NAN);
    }
    if (this->gnss_altitude_ != nullptr) {
      this->gnss_altitude_->publish_state(NAN);
    }
    if (this->gnss_speed_ != nullptr) {
      this->gnss_speed_->publish_state(NAN);
    }
  }
  this->publish_gnss_fix_("Không có fix");
}

void EC20Component::parse_nmea_(const std::string &line) {
  const std::vector<std::string> fields = split_nmea_fields(line);
  if (fields.size() < 3 || fields[0].size() < 6) {
    return;
  }
  const std::string kind = fields[0].substr(3, 3);

  if (kind == "GGA") {
    if (fields.size() < 10) {
      return;
    }
    int quality = 0;
    if (!fields[6].empty()) {
      quality = std::atoi(fields[6].c_str());
    }
    float satellites = 0.0f;
    if (parse_nmea_float(fields[7], satellites) && this->gnss_satellites_ != nullptr) {
      this->gnss_satellites_->publish_state(satellites);
    }
    float latitude = 0.0f;
    float longitude = 0.0f;
    if (quality > 0 && parse_nmea_coord(fields[2], fields[3], latitude) &&
        parse_nmea_coord(fields[4], fields[5], longitude)) {
      this->gnss_has_fix_ = true;
      if (this->gnss_latitude_ != nullptr) {
        this->gnss_latitude_->publish_state(latitude);
      }
      if (this->gnss_longitude_ != nullptr) {
        this->gnss_longitude_->publish_state(longitude);
      }
      float altitude = 0.0f;
      if (parse_nmea_float(fields[9], altitude) && this->gnss_altitude_ != nullptr) {
        this->gnss_altitude_->publish_state(altitude);
      }
      const char *fix = "GPS";
      if (quality == 2) {
        fix = "DGPS";
      } else if (quality == 4) {
        fix = "RTK";
      } else if (quality == 5) {
        fix = "RTK float";
      }
      this->publish_gnss_fix_(fix);
    } else {
      if (this->gnss_satellites_ != nullptr && fields[7].empty()) {
        this->gnss_satellites_->publish_state(0);
      }
      this->clear_gnss_();
    }
    return;
  }

  if (kind == "RMC") {
    if (fields.size() < 8) {
      return;
    }
    if (fields[2] == "A") {
      float latitude = 0.0f;
      float longitude = 0.0f;
      if (!parse_nmea_coord(fields[3], fields[4], latitude) ||
          !parse_nmea_coord(fields[5], fields[6], longitude)) {
        return;
      }
      this->gnss_has_fix_ = true;
      if (this->gnss_latitude_ != nullptr) {
        this->gnss_latitude_->publish_state(latitude);
      }
      if (this->gnss_longitude_ != nullptr) {
        this->gnss_longitude_->publish_state(longitude);
      }
      float knots = 0.0f;
      if (parse_nmea_float(fields[7], knots) && this->gnss_speed_ != nullptr) {
        this->gnss_speed_->publish_state(knots * 1.852f);
      }
      if (this->gnss_fix_text_.empty() || this->gnss_fix_text_ == "Không có fix") {
        this->publish_gnss_fix_("GPS");
      }
    } else if (fields[2] == "V") {
      this->clear_gnss_();
    }
  }
}

void EC20Component::ensure_listener_() {
  if (this->listen_fd_ >= 0) {
    return;
  }
  const uint32_t now = millis();
  if (now - this->last_listen_try_ < 5000) {
    return;
  }
  this->last_listen_try_ = now;

  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    ESP_LOGW(TAG, "TCP socket() failed: %d", errno);
    return;
  }
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(this->port_);
  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
    ESP_LOGW(TAG, "TCP bind port %u failed: %d", this->port_, errno);
    ::close(fd);
    return;
  }
  if (::listen(fd, 1) != 0) {
    ESP_LOGW(TAG, "TCP listen port %u failed: %d", this->port_, errno);
    ::close(fd);
    return;
  }
  ::fcntl(fd, F_SETFL, O_NONBLOCK);
  this->listen_fd_ = fd;
  ESP_LOGI(TAG, "TCP bridge listening on port %u", this->port_);
}

void EC20Component::close_client_() {
  if (this->client_fd_ >= 0) {
    ::close(this->client_fd_);
    this->client_fd_ = -1;
  }
  this->tcp_active_ = false;
  if (this->tcp_connected_ != nullptr) {
    this->tcp_connected_->publish_state(false);
  }
  {
    std::lock_guard<std::mutex> lock(this->rx_mutex_);
    this->rx_buffer_.clear();
  }
  ESP_LOGI(TAG, "TCP client disconnected");
}

void EC20Component::handle_tcp_() {
  if (this->listen_fd_ >= 0 && this->client_fd_ < 0) {
    struct sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    const int fd = ::accept(this->listen_fd_, reinterpret_cast<struct sockaddr *>(&peer), &peer_len);
    if (fd >= 0) {
      ::fcntl(fd, F_SETFL, O_NONBLOCK);
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      this->client_fd_ = fd;
      this->tcp_active_ = true;
      {
        std::lock_guard<std::mutex> lock(this->rx_mutex_);
        this->rx_buffer_.clear();
      }
      char peer_ip[32] = {};
      inet_ntoa_r(peer.sin_addr, peer_ip, sizeof(peer_ip));
      ESP_LOGI(TAG, "TCP client connected from %s:%u", peer_ip, ntohs(peer.sin_port));
      if (this->tcp_connected_ != nullptr) {
        this->tcp_connected_->publish_state(true);
      }
    }
  }

  if (this->client_fd_ < 0) {
    return;
  }

  std::string from_ec20;
  {
    std::lock_guard<std::mutex> lock(this->rx_mutex_);
    from_ec20.swap(this->rx_buffer_);
  }
  size_t offset = 0;
  while (offset < from_ec20.size()) {
    const ssize_t sent = ::send(this->client_fd_, from_ec20.data() + offset, from_ec20.size() - offset, 0);
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      this->close_client_();
      return;
    }
    if (sent == 0) {
      this->close_client_();
      return;
    }
    offset += static_cast<size_t>(sent);
  }

  uint8_t buffer[512];
  const ssize_t received = ::recv(this->client_fd_, buffer, sizeof(buffer), 0);
  if (received > 0) {
    if (!this->send_raw_(buffer, static_cast<size_t>(received))) {
      ESP_LOGW(TAG, "TCP -> EC20 write failed");
    }
  } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
    this->close_client_();
  }
}

bool EC20Component::send_raw_(const uint8_t *data, size_t len) {
  if (this->handle_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> tx_lock(this->tx_mutex_);
  return cdc_acm_host_data_tx_blocking(this->handle_, data, len, 1000) == ESP_OK;
}

void EC20Component::event_cb_(const cdc_acm_host_dev_event_data_t *event, void *arg) {
  auto *self = static_cast<EC20Component *>(arg);
  switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
      ESP_LOGW(TAG, "EC20 disconnected");
      self->disconnected_ = true;
      break;
    case CDC_ACM_HOST_ERROR:
      ESP_LOGW(TAG, "EC20 error: %d", event->data.error);
      break;
    default:
      break;
  }
}

void EC20Component::nmea_event_cb_(const cdc_acm_host_dev_event_data_t *event, void *arg) {
  auto *self = static_cast<EC20Component *>(arg);
  if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
    self->nmea_disconnected_ = true;
  }
}

}  // namespace esphome::ec20
