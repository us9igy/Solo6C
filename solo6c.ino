// ESP32C6 port of solo6c R2A15908SP audio controller
// Requires (install via Library Manager):
//   - IRremote v3.x+
// Bundled libraries (project folder): R2A15908SP

// --- IR button codes (NEC 32-bit) ---
#define IR_1 0x2FDD02F  // Up
#define IR_2 0x2FD32CD  // Down
#define IR_3 0x2FD906F  // Right (>)
#define IR_4 0x2FDF20D  // Left (<)
#define IR_5 0x2FD708F  // IN
#define IR_6 0x2FD00FF  // POWER
#define IR_7 0x2FD2AD5  // MUTE

// --- IR button codes (new remote, NEC 32-bit) ---
#define IR_NEW_INPUT    0xFB04FE01UL  // Input switch
#define IR_NEW_MUTE     0xFD02FE01UL  // Mute
#define IR_NEW_VOLUP    0xEE11FE01UL  // Vol+
#define IR_NEW_VOLDOWN  0xEF10FE01UL  // Vol-
#define IR_NEW_TREBUP   0xF609FE01UL  // Treble+
#define IR_NEW_TREBDOWN 0xF20DFE01UL  // Treble-
#define IR_NEW_BASUP    0xF50AFE01UL  // Bass+
#define IR_NEW_BASDOWN  0xF10EFE01UL  // Bass-

// --- ESP32C6 GPIO assignments (adjust to your wiring) ---
#define ENC_CLK      20   // Encoder CLK (A) — S1
#define ENC_DT       19   // Encoder DT  (B) — S2
#define ENC_BTN      18   // Encoder pushbutton (active LOW) — SW
#define IR_RECV_PIN  14   // IR receiver signal pin
#define STANDBY_PIN  21   // Standby relay/output
#define I2C_SDA      22   // I2C SDA — R2A15908SP
#define I2C_SCL      23   // I2C SCL — R2A15908SP

// ── Boot isolation — flip 0→1 one by one to find crash ───────────────────────
#define ENABLE_ENCODER 1
#define ENABLE_IR      1
#define ENABLE_SEG7    1
#define ENABLE_AUDIO   1

#include <Wire.h>
#include <Preferences.h>
#include <R2A15908SP.h>
#define DECODE_NEC
#include <IRremote.hpp>   // IRremote v3+;
#include <Ticker.h>

R2A15908SP        ra;
Preferences       prefs;
volatile int _encAcc = 0;
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
void gain_func() { if (gain0 > 10) gain0 = 10; if (gain0 < 0) gain0 = 0; }
void vol_func()  { if (vol > 87) vol = 87; if (vol < 12) vol = 12; }

void audio() {
  ra.setVolume_left(vol - 6 + ball);
  ra.setVolume_right(vol - 6 - ball);
  ra.setIn_Gain(in, gain0);
  ra.setStereo(stereo, mode);
  ra.setTone(bas, treb);
}

void updateDisplay() {
  if (power == 1) { seg7Set(0x00, 0x00); return; }
  if (mute  == 1) { seg7Set(0x40, 0x40); return; }  // '--'

  bool fallback = (displayParam == PARAM_VOL) || (millis() - displayTime >= 3000);
  if (fallback) {
    int v = constrain(vol - 12, 0, 99);
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
      seg7Set(seg7Encode('A'), seg7Encode('1' + in));
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
        case 2: seg7Set(seg7Encode('S'), seg7Encode('H')); break;
        case 3: seg7Set(seg7Encode('S'), seg7Encode('L')); break;
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

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println(F("[3] Wire OK"));

#if ENABLE_IR
  IrReceiver.begin(IR_RECV_PIN, DISABLE_LED_FEEDBACK);
  Serial.println(F("[4] IR OK"));
#else
  Serial.println(F("[4] IR skipped"));
#endif

  prefs.begin("solo6c", false);
  vol    = prefs.getUChar("vol",    50);
  in     = prefs.getUChar("in",      0);
  bas    = (int)prefs.getUChar("bas",   7) - 7;
  treb   = (int)prefs.getUChar("treb",  7) - 7;
  ball   = (int)prefs.getUChar("ball",  6) - 6;
  stereo = prefs.getUChar("stereo",  0);
  mode   = prefs.getUChar("mode",    0);
  gain1  = prefs.getUChar("gain1",   0);
  gain2  = prefs.getUChar("gain2",   0);
  gain3  = prefs.getUChar("gain3",   0);
  gain4  = prefs.getUChar("gain4",   0);
  gain5  = prefs.getUChar("gain5",   0);
  Serial.println(F("[5] NVS OK"));

  switch (in) {
    case 0: gain0 = gain1; break;
    case 1: gain0 = gain2; break;
    case 2: gain0 = gain3; break;
    case 3: gain0 = gain4; break;
    case 4: gain0 = gain5; break;
  }
#if ENABLE_AUDIO
  audio();
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

  // ---------- power-on IR menu navigation ----------
  if (power == 0) {
    if (irValue == IR_2 && mute == 0) { menu++; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; if (menu > 5) menu = 0; }
    if (irValue == IR_1 && mute == 0) { menu--; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; if (menu < 0) menu = 5; }
    if (irValue == IR_5 || irValue == IR_NEW_INPUT) { menu=0; in++; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; if (in > 4) in = 0; audio(); setDisplay(PARAM_IN); }
    if ((irValue == IR_7 || irValue == IR_NEW_MUTE) && mute == 0) { mute=1; menu=100; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; in_old=in; in=7; audio(); updateDisplay(); delay(300); }
    if ((irValue == IR_7 || irValue == IR_NEW_MUTE) && mute == 1) { mute=0; menu=0; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; in=in_old; audio(); updateDisplay(); delay(300); }
  }
  if (irValue == IR_6 && power == 0) { power=1; menu=100; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; in_old=in; in=7; audio(); updateDisplay(); delay(3000); }
  if (irValue == IR_6 && power == 1) { power=0; menu=0; gr1=0; gr2=0; cl2(); times=millis(); w=1; w2=1; in=in_old; audio(); updateDisplay(); delay(1000); }
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
    } else {
      switch (menu) {
        case 1: bas    += delta; bass_func(); audio(); setDisplay(PARAM_BAS);  break;
        case 2: treb   += delta; treb_func(); audio(); setDisplay(PARAM_TREB); break;
        case 3: ball   += delta; ball_func(); audio(); setDisplay(PARAM_BALL); break;
        case 4: stereo  = ((stereo + delta) % 3 + 3) % 3; audio(); setDisplay(PARAM_ST);   break;
        case 5: mode    = ((mode   + delta) % 4 + 4) % 4; audio(); setDisplay(PARAM_MODE); break;
        case 6:
          in = ((in + delta) % 5 + 5) % 5;
          switch (in) {
            case 0: gain0=gain1; break; case 1: gain0=gain2; break;
            case 2: gain0=gain3; break; case 3: gain0=gain4; break; case 4: gain0=gain5; break;
          }
          audio(); setDisplay(PARAM_IN); break;
      }
    }
  }
#endif // ENABLE_ENCODER

  // ---------- VOLUME ----------
  if (menu == 0 && menu_in == 0 && power == 0) {
#if ENABLE_IR
    if (irValue  == IR_3 || irValue == IR_NEW_VOLUP)   { vol++; gr1=1; gr2=0; cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
    if (irRepeat && gr1 == 1)                          { vol++; gr2=0;        cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
    if (irValue  == IR_4 || irValue == IR_NEW_VOLDOWN) { vol--; gr1=0; gr2=1; cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
    if (irRepeat && gr2 == 1)                          { vol--; gr1=0;        cl(); times=millis(); w=1; w2=1; vol_func(); audio(); setDisplay(PARAM_VOL); }
#endif
  }

  // ---------- BASS ----------
  if (menu == 1) {
#if ENABLE_IR
    if (irValue  == IR_3)     { bas++; gr1=1; gr2=0; cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
    if (irRepeat && gr1 == 1) { bas++; gr2=0;        cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
    if (irValue  == IR_4)     { bas--; gr1=0; gr2=1; cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
    if (irRepeat && gr2 == 1) { bas--; gr1=0;        cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
#endif
  }

  // ---------- TREBLE ----------
  if (menu == 2) {
#if ENABLE_IR
    if (irValue  == IR_3)     { treb++; gr1=1; gr2=0; cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
    if (irRepeat && gr1 == 1) { treb++; gr2=0;        cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
    if (irValue  == IR_4)     { treb--; gr1=0; gr2=1; cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
    if (irRepeat && gr2 == 1) { treb--; gr1=0;        cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
#endif
  }

  // ---------- BALANCE ----------
  if (menu == 3) {
#if ENABLE_IR
    if (irValue  == IR_3)     { ball++; gr1=1; gr2=0; cl3(); times=millis(); w=1; w2=1; ball_func(); audio(); setDisplay(PARAM_BALL); }
    if (irRepeat && gr1 == 1) { ball++; gr2=0;        cl3(); times=millis(); w=1; w2=1; ball_func(); audio(); setDisplay(PARAM_BALL); }
    if (irValue  == IR_4)     { ball--; gr1=0; gr2=1; cl3(); times=millis(); w=1; w2=1; ball_func(); audio(); setDisplay(PARAM_BALL); }
    if (irRepeat && gr2 == 1) { ball--; gr1=0;        cl3(); times=millis(); w=1; w2=1; ball_func(); audio(); setDisplay(PARAM_BALL); }
#endif
  }

  // ---------- STEREO / MONO ----------
  if (menu == 4) {
#if ENABLE_IR
    if (irValue == IR_3) { stereo++; gr1=1; gr2=0; cl3(); times=millis(); w=1; w2=1; if (stereo > 2) stereo=0; audio(); setDisplay(PARAM_ST); }
    if (irValue == IR_4) { stereo--; gr1=0; gr2=1; cl3(); times=millis(); w=1; w2=1; if (stereo < 0) stereo=2; audio(); setDisplay(PARAM_ST); }
#endif
  }

  // ---------- MODE SELECTOR ----------
  if (menu == 5) {
#if ENABLE_IR
    if (irValue == IR_3) { mode++; gr1=1; gr2=0; cl3(); times=millis(); w=1; w2=1; if (mode > 3) mode=0; audio(); setDisplay(PARAM_MODE); }
    if (irValue == IR_4) { mode--; gr1=0; gr2=1; cl3(); times=millis(); w=1; w2=1; if (mode < 0) mode=3; audio(); setDisplay(PARAM_MODE); }
#endif
  }

  // ---------- BASS direct (new remote — works in any menu) ----------
#if ENABLE_IR
  if (power == 0 && mute == 0) {
    if (irValue == IR_NEW_BASUP)   { bas++; gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
    if (irValue == IR_NEW_BASDOWN) { bas--; gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; bass_func(); audio(); setDisplay(PARAM_BAS); }
  }
  // ---------- TREBLE direct (new remote — works in any menu) ----------
  if (power == 0 && mute == 0) {
    if (irValue == IR_NEW_TREBUP)   { treb++; gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
    if (irValue == IR_NEW_TREBDOWN) { treb--; gr1=0; gr2=0; cl3(); times=millis(); w=1; w2=1; treb_func(); audio(); setDisplay(PARAM_TREB); }
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
