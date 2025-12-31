#include "lpr.h"
#include "esp_log.h"

static const char *TAG = "LPR";

esp_err_t lpr_init(const lpr_config_t *config) {
    ESP_LOGI(TAG, "Initializing LPR module...");
    // TODO: Load models
    return ESP_OK;
}

esp_err_t lpr_set_callback(lpr_callback_t cb, void *user_data) {
    // TODO: Save callback
    return ESP_OK;
}

esp_err_t lpr_process_frame(const uint8_t *frame_data, int width, int height) {
    // TODO: Implement processing pipeline
    // 1. Motion detection check (optional here or outside)
    // 2. Plate detection
    // 3. OCR
    return ESP_OK;
}

void lpr_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing LPR module...");
    // TODO: Free resources
}
