#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char plate_text[16];      // rozpoznany tekst "WA 12345"
    float confidence;         // pewność 0-1.0
    int bbox[4];              // x, y, w, h
    int64_t timestamp;        // czas detekcji
} lpr_result_t;

typedef void (*lpr_callback_t)(const lpr_result_t *result, void *user_data);

typedef struct {
    // Konfiguracja modelu, ścieżki, progi itp.
    float detection_threshold;
    float ocr_threshold;
} lpr_config_t;

esp_err_t lpr_init(const lpr_config_t *config);
esp_err_t lpr_set_callback(lpr_callback_t cb, void *user_data);
esp_err_t lpr_process_frame(const uint8_t *frame_data, int width, int height);
void lpr_deinit(void);

#ifdef __cplusplus
}
#endif
