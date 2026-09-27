// SPDX-License-Identifier: MIT
#ifndef SURFACE_IR_ROTATION_H
#define SURFACE_IR_ROTATION_H

#include <stddef.h>
#include <stdint.h>

/* Source (x, y) becomes output (y, width - 1 - x). The output row
 * length is the source height. */
static inline void rotate_ccw_line(const uint8_t *line, uint8_t *frame,
                                   size_t width, size_t height, size_t y) {
    for (size_t x = 0; x < width; ++x)
        frame[(width - 1 - x) * height + y] = line[x];
}

#endif
