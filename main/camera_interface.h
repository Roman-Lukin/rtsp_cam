/**
 * @file camera_interface.h
 * @brief Abstract interface for camera sensors
 * 
 * This provides a unified API for different camera sensors (IMX219, OV5647, etc.)
 * Each sensor implements this interface with its specific register operations.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus

/**
 * @brief Camera sensor capabilities
 */
struct CameraCapabilities {
    const char* name;           // Sensor name (e.g., "IMX219", "OV5647")
    uint16_t chip_id;           // Expected chip ID
    uint8_t i2c_addr;           // I2C address (7-bit)
    
    // Resolution limits
    uint16_t max_width;
    uint16_t max_height;
    
    // Exposure limits (in lines)
    uint16_t exposure_min;
    uint16_t exposure_max;
    
    // Gain limits
    uint16_t analog_gain_min;
    uint16_t analog_gain_max;
    uint16_t digital_gain_min;
    uint16_t digital_gain_max;
    
    // Test pattern modes count
    uint8_t test_pattern_count;
    
    // Feature flags
    bool has_auto_exposure;     // Sensor has built-in AEC
    bool has_auto_gain;         // Sensor has built-in AGC
    bool has_auto_wb;           // Sensor has built-in AWB
};

/**
 * @brief Test pattern modes (common across sensors)
 */
enum class TestPattern : uint8_t {
    DISABLED = 0,
    SOLID_COLOR = 1,
    COLOR_BARS = 2,
    GREY_BARS = 3,
    PN9 = 4,
    // Sensor-specific patterns can be added
};

/**
 * @brief Abstract camera interface
 */
class ICameraInterface {
public:
    virtual ~ICameraInterface() = default;
    
    // ===== Identification =====
    
    /**
     * @brief Get sensor capabilities
     */
    virtual const CameraCapabilities& getCapabilities() const = 0;
    
    /**
     * @brief Verify sensor by reading chip ID
     * @return ESP_OK if sensor responds with correct ID
     */
    virtual esp_err_t verifyChipId() = 0;
    
    // ===== Control =====
    
    /**
     * @brief Perform software reset
     */
    virtual esp_err_t softReset() = 0;
    
    /**
     * @brief Enable/disable streaming
     */
    virtual esp_err_t setStreaming(bool enable) = 0;
    
    // ===== Exposure Control =====
    
    /**
     * @brief Set exposure time in lines
     * @param lines Exposure lines (sensor-specific range)
     */
    virtual void setExposure(uint16_t lines) = 0;
    
    /**
     * @brief Get current exposure
     */
    virtual uint16_t getExposure() = 0;
    
    /**
     * @brief Enable/disable auto exposure (if supported)
     * @return ESP_ERR_NOT_SUPPORTED if sensor has no AEC
     */
    virtual esp_err_t setAutoExposure(bool enable) = 0;
    
    // ===== Gain Control =====
    
    /**
     * @brief Set analog gain
     * @param gain Gain value (sensor-specific range and formula)
     */
    virtual void setAnalogGain(uint16_t gain) = 0;
    
    /**
     * @brief Get current analog gain
     */
    virtual uint16_t getAnalogGain() = 0;
    
    /**
     * @brief Set digital gain for color channels (white balance)
     * @param red Red channel gain (256 = 1x typically)
     * @param green Green channel gain
     * @param blue Blue channel gain
     */
    virtual void setDigitalGain(uint16_t red, uint16_t green, uint16_t blue) = 0;
    
    // ===== Test Pattern =====
    
    /**
     * @brief Set test pattern mode
     * @param pattern Test pattern type
     */
    virtual void setTestPattern(TestPattern pattern) = 0;
    
    /**
     * @brief Set test pattern by raw value (sensor-specific)
     */
    virtual void setTestPatternRaw(uint16_t value) = 0;
    
    // ===== Orientation =====
    
    /**
     * @brief Set image orientation (mirror/flip)
     */
    virtual void setOrientation(bool hMirror, bool vFlip) = 0;
    
    // ===== Frame Timing =====
    
    /**
     * @brief Set frame length (VTS) for frame rate control
     */
    virtual void setFrameLength(uint16_t lines) = 0;
    
    /**
     * @brief Get current frame length
     */
    virtual uint16_t getFrameLength() = 0;
    
    // ===== Debug =====
    
    /**
     * @brief Print current sensor status to log
     */
    virtual void debugStatus() = 0;
    
protected:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
};

/**
 * @brief Factory function to create camera interface by sensor type
 * @param type Sensor type string ("IMX219", "OV5647")
 * @param i2c_bus I2C bus handle
 * @return Pointer to camera interface or nullptr if unknown type
 */
ICameraInterface* createCameraInterface(const char* type, i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Auto-detect and create camera interface
 * @param i2c_bus I2C bus handle
 * @return Pointer to camera interface or nullptr if no sensor found
 */
ICameraInterface* autoDetectCamera(i2c_master_bus_handle_t i2c_bus);

// Global camera instance (set by main)
extern ICameraInterface* g_camera;

#endif // __cplusplus

// ===== C-compatible API =====
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize camera interface (auto-detect sensor)
 */
esp_err_t camera_interface_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Get camera name
 */
const char* camera_get_name(void);

/**
 * @brief Set exposure (lines)
 */
void camera_set_exposure(uint16_t lines);

/**
 * @brief Set analog gain
 */
void camera_set_gain(uint16_t gain);

/**
 * @brief Set auto exposure
 */
esp_err_t camera_set_auto_exposure(bool enable);

/**
 * @brief Set test pattern (0=disabled, 2=color bars, etc.)
 */
void camera_set_test_pattern(uint16_t pattern);

/**
 * @brief Set digital white balance gains
 */
void camera_set_wb_gains(uint16_t red, uint16_t green, uint16_t blue);

/**
 * @brief Get exposure limits
 */
void camera_get_exposure_limits(uint16_t* min, uint16_t* max);

/**
 * @brief Get gain limits
 */
void camera_get_gain_limits(uint16_t* min, uint16_t* max);

#ifdef __cplusplus
}
#endif
