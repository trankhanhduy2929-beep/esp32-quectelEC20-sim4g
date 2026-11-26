#pragma once

#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <usb/cdc_acm_host.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <lwip/sockets.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome::ec20 {

class EC20Component : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  void set_at_response_sensor(text_sensor::TextSensor *sensor) { this->at_response_sensor_ = sensor; }
  void set_port(uint16_t port) { this->port_ = port; }
  void test_at();

 protected:
  static void task_fn(void *arg);
  void task_loop_();
  bool open_device_();
  void close_device_();
  bool configure_device_();
  bool send_at_();
  bool response_has_ok_();
  void store_response_();
  void ensure_listener_();
  void handle_tcp_();
  void close_client_();
  bool send_raw_(const uint8_t *data, size_t len);

  static bool data_cb_(const uint8_t *data, size_t len, void *arg);
  static void event_cb_(const cdc_acm_host_dev_event_data_t *event, void *arg);

  cdc_acm_dev_hdl_t handle_{nullptr};
  text_sensor::TextSensor *at_response_sensor_{nullptr};
  TaskHandle_t task_handle_{nullptr};

  std::atomic<bool> force_at_{false};
  std::atomic<bool> disconnected_{false};
  std::atomic<bool> has_pending_response_{false};

  std::mutex rx_mutex_;
  std::string rx_buffer_;
  std::mutex pending_mutex_;
  std::string pending_response_;
  std::mutex tx_mutex_;

  uint32_t last_at_{0};
  int keepalive_failures_{0};
  std::vector<uint8_t> at_interfaces_{2, 3, 0, 1, 4};
  size_t at_index_{0};
  uint16_t port_{9000};
  int listen_fd_{-1};
  int client_fd_{-1};
  uint32_t last_listen_try_{0};
  std::atomic<bool> tcp_active_{false};
};

}  // namespace esphome::ec20
