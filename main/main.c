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

#include "esp_h264_enc_param_hw.h"
#include "esp_gmf_video_element.h"
#include "esp_video_enc.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "esp_video_isp_ioctl.h"

#include "rtsp_server.h"
#include "http_server.h"
#include "ov5647_helper.h"

// --- HACK: Private structs from esp_gmf_video_enc.c and esp_video_enc.c ---
// Needed to access H.264 HW handle for Motion Vectors

typedef enum {
    VENC_EXTRA_SET_MASK_NONE    = 0,
    VENC_EXTRA_SET_MASK_BITRATE = (1 << 0),
    VENC_EXTRA_SET_MASK_QP      = (1 << 1),
    VENC_EXTRA_SET_MASK_GOP     = (1 << 2),
    VENC_EXTRA_SET_MASK_ALL     = (0xFF),
} venc_extra_set_mask_t;

typedef struct {
    uint32_t               bitrate;
    uint32_t               min_qp;
    uint32_t               max_qp;
    uint32_t               gop;
    venc_extra_set_mask_t  mask;
} venc_extra_set_t;

typedef struct {
    esp_gmf_video_element_t  parent;       /*!< Video element parent */
    esp_video_codec_type_t   dst_codec;    /*!< Video encoder destination codec */
    bool                     venc_bypass;  /*!< Whether video encoder is bypassed or not */
    uint32_t                 codec_cc;     /*!< FourCC used to find encoder if set */
    esp_video_enc_handle_t   enc_handle;   /*!< Video encoder handle */
    venc_extra_set_t         extra_set;    /*!< Video encoder extra setting */
} venc_t;

typedef struct {
    const void *ops;
    void *enc_handle;
} video_enc_impl_t;
// --- HACK: Private struct from capture_video_v4l2_src.c ---
// Needed to access FD for V4L2 controls
#define MAX_SUPPORT_FORMATS_NUM (4)
typedef struct {
    esp_capture_video_src_if_t  base;
    char                        dev_name[16];
    uint8_t                     buf_count;
    esp_capture_format_id_t     support_formats[MAX_SUPPORT_FORMATS_NUM];
    uint8_t                     format_count;
    int                         fd;
} v4l2_src_hack_t;// --------------------------------------------------------------------------

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

// Forward declaration for encoder settings callback
static void on_settings_change(const camera_settings_t *settings);
int get_camera_fd(void);

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

    // --- Motion Vector Setup ---
    esp_h264_enc_param_hw_handle_t h264_hw_handle = NULL;
    esp_gmf_element_handle_t venc_hd = NULL;
    esp_capture_sink_get_element_by_tag(capture_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_enc", &venc_hd);
    
    if (venc_hd) {
        venc_t *venc = (venc_t *)venc_hd;
        if (venc->enc_handle) {
            video_enc_impl_t *venc_impl = (video_enc_impl_t *)venc->enc_handle;
            h264_hw_handle = (esp_h264_enc_param_hw_handle_t)venc_impl->enc_handle;
            
            if (h264_hw_handle) {
                ESP_LOGI(TAG, "Found H.264 HW handle: %p", h264_hw_handle);
                
                esp_h264_enc_mv_cfg_t mv_cfg = {
                    .mv_mode = ESP_H264_MVM_MODE_P16X16,
                    .mv_fmt  = ESP_H264_MVM_FMT_ALL,
                };
                esp_h264_err_t ret = esp_h264_enc_hw_cfg_mv(h264_hw_handle, mv_cfg);
                if (ret == ESP_H264_ERR_OK) {
                    ESP_LOGI(TAG, "Motion Vectors Enabled!");
                } else {
                    ESP_LOGE(TAG, "Failed to enable Motion Vectors: %d", ret);
                    h264_hw_handle = NULL;
                }
            }
        }
    }
    
    size_t mv_buf_size = (VIDEO_WIDTH / 16) * (VIDEO_HEIGHT / 16) * sizeof(esp_h264_enc_mv_data_t);
    uint8_t *mv_buf = heap_caps_malloc(mv_buf_size, MALLOC_CAP_SPIRAM);
    if (!mv_buf) {
        ESP_LOGE(TAG, "Failed to allocate MV buffer");
    }
    // ---------------------------

    uint32_t frame_count = 0;
    uint32_t last_log_time = 0;
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool last_motion_state = false;
    
    while (capture_running) {
        // --- Set MV Packet for NEXT frame ---
        if (h264_hw_handle && mv_buf) {
            esp_h264_enc_mvm_pkt_t mv_pkt = {
                .data = (esp_h264_enc_mv_data_t *)mv_buf,
                .len = mv_buf_size,
            };
            esp_h264_enc_hw_set_mv_pkt(h264_hw_handle, mv_pkt);
        }
        // ------------------------------------

        // Acquire frame (blocking with timeout)
        esp_capture_err_t ret = esp_capture_sink_acquire_frame(capture_sink, &frame, false);
        
        if (ret == ESP_CAPTURE_ERR_OK) {
            if (frame.data && frame.size > 0) {
                frame_count++;
                
                // Calculate timestamp in ms from start
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                uint32_t timestamp_ms = now - start_time;

                // --- Process MV Data ---
                if (h264_hw_handle && mv_buf) {
                    uint32_t mv_len = 0;
                    esp_h264_enc_hw_get_mv_data_len(h264_hw_handle, &mv_len);
                    if (mv_len > 0) {
                        // Check for motion
                        int motion_count = 0;
                        esp_h264_enc_mv_data_t *mvs = (esp_h264_enc_mv_data_t *)mv_buf;
                        int mb_count = mv_len / sizeof(esp_h264_enc_mv_data_t);
                        
                        for (int i = 0; i < mb_count; i++) {
                            // Simple threshold: if MV is large enough
                            if (abs(mvs[i].mv_x) > 4 || abs(mvs[i].mv_y) > 4) {
                                motion_count++;
                            }
                        }
                        
                        bool current_motion_state = (motion_count > 50);
                        if (current_motion_state != last_motion_state) {
                            if (current_motion_state) {
                                ESP_LOGI(TAG, "Motion Detected! MBs: %d", motion_count);
                                // TODO: Trigger LPR
                            } else {
                                ESP_LOGI(TAG, "Motion Stopped");
                            }
                            last_motion_state = current_motion_state;
                        }
                    }
                }
                // -----------------------
                
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



int get_camera_fd(void)
{
    if (video_src) {
        v4l2_src_hack_t *src = (v4l2_src_hack_t *)video_src;
        return src->fd;
    }
    return -1;
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
        
        // Initialize OV5647 helper
        ov5647_helper_init(i2c_bus_handle);
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

    // Wstępne ustawienia kamery przed uruchomieniem pipeline
    vTaskDelay(pdMS_TO_TICKS(50));
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

    // Start HTTP Server for settings page
    ESP_LOGI(TAG, "Starting HTTP Server for settings...");
    http_server_set_settings_callback(on_settings_change);
    ret = http_server_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start HTTP server (non-critical)");
    }

    // Enable sink and start capture
    esp_capture_sink_enable(capture_sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
    cap_ret = esp_capture_start(capture_handle);
    if (cap_ret != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "Failed to start capture: %d", cap_ret);
        return;
    }

    // Ponowna próba ustawień po starcie pipeline (niektóre sterowniki akceptują dopiero po STREAMON)
    vTaskDelay(pdMS_TO_TICKS(100));
    setup_camera_controls();

    // Start capture task
    capture_running = true;
    // Increase priority to 10 to ensure low latency video processing and sending
    xTaskCreatePinnedToCore(capture_task, "capture_task", 8192, NULL, 10, &capture_task_handle, 1);

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "RTSP Camera Ready!");
    ESP_LOGI(TAG, "Video: %dx%d @ %dfps H.264", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    ESP_LOGI(TAG, "Stream URL: rtsp://<IP>:554/stream");
    ESP_LOGI(TAG, "Settings:   http://<IP>/");
    ESP_LOGI(TAG, "=================================");

    // Main monitoring loop
    int last_clients = 0;
    int loop_count = 0;
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

        if (loop_count++ % 5 == 0) {
             size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
             size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
             ESP_LOGI(TAG, "Free PSRAM: %d KB, Internal: %d KB", free_psram / 1024, free_internal / 1024);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Callback for HTTP server when encoder settings change
 */
static void on_settings_change(const camera_settings_t *settings)
{
    ESP_LOGI(TAG, "Applying encoder settings: bitrate=%d kbps, GOP=%d, QP=%d-%d",
             settings->bitrate, settings->gop, settings->min_qp, settings->max_qp);
    
    esp_gmf_element_handle_t venc_hd = NULL;
    esp_capture_sink_get_element_by_tag(capture_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_enc", &venc_hd);
    
    if (venc_hd) {
        // Update bitrate (convert kbps to bps)
        esp_capture_sink_set_bitrate(capture_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, settings->bitrate * 1000);
        
        // Update GOP
        esp_gmf_video_enc_set_gop(venc_hd, settings->gop);
        
        // Update QP range
        esp_gmf_video_enc_set_qp(venc_hd, settings->min_qp, settings->max_qp);
        
        ESP_LOGI(TAG, "Encoder settings updated");
    } else {
        ESP_LOGW(TAG, "Video encoder not found");
    }
}
