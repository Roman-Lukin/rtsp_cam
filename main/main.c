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
#include "esp_capture_advance.h"
#include "esp_gmf_element.h"

#include "esp_video_enc_default.h"
#include "esp_gmf_video_enc.h"

#include "driver/i2c_master.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "rtsp_server.h"

static const char *TAG = "RTSP_CAM";

// I2C bus handle for camera SCCB
static i2c_master_bus_handle_t i2c_bus_handle = NULL;

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

    csi_config.sccb_config.i2c_handle = i2c_bus_handle;
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
    uint32_t frame_count = 0;
    uint32_t last_log_time = 0;
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    while (capture_running) {
        // Acquire frame (blocking with timeout)
        esp_capture_err_t ret = esp_capture_sink_acquire_frame(capture_sink, &frame, false);
        
        if (ret == ESP_CAPTURE_ERR_OK) {
            if (frame.data && frame.size > 0) {
                frame_count++;
                
                // Calculate timestamp in ms from start
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                uint32_t timestamp_ms = now - start_time;
                
                // Log frame info every 5 seconds or first 10 frames
                // if (now - last_log_time > 5000 || frame_count <= 10) {
                //     // Dump first 32 bytes to see format
                //     ESP_LOGI(TAG, "Frame #%lu: size=%d, ts=%lu ms", 
                //              frame_count, frame.size, timestamp_ms);
                //     ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame.data, 
                //              frame.size > 32 ? 32 : frame.size, ESP_LOG_INFO);
                //     last_log_time = now;
                // }
                
                // Feed H.264 frame to RTSP server
                // Check if frame starts with NAL unit (keyframe detection)
                bool is_keyframe = false;
                if (frame.size >= 5) {
                    // Check for IDR NAL type (0x65) or SPS (0x67)
                    uint8_t nal_type = frame.data[4] & 0x1F;
                    is_keyframe = (nal_type == 5 || nal_type == 7);
                }
                
                // Use our calculated timestamp instead of frame.pts (which seems corrupted)
                rtsp_server_feed_frame(rtsp_server, frame.data, frame.size, 
                                       timestamp_ms, is_keyframe);
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

/**
 * @brief Configure camera settings (White Balance, Exposure, etc.)
 */
static void setup_camera_controls(void)
{
    ESP_LOGI(TAG, "Configuring camera settings...");
    int fd = open("/dev/video0", O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open video device for controls");
        return;
    }

    struct v4l2_control ctrl;

    // 1. Auto White Balance
    // 1 = Auto, 0 = Manual
    ctrl.id = V4L2_CID_AUTO_WHITE_BALANCE;
    ctrl.value = 1; 
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        ESP_LOGW(TAG, "Failed to set Auto White Balance");
    } else {
        ESP_LOGI(TAG, "Auto White Balance: %s", ctrl.value ? "Enabled" : "Disabled");
    }

    // 2. Auto Exposure
    // V4L2_EXPOSURE_AUTO = 0, V4L2_EXPOSURE_MANUAL = 1
    ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    ctrl.value = V4L2_EXPOSURE_AUTO;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        ESP_LOGW(TAG, "Failed to set Auto Exposure");
    } else {
        ESP_LOGI(TAG, "Auto Exposure: %s", ctrl.value == V4L2_EXPOSURE_AUTO ? "Auto" : "Manual");
    }

    // 3. Anti-flicker (Power Line Frequency)
    // 1 = 50Hz (Europe), 2 = 60Hz (USA)
    ctrl.id = V4L2_CID_POWER_LINE_FREQUENCY;
    ctrl.value = 1;  // 50Hz dla Polski/Europy
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        ESP_LOGW(TAG, "Failed to set Power Line Frequency (anti-flicker)");
    } else {
        ESP_LOGI(TAG, "Anti-flicker: 50Hz");
    }

    // 4. Manual Exposure (eliminates AE hunting)
    // Wyłączamy auto i ustawiamy stałą wartość
    ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    ctrl.value = V4L2_EXPOSURE_MANUAL;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0) {
        ESP_LOGI(TAG, "Switched to Manual Exposure");
        
        // Ustaw czas ekspozycji (w jednostkach 100µs)
        // 200 = 20ms (dla 50Hz: pełny cykl sieci)
        // Możesz dostosować: mniejsza wartość = ciemniej ale ostrzej
        ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
        ctrl.value = 200;  // 20ms
        if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0) {
            ESP_LOGI(TAG, "Exposure set to %d (20ms)", ctrl.value);
        }
        
        // Ustaw gain (wzmocnienie) dla kompensacji jasności
        // Wyższa wartość = jaśniej ale więcej szumu
        ctrl.id = V4L2_CID_GAIN;
        ctrl.value = 16;  // Umiarkowane wzmocnienie
        if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0) {
            ESP_LOGI(TAG, "Gain set to %d", ctrl.value);
        }
    } else {
        ESP_LOGW(TAG, "Failed to switch to Manual Exposure - AE hunting may occur");
    }

    // Stare przykłady (zakomentowane)
    // ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
    // ctrl.value = 100;
    // ioctl(fd, VIDIOC_S_CTRL, &ctrl);

    // Example: Gain
    // ctrl.id = V4L2_CID_GAIN;
    // ctrl.value = 0;
    // ioctl(fd, VIDIOC_S_CTRL, &ctrl);

    close(fd);
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

    // Initialize Board (get pin config)
    ESP_LOGI(TAG, "Initializing Board...");
    set_codec_board_type(TEST_BOARD_NAME);
    
    // Initialize I2C for camera SCCB (codec_init won't do it with DUMMY codec)
    codec_i2c_pin_t i2c_pin;
    if (get_i2c_pin(0, &i2c_pin) == 0 && i2c_pin.sda >= 0 && i2c_pin.scl >= 0) {
        i2c_master_bus_config_t i2c_bus_config = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = 0,
            .scl_io_num = i2c_pin.scl,
            .sda_io_num = i2c_pin.sda,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ret = i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2C bus: %d", ret);
            return;
        }
        ESP_LOGI(TAG, "I2C initialized (SDA:%d, SCL:%d)", i2c_pin.sda, i2c_pin.scl);
    } else {
        ESP_LOGE(TAG, "Invalid I2C pin configuration");
        return;
    }

    // Register video encoders (H.264, etc.)
    ESP_LOGI(TAG, "Registering video encoders...");
    esp_video_enc_register_default();

    // Create video source for camera
    ESP_LOGI(TAG, "Creating video source...");
    video_src = create_video_source();
    if (video_src == NULL) {
        ESP_LOGE(TAG, "Failed to create video source");
        return;
    }

    // Configure camera settings (AWB, AE, etc.)
    setup_camera_controls();

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

    // Set video bitrate to limit H.264 encoder output size
    // For 1280x960@25fps, use 6Mbps for good quality during motion
    esp_capture_sink_set_bitrate(capture_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, 6000000);
    ESP_LOGI(TAG, "Video bitrate set to 6 Mbps");

    // Set GOP to 25 (1 second) to reduce artifact persistence
    esp_gmf_element_handle_t venc_hd = NULL;
    esp_capture_sink_get_element_by_tag(capture_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_enc", &venc_hd);
    if (venc_hd) {
        esp_gmf_video_enc_set_gop(venc_hd, VIDEO_FPS);
        ESP_LOGI(TAG, "GOP set to %d frames (1 second)", VIDEO_FPS);
        
        // Set QP range for better quality (lower = better quality, higher file size)
        // min_qp=20, max_qp=35 gives good balance for motion
        esp_gmf_video_enc_set_qp(venc_hd, 20, 35);
        ESP_LOGI(TAG, "QP range set to 20-35");
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
    // Increase priority to 10 to ensure low latency video processing and sending
    xTaskCreatePinnedToCore(capture_task, "capture_task", 8192, NULL, 10, &capture_task_handle, 1);

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "RTSP Camera Ready!");
    ESP_LOGI(TAG, "Video: %dx%d @ %dfps H.264", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    ESP_LOGI(TAG, "Stream URL: rtsp://<IP>:554/stream");
    ESP_LOGI(TAG, "=================================");

    // Main monitoring loop
    int last_clients = 0;
    while (1) {
        int clients = rtsp_server_get_client_count(rtsp_server);
        if (clients != last_clients) {
            if (clients > 0) {
                ESP_LOGI(TAG, "Streaming to %d client(s)", clients);
            } else {
                ESP_LOGI(TAG, "No clients connected");
            }
            last_clients = clients;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
