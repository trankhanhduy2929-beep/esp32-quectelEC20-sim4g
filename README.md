# ESP32-S3 + Quectel EC20 SIM 4G

Cầu nối **USB CDC-ACM → TCP serial** cho module **Quectel EC20/EC2x** trên ESP32-S3, dùng với Home Assistant và add-on **Calling Messenger Hub**.

- Mở trực tiếp **interface 2** (AT port, vendor-specific `0xFF`) của EC20 bằng `usb_host_cdc_acm` / `cdc_acm_host_open_vendor_specific`.
- Gửi `SET_LINE_CODING` 115200 8N1 và `SET_CONTROL_LINE_STATE` bật **DTR/RTS**.
- Gửi AT kết thúc bằng `\r\n`.
- TCP bridge thô tại cổng **9000**, để add-on Calling Messenger Hub dùng `transport: tcp`.
- Có sẵn ESPHome cho ESP32-S3 N16R8, web UI, OTA, WiFi AP fallback, Bluetooth proxy, loa I2S MAX98357A và thông báo IP qua TTS (tuỳ chọn).
- Giám sát AT/USB, lưu lỗi AT gần nhất và tự ngắt/bật nguồn EC20 qua GPIO18 sau 5 phút không khỏe (có công tắc bật/tắt và chống lặp 15 phút).

## Kiến trúc

```
Quectel EC20 (USB) ── USB-C native ── ESP32-S3
                                      │
                                      ├── TCP :9000  ← Calling Messenger Hub (transport: tcp)
                                      ├── ESPHome API / OTA / web UI :80
                                      └── I2S ── MAX98357A ── loa
```

Add-on **Calling Messenger Hub** gửi lệnh AT qua TCP 9000; ESP32 chuyển nguyên byte sang interface 2 của EC20 và trả response về TCP. Add-on tự quản lý SIM/SMS/cuộc gọi.

## Phần cứng

| Thành phần | Ghi chú |
|---|---|
| ESP32-S3 DevKitC-1 N16R8 | 16 MB flash, 8 MB PSRAM, USB native + USB UART |
| Quectel EC20/EC2x | USB 4G modem, SIM + anten |
| MAX98357A + loa | I2S: BCLK GPIO15, LRC GPIO16, DIN GPIO17 |
| Nguồn EC20 | **5V/2A**, nên dùng USB hub có nguồn; GND chung với ESP32 |

> EC20 phải cắm vào **cổng USB native** của ESP32-S3 (cổng còn lại là USB UART/COM dùng để nạp firmware). EC20 có thể cần nguồn ngoài; nếu USB liên tục reset, kiểm tra nguồn trước.

> Tự phục hồi nguồn cần MOSFET/relay hoặc mạch enable 5V trên GPIO18; nếu chưa có phần cứng này, tắt công tắc **Tự phục hồi EC20** trên web UI.

## Cài đặt ESPHome

Yêu cầu Python 3.12+.

```bash
git clone https://github.com/trankhanhduy2929-beep/esp32-quectelEC20-sim4g.git
cd esp32-quectelEC20-sim4g

python -m venv .venv
# Windows
.venv\Scripts\activate
# Linux/macOS
source .venv/bin/activate

pip install esphome==2026.10.2
```

Windows có dấu cách trong tên user có thể gặp lỗi ESP-IDF; đặt thêm:

```powershell
$env:ESPHOME_ESP_IDF_PREFIX="D:\esphome-idf"
```

## Cấu hình

File chính: `esphome/esp32-s3-ec20.yaml`.

Các substitution đầu file:

```yaml
substitutions:
  device_name: esp32-s3-speaker
  friendly_name: ESP32-S3 Speaker
  media_player_name: Loa ESP32-S3
  ap_ssid: "EC20 SIM SPEAKER ESP32"
  ap_password: "12345678"
  tts_entity: tts.edge_tts
  tts_media_player: media_player.esp32_s3_speaker_loa_esp32_s3
```

- `device_name` / `friendly_name`: tên thiết bị trong ESPHome/HA.
- `ap_ssid` / `ap_password`: WiFi AP dự phòng khi chưa có WiFi.
- `tts_entity` / `tts_media_player`: dùng cho thông báo IP sau khi kết nối WiFi. Nếu không dùng, xoá block `script: announce_wifi` và `wifi.on_connect`.

Component custom nằm ở `esphome/components/ec20/`; YAML tự nạp qua:

```yaml
external_components:
  - source:
      type: local
      path: components
```

## Build và nạp lần đầu (USB)

Cắm USB UART của ESP32 vào máy tính, xác định cổng COM (Windows) hoặc `/dev/ttyUSB0` (Linux).

```bash
cd esphome
esphome config esp32-s3-ec20.yaml
esphome run esp32-s3-ec20.yaml --device COM6
```

Linux/macOS:

```bash
esphome run esp32-s3-ec20.yaml --device /dev/ttyUSB0
```

Nếu chỉ có file factory:

```bash
esptool.py --chip esp32s3 --port COM6 write_flash 0x0 \
  .esphome/build/esp32-s3-speaker/build/firmware.factory.bin
```

## OTA qua WiFi

Sau khi thiết bị vào WiFi, dùng IP thực tế:

```bash
esphome run esp32-s3-ec20.yaml --device 192.168.1.97
```

Hoặc vào web UI của thiết bị, mục **OTA/Update**, hoặc dùng ESPHome dashboard.

## Cấu hình WiFi lần đầu

Nếu chưa có WiFi đã lưu, thiết bị phát AP:

- SSID: `EC20 SIM SPEAKER ESP32`
- Password: `12345678`
- Web UI: `http://192.168.4.1`

Vào nhóm **WiFi** trên web UI:

- Xem **WiFi xung quanh** để chọn mạng; nhấn một mạng để điền SSID, nhập mật khẩu rồi bấm **Kết nối WiFi**.
- Bấm **Quét WiFi** để cập nhật danh sách. Trong AP mode danh sách có thể trống; vẫn có thể nhập SSID thủ công.
- Thiết bị lưu WiFi vào NVS và tự kết nối lại sau reboot/OTA. Firmware dùng DHCP, không cấu hình IP tĩnh.
- Khi mất WiFi, ESPHome tự thử lại liên tục; nếu mất hơn 15 phút, watchdog tự reboot để reset WiFi. Khi mạng có lại, thiết bị tự vào lại LAN/HA mà không cần thao tác.
- Thông báo TTS IP chỉ phát lần đầu sau mỗi lần boot, tránh lặp khi WiFi chập chờn.
- Muốn xoá WiFi đã lưu, bấm **Xóa WiFi và khởi động lại**; thiết bị sẽ phát AP lại sau 120 giây nếu không kết nối được.

> Nếu AP không hiện, giữ nút **BOOT** 5 giây để factory reset rồi thử lại.

## Home Assistant

1. Settings → Devices & Services → Add Integration → **ESPHome**.
2. Nhập IP thiết bị (hoặc để tự phát hiện). Firmware v0.1.13 không dùng encryption key nên không cần nhập khóa.
3. Các entity có sẵn: media player, WiFi signal, uptime, IP/SSID/MAC, các nút WiFi/restart, **EC20 Test AT**, text sensor **EC20 AT Response**.
4. Firmware v0.1.8 bổ sung nhóm **GNSS** (vĩ độ, kinh độ, độ cao, tốc độ, vệ tinh, trạng thái fix), modem health (**EC20 USB**, **Cầu TCP**, **Mở lại modem**) và nút **Reset EC20**; v0.1.10 bổ sung quét/chọn WiFi trên web UI; v0.1.11 thêm watchdog tự phục hồi WiFi; v0.1.12 thêm giám sát AT và tự phục hồi EC20; v0.1.13 tắt API encryption.
5. GNSS cần anten GPS gắn vào EC20; không có anten sẽ báo `0` vệ tinh và `Không có fix`.

## Bảo mật

Từ firmware v0.1.13, API ESPHome không dùng encryption key để thuận tiện thêm vào Home Assistant. Chỉ chạy thiết bị trong LAN tin cậy và không mở cổng API/OTA ra Internet. Nếu cần bật lại Noise encryption, thêm khóa vào `secrets.yaml` và cấu hình lại tích hợp ESPHome trong Home Assistant:

```yaml
api:
  encryption:
    key: !secret api_encryption_key
```

## Reset EC20 từ xa (tùy chọn)

Nút **Reset EC20** điều khiển GPIO18 để cắt nguồn 5V của EC20 trong 5 giây rồi cấp lại:

- Dùng MOSFET logic-level hoặc module relay chịu được dòng 5V/2A của EC20.
- Nối GPIO18 → chân điều khiển MOSFET/relay, GND ESP32 nối chung với GND nguồn EC20.
- Không cấp nguồn EC20 trực tiếp từ GPIO.
- Mặc định logic active-high, switch nguồn ở trạng thái ON khi khởi động.
- Phần cứng này chưa được kiểm thử; cần đo dòng và kiểm tra cách ly trước khi dùng.

## Calling Messenger Hub (TCP 9000)

Trong add-on **Calling Messenger Hub**:

1. Mở **SIM/4G → Thêm SIM**.
2. Chọn:
   - **Transport:** `TCP`
   - **Host:** IP ESP32 (ví dụ `192.168.1.64`)
   - **Port:** `9000`
   - **Driver:** `Quectel EC20/EC2x`
   - **Baudrate:** `115200`
3. Lưu. Add-on sẽ gửi `AT`, `ATE0`, `AT+CPIN?`, `AT+CSQ`, `AT+COPS?`... qua cầu TCP.

Kiểm tra nhanh cầu TCP từ máy tính:

```bash
python - <<'PY'
import socket
s = socket.create_connection(("192.168.1.64", 9000), timeout=5)
s.sendall(b"AT\r\n")
print(s.recv(1024))
PY
```

Kết quả đúng (đã test với EC20):

```
b'AT\r\r\nOK\r\n'
```

Một số lệnh kiểm tra SIM:

```
AT+CPIN?
AT+CSQ
AT+CREG?
AT+COPS?
AT+QCCID
AT+CGSN
```

## GitHub Actions

Workflow `.github/workflows/build.yml`:

- Chạy khi push lên `main`, khi tạo tag `v*`, hoặc bấm **Run workflow**.
- Cài ESPHome 2026.10.2, build firmware.
- Upload artifact `firmware` gồm `firmware.factory.bin` và `firmware.ota.bin`.
- Với tag `v*`, tạo GitHub Release kèm firmware.

Tải artifact trong tab **Actions → build → Artifacts**, hoặc vào **Releases** nếu tạo tag.

Nạp file `firmware.factory.bin` bằng esptool như mục “Build và nạp lần đầu”.

## Troubleshooting

### EC20 không nhận / reset liên tục
- Dùng **USB hub có nguồn** hoặc nguồn 5V/2A riêng; nối GND chung.
- EC20 phải ở cổng **USB native** của ESP32-S3.
- Kiểm tra LED nguồn trên EC20.

### TCP 9000 không kết nối
- Kiểm tra IP ESP32 trong web UI/HA.
- ESP32 và máy chạy add-on phải cùng mạng LAN (hoặc route được).
- Xem log ESPHome: dòng `TCP bridge listening on port 9000`.
- Thử `nc <ip> 9000` hoặc đoạn Python ở trên.

### AT chỉ echo, không có OK
- Kiểm tra SIM đã lắp, không khoá PIN.
- Thử EC20 trên PC bằng terminal serial để xác nhận firmware module.
- Một số firmware EC20 chưa bật AT trên USB composition hiện tại; cần đổi USB composition hoặc dùng bản firmware khác.
- Cầu TCP vẫn hoạt động; add-on sẽ báo lỗi AT cụ thể.

### Build Windows lỗi ESP-IDF
- Đặt `ESPHOME_ESP_IDF_PREFIX` như trên.
- Tránh đường dẫn có dấu cách; dùng `D:\esp32-s3-speaker` hoặc `D:\esp32-quectelEC20-sim4g`.

### Lỗi component `usb_host_cdc_acm`
- Lần build đầu cần Internet để tải component từ ESP Component Registry.
- Nếu mạng chặn, chạy lại build hoặc dùng mirror nội bộ.

## License

MIT. Xem `LICENSE`.
