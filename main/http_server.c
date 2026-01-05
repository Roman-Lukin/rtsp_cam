#include "http_server.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "esp_video_isp_ioctl.h"
#include "ov5647_helper.h"

static const char *TAG = "HTTP_SERVER";

// Embedded HTML file (from EMBED_FILES in CMakeLists.txt)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t server = NULL;
static camera_settings_t current_settings = {0};
static settings_change_cb_t settings_callback = NULL;

void http_server_set_settings_callback(settings_change_cb_t callback)
{
    settings_callback = callback;
}

// Load current camera settings
// Note: OV5647 sensor settings are stored in memory (not read back from sensor)
// ISP settings can be read from V4L2
static void load_current_settings(void)
{
    // Set defaults for sensor settings (stored in memory)
    // These will be overwritten when user changes them
    static bool initialized = false;
    if (!initialized) {
        current_settings.auto_exposure = true;
        current_settings.exposure_value = 200;
        current_settings.gain = 16;
        current_settings.auto_white_balance = true;
        current_settings.wb_red_gain = 0x400;
        current_settings.wb_green_gain = 0x400;
        current_settings.wb_blue_gain = 0x400;
        current_settings.test_pattern = 0;
        current_settings.power_line_freq = 1;  // 50Hz default
        current_settings.bitrate = 6000;
        current_settings.gop = 25;
        current_settings.min_qp = 20;
        current_settings.max_qp = 35;
        current_settings.denoise_enable = true;
        current_settings.denoise_level = 10;
        initialized = true;
    }
    // All settings stored in memory - V4L2/ISP controls not supported by OV5647 driver
}

// Set ISP White Balance via V4L2 device
static esp_err_t set_isp_white_balance(bool enable, float red_gain, float blue_gain)
{
    int isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (isp_fd < 0) {
        ESP_LOGE(TAG, "Failed to open ISP device %s", ESP_VIDEO_ISP1_DEVICE_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_video_isp_wb_t wb_config = {
        .enable = enable,
        .red_gain = red_gain,
        .blue_gain = blue_gain
    };
    
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_USER_CLASS,
        .count = 1,
    };
    struct v4l2_ext_control control = {
        .id = V4L2_CID_USER_ESP_ISP_WB,
        .size = sizeof(esp_video_isp_wb_t),
        .p_u8 = (uint8_t *)&wb_config
    };
    controls.controls = &control;
    
    int ret = ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &controls);
    close(isp_fd);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set ISP WB: %d", ret);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "ISP WB set: enable=%d, R=%.2f, B=%.2f", enable, red_gain, blue_gain);
    return ESP_OK;
}

// Apply camera settings via I2C (OV5647) and V4L2 (ISP)
static esp_err_t apply_camera_settings(camera_settings_t *settings)
{
    // ===== OV5647 Sensor Settings (via I2C) =====
    
    // Auto Exposure / Auto Gain
    ov5647_set_auto_exposure(settings->auto_exposure);
    
    // Manual Exposure and Gain (only if auto is disabled)
    if (!settings->auto_exposure) {
        ov5647_set_exposure(settings->exposure_value);
        ov5647_set_gain(settings->gain);
    }
    
    // ===== ISP White Balance (via V4L2) =====
    // ISP Pipeline Controller nadpisuje rejestry sensora, 
    // więc musimy sterować przez ISP device
    if (settings->auto_white_balance) {
        // Auto WB - wyłącz manualny ISP WB, pozwól ISP Pipeline sterować
        set_isp_white_balance(false, 1.0f, 1.0f);
    } else {
        // Manual WB - ustaw gainy w ISP
        // Konwersja z zakresu UI (256-4095, gdzie 1024=1.0) na float
        float red_gain = (float)settings->wb_red_gain / 1024.0f;
        float blue_gain = (float)settings->wb_blue_gain / 1024.0f;
        // Green gain jest reference (1.0), red/blue są relative
        set_isp_white_balance(true, red_gain, blue_gain);
    }
    
    // Test Pattern (sensor register)
    ov5647_set_test_pattern(settings->test_pattern);
    
    // Note: ISP Bayer Filter (Denoise) not supported
    // Settings stored in memory only
    
    // Update current settings
    current_settings = *settings;
    
    // Notify callback for encoder settings
    if (settings_callback) {
        settings_callback(settings);
    }
    
    ESP_LOGI(TAG, "Camera settings applied");
    return ESP_OK;
}

// HTTP Handlers
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t get_settings_handler(httpd_req_t *req)
{
    load_current_settings();
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "width", VIDEO_WIDTH);
    cJSON_AddNumberToObject(json, "height", VIDEO_HEIGHT);
    cJSON_AddNumberToObject(json, "fps", VIDEO_FPS);
    cJSON_AddBoolToObject(json, "auto_exposure", current_settings.auto_exposure);
    cJSON_AddNumberToObject(json, "exposure_value", current_settings.exposure_value);
    cJSON_AddNumberToObject(json, "gain", current_settings.gain);
    cJSON_AddBoolToObject(json, "auto_white_balance", current_settings.auto_white_balance);
    cJSON_AddNumberToObject(json, "wb_red_gain", current_settings.wb_red_gain);
    cJSON_AddNumberToObject(json, "wb_green_gain", current_settings.wb_green_gain);
    cJSON_AddNumberToObject(json, "wb_blue_gain", current_settings.wb_blue_gain);
    cJSON_AddNumberToObject(json, "test_pattern", current_settings.test_pattern);
    cJSON_AddBoolToObject(json, "denoise_enable", current_settings.denoise_enable);
    cJSON_AddNumberToObject(json, "denoise_level", current_settings.denoise_level);
    cJSON_AddNumberToObject(json, "power_line_freq", current_settings.power_line_freq);
    cJSON_AddNumberToObject(json, "bitrate", current_settings.bitrate);
    cJSON_AddNumberToObject(json, "gop", current_settings.gop);
    cJSON_AddNumberToObject(json, "min_qp", current_settings.min_qp);
    cJSON_AddNumberToObject(json, "max_qp", current_settings.max_qp);
    
    char *response = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    
    free(response);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t post_settings_handler(httpd_req_t *req)
{
    char buf[512];
    int ret, remaining = req->content_len;
    
    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }
    
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    camera_settings_t new_settings = current_settings;
    
    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "auto_exposure"))) 
        new_settings.auto_exposure = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(json, "exposure_value"))) 
        new_settings.exposure_value = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "gain"))) 
        new_settings.gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "auto_white_balance"))) 
        new_settings.auto_white_balance = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(json, "wb_red_gain"))) 
        new_settings.wb_red_gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "wb_green_gain"))) 
        new_settings.wb_green_gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "wb_blue_gain"))) 
        new_settings.wb_blue_gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "test_pattern"))) 
        new_settings.test_pattern = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "denoise_enable"))) 
        new_settings.denoise_enable = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(json, "denoise_level"))) 
        new_settings.denoise_level = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "power_line_freq"))) 
        new_settings.power_line_freq = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "bitrate"))) 
        new_settings.bitrate = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "gop"))) 
        new_settings.gop = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "min_qp"))) 
        new_settings.min_qp = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "max_qp"))) 
        new_settings.max_qp = item->valueint;
    
    cJSON_Delete(json);
    
    esp_err_t err = apply_camera_settings(&new_settings);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(response, "error", "Failed to apply settings");
    }
    
    char *resp_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    
    free(resp_str);
    cJSON_Delete(response);
    return ESP_OK;
}

esp_err_t http_server_start(void)
{
    if (server) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }
    
    // Load initial settings
    load_current_settings();
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", ret);
        return ret;
    }
    
    // Register URI handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(server, &index_uri);
    
    httpd_uri_t get_settings_uri = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = get_settings_handler,
    };
    httpd_register_uri_handler(server, &get_settings_uri);
    
    httpd_uri_t post_settings_uri = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = post_settings_handler,
    };
    httpd_register_uri_handler(server, &post_settings_uri);
    
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (!server) {
        return ESP_OK;
    }
    
    esp_err_t ret = httpd_stop(server);
    server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
    return ret;
}
