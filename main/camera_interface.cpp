/**
 * @file camera_interface.cpp
 * @brief Camera interface factory and C-compatible API
 */

#include "camera_interface.h"
#include "imx219_camera.h"
#include "ov5647_camera.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "CAM_IF";

// Global camera instance
ICameraInterface* g_camera = nullptr;

/**
 * @brief Factory function to create camera interface by sensor type
 */
ICameraInterface* createCameraInterface(const char* type, i2c_master_bus_handle_t i2c_bus) {
    if (strcmp(type, "IMX219") == 0) {
        ESP_LOGI(TAG, "Creating IMX219 camera interface");
        return new IMX219Camera(i2c_bus);
    } else if (strcmp(type, "OV5647") == 0) {
        ESP_LOGI(TAG, "Creating OV5647 camera interface");
        return new OV5647Camera(i2c_bus);
    }
    
    ESP_LOGE(TAG, "Unknown camera type: %s", type);
    return nullptr;
}

/**
 * @brief Auto-detect and create camera interface
 */
ICameraInterface* autoDetectCamera(i2c_master_bus_handle_t i2c_bus) {
    ESP_LOGI(TAG, "Auto-detecting camera sensor...");
    
    // Try IMX219 first (0x10)
    if (i2c_master_probe(i2c_bus, 0x10, 50) == ESP_OK) {
        ESP_LOGI(TAG, "Found device at 0x10 - trying IMX219");
        auto* camera = new IMX219Camera(i2c_bus);
        if (camera->verifyChipId() == ESP_OK) {
            ESP_LOGI(TAG, "IMX219 detected and verified");
            return camera;
        }
        delete camera;
    }
    
    // Try OV5647 (0x36)
    if (i2c_master_probe(i2c_bus, 0x36, 50) == ESP_OK) {
        ESP_LOGI(TAG, "Found device at 0x36 - trying OV5647");
        auto* camera = new OV5647Camera(i2c_bus);
        if (camera->verifyChipId() == ESP_OK) {
            ESP_LOGI(TAG, "OV5647 detected and verified");
            return camera;
        }
        delete camera;
    }
    
    ESP_LOGE(TAG, "No supported camera sensor found!");
    return nullptr;
}

// ===== C-compatible API =====

extern "C" {

esp_err_t camera_interface_init(i2c_master_bus_handle_t i2c_bus) {
    if (g_camera) {
        ESP_LOGW(TAG, "Camera interface already initialized");
        return ESP_OK;
    }
    
    g_camera = autoDetectCamera(i2c_bus);
    if (!g_camera) {
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Camera interface initialized: %s", g_camera->getCapabilities().name);
    return ESP_OK;
}

const char* camera_get_name(void) {
    if (!g_camera) return "NONE";
    return g_camera->getCapabilities().name;
}

void camera_set_exposure(uint16_t lines) {
    if (g_camera) {
        g_camera->setExposure(lines);
    }
}

void camera_set_gain(uint16_t gain) {
    if (g_camera) {
        g_camera->setAnalogGain(gain);
    }
}

esp_err_t camera_set_auto_exposure(bool enable) {
    if (!g_camera) return ESP_ERR_INVALID_STATE;
    return g_camera->setAutoExposure(enable);
}

void camera_set_test_pattern(uint16_t pattern) {
    if (g_camera) {
        g_camera->setTestPatternRaw(pattern);
    }
}

void camera_set_wb_gains(uint16_t red, uint16_t green, uint16_t blue) {
    if (g_camera) {
        g_camera->setDigitalGain(red, green, blue);
    }
}

void camera_get_exposure_limits(uint16_t* min, uint16_t* max) {
    if (g_camera) {
        const auto& caps = g_camera->getCapabilities();
        if (min) *min = caps.exposure_min;
        if (max) *max = caps.exposure_max;
    } else {
        if (min) *min = 1;
        if (max) *max = 1000;
    }
}

void camera_get_gain_limits(uint16_t* min, uint16_t* max) {
    if (g_camera) {
        const auto& caps = g_camera->getCapabilities();
        if (min) *min = caps.analog_gain_min;
        if (max) *max = caps.analog_gain_max;
    } else {
        if (min) *min = 0;
        if (max) *max = 255;
    }
}

} // extern "C"
