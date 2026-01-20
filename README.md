# RTSP Camera Project

This project implements an RTSP IP Camera on ESP32-P4.

## Features

- **H.264 hardware encoding** at up to 1280x960 @ 30fps
- **Web interface** for camera settings adjustment
- **Dual sensor support**: OV5647 and IMX219 with auto-detection
- **NVS settings persistence** - settings survive reboots
- **Real-time exposure/gain control** via HTTP API

## Supported Sensors

### OV5647
- 5MP rolling shutter sensor
- I2C address: 0x36
- Fully working with RTSP streaming

### IMX219 ✅ WORKING
- 8MP rolling shutter Sony sensor (Raspberry Pi Camera Module v2 compatible)
- I2C address: 0x10
- Pin-compatible with OV5647 (same FPC connector)
- Tested resolution: 1280x960 @ 30fps H.264 output

## IMX219 Implementation Details

### Features implemented:

1. **Full IMX219 driver** (`main/imx219_helper.c/h`, `main/imx219_camera.cpp`)
   - Direct I2C register access bypassing V4L2 (better control)
   - Group Hold (register 0x0104) for atomic exposure/gain updates
   - Prevents MIPI stream freezing during parameter changes

2. **Exposure control**
   - Range: 4-1760 lines (VTS=1764 for 1280x960)
   - Atomic updates during streaming via Group Hold mechanism

3. **Analog gain control**
   - Range: 0-232
   - Real gain = 256/(256-val): 0=1x, 128=2x, 192=4x, 224=8x, 232=10.67x

4. **Camera interface abstraction** (`main/camera_interface.h/cpp`)
   - `ICameraInterface` abstract base class
   - Auto-detection of IMX219 vs OV5647
   - Factory pattern for sensor instantiation

### Hardware configuration:
- ESP32-P4 with 32MB PSRAM
- GPIO48 = Camera power enable (HIGH for IMX219, LOW for OV5647)
- GPIO7 = I2C SDA, GPIO8 = I2C SCL
- 2-lane MIPI CSI

## Setup

1. Install ESP-IDF v5.5+ and set ESP32-P4 as target

2. Build and Flash:
   ```bash
   idf.py set-target esp32p4
   idf.py build
   idf.py flash monitor
   ```

## Configuration

### Sensor selection
The firmware auto-detects the sensor by scanning I2C bus:
- 0x36 → OV5647
- 0x10 → IMX219

### Video settings ([main/settings.h](main/settings.h))
```cpp
#define VIDEO_WIDTH  1280
#define VIDEO_HEIGHT 960
#define VIDEO_FPS    30
```

### Camera settings (via web interface)
Access `http://<device-ip>/` for web configuration:
- **Exposure**: 4-1760 lines (for 1280x960 mode)
- **Gain**: 0-232 (analog gain)
- **Denoise**: ISP bilateral filter
- **White balance**: Manual or auto
- **Bitrate**: H.264 encoder bitrate (kbps)

## Web API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Camera settings web UI |
| `/api/settings` | GET | Get current settings (JSON) |
| `/api/settings` | POST | Update settings (JSON) |
| `/api/defaults` | POST | Reset to default settings |
| `/stream` | GET | MJPEG stream (if enabled) |

## Project Structure

```
main/
├── main.cpp              # Application entry, capture pipeline
├── app_params.cpp/h      # Settings management, NVS persistence
├── camera_interface.cpp/h # Abstract camera interface
├── imx219_camera.cpp/h   # IMX219 implementation
├── imx219_helper.c/h     # IMX219 low-level I2C control
├── ov5647_camera.cpp/h   # OV5647 implementation  
├── ov5647_helper.c/h     # OV5647 low-level control
├── http_server.c/h       # Web interface and API
├── rtsp_server.c/h       # RTSP streaming server
├── network.c/h           # Network initialization
└── settings.h            # Video resolution/FPS config
```

## Troubleshooting

### IMX219 not detected
1. Check FPC cable connection (15-pin, contacts facing away from board)
2. Verify GPIO48 is set HIGH before I2C scan
3. Check I2C pull-ups on SDA/SCL lines

### Dark image
1. Increase exposure value (default: 1500 lines)
2. Increase gain value (default: 200, ~5x amplification)
3. Check lighting conditions

### Stream freezes
This was caused by MIPI timing issues during register writes.
Solution: Group Hold mechanism (register 0x0104) buffers changes
and applies them atomically at frame boundary.