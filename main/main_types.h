#pragma once

#include "esp_capture.h"
#include "esp_gmf_video_element.h"
#include "esp_video_enc.h"
#include "esp_h264_enc_param_hw.h"

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

// video_enc_t from esp_video_enc.c - wrapper around codec-specific impl
typedef struct {
    const void *ops;                        // esp_video_enc_ops_t*
    esp_video_enc_handle_t enc_handle;      // Points to hw_h264_t
} video_enc_t;

// hw_h264_t from esp_video_enc_h264.c - actual H.264 encoder context
typedef struct {
    esp_h264_enc_handle_t enc_handle;       // H.264 encoder handle
    esp_h264_enc_param_handle_t param_handle;
    // rest of fields not needed
} hw_h264_t;

// --- HACK: Private struct from capture_video_v4l2_src.c ---
// Needed to access FD for V4L2 controls
constexpr int MAX_SUPPORT_FORMATS_NUM = 4;

typedef struct {
    esp_capture_video_src_if_t  base;
    char                        dev_name[16];
    uint8_t                     buf_count;
    esp_capture_format_id_t     support_formats[MAX_SUPPORT_FORMATS_NUM];
    uint8_t                     format_count;
    int                         fd;
} v4l2_src_hack_t;
