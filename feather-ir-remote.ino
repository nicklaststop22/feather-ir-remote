// Feather M0 Bluefruit LE - phone-controlled IR remote, NEC scanner and decoder.
// Ported from the Mega 2560 scanner. No OLED and no buttons: the phone
// (Adafruit Bluefruit Connect, UART screen) is the display and the keypad.
// The USB serial monitor accepts the same commands and shows the same output.
//
// Wiring:
//   PNP driver (S9012, pins E-B-C flat face toward you):
//   3V -> emitter, pin 5 -> 1k -> base, collector -> 27 ohm -> IR LED anode,
//   IR LED cathode -> GND.  Emitter must be on 3V, not USB, or the LED never turns off.
//   TSOP38238 receiver: OUT -> pin 6, GND -> GND, VCC -> 3V
//   (Pins 8, 7, 4 and the SPI pins belong to the Bluefruit radio.)

#include <Arduino.h>
#define IR_SEND_PIN    5
#define IR_RECEIVE_PIN 6
#define USE_ACTIVE_LOW_OUTPUT_FOR_SEND_PIN   // PNP (S9012) driver: LOW = LED on, idle HIGH
#include <IRremote.hpp>
#include <Adafruit_BLE.h>
#include <Adafruit_BluefruitLE_SPI.h>

Adafruit_BluefruitLE_SPI ble(8, 7, 4);   // radio: CS, IRQ, RST

// Declared up here because the Arduino builder generates function
// prototypes above the first function, and pollInput() takes a LineIn&.
struct LineIn { String buf; unsigned long lastByte = 0; };
LineIn bleIn, usbIn;

// ---------- Known codes for your LED light (found with the Mega scanner) ----------
struct Named { const char* name; uint16_t addr; uint8_t cmd; };
const Named LIGHT[] = {
  {"off",       0xEF00, 0x02},   // 0
  {"on",        0xEF00, 0x03},   // 1
  {"red",       0xEF00, 0x04},   // 2  <- first color
  {"green",     0xEF00, 0x05},
  {"blue",      0xEF00, 0x06},
  {"white",     0xEF00, 0x07},
  {"orange",    0xEF00, 0x08},
  {"teal",      0xEF00, 0x09},
  {"blue2",     0xEF00, 0x0A},
  {"yellowish", 0xEF00, 0x0C},
  {"blueish",   0xEF00, 0x0D},
  {"purple",    0xEF00, 0x0E},
  {"red2",      0xEF00, 0x0F},
  {"yellow",    0xEF00, 0x10},
  {"blue3",     0xEF00, 0x11},
  {"purplish",  0xEF00, 0x12},
  {"white2",    0xEF00, 0x13},
  {"yellow2",   0xEF00, 0x14},
  {"blue4",     0xEF00, 0x15},
  {"purple2",   0xEF00, 0x16},   // 19 <- last color
  {"cycle",     0xEF00, 0x0B},
  {"strobe",    0xEF00, 0x17},
  {"off2",      0x80,   0x02},
};
const uint8_t NUM_LIGHT   = sizeof(LIGHT) / sizeof(LIGHT[0]);
const uint8_t COLOR_FIRST = 2, COLOR_LAST = 19;
uint8_t colorIdx = COLOR_FIRST;

// ---------- Quick power test (from the Mega sketch) ----------
struct CommonCode { uint16_t addr; uint8_t cmd; const char* label; };
const CommonCode QUICK[] = {
  {0x00,   0x40, "44-Key / Generic Power"},
  {0x00,   0x03, "24-Key Power ON (Set A)"},
  {0xEF00, 0x03, "24-Key Power ON (0xEF00)"},
  {0x00,   0x45, "Mini 21-Key / Puck Power"},
  {0x00,   0x12, "LED Bulb / Candle Power ON"},
  {0x00,   0x02, "24-Key Power OFF/ON Alt"},
  {0x00,   0x07, "RGB Controller Power"},
  {0x00,   0x47, "Puck Light Power Alt"},
  {0x80,   0x12, "Ceiling/Ring Light Power"},
  {0x00,   0x1A, "Generic Bulb Power"},
};

// ---------- Scanner state ----------
const uint16_t SCAN_ADDRS[] = { 0x00, 0xEF00, 0x80, 0x01 };
const uint8_t  NUM_SCAN_ADDRS = sizeof(SCAN_ADDRS) / sizeof(SCAN_ADDRS[0]);
const unsigned long SCAN_STEP_MS = 350;
uint8_t  scanAddrIdx = 0;
uint8_t  scanCmd = 0;
bool     scanning = false;
bool     advanceNext = false;   // false = resend the current code before stepping on
unsigned long lastScanMs = 0;

// ---------- Decoder state ----------
const int SLOTS = 8;
IRData slots[SLOTS];
bool   slotUsed[SLOTS] = {false};
IRData last;
bool   haveLast = false;
bool   learning = false;

// ---------- Output: every line goes to the phone and to USB serial ----------
void out(const String& s) { Serial.println(s); ble.println(s); }

String hex(uint32_t v, uint8_t digits) {
  char b[12];
  snprintf(b, sizeof(b), "0x%0*lX", digits, (unsigned long)v);
  return String(b);
}

String describe(const IRData& d) {
  return String(getProtocolString(d.protocol)) + " addr=" + hex(d.address, 4) +
         " cmd=" + hex(d.command, 2) + " bits=" + String(d.numberOfBits);
}

// Every transmission goes through these two, so the decoder never hears our own LED
void irSendNEC(uint16_t addr, uint8_t cmd, const String& tag) {
  if (learning) IrReceiver.stop();
  IrSender.sendNEC(addr, cmd, 0);
  if (learning) IrReceiver.start();
  out("-> NEC " + hex(addr, 4) + " " + hex(cmd, 2) + "  " + tag);
}

void irSendData(IRData& d, const String& tag) {
  if (learning) IrReceiver.stop();
  IrSender.write(&d);
  if (learning) IrReceiver.start();
  out("-> " + describe(d) + "  " + tag);
}

// ---------- Scanner ----------
void scanSend(const char* tag) { irSendNEC(SCAN_ADDRS[scanAddrIdx], scanCmd, tag); }

void scanPauseToggle() {
  scanning = !scanning;
  if (scanning) {
    advanceNext = false;     // resume by resending the current code, like the Mega did
    lastScanMs = 0;
    out("*** SCANNING  (p=pause) ***");
  } else {
    out("*** PAUSED at " + hex(SCAN_ADDRS[scanAddrIdx], 4) + " " + hex(scanCmd, 2) +
        "  (n=next b=back r=repeat p=resume) ***");
  }
}

void scanTick() {
  if (!scanning || millis() - lastScanMs < SCAN_STEP_MS) return;
  if (advanceNext && ++scanCmd == 0) {
    scanAddrIdx = (scanAddrIdx + 1) % NUM_SCAN_ADDRS;
    out("--- next address " + hex(SCAN_ADDRS[scanAddrIdx], 4) + " ---");
  }
  scanSend("[scan]");
  lastScanMs = millis();
  advanceNext = true;
}

void quickPowerTest() {
  out("Quick power test, about 7 s...");
  for (const CommonCode& q : QUICK) {
    irSendNEC(q.addr, q.cmd, q.label);
    delay(700);
  }
  out("Quick power test done");
}

// ---------- Decoder ----------
void checkDecoder() {
  if (!IrReceiver.decode()) return;
  IRData& d = IrReceiver.decodedIRData;
  if (!(d.flags & IRDATA_FLAGS_IS_REPEAT)) {        // ignore held-button repeats
    if (d.protocol == UNKNOWN) {
      out("<- unknown protocol, can't replay this one");
    } else {
      last = d; last.flags = 0; haveLast = true;
      out("<- " + describe(last) + "  (save N to keep)");
    }
  }
  IrReceiver.resume();
}

// ---------- Self-test ----------
bool bleOk = false;

// Probes the wiring with no extra parts:
//  pin 5: with an internal pull-down, a PNP base wired through 1k to an S9012
//         whose emitter is on 3V gets pulled HIGH through the base-emitter
//         junction. Nothing connected reads LOW.
//  pin 6: a powered TSOP receiver drives its output HIGH at idle, overpowering
//         the pull-down. A missing or unpowered receiver reads LOW.
//  loopback: send a dummy NEC code (address 0xBB, which your light ignores) with
//         the receiver listening, and see if it decodes its own LED.
void diag() {
  out("--- diag ---");
  out(String("BLE radio: ") + (bleOk ? "OK" : "NOT FOUND"));

  pinMode(IR_SEND_PIN, INPUT_PULLDOWN);
  delay(5);
  bool pnpSeen = digitalRead(IR_SEND_PIN) == HIGH;
  pinMode(IR_SEND_PIN, OUTPUT);
  digitalWrite(IR_SEND_PIN, HIGH);                 // PNP back off
  out(String("pin 5 (PNP driver): ") +
      (pnpSeen ? "S9012 base detected, emitter on 3V" : "nothing detected - check 1k, base leg, emitter on 3V"));

  IrReceiver.stop();
  pinMode(IR_RECEIVE_PIN, INPUT_PULLDOWN);
  delay(5);
  bool rxSeen = digitalRead(IR_RECEIVE_PIN) == HIGH;
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  out(String("pin 6 (receiver): ") +
      (rxSeen ? "receiver powered, output idling HIGH" : "LOW - receiver missing, unpowered, or OUT not on pin 6"));

  bool gotIt = false;
  if (rxSeen) {
    IrSender.sendNEC(0xBB, 0x5A, 0);                 // receiver left running on purpose
    unsigned long t = millis();
    while (millis() - t < 300) {
      if (IrReceiver.decode()) {
        IRData& d = IrReceiver.decodedIRData;
        if (d.protocol == NEC && d.address == 0xBB && d.command == 0x5A) gotIt = true;
        IrReceiver.resume();
        if (gotIt) break;
      }
    }
    out(String("IR loopback: ") +
        (gotIt ? "PASS - LED fires and receiver decodes it" :
                 "no decode - LED may not fire, or LED and receiver can't see each other"));
  } else {
    out("IR loopback: skipped (no receiver)");
  }
  if (!learning) IrReceiver.stop();
  out("--- end diag ---");
}

// ---------- Commands ----------
void help() {
  out("LIGHT: on off cycle strobe off2 next prev | colors:");
  String names;
  for (uint8_t i = COLOR_FIRST; i <= COLOR_LAST; i++) { names += LIGHT[i].name; names += ' '; }
  out("  " + names);
  out("SCAN:  p=pause/resume n=next b=back r=repeat quick");
  out("SEND:  send CC  or  send AAAA CC  (hex)");
  out("LEARN: learn stop replay save N play N list");
  out("TEST:  diag  (checks radio, PNP, receiver, IR loopback)");
}

void handle(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  // Scanner (same letters as the Mega sketch)
  if (cmd == "p" || cmd == "scan")  { scanPauseToggle(); return; }
  if (cmd == "n") { scanning = false; scanCmd++; scanSend("[paused] next"); return; }
  if (cmd == "b") { scanning = false; scanCmd--; scanSend("[paused] back"); return; }
  if (cmd == "r") { scanning = false;            scanSend("[paused] repeat"); return; }
  if (cmd == "quick") { scanning = false; quickPowerTest(); return; }

  // Named light codes
  for (uint8_t i = 0; i < NUM_LIGHT; i++) {
    if (cmd == LIGHT[i].name) {
      if (i >= COLOR_FIRST && i <= COLOR_LAST) colorIdx = i;
      irSendNEC(LIGHT[i].addr, LIGHT[i].cmd, LIGHT[i].name);
      return;
    }
  }
  if (cmd == "next" || cmd == "prev") {
    if (cmd == "next") colorIdx = (colorIdx >= COLOR_LAST)  ? COLOR_FIRST : colorIdx + 1;
    else               colorIdx = (colorIdx <= COLOR_FIRST) ? COLOR_LAST  : colorIdx - 1;
    irSendNEC(LIGHT[colorIdx].addr, LIGHT[colorIdx].cmd, LIGHT[colorIdx].name);
    return;
  }

  // Raw NEC: "send 13" (address 0xEF00) or "send 80 02"
  if (cmd.startsWith("send ")) {
    String a = cmd.substring(5); a.trim();
    int sp = a.indexOf(' ');
    uint16_t addr = 0xEF00;
    uint8_t  c;
    if (sp > 0) {
      addr = strtoul(a.substring(0, sp).c_str(), nullptr, 16);
      c    = strtoul(a.substring(sp + 1).c_str(), nullptr, 16);
    } else {
      c = strtoul(a.c_str(), nullptr, 16);
    }
    irSendNEC(addr, c, "[manual]");
    return;
  }

  // Decoder
  if (cmd == "learn") {
    scanning = false;
    learning = true;  IrReceiver.start();
    out("decoder ON - point a remote at the receiver");
    return;
  }
  if (cmd == "stop") { learning = false; IrReceiver.stop(); out("decoder OFF"); return; }
  if (cmd == "replay") {
    if (haveLast) irSendData(last, "[replay]"); else out("nothing learned yet");
    return;
  }
  if (cmd.startsWith("save ")) {
    int n = cmd.substring(5).toInt();
    if (!haveLast || n < 1 || n > SLOTS) { out("usage: save 1-8 (after learning a code)"); return; }
    slots[n - 1] = last; slotUsed[n - 1] = true;
    out("slot " + String(n) + " = " + describe(last));
    return;
  }
  if (cmd.startsWith("play ")) {
    int n = cmd.substring(5).toInt();
    if (n >= 1 && n <= SLOTS && slotUsed[n - 1]) irSendData(slots[n - 1], "[slot " + String(n) + "]");
    else out("slot empty");
    return;
  }
  if (cmd == "list") {
    for (int i = 0; i < SLOTS; i++)
      out(String(i + 1) + ": " + (slotUsed[i] ? describe(slots[i]) : String("-")));
    return;
  }

  if (cmd == "help" || cmd == "?") { help(); return; }
  if (cmd == "diag") { scanning = false; diag(); return; }
  out("? " + cmd + "  (type help)");
}

// Control Pad: 1 on, 2 off, 3 cycle, 4 strobe, Up/Down = color, Left/Right = scanner back/next
void handlePad(uint8_t b) {
  switch (b) {
    case 1: handle("on");     break;
    case 2: handle("off");    break;
    case 3: handle("cycle");  break;
    case 4: handle("strobe"); break;
    case 5: handle("next");   break;
    case 6: handle("prev");   break;
    case 7: handle("b");      break;
    case 8: handle("n");      break;
  }
}

// ---------- Input ----------
// A command ends at a newline, or after 60 ms of silence. The phone app may
// send text with no line ending, so the timeout catches those commands too.
void pollInput(Stream& s, LineIn& in) {
  while (s.available()) {
    char c = s.read();
    in.lastByte = millis();
    if (c == '\n' || c == '\r') { handle(in.buf); in.buf = ""; continue; }
    in.buf += c;
    if (in.buf.startsWith("!B") && in.buf.length() >= 5) {   // "!B" button state CRC
      if (in.buf[3] == '1') handlePad(in.buf[2] - '0');      // act on press only
      in.buf = "";
    }
  }
  if (in.buf.length() && millis() - in.lastByte > 60) { handle(in.buf); in.buf = ""; }
}

void setup() {
  pinMode(IR_SEND_PIN, OUTPUT);                  // PNP off right away: base HIGH = LED off
  digitalWrite(IR_SEND_PIN, HIGH);
  Serial.begin(115200);                          // no wait, so it runs on battery too
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  IrReceiver.stop();                             // decoder idle until "learn"
  // With IR_SEND_PIN #defined, IRremote 4.7 only links the feedback-pin-only begin()
  IrSender.begin(USE_DEFAULT_FEEDBACK_LED_PIN);   // send on pin 5; red LED 13 blinks on send

  bleOk = ble.begin(false);
  if (!bleOk) Serial.println(F("Bluefruit radio not found"));
  ble.echo(false);
  ble.sendCommandCheckOK("AT+GAPDEVNAME=IR-Remote");
  ble.setMode(BLUEFRUIT_MODE_DATA);
  out("IR-Remote ready - type help");
}

void loop() {
  pollInput(ble, bleIn);
  pollInput(Serial, usbIn);
  if (learning) checkDecoder();
  scanTick();
}
