#include "neopixels.h"

bool mcugdx_neopixels_init(mcugdx_neopixels_config_t *config) {
    (void)config;
    return true;
}

void mcugdx_neopixels_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
    (void)index;
    (void)r;
    (void)g;
    (void)b;
}

uint32_t mcugdx_neopixels_power_usage_milli_ampere(void) {
    return 0;
}

void mcugdx_neopixels_show(void) {

}

void mcugdx_neopixels_show_max_milli_ampere(uint32_t max_milli_ampere) {
    (void)max_milli_ampere;
}