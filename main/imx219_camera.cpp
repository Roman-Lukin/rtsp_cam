/**
 * @file imx219_camera.cpp
 * @brief IMX219 sensor implementation
 * 
 * Note: This implementation delegates to imx219_helper.c functions
 * to avoid I2C device conflicts with the esp_video driver.
 */

#include "imx219_camera.h"
#include "imx219_helper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "IMX219_CAM";

// IMX219 Register addresses
namespace IMX219Reg {
    // Chip ID
    constexpr uint16_t CHIP_ID_H        = 0x0000;
    constexpr uint16_t CHIP_ID_L        = 0x0001;
    
    // Mode control
    constexpr uint16_t MODE_SELECT      = 0x0100;
    constexpr uint16_t SOFTWARE_RESET   = 0x0103;
    
    // Frame timing
    constexpr uint16_t FRAME_LEN_H      = 0x0160;
    constexpr uint16_t FRAME_LEN_L      = 0x0161;
    constexpr uint16_t LINE_LEN_H       = 0x0162;
    constexpr uint16_t LINE_LEN_L       = 0x0163;
    
    // Exposure (coarse integration time)
    constexpr uint16_t EXPOSURE_H       = 0x015A;
    constexpr uint16_t EXPOSURE_L       = 0x015B;
    
    // Gain
    constexpr uint16_t ANALOG_GAIN      = 0x0157;
    constexpr uint16_t DIG_GAIN_GR_H    = 0x0158;
    constexpr uint16_t DIG_GAIN_GR_L    = 0x0159;
    constexpr uint16_t DIG_GAIN_R_H     = 0x0180;
    constexpr uint16_t DIG_GAIN_R_L     = 0x0181;
    constexpr uint16_t DIG_GAIN_B_H     = 0x0182;
    constexpr uint16_t DIG_GAIN_B_L     = 0x0183;
    constexpr uint16_t DIG_GAIN_GB_H    = 0x0184;
    constexpr uint16_t DIG_GAIN_GB_L    = 0x0185;
    
    // Test pattern
    constexpr uint16_t TEST_PATTERN_H   = 0x0600;
    constexpr uint16_t TEST_PATTERN_L   = 0x0601;
    
    // Orientation
    constexpr uint16_t ORIENTATION      = 0x0172;
    
    // Output size
    constexpr uint16_t X_OUTPUT_SIZE_H  = 0x016C;
    constexpr uint16_t X_OUTPUT_SIZE_L  = 0x016D;
    constexpr uint16_t Y_OUTPUT_SIZE_H  = 0x016E;
    constexpr uint16_t Y_OUTPUT_SIZE_L  = 0x016F;
}

// IMX219 Constants
namespace IMX219Const {
    constexpr uint8_t I2C_ADDR          = 0x10;
    constexpr uint16_t CHIP_ID          = 0x0219;
    
    // Exposure: 4 to VTS-4
    constexpr uint16_t EXPOSURE_MIN     = 4;
    constexpr uint16_t EXPOSURE_MAX     = 65535;
    
    // Analog gain: 0-232
    // Real gain = 256 / (256 - gain)
    // 0 = 1x, 170 ≈ 3x, 232 ≈ 10.67x
    constexpr uint16_t ANALOG_GAIN_MIN  = 0;
    constexpr uint16_t ANALOG_GAIN_MAX  = 232;
    
    // Digital gain: 0x0100-0x0FFF (256-4095)
    // 256 = 1x, 512 = 2x, etc.
    constexpr uint16_t DIGITAL_GAIN_MIN = 0x0100;
    constexpr uint16_t DIGITAL_GAIN_MAX = 0x0FFF;
}

// Static capabilities definition
const CameraCapabilities IMX219Camera::capabilities_ = {
    .name = "IMX219",
    .chip_id = IMX219Const::CHIP_ID,
    .i2c_addr = IMX219Const::I2C_ADDR,
    .max_width = 3280,
    .max_height = 2464,
    .exposure_min = IMX219Const::EXPOSURE_MIN,
    .exposure_max = IMX219Const::EXPOSURE_MAX,
    .analog_gain_min = IMX219Const::ANALOG_GAIN_MIN,
    .analog_gain_max = IMX219Const::ANALOG_GAIN_MAX,
    .digital_gain_min = IMX219Const::DIGITAL_GAIN_MIN,
    .digital_gain_max = IMX219Const::DIGITAL_GAIN_MAX,
    .test_pattern_count = 5,
    .has_auto_exposure = false,  // IMX219 has no built-in AEC
    .has_auto_gain = false,
    .has_auto_wb = false,
};

IMX219Camera::IMX219Camera(i2c_master_bus_handle_t i2c_bus) {
    i2c_bus_ = i2c_bus;
    
    // Initialize the imx219_helper with the I2C bus
    // This shares the bus with the esp_video driver
    imx219_helper_init(i2c_bus);
    
    // Don't create a separate I2C device - use imx219_helper functions instead
    // to avoid conflicts with the esp_video imx219 driver
    i2c_dev_ = nullptr;
}

const CameraCapabilities& IMX219Camera::getCapabilities() const {
    return capabilities_;
}

// I2C helpers - delegate to imx219_helper
esp_err_t IMX219Camera::readReg(uint16_t reg, uint8_t* value) {
    return imx219_read_reg(reg, value);
}

esp_err_t IMX219Camera::writeReg(uint16_t reg, uint8_t value) {
    return imx219_write_reg(reg, value);
}

esp_err_t IMX219Camera::readReg16(uint16_t reg, uint16_t* value) {
    uint8_t h, l;
    esp_err_t ret = readReg(reg, &h);
    if (ret != ESP_OK) return ret;
    ret = readReg(reg + 1, &l);
    if (ret != ESP_OK) return ret;
    *value = (h << 8) | l;
    return ESP_OK;
}

esp_err_t IMX219Camera::writeReg16(uint16_t reg, uint16_t value) {
    esp_err_t ret = writeReg(reg, (value >> 8) & 0xFF);
    if (ret != ESP_OK) return ret;
    return writeReg(reg + 1, value & 0xFF);
}

esp_err_t IMX219Camera::verifyChipId() {
    uint16_t chip_id = 0;
    esp_err_t ret = imx219_get_chip_id(&chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID: %d", ret);
        return ret;
    }
    
    if (chip_id != IMX219Const::CHIP_ID) {
        ESP_LOGE(TAG, "Wrong chip ID: 0x%04X (expected 0x%04X)", chip_id, IMX219Const::CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "IMX219 verified, chip ID: 0x%04X", chip_id);
    return ESP_OK;
}

esp_err_t IMX219Camera::softReset() {
    ESP_LOGI(TAG, "Performing software reset");
    return imx219_soft_reset();
}

esp_err_t IMX219Camera::setStreaming(bool enable) {
    ESP_LOGI(TAG, "Streaming: %s", enable ? "ON" : "OFF");
    return imx219_set_stream(enable);
}

void IMX219Camera::setExposure(uint16_t lines) {
    // Clamp to valid range
    if (lines < IMX219Const::EXPOSURE_MIN) lines = IMX219Const::EXPOSURE_MIN;
    
    // Max exposure = VTS - 4
    uint16_t vts = getFrameLength();
    uint16_t max_exp = (vts > 4) ? (vts - 4) : IMX219Const::EXPOSURE_MIN;
    if (lines > max_exp) lines = max_exp;
    
    // Use imx219_helper function
    imx219_set_exposure(lines);
    ESP_LOGD(TAG, "Exposure set to %u lines (max=%u)", lines, max_exp);
}

uint16_t IMX219Camera::getExposure() {
    return imx219_get_exposure();
}

esp_err_t IMX219Camera::setAutoExposure(bool enable) {
    // IMX219 has no built-in AEC - return not supported
    // Auto exposure must be handled by ISP or external algorithm
    if (enable) {
        ESP_LOGW(TAG, "IMX219 has no built-in AEC - use ISP for auto exposure");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void IMX219Camera::setAnalogGain(uint16_t gain) {
    // Clamp to valid range (0-232)
    if (gain > IMX219Const::ANALOG_GAIN_MAX) {
        gain = IMX219Const::ANALOG_GAIN_MAX;
    }
    
    // Use imx219_helper function
    imx219_set_analog_gain(static_cast<uint8_t>(gain));
    
    // Calculate actual gain for logging
    float actual_gain = 256.0f / (256.0f - gain);
    ESP_LOGD(TAG, "Analog gain set to %u (%.2fx)", gain, actual_gain);
}

uint16_t IMX219Camera::getAnalogGain() {
    return imx219_get_analog_gain();
}

void IMX219Camera::setDigitalGain(uint16_t red, uint16_t green, uint16_t blue) {
    // Use imx219_helper function
    imx219_set_digital_gain(red, green, blue);
    ESP_LOGD(TAG, "Digital gain: R=%u, G=%u, B=%u", red, green, blue);
}

void IMX219Camera::setTestPattern(TestPattern pattern) {
    // Map common patterns to IMX219 values
    uint16_t value = 0;
    switch (pattern) {
        case TestPattern::DISABLED:     value = 0; break;
        case TestPattern::SOLID_COLOR:  value = 1; break;
        case TestPattern::COLOR_BARS:   value = 2; break;
        case TestPattern::GREY_BARS:    value = 3; break;
        case TestPattern::PN9:          value = 4; break;
        default:                        value = 0; break;
    }
    setTestPatternRaw(value);
}

void IMX219Camera::setTestPatternRaw(uint16_t value) {
    // Use imx219_helper function
    imx219_set_test_pattern(value);
    
    const char* names[] = {"Disabled", "Solid", "Color Bars", "Grey Bars", "PN9"};
    const char* name = (value < 5) ? names[value] : "Unknown";
    ESP_LOGI(TAG, "Test pattern: %s (%u)", name, value);
}

void IMX219Camera::setOrientation(bool hMirror, bool vFlip) {
    imx219_set_orientation(hMirror, vFlip);
    ESP_LOGD(TAG, "Orientation: hMirror=%d, vFlip=%d", hMirror, vFlip);
}

void IMX219Camera::setFrameLength(uint16_t lines) {
    imx219_set_frame_length(lines);
    ESP_LOGD(TAG, "Frame length (VTS) set to %u", lines);
}

uint16_t IMX219Camera::getFrameLength() {
    return imx219_get_frame_length();
}

void IMX219Camera::debugStatus() {
    // Use imx219_helper debug function
    imx219_debug_status();
}
