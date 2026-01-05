#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Camera settings structure
 */
typedef struct {
    // Exposure settings
    bool auto_exposure;
    int exposure_value;      // 1-1000 (in 100µs units)
    int gain;                // 0-255
    
    // White Balance
    bool auto_white_balance;
    int wb_red_gain;         // Manual WB gains (if AWB disabled) 0x000-0xFFF
    int wb_green_gain;
    int wb_blue_gain;
    
    // Test pattern
    int test_pattern;        // 0=off, 0x80=bars, 0x82=squares
    
    // ISP settings
    bool denoise_enable;
    int denoise_level;       // 2-20
    
    // Anti-flicker
    int power_line_freq;     // 0=disabled, 1=50Hz, 2=60Hz
    
    // H.264 encoder settings
    int bitrate;             // in kbps
    int gop;                 // GOP size (frames)
    int min_qp;              // 0-51
    int max_qp;              // 0-51
} camera_settings_t;

/**
 * @brief Get current camera settings
 */
camera_settings_t http_server_get_settings(void);

/**
 * @brief Start HTTP server for camera settings
 * 
 * @return ESP_OK on success
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop HTTP server
 * 
 * @return ESP_OK on success
 */
esp_err_t http_server_stop(void);

/**
 * @brief Set callback for settings change
 * 
 * @param callback Function to call when settings change
 */
typedef void (*settings_change_cb_t)(const camera_settings_t *settings);
void http_server_set_settings_callback(settings_change_cb_t callback);

#ifdef __cplusplus
}
#endif
