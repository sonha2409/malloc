#include "malloc_types.h"
#include <math.h>

/*
 * Size class design:
 *   Bins 0–15:  8-byte spacing, sizes 8, 16, 24, ..., 128  (16 bins)
 *   Bins 16–74: ~12.5% geometric spacing, sizes 144 ... 131072 (128KB)
 *
 * We precompute these tables at init time.
 */

void size_class_init(void) {
    /* Small bins: 8-byte spacing */
    for (int i = 0; i < SMALL_BIN_COUNT; i++) {
        g_state.bin_size[i] = (uint32_t)((i + 1) * 8);
    }

    /* Quick lookup table for small sizes */
    for (int s = 0; s <= SMALL_MAX / 8; s++) {
        int bin = s > 0 ? s - 1 : 0;
        g_state.size_to_bin_small[s] = (uint8_t)bin;
    }

    /* Medium bins: ~12.5% geometric spacing from 128 up to 128KB */
    int bin = SMALL_BIN_COUNT;
    double size = (double)SMALL_MAX;
    while (bin < BIN_COUNT && size < (double)MEDIUM_MAX) {
        size *= 1.125;
        /* Round up to multiple of 16 for alignment */
        size_t rounded = ((size_t)size + 15) & ~(size_t)15;
        if (rounded <= SMALL_MAX) rounded = SMALL_MAX + 16;
        /* Ensure strictly increasing */
        if (bin > SMALL_BIN_COUNT && rounded <= g_state.bin_size[bin - 1]) {
            rounded = g_state.bin_size[bin - 1] + 16;
        }
        if (rounded > MEDIUM_MAX) rounded = MEDIUM_MAX;
        g_state.bin_size[bin] = (uint32_t)rounded;
        bin++;
        if (rounded >= MEDIUM_MAX) break;
    }

    /* Fill remaining bins with max medium size */
    for (int i = bin; i < BIN_COUNT; i++) {
        g_state.bin_size[i] = g_state.bin_size[bin - 1];
    }
}

uint8_t size_to_bin(size_t size) {
    if (size == 0) size = 1;

    /* Align up to minimum 8 bytes */
    size = (size + 7) & ~(size_t)7;

    if (size <= SMALL_MAX) {
        return g_state.size_to_bin_small[size / 8];
    }

    /* Binary search in medium bins */
    int lo = SMALL_BIN_COUNT, hi = BIN_COUNT - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_state.bin_size[mid] < size) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (uint8_t)lo;
}

size_t bin_to_size(uint8_t bin) {
    if (bin >= BIN_COUNT) return MEDIUM_MAX;
    return g_state.bin_size[bin];
}
