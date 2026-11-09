#include QMK_KEYBOARD_H

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    /*  7 8 9
     *  4 5 6
     *  1 2 3
     */
    [0] = LAYOUT(
        KC_7,    KC_8,    KC_9,
        KC_4,    KC_5,    KC_6,
        KC_1,    KC_2,    KC_3
    )
};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [0] = { ENCODER_CCW_CW(KC_VOLD, KC_VOLU) },
};
#endif

void keyboard_post_init_user(void) {
    // The ws2812 driver leaves the data pin at the 2mA default; drive it at 12mA
    // so the 3.3V signal clears the LEDs' input threshold with more margin.
    palSetLineMode(GP26, PAL_RP_PAD_SLEWFAST | PAL_RP_PAD_DRIVE12 | PAL_RP_GPIO_OE | PAL_MODE_ALTERNATE_PIO1);

    rgb_matrix_enable_noeeprom();
    rgb_matrix_mode_noeeprom(RGB_MATRIX_SOLID_COLOR);
    rgb_matrix_sethsv_noeeprom(128, 255, 255); 
}


// Each Scroll Lock toggle from the host steps LED brightness down: 200 -> 150 -> 100 -> 50 -> 200.
bool led_update_user(led_t led_state) {
    static bool seen = false;
    static bool last_scroll;
    if (!seen) {
        seen        = true;
        last_scroll = led_state.scroll_lock;
    } else if (led_state.scroll_lock != last_scroll) {
        last_scroll = led_state.scroll_lock;
        uint8_t val = rgb_matrix_get_val();
        val         = val > 75 ? val - 50 : RGB_MATRIX_MAXIMUM_BRIGHTNESS;
        rgb_matrix_sethsv_noeeprom(rgb_matrix_get_hue(), rgb_matrix_get_sat(), val);
    }
    return true;
}

#ifdef OLED_ENABLE
bool oled_task_user(void) {
    oled_write_P(PSTR("Hackpad\n\n"), false);

    led_t led_state = host_keyboard_led_state();
    oled_write_P(led_state.num_lock ? PSTR("NUM: on\n") : PSTR("NUM: off\n"), false);

    oled_write_P(rgb_matrix_is_enabled() ? PSTR("RGB: on\n") : PSTR("RGB: off\n"), false);

    return false;
}
#endif