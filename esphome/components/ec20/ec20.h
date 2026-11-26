#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
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
  void set_gnss_latitude_sensor(sensor::Sensor *sensor) { this->gnss_latitude_ = sensor; }
  void set_gnss_longitude_sensor(sensor::Sensor *sensor) { this->gnss_longitude_ = sensor; }
  void set_gnss_altitude_sensor(sensor::Sensor *sensor) { this->gnss_altitude_ = sensor; }
  void set_gnss_speed_sensor(sensor::Sensor *sensor) { this->gnss_speed_ = sensor; }
  void set_gnss_satellites_sensor(sensor::Sensor *sensor) { this->gnss_satellites_ = sensor; }
  void set_gnss_fix_sensor(text_sensor::TextSensor *sensor) { this->gnss_fix_ = sensor; }
  void set_usb_connected_sensor(binary_sensor::BinarySensor *sensor) { this->usb_connected_ = sensor; }
  void set_tcp_connected_sensor(binary_sensor::BinarySensor *sensor) { this->tcp_connected_ = sensor; }
  void set_reconnect_sensor(sensor::Sensor *sensor) { this->reconnects_ = sensor; }
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
  void ensure_nmea_port_();
  void close_nmea_();
  void feed_nmea_(const uint8_t *data, size_t len);
  void parse_nmea_(const std::string &line);
  void publish_gnss_fix_(const std::string &fix);
  void clear_gnss_();

  static bool data_cb_(const uint8_t *data, size_t len, void *arg);
  static bool nmea_data_cb_(const uint8_t *data, size_t len, void *arg);
  static void event_cb_(const cdc_acm_host_dev_event_data_t *event, void *arg);
  static void nmea_event_cb_(const cdc_acm_host_dev_event_data_t *event, void *arg);

  cdc_acm_dev_hdl_t handle_{nullptr};
  cdc_acm_dev_hdl_t nmea_handle_{nullptr};
  text_sensor::TextSensor *at_response_sensor_{nullptr};
  sensor::Sensor *gnss_latitude_{nullptr};
  sensor::Sensor *gnss_longitude_{nullptr};
  sensor::Sensor *gnss_altitude_{nullptr};
  sensor::Sensor *gnss_speed_{nullptr};
  sensor::Sensor *gnss_satellites_{nullptr};
  text_sensor::TextSensor *gnss_fix_{nullptr};
  binary_sensor::BinarySensor *usb_connected_{nullptr};
  binary_sensor::BinarySensor *tcp_connected_{nullptr};
  sensor::Sensor *reconnects_{nullptr};
  TaskHandle_t task_handle_{nullptr};

  std::atomic<bool> force_at_{false};
  std::atomic<bool> disconnected_{false};
  std::atomic<bool> nmea_disconnected_{false};
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

  std::mutex nmea_mutex_;
  std::string nmea_rx_buffer_;
  std::string nmea_buffer_;
  std::string gnss_fix_text_;
  bool gnss_has_fix_{false};
  bool gps_init_attempted_{false};
  bool ever_opened_{false};
  bool at_ready_{false};
  uint32_t nmea_retry_at_{0};
  uint32_t reconnect_count_{0};
};

}  // namespace esphome::ec20
