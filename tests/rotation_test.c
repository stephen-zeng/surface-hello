// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/rotation.h"

int main(void) {
    const uint8_t source[2][3] = {{1, 2, 3}, {4, 5, 6}};
    const uint8_t expected[6] = {3, 6, 2, 5, 1, 4};
    uint8_t output[6] = {0};

    for (size_t y = 0; y < 2; ++y)
        rotate_ccw_line(source[y], output, 3, 2, y);
    if (memcmp(output, expected, sizeof(expected)) != 0) {
        fputs("90-degree CCW frame rotation failed\n", stderr);
        return 1;
    }
    return 0;
}
