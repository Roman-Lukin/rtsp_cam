/*
 * SPDX-FileCopyrightText: 2024-2025 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Special register values
#define IMX219_REG_DELAY            0xFFFF
#define IMX219_REG_END              0xFFFE

// Chip ID registers
#define IMX219_REG_CHIP_ID_H        0x0000
#define IMX219_REG_CHIP_ID_L        0x0001

// Mode control
#define IMX219_REG_MODE_SELECT      0x0100
#define IMX219_REG_SOFTWARE_RESET   0x0103

// Frame format
#define IMX219_REG_FRAME_LEN_H      0x0160
#define IMX219_REG_FRAME_LEN_L      0x0161
#define IMX219_REG_LINE_LEN_H       0x0162
#define IMX219_REG_LINE_LEN_L       0x0163

// Exposure
#define IMX219_REG_EXPOSURE_H       0x015A
#define IMX219_REG_EXPOSURE_L       0x015B

// Gain
#define IMX219_REG_ANALOG_GAIN      0x0157
#define IMX219_REG_DIG_GAIN_GR_H    0x0158
#define IMX219_REG_DIG_GAIN_GR_L    0x0159

// Test pattern
#define IMX219_REG_TEST_PATTERN_H   0x0600
#define IMX219_REG_TEST_PATTERN_L   0x0601

// Orientation
#define IMX219_REG_ORIENTATION      0x0172

// Output size
#define IMX219_REG_X_OUTPUT_SIZE_H  0x016C
#define IMX219_REG_X_OUTPUT_SIZE_L  0x016D
#define IMX219_REG_Y_OUTPUT_SIZE_H  0x016E
#define IMX219_REG_Y_OUTPUT_SIZE_L  0x016F

// Binning
#define IMX219_REG_BINNING_MODE_H   0x0174
#define IMX219_REG_BINNING_MODE_V   0x0175

// CSI
#define IMX219_REG_CSI_DATA_FORMAT_H    0x018C
#define IMX219_REG_CSI_DATA_FORMAT_L    0x018D
#define IMX219_REG_CSI_LANE_MODE        0x0114

#ifdef __cplusplus
}
#endif
