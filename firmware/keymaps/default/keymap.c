#include QMK_KEYBOARD_H
#include <string.h>
#include "raw_hid.h"
#include "pets_data.h"

enum layers { _BASE, _FN };

// Host-side actions bound to hotkeys:
#define CLAUDE   C(A(S(KC_C)))     // Start Menu shortcut: VS Code -> vscode://anthropic.claude-code/open
#define WISPR    C(KC_LGUI)        // Wispr Flow push-to-talk (hold Ctrl+Win)
#define WISPR_HF C(G(KC_SPC))      // Wispr Flow hands-free toggle
#define WISPR_PL S(A(KC_Z))        // Wispr Flow paste last dictation
#define SPOTIFY  C(A(S(KC_S)))     // Start Menu shortcut: open Spotify
#define DESKTOP  G(KC_D)           // Show desktop
#define MIC_MUTE G(A(KC_K))        // Windows 11 microphone mute
enum custom_keycodes { SCREEN_NEXT = SAFE_RANGE }; // cycle the OLED screen

// OLED screens, cycled with the bottom-right key. The choice is saved in EEPROM.
enum screens { SCR_AUTO, SCR_SPOTIFY, SCR_CLOCK, SCR_BONGO, SCR_LUNA, SCR_STATS, SCR_COUNT };
static uint8_t  current_screen;
static uint32_t screen_switch_time;

// Key activity that animates the pets: presses on the hackpad itself, plus press
// counts the host agent reports for every other keyboard (raw HID 0x06).
static uint32_t press_times[8];
static uint8_t  press_idx;
static uint32_t last_local_press;

static void note_press(void) {
    uint32_t now = timer_read32();
    press_times[press_idx++ & 7] = now ? now : 1;
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    /* Knob and OLED at the top.
     *  Prev    Play/Pause  Next
     *  Claude  Wispr       Mute
     *  Fn      Spotify     Next OLED screen
     */
    [_BASE] = LAYOUT(
        KC_MPRV, KC_MPLY,  KC_MNXT,
        CLAUDE,  WISPR,    KC_MUTE,
        MO(_FN), SPOTIFY,  SCREEN_NEXT
    ),
    /*  Bootloader  LEDs on/off   LED effect
     *  Paste last  Hands-free    Mic mute
     *  (held)      Print Screen  Show desktop
     */
    [_FN] = LAYOUT(
        QK_BOOT,  RM_TOGG,  RM_NEXT,
        WISPR_PL, WISPR_HF, MIC_MUTE,
        _______,  KC_PSCR,  DESKTOP
    )
};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [_BASE] = { ENCODER_CCW_CW(KC_VOLU, KC_VOLD) },
    [_FN]   = { ENCODER_CCW_CW(RM_VALU, RM_VALD) },
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

    current_screen = eeconfig_read_user() % SCR_COUNT;
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
 * 0x04 hour min sec dow month day          local time (dow 0 = Sunday)
 * 0x05 cpu% ram%               PC load
 * 0x06 count                   key presses on other keyboards (count only, no key codes)
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

static uint32_t clock_base_sec, clock_stamp;
static bool     clock_valid;
static uint8_t  clock_dow, clock_month, clock_day;

static uint8_t  stat_cpu, stat_ram;
static uint32_t stats_stamp;
static bool     stats_valid;

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
        case 0x04:
            clock_base_sec = data[1] * 3600UL + data[2] * 60UL + data[3];
            clock_dow      = data[4];
            clock_month    = data[5];
            clock_day      = data[6];
            clock_stamp    = timer_read32();
            clock_valid    = true;
            break;
        case 0x05:
            stat_cpu    = data[1];
            stat_ram    = data[2];
            stats_stamp = timer_read32();
            stats_valid = true;
            break;
        case 0x06:
            // The host also sees the hackpad's own presses; skip those echoes.
            if (!last_local_press || timer_elapsed32(last_local_press) > 150)
                for (uint8_t i = 0; i < data[1] && i < 4; i++) note_press();
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

/* ---------- Idle pets (community animations, see pets_data.h) ---------- */
static uint8_t presses_within(uint32_t ms) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < 8; i++)
        if (press_times[i] && timer_elapsed32(press_times[i]) < ms) n++;
    return n;
}

// Bongo Cat: idles, raises its paws after a key press, and slaps the desk on each press.
static void render_bongo(uint16_t frame) {
    uint32_t    last = press_times[(press_idx - 1) & 7];
    const char *f;
    if (last && timer_elapsed32(last) < 150) {
        f = bongo_tap[press_idx & 1]; // alternate paws
    } else if (last && timer_elapsed32(last) < 1500) {
        f = bongo_prep[0];
    } else {
        f = bongo_idle[(frame / 4) % 5];
    }
    oled_set_cursor(0, 0);
    oled_write_raw_P(f, 512);
}

// Draws a 32x24 page-format sprite at pixel column x (may be partly off screen), pages 1-3.
static void draw_luna_sprite(const char *sprite, int16_t x) {
    for (uint8_t p = 0; p < 3; p++)
        for (uint8_t c = 0; c < 32; c++) {
            int16_t col = x + c;
            if (col >= 0 && col < 128) oled_write_raw_byte(pgm_read_byte(sprite + p * 32 + c), (p + 1) * 128 + col);
        }
}

// Luna: sits when idle, walks or runs across the screen as you press keys,
// barks with Caps Lock and sneaks while Ctrl or Shift is held.
static void render_luna(uint16_t frame) {
    static int16_t x = 48;
    uint8_t        f      = (frame / 4) % 2;
    uint8_t        recent = presses_within(2000);
    const char    *s;
    if (host_keyboard_led_state().caps_lock) {
        s = luna_bark[f];
    } else if (get_mods() & MOD_MASK_CS) {
        s = luna_sneak[f];
        if (frame % 4 == 0) x++;
    } else if (recent >= 4) {
        s = luna_run[f];
        x += 2;
    } else if (recent >= 1) {
        s = luna_walk[f];
        if (frame % 2 == 0) x++;
    } else {
        s = luna_sit[f];
    }
    if (x > 128) x = -32;
    draw_luna_sprite(s, x);
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        uint32_t now = timer_read32();
        note_press();
        last_local_press = now ? now : 1;
        if (keycode == SCREEN_NEXT) {
            current_screen     = (current_screen + 1) % SCR_COUNT;
            screen_switch_time = now ? now : 1;
            eeconfig_update_user(current_screen);
            return false;
        }
    }
    return true;
}

/* ---------- Clock (big 7-segment digits, time synced from the PC) ---------- */
static void fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    for (uint8_t dy = 0; dy < h; dy++)
        for (uint8_t dx = 0; dx < w; dx++) oled_write_pixel(x + dx, y + dy, true);
}

// Segments a-g as bits 0-6.
static const uint8_t seven_seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void draw_digit(uint8_t x, uint8_t y, uint8_t digit) {
    const uint8_t w = 14, h = 24, t = 3, half = h / 2;
    uint8_t       s = seven_seg[digit];
    if (s & 0x01) fill_rect(x + t, y, w - 2 * t, t);                 // a
    if (s & 0x02) fill_rect(x + w - t, y + t, t, half - t);          // b
    if (s & 0x04) fill_rect(x + w - t, y + half, t, half - t);       // c
    if (s & 0x08) fill_rect(x + t, y + h - t, w - 2 * t, t);         // d
    if (s & 0x10) fill_rect(x, y + half, t, half - t);               // e
    if (s & 0x20) fill_rect(x, y + t, t, half - t);                  // f
    if (s & 0x40) fill_rect(x + t, y + half - 1, w - 2 * t, t);      // g
}

static void render_clock(void) {
    if (!clock_valid) {
        oled_set_cursor(0, 1);
        oled_write_P(PSTR("Clock: waiting for PC"), false);
        return;
    }
    static const char days[7][4]    = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char months[13][4] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    uint32_t now  = (clock_base_sec + timer_elapsed32(clock_stamp) / 1000) % 86400;
    uint8_t  hour = now / 3600, minute = now / 60 % 60, second = now % 60;
    bool     pm   = hour >= 12;
    uint8_t  h12  = hour % 12 ? hour % 12 : 12;

    const uint8_t y = 4;
    if (h12 >= 10) draw_digit(2, y, h12 / 10);
    draw_digit(20, y, h12 % 10);
    if (second % 2 == 0) { // blinking colon
        fill_rect(38, y + 6, 3, 3);
        fill_rect(38, y + 15, 3, 3);
    }
    draw_digit(45, y, minute / 10);
    draw_digit(63, y, minute % 10);

    char buf[8];
    oled_set_cursor(14, 0);
    oled_write_P(pm ? PSTR("PM") : PSTR("AM"), false);
    buf[0] = ':';
    buf[1] = '0' + second / 10;
    buf[2] = '0' + second % 10;
    buf[3] = '\0';
    oled_set_cursor(14, 1);
    oled_write(buf, false);
    oled_set_cursor(14, 2);
    oled_write(days[clock_dow % 7], false);
    oled_set_cursor(14, 3);
    oled_write(months[clock_month <= 12 ? clock_month : 0], false);
    buf[0] = ' ';
    buf[1] = clock_day >= 10 ? '0' + clock_day / 10 : ' ';
    buf[2] = '0' + clock_day % 10;
    oled_write(buf, false);
}

/* ---------- PC stats ---------- */
static void draw_stat_row(uint8_t line, const char *label, uint8_t percent) {
    const uint8_t x0 = 22, x1 = 98;
    uint8_t       y0 = line * 8 + 1, y1 = line * 8 + 6;
    if (percent > 100) percent = 100;
    oled_set_cursor(0, line);
    oled_write(label, false);
    for (uint8_t x = x0; x <= x1; x++) {
        oled_write_pixel(x, y0, true);
        oled_write_pixel(x, y1, true);
    }
    for (uint8_t yy = y0; yy <= y1; yy++) {
        oled_write_pixel(x0, yy, true);
        oled_write_pixel(x1, yy, true);
    }
    fill_rect(x0 + 1, y0 + 1, (uint16_t)(x1 - x0 - 1) * percent / 100, y1 - y0 - 1);
    char buf[5];
    buf[0] = percent >= 100 ? '1' : ' ';
    buf[1] = percent >= 10 ? '0' + percent / 10 % 10 : ' ';
    buf[2] = '0' + percent % 10;
    buf[3] = '%';
    buf[4] = '\0';
    oled_set_cursor(17, line);
    oled_write(buf, false);
}

static void render_stats(void) {
    oled_write_P(PSTR("PC stats"), false);
    if (!stats_valid || timer_elapsed32(stats_stamp) > 10000) {
        oled_set_cursor(0, 2);
        oled_write_P(PSTR("waiting for PC..."), false);
        return;
    }
    draw_stat_row(1, "CPU", stat_cpu);
    draw_stat_row(2, "RAM", stat_ram);
}

/* ---------- Spotify ---------- */
static bool spotify_active(void) {
    return host_seen && timer_elapsed32(host_stamp) < 5000 && play_status != 0;
}

static void render_spotify(uint16_t tick) {
    draw_icon(0, 0, spotify_icon);
    oled_set_cursor(2, 0);
    oled_write_P(PSTR("Spotify"), false);

    if (!spotify_active()) {
        oled_set_cursor(0, 2);
        oled_write_P(host_seen ? PSTR("Nothing playing") : PSTR("waiting for PC..."), false);
        return;
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
}

static const char *const screen_names[SCR_COUNT] = {"Auto", "Spotify", "Clock", "Bongo Cat", "Luna", "PC stats"};

bool oled_task_user(void) {
    static uint32_t frame_timer;
    static uint16_t frame;
    if (timer_elapsed32(frame_timer) < 50) return false;
    frame_timer = timer_read32();
    frame++;
    uint16_t tick = frame / 5; // marquee step every 250ms

    oled_clear();

    // While Fn is held, show what each key does on the Fn layer.
    if (layer_state_is(_FN)) {
        oled_write_P(PSTR("Fn  knob=brightness\n"), true);
        oled_write_P(PSTR("Boot   LEDs   Effect\n"), false);
        oled_write_P(PSTR("Paste  Hands  MicOff\n"), false);
        oled_write_P(PSTR("       PrtSc  Desktop"), false);
        return false;
    }

    // Key legend for the first 5 seconds after plugging in.
    if (timer_read32() < 5000) {
        oled_write_P(PSTR("Sage Hackpad  keys:\n"), true);
        oled_write_P(PSTR("Prev   Play   Next\n"), false);
        oled_write_P(PSTR("Claude Wispr  Mute\n"), false);
        oled_write_P(PSTR("Fn     Spot   Screen"), false);
        return false;
    }

    switch (current_screen) {
        case SCR_SPOTIFY: render_spotify(tick); break;
        case SCR_CLOCK: render_clock(); break;
        case SCR_BONGO: render_bongo(frame); break;
        case SCR_LUNA: render_luna(frame); break;
        case SCR_STATS: render_stats(); break;
        default: // Auto: Spotify while music is playing, Bongo Cat otherwise
            if (spotify_active()) {
                render_spotify(tick);
            } else {
                render_bongo(frame);
            }
            break;
    }

    // Briefly show the screen's name after switching.
    if (screen_switch_time && timer_elapsed32(screen_switch_time) < 1200) {
        char        label[22];
        const char *name = screen_names[current_screen];
        uint8_t     len = strlen(name), pad = (21 - len) / 2;
        memset(label, ' ', 21);
        memcpy(label + pad, name, len);
        label[21] = '\0';
        oled_set_cursor(0, 0);
        oled_write(label, true);
    }
    return false;
}
#endif
