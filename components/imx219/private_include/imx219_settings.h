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
// Based on Linux kernel imx219 driver (imx219_common_regs + imx219_2lane_regs)
static const imx219_reginfo_t imx219_common_init[] = {
    // Stop streaming first
    {IMX219_REG_MODE_SELECT, 0x00},
    
    // Access command sequence (required before register access)
    {0x30EB, 0x05},
    {0x30EB, 0x0C},
    {0x300A, 0xFF},
    {0x300B, 0xFF},
    {0x30EB, 0x05},
    {0x30EB, 0x09},
    
    // DPHY timing parameters (undocumented but critical)
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
    
    // Frame timing (LINE_LENGTH_A = 3448 pixels)
    {IMX219_REG_LINE_LEN_H, 0x0D},
    {IMX219_REG_LINE_LEN_L, 0x78},
    
    // Pixel increment (X and Y odd increment = 1)
    {IMX219_REG_X_ODD_INC_A, 0x01},
    {IMX219_REG_Y_ODD_INC_A, 0x01},
    
    // Output setup - DPHY timing auto
    {IMX219_REG_DPHY_CTRL, IMX219_DPHY_CTRL_TIMING_AUTO},
    
    // External clock frequency = 24MHz (24 * 256 = 0x1800)
    {IMX219_REG_EXCK_FREQ_H, 0x18},
    {IMX219_REG_EXCK_FREQ_L, 0x00},
    
    // PLL configuration for 24MHz input clock (2-lane mode)
    {0x0301, 0x05},  // VTPXCK_DIV
    {0x0303, 0x01},  // VTSYCK_DIV
    {0x0304, 0x03},  // PREPLLCK_VT_DIV (AUTO)
    {0x0305, 0x03},  // PREPLLCK_OP_DIV (AUTO)
    {0x0306, 0x00},  // PLL_VT_MPY[10:8]
    {0x0307, 0x39},  // PLL_VT_MPY[7:0] = 57
    {0x030B, 0x01},  // OPSYCK_DIV
    {0x030C, 0x00},  // PLL_OP_MPY[10:8]
    {0x030D, 0x72},  // PLL_OP_MPY[7:0] = 114
    
    // CSI-2 configuration: 2 lanes
    {IMX219_REG_CSI_LANE_MODE, 0x01},
    
    {IMX219_REG_END, 0x00},
};

// Software reset sequence
static const imx219_reginfo_t imx219_sw_reset[] = {
    {IMX219_REG_MODE_SELECT, 0x00},     // Stop streaming
    {IMX219_REG_SOFTWARE_RESET, 0x01},  // Software reset
    {IMX219_REG_DELAY, 10},             // Wait 10ms
    {IMX219_REG_END, 0x00},
};

// 1280x960 @ 60fps - 2x2 binned mode, 4:3 aspect ratio
// Same sensor timing as working 720p but different output crop
// Resolution is 16-byte aligned (required by ISP)
static const imx219_reginfo_t imx219_mode_1640x1232_30fps[] = {
    // Full sensor readout (same as working 720p mode)
    {0x0164, 0x00}, {0x0165, 0x00},  // X_ADDR_START = 0
    {0x0166, 0x0C}, {0x0167, 0xCF},  // X_ADDR_END = 3279
    {0x0168, 0x00}, {0x0169, 0x00},  // Y_ADDR_START = 0
    {0x016A, 0x09}, {0x016B, 0x9F},  // Y_ADDR_END = 2463
    
    // Output size 1280x960 (after 2x2 binning = 1640x1232, then cropped)
    // Must be 16-byte aligned for ISP!
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x05}, {IMX219_REG_X_OUTPUT_SIZE_L, 0x00},  // 1280
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x03}, {IMX219_REG_Y_OUTPUT_SIZE_L, 0xC0},  // 960
    
    // Test pattern window
    {0x0624, 0x05}, {0x0625, 0x00},  // TP_WINDOW_WIDTH = 1280
    {0x0626, 0x03}, {0x0627, 0xC0},  // TP_WINDOW_HEIGHT = 960
    
    // 2x2 binning (NORMAL mode for RAW10) - same as working 720p
    {IMX219_REG_BINNING_MODE_H, 0x01},
    {IMX219_REG_BINNING_MODE_V, 0x01},
    
    // Frame timing - SAME AS 720p (860 lines) for 60fps
    {IMX219_REG_FRAME_LEN_H, 0x03}, {IMX219_REG_FRAME_LEN_L, 0x5C},  // 860 lines (same as 720p!)
    
    // RAW10 output format
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    {IMX219_REG_OPPXCK_DIV, 0x0A},
    
    // Default exposure and gain (same as 720p)
    {IMX219_REG_EXPOSURE_H, 0x03}, {IMX219_REG_EXPOSURE_L, 0x00},
    {IMX219_REG_ANALOG_GAIN, 0x40},
    
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
    
    // Test pattern window (same as output size)
    {0x0624, 0x05}, {0x0625, 0x00},  // TP_WINDOW_WIDTH = 1280
    {0x0626, 0x02}, {0x0627, 0xD0},  // TP_WINDOW_HEIGHT = 720
    
    // 2x2 binning (normal mode for RAW10)
    {IMX219_REG_BINNING_MODE_H, 0x01},
    {IMX219_REG_BINNING_MODE_V, 0x01},
    
    // Frame timing for 60fps
    {IMX219_REG_LINE_LEN_H, 0x0D}, {IMX219_REG_LINE_LEN_L, 0x78},    // 3448 pixels
    {IMX219_REG_FRAME_LEN_H, 0x03}, {IMX219_REG_FRAME_LEN_L, 0x5C},  // 860 lines
    
    // RAW10 output format
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    {IMX219_REG_OPPXCK_DIV, 0x0A},  // Output pixel clock divider for RAW10
    
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
