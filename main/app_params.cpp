#include "app_params.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "ov5647_helper.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "esp_video_isp_ioctl.h"

namespace {
    constexpr const char* TAG = "APP_PARAMS";
    constexpr const char* NVS_NAMESPACE = "cam_settings";
}

// Global instance pointer
AppParams* g_app_params = nullptr;

// Default settings
CameraSettings app_params_default_settings(void) {
    CameraSettings s = {};
    s.auto_exposure = true;
    s.exposure_value = 200;
    s.gain = 16;
    s.auto_white_balance = true;
    s.wb_red_gain = 0x400;
    s.wb_green_gain = 0x400;
    s.wb_blue_gain = 0x400;
    s.test_pattern = 0;
    s.denoise_enable = true;
    s.denoise_level = 10;
    s.power_line_freq = 1;
    s.bitrate = 6000;
    s.gop = 25;
    s.min_qp = 20;
    s.max_qp = 35;
    return s;
}

AppParams::AppParams() : settings_(app_params_default_settings()) {
    init_nvs();
    load_settings();
    g_app_params = this;
}

AppParams::~AppParams() {
    if (g_app_params == this) {
        g_app_params = nullptr;
    }
}

void AppParams::init_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated and needs to be erased");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS Initialized");
}

void AppParams::load_settings() {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved settings found, using defaults");
        return;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", ret);
        return;
    }
    
    // Load each setting (use defaults if not found)
    uint8_t u8val;
    int32_t i32val;
    
    if (nvs_get_u8(handle, "auto_exp", &u8val) == ESP_OK)
        settings_.auto_exposure = u8val;
    if (nvs_get_i32(handle, "exp_val", &i32val) == ESP_OK)
        settings_.exposure_value = i32val;
    if (nvs_get_i32(handle, "gain", &i32val) == ESP_OK)
        settings_.gain = i32val;
    
    if (nvs_get_u8(handle, "auto_wb", &u8val) == ESP_OK)
        settings_.auto_white_balance = u8val;
    if (nvs_get_i32(handle, "wb_r", &i32val) == ESP_OK)
        settings_.wb_red_gain = i32val;
    if (nvs_get_i32(handle, "wb_g", &i32val) == ESP_OK)
        settings_.wb_green_gain = i32val;
    if (nvs_get_i32(handle, "wb_b", &i32val) == ESP_OK)
        settings_.wb_blue_gain = i32val;
    
    if (nvs_get_i32(handle, "test_pat", &i32val) == ESP_OK)
        settings_.test_pattern = i32val;
    
    if (nvs_get_u8(handle, "denoise_en", &u8val) == ESP_OK)
        settings_.denoise_enable = u8val;
    if (nvs_get_i32(handle, "denoise_lv", &i32val) == ESP_OK)
        settings_.denoise_level = i32val;
    
    if (nvs_get_i32(handle, "pwr_freq", &i32val) == ESP_OK)
        settings_.power_line_freq = i32val;
    
    if (nvs_get_i32(handle, "bitrate", &i32val) == ESP_OK)
        settings_.bitrate = i32val;
    if (nvs_get_i32(handle, "gop", &i32val) == ESP_OK)
        settings_.gop = i32val;
    if (nvs_get_i32(handle, "min_qp", &i32val) == ESP_OK)
        settings_.min_qp = i32val;
    if (nvs_get_i32(handle, "max_qp", &i32val) == ESP_OK)
        settings_.max_qp = i32val;
    
    nvs_close(handle);
    ESP_LOGI(TAG, "Settings loaded from NVS");
}

esp_err_t AppParams::save_settings() {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %d", ret);
        return ret;
    }
    
    // Save all settings
    nvs_set_u8(handle, "auto_exp", settings_.auto_exposure ? 1 : 0);
    nvs_set_i32(handle, "exp_val", settings_.exposure_value);
    nvs_set_i32(handle, "gain", settings_.gain);
    
    nvs_set_u8(handle, "auto_wb", settings_.auto_white_balance ? 1 : 0);
    nvs_set_i32(handle, "wb_r", settings_.wb_red_gain);
    nvs_set_i32(handle, "wb_g", settings_.wb_green_gain);
    nvs_set_i32(handle, "wb_b", settings_.wb_blue_gain);
    
    nvs_set_i32(handle, "test_pat", settings_.test_pattern);
    
    nvs_set_u8(handle, "denoise_en", settings_.denoise_enable ? 1 : 0);
    nvs_set_i32(handle, "denoise_lv", settings_.denoise_level);
    
    nvs_set_i32(handle, "pwr_freq", settings_.power_line_freq);
    
    nvs_set_i32(handle, "bitrate", settings_.bitrate);
    nvs_set_i32(handle, "gop", settings_.gop);
    nvs_set_i32(handle, "min_qp", settings_.min_qp);
    nvs_set_i32(handle, "max_qp", settings_.max_qp);
    
    ret = nvs_commit(handle);
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Settings saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %d", ret);
    }
    
    return ret;
}

void AppParams::apply_to_camera() {
    ESP_LOGI(TAG, "Applying camera settings...");
    
    // ===== OV5647 Sensor Settings (via I2C) =====
    
    // Auto Exposure / Auto Gain
    ov5647_set_auto_exposure(settings_.auto_exposure);
    
    // Manual Exposure and Gain (only if auto is disabled)
    if (!settings_.auto_exposure) {
        ov5647_set_exposure(settings_.exposure_value);
        ov5647_set_gain(settings_.gain);
    }
    
    // Test Pattern (sensor register)
    ov5647_set_test_pattern(settings_.test_pattern);
    
    // ===== ISP White Balance (via V4L2) =====
    int isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (isp_fd >= 0) {
        if (settings_.auto_white_balance) {
            // Auto WB - disable manual ISP WB
            esp_video_isp_wb_t wb_config = {
                .enable = false,
                .red_gain = 1.0f,
                .blue_gain = 1.0f
            };
            
            struct v4l2_ext_control control = {
                .id = V4L2_CID_USER_ESP_ISP_WB,
                .size = sizeof(esp_video_isp_wb_t),
            };
            control.p_u8 = reinterpret_cast<uint8_t*>(&wb_config);
            
            struct v4l2_ext_controls controls = {
                .ctrl_class = V4L2_CID_USER_CLASS,
                .count = 1,
            };
            controls.controls = &control;
            
            ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &controls);
        } else {
            // Manual WB - set gains in ISP
            float red_gain = static_cast<float>(settings_.wb_red_gain) / 1024.0f;
            float blue_gain = static_cast<float>(settings_.wb_blue_gain) / 1024.0f;
            
            esp_video_isp_wb_t wb_config = {
                .enable = true,
                .red_gain = red_gain,
                .blue_gain = blue_gain
            };
            
            struct v4l2_ext_control control = {
                .id = V4L2_CID_USER_ESP_ISP_WB,
                .size = sizeof(esp_video_isp_wb_t),
            };
            control.p_u8 = reinterpret_cast<uint8_t*>(&wb_config);
            
            struct v4l2_ext_controls controls = {
                .ctrl_class = V4L2_CID_USER_CLASS,
                .count = 1,
            };
            controls.controls = &control;
            
            ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &controls);
        }
        
        // ISP Bayer Filter (Denoise)
        uint8_t level = settings_.denoise_level;
        if (level < 2) level = 2;
        if (level > 20) level = 20;
        
        esp_video_isp_bf_t bf_config = {
            .enable = settings_.denoise_enable,
            .level = level,
            .matrix = {
                {1, 2, 1},
                {2, 4, 2},
                {1, 2, 1}
            }
        };
        
        struct v4l2_ext_control bf_control = {
            .id = V4L2_CID_USER_ESP_ISP_BF,
            .size = sizeof(esp_video_isp_bf_t),
        };
        bf_control.p_u8 = reinterpret_cast<uint8_t*>(&bf_config);
        
        struct v4l2_ext_controls bf_controls = {
            .ctrl_class = V4L2_CID_USER_CLASS,
            .count = 1,
        };
        bf_controls.controls = &bf_control;
        
        int bf_ret = ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &bf_controls);
        if (bf_ret != 0) {
            ESP_LOGE(TAG, "Failed to set ISP BF (denoise): ret=%d, errno=%d", bf_ret, errno);
        } else {
            ESP_LOGI(TAG, "ISP Denoise: enable=%d, level=%d", settings_.denoise_enable, level);
        }
        
        close(isp_fd);
    } else {
        ESP_LOGW(TAG, "Failed to open ISP device for settings");
    }
    
    ESP_LOGI(TAG, "Camera settings applied");
}

// ============= C-compatible API =============

extern "C" const CameraSettings* app_params_get_settings(void) {
    if (g_app_params) {
        return &g_app_params->get_settings();
    }
    return nullptr;
}

extern "C" CameraSettings* app_params_get_settings_mut(void) {
    if (g_app_params) {
        return &g_app_params->get_settings_mut();
    }
    return nullptr;
}

extern "C" esp_err_t app_params_apply_and_save(void) {
    if (!g_app_params) {
        return ESP_ERR_INVALID_STATE;
    }
    g_app_params->apply_to_camera();
    return g_app_params->save_settings();
}
