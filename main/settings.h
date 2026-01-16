#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_IDF_TARGET_ESP32P4
#define TEST_BOARD_NAME "ESP32_P4_RTSP_CAM"
#else
#define TEST_BOARD_NAME "S3_Korvo_V2"
#endif

#if CONFIG_IDF_TARGET_ESP32P4
// Using RAW10 1920x1080 mode (camera runs at 30fps)
#define VIDEO_WIDTH  1920
#define VIDEO_HEIGHT 1080
#define VIDEO_FPS    24
#else
#define VIDEO_WIDTH  320
#define VIDEO_HEIGHT 240
#define VIDEO_FPS    10
#endif

#ifdef __cplusplus
}
#endif
