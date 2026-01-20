/*
 * SPDX-FileCopyrightText: 2024-2025 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * IMX219 Camera Sensor Driver for ESP32-P4
 * Sony IMX219 8MP CMOS sensor (Raspberry Pi Camera v2 compatible)
 * 
 * Based on Linux kernel imx219 driver and Sony IMX219 datasheet
 */

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "imx219_settings.h"
#include "imx219.h"

#define IMX219_IO_MUX_LOCK(mux)
#define IMX219_IO_MUX_UNLOCK(mux)
#define IMX219_ENABLE_OUT_CLOCK(pin,clk)
#define IMX219_DISABLE_OUT_CLOCK(pin)

#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif
#define delay_ms(ms)  vTaskDelay((ms > portTICK_PERIOD_MS ? ms / portTICK_PERIOD_MS : 1))

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

static const char *TAG = "imx219";

/**
 * IMX219 Gain Table for ISP Pipeline Controller
 * 
 * IMX219 analog gain formula: Gain = 256 / (256 - reg_value)
 * reg_value range: 0-232 -> gain range: 1.0x to 10.67x
 * 
 * Values are gain * 1000 (to avoid decimals)
 * ISP uses index into this table as gain_index
 */
static const uint32_t imx219_gain_val_map[] = {
    // reg=0-31: 1.0x to 1.14x
    1000, 1004, 1008, 1012, 1016, 1020, 1024, 1029,
    1033, 1037, 1041, 1046, 1050, 1055, 1059, 1064,
    1068, 1073, 1078, 1082, 1087, 1092, 1097, 1102,
    1107, 1112, 1117, 1122, 1127, 1133, 1138, 1143,
    // reg=32-63: 1.14x to 1.33x
    1149, 1154, 1160, 1166, 1171, 1177, 1183, 1189,
    1195, 1201, 1207, 1214, 1220, 1226, 1233, 1239,
    1246, 1252, 1259, 1266, 1273, 1280, 1287, 1294,
    1301, 1309, 1316, 1324, 1331, 1339, 1347, 1355,
    // reg=64-95: 1.33x to 1.60x
    1362, 1371, 1379, 1387, 1395, 1404, 1412, 1421,
    1430, 1438, 1447, 1456, 1466, 1475, 1485, 1494,
    1504, 1514, 1524, 1534, 1544, 1554, 1565, 1575,
    1586, 1597, 1608, 1619, 1631, 1642, 1654, 1666,
    // reg=96-127: 1.60x to 2.0x
    1678, 1690, 1702, 1715, 1728, 1741, 1754, 1767,
    1781, 1795, 1809, 1823, 1838, 1852, 1867, 1882,
    1898, 1913, 1929, 1945, 1962, 1978, 1995, 2012,
    2030, 2048, 2065, 2084, 2102, 2121, 2140, 2159,
    // reg=128-159: 2.0x to 2.67x
    2179, 2199, 2219, 2240, 2261, 2282, 2304, 2327,
    2349, 2372, 2396, 2420, 2444, 2469, 2494, 2520,
    2546, 2573, 2600, 2628, 2656, 2685, 2714, 2744,
    2774, 2805, 2837, 2869, 2902, 2935, 2969, 3004,
    // reg=160-191: 2.67x to 4.0x
    3039, 3075, 3112, 3150, 3188, 3227, 3267, 3308,
    3350, 3393, 3436, 3481, 3526, 3573, 3620, 3669,
    3718, 3769, 3821, 3874, 3928, 3983, 4040, 4098,
    4157, 4217, 4279, 4343, 4408, 4474, 4542, 4612,
    // reg=192-223: 4.0x to 8.0x
    4683, 4756, 4831, 4907, 4986, 5067, 5150, 5235,
    5322, 5412, 5504, 5600, 5698, 5799, 5903, 6010,
    6121, 6235, 6353, 6475, 6601, 6731, 6866, 7006,
    7151, 7301, 7457, 7619, 7787, 7962, 8145, 8336,
    // reg=224-232: 8.0x to 10.67x
    8533, 8738, 8952, 9175, 9407, 9650, 9904, 10170, 10667,
};

#define IMX219_GAIN_MAP_SIZE ARRAY_SIZE(imx219_gain_val_map)

// ISP info for different modes
// IMX219 uses Bayer RGGB pattern (different from OV5647's GBRG)
// tline_ns = hts * 1e9 / pclk = 3448 * 1e9 / 182.4MHz = 18903ns
static const esp_cam_sensor_isp_info_t imx219_isp_info[] = {
    // Mode 0: 1280x960 @ 60fps (2x2 binned)
    {
        .isp_v1_info = {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .pclk = 182400000,
            .vts = 1100,
            .hts = 3448,
            .tline_ns = 18903,  // line time in nanoseconds
            .gain_def = 64,     // gain index for ~1.33x
            .exp_def = 800,     // reasonable default exposure
            .bayer_type = ESP_CAM_SENSOR_BAYER_RGGB,
        }
    },
    // Mode 1: 1280x720 @ 60fps  
    {
        .isp_v1_info = {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .pclk = 182400000,
            .vts = 860,
            .hts = 3448,
            .tline_ns = 18903,
            .gain_def = 64,
            .exp_def = 800,
            .bayer_type = ESP_CAM_SENSOR_BAYER_RGGB,
        }
    },
    // Mode 2: 640x480 @ 90fps
    {
        .isp_v1_info = {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .pclk = 182400000,
            .vts = 570,
            .hts = 3448,
            .tline_ns = 18903,
            .gain_def = 64,
            .exp_def = 500,
            .bayer_type = ESP_CAM_SENSOR_BAYER_RGGB,
        }
    },
    // Mode 3: 3280x2464 @ 15fps (full resolution)
    {
        .isp_v1_info = {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .pclk = 182400000,
            .vts = 3560,
            .hts = 3448,
            .tline_ns = 18903,
            .gain_def = 64,
            .exp_def = 1500,
            .bayer_type = ESP_CAM_SENSOR_BAYER_RGGB,
        }
    },
};

// Format definitions
static const esp_cam_sensor_format_t imx219_format_info[] = {
    {
        .name = "MIPI_2lane_24Minput_RAW10_1280x960_60fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = 24000000,
        .width = 1280,
        .height = 960,
        .regs = imx219_mode_1640x1232_30fps,  // Reusing the array name
        .regs_size = ARRAY_SIZE(imx219_mode_1640x1232_30fps),
        .fps = 60,  // Same as 720p!
        .isp_info = &imx219_isp_info[0],
        .mipi_info = {
            .mipi_clk = IMX219_MIPI_CSI_LINE_RATE_1280x720_60FPS,  // Same as 720p!
            .lane_num = 2,
            .line_sync_en = false,
        },
        .reserved = NULL,
    },
    {
        .name = "MIPI_2lane_24Minput_RAW10_1280x720_60fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = 24000000,
        .width = 1280,
        .height = 720,
        .regs = imx219_mode_1280x720_60fps,
        .regs_size = ARRAY_SIZE(imx219_mode_1280x720_60fps),
        .fps = 60,
        .isp_info = &imx219_isp_info[1],
        .mipi_info = {
            .mipi_clk = IMX219_MIPI_CSI_LINE_RATE_1280x720_60FPS,
            .lane_num = 2,
            .line_sync_en = false,
        },
        .reserved = NULL,
    },
    {
        .name = "MIPI_2lane_24Minput_RAW10_640x480_90fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = 24000000,
        .width = 640,
        .height = 480,
        .regs = imx219_mode_640x480_90fps,
        .regs_size = ARRAY_SIZE(imx219_mode_640x480_90fps),
        .fps = 90,
        .isp_info = &imx219_isp_info[2],
        .mipi_info = {
            .mipi_clk = IMX219_MIPI_CSI_LINE_RATE_640x480_90FPS,
            .lane_num = 2,
            .line_sync_en = false,
        },
        .reserved = NULL,
    },
    {
        .name = "MIPI_2lane_24Minput_RAW10_3280x2464_15fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = 24000000,
        .width = 3280,
        .height = 2464,
        .regs = imx219_mode_3280x2464_15fps,
        .regs_size = ARRAY_SIZE(imx219_mode_3280x2464_15fps),
        .fps = 15,
        .isp_info = &imx219_isp_info[3],
        .mipi_info = {
            .mipi_clk = IMX219_MIPI_CSI_LINE_RATE_3280x2464_15FPS,
            .lane_num = 2,
            .line_sync_en = false,
        },
        .reserved = NULL,
    },
};

// ============================================================================
// Low-level I2C/SCCB functions
// ============================================================================

static esp_err_t imx219_read(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t *read_buf)
{
    return esp_sccb_transmit_receive_reg_a16v8(sccb_handle, reg, read_buf);
}

static esp_err_t imx219_write(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t data)
{
    return esp_sccb_transmit_reg_a16v8(sccb_handle, reg, data);
}

static esp_err_t imx219_write_array(esp_sccb_io_handle_t sccb_handle, const imx219_reginfo_t *regarray)
{
    int i = 0;
    esp_err_t ret = ESP_OK;
    while ((ret == ESP_OK) && regarray[i].reg != IMX219_REG_END) {
        if (regarray[i].reg == IMX219_REG_DELAY) {
            delay_ms(regarray[i].val);
        } else {
            ret = imx219_write(sccb_handle, regarray[i].reg, regarray[i].val);
        }
        i++;
    }
    ESP_LOGD(TAG, "Wrote %d registers", i);
    return ret;
}

static esp_err_t imx219_set_reg_bits(esp_sccb_io_handle_t sccb_handle, uint16_t reg, 
                                      uint8_t offset, uint8_t length, uint8_t value)
{
    esp_err_t ret = ESP_OK;
    uint8_t reg_data = 0;

    ret = imx219_read(sccb_handle, reg, &reg_data);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t mask = ((1 << length) - 1) << offset;
    value = (reg_data & ~mask) | ((value << offset) & mask);
    ret = imx219_write(sccb_handle, reg, value);
    return ret;
}

// ============================================================================
// Sensor operations
// ============================================================================

static esp_err_t imx219_set_test_pattern(esp_cam_sensor_device_t *dev, int enable)
{
    // IMX219 test pattern: 0=off, 1=solid, 2=color bars, 3=fade bars, 4=PN9
    uint16_t pattern = enable ? 0x0002 : 0x0000;  // Color bars or off
    esp_err_t ret = imx219_write(dev->sccb_handle, IMX219_REG_TEST_PATTERN_H, (pattern >> 8) & 0xFF);
    if (ret == ESP_OK) {
        ret = imx219_write(dev->sccb_handle, IMX219_REG_TEST_PATTERN_L, pattern & 0xFF);
    }
    return ret;
}

static esp_err_t imx219_hw_reset(esp_cam_sensor_device_t *dev)
{
    if (dev->reset_pin >= 0) {
        gpio_set_level(dev->reset_pin, 0);
        delay_ms(10);
        gpio_set_level(dev->reset_pin, 1);
        delay_ms(10);
    }
    return ESP_OK;
}

static esp_err_t imx219_soft_reset(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = imx219_write(dev->sccb_handle, IMX219_REG_SOFTWARE_RESET, 0x01);
    delay_ms(10);
    return ret;
}

static esp_err_t imx219_get_sensor_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id)
{
    uint8_t pid_h, pid_l;
    esp_err_t ret = imx219_read(dev->sccb_handle, IMX219_REG_CHIP_ID_H, &pid_h);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "read pid_h failed");

    ret = imx219_read(dev->sccb_handle, IMX219_REG_CHIP_ID_L, &pid_l);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "read pid_l failed");

    uint16_t pid = (pid_h << 8) | pid_l;
    if (pid) {
        id->pid = pid;
    }
    ESP_LOGI(TAG, "IMX219 chip ID: 0x%04X", pid);
    return ret;
}

static esp_err_t imx219_set_stream(esp_cam_sensor_device_t *dev, int enable)
{
    ESP_LOGI(TAG, ">>> imx219_set_stream(%d) called", enable);
    esp_err_t ret = imx219_write(dev->sccb_handle, IMX219_REG_MODE_SELECT, enable ? 0x01 : 0x00);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "set stream failed");
    
    // Verify the write
    uint8_t mode_val = 0;
    ret = imx219_read(dev->sccb_handle, IMX219_REG_MODE_SELECT, &mode_val);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MODE_SELECT register verified: 0x%02X", mode_val);
    }
    
    // Log some key registers for debugging
    if (enable) {
        uint8_t hi, lo;
        imx219_read(dev->sccb_handle, IMX219_REG_X_OUTPUT_SIZE_H, &hi);
        imx219_read(dev->sccb_handle, IMX219_REG_X_OUTPUT_SIZE_L, &lo);
        ESP_LOGI(TAG, "Output width: %d", (hi << 8) | lo);
        
        imx219_read(dev->sccb_handle, IMX219_REG_Y_OUTPUT_SIZE_H, &hi);
        imx219_read(dev->sccb_handle, IMX219_REG_Y_OUTPUT_SIZE_L, &lo);
        ESP_LOGI(TAG, "Output height: %d", (hi << 8) | lo);
        
        imx219_read(dev->sccb_handle, IMX219_REG_FRAME_LEN_H, &hi);
        imx219_read(dev->sccb_handle, IMX219_REG_FRAME_LEN_L, &lo);
        ESP_LOGI(TAG, "Frame length: %d lines", (hi << 8) | lo);
        
        imx219_read(dev->sccb_handle, IMX219_REG_LINE_LEN_H, &hi);
        imx219_read(dev->sccb_handle, IMX219_REG_LINE_LEN_L, &lo);
        ESP_LOGI(TAG, "Line length: %d pixels", (hi << 8) | lo);
        
        imx219_read(dev->sccb_handle, IMX219_REG_CSI_LANE_MODE, &lo);
        ESP_LOGI(TAG, "CSI lane mode: %d (1=2-lane, 3=4-lane)", lo);
        
        imx219_read(dev->sccb_handle, IMX219_REG_CSI_DATA_FORMAT_H, &hi);
        imx219_read(dev->sccb_handle, IMX219_REG_CSI_DATA_FORMAT_L, &lo);
        ESP_LOGI(TAG, "CSI data format: 0x%02X%02X", hi, lo);
    }
    
    dev->stream_status = enable;
    ESP_LOGI(TAG, "Stream %s", enable ? "STARTED" : "STOPPED");
    return ESP_OK;
}

// Exposure control
static esp_err_t imx219_set_exposure(esp_cam_sensor_device_t *dev, int val)
{
    esp_err_t ret;
    
    // IMX219 exposure is 16-bit coarse integration time
    // Valid range: 1 to (frame_length - 4)
    ret = imx219_write(dev->sccb_handle, IMX219_REG_EXPOSURE_H, (val >> 8) & 0xFF);
    if (ret != ESP_OK) return ret;
    
    ret = imx219_write(dev->sccb_handle, IMX219_REG_EXPOSURE_L, val & 0xFF);
    return ret;
}

// Analog gain control
static esp_err_t imx219_set_analogue_gain(esp_cam_sensor_device_t *dev, int val)
{
    // IMX219 analog gain: Gain = 256 / (256 - val)
    // val=0 -> 1x, val=128 -> 2x, val=232 -> ~10.67x (max)
    if (val > 232) val = 232;
    if (val < 0) val = 0;
    
    return imx219_write(dev->sccb_handle, IMX219_REG_ANALOG_GAIN, (uint8_t)val);
}

// Mirror/flip control
static esp_err_t imx219_set_mirror(esp_cam_sensor_device_t *dev, int enable)
{
    return imx219_set_reg_bits(dev->sccb_handle, IMX219_REG_ORIENTATION, 0, 1, enable ? 0x01 : 0x00);
}

static esp_err_t imx219_set_vflip(esp_cam_sensor_device_t *dev, int enable)
{
    return imx219_set_reg_bits(dev->sccb_handle, IMX219_REG_ORIENTATION, 1, 1, enable ? 0x01 : 0x00);
}

// ============================================================================
// Sensor capability and format management
// ============================================================================

static esp_err_t imx219_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
{
    esp_err_t ret = ESP_OK;
    switch (qdesc->id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 1;
        qdesc->number.maximum = 65535;
        qdesc->number.step = 1;
        qdesc->default_value = 1536;
        break;
    case ESP_CAM_SENSOR_GAIN:
        // Use ENUMERATION type - ISP Pipeline expects gain_index as index into this table
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION;
        qdesc->enumeration.count = IMX219_GAIN_MAP_SIZE;
        qdesc->enumeration.elements = imx219_gain_val_map;
        qdesc->default_value = 64;  // Index 64 = ~1.33x gain
        break;
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN:
        // ISP Pipeline Controller uses this for atomic exposure+gain updates
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_U8;
        qdesc->u8.size = sizeof(esp_cam_sensor_gh_exp_gain_t);
        break;
    case ESP_CAM_SENSOR_VFLIP:
    case ESP_CAM_SENSOR_HMIRROR:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 0;
        qdesc->number.maximum = 1;
        qdesc->number.step = 1;
        qdesc->default_value = 0;
        break;
    default:
        ESP_LOGD(TAG, "Unsupported id=%"PRIx32, qdesc->id);
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    return ret;
}

static esp_err_t imx219_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t imx219_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
{
    esp_err_t ret = ESP_OK;

    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL: {
        int *val = (int *)arg;
        ret = imx219_set_exposure(dev, *val);
        break;
    }
    case ESP_CAM_SENSOR_GAIN: {
        // gain_index is the index into imx219_gain_val_map[]
        // For IMX219, index directly maps to register value (0-232)
        int *val = (int *)arg;
        int gain_idx = *val;
        if (gain_idx < 0) gain_idx = 0;
        if (gain_idx >= (int)IMX219_GAIN_MAP_SIZE) gain_idx = IMX219_GAIN_MAP_SIZE - 1;
        ret = imx219_set_analogue_gain(dev, gain_idx);
        ESP_LOGD(TAG, "Set gain index=%d (gain=%.2fx)", gain_idx, imx219_gain_val_map[gain_idx] / 1000.0f);
        break;
    }
    case ESP_CAM_SENSOR_VFLIP: {
        int *val = (int *)arg;
        ret = imx219_set_vflip(dev, *val);
        break;
    }
    case ESP_CAM_SENSOR_HMIRROR: {
        int *val = (int *)arg;
        ret = imx219_set_mirror(dev, *val);
        break;
    }
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN: {
        // Atomic exposure + gain update for manual control
        // With AGC disabled in IPA config, only manual changes come through here
        esp_cam_sensor_gh_exp_gain_t *value = (esp_cam_sensor_gh_exp_gain_t *)arg;
        uint32_t exp_val = 0;
        
        // Determine exposure value (prefer exposure_val if set)
        if (value->exposure_val != 0) {
            exp_val = value->exposure_val;
        } else if (value->exposure_us != 0) {
            // Convert us to line count using tline_ns
            // tline_ns = 18903ns, so line_time = ~18.9us
            exp_val = (value->exposure_us * 1000) / 18903;
            if (exp_val < 1) exp_val = 1;
            if (exp_val > 65535) exp_val = 65535;
        } else {
            ESP_LOGE(TAG, "GROUP_EXP_GAIN: no valid exposure value");
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        
        // Use Group Hold for atomic update at frame boundary
        // Write group_hold = 1 BEFORE changes
        ret = imx219_write(dev->sccb_handle, 0x0104, 0x01);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set group hold");
            break;
        }
        
        // Set gain (gain_index maps directly to register value 0-232)
        uint8_t gain_val = (value->gain_index > 232) ? 232 : (uint8_t)value->gain_index;
        ret = imx219_write(dev->sccb_handle, IMX219_REG_ANALOG_GAIN, gain_val);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write gain");
            imx219_write(dev->sccb_handle, 0x0104, 0x00);  // Release group hold
            break;
        }
        
        // Set exposure (2 bytes, MSB first)
        ret = imx219_write(dev->sccb_handle, IMX219_REG_EXPOSURE_H, (exp_val >> 8) & 0xFF);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write exposure H");
            imx219_write(dev->sccb_handle, 0x0104, 0x00);  // Release group hold
            break;
        }
        ret = imx219_write(dev->sccb_handle, IMX219_REG_EXPOSURE_L, exp_val & 0xFF);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write exposure L");
            imx219_write(dev->sccb_handle, 0x0104, 0x00);  // Release group hold
            break;
        }
        
        // Release group hold - changes take effect at next frame
        ret = imx219_write(dev->sccb_handle, 0x0104, 0x00);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to release group hold");
            break;
        }
        
        ESP_LOGI(TAG, "Manual EXP/GAIN: exp=%"PRIu32", gain=%"PRIu32" (%.2fx)", 
                 exp_val, value->gain_index, 
                 (value->gain_index < 233) ? 256.0f / (256.0f - value->gain_index) : 0);
        break;
    }
    default:
        ESP_LOGE(TAG, "set id=%" PRIx32 " not supported", id);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    return ret;
}

static esp_err_t imx219_query_support_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, formats);

    formats->count = ARRAY_SIZE(imx219_format_info);
    formats->format_array = &imx219_format_info[0];
    return ESP_OK;
}

static esp_err_t imx219_query_support_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *sensor_cap)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, sensor_cap);

    sensor_cap->fmt_raw = 1;  // RAW format supported
    return ESP_OK;
}

static esp_err_t imx219_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    
    ESP_LOGI(TAG, ">>> imx219_set_format() called, format=%p", format);
    
    // If format is NULL, use default format (like OV5647 does)
    if (format == NULL) {
        if (dev->sensor_port == ESP_CAM_SENSOR_MIPI_CSI) {
            format = &imx219_format_info[CONFIG_CAMERA_IMX219_MIPI_IF_FORMAT_INDEX_DEFAULT];
            ESP_LOGI(TAG, "Using default format index %d: %s", 
                     CONFIG_CAMERA_IMX219_MIPI_IF_FORMAT_INDEX_DEFAULT, format->name);
        } else {
            ESP_LOGE(TAG, "IMX219 only supports MIPI CSI interface");
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    
    if (format->regs == NULL) {
        ESP_LOGE(TAG, "Format has no register settings");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    
    // Stop streaming first
    ret = imx219_set_stream(dev, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop stream");
        return ret;
    }
    
    // Write common initialization
    ret = imx219_write_array(dev->sccb_handle, imx219_common_init);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write common init");
        return ret;
    }
    
    // Write mode-specific registers
    ret = imx219_write_array(dev->sccb_handle, (const imx219_reginfo_t *)format->regs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write mode registers");
        return ret;
    }
    
    // =========================================================================
    // CRITICAL: LP-11 initialization sequence
    // IMX219 doesn't enter LP-11 state on MIPI lines until streaming is first
    // started and then stopped. Without this, the MIPI CSI receiver will not
    // detect any frames!
    // Reference: Linux kernel imx219 driver - imx219_probe()
    // =========================================================================
    ESP_LOGI(TAG, "Executing LP-11 initialization sequence...");
    
    // Start streaming briefly
    ret = imx219_write(dev->sccb_handle, IMX219_REG_MODE_SELECT, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP-11 init: failed to start streaming");
        return ret;
    }
    
    // Wait for sensor to enter streaming mode (min 100us per datasheet)
    delay_ms(1);
    
    // Stop streaming - this puts MIPI lines into LP-11 state
    ret = imx219_write(dev->sccb_handle, IMX219_REG_MODE_SELECT, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LP-11 init: failed to stop streaming");
        return ret;
    }
    
    // Wait for sensor to stabilize in standby
    delay_ms(1);
    
    ESP_LOGI(TAG, "LP-11 initialization complete - MIPI lines ready");
    // =========================================================================
    
    dev->cur_format = format;
    ESP_LOGI(TAG, "Set format: %s", format->name);
    
    return ret;
}

static esp_err_t imx219_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, format);

    *format = *dev->cur_format;
    return ESP_OK;
}

static esp_err_t imx219_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
{
    esp_err_t ret = ESP_OK;
    uint8_t regval;
    esp_cam_sensor_reg_val_t *sensor_reg;

    switch (cmd) {
    case ESP_CAM_SENSOR_IOC_HW_RESET:
        ret = imx219_hw_reset(dev);
        break;
    case ESP_CAM_SENSOR_IOC_SW_RESET:
        ret = imx219_soft_reset(dev);
        break;
    case ESP_CAM_SENSOR_IOC_S_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = imx219_write(dev->sccb_handle, sensor_reg->regaddr, sensor_reg->value);
        break;
    case ESP_CAM_SENSOR_IOC_G_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = imx219_read(dev->sccb_handle, sensor_reg->regaddr, &regval);
        if (ret == ESP_OK) {
            sensor_reg->value = regval;
        }
        break;
    case ESP_CAM_SENSOR_IOC_S_STREAM:
        ret = imx219_set_stream(dev, *(int *)arg);
        break;
    case ESP_CAM_SENSOR_IOC_S_TEST_PATTERN:
        ret = imx219_set_test_pattern(dev, *(int *)arg);
        break;
    default:
        ESP_LOGW(TAG, "Unsupported cmd: 0x%x", cmd);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

// ============================================================================
// Power management
// ============================================================================

// Hardcoded GPIO for power enable (GPIO48 on ESP32-P4 custom board)
// This is necessary because the board config may not pass the correct pin
#define IMX219_POWER_ENABLE_GPIO  48

static esp_err_t imx219_power_on(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "IMX219 power on sequence starting...");

    // Note: On Raspberry Pi Camera v2 (IMX219 module), the PWDN pin (CAM_GPIO) 
    // is actually a POWER ENABLE pin, active HIGH (not power-down)
    // When HIGH = power regulator enabled, when LOW = power off
    
    // Use hardcoded GPIO48 for power enable since board config may not be correct
    gpio_num_t power_pin = (gpio_num_t)IMX219_POWER_ENABLE_GPIO;
    
    gpio_config_t conf = {0};
    conf.pin_bit_mask = BIT64(power_pin);
    conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&conf);
    
    // For RPi Camera v2 module: power enable is active HIGH
    // Don't toggle if already on - just ensure it's HIGH
    gpio_set_level(power_pin, 1);  // Power on (HIGH = enabled)
    delay_ms(100);  // Wait for power to stabilize
    ESP_LOGI(TAG, "Power enable GPIO%d set to HIGH", power_pin);

    // XCLK is typically not needed for RPi Camera v2 - it has internal oscillator
    // Only enable if explicitly configured with a valid pin (> 0, not just >= 0)
    // Pin 0 is typically boot button and should not be used
    if (dev->xclk_pin > 0 && dev->xclk_pin < GPIO_NUM_MAX) {
        IMX219_ENABLE_OUT_CLOCK(dev->xclk_pin, 24000000);  // 24MHz default
        ESP_LOGI(TAG, "XCLK enabled on pin %d @ 24MHz", dev->xclk_pin);
    } else {
        ESP_LOGI(TAG, "XCLK not configured (using internal oscillator)");
    }

    if (dev->reset_pin >= 0) {
        gpio_config_t reset_conf = {0};
        reset_conf.pin_bit_mask = BIT64(dev->reset_pin);
        reset_conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&reset_conf);
        
        // Reset sequence - longer delays for stable init
        gpio_set_level(dev->reset_pin, 0);  // Assert reset
        delay_ms(20);
        gpio_set_level(dev->reset_pin, 1);  // Release reset
        delay_ms(50);  // Wait for sensor to initialize
        ESP_LOGI(TAG, "RESET pin %d released, waiting for sensor init", dev->reset_pin);
    }

    // Additional delay for I2C to be ready
    delay_ms(20);
    ESP_LOGI(TAG, "IMX219 power on sequence complete");

    return ret;
}

static esp_err_t imx219_power_off(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ESP_OK;

    if (dev->xclk_pin >= 0 && dev->xclk_pin < GPIO_NUM_MAX) {
        IMX219_DISABLE_OUT_CLOCK(dev->xclk_pin);
    }
    
    // WARNING: Do NOT power off GPIO48 here if we want to keep using the sensor
    // esp_video_init() calls detect -> success -> then later calls power_off if something else fails
    // or if it continues iterating.
    
    // For now, we only power off if EXPLICITLY requested, but since this function is called
    // by detection failure cleanup, we should be careful. 
    // BUT: esp_video_init iterates through all registered detection functions.
    // If imx219_detect() succeeds, it returns dev. If it fails, it calls power_off.
    
    // PROBLEM: imx219_detect checks ID, if fails -> calls power_off.
    // The log shows:
    // 1. imx219_detect called for 0x1A (IMX219_alt)
    // 2. power_on called (GPIO48 HIGH)
    // 3. get_sensor_id failed (because sensor is at 0x10, not 0x1A)
    // 4. power_off called (GPIO48 LOW)
    // 5. esp_video_init moves to next sensor... oh wait!
    
    // SOLUTION: We should only toggle power if we are sure we want to resetting it
    // effectively. But here, since we share one power line for potentially multiple addresses
    // (0x10 and 0x1A are same chip), powering off one kills the other.
    
    // We will verify if the device structure has a valid "sccb_handle" to decide? 
    // No. simpler: Just DON'T power off GPIO48 in this driver unless really needed.
    // As a workaround for the shared power line issue during scanning:
    
    // Use hardcoded GPIO48 for power
    // gpio_num_t power_pin = (gpio_num_t)IMX219_POWER_ENABLE_GPIO;
    
    // For RPi Camera v2: power enable is active HIGH, so LOW = power off
    // gpio_set_level(power_pin, 0);  // Power off
    // delay_ms(10);
    // ESP_LOGI(TAG, "Power enable GPIO%d set to LOW (power off)", power_pin);
    
    ESP_LOGW(TAG, "imx219_power_off called - SKIPPING power down to avoid killing active sensor");

    if (dev->reset_pin >= 0) {
        gpio_set_level(dev->reset_pin, 0);  // Hold in reset
        delay_ms(10);
    }

    return ret;
}

static esp_err_t imx219_delete(esp_cam_sensor_device_t *dev)
{
    ESP_LOGD(TAG, "del imx219 (%p)", dev);
    if (dev) {
        free(dev);
        dev = NULL;
    }
    return ESP_OK;
}

// ============================================================================
// Sensor operations table
// ============================================================================

static const esp_cam_sensor_ops_t imx219_ops = {
    .query_para_desc = imx219_query_para_desc,
    .get_para_value = imx219_get_para_value,
    .set_para_value = imx219_set_para_value,
    .query_support_formats = imx219_query_support_formats,
    .query_support_capability = imx219_query_support_capability,
    .set_format = imx219_set_format,
    .get_format = imx219_get_format,
    .priv_ioctl = imx219_priv_ioctl,
    .del = imx219_delete
};

// ============================================================================
// Detection function
// ============================================================================

esp_cam_sensor_device_t *imx219_detect(esp_cam_sensor_config_t *config)
{
    esp_cam_sensor_device_t *dev = NULL;

    if (config == NULL) {
        return NULL;
    }

    dev = calloc(1, sizeof(esp_cam_sensor_device_t));
    if (dev == NULL) {
        ESP_LOGE(TAG, "No memory for camera");
        return NULL;
    }

    dev->name = (char *)IMX219_SENSOR_NAME;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->sensor_port = config->sensor_port;
    dev->ops = &imx219_ops;
    
    if (config->sensor_port == ESP_CAM_SENSOR_MIPI_CSI) {
        dev->cur_format = &imx219_format_info[CONFIG_CAMERA_IMX219_MIPI_IF_FORMAT_INDEX_DEFAULT];
    } else {
        ESP_LOGE(TAG, "IMX219 only supports MIPI CSI interface");
        goto err_free_handler;
    }

    // Power on the sensor
    if (imx219_power_on(dev) != ESP_OK) {
        ESP_LOGE(TAG, "Camera power on failed");
        goto err_free_handler;
    }

    // Read and verify sensor ID
    if (imx219_get_sensor_id(dev, &dev->id) != ESP_OK) {
        ESP_LOGE(TAG, "Get sensor ID failed");
        goto err_power_off;
    }
    
    if (dev->id.pid != IMX219_PID) {
        ESP_LOGE(TAG, "Camera sensor is not IMX219, PID=0x%x (expected 0x%x)", 
                 dev->id.pid, IMX219_PID);
        goto err_power_off;
    }
    
    ESP_LOGI(TAG, "Detected IMX219 camera sensor PID=0x%x", dev->id.pid);

    return dev;

err_power_off:
    imx219_power_off(dev);
err_free_handler:
    free(dev);
    return NULL;
}

// ============================================================================
// Auto-detection registration
// ============================================================================

#if CONFIG_CAMERA_IMX219_AUTO_DETECT_MIPI_INTERFACE_SENSOR
// Register detection for primary I2C address (0x10) only
// Note: Alternative address 0x1A is not registered to avoid unnecessary error logs
// during auto-detection. Most IMX219 modules use 0x10.
ESP_CAM_SENSOR_DETECT_FN(imx219_detect, ESP_CAM_SENSOR_MIPI_CSI, IMX219_SCCB_ADDR)
{
    ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    return imx219_detect(config);
}
#endif
