/*
 * SPDX-FileCopyrightText: 2024-2025 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * IMX219 Camera Sensor Driver for ESP32-P4
 * Sony IMX219 8MP CMOS sensor (Raspberry Pi Camera v2 compatible)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_cam_sensor_types.h"

// IMX219 can have different I2C addresses depending on the module
// 0x10 is the default, but some modules use 0x1A
#define IMX219_SCCB_ADDR       0x10
#define IMX219_SCCB_ADDR_ALT   0x1A
#define IMX219_PID             0x0219
#define IMX219_SENSOR_NAME     "IMX219"

/**
 * @brief Power on camera sensor device and detect the IMX219 connected to the designated SCCB bus.
 *
 * @param[in] config Configuration related to device power-on and detection.
 * @return
 *      - Camera device handle on success, otherwise, failed.
 */
esp_cam_sensor_device_t *imx219_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif
