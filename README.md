# Ploopy Adept — stir-to-scroll (`via` keymap)

Custom firmware for the Ploopy Adept Trackball (`ploopyco/madromys/rev1_001`)
that adds **stir-to-scroll**: draw a small loop with the ball to arm scroll
mode, then just roll up/down to scroll. No scroll button needed.

Built on stock QMK behavior — everything else (buttons, drag-scroll toggle,
VIA) works as Ploopy shipped it.

## How it works

1. **Stir** a small closed loop with the ball (either direction, ~3/4 of a
   turn). The cursor freezes for a beat — scroll mode is armed.
2. **Roll up/down** to scroll. The cursor stays put; sideways motion is
   suppressed.
3. **Stop touching the ball** for ~0.6 s and scroll mode disarms; the next
   touch moves the cursor again.

Arming detection: the firmware tracks signed turning of the ball. Steady
looping in one direction accumulates; back-and-forth scrubbing cancels out
and never arms. Loops must be small on average (mean orbit radius under the
tuned ceiling), so big screen-share circling stays a cursor.

## Buttons

| Physical button      | Stock function | Custom behavior                          |
|----------------------|----------------|------------------------------------------|
| Small-left           | Forward (Btn5) | **Tap** = forward click, **hold 0.5 s** = toggle stir detection on/off |

All other buttons are stock (`Btn4`, drag-scroll toggle, `Btn2`, `Btn1`,
`Btn3`). The toggle state does not persist across reboots (defaults on).
Watch `qmk console` for `stir: detection on/off` confirmation.

> VIA shows the small-left key as a raw hex code. It is `QK_USER_0`
> (`0x7E40`); to move it, type `0x7E40` into VIA's **Any** keycode field.

## LED indicator

The Adept's RGB LED (if fitted) shows scroll state:

| LED        | Meaning                                              |
|------------|------------------------------------------------------|
| Off        | Cursor mode                                          |
| Cyan       | Scroll mode armed                                    |
| Dim cyan   | Scroll mode about to time out (last 200 ms)          |
| Cyan 1.5 s | Power-on self-test                                   |

## Tuning

All knobs are `#define`s at the top of
`keyboards/ploopyco/madromys/keymaps/via/keymap.c`:

| Define                  | Default | What it does                                      |
|-------------------------|---------|---------------------------------------------------|
| `STIR_ENTER_ANGLE`      | `5.0f`  | Steady turning (rad) needed to arm (~286°)        |
| `STIR_MAX_RADIUS_ENTER` | `392.0` | Mean orbit-radius ceiling to arm (sensor counts)  |
| `STIR_MIN_PATH`         | `250.0` | Minimum travel in the stroke (sensor counts)      |
| `STIR_SCROLL_IDLE_MS`   | `600`   | Stillness before scroll mode disarms              |
| `STIR_SCROLL_DIVISOR`   | `4.0`   | Ball travel per wheel notch (lower = faster)      |
| `STIR_SCROLL_INVERT`    | `false` | Flip scroll direction                             |
| `STIR_BTN5_HOLD_MS`     | `500`   | Hold time to toggle detection vs. tap             |
| `STIR_MAX_DISPLACE`     | `200.0` | Stroke restarts past this net travel (counts)     |
| `STIR_STROKE_GAP_MS`    | `250`   | Gap ending a non-scroll stroke                    |
| `STIR_EXIT_COOLDOWN_MS` | `200`   | Pause after disarm before re-arming               |

`qmk console` logs `stir: enter …`, `stir: exit idle`, and per-stroke
`angle/path/r` summaries (angle in milliradians) to guide tuning.

## Building & flashing

```sh
qmk compile -kb keyboards/ploopyco/madromys/rev1_001 -km via
