#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "network.h"

#include "codec_board.h"
#include "codec_init.h"

#include "esp_video_init.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_video_enc_default.h"

static const char *TAG = "RTSP_CAM";

static esp_capture_video_src_if_t *video_src = NULL;

static int init_camera(void)
{
    camera_cfg_t cam_pin_cfg = {};
    int ret = get_camera_cfg(&cam_pin_cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to get camera config");
        return -1;
    }

    esp_video_init_csi_config_t csi_config = { 0 };
    esp_video_init_config_t cam_config = { 0 };

    if (cam_pin_cfg.type == CAMERA_TYPE_MIPI) {
        csi_config.sccb_config.i2c_handle = get_i2c_bus_handle(0);
        csi_config.sccb_config.freq = 100000;
        csi_config.reset_pin = cam_pin_cfg.reset;
        csi_config.pwdn_pin = cam_pin_cfg.pwr;
        cam_config.csi = &csi_config;
    } else {
        ESP_LOGE(TAG, "Only MIPI camera supported for now");
        return -1;
    }

    ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", ret);
        return -1;
    }

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .dev_name = "/dev/video0",
        .buf_count = 2,
    };
    video_src = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (video_src == NULL) {
        ESP_LOGE(TAG, "Failed to create video source");
        return -1;
    }
    
    ESP_LOGI(TAG, "Camera initialized successfully");
    return 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting RTSP Camera Application");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Network
    // ESP_ERROR_CHECK(esp_netif_init()); // Called inside network_init
    // ESP_ERROR_CHECK(esp_event_loop_create_default()); // Called inside network_init
    ESP_ERROR_CHECK(network_init());

    // Initialize Board (Codec, Camera Power, etc.)
    ESP_LOGI(TAG, "Initializing Board...");
    set_codec_board_type(TEST_BOARD_NAME);
    codec_init_cfg_t cfg = {.reuse_dev = false};
    init_codec(&cfg);

    // Initialize Camera
    ESP_LOGI(TAG, "Initializing Camera...");
    if (init_camera() != 0) {
        ESP_LOGE(TAG, "Camera initialization failed");
        return;
    }
    
    // Initialize H.264 Encoder
    ESP_LOGI(TAG, "Initializing H.264 Encoder...");
    esp_video_enc_register_default();

    // Start RTSP Server
    ESP_LOGI(TAG, "Starting RTSP Server...");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
