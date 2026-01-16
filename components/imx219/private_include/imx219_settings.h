/*
 * SPDX-FileCopyrightText: 2024-2025 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * IMX219 Register Settings
 * Based on Sony IMX219 datasheet and Linux kernel driver
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "sdkconfig.h"
#include "imx219_regs.h"
#include "imx219_types.h"

// MIPI clock rates for different modes (24MHz input clock)
#define IMX219_IDI_CLOCK_RATE_1920x1080_30FPS    (182400000ULL)
#define IMX219_MIPI_CSI_LINE_RATE_1920x1080_30FPS (IMX219_IDI_CLOCK_RATE_1920x1080_30FPS * 5)

#define IMX219_IDI_CLOCK_RATE_1280x720_60FPS     (182400000ULL)
#define IMX219_MIPI_CSI_LINE_RATE_1280x720_60FPS (IMX219_IDI_CLOCK_RATE_1280x720_60FPS * 5)

#define IMX219_IDI_CLOCK_RATE_640x480_90FPS      (182400000ULL)
#define IMX219_MIPI_CSI_LINE_RATE_640x480_90FPS  (IMX219_IDI_CLOCK_RATE_640x480_90FPS * 5)

#define IMX219_IDI_CLOCK_RATE_3280x2464_15FPS    (182400000ULL)
#define IMX219_MIPI_CSI_LINE_RATE_3280x2464_15FPS (IMX219_IDI_CLOCK_RATE_3280x2464_15FPS * 5)

// Common initialization - access sequence and PLL configuration for 24MHz input
static const imx219_reginfo_t imx219_common_init[] = {
    // Access command sequence (required before register access)
    {0x30EB, 0x05},
    {0x30EB, 0x0C},
    {0x300A, 0xFF},
    {0x300B, 0xFF},
    {0x30EB, 0x05},
    {0x30EB, 0x09},
    
    // PLL configuration for 24MHz input clock
    // Target: ~456 MHz pixel clock for various modes
    {0x0301, 0x05},  // VTPXCK_DIV
    {0x0303, 0x01},  // VTSYCK_DIV
    {0x0304, 0x03},  // PREPLLCK_VT_DIV
    {0x0305, 0x03},  // PREPLLCK_OP_DIV
    {0x0306, 0x00},  // PLL_VT_MPY[10:8]
    {0x0307, 0x39},  // PLL_VT_MPY[7:0] = 57
    {0x030B, 0x01},  // OPSYCK_DIV
    {0x030C, 0x00},  // PLL_OP_MPY[10:8]
    {0x030D, 0x72},  // PLL_OP_MPY[7:0] = 114
    
    // CSI-2 configuration: 2 lanes
    {IMX219_REG_CSI_LANE_MODE, 0x01},
    
    // DPHY timing parameters
    {0x455E, 0x00},
    {0x471E, 0x4B},
    {0x4767, 0x0F},
    {0x4750, 0x14},
    {0x4540, 0x00},
    {0x47B4, 0x14},
    {0x4713, 0x30},
    {0x478B, 0x10},
    {0x478F, 0x10},
    {0x4793, 0x10},
    {0x4797, 0x0E},
    {0x479B, 0x0E},
    
    {IMX219_REG_END, 0x00},
};

// Software reset sequence
static const imx219_reginfo_t imx219_sw_reset[] = {
    {IMX219_REG_MODE_SELECT, 0x00},     // Stop streaming
    {IMX219_REG_SOFTWARE_RESET, 0x01},  // Software reset
    {IMX219_REG_DELAY, 10},             // Wait 10ms
    {IMX219_REG_END, 0x00},
};

// 1920x1080 @ 30fps (cropped from center)
static const imx219_reginfo_t imx219_mode_1920x1080_30fps[] = {
    // Crop from 3280x2464 to 1920x1080 (center)
    // X: (3280-1920)/2 = 680 start
    // Y: (2464-1080)/2 = 692 start
    {0x0164, 0x02}, {0x0165, 0xA8},  // X_ADDR_START = 680
    {0x0166, 0x0A}, {0x0167, 0x27},  // X_ADDR_END = 2599
    {0x0168, 0x02}, {0x0169, 0xB4},  // Y_ADDR_START = 692
    {0x016A, 0x06}, {0x016B, 0xEB},  // Y_ADDR_END = 1771
    
    // Output size
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x07}, {IMX219_REG_X_OUTPUT_SIZE_L, 0x80},  // 1920
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x04}, {IMX219_REG_Y_OUTPUT_SIZE_L, 0x38},  // 1080
    
    // No binning
    {IMX219_REG_BINNING_MODE_H, 0x00},
    {IMX219_REG_BINNING_MODE_V, 0x00},
    
    // Frame timing for 30fps
    {IMX219_REG_LINE_LEN_H, 0x0D}, {IMX219_REG_LINE_LEN_L, 0x78},    // 3448 pixels
    {IMX219_REG_FRAME_LEN_H, 0x06}, {IMX219_REG_FRAME_LEN_L, 0xE3},  // 1763 lines
    
    // RAW10 output
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    // Default exposure and gain
    {IMX219_REG_EXPOSURE_H, 0x06}, {IMX219_REG_EXPOSURE_L, 0x00},  // ~1536 lines
    {IMX219_REG_ANALOG_GAIN, 0x40},  // ~1.33x gain
    
    {IMX219_REG_END, 0x00},
};

// 1280x720 @ 60fps (2x2 binning + crop)
static const imx219_reginfo_t imx219_mode_1280x720_60fps[] = {
    // Full sensor readout area
    {0x0164, 0x00}, {0x0165, 0x00},  // X_ADDR_START = 0
    {0x0166, 0x0C}, {0x0167, 0xCF},  // X_ADDR_END = 3279
    {0x0168, 0x01}, {0x0169, 0x4C},  // Y_ADDR_START = 332 (crop for 720p)
    {0x016A, 0x08}, {0x016B, 0x53},  // Y_ADDR_END = 2131
    
    // Output size after 2x2 binning
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x05}, {IMX219_REG_X_OUTPUT_SIZE_L, 0x00},  // 1280
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x02}, {IMX219_REG_Y_OUTPUT_SIZE_L, 0xD0},  // 720
    
    // 2x2 binning
    {IMX219_REG_BINNING_MODE_H, 0x01},
    {IMX219_REG_BINNING_MODE_V, 0x01},
    
    // Frame timing for 60fps
    {IMX219_REG_LINE_LEN_H, 0x0D}, {IMX219_REG_LINE_LEN_L, 0x78},    // 3448 pixels
    {IMX219_REG_FRAME_LEN_H, 0x03}, {IMX219_REG_FRAME_LEN_L, 0x5C},  // 860 lines
    
    // RAW10 output
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    // Default exposure and gain
    {IMX219_REG_EXPOSURE_H, 0x03}, {IMX219_REG_EXPOSURE_L, 0x00},
    {IMX219_REG_ANALOG_GAIN, 0x40},
    
    {IMX219_REG_END, 0x00},
};

// 640x480 @ 90fps (2x2 binning + crop)
static const imx219_reginfo_t imx219_mode_640x480_90fps[] = {
    // Crop for 4:3 aspect ratio after binning
    {0x0164, 0x03}, {0x0165, 0xE8},  // X_ADDR_START = 1000
    {0x0166, 0x08}, {0x0167, 0xE7},  // X_ADDR_END = 2279
    {0x0168, 0x02}, {0x0169, 0xF0},  // Y_ADDR_START = 752
    {0x016A, 0x06}, {0x016B, 0xAF},  // Y_ADDR_END = 1711
    
    // Output size after 2x2 binning
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x02}, {IMX219_REG_X_OUTPUT_SIZE_L, 0x80},  // 640
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x01}, {IMX219_REG_Y_OUTPUT_SIZE_L, 0xE0},  // 480
    
    // 2x2 binning
    {IMX219_REG_BINNING_MODE_H, 0x01},
    {IMX219_REG_BINNING_MODE_V, 0x01},
    
    // Frame timing for 90fps
    {IMX219_REG_LINE_LEN_H, 0x0D}, {IMX219_REG_LINE_LEN_L, 0x78},    // 3448 pixels
    {IMX219_REG_FRAME_LEN_H, 0x02}, {IMX219_REG_FRAME_LEN_L, 0x3A},  // 570 lines
    
    // RAW10 output
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    // Default exposure and gain
    {IMX219_REG_EXPOSURE_H, 0x02}, {IMX219_REG_EXPOSURE_L, 0x00},
    {IMX219_REG_ANALOG_GAIN, 0x40},
    
    {IMX219_REG_END, 0x00},
};

// 3280x2464 @ 15fps (full resolution, no binning)
static const imx219_reginfo_t imx219_mode_3280x2464_15fps[] = {
    // Full sensor readout
    {0x0164, 0x00}, {0x0165, 0x00},  // X_ADDR_START = 0
    {0x0166, 0x0C}, {0x0167, 0xCF},  // X_ADDR_END = 3279
    {0x0168, 0x00}, {0x0169, 0x00},  // Y_ADDR_START = 0
    {0x016A, 0x09}, {0x016B, 0x9F},  // Y_ADDR_END = 2463
    
    // Full resolution output
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x0C}, {IMX219_REG_X_OUTPUT_SIZE_L, 0xD0},  // 3280
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x09}, {IMX219_REG_Y_OUTPUT_SIZE_L, 0xA0},  // 2464
    
    // No binning
    {IMX219_REG_BINNING_MODE_H, 0x00},
    {IMX219_REG_BINNING_MODE_V, 0x00},
    
    // Frame timing for 15fps
    {IMX219_REG_LINE_LEN_H, 0x0D}, {IMX219_REG_LINE_LEN_L, 0x78},    // 3448 pixels
    {IMX219_REG_FRAME_LEN_H, 0x0D}, {IMX219_REG_FRAME_LEN_L, 0xE8},  // 3560 lines
    
    // RAW10 output
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    // Default exposure and gain
    {IMX219_REG_EXPOSURE_H, 0x0D}, {IMX219_REG_EXPOSURE_L, 0x00},
    {IMX219_REG_ANALOG_GAIN, 0x40},
    
    {IMX219_REG_END, 0x00},
};

#ifdef __cplusplus
}
#endif
