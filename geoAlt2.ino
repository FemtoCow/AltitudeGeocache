/*

  Altitude Geocache
  
  
  
  Programming:
    Select board: Arduino Pro Mini 8Mhz
    fuses: don't play with it :) but if you do this must be set:
    BOD must be disabled, when the device is sleeping, voltage regulator is turned off and 
    the device is powered directly from batteries which can go low as 1V
    Internal oscilator must be used, not external

*/


#define DEG "\xDF"

//////////////////// change your setting here ////////////////////

#define USE_IMPERIAL 1 //1 for imperial + metric screen, 0 for metric only
#define SHOW_SYSTEM 1  //1 to show debug screens, 0 for deployment
#define RESET_TPT 0

#define MSGN "S 12" DEG "34.567'" //win message line1
#define MSGW "E 89" DEG "01.123'" //win message line2
#define TIME_TO_COMPLETE ((long)1000 * 60 * 15) //time to complete in milliseconds
#define DM_TO_GO ((int16_t)10 * 10) //goal altitude difference in decimeters

#define MAX_ACCEL ((100*DM_TO_GO) / (TIME_TO_COMPLETE / 1000 / 10)) //maximum accelaration in mm/s to prevent cheating
#define MIN_TO_COMPLETE (TIME_TO_COMPLETE / 60000) //time to complete in minutes to display in into

//////////////////////////////////////////////////////////////////



#include <BatteryStatusIcon.h>   //https://github.com/cano64/BatteryStatusIcon
#include <DigitalButtons.h>      //https://github.com/cano64/ArduinoButtons
#include <Wire.h>
#include <BMP085.h>              //https://github.com/cano64/Arduino-BMP085-Library-no-pow
#include <LiquidCrystal.h>
#include <FancyPrint.h>          //https://github.com/cano64/FancyPrint
#include <AutoPWM.h>             //https://github.com/cano64/AutoPWM
#include <GeocachingLogo.h>      //https://github.com/cano64/GeocachingLogo
#include <SystemStatus.h>        //https://github.com/cano64/ArduinoSystemStatus
#include <avr/sleep.h>


#define PROTO 0

#if PROTO //for boards Rev.0

// LCD PINS
#define PIN_RS 6
#define PIN_E 7
#define PIN_D4 10
#define PIN_D5 11
#define PIN_D6 12
#define PIN_D7 13
#define PIN_BL 9
#define PIN_CONTRAST 3
// other pins
#define PIN_POWER_ENABLE 0
#define PIN_POWER_GND A3
#define PIN_BTN 2
#define INT_BTN 0
#define PIN_BATT_SENSE 7

#else //for boards Rev.1, Rev.2

// LCD PINS
#define PIN_RS A3
#define PIN_E A2
#define PIN_D4 A1
#define PIN_D5 A0
#define PIN_D6 5
#define PIN_D7 6
#define PIN_BL 9
#define PIN_CONTRAST 10
// other pins
#define PIN_POWER_ENABLE 4
#define PIN_POWER_GND 3
#define PIN_BTN 2
#define INT_BTN 0
#define PIN_BATT_SENSE 7

#endif


#define MSG_SE0 "  Sensor Error  "
#define MSG_SE1 "   Contact CO   "

#define MSG_TIMESUP0 "   Time's up    "
#define MSG_TIMESUP1 "   Game Over    "

#define MSG_CHEAT0 "Cheating Detectd"
#define MSG_CHEAT1 "   Game Over    "

#define MSG_ZZZ0 "                "
#define MSG_ZZZ1 "             Zzz"


#define VS_LOGO 0

#define VS_INTRO3 12
#define VS_IMPERIAL 20
#define VS_IMPERIAL2 21
#define VS_METRIC 30
#define VS_METRIC2 31
#define VS_SYSTEM1 40
#define VS_SYSTEM2 41
#define VS_SYSTEM3 42
#define VS_SERROR 50
#define VS_FAIL 60
#define VS_RESTART 61
#define VS_WIN 80
#define VS_ZZZ 90

#define TIME_MENU_IDLE 60 * 1000

//macro to convert decimeters to tens of feet
#define DM2FT10(dm) (((dm << 1) + (dm >> 0) + (dm >> 2) + (dm >> 5))) // dm * 3.2808; 11.01000111111..._2


BMP085 bmp;
LiquidCrystal lcd(PIN_RS, PIN_E, PIN_D4, PIN_D5, PIN_D6, PIN_D7);
DigitalButtons btn(PIN_BTN);
BatteryStatusIcon bsi(&lcd, 0, 1500, 3000);
AutoPWM contrast(PIN_CONTRAST, 2200, 2800, 0, 70); //based on VCC
AutoPWM backlight(PIN_BL, 1500, 2300, 0, 96); //based on VBatt
FancyPrint fp(&lcd);
GeocachingLogo glogo(&lcd, 0);
SystemStatus ss(PIN_BATT_SENSE);

unsigned int vcc = 3300; //main voltage, should be 3300mV
unsigned int vBatt = 3000; //battery voltage, should be 3000mV when full

int16_t lalt = 0; //lowest altitude found so far
int16_t malt = 0; //highest altitude found
unsigned long  ltime = 0; //time in s when lowest altitude was found
unsigned long htime = 0; //time in s when highest altitude was found
unsigned long timeGameStart = 0;
unsigned long timeLastViewChange = 0;
uint8_t upCycler = 0;
uint8_t viewState = VS_LOGO;

#define ABSHIFT 4
#define ABSIZE (1 << ABSHIFT)
int16_t altBuff[ABSIZE]; //altitude (dm) smoothening buffer
uint8_t altCursor = 0; //altitude buffer cursor
long altSum = 0; //sum of the buffer
int16_t salt = 0; //starting altitude in dm
int16_t alt = 0; //current altitude in dm

void setup() {
  btn.onKeyDown = onKeyDown;
  btn.onKeyLongDown = onKeyLongDown;
  gameInit();
}


void loop() {
  btn.update();
  
  if (viewState == VS_ZZZ) { //we just received ZZZ command
    goZzz();
  }
  if (viewState >= VS_LOGO && viewState <= VS_INTRO3 && (millis() - timeLastViewChange > TIME_MENU_IDLE)) { //stuck on initial menu for too long
    goZzz();
  }
  if (getTimeLeftSec() < 0) { //time is up, game over
    disp(MSG_TIMESUP0, MSG_TIMESUP1);
    delay(3000);
    goZzz();
  }  
  if (viewState == VS_SERROR) { //sensor error, game over
    disp(MSG_SE0, MSG_SE1);
    delay(3000);
    goZzz();
  }  
/*
  if (getAcceleration() > MAX_ACCEL) { //cheating detected, game over
    disp(MSG_CHEAT0, MSG_CHEAT1);
    delay(3000);
    goZzz();
  }
*/  

  alt = updateAltitude();

  //certain values don't need to be updated every frame
  //this will increase button responsivness
  switch (upCycler++ % 5) { 
    case 0:
      break;
    case 1: 
      vcc = ss.getVCC();
      vBatt = ss.getVBatt(vcc);
      break;
    case 2:
      contrast.update(vcc);
      backlight.update(vBatt);
      break;
    case 3:
      bsi.update(vBatt);
      //bsi.setLevel(((uint8_t)alt) % 6);
      break;
    case 4:
      if (alt < lalt) {
           lalt = alt;
           ltime = millis();   
      } else if (alt > malt) {
          malt = alt;
          htime = millis();
      }    
      break;
  }
  
  updateScreen();
}


void goZzz() {
  backlight.off();
  disp(MSG_ZZZ0, MSG_ZZZ1);
  //wait until no button is pressed
  while (btn.getState() != 0);
  delay(100);
  
  powerOff();
  ss.SleepWakeOnInterrupt(0);
  //wake up here
  gameInit();
}

//initialize the device and game after power up or wake up
void gameInit() {
  powerOn();

  timeGameStart = millis(); //has to be initialized before dispSplashScreen

  vcc = ss.getVCC();
  vBatt = ss.getVBatt(vcc);
  contrast.autoPrescaler();
  contrast.update(vcc, true);
  backlight.autoPrescaler();
  backlight.update(vBatt, true);
  delay(50);
  lcd.begin(16, 2);
  viewState = VS_LOGO;
  dispSplashScreen();

  if (!bmp.begin()) {
    viewState = VS_SERROR;
  }
  alt = initAltitude();
  salt = alt - 5; //set starting altitude lower so we don't have to deal with negative numbers at the beginning due to noise

  lalt = alt;
  malt = alt;
  
  ltime = timeGameStart - 5000;
  htime = timeGameStart + 5000;
  timeLastViewChange = timeGameStart;

  //digitalWrite(PIN_BL, 1);
}

void updateScreen() {
  
  if (isSolved()) {
    dispWin();
  } else {
    switch (viewState) {
      case VS_LOGO:
        dispSplashScreen();
        break;
//      case VS_INTRO1:
//        break;
//      case VS_INTRO2:
//        break;
      case VS_INTRO3:
        //  #define MSG30 " Get me higher  "
        //  #define MSG31 " in 30 minutes  "
      
        lcd.setCursor(0, 0);
        lcd.print(" Get me higher  ");
        lcd.setCursor(0, 1);
        lcd.print(" in ");
        lcd.print(MIN_TO_COMPLETE);
        lcd.print(" minutes  ");
        break;
      case VS_IMPERIAL:
        //sample output
        //  "TO GO: 150ft   #"
        //  " GONE:   6ft    "
        //  "TO GO:150ft    #"
        //  " GONE:  6ft 9:55"
        {
          int16_t togo = DM2FT10(getDMtogo());
          int16_t gone = DM2FT10(getDMgone());
          int16_t timeLeft = getTimeLeftSec();
          
          lcd.setCursor(0, 0);
          lcd.print("TO GO:");
          if (timeLeft > 600) lcd.print(" ");
          fp.print(togo, 1, 3, 0);
          lcd.print("ft   ");
          if (!(timeLeft > 600)) lcd.print(" ");
          bsi.draw(15, 0);
          //second line
          lcd.setCursor(0, 1);
          lcd.print(" GONE:");
          if (timeLeft > 600) lcd.print(" ");
          fp.print(gone, 1, 3, 0);
          lcd.print("ft ");
          if (timeLeft > 600) {
            lcd.print("   ");
          } else {
            lcd.setCursor(12, 1);
            fp.printMS(timeLeft, 1, ' ');
          }
        }
        break;
      case VS_IMPERIAL2:
        //sample output
        //  "TO GO: 150ft   #"
        //  "Time Left: 30:00"
        {
          int16_t togo = DM2FT10(getDMtogo());
          int16_t timeLeft = getTimeLeftSec();
          
          lcd.setCursor(0, 0);
          lcd.print("TO GO:");
          if (timeLeft > 600) lcd.print(" ");
          fp.print(togo, 1, 3, 0);
          lcd.print("ft   ");
          if (!(timeLeft > 600)) lcd.print(" ");
          bsi.draw(15, 0);
          //second line
          lcd.setCursor(0, 1);
          lcd.print("Time Left: ");
          fp.printMS(timeLeft, 2, ' ');
        }
        break;
      case VS_METRIC:
        //sample output
        //  "To Go: 59.9m   #"
        //  " Gone:  3.5m    "
        //  "To Go:59.9m    #"
        //  " Gone: 3.5m 5:31"
        {
          int16_t togo = getDMtogo();
          int16_t gone = getDMgone();
          int16_t timeLeft = getTimeLeftSec();
          
          lcd.setCursor(0, 0);
          lcd.print("TO GO:");
          if (timeLeft > 600) lcd.print(" ");
          fp.print(togo, 1, 2, 1);
          lcd.print("m   ");
          if (!(timeLeft > 600)) lcd.print(" ");
          bsi.draw(15, 0);
          //second line
          lcd.setCursor(0, 1);
          lcd.print(" GONE:");
          if (timeLeft > 600) lcd.print(" ");
          fp.print(gone, 1, 2, 1);
          lcd.print("m ");
          if (timeLeft > 600) {
            lcd.print("   ");
          } else {
            lcd.setCursor(12, 1);
            fp.printMS(timeLeft, 1, ' ');
          }
        }
        break;
      case VS_METRIC2:
        //sample output
        //  "To Go: 59.9m   #"
        //  "Time Left:  0:00"
        {
          int16_t togo = getDMtogo();
          int16_t timeLeft = getTimeLeftSec();
          
          lcd.setCursor(0, 0);
          lcd.print("TO GO:");
          if (timeLeft > 600) lcd.print(" ");
          fp.print(togo, 1, 2, 1);
          lcd.print("m   ");
          if (!(timeLeft > 600)) lcd.print(" ");
          bsi.draw(15, 0);
          //second line
          lcd.setCursor(0, 1);
          lcd.print("Time Left: ");
          fp.printMS(timeLeft, 2, ' ');
        }
        break;
      case VS_SYSTEM1:
        //sample output
        //  "Batt: 2708mV 99%"
        //  "VCC: 3300mV C128"
        
        //  "99% Batt: 2708mV"
        //  "b99c99  VCC:3300"
        {
          
          lcd.setCursor(0, 0);
          fp.print(bsi.percent, 0, 2, 0);
          lcd.print("% ");      
          lcd.print("Batt: ");
          fp.print(vBatt, 0, 4, 0);
          lcd.print("mV ");
          //second line
          lcd.setCursor(0, 1);
          lcd.print("b");
          lcd.print(backlight.pwm);
          lcd.print("c");
          lcd.print(contrast.pwm);
          lcd.print(" ");
          lcd.setCursor(8, 1);
          lcd.print("VCC:");
          fp.print(vcc, 0, 4, 0);
          lcd.print(" ");
        }        
  
        //disp(MSG60, MSG61);
        break;
      case VS_SYSTEM2:
        //sample output
        //  "101325Pa 25.54*C"
        //  "A:103.5m a:32mm/s"
        {
          long pressure = bmp.readPressure();
          int C100 = bmp.readTemperature100C();
          long altMM = getAltitudeMM();
          int16_t acc = getAcceleration();
          lcd.setCursor(0, 0);
          fp.print(pressure, 0, 6, 0);
          lcd.print("Pa ");
          fp.print(C100, 2, 2, 2);
          lcd.print(DEG "C");      
          //second line
          lcd.setCursor(0, 1);
          fp.print(altMM, 3, 3, 3);
          lcd.print("m A:");
          fp.print(acc, 0, 3, 0);
          lcd.print("    ");
        } 
        break;
      case VS_SYSTEM3:
        //sample output    
        //  " 1MHz 1024B Free"
        //  "TPT: 24:55:44   "
        {      
          int freeRAM = ss.getFreeRAM();
          int8_t intTemp = ss.getTemperatureInternal();
          byte mhz = ss.getMHz();
          lcd.setCursor(0, 0);
          fp.print(mhz, 0, 2, 0);
          lcd.print("MHz ");
          fp.print(freeRAM, 0, 4, 0);
          lcd.print("B Free ");      
          //second line
          lcd.setCursor(0, 1);
          lcd.print(" Int temp: ");
          fp.print(intTemp, 0, 2, 0);
          lcd.print(DEG "C  ");
          
        }
        //dispWin();
        break;
    } //switch
  } //if
}

void onKeyDown(uint8_t key) {
  switch (viewState) {
    case VS_LOGO:
      viewState = VS_INTRO3;
      break;
//    case VS_INTRO1:
//      viewState = VS_INTRO2;
//      break;
//    case VS_INTRO2:
//      viewState = VS_INTRO3;
//      break;
    case VS_INTRO3:
      viewState = VS_IMPERIAL;
      break;
    case VS_IMPERIAL:
      viewState = VS_IMPERIAL2;
      break;
    case VS_IMPERIAL2:
      viewState = VS_METRIC;
      break;
    case VS_METRIC:
      viewState = VS_METRIC2;
      break;
    case VS_METRIC2:
      viewState = VS_SYSTEM1;
      break;
    case VS_SYSTEM1:
      viewState = VS_SYSTEM2;
      break;
    case VS_SYSTEM2:
      viewState = VS_SYSTEM3;
      break;
    case VS_SYSTEM3:
      viewState = VS_IMPERIAL;
      break;
  } //switch
  
 
  if (viewState == VS_SYSTEM1 && !SHOW_SYSTEM) viewState = VS_IMPERIAL; //switch to user view if system menu is disabled
  if (viewState == VS_IMPERIAL && !USE_IMPERIAL) viewState = VS_METRIC; //switch to metric view if imperial is disabled
  //delay(100);
  timeLastViewChange = millis();
}

void onKeyLongDown(uint8_t key) {
  viewState = VS_ZZZ;
}


int16_t getTimeLeftSec() {
    int16_t secLeft = (int32_t)(timeGameStart + TIME_TO_COMPLETE - millis()) / 1000;
    return secLeft;
}


int16_t getDMtogo() {
    return salt + DM_TO_GO - alt;
}


int16_t getDMgone() {
    return alt - salt;
}


//get acceleration between min and max altitude in mm/s
int16_t getAcceleration() {
    if (ltime == htime) return 0;
    long dtime = (ltime > htime) ? ltime - htime : htime - ltime;
    dtime = dtime / 1000; //convert ms to s
    //return malt;
    //return malt - lalt;
    return (long)100*((long)malt - (long)lalt) / dtime;
}


int16_t initAltitude() {
  altCursor = 0;
  altSum = 0;
  for (byte i = 0; i < ABSIZE; i++) {
    altBuff[i] = bmp.readAltitudeSTDdm();
    altSum += altBuff[i];
  }
  return altSum >> ABSHIFT;  
}


int16_t updateAltitude() {
  altSum -= altBuff[altCursor];
  altBuff[altCursor] = bmp.readAltitudeSTDdm();
  altSum += altBuff[altCursor];
  altCursor = (altCursor + 1) % ABSIZE;
  return altSum >> ABSHIFT;  
}


long getAltitudeMM() {
  return (100*altSum) >> ABSHIFT;  
}


void disp(const char str1[], const char str2[]) {
    lcd.setCursor(0, 0);
    lcd.print(str1);
    lcd.setCursor(0, 1);
    lcd.print(str2);
}


uint8_t isSolved() {
   return getDMtogo() <= 0; 
}

//enables the boost converters and attached peripherals
void powerOn() {
  if (PROTO) {
    digitalWrite(PIN_POWER_GND, LOW);
    pinMode(PIN_POWER_GND, OUTPUT);
  }
  digitalWrite(PIN_POWER_ENABLE, HIGH);
  pinMode(PIN_POWER_ENABLE, OUTPUT);
}

void powerOff() {
  digitalWrite(PIN_POWER_ENABLE, LOW);
  pinMode(PIN_POWER_ENABLE, OUTPUT);
  
  
}

void dispWin() {
  //lcd.clear();
  glogo.draw(0, 0);
  lcd.setCursor(4, 0);
  lcd.print(MSGN);
  lcd.setCursor(4, 1);
  lcd.print(MSGW);  
}

void dispSplashScreen() {
  //lcd.clear();
  prepareFontArrow(0);
  glogo.draw(0, 0);
  lcd.setCursor(4, 0);
  lcd.print("Caching with");
  lcd.setCursor(4, 1);
  lcd.print("  Altitude ");
  //blink the arrow after some time passes
  if (millis() - timeGameStart > 5000) {
    if ((millis() / 1000) %2) {
      lcd.print(" ");
    } else {
      lcd.write((uint8_t)0);
    }
  } else {
    lcd.print(" ");
  }
}

void prepareFontArrow(uint8_t c) {
  uint8_t L0[8] = {
    0b00000,
    0b10000,
    0b11100,
    0b11111,
    0b11100,
    0b10000,
    0b00000,
    0b00000
  };
  lcd.createChar(c, L0);
}


//end
