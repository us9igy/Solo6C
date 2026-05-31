#include <Arduino.h>
#include <Wire.h>
#include "R2A15908SP.h"

R2A15908SP::R2A15908SP(){
	// Wire is initialized by the application (solo6c.ino setup()) with the correct SDA/SCL pins.
	// Do NOT call Wire.begin() here — this constructor runs before setup(), during static
	// initialization, before the Arduino HAL is ready.
}

void R2A15908SP::setVolume_left(byte vol_l){
  vol_l = ((87 - vol_l)  << 1);
  writeWire(VOLUME_L, vol_l);
}

void R2A15908SP::setVolume_right(byte vol_r){
  vol_r = ((87 - vol_r)  << 1);
  writeWire(VOLUME_R, vol_r);
}

void R2A15908SP::setIn_Gain(byte in, byte gain){
  byte sum_in_gain = (in << 5) + (gain << 1);
  writeWire(INPUT_GAIN, sum_in_gain);
}

void R2A15908SP::setStereo(byte stereo, byte mode){
  byte sum_st_md = (stereo << 6) + (mode << 4);
  writeWire(STEREO_MODE, sum_st_md);
}

void R2A15908SP::setTone(int bass, int treb){
  if(bass >= 0){bass = ((bass + 0b1000) << 4);}
  if(bass < 0){bass = (abs(bass) << 4);}
  if(treb >= 0){treb = treb + 0b1000;}
  if(treb < 0){treb = abs(treb);}
  byte sum_bass_treb = bass + treb;
  writeWire(BASS_TREB, sum_bass_treb);
}

void R2A15908SP::writeWire(char a, char b){
  Wire.beginTransmission(R2A15908SP_address);
  Wire.write(a);
  Wire.write(b);
  uint8_t err = Wire.endTransmission();
  if (err) {
    Serial.print(F("I2C NACK sub=0x")); Serial.print((uint8_t)a, HEX);
    Serial.print(F(" err=")); Serial.println(err);
  }
}
