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
#include <linux/videodev2.h>
#include "esp_video_isp_ioctl.h"

extern int get_camera_fd(void);

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

// Load current camera settings from V4L2
static void load_current_settings(void)
{
    int fd = get_camera_fd();
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to get camera FD");
        return;
    }
    
    struct v4l2_control ctrl;
    
    // Auto exposure
    ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0) {
        current_settings.auto_exposure = (ctrl.value == V4L2_EXPOSURE_AUTO);
    }
    
    // Exposure value
    ctrl.id = V4L2_CID_EXPOSURE;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0) {
        current_settings.exposure_value = ctrl.value;
    }
    
    // Gain
    ctrl.id = V4L2_CID_GAIN;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0) {
        current_settings.gain = ctrl.value;
    }
    
    // Auto white balance
    ctrl.id = V4L2_CID_AUTO_WHITE_BALANCE;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0) {
        current_settings.auto_white_balance = ctrl.value;
    }
    
    // Power line frequency
    ctrl.id = V4L2_CID_POWER_LINE_FREQUENCY;
    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0) {
        current_settings.power_line_freq = ctrl.value;
    }
    
    // ISP Bayer Filter (Denoise)
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control ctrl_ext[1];
    esp_video_isp_bf_t bf_cfg;
    
    memset(&ctrls, 0, sizeof(ctrls));
    memset(ctrl_ext, 0, sizeof(ctrl_ext));
    
    ctrls.ctrl_class = V4L2_CID_USER_CLASS;
    ctrls.count = 1;
    ctrls.controls = ctrl_ext;
    ctrl_ext[0].id = V4L2_CID_USER_ESP_ISP_BF;
    ctrl_ext[0].size = sizeof(esp_video_isp_bf_t);
    ctrl_ext[0].p_u8 = (uint8_t *)&bf_cfg;
    
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) == 0) {
        current_settings.denoise_enable = bf_cfg.enable;
        current_settings.denoise_level = bf_cfg.level;
    }
    
    // close(fd); // Do not close shared FD
    
    // Default encoder settings if not set
    if (current_settings.bitrate == 0) {
        current_settings.bitrate = 6000;
        current_settings.gop = 25;
        current_settings.min_qp = 20;
        current_settings.max_qp = 35;
    }
}

// Apply camera settings via V4L2
static esp_err_t apply_camera_settings(camera_settings_t *settings)
{
    int fd = get_camera_fd();
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to get camera FD");
        return ESP_FAIL;
    }
    
    struct v4l2_control ctrl;
    
    // Auto exposure
    ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    ctrl.value = settings->auto_exposure ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL;
    ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    
    // Exposure value (only if manual)
    if (!settings->auto_exposure) {
        ctrl.id = V4L2_CID_EXPOSURE;
        ctrl.value = settings->exposure_value;
        ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    }
    
    // Gain
    ctrl.id = V4L2_CID_GAIN;
    ctrl.value = settings->gain;
    ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    
    // Auto white balance
    ctrl.id = V4L2_CID_AUTO_WHITE_BALANCE;
    ctrl.value = settings->auto_white_balance;
    ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    
    // Power line frequency
    ctrl.id = V4L2_CID_POWER_LINE_FREQUENCY;
    ctrl.value = settings->power_line_freq;
    ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    
    // ISP Bayer Filter (Denoise)
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control ctrl_ext[1];
    esp_video_isp_bf_t bf_cfg;
    
    memset(&ctrls, 0, sizeof(ctrls));
    memset(ctrl_ext, 0, sizeof(ctrl_ext));
    
    ctrls.ctrl_class = V4L2_CID_USER_CLASS;
    ctrls.count = 1;
    ctrls.controls = ctrl_ext;
    ctrl_ext[0].id = V4L2_CID_USER_ESP_ISP_BF;
    ctrl_ext[0].size = sizeof(esp_video_isp_bf_t);
    ctrl_ext[0].p_u8 = (uint8_t *)&bf_cfg;
    
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) == 0) {
        bf_cfg.enable = settings->denoise_enable;
        bf_cfg.level = settings->denoise_level;
        ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);
    }
    
    // close(fd); // Do not close shared FD
    
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
