#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
// #include "driver/ledc.h"  // Not needed - IMX219 has internal oscillator

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

#include "esp_video_isp_ioctl.h"

#include "network.h"
#include "settings.h"
#include "app_params.h"
#include "rtsp_server.h"
#include "http_server.h"
#include "ov5647_helper.h"
#include "main_types.h"

namespace {
    constexpr const char *TAG = "RTSP_CAM";

    // I2C bus handle for camera SCCB
    i2c_master_bus_handle_t i2c_bus_handle = nullptr;

    // Capture system handles
    esp_capture_handle_t capture_handle = nullptr;
    esp_capture_video_src_if_t *video_src = nullptr;
    esp_capture_sink_handle_t capture_sink = nullptr;
    rtsp_server_handle_t rtsp_server = nullptr;

    // Frame capture task
    TaskHandle_t capture_task_handle = nullptr;
    volatile bool capture_running = false;
    
    // Detected sensor type
    enum class SensorType {
        NONE,
        OV5647,
        IMX219
    };
    SensorType detected_sensor = SensorType::NONE;
}

// Forward declarations
static void on_settings_change(const camera_settings_t *settings);

// Camera GPIO pins for your custom board
#define CAM_PWR_GPIO    GPIO_NUM_48   // CSI_IO0 - Pin 17 on 22-pin FPC = CAM_GPIO (power enable for IMX219)
#define CAM_XCLK_GPIO   GPIO_NUM_47   // CSI_IO1 - Pin 18 on 22-pin FPC = XCLK/MCLK for IMX219 (24MHz)

// LEDC configuration for XCLK generation (not needed - IMX219 has internal oscillator)
// #define XCLK_LEDC_TIMER      LEDC_TIMER_0
// #define XCLK_LEDC_CHANNEL    LEDC_CHANNEL_0
// #define XCLK_FREQUENCY_HZ    24000000   // 24 MHz for IMX219

#if 0  // IMX219 has internal 24.75MHz oscillator, external XCLK not needed
/**
 * @brief Start XCLK signal generation for IMX219
 * IMX219 requires 24MHz external clock on CAM_IO1 pin
 * Uses LEDC peripheral to generate the clock signal
 */
static esp_err_t start_xclk(void)
{
    ESP_LOGI(TAG, "Starting XCLK generation on GPIO%d at %d Hz", CAM_XCLK_GPIO, XCLK_FREQUENCY_HZ);
    
    // Configure LEDC timer
    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_conf.timer_num = XCLK_LEDC_TIMER;
    timer_conf.duty_resolution = LEDC_TIMER_1_BIT;  // 1-bit resolution for 50% duty cycle
    timer_conf.freq_hz = XCLK_FREQUENCY_HZ;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
    
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %d", ret);
        return ret;
    }
    
    // Configure LEDC channel
    ledc_channel_config_t channel_conf = {};
    channel_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_conf.channel = XCLK_LEDC_CHANNEL;
    channel_conf.timer_sel = XCLK_LEDC_TIMER;
    channel_conf.intr_type = LEDC_INTR_DISABLE;
    channel_conf.gpio_num = CAM_XCLK_GPIO;
    channel_conf.duty = 1;  // 50% duty cycle with 1-bit resolution
    channel_conf.hpoint = 0;
    
    ret = ledc_channel_config(&channel_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "XCLK started successfully");
    return ESP_OK;
}

/**
 * @brief Stop XCLK signal generation
 */
static void stop_xclk(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, XCLK_LEDC_CHANNEL, 0);
    ESP_LOGI(TAG, "XCLK stopped");
}
#endif  // XCLK disabled

/**
 * @brief Enable camera power for IMX219
 * IMX219 needs CAM_GPIO HIGH to enable its internal power regulator
 * OV5647 uses this pin as PWDN (power down) - LOW = active
 */
static void enable_camera_power(bool for_imx219)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << CAM_PWR_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    
    if (for_imx219) {
        // IMX219: HIGH = power regulator enabled
        gpio_set_level(CAM_PWR_GPIO, 1);
        ESP_LOGW(TAG, "Camera power GPIO%d set HIGH (IMX219 mode)", CAM_PWR_GPIO);
    } else {
        // OV5647: LOW = not in power-down mode (active)
        gpio_set_level(CAM_PWR_GPIO, 0);
        ESP_LOGW(TAG, "Camera power GPIO%d set LOW (OV5647 mode)", CAM_PWR_GPIO);
    }
    
    // Wait for power to stabilize - IMX219 needs longer time
    vTaskDelay(pdMS_TO_TICKS(200));
}

/**
 * @brief Scan I2C bus for devices and detect sensor type
 * @return SensorType (IMX219, OV5647, or NONE)
 */
static SensorType i2c_scan_and_detect(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  I2C BUS SCAN - Camera Detection");
    ESP_LOGW(TAG, "========================================");
    
    // First enable camera power for IMX219
    enable_camera_power(true);  // true = IMX219 mode (HIGH)
    
    // IMX219 may need more time to fully initialize after power-on
    ESP_LOGW(TAG, "Waiting 500ms for sensor to initialize...");
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Check for known sensors
    SensorType result = SensorType::NONE;
    int found_count = 0;
    
    // Check IMX219 (0x10)
    if (i2c_master_probe(bus_handle, 0x10, 50) == ESP_OK) {
        ESP_LOGW(TAG, "  >> FOUND: 0x10 - IMX219");
        result = SensorType::IMX219;
        found_count++;
    }
    
    // Check IMX219 alt (0x1A)
    if (i2c_master_probe(bus_handle, 0x1A, 50) == ESP_OK) {
        ESP_LOGW(TAG, "  >> FOUND: 0x1A - IMX219 (alt addr)");
        if (result == SensorType::NONE) result = SensorType::IMX219;
        found_count++;
    }
    
    // Check OV5647/OV5640 (0x36)
    if (i2c_master_probe(bus_handle, 0x36, 50) == ESP_OK) {
        ESP_LOGW(TAG, "  >> FOUND: 0x36 - OV5647/OV5640");
        if (result == SensorType::NONE) result = SensorType::OV5647;
        found_count++;
    }
    
    // Full scan for other devices
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (addr == 0x10 || addr == 0x1A || addr == 0x36) continue;
        
        if (i2c_master_probe(bus_handle, addr, 50) == ESP_OK) {
            ESP_LOGW(TAG, "  >> FOUND: 0x%02X - Unknown device", addr);
            found_count++;
        }
    }
    
    if (found_count == 0) {
        ESP_LOGE(TAG, "  NO DEVICES FOUND ON I2C BUS!");
        ESP_LOGE(TAG, "  Check: power, reset pin, cable connection");
    } else {
        ESP_LOGW(TAG, "  Total devices found: %d", found_count);
        const char *sensor_name = "NONE";
        if (result == SensorType::IMX219) sensor_name = "IMX219";
        else if (result == SensorType::OV5647) sensor_name = "OV5647";
        ESP_LOGW(TAG, "  >>> Primary sensor: %s <<<", sensor_name);
    }
    
    ESP_LOGW(TAG, "========================================");
    
    // Pause for 3 seconds to read the results
    ESP_LOGW(TAG, "Pausing 3 seconds before continuing...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    return result;
}

/**
 * @brief Get camera file descriptor for V4L2 controls
 */
int get_camera_fd()
{
    if (video_src) {
        auto *src = reinterpret_cast<v4l2_src_hack_t *>(video_src);
        return src->fd;
    }
    return -1;
}

/**
 * @brief Create video source for MIPI camera on ESP32-P4
 */
static esp_capture_video_src_if_t *create_video_source()
{
    camera_cfg_t cam_pin_cfg = {};
    int ret = get_camera_cfg(&cam_pin_cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to get camera config");
        return nullptr;
    }
    
    ESP_LOGW(TAG, "Camera config: type=%d, pwr=%d, reset=%d, xclk=%d", 
             cam_pin_cfg.type, cam_pin_cfg.pwr, cam_pin_cfg.reset, cam_pin_cfg.xclk);

    if (cam_pin_cfg.type != CAMERA_TYPE_MIPI) {
        ESP_LOGE(TAG, "Only MIPI camera supported on ESP32-P4");
        return nullptr;
    }

    // Initialize V4L2/camera subsystem
    esp_video_init_csi_config_t csi_config = {};
    esp_video_init_config_t cam_config = {};

    csi_config.sccb_config.i2c_handle = i2c_bus_handle;
    csi_config.sccb_config.freq = 100000;
    
    // Set reset pin only if valid (>= 0), otherwise use -1
    csi_config.reset_pin = (cam_pin_cfg.reset >= 0) ? 
                           static_cast<gpio_num_t>(cam_pin_cfg.reset) : GPIO_NUM_NC;
    
    // For IMX219: pwdn_pin should NOT be used by esp_video (we control it ourselves)
    // Pass -1 to prevent esp_video from toggling our power pin
    if (detected_sensor == SensorType::IMX219) {
        csi_config.pwdn_pin = GPIO_NUM_NC;  // Don't let esp_video control power
        ESP_LOGW(TAG, "IMX219 mode: pwdn_pin disabled (we control GPIO48 ourselves)");
    } else {
        csi_config.pwdn_pin = (cam_pin_cfg.pwr >= 0) ? 
                              static_cast<gpio_num_t>(cam_pin_cfg.pwr) : GPIO_NUM_NC;
    }
    
    ESP_LOGW(TAG, "CSI config: reset_pin=%d, pwdn_pin=%d", 
             csi_config.reset_pin, csi_config.pwdn_pin);
    
    cam_config.csi = &csi_config;

    ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", ret);
        return nullptr;
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
    
    ESP_LOGI(TAG, "Capture task started - entering loop");

    uint32_t frame_count = 0;
    uint32_t poll_count = 0;
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t last_log_time = start_time;
    
    ESP_LOGW(TAG, ">>> Polling for frames from capture pipeline...");
    
    while (capture_running) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        poll_count++;
        
        // Log heartbeat every 5 seconds
        if ((now - last_log_time > 5000)) {
            ESP_LOGW(TAG, ">>> Heartbeat: frames=%lu, polls=%lu, elapsed=%lu ms", 
                     frame_count, poll_count, now - start_time);
            last_log_time = now;
        }

        // Use non-blocking acquire
        esp_capture_err_t ret = esp_capture_sink_acquire_frame(capture_sink, &frame, true);
        
        if (ret == ESP_CAPTURE_ERR_OK) {
            // Got frame!
            if (frame_count == 0) {
                ESP_LOGI(TAG, ">>> FIRST FRAME RECEIVED after %lu polls!", poll_count);
            }
            
            if (frame.data && frame.size > 0) {
                frame_count++;
                uint32_t timestamp_ms = now - start_time;
                
                // Check if frame starts with NAL unit (keyframe detection)
                bool is_keyframe = false;
                if (frame.size >= 5) {
                    uint8_t nal_type = frame.data[4] & 0x1F;
                    is_keyframe = (nal_type == 5 || nal_type == 7);
                }
                
                // Log first few frames and then periodically
                if (frame_count <= 10 || frame_count % 30 == 0) {
                    ESP_LOGI(TAG, "Frame #%lu: size=%d, keyframe=%d, ts=%lu ms", 
                             frame_count, frame.size, is_keyframe, timestamp_ms);
                }
                
                rtsp_server_feed_frame(rtsp_server, frame.data, frame.size, 
                                       timestamp_ms, is_keyframe);
            }
            
            esp_capture_sink_release_frame(capture_sink, &frame);
        } else {
            // No frame available - wait ~33ms (30fps rate)
            vTaskDelay(pdMS_TO_TICKS(33));
        }
    }
    
    ESP_LOGI(TAG, "Capture task stopped");
    vTaskDelete(nullptr);
}

/**
 * @brief Main application entry point
 */
extern "C" void app_main()
{
    esp_err_t ret;
    ESP_LOGI(TAG, "Starting RTSP Camera Application");

    // Enable camera power for IMX219 (GPIO48 HIGH)
    // Must be done before any camera initialization
    enable_camera_power(true);  // true = IMX219 mode
    ESP_LOGI(TAG, "Camera power enabled (IMX219 mode)");

    // Create AppParams object (initializes NVS)
    AppParams app_params;

    // Initialize Network
    ESP_ERROR_CHECK(network_init());

    // Initialize Board (get pin config)
    ESP_LOGI(TAG, "Initializing Board...");
    set_codec_board_type(TEST_BOARD_NAME);
    
    // Initialize I2C for camera SCCB (codec_init won't do it with DUMMY codec)
    codec_i2c_pin_t i2c_pin;
    if (get_i2c_pin(0, &i2c_pin) == 0 && i2c_pin.sda >= 0 && i2c_pin.scl >= 0) {
        i2c_master_bus_config_t i2c_bus_config = {};
        i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_config.i2c_port = 0;
        i2c_bus_config.scl_io_num = static_cast<gpio_num_t>(i2c_pin.scl);
        i2c_bus_config.sda_io_num = static_cast<gpio_num_t>(i2c_pin.sda);
        i2c_bus_config.glitch_ignore_cnt = 7;
        i2c_bus_config.flags.enable_internal_pullup = true;
        ret = i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2C bus: %d", ret);
            return;
        }
        ESP_LOGI(TAG, "I2C initialized (SDA:%d, SCL:%d)", i2c_pin.sda, i2c_pin.scl);
        
        // Scan I2C bus and detect sensor type
        detected_sensor = i2c_scan_and_detect(i2c_bus_handle);
        
        // Initialize sensor-specific helper (only for OV5647)
        if (detected_sensor == SensorType::OV5647) {
            ov5647_helper_init(i2c_bus_handle);
            ESP_LOGI(TAG, "OV5647 helper initialized");
        } else if (detected_sensor == SensorType::IMX219) {
            ESP_LOGI(TAG, "IMX219 detected - no helper needed");
        } else {
            ESP_LOGW(TAG, "No known sensor detected!");
        }
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
    if (video_src == nullptr) {
        ESP_LOGE(TAG, "Failed to create video source");
        return;
    }

    // Wstępne ustawienia kamery przed uruchomieniem pipeline
    vTaskDelay(pdMS_TO_TICKS(50));
    if (detected_sensor == SensorType::OV5647) {
        setup_camera_controls();  // Podstawowa inicjalizacja OV5647
        app_params.apply_to_camera();
    }

    // Create capture system (video only, no audio for RTSP streaming)
    ESP_LOGI(TAG, "Setting up capture system...");
    esp_capture_cfg_t capture_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = nullptr,
        .video_src = video_src,
    };
    
    esp_capture_err_t cap_ret = esp_capture_open(&capture_cfg, &capture_handle);
    if (cap_ret != ESP_CAPTURE_ERR_OK || capture_handle == nullptr) {
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
    if (cap_ret != ESP_CAPTURE_ERR_OK || capture_sink == nullptr) {
        ESP_LOGE(TAG, "Failed to setup capture sink: %d", cap_ret);
        return;
    }

    // Set video bitrate to limit H.264 encoder output size
    // For 1280x720@30fps, 3Mbps is good quality
    esp_capture_sink_set_bitrate(capture_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, 3000000);
    ESP_LOGI(TAG, "Video bitrate set to 3 Mbps");

    // Set GOP to 25 (1 second) to reduce artifact persistence
    esp_gmf_element_handle_t venc_hd = nullptr;
    esp_capture_sink_get_element_by_tag(capture_sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_enc", &venc_hd);
    if (venc_hd) {
        esp_gmf_video_enc_set_gop(venc_hd, VIDEO_FPS);
        ESP_LOGI(TAG, "GOP set to %d frames (1 second)", VIDEO_FPS);
        
        // Set QP range for good quality at 720p
        // min_qp=20, max_qp=38 gives good balance
        esp_gmf_video_enc_set_qp(venc_hd, 20, 38);
        ESP_LOGI(TAG, "QP range set to 20-38");
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

    // Enable sink first, then start capture
    // The esp_capture example does it this way
    esp_capture_sink_enable(capture_sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
    
    cap_ret = esp_capture_start(capture_handle);
    if (cap_ret != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "Failed to start capture: %d", cap_ret);
        return;
    }
    ESP_LOGI(TAG, "Capture pipeline started successfully");

    // Wait for pipeline to stabilize and first frames to flow
    ESP_LOGI(TAG, "Waiting 500ms for pipeline to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(500));

    // Ponowna próba ustawień po starcie pipeline (niektóre sterowniki akceptują dopiero po STREAMON)
    if (detected_sensor == SensorType::OV5647) {
        setup_camera_controls();  // Podstawowa inicjalizacja OV5647
        app_params.apply_to_camera();
    }

    // Start capture task
    capture_running = true;
    // Increase priority to 10 to ensure low latency video processing and sending
    xTaskCreatePinnedToCore(capture_task, "capture_task", 8192, nullptr, 10, &capture_task_handle, 1);

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "RTSP Camera Ready!");
    ESP_LOGI(TAG, "Video: %dx%d @ %dfps H.264", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
    ESP_LOGI(TAG, "Stream URL: rtsp://<IP>:554/stream");
    ESP_LOGI(TAG, "Settings:   http://<IP>/");
    ESP_LOGI(TAG, "=================================");

    // Main monitoring loop
    int last_clients = 0;
    int loop_count = 0;
    while (true) {
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
    
    esp_gmf_element_handle_t venc_hd = nullptr;
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
