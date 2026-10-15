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
    rgb_matrix_mode_noeeprom(RGB_MATRIX_SOLID_COLOR);
    rgb_matrix_sethsv_noeeprom(128, 255, 255); 
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