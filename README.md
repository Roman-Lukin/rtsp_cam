# RTSP Camera Project

This project implements an RTSP IP Camera on ESP32-P4.

## Supported Sensors

### OV5647 (original)
- 5MP rolling shutter sensor
- I2C address: 0x36
- Fully working with RTSP streaming

### IMX219 (NEW - in progress)
- 8MP rolling shutter Sony sensor (Raspberry Pi Camera Module v2 compatible)
- I2C address: 0x10
- Pin-compatible with OV5647 (same FPC connector)
- **Status: Driver implemented, streaming pipeline issues being debugged**

## IMX219 Implementation Details

### What was done:

1. **Full IMX219 driver created** (`components/imx219/`)
   - Register definitions based on Sony datasheet
   - Support for multiple resolutions: 1920x1080, 1280x720, 640x480
   - MIPI CSI-2 2-lane configuration
   - RAW10 Bayer output format

2. **Power control**
   - IMX219 uses **active HIGH** power enable (GPIO48)
   - Different from OV5647 which uses active LOW (PWDN logic)
   - Power sequence: GPIO48 HIGH → 100ms delay → sensor ready

3. **I2C communication**
   - Chip ID verification: reads 0x0219 from registers 0x0000-0x0001
   - 16-bit register addresses (big-endian)
   - Successfully detected at address 0x10

4. **ISP/IPA configuration**
   - Created `imx219_default.json` for Image Processing Algorithm
   - Added Kconfig option `CONFIG_CAMERA_IMX219_DEFAULT_IPA_JSON_CONFIGURATION_FILE`
   - Registered via `project_include.cmake`

5. **H.264 encoder buffer fix**
   - Modified `H264_ENC_MIN_COMPRESS_RATIO` from 2 to 1
   - Required for high-bitrate 720p encoding
   - File: `managed_components/espressif__gmf_video/esp_gmf_video_enc.c`

### Current issues being investigated:

1. **Frame acquisition problem**
   - First frame is successfully captured (~800KB keyframe)
   - Subsequent frames don't arrive to capture sink queue
   - `esp_capture_sink_acquire_frame()` blocks or returns -4 (NOT_FOUND)
   - Pipeline negotiation succeeds: `o_uyy_e_vyy` → H.264 @ 1280x720 30fps

2. **Possible causes**
   - `share_q` / `video_q` configuration in esp_capture
   - Timing between `esp_capture_sink_enable` and `esp_capture_start`
   - GMF pipeline data flow after first frame

### Files created/modified:

```
components/imx219/
├── CMakeLists.txt
├── Kconfig
├── imx219.c                    # Main driver
├── include/
│   ├── imx219.h               # Public API
│   └── imx219_types.h         # Type definitions
├── private_include/
│   ├── imx219_regs.h          # Register definitions
│   └── imx219_settings.h      # Resolution configurations
├── cfg/
│   └── imx219_default.json    # IPA configuration
└── project_include.cmake       # Build system integration

main/main.cpp                   # Auto-detection OV5647/IMX219
main/settings.h                 # Video resolution settings
sdkconfig                       # IMX219 enabled
sdkconfig.defaults             # Default configuration
```

### Hardware setup tested:
- ESP32-P4 custom board with 32MB PSRAM
- Ethernet connectivity
- IMX219 sensor via 15-pin FPC cable
- GPIO48 = Camera power enable
- GPIO7 = I2C SDA, GPIO8 = I2C SCL

## Setup

1. Copy the following components from `esp-webrtc-solution/components` to `components/`:
   - `av_render`
   - `codec_board`
   - `media_lib_sal`
   - `webrtc_utils` (optional, if needed for time sync)

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

### Video settings (main/settings.h)
```cpp
#define VIDEO_WIDTH  1280
#define VIDEO_HEIGHT 720
#define VIDEO_FPS    30
```

## Troubleshooting

### IMX219 not detected
1. Check FPC cable connection (replace if damaged)
2. Verify GPIO48 is set HIGH before I2C scan
3. Check I2C pull-ups on SDA/SCL lines

### No video stream
1. Check ISP/IPA JSON is registered (log: "IPA configuration loaded")
2. Verify MIPI CSI lane configuration
3. Check H.264 encoder buffer size (ratio=1 for 720p)

