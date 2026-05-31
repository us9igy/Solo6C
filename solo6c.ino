// ESP32C6 port of solo6c — R2S15904SP audio controller
// Requires (install via Library Manager):
//   - IRremote v3.x+
// Bundled libraries (project folder): R2S15904SP

// --- IR button codes (NEC 32-bit) ---
#define IR_INPUT    0xFB04FE01UL  // Input switch
#define IR_MUTE     0xFD02FE01UL  // Mute
#define IR_VOLUP    0xEE11FE01UL  // Vol+
#define IR_VOLDOWN  0xEF10FE01UL  // Vol-
#define IR_TREBUP   0xF609FE01UL  // Treble+
#define IR_TREBDOWN 0xF20DFE01UL  // Treble-
#define IR_BASUP    0xF50AFE01UL  // Bass+
#define IR_BASDOWN  0xF10EFE01UL  // Bass-

// --- ESP32C6 GPIO assignments (adjust to your wiring) ---
#define ENC_CLK      20   // Encoder CLK (A) — S1
#define ENC_DT       19   // Encoder DT  (B) — S2
#define ENC_BTN      18   // Encoder pushbutton (active LOW) — SW
#define IR_RECV_PIN  14   // IR receiver signal pin
#define STANDBY_PIN  21   // Standby relay/output
#define SPI_DI       22   // R2S15904SP data in (DI)
#define SPI_CLK      23   // R2S15904SP clock (CLK)

// ── Boot isolation — flip 0→1 one by one to find crash ───────────────────────
#define ENABLE_ENCODER 1
#define ENABLE_IR      1
#define ENABLE_SEG7    1
#define ENABLE_AUDIO   1

#include <Preferences.h>
#include "R2S15904SP.h"
#define DECODE_NEC
#include <IRremote.hpp>   // IRremote v3+
#include <Ticker.h>

R2S15904SP        ra;
Preferences       prefs;
volatile int _encAcc = 0;
bool webDirty = false;
byte encMode = 0;  // 0=volume, 1=param-select, 2=param-edit

unsigned long times, times1;
long          oldPosition = 0, newPosition;
byte          w, w2, power, gr1, gr2, in, in_old, xd, www, mute;
int           mode, menu, vol, menu_in, x;
int           gain0, gain1, gain2, gain3, gain4, gain5;
int           bas, treb, ball, stereo;

// IR state — persists between loop() calls like ir.value did in original
unsigned long irValue  = 0;
bool          irRepeat = false;

enum DisplayParam : byte {
  PARAM_VOL = 0, PARAM_BAS, PARAM_TREB, PARAM_BALL,
  PARAM_IN, PARAM_GAIN, PARAM_ST, PARAM_MODE
};
byte          displayParam = PARAM_VOL;
unsigned long displayTime  = 0;

static const byte _menuParam[] = {
  PARAM_VOL, PARAM_BAS, PARAM_TREB, PARAM_BALL, PARAM_ST, PARAM_MODE, PARAM_IN
};

#if ENABLE_ENCODER
// 1 ms Ticker samples both CLK and DT simultaneously — eliminates the timing
// race that occurs when reading DT inside a CLK interrupt with RC-filtered signals.
static Ticker _encTicker;
static void IRAM_ATTR _encPoll() {
  static uint8_t last = 0xFF;
  static const int8_t tbl[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  uint8_t now = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  if (last == 0xFF) { last = now; return; }
  _encAcc += tbl[(last << 2) | now];
  last = now;
}
#endif

// ---------- helpers (match originals) ----------
void cl()  { irValue = 0; irRepeat = false; }
void cl1() { irValue = 0; irRepeat = false; delay(20); }
void cl2() { irValue = 0; irRepeat = false; }
void cl3() { irValue = 0; irRepeat = false; delay(100); }

void ball_func() { if (ball >  6) ball =  6; if (ball < -6) ball = -6; }
void treb_func() { if (treb >  7) treb =  7; if (treb < -7) treb = -7; }
void bass_func() { if (bas  >  7) bas  =  7; if (bas  < -7) bas  = -7; }
void gain_func() { if (gain0 > 6) gain0 = 6; if (gain0 < 0) gain0 = 0; }
void vol_func()  { if (vol > 87) vol = 87; if (vol < 12) vol = 12; }

// R2S15904SP has 4 inputs (0–3).
// vol=87 → 0 dB attenuation (loudest), vol=12 → 75 dB attenuation (near-mute).
// in=7 is the mute sentinel set by the mute/power logic above.
// Write order matches WiseLord sndPowerOn: INCTRL → BTCTRL → VOLCTRL.
// VOLCTRL (address 0x02) is the commit trigger — IC latches all three registers when
// VOLCTRL is received last.  Writing VOLCTRL first (old order) committed stale values.
// Input reversed: IC input = 3 - in (WiseLord convention).
void audio() {
  webDirty = true;
  if (in == 7) { ra.mute(); return; }

  int l_att = constrain(87 - vol + ball, 0, 89);
  int r_att = constrain(87 - vol - ball, 0, 89);
  if (stereo == 1) r_att = 89;
  if (stereo == 2) l_att = 89;

  Serial.printf("[audio] vol=%d l=%d r=%d bas=%d treb=%d in=%d gain=%d mode=%d st=%d\n",
                vol, l_att, r_att, bas, treb, in, gain0, mode, stereo);

  ra.setInput((uint8_t)(3 - constrain(in, 0, 3)), (uint8_t)constrain(gain0, 0, 6), mode != 0);
  delay(1);
  ra.setTone((int8_t)bas, (int8_t)treb);
  delay(1);
  ra.setVolume(l_att, r_att);
}

void updateDisplay() {
  if (power == 1) { seg7Set(0x00, 0x00); return; }
  if (mute  == 1) { seg7Set(0x40, 0x40); return; }  // '--'

  bool fallback = (displayParam == PARAM_VOL) || (millis() - displayTime >= 3000);
  // Notify web UI exactly once when the display transitions back to volume
  static bool _wasFallback = true;
  if (fallback && !_wasFallback) webDirty = true;
  _wasFallback = fallback;
  if (fallback) {
    int v = constrain(87 - vol, 0, 99);  // 0=loudest, 75=near-mute
    seg7Set(seg7Encode('0' + v / 10), seg7Encode('0' + v % 10));
    return;
  }

  switch (displayParam) {
    case PARAM_BAS: {
      uint8_t n = seg7Encode('0' + abs(bas));
      if (bas < 0 && (millis() / 600) & 1)
        seg7Set(seg7Encode('-'), n);
      else
        seg7Set(seg7Encode('b'), n);
      break;
    }
    case PARAM_TREB: {
      uint8_t n = seg7Encode('0' + abs(treb));
      if (treb < 0 && (millis() / 600) & 1)
        seg7Set(seg7Encode('-'), n);
      else
        seg7Set(seg7Encode('t'), n);
      break;
    }
    case PARAM_BALL:
      if      (ball == 0) seg7Set(seg7Encode('0'), seg7Encode('0'));
      else if (ball  > 0) seg7Set(seg7Encode('r'), seg7Encode('0' +  ball));
      else                seg7Set(seg7Encode('L'), seg7Encode('0' + -ball));
      break;
    case PARAM_IN:
      seg7Set(seg7Encode('A'), seg7Encode('1' + (in - 2)));
      break;
    case PARAM_GAIN: {
      uint8_t L = seg7Encode('G');
      seg7Set(L, seg7Encode('0' + gain0 % 10));
      break;
    }
    case PARAM_ST:
      switch (stereo) {
        case 0: seg7Set(seg7Encode('S'), seg7Encode('t')); break;
        case 1: seg7Set(seg7Encode('L'), seg7Encode('c')); break;
        case 2: seg7Set(seg7Encode('r'), seg7Encode('c')); break;
      }
      break;
    case PARAM_MODE:
      switch (mode) {
        case 0: seg7Set(seg7Encode('b'), seg7Encode('P')); break;
        case 1: seg7Set(seg7Encode('t'), seg7Encode('o')); break;
      }
      break;
    default: break;
  }
}

void setDisplay(byte param) {
  displayParam = param;
  displayTime  = millis();
  updateDisplay();
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC enumerate before first print
  Serial.println(F("[1] Serial OK"));

  pinMode(STANDBY_PIN, OUTPUT);
#if ENABLE_ENCODER
  pinMode(ENC_BTN, INPUT_PULLUP);
#endif
  Serial.println(F("[2] Pins OK"));

#if ENABLE_AUDIO
  ra.begin(SPI_CLK, SPI_DI);
  Serial.println(F("[3] R2S15904SP SPI OK"));
#else
  Serial.println(F("[3] Audio skipped"));
#endif

#if ENABLE_IR
  IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK);
  Serial.println(F("[4] IR OK"));
#else
  Serial.println(F("[4] IR skipped"));
#endif

  prefs.begin("solo6c", false);
  vol    = prefs.getUChar("vol",    50);
  in     = prefs.getUChar("in",      2);
  if (in < 2 || in > 3) in = 2;   // only inputs 2 and 3 are wired (displayed as A1, A2)
  bas    = (int)prefs.getUChar("bas",   7) - 7;
  treb   = (int)prefs.getUChar("treb",  7) - 7;
  ball   = (int)prefs.getUChar("ball",  6) - 6;
  stereo = prefs.getUChar("stereo",  0);
  mode   = prefs.getUChar("mode",    1); if (mode > 1) mode = 1;
  gain1  = prefs.getUChar("gain1",   0);
  gain2  = prefs.getUChar("gain2",   0);
  gain3  = prefs.getUChar("gain3",   0);
  gain4  = prefs.getUChar("gain4",   0);
  gain5  = prefs.getUChar("gain5",   0);
  Serial.println(F("[5] NVS OK"));

  switch (in) {
    case 2: gain0 = gain3; break;
    case 3: gain0 = gain4; break;
  }
#if ENABLE_AUDIO
  ra.mute();          // mute before writing parameters (WiseLord sndPowerOn convention)
  delay(10);
  audio();            // INCTRL → BTCTRL → VOLCTRL — unmutes as final step
  Serial.println(F("[6] Audio OK"));
#else
  Serial.println(F("[6] Audio skipped"));
#endif

#if ENABLE_SEG7
  seg7Setup();
  Serial.println(F("[7] Seg7 OK"));
  updateDisplay();
  Serial.println(F("[8] Display OK"));
#else
  Serial.println(F("[7-8] Seg7 skipped"));
#endif

#if ENABLE_ENCODER
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  _encTicker.attach_ms(1, _encPoll);
  Serial.println(F("[E] Encoder OK"));
#else
  oldPosition = 0;  // prevent spurious trigger when encoder disabled
  Serial.println(F("[E] Encoder skipped"));
#endif

  setupWebUI();
  Serial.println(F("[9] Done"));
}

// ---------- loop ----------
void loop() {
  newPosition = _encAcc / 4;

  handleWebUI();
#if ENABLE_SEG7
  if (encMode != 0 && power == 0 && mute == 0) displayTime = millis();
  updateDisplay();
#endif

  if (power == 0) { digitalWrite(STANDBY_PIN, HIGH); } else { digitalWrite(STANDBY_PIN, LOW); }

#if ENABLE_IR
  // IR receive
  irRepeat = false;
  if (IrReceiver.decode()) {
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
      irRepeat = true;
    } else {
      irValue = IrReceiver.decodedIRData.decodedRawData;
      Serial.print("0x"); Serial.println(irValue, HEX);
      times = millis(); w = 1;
    }
    IrReceiver.resume();
  }
  if (irValue == 0) { gr1 = 0; gr2 = 0; }

  // ---------- IR commands ----------
  if (power == 0) {
    if (irValue == IR_INPUT) { menu=0; in++; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; if (in > 3) in = 2; audio(); setDisplay(PARAM_IN); }
    if (irValue == IR_MUTE && mute == 0) { mute=1; menu=100; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; in_old=in; in=7; audio(); updateDisplay(); delay(300); }
    if (irValue == IR_MUTE && mute == 1) { mute=0; menu=0; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; in=in_old; audio(); updateDisplay(); delay(300); }
  }
#endif // ENABLE_IR

#if ENABLE_ENCODER
  // ---------- encoder button (non-blocking) ----------
  static bool          _btnWas    = false;
  static unsigned long _btnDownAt = 0;
  bool _btnNow = (digitalRead(ENC_BTN) == LOW);

  if (_btnNow && !_btnWas)  _btnDownAt = millis();

  if (!_btnNow && _btnWas) {
    unsigned long held = millis() - _btnDownAt;
    if (held >= 800) {
      // long press: mute/standby cycle, always resets to volume mode
      encMode = 0; menu = 0;
      if (power == 1) {
        power=0; in=in_old; audio(); updateDisplay();
      } else if (mute == 1) {
        mute=0; in=in_old; audio(); updateDisplay();
      } else {
        mute=1; in_old=in; in=7; audio(); updateDisplay();
      }
      times=millis(); w=1; w2=1;
    } else if (held >= 30) {
      // short press: advance encoder mode  0→1→2→0
      if (power == 0 && mute == 0) {
        encMode++;
        if (encMode == 1) {
          if (menu < 1 || menu > 6) menu = 1;
          setDisplay(_menuParam[menu]);
        } else if (encMode == 2) {
          setDisplay(_menuParam[menu]);
        } else if (encMode == 3 && menu == 6) {
          // input is selected — one extra click enters gain edit for that input
          setDisplay(PARAM_GAIN);
        } else {
          encMode = 0; menu = 0;
          setDisplay(PARAM_VOL);
        }
        times=millis(); w=1; w2=1;
      }
    }
  }
  _btnWas = _btnNow;

  // ---------- encoder rotation ----------
  if (newPosition != oldPosition && power == 0 && mute == 0) {
    int delta = newPosition;
    _encAcc = 0; newPosition = 0; oldPosition = 0;
    times=millis(); w=1; w2=1;
    if (encMode == 0) {
      vol += delta; vol_func(); audio(); setDisplay(PARAM_VOL);
    } else if (encMode == 1) {
      menu += delta;
      if (menu < 1) menu = 6;
      if (menu > 6) menu = 1;
      setDisplay(_menuParam[menu]);
    } else if (encMode == 3) {
      gain0 += delta; gain_func();
      switch (in) { case 2: gain3=gain0; break; case 3: gain4=gain0; break; }
      audio(); setDisplay(PARAM_GAIN);
    } else {
      switch (menu) {
        case 1: bas    += delta; bass_func(); audio(); setDisplay(PARAM_BAS);  break;
        case 2: treb   += delta; treb_func(); audio(); setDisplay(PARAM_TREB); break;
        case 3: ball   += delta; ball_func(); audio(); setDisplay(PARAM_BALL); break;
        case 4: stereo  = ((stereo + delta) % 3 + 3) % 3; audio(); setDisplay(PARAM_ST);   break;
        case 5: mode    = ((mode   + delta) % 2 + 2) % 2; audio(); setDisplay(PARAM_MODE); break;
        case 6:
          in = (in - 2 + delta + 2) % 2 + 2;
          switch (in) {
            case 2: gain0=gain3; break; case 3: gain0=gain4; break;
          }
          audio(); setDisplay(PARAM_IN); break;
      }
    }
  }
#endif // ENABLE_ENCODER

  // ---------- VOLUME ----------
  if (menu == 0 && menu_in == 0 && power == 0) {
#if ENABLE_IR
    if (irValue  == IR_VOLUP)   { vol++; gr1=1; gr2=0; cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
    if (irRepeat && gr1 == 1)  { vol++; gr2=0;        cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
    if (irValue  == IR_VOLDOWN) { vol--; gr1=0; gr2=1; cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
    if (irRepeat && gr2 == 1)  { vol--; gr1=0;        cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
#endif
  }

  // ---------- BASS / TREBLE direct ----------
#if ENABLE_IR
  if (power == 0 && mute == 0) {
    if (irValue == IR_BASUP)    { bas++;  gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
    if (irValue == IR_BASDOWN)  { bas--;  gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
    if (irValue == IR_TREBUP)   { treb++; gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
    if (irValue == IR_TREBDOWN) { treb--; gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
  }
#endif // ENABLE_IR

  // ---------- Save to NVS after 5 s of inactivity ----------
  if (millis()-times > 5000 && w == 1) {
    prefs.putUChar("vol",    (uint8_t)vol);
    prefs.putUChar("in",     in);
    prefs.putUChar("bas",    (uint8_t)(bas  + 7));
    prefs.putUChar("treb",   (uint8_t)(treb + 7));
    prefs.putUChar("ball",   (uint8_t)(ball + 6));
    prefs.putUChar("stereo", (uint8_t)stereo);
    prefs.putUChar("mode",   (uint8_t)mode);
    prefs.putUChar("gain1",  (uint8_t)gain1);
    prefs.putUChar("gain2",  (uint8_t)gain2);
    prefs.putUChar("gain3",  (uint8_t)gain3);
    prefs.putUChar("gain4",  (uint8_t)gain4);
    prefs.putUChar("gain5",  (uint8_t)gain5);
    menu=0; encMode=0; w=0; updateDisplay();
  }
}
