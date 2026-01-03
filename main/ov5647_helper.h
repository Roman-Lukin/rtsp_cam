#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/**
 * @brief Initialize the OV5647 helper with the I2C bus handle.
 * 
 * @param bus_handle The I2C master bus handle.
 */
void ov5647_helper_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Read a value from an OV5647 register via I2C (SCCB).
 * 
 * @param reg The register address (16-bit).
 * @param value Pointer to store the read value (8-bit).
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t ov5647_read_reg(uint16_t reg, uint8_t *value);

/**
 * @brief Write a value to an OV5647 register via I2C (SCCB).
 * 
 * @param reg The register address (16-bit).
 * @param value The value to write (8-bit).
 * @return esp_err_t ESP_OK on success, or an error code.
 */
esp_err_t ov5647_write_reg(uint16_t reg, uint8_t value);

/**
 * @brief Set OV5647 exposure directly (registers 0x3500-0x3502).
 * 
 * @param exposure_lines Number of exposure lines (higher = brighter).
 *                       Typical range: 1 - (VTS-4), where VTS ~1000 for 25fps.
 */
void ov5647_set_exposure(uint32_t exposure_lines);

/**
 * @brief Set OV5647 analogue gain directly (registers 0x350A-0x350B).
 * 
 * @param gain Gain value (1x = 0x10, 2x = 0x20, 4x = 0x40, 8x = 0x80).
 */
void ov5647_set_gain(uint16_t gain);

/**
 * @brief Set OV5647 white balance (registers 0x3400-0x3406).
 * 
 * @param red_gain   Red channel gain (0x400 = 1x, 0x800 = 2x).
 * @param green_gain Green channel gain.
 * @param blue_gain  Blue channel gain.
 */
void ov5647_set_awb(uint16_t red_gain, uint16_t green_gain, uint16_t blue_gain);

/**
 * @brief Set OV5647 test pattern (register 0x503D).
 * 
 * @param pattern Pattern mode:
 *                0x00: Disabled
 *                0x80: Color Bars
 *                0x81: Random Data
 *                0x82: Color Squares
 */
void ov5647_set_test_pattern(uint8_t pattern);

/**
 * @brief Set OV5647 Vertical Total Size (VTS) (registers 0x380E-0x380F).
 *        VTS controls the frame rate. FPS = PCLK / (HTS * VTS).
 * 
 * @param vts Vertical Total Size (lines).
 */
void ov5647_set_vts(uint16_t vts);

/**
 * @brief Get OV5647 Vertical Total Size (VTS) (registers 0x380E-0x380F).
 * 
 * @return uint16_t Current VTS value.
 */
uint16_t ov5647_get_vts(void);

/**
 * @brief Configure OV5647 camera controls directly via I2C.
 * Registers according to official OV5647 documentation.
 */
void setup_camera_controls(void);
