/**
 * @file ov5647_camera.cpp
 * @brief OV5647 sensor implementation
 */

#include "ov5647_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "OV5647_CAM";

// OV5647 Register addresses
namespace OV5647Reg {
    // Chip ID
    constexpr uint16_t CHIP_ID_H        = 0x300A;
    constexpr uint16_t CHIP_ID_L        = 0x300B;
    
    // System control
    constexpr uint16_t SOFTWARE_RESET   = 0x0103;
    constexpr uint16_t MIPI_CTRL        = 0x4800;
    constexpr uint16_t IO_MIPI_CTRL     = 0x300E;
    
    // Frame timing
    constexpr uint16_t VTS_H            = 0x380E;
    constexpr uint16_t VTS_L            = 0x380F;
    constexpr uint16_t HTS_H            = 0x380C;
    constexpr uint16_t HTS_L            = 0x380D;
    
    // Exposure (3 bytes: [19:16], [15:8], [7:0])
    constexpr uint16_t EXPOSURE_H       = 0x3500;
    constexpr uint16_t EXPOSURE_M       = 0x3501;
    constexpr uint16_t EXPOSURE_L       = 0x3502;
    
    // AEC/AGC control
    constexpr uint16_t AEC_AGC_CTRL     = 0x3503;
    
    // Gain
    constexpr uint16_t GAIN_H           = 0x350A;
    constexpr uint16_t GAIN_L           = 0x350B;
    
    // AWB gain
    constexpr uint16_t AWB_GAIN_R_H     = 0x5186;
    constexpr uint16_t AWB_GAIN_R_L     = 0x5187;
    constexpr uint16_t AWB_GAIN_G_H     = 0x5188;
    constexpr uint16_t AWB_GAIN_G_L     = 0x5189;
    constexpr uint16_t AWB_GAIN_B_H     = 0x518A;
    constexpr uint16_t AWB_GAIN_B_L     = 0x518B;
    
    // Test pattern
    constexpr uint16_t ISP_CTRL_0       = 0x5000;
    constexpr uint16_t PRE_ISP_CTRL     = 0x503D;
    
    // Orientation
    constexpr uint16_t TIMING_TC_REG20  = 0x3820;
    constexpr uint16_t TIMING_TC_REG21  = 0x3821;
}

// OV5647 Constants
namespace OV5647Const {
    constexpr uint8_t I2C_ADDR          = 0x36;
    constexpr uint16_t CHIP_ID          = 0x5647;
    
    // Exposure in 1/16 line units
    constexpr uint16_t EXPOSURE_MIN     = 16;
    constexpr uint16_t EXPOSURE_MAX     = 65535;
    
    // Gain: 0-255 (0-8x approx)
    constexpr uint16_t GAIN_MIN         = 0;
    constexpr uint16_t GAIN_MAX         = 255;
    
    // Digital gain: 256 = 1x
    constexpr uint16_t DIGITAL_GAIN_MIN = 256;
    constexpr uint16_t DIGITAL_GAIN_MAX = 4095;
}

// Static capabilities definition
const CameraCapabilities OV5647Camera::capabilities_ = {
    .name = "OV5647",
    .chip_id = OV5647Const::CHIP_ID,
    .i2c_addr = OV5647Const::I2C_ADDR,
    .max_width = 2592,
    .max_height = 1944,
    .exposure_min = OV5647Const::EXPOSURE_MIN,
    .exposure_max = OV5647Const::EXPOSURE_MAX,
    .analog_gain_min = OV5647Const::GAIN_MIN,
    .analog_gain_max = OV5647Const::GAIN_MAX,
    .digital_gain_min = OV5647Const::DIGITAL_GAIN_MIN,
    .digital_gain_max = OV5647Const::DIGITAL_GAIN_MAX,
    .test_pattern_count = 3,
    .has_auto_exposure = true,   // OV5647 has built-in AEC
    .has_auto_gain = true,       // OV5647 has built-in AGC
    .has_auto_wb = true,         // OV5647 has AWB
};

OV5647Camera::OV5647Camera(i2c_master_bus_handle_t i2c_bus) {
    i2c_bus_ = i2c_bus;
    
    // Create I2C device handle
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = OV5647Const::I2C_ADDR;
    dev_cfg.scl_speed_hz = 100000;
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_dev_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
    }
}

const CameraCapabilities& OV5647Camera::getCapabilities() const {
    return capabilities_;
}

// I2C helpers
esp_err_t OV5647Camera::readReg(uint16_t reg, uint8_t* value) {
    if (!i2c_dev_) return ESP_ERR_INVALID_STATE;
    
    uint8_t reg_addr[2] = {
        static_cast<uint8_t>((reg >> 8) & 0xFF),
        static_cast<uint8_t>(reg & 0xFF)
    };
    
    return i2c_master_transmit_receive(i2c_dev_, reg_addr, 2, value, 1, 100);
}

esp_err_t OV5647Camera::writeReg(uint16_t reg, uint8_t value) {
    if (!i2c_dev_) return ESP_ERR_INVALID_STATE;
    
    uint8_t data[3] = {
        static_cast<uint8_t>((reg >> 8) & 0xFF),
        static_cast<uint8_t>(reg & 0xFF),
        value
    };
    
    return i2c_master_transmit(i2c_dev_, data, 3, 100);
}

esp_err_t OV5647Camera::readReg16(uint16_t reg, uint16_t* value) {
    uint8_t h, l;
    esp_err_t ret = readReg(reg, &h);
    if (ret != ESP_OK) return ret;
    ret = readReg(reg + 1, &l);
    if (ret != ESP_OK) return ret;
    *value = (h << 8) | l;
    return ESP_OK;
}

esp_err_t OV5647Camera::writeReg16(uint16_t reg, uint16_t value) {
    esp_err_t ret = writeReg(reg, (value >> 8) & 0xFF);
    if (ret != ESP_OK) return ret;
    return writeReg(reg + 1, value & 0xFF);
}

esp_err_t OV5647Camera::verifyChipId() {
    uint16_t chip_id;
    esp_err_t ret = readReg16(OV5647Reg::CHIP_ID_H, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID: %d", ret);
        return ret;
    }
    
    if (chip_id != OV5647Const::CHIP_ID) {
        ESP_LOGE(TAG, "Wrong chip ID: 0x%04X (expected 0x%04X)", chip_id, OV5647Const::CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "OV5647 verified, chip ID: 0x%04X", chip_id);
    return ESP_OK;
}

esp_err_t OV5647Camera::softReset() {
    ESP_LOGI(TAG, "Performing software reset");
    esp_err_t ret = writeReg(OV5647Reg::SOFTWARE_RESET, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ret;
}

esp_err_t OV5647Camera::setStreaming(bool enable) {
    ESP_LOGI(TAG, "Streaming: %s", enable ? "ON" : "OFF");
    
    if (enable) {
        // Wake up and enable MIPI
        writeReg(OV5647Reg::MIPI_CTRL, 0x04);
        writeReg(OV5647Reg::IO_MIPI_CTRL, 0x45);
    } else {
        // Enter standby
        writeReg(OV5647Reg::IO_MIPI_CTRL, 0x40);
        writeReg(OV5647Reg::MIPI_CTRL, 0x25);
    }
    
    return ESP_OK;
}

void OV5647Camera::setExposure(uint16_t lines) {
    // OV5647 uses 20-bit exposure in 1/16 line units
    // Input is in lines, so multiply by 16
    uint32_t exposure = static_cast<uint32_t>(lines) * 16;
    
    // Clamp
    if (exposure < OV5647Const::EXPOSURE_MIN) exposure = OV5647Const::EXPOSURE_MIN;
    if (exposure > 0xFFFFF) exposure = 0xFFFFF;  // 20-bit max
    
    writeReg(OV5647Reg::EXPOSURE_H, (exposure >> 16) & 0x0F);
    writeReg(OV5647Reg::EXPOSURE_M, (exposure >> 8) & 0xFF);
    writeReg(OV5647Reg::EXPOSURE_L, exposure & 0xF0);  // Lower 4 bits must be 0
    
    ESP_LOGD(TAG, "Exposure set to %lu (input lines: %u)", exposure, lines);
}

uint16_t OV5647Camera::getExposure() {
    uint8_t h, m, l;
    readReg(OV5647Reg::EXPOSURE_H, &h);
    readReg(OV5647Reg::EXPOSURE_M, &m);
    readReg(OV5647Reg::EXPOSURE_L, &l);
    
    uint32_t exposure = ((h & 0x0F) << 16) | (m << 8) | (l & 0xF0);
    return static_cast<uint16_t>(exposure / 16);  // Convert back to lines
}

esp_err_t OV5647Camera::setAutoExposure(bool enable) {
    uint8_t ctrl;
    esp_err_t ret = readReg(OV5647Reg::AEC_AGC_CTRL, &ctrl);
    if (ret != ESP_OK) return ret;
    
    if (enable) {
        ctrl &= ~0x01;  // Clear bit 0 to enable AEC
    } else {
        ctrl |= 0x01;   // Set bit 0 to disable AEC (manual)
    }
    
    ret = writeReg(OV5647Reg::AEC_AGC_CTRL, ctrl);
    ESP_LOGI(TAG, "Auto exposure: %s", enable ? "ON" : "OFF");
    return ret;
}

void OV5647Camera::setAnalogGain(uint16_t gain) {
    // Clamp to valid range (0-255)
    if (gain > OV5647Const::GAIN_MAX) {
        gain = OV5647Const::GAIN_MAX;
    }
    
    // OV5647 gain is 10-bit (0x350A/0x350B)
    writeReg16(OV5647Reg::GAIN_H, gain);
    ESP_LOGD(TAG, "Gain set to %u", gain);
}

uint16_t OV5647Camera::getAnalogGain() {
    uint16_t value = 0;
    readReg16(OV5647Reg::GAIN_H, &value);
    return value & 0x3FF;  // 10-bit
}

void OV5647Camera::setDigitalGain(uint16_t red, uint16_t green, uint16_t blue) {
    // OV5647 AWB gain registers
    writeReg16(OV5647Reg::AWB_GAIN_R_H, red);
    writeReg16(OV5647Reg::AWB_GAIN_G_H, green);
    writeReg16(OV5647Reg::AWB_GAIN_B_H, blue);
    
    ESP_LOGD(TAG, "Digital gain: R=%u, G=%u, B=%u", red, green, blue);
}

void OV5647Camera::setTestPattern(TestPattern pattern) {
    // Map common patterns to OV5647 values
    uint16_t value = 0;
    switch (pattern) {
        case TestPattern::DISABLED:     value = 0x00; break;
        case TestPattern::COLOR_BARS:   value = 0x80; break;  // 8-bar color
        case TestPattern::GREY_BARS:    value = 0x82; break;  // Color squares
        default:                        value = 0x00; break;
    }
    setTestPatternRaw(value);
}

void OV5647Camera::setTestPatternRaw(uint16_t value) {
    if (value == 0) {
        // Disable test pattern
        writeReg(OV5647Reg::PRE_ISP_CTRL, 0x00);
        ESP_LOGI(TAG, "Test pattern: Disabled");
    } else {
        // Enable test pattern
        writeReg(OV5647Reg::PRE_ISP_CTRL, static_cast<uint8_t>(value));
        ESP_LOGI(TAG, "Test pattern: 0x%02X", value);
    }
}

void OV5647Camera::setOrientation(bool hMirror, bool vFlip) {
    uint8_t reg20, reg21;
    readReg(OV5647Reg::TIMING_TC_REG20, &reg20);
    readReg(OV5647Reg::TIMING_TC_REG21, &reg21);
    
    if (vFlip) {
        reg20 |= 0x02;
    } else {
        reg20 &= ~0x02;
    }
    
    if (hMirror) {
        reg21 |= 0x02;
    } else {
        reg21 &= ~0x02;
    }
    
    writeReg(OV5647Reg::TIMING_TC_REG20, reg20);
    writeReg(OV5647Reg::TIMING_TC_REG21, reg21);
    
    ESP_LOGD(TAG, "Orientation: hMirror=%d, vFlip=%d", hMirror, vFlip);
}

void OV5647Camera::setFrameLength(uint16_t lines) {
    writeReg16(OV5647Reg::VTS_H, lines);
    ESP_LOGD(TAG, "Frame length (VTS) set to %u", lines);
}

uint16_t OV5647Camera::getFrameLength() {
    uint16_t value = 0;
    readReg16(OV5647Reg::VTS_H, &value);
    return value;
}

void OV5647Camera::debugStatus() {
    uint16_t chip_id = 0, vts = 0, hts = 0, gain = 0;
    uint8_t aec_ctrl = 0, test_pattern = 0;
    
    readReg16(OV5647Reg::CHIP_ID_H, &chip_id);
    readReg(OV5647Reg::AEC_AGC_CTRL, &aec_ctrl);
    readReg16(OV5647Reg::GAIN_H, &gain);
    readReg16(OV5647Reg::VTS_H, &vts);
    readReg16(OV5647Reg::HTS_H, &hts);
    readReg(OV5647Reg::PRE_ISP_CTRL, &test_pattern);
    
    ESP_LOGI(TAG, "=== OV5647 Status ===");
    ESP_LOGI(TAG, "  Chip ID: 0x%04X", chip_id);
    ESP_LOGI(TAG, "  AEC/AGC: 0x%02X (AEC %s, AGC %s)", 
             aec_ctrl,
             (aec_ctrl & 0x01) ? "OFF" : "ON",
             (aec_ctrl & 0x02) ? "OFF" : "ON");
    ESP_LOGI(TAG, "  Exposure: %u lines", getExposure());
    ESP_LOGI(TAG, "  Gain: %u", gain & 0x3FF);
    ESP_LOGI(TAG, "  VTS: %u, HTS: %u", vts, hts);
    ESP_LOGI(TAG, "  Test Pattern: 0x%02X", test_pattern);
}
