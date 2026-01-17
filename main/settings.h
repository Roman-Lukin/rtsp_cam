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
// Testing 1280x960 (4:3, 16-byte aligned) - same sensor timing as 720p
#define VIDEO_WIDTH  1280
#define VIDEO_HEIGHT 960
#define VIDEO_FPS    30
#else
#define VIDEO_WIDTH  320
#define VIDEO_HEIGHT 240
#define VIDEO_FPS    10
#endif

#ifdef __cplusplus
}
#endif
