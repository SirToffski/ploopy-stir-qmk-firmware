/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Copyright 2020 Christopher Courtney, aka Drashna Jael're  (@drashna) <drashna@live.com>
 * Copyright 2019 Sunjun Kim
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include QMK_KEYBOARD_H
#include <math.h>

// Custom keycode must precede the keymaps array that uses it.
enum custom_keycodes { STIR_BTN5 = QK_USER_0 }; // 0x7E40, via "Any" field

/* NOTE: VIA exports layers in matrix order (col0..col5), NOT LAYOUT order.
 * LAYOUT order here is [col1, col2, col3, col4, col0, col5], so the stock
 * VIA export [BTN1, BTN4, BTN5, DRAG, BTN2, BTN3] maps to:
 *   LAYOUT(BTN4, BTN5, DRAG, BTN2, BTN1, BTN3). */
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT( MS_BTN4, STIR_BTN5, DRAG_SCROLL, MS_BTN2, MS_BTN1, MS_BTN3 ),
    [1] = LAYOUT( _______, _______, _______, _______, _______, _______ ),
    [2] = LAYOUT( _______, _______, _______, _______, _______, _______ ),
    [3] = LAYOUT( _______, _______, _______, _______, _______, _______ ),
    [4] = LAYOUT( _______, _______, _______, _______, _______, _______ ),
    [5] = LAYOUT( _______, _______, _______, _______, _______, _______ ),
    [6] = LAYOUT( _______, _______, _______, _______, _______, _______ ),
    [7] = LAYOUT( _______, _______, _______, _______, _______, _______ )
};

/* Stir-to-scroll (via keymap only, default untouched).
 *
 * UX: a small closed-loop "stir" ARMS scroll mode (either direction). Once
 * armed, plain up/down ball motion scrolls and the cursor stays put. Scroll
 * mode disarms after STIR_SCROLL_IDLE_MS with no ball input.
 * Hold small-left (BTN5) to toggle detection; a short tap still sends
 * forward as normal.
 *
 * Detection: per-report signed turning totals. Back-and-forth scrub cancels
 * to ~zero and never arms; steady looping builds to STIR_ENTER_ANGLE with a
 * small mean radius (path / |angle|). No circle-purity test: real gestures
 * are ~2:1 ellipses whose flat sides barely turn per unit travel, so
 * straight-run/corner rejection goes deaf mid-orbit.
 *
 * Tune via `qmk console`. Angle logs are milliradians (5000 = 5.0 rad).
 */

/* ---- arming gates (stroke totals; signed angle so scrub cancels) ---- */
#define STIR_ENTER_ANGLE       5.0f    // ~286 deg of steady turning
#define STIR_MAX_RADIUS_ENTER  392.0f  // mean orbit radius ceiling, counts
#define STIR_MIN_PATH          250.0f  // real gate now, not decoration
#define STIR_STROKE_MAX_MS     1500    // abandon a stroke that drags on

/* ---- scroll-mode feel ---- */
#define STIR_SCROLL_IDLE_MS    600     // stillness before disarming
#define STIR_SCROLL_DIVISOR    4.0f    // matches PLOOPY_DRAGSCROLL_DIVISOR_V
#define STIR_SCROLL_INVERT     true   // flip if it opposes button drag-scroll

/* ---- input conditioning ---- */
#define STIR_NOISE_FLOOR       2.0f
#define STIR_STROKE_GAP_MS     250     // gap that ends a non-scroll stroke
#define STIR_EXIT_COOLDOWN_MS  200     // after disarm, before re-arming

/* ---- BTN5 tap (forward click) / hold (toggle stir detection) ---- */
#define STIR_BTN5_HOLD_MS      500     // hold past this to toggle, not tap

/* ---- indicator ---- */
#define STIR_LED_HUE           128     // cyan
#define STIR_LED_SAT           255
#define STIR_LED_VAL           255     // clamped to max_brightness (40)
#define STIR_LED_WARN_MS       200     // dim this long before timeout
#define STIR_LED_BOOT_TEST_MS  1500    // 0 disables the power-on self-test

#define STIR_MAX_DISPLACE   200.0f  // stroke restarts past this, counts
#define STIR_MIN_COHERENCE  0.0f   // |net turn| / total turn

// NOTE: is_drag_scroll lives in keyboards/ploopyco/ploopyco.c (non-static).
// If upstream ever makes it static this extern fails at link time -- then
// track DRAG_SCROLL toggles locally in process_record_user instead.
extern bool is_drag_scroll;

static float    stir_angle       = 0.0f;
static float    stir_path        = 0.0f;
static float    stir_scroll_frac = 0.0f;
static int16_t  stir_prev_dx     = 0;
static int16_t  stir_prev_dy     = 0;
static bool     stir_have_prev   = false;
static bool     stir_active      = false;
static uint16_t stir_last_motion = 0;
static uint16_t stir_last_exit   = 0;
static uint16_t stir_stroke_start = 0;
static bool     stir_timer_init  = false;
static uint16_t stir_boot_time   = 0;
static bool     stir_boot_done   = false;
static bool     stir_enabled     = true;  // master switch, BTN5 hold toggles
static uint16_t btn5_timer       = 0;
static bool     btn5_consumed    = false;

static float stir_net_x = 0.0f, stir_net_y = 0.0f;
static float stir_abs_angle = 0.0f;   // sum of |dtheta|, for coherence


/* ---------- indicator ---------- */

typedef enum { STIR_LED_OFF, STIR_LED_ON, STIR_LED_WARN } stir_led_state_t;

static void stir_disarm(uint16_t now); // defined in the stir section below

static void stir_led(stir_led_state_t want) {
#ifdef RGBLIGHT_ENABLE
    static stir_led_state_t have = 0xFF;
    if (want == have) return;   // only touch the driver on transitions
    have = want;
    switch (want) {
        case STIR_LED_OFF:
            rgblight_disable_noeeprom();
            break;
        case STIR_LED_ON:
            rgblight_enable_noeeprom();
            rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);
            rgblight_sethsv_noeeprom(STIR_LED_HUE, STIR_LED_SAT, STIR_LED_VAL);
            break;
        case STIR_LED_WARN:
            rgblight_sethsv_noeeprom(STIR_LED_HUE, STIR_LED_SAT, STIR_LED_VAL / 5);
            break;
    }
#else
    (void)want;
#endif
}

void keyboard_post_init_user(void) {
    stir_boot_time = timer_read();
#ifdef RGBLIGHT_ENABLE
#    if STIR_LED_BOOT_TEST_MS > 0
    stir_led(STIR_LED_ON);          // self-test: are these LEDs even fitted?
#    else
    stir_led(STIR_LED_OFF);
    stir_boot_done = true;
#    endif
#else
    stir_boot_done = true;
#endif
}

void housekeeping_task_user(void) {

    if (!stir_boot_done) {
        if (timer_elapsed(stir_boot_time) > STIR_LED_BOOT_TEST_MS) {
            stir_boot_done = true;
            stir_led(STIR_LED_OFF);
        }
        return;
    }

    // BTN5 hold-to-toggle fires here (not on release) for immediate feedback.
    if (!btn5_consumed && btn5_timer != 0 && timer_elapsed(btn5_timer) > STIR_BTN5_HOLD_MS) {
        stir_enabled  = !stir_enabled;
        btn5_consumed = true;
        if (!stir_enabled && stir_active) stir_disarm(timer_read());
#ifdef CONSOLE_ENABLE
        uprintf("stir: detection %s\n", stir_enabled ? "on" : "off");
#endif
    }

    if (stir_active) {
        uint16_t idle = timer_elapsed(stir_last_motion);
        stir_led((idle + STIR_LED_WARN_MS >= STIR_SCROLL_IDLE_MS)
                     ? STIR_LED_WARN : STIR_LED_ON);
    } else {
        stir_led(STIR_LED_OFF);
    }
}

/* ---------- BTN5 tap / hold ---------- */

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (keycode == STIR_BTN5) {
        if (record->event.pressed) {
            btn5_timer    = timer_read();
            btn5_consumed = false;
        } else {
            if (!btn5_consumed) {
                tap_code16(MS_BTN5); // short tap = normal forward click
            }
            btn5_timer = 0; // stop the hold detector from re-firing
        }
        return false;
    }
    return true;
}

/* ---------- stir ---------- */


static void stir_reset_stroke(uint16_t now) {
    stir_angle        = 0.0f;
    stir_path         = 0.0f;
    stir_net_x        = 0.0f;
    stir_net_y        = 0.0f;
    stir_abs_angle    = 0.0f;
    stir_have_prev    = false;
    stir_stroke_start = now;
}

static void stir_disarm(uint16_t now) {
    stir_active      = false;
    stir_last_exit   = now;
    stir_scroll_frac = 0.0f;
    stir_reset_stroke(now);
}

// Drain whole scroll units into the report. Always clears x/y/h: in scroll
// mode the cursor must never move, on any path out of the task.
static void stir_emit_scroll(report_mouse_t *report) {
    int16_t v_out = (int16_t)stir_scroll_frac;
    if (v_out >  127) v_out =  127;
    if (v_out < -127) v_out = -127;
    stir_scroll_frac -= v_out;
    report->v = (int8_t)CONSTRAIN_HID((int16_t)report->v + v_out);
    report->x = 0;
    report->y = 0;
    report->h = 0;
}

static void stir_accumulate_scroll(int16_t dy) {
    // Sign matches Ploopy's drag scroll with PLOOPY_DRAGSCROLL_INVERT unset.
    stir_scroll_frac += (STIR_SCROLL_INVERT ? -(float)dy : (float)dy)
                        / STIR_SCROLL_DIVISOR;
}

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    int16_t  dx  = mouse_report.x;
    int16_t  dy  = mouse_report.y;
    uint16_t now = timer_read();

    if (!stir_timer_init) {
        stir_last_motion  = now;
        stir_last_exit    = now;
        stir_stroke_start = now;
        stir_timer_init   = true;
    }

    // Detection master switch (BTN5 hold toggles). Everything below is
    // stock behavior while off.
    if (!stir_enabled) {
        if (stir_active) stir_disarm(now);
        return mouse_report;
    }

    // Never fight button drags/selects or the stock drag-scroll toggle.
    if (is_drag_scroll || mouse_report.buttons != 0) {
        if (stir_active) stir_disarm(now);
        else             stir_reset_stroke(now);
        stir_scroll_frac = 0.0f;
        stir_last_motion = now;
        return mouse_report;
    }

    float mag = sqrtf((float)dx * dx + (float)dy * dy);

    /* --- below the noise floor: idle handling --- */
    if (mag < STIR_NOISE_FLOOR) {
        if (stir_active) {
            // Sub-floor is not zero. Suppress it or the cursor creeps while
            // scrolling slowly -- exactly when sub-floor reports are commonest.
            mouse_report.x = 0;
            mouse_report.y = 0;
            mouse_report.h = 0;
            if (timer_elapsed(stir_last_motion) > STIR_SCROLL_IDLE_MS) {
#ifdef CONSOLE_ENABLE
                uprintf("stir: exit idle\n");
#endif
                stir_disarm(now);
            }
        } else if (timer_elapsed(stir_last_motion) > STIR_STROKE_GAP_MS) {
#ifdef CONSOLE_ENABLE
            if (stir_path > 15.0f) {
                float a = fabsf(stir_angle);
                float r = (a > 0.3f) ? (stir_path / a) : 9999.0f;
                uprintf("stir: stroke angle=%d path=%d r=%d "
                        "(need |angle|>%d, path>%d, r<%d)\n",
                        (int)(stir_angle * 1000), (int)stir_path, (int)r,
                        (int)(STIR_ENTER_ANGLE * 1000), (int)STIR_MIN_PATH,
                        (int)STIR_MAX_RADIUS_ENTER);
            }
#endif
            stir_reset_stroke(now);
        }
        return mouse_report;
    }

    /* --- real motion --- */
    uint16_t gap_limit = stir_active ? STIR_SCROLL_IDLE_MS : STIR_STROKE_GAP_MS;
    if (timer_elapsed(stir_last_motion) > gap_limit) {
        if (stir_active) stir_disarm(now);
        else             stir_reset_stroke(now);
        stir_scroll_frac = 0.0f;
    }
    stir_last_motion = now;

    if (stir_active) {
        stir_accumulate_scroll(dy);
        stir_emit_scroll(&mouse_report);
        stir_prev_dx   = dx;
        stir_prev_dy   = dy;
        stir_have_prev = true;
        return mouse_report;
    }

    // Bound the accumulators: without this, a long uninterrupted session
    // inflates path/angle and r_est stops reflecting what the hand is doing.
    if (timer_elapsed(stir_stroke_start) > STIR_STROKE_MAX_MS) {
        stir_reset_stroke(now);
    }

    if (stir_have_prev) {
        float dot    = (float)stir_prev_dx * dx + (float)stir_prev_dy * dy;
        float cross  = (float)stir_prev_dx * dy - (float)stir_prev_dy * dx;
        float dtheta = atan2f(cross, dot);

        stir_angle += dtheta;
        stir_path  += mag;

        stir_net_x += dx;
        stir_net_y += dy;
        stir_abs_angle += fabsf(dtheta);

        float disp = sqrtf(stir_net_x * stir_net_x + stir_net_y * stir_net_y);
        if (disp > STIR_MAX_DISPLACE) {
            stir_reset_stroke(now);   // travelling, not stirring -- start over here
            stir_prev_dx = dx;
            stir_prev_dy = dy;
            stir_have_prev = true;
            return mouse_report;
        }

        float coherence = (stir_abs_angle > 0.1f) ? (fabsf(stir_angle) / stir_abs_angle) : 0.0f;

        float abs_angle = fabsf(stir_angle);
        float r_est     = (abs_angle > 0.3f) ? (stir_path / abs_angle) : 9999.0f;
        bool  cooled    = timer_elapsed(stir_last_exit) > STIR_EXIT_COOLDOWN_MS;

        if (cooled && abs_angle > STIR_ENTER_ANGLE
                   && stir_path  > STIR_MIN_PATH
                   && r_est      < STIR_MAX_RADIUS_ENTER
                   && coherence > STIR_MIN_COHERENCE) {
#ifdef CONSOLE_ENABLE
            if (stir_path > 15.0f) {
                float a   = fabsf(stir_angle);
                float r   = (a > 0.3f) ? (stir_path / a) : 9999.0f;
                float coh = (stir_abs_angle > 0.1f) ? (a / stir_abs_angle) : 0.0f;
                uprintf("stir: stroke angle=%d path=%d r=%d coh=%d\n",
                        (int)(stir_angle * 1000), (int)stir_path, (int)r,
                        (int)(coh * 100));
            }
#endif
            stir_active      = true;
            stir_scroll_frac = 0.0f;
            stir_accumulate_scroll(dy);
            stir_emit_scroll(&mouse_report);
            stir_prev_dx   = dx;
            stir_prev_dy   = dy;
            stir_have_prev = true;
            return mouse_report;
        }
    }

    stir_prev_dx   = dx;
    stir_prev_dy   = dy;
    stir_have_prev = true;
    return mouse_report;
}
