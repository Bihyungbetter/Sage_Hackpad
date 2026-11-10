#include QMK_KEYBOARD_H
#include <string.h>
#include "raw_hid.h"

enum layers { _BASE, _FN };

// Host-side actions bound to hotkeys:
#define CLAUDE   C(A(S(KC_C)))     // Start Menu shortcut: VS Code -> vscode://anthropic.claude-code/open
#define WISPR    C(KC_LGUI)        // Wispr Flow push-to-talk (hold Ctrl+Win)
#define WISPR_HF C(G(KC_SPC))      // Wispr Flow hands-free toggle
#define WISPR_PL S(A(KC_Z))        // Wispr Flow paste last dictation
#define SPOTIFY  C(A(S(KC_S)))     // Start Menu shortcut: open Spotify
#define DESKTOP  G(KC_D)           // Show desktop
#define MIC_MUTE G(A(KC_K))        // Windows 11 microphone mute
#define TASKMGR  C(S(KC_ESC))      // Task Manager

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    /* Knob and OLED at the top.
     *  Prev    Play/Pause  Next
     *  Claude  Wispr       Mute
     *  Fn      Spotify     Desktop
     */
    [_BASE] = LAYOUT(
        KC_MPRV, KC_MPLY,  KC_MNXT,
        CLAUDE,  WISPR,    KC_MUTE,
        MO(_FN), SPOTIFY,  DESKTOP
    ),
    /*  Bootloader  LEDs on/off   LED effect
     *  Paste last  Hands-free    Mic mute
     *  (held)      Print Screen  Task Manager
     */
    [_FN] = LAYOUT(
        QK_BOOT,  RM_TOGG,  RM_NEXT,
        WISPR_PL, WISPR_HF, MIC_MUTE,
        _______,  KC_PSCR,  TASKMGR
    )
};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [_BASE] = { ENCODER_CCW_CW(KC_VOLD, KC_VOLU) },
    [_FN]   = { ENCODER_CCW_CW(RM_VALD, RM_VALU) },
};
#endif

#define DEFAULT_LED_VAL 80
#define DEFAULT_LED_SPD 64

void keyboard_post_init_user(void) {
    // The ws2812 driver leaves the data pin at the 2mA default; drive it at 12mA
    // so the 3.3V signal clears the LEDs' input threshold with more margin.
    palSetLineMode(GP26, PAL_RP_PAD_SLEWFAST | PAL_RP_PAD_DRIVE12 | PAL_RP_GPIO_OE | PAL_MODE_ALTERNATE_PIO1);

    // Slow rainbow fade across all keys.
    rgb_matrix_enable_noeeprom();
    rgb_matrix_mode_noeeprom(RGB_MATRIX_CYCLE_ALL);
    rgb_matrix_sethsv_noeeprom(0, 255, DEFAULT_LED_VAL);
    rgb_matrix_set_speed_noeeprom(DEFAULT_LED_SPD);
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
 * 0xB0 0x5A 0xA5               reboot into the UF2 bootloader (for flashing from the host)
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
        case 0xB0:
            if (data[1] == 0x5A && data[2] == 0xA5) reset_keyboard(); // jump to the UF2 bootloader
            break;
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

// Quarter-wave sine table, 64 steps per turn, amplitude 127.
static const int8_t sine_q[17] = {0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127};

static int8_t isin(uint8_t a) {
    a &= 63;
    if (a < 16) return sine_q[a];
    if (a < 32) return sine_q[32 - a];
    if (a < 48) return -sine_q[a - 32];
    return -sine_q[64 - a];
}
#define icos(a) isin((a) + 16)

static void fill_block(int16_t x, int16_t y, uint8_t size) {
    for (uint8_t dy = 0; dy < size; dy++)
        for (uint8_t dx = 0; dx < size; dx++) oled_write_pixel(x + dx, y + dy, true);
}

// Idle screen: Sage's healing orb pulsing with orbiting particles, beside a flowing wave.
static void draw_idle_animation(uint16_t t) {
    const int16_t cx = 18, cy = 16;

    int8_t r = 5 + isin(t * 2) * 2 / 127;
    for (int8_t dy = -r; dy <= r; dy++)
        for (int8_t dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) oled_write_pixel(cx + dx, cy + dy, true);

    for (uint8_t i = 0; i < 16; i += 2) {
        uint8_t a = i * 4 + t;
        oled_write_pixel(cx + icos(a) * 10 / 127, cy + isin(a) * 10 / 127, true);
    }

    for (uint8_t k = 0; k < 3; k++) {
        uint8_t a = k * 21 - t * 2;
        fill_block(cx + icos(a) * 15 / 127 - 1, cy + isin(a) * 13 / 127 - 1, 2);
    }

    for (uint8_t x = 42; x < 128; x++) {
        int16_t y = 25 + (isin(x * 2 + t * 3) * 4 + isin(x * 5 - t * 2) * 2) / 127;
        oled_write_pixel(x, y, true);
        oled_write_pixel(x, y + 1, true);
    }

    oled_set_cursor(7, 0);
    oled_write_P(PSTR("Sage Hackpad"), false);
    oled_set_cursor(7, 1);
    oled_write_P(host_seen ? PSTR("Spotify idle") : PSTR("no PC link"), false);
}

bool oled_task_user(void) {
    static uint32_t frame_timer;
    static uint16_t frame;
    if (timer_elapsed32(frame_timer) < 50) return false;
    frame_timer = timer_read32();
    frame++;
    uint16_t tick = frame / 5; // marquee step every 250ms

    bool active = host_seen && timer_elapsed32(host_stamp) < 5000 && play_status != 0;

    oled_clear();

    // While Fn is held, show what each key does on the Fn layer.
    if (layer_state_is(_FN)) {
        oled_write_P(PSTR("Fn  knob=brightness\n"), true);
        oled_write_P(PSTR("Boot   LEDs   Effect\n"), false);
        oled_write_P(PSTR("Paste  Hands  MicOff\n"), false);
        oled_write_P(PSTR("       PrtSc  TaskMgr"), false);
        return false;
    }

    if (!active) {
        // Show the key legend for the first 5 seconds after plugging in, then animate.
        if (timer_read32() < 5000) {
            oled_write_P(PSTR("Sage Hackpad  keys:\n"), true);
            oled_write_P(PSTR("Prev   Play   Next\n"), false);
            oled_write_P(PSTR("Claude Wispr  Mute\n"), false);
            oled_write_P(PSTR("Fn     Spot   Desk"), false);
        } else {
            draw_idle_animation(frame);
        }
        return false;
    }

    draw_icon(0, 0, spotify_icon);
    oled_set_cursor(2, 0);
    oled_write_P(PSTR("Spotify"), false);

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
