#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Camera settings structure (C-compatible)
 */
typedef struct CameraSettings {
    // Exposure settings
    bool auto_exposure;
    int32_t exposure_value;     // 1-1000 (in 100µs units)
    int32_t gain;               // 0-255
    
    // White Balance
    bool auto_white_balance;
    int32_t wb_red_gain;        // Manual WB gains (if AWB disabled) 0x000-0xFFF
    int32_t wb_green_gain;
    int32_t wb_blue_gain;
    
    // Test pattern
    int32_t test_pattern;       // 0=off, 0x80=bars, 0x82=squares
    
    // ISP settings
    bool denoise_enable;
    int32_t denoise_level;      // 2-20
    
    // Anti-flicker
    int32_t power_line_freq;    // 0=disabled, 1=50Hz, 2=60Hz
    
    // H.264 encoder settings
    int32_t bitrate;            // in kbps
    int32_t gop;                // GOP size (frames)
    int32_t min_qp;             // 0-51
    int32_t max_qp;             // 0-51
} CameraSettings;

/**
 * @brief Get default camera settings
 */
CameraSettings app_params_default_settings(void);

/**
 * @brief Get pointer to CameraSettings for reading
 */
const CameraSettings* app_params_get_settings(void);

/**
 * @brief Get pointer to CameraSettings for modification
 */
CameraSettings* app_params_get_settings_mut(void);

/**
 * @brief Apply settings to camera hardware and save to NVS
 */
esp_err_t app_params_apply_and_save(void);

#ifdef __cplusplus
}

/**
 * @brief Application parameters manager (C++ only)
 * 
 * Handles NVS initialization and persistent storage of camera settings.
 */
class AppParams {
public:
    AppParams();
    ~AppParams();

    /**
     * @brief Get current camera settings (const reference)
     */
    const CameraSettings& get_settings() const { return settings_; }

    /**
     * @brief Get mutable camera settings reference for updates
     */
    CameraSettings& get_settings_mut() { return settings_; }

    /**
     * @brief Save current settings to NVS
     */
    esp_err_t save_settings();

    /**
     * @brief Apply current settings to camera hardware (OV5647 + ISP)
     */
    void apply_to_camera();

private:
    void init_nvs();
    void load_settings();

    CameraSettings settings_;
};

// Global instance pointer (created in main)
extern AppParams* g_app_params;

#endif // __cplusplus
