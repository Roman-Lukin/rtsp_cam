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
// Using 1280x960 binning mode - native OV5647 resolution
// 1080p has buffer overflow issues with H.264 HW encoder
#define VIDEO_WIDTH  1280
#define VIDEO_HEIGHT 960
#define VIDEO_FPS    25
#else
#define VIDEO_WIDTH  320
#define VIDEO_HEIGHT 240
#define VIDEO_FPS    10
#endif

#ifdef __cplusplus
}
#endif
