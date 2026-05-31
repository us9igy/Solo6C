#pragma once
#include <Arduino.h>

// Driver for the Renesas R2S15904SP audio processor.
// Protocol: 3-wire software SPI, 16-bit words MSB-first.
// Register address occupies bits [1:0] of the 16-bit word.
// No chip-select; the IC latches on the extra clock pulse after bit 0.

class R2S15904SP {
public:
    R2S15904SP() : _clk(0), _di(0) {}

    // Call once in setup() — configures CLK and DI pins as OUTPUT.
    void begin(uint8_t clkPin, uint8_t diPin);

    // left_att / right_att: attenuation in dB, 0 = max volume, 89 = fully attenuated.
    void setVolume(int left_att, int right_att);

    // in: input index 0–3.  gain: 0–7 (0 = 0 dB, each step ~2 dB).
    // toneEnable: true = tone/surround processing active, false = bypass.
    void setInput(uint8_t in, uint8_t gain, bool toneEnable);

    // bass / treble: –7 … +7 (each step = 2 dB).
    void setTone(int8_t bass, int8_t treble);

    // Hardware mute via max-attenuation volume word.
    void mute();

private:
    uint8_t _clk, _di;
    void sendWord(uint16_t data);
};
