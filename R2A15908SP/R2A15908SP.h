// Алксандр Лиман
// liman324@yandex.ru

#ifndef R2A15908SP_H
#define R2A15908SP_H
// address 7 bit
#define R2A15908SP_address 0b1000001 // 0x65 - 7 bit

//Sub addresses
#define VOLUME_L    0x00
#define VOLUME_R    0x01
#define INPUT_GAIN  0x02
#define STEREO_MODE 0x03
#define BASS_TREB   0x04


#include <Arduino.h>
class R2A15908SP
{
  public:
     R2A15908SP();
void setVolume_left(byte vol_l);        // volume left  -87...0 dB === byte 0...87
void setVolume_right(byte vol_r);       // volume right -87...0 dB === byte 0...87
void setIn_Gain(byte in, byte gain);    // in1...in5 === byte 0...4 /// mute === byte 7
                                        // gain_in 0...20 dB === byte 0...10 /// step 2 dB
void setStereo(byte stereo, byte mode); // byte 0 === stereo, byte 1 === Lch, byte 2 === Rch
                                        // byte 0 === Bypass, byte 1 === Tone, byte 2 === Tone&Surround Hi, byte 3 === Tone&Surround Low
void setTone(int bass, int treb);    // -14...14 dB === int -7...+7

  private:
	void writeWire(char a, char b);
};
	
#endif //R2A15908SP_H

