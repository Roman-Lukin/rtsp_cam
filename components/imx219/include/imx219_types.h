/*
 * SPDX-FileCopyrightText: 2024-2025 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IMX219 camera sensor register type definition.
 */
typedef struct {
    uint16_t reg;
    uint8_t val;
} imx219_reginfo_t;

#ifdef __cplusplus
}
#endif
