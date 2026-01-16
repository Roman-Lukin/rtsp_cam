/*
 * IMX219 Camera Sensor Helper for ESP32-P4
 * 
 * Implementation based on Sony IMX219 datasheet and Linux driver.
 * Pin-compatible with OV5647 modules but requires different I2C address
 * and register configuration.
 */

#include "imx219_helper.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "IMX219_HELPER";
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

// ============================================================================
// Register configuration tables
// ============================================================================

typedef struct {
    uint16_t reg;
    uint8_t val;
} imx219_reg_t;

#define IMX219_REG_DELAY    0xFFFF
#define IMX219_REG_END      0xFFFE

// Common initialization sequence (from Linux driver)
static const imx219_reg_t imx219_common_init[] = {
    // Access command sequence
    {0x30EB, 0x05},
    {0x30EB, 0x0C},
    {0x300A, 0xFF},
    {0x300B, 0xFF},
    {0x30EB, 0x05},
    {0x30EB, 0x09},
    
    // External clock frequency (assuming 24MHz)
    {0x0301, 0x05},  // VTPXCK_DIV
    {0x0303, 0x01},  // VTSYCK_DIV
    {0x0304, 0x03},  // PREPLLCK_VT_DIV
    {0x0305, 0x03},  // PREPLLCK_OP_DIV
    {0x0306, 0x00},  // PLL_VT_MPY[10:8]
    {0x0307, 0x39},  // PLL_VT_MPY[7:0]
    {0x030B, 0x01},  // OPSYCK_DIV
    {0x030C, 0x00},  // PLL_OP_MPY[10:8]
    {0x030D, 0x72},  // PLL_OP_MPY[7:0]
    
    // 2-lane MIPI
    {IMX219_REG_CSI_LANE_MODE, 0x01},
    
    // DPHY timing
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

// 1920x1080 @ 30fps mode (cropped from center of 3280x2464)
static const imx219_reg_t imx219_mode_1920x1080_30fps[] = {
    // Output size
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x07},  // 1920
    {IMX219_REG_X_OUTPUT_SIZE_L, 0x80},
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x04},  // 1080
    {IMX219_REG_Y_OUTPUT_SIZE_L, 0x38},
    
    // Crop window
    {0x0164, 0x02},  // X_ADDR_START[11:8]
    {0x0165, 0xA8},  // X_ADDR_START[7:0] = 680
    {0x0166, 0x0A},  // X_ADDR_END[11:8]
    {0x0167, 0x27},  // X_ADDR_END[7:0] = 2599
    {0x0168, 0x02},  // Y_ADDR_START[11:8]
    {0x0169, 0xB4},  // Y_ADDR_START[7:0] = 692
    {0x016A, 0x06},  // Y_ADDR_END[11:8]
    {0x016B, 0xEB},  // Y_ADDR_END[7:0] = 1771
    
    // No binning
    {IMX219_REG_BINNING_MODE_H, 0x00},
    {IMX219_REG_BINNING_MODE_V, 0x00},
    
    // Timing
    {IMX219_REG_LINE_LEN_H, 0x0D},  // Line length = 3448
    {IMX219_REG_LINE_LEN_L, 0x78},
    {IMX219_REG_FRAME_LEN_H, 0x04},  // Frame length = 1118
    {IMX219_REG_FRAME_LEN_L, 0x5E},
    
    // CSI data format: RAW10
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    {IMX219_REG_END, 0x00},
};

// 1280x720 @ 60fps mode (2x2 binned)
static const imx219_reg_t imx219_mode_1280x720_60fps[] = {
    // Output size
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x05},  // 1280
    {IMX219_REG_X_OUTPUT_SIZE_L, 0x00},
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x02},  // 720
    {IMX219_REG_Y_OUTPUT_SIZE_L, 0xD0},
    
    // Crop window (for 720p from binned image)
    {0x0164, 0x00},  // X_ADDR_START
    {0x0165, 0x00},
    {0x0166, 0x0C},  // X_ADDR_END
    {0x0167, 0xCF},
    {0x0168, 0x01},  // Y_ADDR_START
    {0x0169, 0x20},
    {0x016A, 0x08},  // Y_ADDR_END
    {0x016B, 0xDF},
    
    // 2x2 binning
    {IMX219_REG_BINNING_MODE_H, 0x01},
    {IMX219_REG_BINNING_MODE_V, 0x01},
    
    // Timing
    {IMX219_REG_LINE_LEN_H, 0x0D},  // Line length
    {IMX219_REG_LINE_LEN_L, 0x78},
    {IMX219_REG_FRAME_LEN_H, 0x03},  // Frame length for 60fps
    {IMX219_REG_FRAME_LEN_L, 0x0A},
    
    // CSI data format: RAW10
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    {IMX219_REG_END, 0x00},
};

// 640x480 @ 90fps mode (2x2 binned, cropped)
static const imx219_reg_t imx219_mode_640x480_90fps[] = {
    // Output size
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x02},  // 640
    {IMX219_REG_X_OUTPUT_SIZE_L, 0x80},
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x01},  // 480
    {IMX219_REG_Y_OUTPUT_SIZE_L, 0xE0},
    
    // Crop window
    {0x0164, 0x03},  // X_ADDR_START
    {0x0165, 0xE8},
    {0x0166, 0x08},  // X_ADDR_END
    {0x0167, 0xE7},
    {0x0168, 0x02},  // Y_ADDR_START
    {0x0169, 0xF0},
    {0x016A, 0x06},  // Y_ADDR_END
    {0x016B, 0xAF},
    
    // 2x2 binning
    {IMX219_REG_BINNING_MODE_H, 0x01},
    {IMX219_REG_BINNING_MODE_V, 0x01},
    
    // Timing for 90fps
    {IMX219_REG_LINE_LEN_H, 0x0D},
    {IMX219_REG_LINE_LEN_L, 0x78},
    {IMX219_REG_FRAME_LEN_H, 0x02},
    {IMX219_REG_FRAME_LEN_L, 0x06},
    
    // CSI data format: RAW10
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    {IMX219_REG_END, 0x00},
};

// 3280x2464 @ 15fps (full resolution, no binning)
static const imx219_reg_t imx219_mode_3280x2464_15fps[] = {
    // Output size
    {IMX219_REG_X_OUTPUT_SIZE_H, 0x0C},  // 3280
    {IMX219_REG_X_OUTPUT_SIZE_L, 0xD0},
    {IMX219_REG_Y_OUTPUT_SIZE_H, 0x09},  // 2464
    {IMX219_REG_Y_OUTPUT_SIZE_L, 0xA0},
    
    // Full sensor window
    {0x0164, 0x00},  // X_ADDR_START
    {0x0165, 0x00},
    {0x0166, 0x0C},  // X_ADDR_END
    {0x0167, 0xCF},
    {0x0168, 0x00},  // Y_ADDR_START
    {0x0169, 0x00},
    {0x016A, 0x09},  // Y_ADDR_END
    {0x016B, 0x9F},
    
    // No binning
    {IMX219_REG_BINNING_MODE_H, 0x00},
    {IMX219_REG_BINNING_MODE_V, 0x00},
    
    // Timing for 15fps
    {IMX219_REG_LINE_LEN_H, 0x0D},
    {IMX219_REG_LINE_LEN_L, 0x78},
    {IMX219_REG_FRAME_LEN_H, 0x09},
    {IMX219_REG_FRAME_LEN_L, 0xC4},
    
    // CSI data format: RAW10
    {IMX219_REG_CSI_DATA_FORMAT_H, 0x0A},
    {IMX219_REG_CSI_DATA_FORMAT_L, 0x0A},
    
    {IMX219_REG_END, 0x00},
};

// ============================================================================
// Low-level I2C functions
// ============================================================================

void imx219_helper_init(i2c_master_bus_handle_t bus_handle)
{
    s_i2c_bus_handle = bus_handle;
    ESP_LOGI(TAG, "IMX219 helper initialized");
}

esp_err_t imx219_read_reg(uint16_t reg, uint8_t *value)
{
    if (s_i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_addr[2] = {
        (reg >> 8) & 0xFF,
        reg & 0xFF
    };
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMX219_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
        return ret;
    }
    
    ret = i2c_master_transmit_receive(dev_handle, reg_addr, 2, value, 1, 100);
    i2c_master_bus_rm_device(dev_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read IMX219 reg 0x%04X failed", reg);
    }
    return ret;
}

esp_err_t imx219_write_reg(uint16_t reg, uint8_t value)
{
    if (s_i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[3] = {
        (reg >> 8) & 0xFF,
        reg & 0xFF,
        value
    };
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMX219_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    
    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
        return ret;
    }
    
    ret = i2c_master_transmit(dev_handle, data, 3, 100);
    i2c_master_bus_rm_device(dev_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Write IMX219 reg 0x%04X = 0x%02X failed", reg, value);
    } else {
        ESP_LOGD(TAG, "IMX219 reg 0x%04X = 0x%02X OK", reg, value);
    }
    return ret;
}

// Write register array
static esp_err_t imx219_write_array(const imx219_reg_t *regs)
{
    esp_err_t ret = ESP_OK;
    int i = 0;
    
    while (ret == ESP_OK && regs[i].reg != IMX219_REG_END) {
        if (regs[i].reg == IMX219_REG_DELAY) {
            vTaskDelay(pdMS_TO_TICKS(regs[i].val));
        } else {
            ret = imx219_write_reg(regs[i].reg, regs[i].val);
        }
        i++;
    }
    
    ESP_LOGD(TAG, "Wrote %d registers", i);
    return ret;
}

// ============================================================================
// Sensor ID and reset
// ============================================================================

esp_err_t imx219_get_chip_id(uint16_t *chip_id)
{
    uint8_t id_h = 0, id_l = 0;
    
    esp_err_t ret = imx219_read_reg(IMX219_REG_CHIP_ID_H, &id_h);
    if (ret != ESP_OK) {
        return ret;
    }
    
    ret = imx219_read_reg(IMX219_REG_CHIP_ID_L, &id_l);
    if (ret != ESP_OK) {
        return ret;
    }
    
    *chip_id = (id_h << 8) | id_l;
    ESP_LOGI(TAG, "IMX219 Chip ID: 0x%04X", *chip_id);
    
    return ESP_OK;
}

esp_err_t imx219_soft_reset(void)
{
    ESP_LOGI(TAG, "IMX219 software reset");
    esp_err_t ret = imx219_write_reg(IMX219_REG_SOFTWARE_RESET, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));  // Wait for reset
    return ret;
}

esp_err_t imx219_set_stream(bool enable)
{
    ESP_LOGI(TAG, "IMX219 stream %s", enable ? "START" : "STOP");
    return imx219_write_reg(IMX219_REG_MODE_SELECT, enable ? 0x01 : 0x00);
}

// ============================================================================
// Exposure control
// ============================================================================

void imx219_set_exposure(uint16_t exposure_lines)
{
    // IMX219 exposure is 16-bit value in registers 0x015A-0x015B
    // Valid range: 1 to (frame_length - 4)
    imx219_write_reg(IMX219_REG_EXPOSURE_H, (exposure_lines >> 8) & 0xFF);
    imx219_write_reg(IMX219_REG_EXPOSURE_L, exposure_lines & 0xFF);
    ESP_LOGD(TAG, "IMX219 exposure set to: %d lines", exposure_lines);
}

uint16_t imx219_get_exposure(void)
{
    uint8_t hi = 0, lo = 0;
    imx219_read_reg(IMX219_REG_EXPOSURE_H, &hi);
    imx219_read_reg(IMX219_REG_EXPOSURE_L, &lo);
    uint16_t exposure = (hi << 8) | lo;
    ESP_LOGD(TAG, "IMX219 exposure read: %d lines", exposure);
    return exposure;
}

// ============================================================================
// Gain control
// ============================================================================

void imx219_set_analog_gain(uint8_t gain)
{
    // IMX219 analog gain formula: Gain = 256 / (256 - gain)
    // gain = 0   -> 1.0x
    // gain = 128 -> 2.0x
    // gain = 170 -> ~3.0x
    // gain = 192 -> 4.0x
    // gain = 232 -> ~10.67x (max)
    if (gain > 232) {
        gain = 232;  // Clamp to max
    }
    imx219_write_reg(IMX219_REG_ANALOG_GAIN, gain);
    ESP_LOGD(TAG, "IMX219 analog gain set to: %d (%.2fx)", gain, 256.0f / (256.0f - gain));
}

uint8_t imx219_get_analog_gain(void)
{
    uint8_t gain = 0;
    imx219_read_reg(IMX219_REG_ANALOG_GAIN, &gain);
    return gain;
}

void imx219_set_digital_gain(uint16_t red_gain, uint16_t green_gain, uint16_t blue_gain)
{
    // Digital gain: 256 = 1x, 512 = 2x, etc.
    // Used for white balance adjustments
    
    // Green channels (GR and GB should be the same for proper Bayer processing)
    imx219_write_reg(IMX219_REG_DIG_GAIN_GR_H, (green_gain >> 8) & 0x0F);
    imx219_write_reg(IMX219_REG_DIG_GAIN_GR_L, green_gain & 0xFF);
    imx219_write_reg(IMX219_REG_DIG_GAIN_GB_H, (green_gain >> 8) & 0x0F);
    imx219_write_reg(IMX219_REG_DIG_GAIN_GB_L, green_gain & 0xFF);
    
    // Red channel
    imx219_write_reg(IMX219_REG_DIG_GAIN_R_H, (red_gain >> 8) & 0x0F);
    imx219_write_reg(IMX219_REG_DIG_GAIN_R_L, red_gain & 0xFF);
    
    // Blue channel
    imx219_write_reg(IMX219_REG_DIG_GAIN_B_H, (blue_gain >> 8) & 0x0F);
    imx219_write_reg(IMX219_REG_DIG_GAIN_B_L, blue_gain & 0xFF);
    
    ESP_LOGI(TAG, "IMX219 digital gain: R=%d G=%d B=%d", red_gain, green_gain, blue_gain);
}

// ============================================================================
// Test pattern
// ============================================================================

void imx219_set_test_pattern(uint16_t pattern)
{
    // 0x0000: Disabled
    // 0x0001: Solid color
    // 0x0002: 100% color bars
    // 0x0003: Fade to grey color bars
    // 0x0004: PN9
    imx219_write_reg(IMX219_REG_TEST_PATTERN_H, (pattern >> 8) & 0xFF);
    imx219_write_reg(IMX219_REG_TEST_PATTERN_L, pattern & 0xFF);
    ESP_LOGI(TAG, "IMX219 test pattern: 0x%04X", pattern);
}

// ============================================================================
// Frame timing
// ============================================================================

void imx219_set_frame_length(uint16_t frame_length)
{
    imx219_write_reg(IMX219_REG_FRAME_LEN_H, (frame_length >> 8) & 0xFF);
    imx219_write_reg(IMX219_REG_FRAME_LEN_L, frame_length & 0xFF);
    ESP_LOGI(TAG, "IMX219 frame length set to: %d", frame_length);
}

uint16_t imx219_get_frame_length(void)
{
    uint8_t hi = 0, lo = 0;
    imx219_read_reg(IMX219_REG_FRAME_LEN_H, &hi);
    imx219_read_reg(IMX219_REG_FRAME_LEN_L, &lo);
    return (hi << 8) | lo;
}

// ============================================================================
// Orientation
// ============================================================================

void imx219_set_orientation(bool h_mirror, bool v_flip)
{
    uint8_t val = 0;
    if (h_mirror) val |= 0x01;
    if (v_flip) val |= 0x02;
    imx219_write_reg(IMX219_REG_ORIENTATION, val);
    ESP_LOGI(TAG, "IMX219 orientation: mirror=%d, flip=%d", h_mirror, v_flip);
}

// ============================================================================
// Mode configuration
// ============================================================================

esp_err_t imx219_set_mode(uint8_t mode)
{
    const imx219_reg_t *mode_regs;
    const char *mode_name;
    
    switch (mode) {
        case 0:
            mode_regs = imx219_mode_1920x1080_30fps;
            mode_name = "1920x1080@30fps";
            break;
        case 1:
            mode_regs = imx219_mode_1280x720_60fps;
            mode_name = "1280x720@60fps";
            break;
        case 2:
            mode_regs = imx219_mode_640x480_90fps;
            mode_name = "640x480@90fps";
            break;
        case 3:
            mode_regs = imx219_mode_3280x2464_15fps;
            mode_name = "3280x2464@15fps";
            break;
        default:
            ESP_LOGE(TAG, "Invalid mode: %d", mode);
            return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Setting IMX219 mode: %s", mode_name);
    return imx219_write_array(mode_regs);
}

esp_err_t imx219_init_default(void)
{
    ESP_LOGI(TAG, "Initializing IMX219 sensor");
    
    // Verify chip ID
    uint16_t chip_id = 0;
    esp_err_t ret = imx219_get_chip_id(&chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read IMX219 chip ID");
        return ret;
    }
    
    if (chip_id != IMX219_CHIP_ID) {
        ESP_LOGE(TAG, "Invalid chip ID: 0x%04X (expected 0x%04X)", chip_id, IMX219_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Software reset
    ret = imx219_soft_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Software reset failed");
        return ret;
    }
    
    // Write common initialization
    ret = imx219_write_array(imx219_common_init);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Common init failed");
        return ret;
    }
    
    // Set default mode (1920x1080@30fps)
    ret = imx219_set_mode(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mode configuration failed");
        return ret;
    }
    
    // Set default exposure and gain
    imx219_set_exposure(1000);
    imx219_set_analog_gain(64);  // ~1.33x
    
    ESP_LOGI(TAG, "IMX219 initialization complete");
    return ESP_OK;
}

// ============================================================================
// Debug
// ============================================================================

void imx219_debug_status(void)
{
    ESP_LOGI(TAG, "=== IMX219 Debug Status ===");
    
    uint16_t chip_id = 0;
    imx219_get_chip_id(&chip_id);
    
    uint8_t mode_sel = 0;
    imx219_read_reg(IMX219_REG_MODE_SELECT, &mode_sel);
    ESP_LOGI(TAG, "Mode select: 0x%02X (%s)", mode_sel, mode_sel ? "STREAMING" : "STANDBY");
    
    uint16_t exposure = imx219_get_exposure();
    ESP_LOGI(TAG, "Exposure: %d lines", exposure);
    
    uint8_t gain = imx219_get_analog_gain();
    ESP_LOGI(TAG, "Analog gain: %d (%.2fx)", gain, 256.0f / (256.0f - gain));
    
    uint16_t frame_len = imx219_get_frame_length();
    ESP_LOGI(TAG, "Frame length: %d lines", frame_len);
    
    uint8_t hi, lo;
    imx219_read_reg(IMX219_REG_LINE_LEN_H, &hi);
    imx219_read_reg(IMX219_REG_LINE_LEN_L, &lo);
    ESP_LOGI(TAG, "Line length: %d pixels", (hi << 8) | lo);
    
    imx219_read_reg(IMX219_REG_X_OUTPUT_SIZE_H, &hi);
    imx219_read_reg(IMX219_REG_X_OUTPUT_SIZE_L, &lo);
    ESP_LOGI(TAG, "Output width: %d", (hi << 8) | lo);
    
    imx219_read_reg(IMX219_REG_Y_OUTPUT_SIZE_H, &hi);
    imx219_read_reg(IMX219_REG_Y_OUTPUT_SIZE_L, &lo);
    ESP_LOGI(TAG, "Output height: %d", (hi << 8) | lo);
    
    imx219_read_reg(IMX219_REG_ORIENTATION, &lo);
    ESP_LOGI(TAG, "Orientation: 0x%02X (mirror=%d, flip=%d)", lo, lo & 0x01, (lo >> 1) & 0x01);
    
    ESP_LOGI(TAG, "============================");
}
