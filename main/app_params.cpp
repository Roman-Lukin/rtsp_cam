#include "app_params.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "camera_interface.h"
#include "imx219_helper.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"      // For V4L2_CID_CAMERA_GROUP
#include "esp_video_isp_ioctl.h"
#include "esp_cam_sensor_types.h"  // For esp_cam_sensor_gh_exp_gain_t

namespace {
    constexpr const char* TAG = "APP_PARAMS";
    constexpr const char* NVS_NAMESPACE = "cam_settings";
    constexpr uint32_t SETTINGS_VERSION = 5;  // Increment to reset settings on update
}

// Global instance pointer
AppParams* g_app_params = nullptr;

// Default settings for IMX219 sensor
// Exposure: 4-1760 (VTS - 4), where VTS=1764 for 1280x960  
// Analog Gain: 0-232, where real_gain = 256/(256-val)
//   0 = 1x, 128 = 2x, 192 = 4x, 224 = 8x, 232 = 10.67x
CameraSettings app_params_default_settings(void) {
    CameraSettings s = {};
    s.auto_exposure = false;     // IMX219 has no built-in AEC, use manual exposure
    s.exposure_value = 1500;     // IMX219: near max exposure for indoor lighting (4-1760 lines)
    s.gain = 200;                // IMX219: analog gain ~5x (0-232)
    s.auto_white_balance = true;
    s.wb_red_gain = 0x400;
    s.wb_green_gain = 0x400;
    s.wb_blue_gain = 0x400;
    s.test_pattern = 0;          // IMX219: 0=disabled, 1=solid, 2=color bars, 3=grey, 4=PN9
    s.denoise_enable = true;
    s.denoise_level = 10;
    s.power_line_freq = 1;       // 50Hz (Europe)
    s.bitrate = 4000;            // kbps for 1280x960
    s.gop = 30;                  // GOP = 1 second at 30fps
    s.min_qp = 20;
    s.max_qp = 40;
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
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved settings found, using defaults");
        return;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", ret);
        return;
    }
    
    // Check settings version - reset to defaults if version changed
    uint32_t stored_version = 0;
    if (nvs_get_u32(handle, "version", &stored_version) != ESP_OK || stored_version != SETTINGS_VERSION) {
        ESP_LOGW(TAG, "Settings version changed (%lu -> %lu), resetting to defaults", 
                 stored_version, SETTINGS_VERSION);
        nvs_erase_all(handle);
        nvs_set_u32(handle, "version", SETTINGS_VERSION);
        nvs_commit(handle);
        nvs_close(handle);
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
    
    // Save settings version
    nvs_set_u32(handle, "version", SETTINGS_VERSION);
    
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
    ESP_LOGW(TAG, "=== Applying camera settings ===");
    ESP_LOGW(TAG, "  auto_exposure=%d, exposure=%d, gain=%d", 
             settings_.auto_exposure, settings_.exposure_value, settings_.gain);
    ESP_LOGW(TAG, "  auto_wb=%d, wb_r=%d, wb_g=%d, wb_b=%d",
             settings_.auto_white_balance, settings_.wb_red_gain, 
             settings_.wb_green_gain, settings_.wb_blue_gain);
    
    // Check for direct bypass support (IMX219 with atomic Group Hold)
    bool direct_bypass = false;
    if (g_camera && strcmp(g_camera->getCapabilities().name, "IMX219") == 0) {
        direct_bypass = true;
        
        // For IMX219: DO NOT use direct I2C writes during streaming!
        // ISP Pipeline controls exposure/gain through ESP_CAM_SENSOR_GROUP_EXP_GAIN
        // Direct I2C writes conflict with ISP and cause stream to freeze
        if (!settings_.auto_exposure) {
            ESP_LOGW(TAG, "IMX219: Manual exposure - will use V4L2 below (NOT direct I2C)");
            // Don't call imx219_set_exposure_gain_atomic() - it conflicts with ISP!
            // The V4L2 path below will handle it properly through the driver
        } else {
            ESP_LOGW(TAG, "IMX219: Auto exposure enabled - ISP/IPA controls exposure/gain");
        }
    }

    // ===== Sensor Settings via V4L2 (through /dev/video0) =====
    // Use V4L2_CID_CAMERA_GROUP for atomic exposure+gain updates with Group Hold
    // This avoids the stream freeze issue caused by separate exposure/gain writes
    int cam_fd = open("/dev/video0", O_RDWR);
    if (cam_fd >= 0) {
        // Set exposure and gain - only if manual mode
        if (!settings_.auto_exposure) {
            // Use GROUP for atomic update (uses Group Hold internally)
            esp_cam_sensor_gh_exp_gain_t group_params = {};
            group_params.exposure_val = settings_.exposure_value;
            group_params.exposure_us = 0;  // Use exposure_val directly
            group_params.gain_index = settings_.gain;  // gain index 0-232
            
            struct v4l2_ext_controls controls = {};
            struct v4l2_ext_control control[1] = {};
            controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
            controls.count = 1;
            controls.controls = control;
            control[0].id = V4L2_CID_CAMERA_GROUP;
            control[0].p_u8 = (uint8_t*)&group_params;
            control[0].size = sizeof(group_params);
            
            int ret = ioctl(cam_fd, VIDIOC_S_EXT_CTRLS, &controls);
            if (ret == 0) {
                float real_gain = 256.0f / (256.0f - settings_.gain);
                ESP_LOGI(TAG, "V4L2 GROUP: exposure=%d, gain=%d (%.2fx)", 
                         settings_.exposure_value, settings_.gain, real_gain);
            } else {
                ESP_LOGW(TAG, "V4L2 GROUP failed: %d (errno=%d) - trying separate writes", ret, errno);
                
                // Fallback to separate writes (may cause issues during streaming)
                controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
                controls.count = 1;
                control[0].id = V4L2_CID_EXPOSURE;
                control[0].value = settings_.exposure_value;
                ret = ioctl(cam_fd, VIDIOC_S_EXT_CTRLS, &controls);
                if (ret == 0) {
                    ESP_LOGI(TAG, "V4L2 Exposure set to %d lines", settings_.exposure_value);
                }
                
                controls.ctrl_class = V4L2_CID_USER_CLASS;
                control[0].id = V4L2_CID_GAIN;
                control[0].value = settings_.gain;
                ret = ioctl(cam_fd, VIDIOC_S_EXT_CTRLS, &controls);
                if (ret == 0) {
                    float real_gain = 256.0f / (256.0f - settings_.gain);
                    ESP_LOGI(TAG, "V4L2 Gain set to %d (%.2fx)", settings_.gain, real_gain);
                }
            }
        } else {
            ESP_LOGI(TAG, "Auto exposure enabled - ISP/IPA controls exposure/gain");
        }
        
        close(cam_fd);
    } else {
        ESP_LOGW(TAG, "Failed to open /dev/video0 for exposure/gain control");
    }
    
    // ===== ISP White Balance (via V4L2) =====
    // Note: ISP Pipeline with IPA algorithm automatically handles AWB when enabled
    // We only need to set manual WB gains if auto_white_balance is disabled
    int isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (isp_fd >= 0) {
        if (settings_.auto_white_balance) {
            // Auto WB - disable manual WB override to let IPA take control
            esp_video_isp_wb_t wb_config = {
                .enable = false,  // ← Disable manual WB override
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
            ESP_LOGI(TAG, "Auto WB enabled - reset ISP manual WB override");
        } 
        else {
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
