# feather-ir-remote

Phone-controlled IR remote for an RGB LED light, running on an **Adafruit Feather M0 Bluefruit LE**.
Commands come in over Bluetooth from the **Adafruit Bluefruit Connect** app (UART screen or Control Pad)
or over USB serial. The Feather sends NEC IR codes, and it also includes an NEC scanner and an IR decoder for learning codes from other remotes.

Ported from an Arduino Mega 2560 scanner sketch. The OLED and physical buttons were dropped: the phone is the display and keypad.

## Hardware

**IR LED, driven by an S9012 PNP transistor**
(S9012 pinout: E-B-C, flat face toward you, legs down)

```
Feather 3V ─────────── S9012 Emitter (left)
Feather pin 5 ─[1kΩ]── S9012 Base    (middle)
S9012 Collector (right) ─[27Ω]── IR LED long leg (+)
IR LED short leg (−) ── Feather GND
```

The emitter must go to **3V**, not USB/5V. The pin only swings to 3.3V, so a 5V emitter would never turn off.
The sketch sets `USE_ACTIVE_LOW_OUTPUT_FOR_SEND_PIN` because a PNP turns on when the base is pulled LOW.
**If you use an NPN instead** (emitter to GND, LED + resistor from USB 5V to the collector), delete that `#define`.

**IR receiver (TSOP38238 / VS1838B)**, dome toward you: OUT → pin 6, GND → GND, VCC → **3V**

Pins 8, 7, 4 and the SPI pins belong to the Bluefruit radio. Don't use them.

## Build / upload

Libraries: **IRremote** (4.7.x) and **Adafruit BluefruitLE nRF51**.

```
arduino-cli compile --upload -p COM4 --fqbn adafruit:samd:adafruit_feather_m0 feather-ir-remote
```

## Commands

Send these from Bluefruit Connect → UART (line ending optional) or the USB serial monitor at 115200 baud.

| Command | Does |
|---|---|
| `on` `off` `cycle` `strobe` `off2` | Light codes |
| `red` `green` `blue` `white` `orange` `teal` `yellow` `purple` … | Colors (`help` lists all) |
| `next` / `prev` | Step through the colors |
| `p` | Start / pause the NEC scanner |
| `n` `b` `r` | Scanner next / back / repeat (pauses) |
| `quick` | Try 10 common LED-strip power codes |
| `send CC` / `send AAAA CC` | Any NEC code in hex (address defaults to EF00) |
| `learn` `stop` `replay` `save N` `play N` `list` | Decoder and 8 RAM slots |
| `diag` | Self-test: radio, PNP wiring, receiver, IR loopback |

**Control Pad:** 1 on, 2 off, 3 cycle, 4 strobe, Up/Down colors, Left/Right scanner back/next.

## Light codes (NEC, address 0xEF00)

Found by scanning with the original Mega sketch:

| Cmd | Name | Cmd | Name |
|---|---|---|---|
| 0x02 | off | 0x0E | purple |
| 0x03 | on | 0x0F | red2 |
| 0x04 | red | 0x10 | yellow |
| 0x05 | green | 0x11 | blue3 |
| 0x06 | blue | 0x12 | purplish |
| 0x07 | white | 0x13 | white2 (slow brighten) |
| 0x08 | orange | 0x14 | yellow2 |
| 0x09 | teal | 0x15 | blue4 |
| 0x0A | blue2 | 0x16 | purple2 |
| 0x0B | cycle all colors | 0x17 | strobe |
| 0x0C | yellowish | | |
| 0x0D | blueish | | |

`off2` = address 0x80, cmd 0x02 also turns it off.

## Notes

- `diag`'s loopback decode can fail even when everything works: the Feather can't reliably decode while it is sending. If the receiver module's LED blinks on each send, the IR LED is working.
- Learned codes in `save N` slots are RAM only and are lost on reset.
