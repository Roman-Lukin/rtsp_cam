/*
 * IMX219 Camera Sensor Helper for ESP32-P4
 * 
 * IMX219 is the Sony 8MP sensor used in Raspberry Pi Camera Module v2.
 * It is pin-compatible with OV5647 (RPi Camera v1) modules.
 * 
 * Key differences from OV5647:
 * - I2C address: 0x10 (vs 0x36 for OV5647)
 * - Sensor ID: 0x0219
 * - Higher resolution: 3280x2464 (8MP) vs 2592x1944 (5MP)
 * - Bayer pattern: RGGB (vs GBRG for OV5647)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// IMX219 I2C address (7-bit)
#define IMX219_I2C_ADDR     0x10

// IMX219 Chip ID registers
#define IMX219_REG_CHIP_ID_H    0x0000
#define IMX219_REG_CHIP_ID_L    0x0001
#define IMX219_CHIP_ID          0x0219

// IMX219 Mode registers
#define IMX219_REG_MODE_SELECT      0x0100
#define IMX219_REG_SOFTWARE_RESET   0x0103

// Frame format registers
#define IMX219_REG_FRAME_LEN_H      0x0160
#define IMX219_REG_FRAME_LEN_L      0x0161
#define IMX219_REG_LINE_LEN_H       0x0162
#define IMX219_REG_LINE_LEN_L       0x0163

// Exposure registers (coarse integration time)
#define IMX219_REG_EXPOSURE_H       0x015A
#define IMX219_REG_EXPOSURE_L       0x015B

// Analog gain register
#define IMX219_REG_ANALOG_GAIN      0x0157

// Digital gain registers
#define IMX219_REG_DIG_GAIN_GR_H    0x0158
#define IMX219_REG_DIG_GAIN_GR_L    0x0159
#define IMX219_REG_DIG_GAIN_R_H     0x0180
#define IMX219_REG_DIG_GAIN_R_L     0x0181
#define IMX219_REG_DIG_GAIN_B_H     0x0182
#define IMX219_REG_DIG_GAIN_B_L     0x0183
#define IMX219_REG_DIG_GAIN_GB_H    0x0184
#define IMX219_REG_DIG_GAIN_GB_L    0x0185

// Test pattern register
#define IMX219_REG_TEST_PATTERN_H   0x0600
#define IMX219_REG_TEST_PATTERN_L   0x0601

// Image orientation
#define IMX219_REG_ORIENTATION      0x0172

// Output size
#define IMX219_REG_X_OUTPUT_SIZE_H  0x016C
#define IMX219_REG_X_OUTPUT_SIZE_L  0x016D
#define IMX219_REG_Y_OUTPUT_SIZE_H  0x016E
#define IMX219_REG_Y_OUTPUT_SIZE_L  0x016F

// Binning mode
#define IMX219_REG_BINNING_MODE_H   0x0174
#define IMX219_REG_BINNING_MODE_V   0x0175

// CSI data format
#define IMX219_REG_CSI_DATA_FORMAT_H    0x018C
#define IMX219_REG_CSI_DATA_FORMAT_L    0x018D

// CSI lane mode (0x01 = 2 lanes, 0x03 = 4 lanes)
#define IMX219_REG_CSI_LANE_MODE    0x0114

/**
 * @brief Initialize the IMX219 helper with the I2C bus handle.
 * 
 * @param bus_handle The I2C master bus handle.
 */
void imx219_helper_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Read a value from an IMX219 register via I2C.
 * 
 * @param reg The register address (16-bit).
 * @param value Pointer to store the read value (8-bit).
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t imx219_read_reg(uint16_t reg, uint8_t *value);

/**
 * @brief Write a value to an IMX219 register via I2C.
 * 
 * @param reg The register address (16-bit).
 * @param value The value to write (8-bit).
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t imx219_write_reg(uint16_t reg, uint8_t value);

/**
 * @brief Read IMX219 chip ID to verify sensor presence.
 * 
 * @param chip_id Pointer to store the chip ID (should be 0x0219).
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t imx219_get_chip_id(uint16_t *chip_id);

/**
 * @brief Perform software reset of the IMX219 sensor.
 * 
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t imx219_soft_reset(void);

/**
 * @brief Set IMX219 streaming mode.
 * 
 * @param enable true to start streaming, false to stop.
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t imx219_set_stream(bool enable);

/**
 * @brief Set IMX219 exposure (coarse integration time).
 * 
 * @param exposure_lines Number of exposure lines.
 *                       Range: 1 to (frame_length - 4)
 */
void imx219_set_exposure(uint16_t exposure_lines);

/**
 * @brief Get current IMX219 exposure setting.
 * 
 * @return uint16_t Current exposure in lines.
 */
uint16_t imx219_get_exposure(void);

/**
 * @brief Set IMX219 analog gain.
 * 
 * @param gain Analog gain value (0-232).
 *             Actual gain = 256 / (256 - gain)
 *             0 = 1x, 170 = ~3x, 232 = ~10.67x
 */
void imx219_set_analog_gain(uint8_t gain);

/**
 * @brief Get current IMX219 analog gain.
 * 
 * @return uint8_t Current analog gain value.
 */
uint8_t imx219_get_analog_gain(void);

/**
 * @brief Set IMX219 digital gain for white balance.
 * 
 * @param red_gain   Red channel gain (256 = 1x, 512 = 2x).
 * @param green_gain Green channel gain.
 * @param blue_gain  Blue channel gain.
 */
void imx219_set_digital_gain(uint16_t red_gain, uint16_t green_gain, uint16_t blue_gain);

/**
 * @brief Set IMX219 test pattern.
 * 
 * @param pattern Pattern mode:
 *                0x0000: Disabled
 *                0x0001: Solid color
 *                0x0002: 100% color bars
 *                0x0003: Fade to grey color bars
 *                0x0004: PN9
 */
void imx219_set_test_pattern(uint16_t pattern);

/**
 * @brief Set IMX219 frame length (VTS - Vertical Total Size).
 * 
 * @param frame_length Frame length in lines.
 */
void imx219_set_frame_length(uint16_t frame_length);

/**
 * @brief Get current IMX219 frame length.
 * 
 * @return uint16_t Current frame length in lines.
 */
uint16_t imx219_get_frame_length(void);

/**
 * @brief Set IMX219 image orientation (mirror/flip).
 * 
 * @param h_mirror Horizontal mirror (true = enabled).
 * @param v_flip   Vertical flip (true = enabled).
 */
void imx219_set_orientation(bool h_mirror, bool v_flip);

/**
 * @brief Configure IMX219 for a specific resolution mode.
 *        This writes the full register configuration.
 * 
 * @param mode Resolution mode:
 *             0: 1920x1080 @ 30fps
 *             1: 1280x720 @ 60fps
 *             2: 640x480 @ 90fps
 *             3: 3280x2464 @ 15fps (full resolution)
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t imx219_set_mode(uint8_t mode);

/**
 * @brief Initialize IMX219 with default settings for 1080p @ 30fps.
 * 
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t imx219_init_default(void);

/**
 * @brief Debug: Print current IMX219 register status.
 */
void imx219_debug_status(void);

#ifdef __cplusplus
}
#endif
