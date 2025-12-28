# RTSP Camera Project

This project implements an RTSP IP Camera on ESP32-P4.

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
