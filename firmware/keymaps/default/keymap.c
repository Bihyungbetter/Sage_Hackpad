#include QMK_KEYBOARD_H
#include <string.h>
#include "raw_hid.h"

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

#define DEFAULT_LED_VAL 80

void keyboard_post_init_user(void) {
    // The ws2812 driver leaves the data pin at the 2mA default; drive it at 12mA
    // so the 3.3V signal clears the LEDs' input threshold with more margin.
    palSetLineMode(GP26, PAL_RP_PAD_SLEWFAST | PAL_RP_PAD_DRIVE12 | PAL_RP_GPIO_OE | PAL_MODE_ALTERNATE_PIO1);

    rgb_matrix_enable_noeeprom();
    rgb_matrix_mode_noeeprom(RGB_MATRIX_SOLID_COLOR);
    rgb_matrix_sethsv_noeeprom(128, 255, DEFAULT_LED_VAL);
}

// Each Scroll Lock toggle from the host steps LED brightness up: 40 -> 80 -> 120 -> 160 -> 200 -> 40.
bool led_update_user(led_t led_state) {
    static bool seen = false;
    static bool last_scroll;
    if (!seen) {
        seen        = true;
        last_scroll = led_state.scroll_lock;
    } else if (led_state.scroll_lock != last_scroll) {
        last_scroll = led_state.scroll_lock;
        uint8_t val = rgb_matrix_get_val() / 40 * 40 + 40;
        if (val > RGB_MATRIX_MAXIMUM_BRIGHTNESS) val = 40;
        rgb_matrix_sethsv_noeeprom(rgb_matrix_get_hue(), rgb_matrix_get_sat(), val);
    }
    return true;
}

/* ---------- Host link (raw HID) ----------
 * 0x01 offset total chars...  track title chunk
 * 0x02 offset total chars...  artist chunk
 * 0x03 status pos_hi pos_lo dur_hi dur_lo   status: 0 none, 1 paused, 2 playing
 * 0x10 on                      per-LED override on/off
 * 0x11 index b0 b1 b2          override LED wire bytes (sent to the LED in this order)
 * 0x12 b0 b1 b2                override all LEDs' wire bytes
 * 0x13 h s v                   solid color for the normal (non-override) mode
 * 0x20 rotation                OLED rotation 0-3 (0, 90, 180, 270 degrees)
 */
#define TEXT_MAX 64

static char     track_title[TEXT_MAX + 1];
static char     track_artist[TEXT_MAX + 1];
static uint8_t  play_status;
static uint16_t play_pos, play_dur;
static uint32_t pos_stamp, host_stamp;
static bool     host_seen;

static bool    led_override;
static uint8_t led_wire[RGB_MATRIX_LED_COUNT][3];

static void store_chunk(char *dst, const uint8_t *data, uint8_t length) {
    uint8_t offset = data[1], total = data[2];
    if (total > TEXT_MAX) total = TEXT_MAX;
    for (uint8_t i = 3; i < length && offset < total; i++, offset++) dst[offset] = data[i];
    dst[total] = '\0';
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    host_stamp = timer_read32();
    host_seen  = true;
    switch (data[0]) {
        case 0x01: store_chunk(track_title, data, length); break;
        case 0x02: store_chunk(track_artist, data, length); break;
        case 0x03:
            play_status = data[1];
            play_pos    = (data[2] << 8) | data[3];
            play_dur    = (data[4] << 8) | data[5];
            pos_stamp   = timer_read32();
            break;
        case 0x10: led_override = data[1]; break;
        case 0x11:
            if (data[1] < RGB_MATRIX_LED_COUNT) memcpy(led_wire[data[1]], &data[2], 3);
            break;
        case 0x12:
            for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) memcpy(led_wire[i], &data[1], 3);
            break;
        case 0x13: rgb_matrix_sethsv_noeeprom(data[1], data[2], data[3]); break;
#ifdef OLED_ENABLE
        case 0x20: oled_init(data[1] & 3); break;
#endif
    }
}

bool rgb_matrix_indicators_user(void) {
    if (led_override) {
        // The driver puts green, red, blue on the wire, so map wire bytes back to (r, g, b).
        for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) rgb_matrix_set_color(i, led_wire[i][1], led_wire[i][0], led_wire[i][2]);
    }
    return false;
}

#ifdef OLED_ENABLE
/* ---------- Spotify widget (128x32) ---------- */
static const uint8_t spotify_icon[8] = {0x3C, 0x7E, 0x81, 0xFF, 0xC3, 0xFF, 0x66, 0x3C};

static void draw_icon(uint8_t x0, uint8_t y0, const uint8_t *rows) {
    for (uint8_t y = 0; y < 8; y++)
        for (uint8_t x = 0; x < 8; x++) oled_write_pixel(x0 + x, y0 + y, rows[y] & (0x80 >> x));
}

static void draw_play_state(uint8_t x0, uint8_t y0, bool playing) {
    for (uint8_t y = 0; y < 7; y++) {
        for (uint8_t x = 0; x < 7; x++) {
            bool on = playing ? (x <= (y < 4 ? y : 6 - y) * 2) : (x < 2 || (x > 3 && x < 6));
            oled_write_pixel(x0 + x, y0 + y, on);
        }
    }
}

static void write_time(uint16_t secs) {
    char buf[6];
    uint8_t m = secs / 60 > 99 ? 99 : secs / 60, s = secs % 60;
    buf[0] = m >= 10 ? '0' + m / 10 : ' ';
    buf[1] = '0' + m % 10;
    buf[2] = ':';
    buf[3] = '0' + s / 10;
    buf[4] = '0' + s % 10;
    buf[5] = '\0';
    oled_write(buf, false);
}

// Writes `text` on `line`, scrolling it as a marquee when it is wider than the screen.
static void write_marquee(uint8_t line, const char *text, uint16_t tick) {
    const uint8_t width = 21, gap = 4;
    uint8_t len = strlen(text);
    char out[22];
    if (len <= width) {
        memset(out, ' ', width);
        memcpy(out, text, len);
    } else {
        uint16_t cycle = len + gap, start = tick % cycle;
        for (uint8_t i = 0; i < width; i++) {
            uint16_t p = (start + i) % cycle;
            out[i] = p < len ? text[p] : ' ';
        }
    }
    out[width] = '\0';
    oled_set_cursor(0, line);
    oled_write(out, false);
}

bool oled_task_user(void) {
    static uint32_t frame_timer;
    static uint16_t tick;
    if (timer_elapsed32(frame_timer) < 250) return false;
    frame_timer = timer_read32();
    tick++;

    bool active = host_seen && timer_elapsed32(host_stamp) < 5000 && play_status != 0;

    oled_clear();
    draw_icon(0, 0, spotify_icon);
    oled_set_cursor(2, 0);
    oled_write_P(PSTR("Spotify"), false);

    if (!active) {
        oled_set_cursor(0, 1);
        oled_write_P(host_seen ? PSTR("Nothing playing") : PSTR("Waiting for PC..."), false);
        oled_set_cursor(0, 3);
        oled_write_P(PSTR("Sage Hackpad"), false);
        return false;
    }

    bool     playing = play_status == 2;
    uint16_t pos     = play_pos;
    if (playing) pos += timer_elapsed32(pos_stamp) / 1000;
    if (play_dur && pos > play_dur) pos = play_dur;

    draw_play_state(120, 0, playing);
    write_marquee(1, track_title, tick);
    write_marquee(2, track_artist, tick);

    oled_set_cursor(0, 3);
    write_time(pos);
    oled_set_cursor(16, 3);
    write_time(play_dur);

    // Progress bar between the two timestamps.
    const uint8_t x0 = 33, x1 = 93, y0 = 26, y1 = 30;
    uint8_t fill = play_dur ? (uint32_t)(x1 - x0 - 1) * pos / play_dur : 0;
    for (uint8_t x = x0; x <= x1; x++) {
        oled_write_pixel(x, y0, true);
        oled_write_pixel(x, y1, true);
    }
    for (uint8_t y = y0; y <= y1; y++) {
        oled_write_pixel(x0, y, true);
        oled_write_pixel(x1, y, true);
    }
    for (uint8_t x = x0 + 1; x <= x0 + fill; x++)
        for (uint8_t y = y0 + 1; y < y1; y++) oled_write_pixel(x, y, true);

    return false;
}
#endif
