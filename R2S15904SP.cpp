#include <Arduino.h>
#include "R2S15904SP.h"

// 16-bit word layout: bits[1:0] = register address (command)
#define _VOLCTRL  0x0002u   // volume left/right
#define _BTCTRL   0x0001u   // bass / treble
#define _INCTRL   0x0000u   // input selector + gain + tone enable
#define _TONE_EN  0x0200u   // bit 9: tone processing enable
#define _MUTE_CMD 0xFFFCu   // all-ones attenuation word (OR with _VOLCTRL → 0xFFFE)

// Bit-bang SPI: MSB first, 16 clocks, then a latch clock.
// The IC latches when it sees DI held HIGH through the falling edge of bit 0,
// then DI pulled LOW before the latch clock.
// Interrupts are disabled for the ~280 µs duration to prevent the 1 ms seg7
// Ticker ISR from injecting jitter between clock edges on ESP32.
void R2S15904SP::sendWord(uint16_t data) {
    Serial.printf("[SPI] 0x%04X\n", data);
    noInterrupts();
    for (uint8_t i = 0; i < 16; i++) {
        digitalWrite(_di,  (data & 0x8000) ? HIGH : LOW);
        delayMicroseconds(4);
        digitalWrite(_clk, HIGH);
        delayMicroseconds(4);
        digitalWrite(_di,  (i == 15) ? HIGH : LOW);
        delayMicroseconds(4);
        digitalWrite(_clk, LOW);
        delayMicroseconds(4);
        data <<= 1;
    }
    // Latch pulse: DI low, one extra clock + settling delay (matches WiseLord timing)
    digitalWrite(_di, LOW);
    delayMicroseconds(4);
    digitalWrite(_clk, HIGH);
    delayMicroseconds(4);
    digitalWrite(_clk, LOW);
    delayMicroseconds(4);
    interrupts();
}

void R2S15904SP::begin(uint8_t clkPin, uint8_t diPin) {
    _clk = clkPin;
    _di  = diPin;
    pinMode(_clk, OUTPUT);
    pinMode(_di,  OUTPUT);
    digitalWrite(_clk, LOW);
    digitalWrite(_di,  LOW);
}

void R2S15904SP::setVolume(int left_att, int right_att) {
    left_att  = constrain(left_att,  0, 89);
    right_att = constrain(right_att, 0, 89);
    // left  → bits [8:2],  right → bits [15:9]
    sendWord(((uint16_t)right_att << 9) | ((uint16_t)left_att << 2) | _VOLCTRL);
}

void R2S15904SP::setInput(uint8_t in, uint8_t gain, bool toneEnable) {
    in   = constrain(in,   0, 3);
    gain = constrain(gain, 0, 6);
    uint16_t tone = toneEnable ? _TONE_EN : 0u;
    // in → bits [15:13],  gain → bits [12:10]
    sendWord(((uint16_t)in << 13) | ((uint16_t)gain << 10) | tone | _INCTRL);
}

void R2S15904SP::setTone(int8_t bass, int8_t treble) {
    // Sign-magnitude encoding: positive → abs | 0x10, negative/zero → abs
    uint8_t b = (bass   > 0) ? ((uint8_t)bass   | 0x10u) : (uint8_t)(-bass);
    uint8_t t = (treble > 0) ? ((uint8_t)treble | 0x10u) : (uint8_t)(-treble);
    // bass → bits [15:11],  treble → bits [10:6]
    sendWord(((uint16_t)b << 11) | ((uint16_t)t << 6) | _BTCTRL);
}

void R2S15904SP::mute() {
    sendWord(0xFFFCu | _VOLCTRL);  // 0xFFFE — all-ones attenuation, WiseLord mute word
}
