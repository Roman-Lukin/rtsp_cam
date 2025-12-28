#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "network.h"
#include "settings.h"

#include "codec_board.h"
#include "codec_init.h"

#include "esp_video_init.h"
#include "esp_capture.h"
#include "esp_capture_types.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"

#include "rtsp_server.h"

static const char *TAG = "RTSP_CAM";

// Capture system handles
static esp_capture_handle_t capture_handle = NULL;
static esp_capture_video_src_if_t *video_src = NULL;
static esp_capture_sink_handle_t capture_sink = NULL;
static rtsp_server_handle_t rtsp_server = NULL;

// Frame capture task
static TaskHandle_t capture_task_handle = NULL;
static volatile bool capture_running = false;

/**
 * @brief Create video source for MIPI camera on ESP32-P4
 */
static esp_capture_video_src_if_t *create_video_source(void)
{
    camera_cfg_t cam_pin_cfg = {};
    int ret = get_camera_cfg(&cam_pin_cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to get camera config");
        return NULL;
    }

    if (cam_pin_cfg.type != CAMERA_TYPE_MIPI) {
        ESP_LOGE(TAG, "Only MIPI camera supported on ESP32-P4");
        return NULL;
    }

    // Initialize V4L2/camera subsystem
    esp_video_init_csi_config_t csi_config = {0};
    esp_video_init_config_t cam_config = {0};

    csi_config.sccb_config.i2c_handle = get_i2c_bus_handle(0);
    csi_config.sccb_config.freq = 100000;
    csi_config.reset_pin = cam_pin_cfg.reset;
    csi_config.pwdn_pin = cam_pin_cfg.pwr;
    cam_config.csi = &csi_config;

    ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", ret);
        return NULL;
    }

    // Create V4L2 video source
    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .dev_name = "/dev/video0",
        .buf_count = 4,  // Use 4 buffers for smooth streaming
    };
    
    return esp_capture_new_video_v4l2_src(&v4l2_cfg);
}

/**
 * @brief Task to continuously acquire H.264 frames and feed to RTSP server
 */
static void capture_task(void *pvParameters)
{
    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    
    ESP_LOGI(TAG, "Capture task started");
    
    while (capture_running) {
        // Acquire frame (blocking with timeout)
        esp_capture_err_t ret = esp_capture_sink_acquire_frame(capture_sink, &frame, false);
        
        if (ret == ESP_CAPTURE_ERR_OK) {
            if (frame.data && frame.size > 0) {
                // Feed H.264 frame to RTSP server
                // Check if frame starts with NAL unit (keyframe detection)
                bool is_keyframe = false;
                if (frame.size >= 5) {
                    // Check for IDR NAL type (0x65) or SPS (0x67)
                    uint8_t nal_type = frame.data[4] & 0x1F;
                    is_keyframe = (nal_type == 5 || nal_type == 7);
                }
                
                rtsp_server_feed_frame(rtsp_server, frame.data, frame.size, 
                                       frame.pts, is_keyframe);
            }
            
            // Release frame back to capture system
            esp_capture_sink_release_frame(capture_sink, &frame);
        } else if (ret != ESP_CAPTURE_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Failed to acquire frame: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    ESP_LOGI(TAG, "Capture task stopped");
    vTaskDelete(NULL);
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
    ESP_ERROR_CHECK(network_init());

    // Initialize Board (Codec, Camera Power, etc.)
    ESP_LOGI(TAG, "Initializing Board...");
    set_codec_board_type(TEST_BOARD_NAME);
    codec_init_cfg_t cfg = {.reuse_dev = false};
    init_codec(&cfg);

    // Create video source for camera
    ESP_LOGI(TAG, "Creating video source...");
    video_src = create_video_source();
    if (video_src == NULL) {
        ESP_LOGE(TAG, "Failed to create video source");
        return;
    }

    // Create capture system (video only, no audio for RTSP streaming)
    ESP_LOGI(TAG, "Setting up capture system...");
    esp_capture_cfg_t capture_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = NULL,  // No audio for now
        .video_src = video_src,
    };
    
    esp_capture_err_t cap_ret = esp_capture_open(&capture_cfg, &capture_handle);
    if (cap_ret != ESP_CAPTURE_ERR_OK || capture_handle == NULL) {
        ESP_LOGE(TAG, "Failed to open capture: %d", cap_ret);
        return;
    }

    // Setup capture sink for H.264 output
    esp_capture_sink_cfg_t sink_cfg = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_H264,
            .width = VIDEO_WIDTH,
            .height = VIDEO_HEIGHT,
            .fps = VIDEO_FPS,
        },
    };
    
    cap_ret = esp_capture_sink_setup(capture_handle, 0, &sink_cfg, &capture_sink);
    if (cap_ret != ESP_CAPTURE_ERR_OK || capture_sink == NULL) {
        ESP_LOGE(TAG, "Failed to setup capture sink: %d", cap_ret);
        return;
    }

    // Start RTSP Server
    ESP_LOGI(TAG, "Starting RTSP Server...");
    rtsp_server_config_t rtsp_cfg = RTSP_SERVER_CONFIG_DEFAULT();
    rtsp_cfg.width = VIDEO_WIDTH;
    rtsp_cfg.height = VIDEO_HEIGHT;
    rtsp_cfg.fps = VIDEO_FPS;
    
    ret = rtsp_server_start(&rtsp_cfg, &rtsp_server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RTSP server");
        return;
    }

    // Enable sink and start capture
    esp_capture_sink_enable(capture_sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
    cap_ret = esp_capture_start(capture_handle);
    if (cap_ret != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "Failed to start capture: %d", cap_ret);
        return;
    }

    // Start capture task
    capture_running = true;
    xTaskCreatePinnedToCore(capture_task, "capture_task", 8192, NULL, 5, &capture_task_handle, 1);

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "RTSP Camera Ready!");
    ESP_LOGI(TAG, "Video: %dx%d @ %dfps H.264", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    ESP_LOGI(TAG, "Stream URL: rtsp://<IP>:554/stream");
    ESP_LOGI(TAG, "=================================");

    // Main monitoring loop
    while (1) {
        int clients = rtsp_server_get_client_count(rtsp_server);
        if (clients > 0) {
            ESP_LOGI(TAG, "Streaming to %d client(s)", clients);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
