// seg7.ino — multiplexed 2-digit 7-segment display driver
// Adjust GPIO numbers below to match your wiring.

#include <Ticker.h>

// ── Pin defines (only place GPIO numbers appear) ──────────────────────────────
#define SEG_A  0
#define SEG_B  1
#define SEG_C  2
#define SEG_D  3
#define SEG_E  4
#define SEG_F  5
#define SEG_G  6
#define DIG1   7   // left digit common cathode — LOW = selected
#define DIG2   8   // right digit common cathode — LOW = selected

// ── Internal state ────────────────────────────────────────────────────────────
static const uint8_t IRAM_ATTR _SEG_PINS[] = {
  SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G
};

static volatile uint8_t _seg_left  = 0x00;
static volatile uint8_t _seg_right = 0x00;
static volatile uint8_t _seg_digit = 0;   // 0 = left, 1 = right
static Ticker _seg7Ticker;

// ── Multiplexer ISR (runs every 1 ms via Ticker) ──────────────────────────────
static void IRAM_ATTR seg7Isr() {
  // Deselect both digits first to prevent ghosting
  digitalWrite(DIG1, HIGH);
  digitalWrite(DIG2, HIGH);

  uint8_t segs = (_seg_digit == 0) ? _seg_left : _seg_right;
  for (uint8_t i = 0; i < 7; i++) {
    digitalWrite(_SEG_PINS[i], (segs >> i) & 1);
  }

  if (_seg_digit == 0) digitalWrite(DIG1, LOW);
  else                 digitalWrite(DIG2, LOW);

  _seg_digit ^= 1;
}

// ── Character encoding ────────────────────────────────────────────────────────
uint8_t seg7Encode(char c) {
  switch (c) {
    // Digits
    case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B;
    case '3': return 0x4F; case '4': return 0x66; case '5': return 0x6D;
    case '6': return 0x7D; case '7': return 0x07; case '8': return 0x7F;
    case '9': return 0x6F;
    // Letters used for parameter display
    case 'A': return 0x77; // input
    case 'b': return 0x7C; // bass / bypass
    case 'c': return 0x58; // mono (Lch / Rch)
    case 'G': return 0x3D; // gain
    case 'H': return 0x76; // surround Hi
    case 'L': return 0x38; // balance left / Lch / surround Lo
    case 'o': return 0x5C; // tone mode
    case 'P': return 0x73; // bypass
    case 'r': return 0x50; // balance right / Rch
    case 'S': return 0x6D; // stereo / surround
    case 't': return 0x78; // treble / tone
    case '-': return 0x40; // mute
    case ' ': return 0x00; // blank
    default:  return 0x00;
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

void seg7Setup() {
  for (uint8_t i = 0; i < 7; i++) {
    pinMode(_SEG_PINS[i], OUTPUT);
    digitalWrite(_SEG_PINS[i], LOW);
  }
  pinMode(DIG1, OUTPUT); digitalWrite(DIG1, HIGH); // HIGH = deselected
  pinMode(DIG2, OUTPUT); digitalWrite(DIG2, HIGH);

  _seg7Ticker.attach_ms(1, seg7Isr); // 1 ms → 500 Hz per digit, no flicker
}

void seg7Set(uint8_t left, uint8_t right) {
  portDISABLE_INTERRUPTS();
  _seg_left  = left;
  _seg_right = right;
  portENABLE_INTERRUPTS();
}

// Atomic pair read — call this from loop() to snapshot both digits together
void seg7GetPair(uint8_t *left, uint8_t *right) {
  portDISABLE_INTERRUPTS();
  *left  = _seg_left;
  *right = _seg_right;
  portENABLE_INTERRUPTS();
}
