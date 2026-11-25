#include "ec20.h"

#include "esphome/core/log.h"

#include <usb/usb_host.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>

namespace esphome::ec20 {

static const char *const TAG = "ec20";
static constexpr uint16_t EC20_VID = 0x2C7C;
static constexpr uint16_t EC20_PID = 0x0125;

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

void EC20Component::test_at() { this->force_at_ = true; }

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
        ESP_LOGW(TAG, "No OK on interface %u, trying next", this->at_interfaces_[this->at_index_]);
        this->close_device_();
        this->at_index_ = (this->at_index_ + 1) % this->at_interfaces_.size();
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    } else {
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
    return false;
  }

  ESP_LOGI(TAG, "Opened EC20 interface %u", interface);
  return true;
}

void EC20Component::close_device_() {
  if (this->handle_ != nullptr) {
    cdc_acm_host_close(this->handle_);
    this->handle_ = nullptr;
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
  {
    std::lock_guard<std::mutex> lock(this->pending_mutex_);
    this->pending_response_ = std::move(clean);
    this->has_pending_response_ = true;
  }
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
  ESP_LOGD(TAG, "RX: %.*s", static_cast<int>(len), reinterpret_cast<const char *>(data));
  return true;
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
    if (sent <= 0) {
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

}  // namespace esphome::ec20
