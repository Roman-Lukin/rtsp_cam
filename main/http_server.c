#include "http_server.h"
#include "settings.h"
#include "app_params.h"
#include "camera_interface.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HTTP_SERVER";

// Embedded HTML file (from EMBED_FILES in CMakeLists.txt)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t server = NULL;
static settings_change_cb_t settings_callback = NULL;

void http_server_set_settings_callback(settings_change_cb_t callback)
{
    settings_callback = callback;
}

// Helper to convert CameraSettings to camera_settings_t for callback
static camera_settings_t convert_settings(void)
{
    camera_settings_t settings = {0};
    const CameraSettings* s = app_params_get_settings();
    if (s) {
        settings.auto_exposure = s->auto_exposure;
        settings.exposure_value = s->exposure_value;
        settings.gain = s->gain;
        settings.auto_white_balance = s->auto_white_balance;
        settings.wb_red_gain = s->wb_red_gain;
        settings.wb_green_gain = s->wb_green_gain;
        settings.wb_blue_gain = s->wb_blue_gain;
        settings.test_pattern = s->test_pattern;
        settings.denoise_enable = s->denoise_enable;
        settings.denoise_level = s->denoise_level;
        settings.power_line_freq = s->power_line_freq;
        settings.bitrate = s->bitrate;
        settings.gop = s->gop;
        settings.min_qp = s->min_qp;
        settings.max_qp = s->max_qp;
    }
    return settings;
}

// Apply settings from app_params to camera hardware and notify encoder callback
static esp_err_t apply_camera_settings(void)
{
    // Apply to camera hardware (OV5647 + ISP) and save to NVS
    esp_err_t ret = app_params_apply_and_save();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply settings: %d", ret);
        return ret;
    }
    
    // Notify callback for encoder settings
    if (settings_callback) {
        camera_settings_t settings = convert_settings();
        settings_callback(&settings);
    }
    
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
    const CameraSettings* s = app_params_get_settings();
    if (!s) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Settings not initialized");
        return ESP_FAIL;
    }
    
    cJSON *json = cJSON_CreateObject();
    
    // Add sensor name from detected camera (using C API)
    const char* sensor_name = camera_get_name();
    cJSON_AddStringToObject(json, "sensor_name", sensor_name);
    
    cJSON_AddNumberToObject(json, "width", VIDEO_WIDTH);
    cJSON_AddNumberToObject(json, "height", VIDEO_HEIGHT);
    cJSON_AddNumberToObject(json, "fps", VIDEO_FPS);
    cJSON_AddBoolToObject(json, "auto_exposure", s->auto_exposure);
    cJSON_AddNumberToObject(json, "exposure_value", s->exposure_value);
    cJSON_AddNumberToObject(json, "gain", s->gain);
    cJSON_AddBoolToObject(json, "auto_white_balance", s->auto_white_balance);
    cJSON_AddNumberToObject(json, "wb_red_gain", s->wb_red_gain);
    cJSON_AddNumberToObject(json, "wb_green_gain", s->wb_green_gain);
    cJSON_AddNumberToObject(json, "wb_blue_gain", s->wb_blue_gain);
    cJSON_AddNumberToObject(json, "test_pattern", s->test_pattern);
    cJSON_AddBoolToObject(json, "denoise_enable", s->denoise_enable);
    cJSON_AddNumberToObject(json, "denoise_level", s->denoise_level);
    cJSON_AddNumberToObject(json, "power_line_freq", s->power_line_freq);
    cJSON_AddNumberToObject(json, "bitrate", s->bitrate);
    cJSON_AddNumberToObject(json, "gop", s->gop);
    cJSON_AddNumberToObject(json, "min_qp", s->min_qp);
    cJSON_AddNumberToObject(json, "max_qp", s->max_qp);
    
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
    
    CameraSettings* s = app_params_get_settings_mut();
    if (!s) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Settings not initialized");
        return ESP_FAIL;
    }
    
    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "auto_exposure"))) 
        s->auto_exposure = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(json, "exposure_value"))) 
        s->exposure_value = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "gain"))) 
        s->gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "auto_white_balance"))) 
        s->auto_white_balance = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(json, "wb_red_gain"))) 
        s->wb_red_gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "wb_green_gain"))) 
        s->wb_green_gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "wb_blue_gain"))) 
        s->wb_blue_gain = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "test_pattern"))) 
        s->test_pattern = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "denoise_enable"))) 
        s->denoise_enable = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(json, "denoise_level"))) 
        s->denoise_level = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "power_line_freq"))) 
        s->power_line_freq = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "bitrate"))) 
        s->bitrate = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "gop"))) 
        s->gop = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "min_qp"))) 
        s->min_qp = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "max_qp"))) 
        s->max_qp = item->valueint;
    
    cJSON_Delete(json);
    
    esp_err_t err = apply_camera_settings();
    
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
