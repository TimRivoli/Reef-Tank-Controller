#include <Wire.h>
#include <Button.h>
#include <avr/wdt.h>
#include <Time.h>
#include <DS1307RTC.h>

//Global Constants
#define EnableDebugging true
#define buttonNotPressed  0
#define buttonUniquePress 1
#define buttonIsPressed   2
#define LEDBitOn 25            //LED intensity to indicate a flash on, max is 255 but that is too bright
#define LEDBitTransition 4     //LED intensity to indicate a transition between bits
#define LEDDigitTransition 4   //LED intensity to indicate a transition between digits
#define LEDGetAttention 150    //LED intensity to indicate start of sequence
#define FlashLEDMaxSequenceLength 50    //Max length of digits for a flash sequence
#define overrideLightsDuration 30       //duration in minutes for light override
#define overridePumpDuration 30         // duration of override in minutes

//Physical channels
#define FlashLEDPin 5             //LED used for flash signaling sequences, pwm channel, only needs to be pwn if we are dimming
#define FlashErrorLEDPin 6        //LED used for flash signaling errors, pwm channel, only needs to be pwn if we are dimming
#define ledBlueChannel     9      //aka led[0] light 0, pwm channel for blue lights
#define ledWhiteChannel    10     //aka led[1] light 1, pwm channel for white lights
#define pumpMainChannel    11     //aka pumps[0], non pwm channel for pump relay
#define pumpCirc1Channel   12     //aka pumps[1], non pwm channel for pump relay
#define pumpCirc2Channel   13     //aka pumps[2], non pwm channel for pump relay

boolean EnableFlashSignaling = false;
boolean EnableStatusDisplayOutput =true;
Button btnDisplay   = Button(4,PULLUP);
Button btnPumps    = Button(7,PULLUP); 
Button btnLights      = Button(8,PULLUP);

//Light variables 
boolean overrideLights = false;
int overrideLightsStartTime = 0;        //minute of override start
int overrideLightsIntensity = 0;        //Current Override light intensity as a percentage
typedef struct {
 int HardwarePin;        // channel pin
 int CurrentIntensity;   // current intensity as a percentage
}
LedsType;
LedsType leds[2];
int ledCount = 2;
unsigned int ledFlashSequence[FlashLEDMaxSequenceLength];  // Array of integers used for flash signaling
unsigned long ledFlashRunStartTime=0;  // Time in milliseconds the run began
unsigned int FlashLEDCurrentSequenceLength = 0;

//Pump Variables
boolean overridePumps = false;
byte overridePumpMode = 0;        // 0 scheduler, 1 On, 2 off
unsigned int overridePumpStartTime = 0;    // minute of override start

typedef struct {
 unsigned int HardwarePin;       //  channel pin
 unsigned int StartTime;         //  time in minutes it started
 unsigned int RunDuration;       //  duration it should run in minutes
 boolean CurrentlyOn;   //  current state
// Button *btn;           //  Button associated with the pump
}
PumpsType;
PumpsType pumps[3];
unsigned int pumpsCount = 3;

//Global variables
bool inLoop = false;  //Just in case, check to make sure we don't loop de loop
bool errorPump = false;
bool errorTemp = false;
bool errorPH = false;
unsigned long currentMillisecond = 0;  //These will be set by loop and used globally
unsigned long lastRun = 0;
unsigned long lastStatusUpdate = 0;
unsigned int currentMinute = 0;  
unsigned int currentTemp = 0;
unsigned int currentPH = 0;
String currentDate = "";
String currentTime = "";

String ZeroFill(int v){
  String r = String(v);
  if (r.length() < 2){r = "0" + r;}
  return r; 
}

void DisplayNewLine(){    
  Serial.println("");
  Serial.flush();
}
void DisplayMessage(String s){    
  Serial.println(currentTime + " - " + s);
  Serial.flush();
}

void DisplayStatus(){
   DisplayNewLine();
   DisplayMessage("Current status at " + String(currentMinute) + " minutes.");
   if (overrideLights) {DisplayMessage("Lights override until " + String(overrideLightsStartTime + overrideLightsDuration));}
   if (overridePumps) {DisplayMessage("Pump override until " + String(overridePumpStartTime + overridePumpDuration));}
   if (!EnableFlashSignaling) {DisplayMessage("LED signaling is off.");}
   DisplayMessage("Pump0: " + String(pumps[0].CurrentlyOn) + " " + String(pumps[0].StartTime) + ":" + String(pumps[0].RunDuration));
   DisplayMessage("Pump1: " + String(pumps[1].CurrentlyOn) + " " + String(pumps[1].StartTime) + ":" + String(pumps[1].RunDuration));
   DisplayMessage("Pump2: " + String(pumps[2].CurrentlyOn) + " " + String(pumps[2].StartTime) + ":" + String(pumps[2].RunDuration));
   DisplayMessage("Light0: " + String(leds[0].CurrentIntensity) + "%");
   DisplayMessage("Light1: " + String(leds[1].CurrentIntensity) + "%");
   DisplayNewLine();
}

//Watchdog setup, the watchdogs is probably overkill since I'm not using interrupts but I like overkill
unsigned long resetTime = 0;
#define TIMEOUTPERIOD 100000             // You can make this time as long as you want,
#define doggieTickle() resetTime = millis();  // This macro will reset the timer
void(* resetFunc) (void) = 0; //declare reset function @ address 0

void watchdogSetup(){
    cli();  // disable all interrupts
    wdt_reset(); // reset the WDT timer
    MCUSR &= ~(1<<WDRF);  // because the data sheet said to
    /*
    WDTCSR configuration:
    WDIE = 1 :Interrupt Enable
    WDE = 1  :Reset Enable - I won't be using this on the 2560
    WDP3 = 0 :For 1000ms Time-out
    WDP2 = 1 :bit pattern is
    WDP1 = 1 :0110  change this for a different
    WDP0 = 0 :timeout period.
    */
    // Enter Watchdog Configuration mode:
    WDTCSR = (1<<WDCE) | (1<<WDE);
    // Set Watchdog settings: interrupte enable, 0110 for timer
    WDTCSR = (1<<WDIE) | (0<<WDP3) | (1<<WDP2) | (1<<WDP1) | (0<<WDP0);
    sei();
}

ISR(WDT_vect){ // Watchdog timer interrupt.
  if(millis() - resetTime > TIMEOUTPERIOD){
    if (EnableDebugging){DisplayMessage("!!Rebooting...");}
    delay(2000);
    resetFunc();                                             // This will call location zero and cause a reboot.
  }
}

/****** Clock Functions ******/
void setDateTimeVars(){
  tmElements_t tm;
  if (RTC.read(tm)) {
    currentMinute = (tm.Minute + (tm.Hour * 60));
    currentDate = ZeroFill(tm.Month) + "\\" + ZeroFill(tm.Day) + "\\" + ZeroFill(tmYearToCalendar(tm.Year));
    currentTime = ZeroFill(tm.Hour) + ":" + ZeroFill(tm.Minute) + ":" + ZeroFill(tm.Second);
  }
}

/****** Flash LED Functions ******/
void ackknowledgeButtonPress(){
   analogWrite(FlashLEDPin, LEDGetAttention);
   delay(250);
   analogWrite(FlashLEDPin, 0);
   if (EnableFlashSignaling){ledFlashRunStartTime=0;}  //Eliminates the delay in finding out what the new settings are after a button press
}
void ackknowledgeHarewareActivation(boolean OnEvent){
   analogWrite(FlashErrorLEDPin, LEDDigitTransition);
   delay(100);
   if (OnEvent){   delay(200); }
   analogWrite(FlashErrorLEDPin, 0);
}

void setFlashSequenceStart(){
   ledFlashRunStartTime = currentMillisecond/1000;
   ledFlashRunStartTime = ledFlashRunStartTime * 1000;  //Round to the nearest second   
}

void runFlashSequence(){
//Each flash will be 1000 milliseconds, 800 data/200 silence
  unsigned long ledFlashRunEndTime = ledFlashRunStartTime + (FlashLEDCurrentSequenceLength * 1000);
  if (currentMillisecond > ledFlashRunEndTime){ledFlashRunStartTime=0;}
  if (ledFlashRunStartTime > 0){
     unsigned long currentRunDuration = (currentMillisecond - ledFlashRunStartTime);
     unsigned int currentRunIndex = (currentRunDuration / 1000);
     if (currentRunIndex < 4){
       analogWrite(FlashLEDPin, LEDGetAttention);
     } else if (ledFlashSequence[currentRunIndex] > LEDBitTransition){
       if ((currentMillisecond % 1000) < 200){
          analogWrite(FlashLEDPin, LEDBitTransition);
       } else {
         analogWrite(FlashLEDPin, ledFlashSequence[currentRunIndex]);
       }
     } else {
       analogWrite(FlashLEDPin, ledFlashSequence[currentRunIndex]);
     }
  }
}

void runFlashErrorSequence(){
//Each flash will be 2000 milliseconds, 1800 data/200 silence
  bool currentStateOn = false;
  unsigned long x  = (currentMillisecond % 2000);
  if (errorPump){
    currentStateOn = (((x / 1000)%2) ==1);
  } else if (errorTemp){
    currentStateOn = (((x / 600)%2) ==1);
  } else if (errorPH){
    currentStateOn = (((x / 400)%2) ==1);
  }
  if (currentStateOn){
      analogWrite(FlashErrorLEDPin, LEDDigitTransition);
  } else {
      analogWrite(FlashErrorLEDPin, LOW);
  } 
}
void setFourDigitFlashSequence(byte n1, byte n2, byte n3, byte n4){  // Sets an array of bits to indicate a flash sequence for the digits 0-9
// Max length is 5+9+2+9+2+9+2+9+1 = 48
  unsigned int currentPosition = 0;
  if(n1>9){n1=9;}
  if(n2>9){n2=9;}
  if(n3>9){n3=9;}
  if(n4>9){n4=9;}
  for (int i = 0; i < 5; i++){//Start with five seconds to flash a start signal and start watching
     ledFlashSequence[currentPosition] = 0;  
     currentPosition += 1;
  }
  for (int i = 0; i < n1; i++){
    ledFlashSequence[currentPosition] = LEDBitOn;
    currentPosition += 1;
  }
  ledFlashSequence[currentPosition] = LEDDigitTransition;
  currentPosition += 1;
  ledFlashSequence[currentPosition] = 0;
  currentPosition += 1;
  for (int i = 0; i < n2; i++){
    ledFlashSequence[currentPosition] = LEDBitOn;
    currentPosition += 1;
  }
  ledFlashSequence[currentPosition] = LEDDigitTransition;
  currentPosition += 1;
  ledFlashSequence[currentPosition] = 0;
  currentPosition += 1;
  for (int i = 0; i < n3; i++){
    ledFlashSequence[currentPosition] = LEDBitOn;
    currentPosition += 1;
  }
  ledFlashSequence[currentPosition] = LEDDigitTransition;
  currentPosition += 1;
  ledFlashSequence[currentPosition] = 0;
  currentPosition += 1;
  for (int i = 0; i < n4; i++){
    ledFlashSequence[currentPosition] = LEDBitOn;
    currentPosition += 1;
  }
  ledFlashSequence[currentPosition] = LEDDigitTransition;
  currentPosition += 1;
  FlashLEDCurrentSequenceLength = currentPosition + 1;
  for (int i = currentPosition + 1; i < FlashLEDMaxSequenceLength; i++){
    ledFlashSequence[i] = 0;
  }
}

void runFlashOverrideSettings() {
  if (ledFlashRunStartTime == 0){
     byte d1 = 1;
     byte d2 = 1;
     byte d3 = 1;
     byte d4 = 1;
     if (overrideLights){
       d1 = (overrideLightsIntensity / 10);
       d2 = (overrideLightsIntensity % 10);
       d3 = ((overrideLightsStartTime + overrideLightsDuration - currentMinute) / 10);
       d4 = ((overrideLightsStartTime + overrideLightsDuration - currentMinute) % 10);
     } else {
       d1 = ((overridePumpStartTime + overridePumpDuration - currentMinute) / 10);
       d2 = ((overridePumpStartTime + overridePumpDuration - currentMinute) % 10);
       d3 = 0;
       d4 = 0;
     }    
     setFourDigitFlashSequence(d1, d2, d3, d4);
     setFlashSequenceStart();
  }
  runFlashSequence();
}

void runFlashTime() {
  if (ledFlashRunStartTime == 0){
     byte d1 = 1;
     byte d2 = 1;
     byte d3 = 1;
     byte d4 = 1;
     tmElements_t tm;
     if (RTC.read(tm)) {
        byte h = tm.Hour;
        if (h > 12){h = h - 12;}
        d1 = h / 10;
        d2 = h % 10;
        d3 = tm.Minute / 10;
        d4 = tm.Minute % 10;
     }
     setFourDigitFlashSequence(d1, d2, d3, d4);
     setFlashSequenceStart();
  } 
  runFlashSequence();
}

/****** Button Functions ******/
byte buttonCheck(Button *button) {
   if (button->uniquePress()) {
     return buttonUniquePress;
   } else if (button->isPressed()) {
     return buttonIsPressed;
   } else {
     return buttonNotPressed;
   }
}

boolean checkButtonAction(Button *button) {
   byte buttonState = buttonCheck(button);
//   return (buttonState == buttonUniquePress || buttonState == buttonIsPressed);
   return (buttonState == buttonUniquePress);
}

/****** Typhon Light Function ******/
//function to set LED brightness according to time of day, function has three phases - ramp up, hold, and ramp down, returns current value
int  getLEDScheduledValue(int mins,    // current time in minutes
            int start,   // start time for this channel of LEDs
            int period,  // photoperiod for this channel of LEDs
            int fade,    // fade duration for this channel of LEDs
            int ledMin,   // min value for this channel as a percentage
            int ledMax)   // max value for this channel as a percentage
  {
   int val = 0;
   //fade up
   if (mins > start || mins <= start + fade)  {
     val = map(mins - start, 0, fade, 0, ledMax);
   }
   //fade down
   if (mins > start + period - fade && mins <= start + period)  {
     val = map(mins - (start + period - fade), 0, fade, ledMax, 0);
   }
   //off or post-midnight run.
   if (mins <= start || mins > start + period)  {
     if((start+period)%1440 < start && (start + period)%1440 > mins )
     {
       val=map((start+period-mins)%1440,0,fade,0,ledMax);
     }
     else  
       val = 0;
   }
   if (val > ledMax) {val = ledMax;}
   if (val < ledMin) {val = ledMin;}
   return val;
}

void activateLight(int channel, int v){
  if (v > leds[channel].CurrentIntensity){
     v = leds[channel].CurrentIntensity + 1;
  } else if (v < leds[channel].CurrentIntensity){
     v = leds[channel].CurrentIntensity - 1;
  }
  if (EnableDebugging && leds[channel].CurrentIntensity != v) {DisplayMessage("Set light " + String(channel) + " to " + String(v) + "%"); }
  leds[channel].CurrentIntensity = v;
  analogWrite(leds[channel].HardwarePin, map(v, 0, 100, 0, 255));
}

void runLights(){
  int lightOneIntensity, lightTwoIntensity;
  if (overrideLights) {
     if (overrideLightsStartTime + overrideLightsDuration >= 1440 || currentMinute >= overrideLightsStartTime + overrideLightsDuration ) {
       overrideLightsStartTime = 0;
       overrideLights = false;
       if (EnableDebugging) {DisplayMessage("Light override has expired."); }
     }
  }
  if (overrideLights) {
    if (overrideLightsIntensity <=50){
       lightOneIntensity = overrideLightsIntensity + 7;
       lightTwoIntensity = overrideLightsIntensity - 5;
    } else {
       lightOneIntensity = overrideLightsIntensity;
       lightTwoIntensity = overrideLightsIntensity;
    }
  } else {
     // Get the scheduled light setting for each light
     if (currentMinute < 405 || currentMinute > 1260 ) {             //before 6:45am or after 9 pm off
       lightOneIntensity = 0;
       lightTwoIntensity = 0;
     } else if (currentMinute < 480 ) {                              // before 8 am, start blue fade up to 90% both
       lightOneIntensity = getLEDScheduledValue(currentMinute, 390, 600, 90, 0, 90); // 6:30 am 90 hr fade in
       lightTwoIntensity = getLEDScheduledValue(currentMinute, 435, 600, 45, 0, 90); // 7:15 am 45 min fade in 
     } else if (currentMinute < 660 ) {                              // 8 am to 11 am, bring both up to 100% 
       lightOneIntensity = getLEDScheduledValue(currentMinute, 480, 600, 15, 90, 100); 
       lightTwoIntensity = getLEDScheduledValue(currentMinute, 480, 600, 15, 90, 100); 
     } else if (currentMinute < 1020 ) {                              // 11 am to 5pm, drop white down to 35%, hold blue
       lightOneIntensity = getLEDScheduledValue(currentMinute, 660, 600, 10, 100, 100);  
       lightTwoIntensity = getLEDScheduledValue(currentMinute, 660, 600, 10, 35, 35);  
     } else {          //1020 - 1260                                  // 5pm to 9pm, ramp up to 90% then slow fade out for the day
       lightOneIntensity = getLEDScheduledValue(currentMinute, 860, 400, 120, 0, 90); //(1140 - 1260 fade) 7 - 9 pm fade out
       lightTwoIntensity = getLEDScheduledValue(currentMinute, 860, 340, 60, 0, 90); //(1140 - 1200 fade) 7 - 8 pm fade out
     }    //net lighting 8 am to 5 pm, 9 hrs full blue, 3 hrs full white, plus another 5 hrs of fading in/out with 90% max
  }
  activateLight(0, lightOneIntensity);
  activateLight(1, lightTwoIntensity);
}

void activatePump(int channel, boolean state){
  if (pumps[channel].CurrentlyOn != state) {
     pumps[channel].CurrentlyOn = state;
     if (state){
       if (EnableDebugging) {DisplayMessage("Turned pump " + String(channel) + " on for " + String(pumps[channel].RunDuration) + " minutes."); }
       digitalWrite(pumps[channel].HardwarePin, HIGH);
       if (EnableFlashSignaling){ackknowledgeHarewareActivation(true);}
     } else {
       if (EnableDebugging) {DisplayMessage("Turned pump " + String(channel) + " off"); }
       digitalWrite(pumps[channel].HardwarePin, LOW);
       if (EnableFlashSignaling){ackknowledgeHarewareActivation(false);}
     }
  }
}

void executeNormalPumpSchedule(){
   if (pumps[0].StartTime == 0) {
      pumps[0].StartTime = currentMinute + 10;
      pumps[0].RunDuration = 120;
   }
   if (pumps[1].StartTime == 0 && pumps[2].StartTime ==0){
      pumps[1].StartTime = currentMinute;
      pumps[1].RunDuration = 30;
      pumps[2].StartTime = currentMinute + 15;
      pumps[2].RunDuration = 30;
   }       
}

void executeFeedingPumpSchedule(){
   pumps[0].StartTime = 0;
   pumps[0].RunDuration = 0;
   if (pumps[1].RunDuration > 5 || pumps[2].RunDuration > 5){
     pumps[1].StartTime = 0;
     pumps[1].RunDuration = 0;
     pumps[2].StartTime = 0;
     pumps[2].RunDuration = 0;
   }
   if (pumps[1].StartTime == 0 && pumps[2].StartTime == 0){
      pumps[1].StartTime = currentMinute;
      pumps[1].RunDuration = 5;
      pumps[2].StartTime = currentMinute + 10;
      pumps[2].RunDuration = 5;
   }  
}

void runPumps(){
//  DisplayMessage("Running Pumps");
  if (overridePumps && (currentMinute >= overridePumpStartTime + overridePumpDuration)) {
    overridePumpStartTime = 0;
    overridePumps = false;
    if (EnableDebugging) {DisplayMessage("Pump override has expired."); }
  }
  for (int i=0; i < pumpsCount; i++){
    if (pumps[i].StartTime > currentMinute){//Safety check, if the pumps aren't schedule to go for over 120 minutes then clear schedule
       if (pumps[i].StartTime - currentMinute > 120 || pumps[i].StartTime > 1440) {  //note since these variables are unsigned 0-10 = a very high int
          pumps[i].StartTime = 0;
          pumps[i].RunDuration = 0;
       }
    }
    if (pumps[i].CurrentlyOn && (pumps[i].StartTime + pumps[i].RunDuration < currentMinute)) {
       if (EnableDebugging) { DisplayMessage("Pump " + String(i) + " schedule has expired.");        }
       pumps[i].StartTime = 0;
       pumps[i].RunDuration = 0;
    }
  }
  if (!overridePumps){                                            //Schedule
    if (currentMinute < 435 || currentMinute >= 1230) {           //before 7:15 am or after 8:30 pm night time run
       executeNormalPumpSchedule();
    } else if (currentMinute >= 500 && currentMinute < 1230) {    //after 8:20 am or before 7:00 pm day time run
       executeNormalPumpSchedule();  
    } else {                                                      //feeding time 7:15am-8:20am and 7pm-8:30PM
       executeFeedingPumpSchedule();
    }
  }
  bool a = false;
  bool b = false;
  for (int i=0; i < pumpsCount; i++){
    a = (pumps[i].StartTime <= currentMinute);
    b = ((pumps[i].RunDuration > 0) && (currentMinute <= (pumps[i].StartTime + pumps[i].RunDuration)));
    activatePump(i, a && b); 
  }
}

void checkForButtonOverrides(){
  if(checkButtonAction(&btnDisplay)){ 
    EnableFlashSignaling = !EnableFlashSignaling;
    ledFlashRunStartTime=0;
    DisplayStatus();  
    ackknowledgeButtonPress();
   }
  if(checkButtonAction(&btnLights)){
    if ((overrideLightsIntensity == 0) && !overrideLights){overrideLightsIntensity = leds[1].CurrentIntensity;}
    overrideLights=true;
    overrideLightsStartTime = currentMinute;
    if (overrideLightsIntensity < 25){
      overrideLightsIntensity = 25;  
    } else if (overrideLightsIntensity < 50) {
      overrideLightsIntensity = 50;
    } else if (overrideLightsIntensity < 75) {
      overrideLightsIntensity = 75;
    } else if (overrideLightsIntensity < 95) {
      overrideLightsIntensity = 95;
    } else {
      overrideLightsIntensity = 0;
    }
    if (EnableDebugging) {DisplayMessage("Light override selected, intensity " + String(overrideLightsIntensity) + "%"); }
    ackknowledgeButtonPress();
  }

 if(checkButtonAction(&btnPumps)){
   if (!overridePumps){
     overridePumpMode = 1;
   } else {
     overridePumpMode += 1;
     if (overridePumpMode > 2){overridePumpMode=0;}
   }
   overridePumps=true;
   overridePumpStartTime = currentMinute;
   if (overridePumpStartTime + overridePumpDuration >= 1440) {overridePumpStartTime = 1;}
   switch (overridePumpMode) {
     case 0:   //return to schedule
       if (EnableDebugging) {DisplayMessage("Pump override selected. Return to scheduler."); }      
       overridePumps=false;
       break;
     case 1:  //On
        if (EnableDebugging) {DisplayMessage("Pump override selected. All on."); }      
        for (int i = 0; i<3; i++){
           pumps[i].StartTime = currentMinute;
           pumps[i].RunDuration = overridePumpDuration;
           activatePump(i, true);
        }
       break;
     case 2:  // Off
        if (EnableDebugging) {DisplayMessage("Pump override selected. All off."); }      
        for (int i = 0; i<3; i++){
           pumps[i].StartTime = currentMinute + overridePumpDuration;
           pumps[i].RunDuration = 5;
           activatePump(i, false);
        }
       break;
   }
   ackknowledgeButtonPress();
 }
}

void setup() {
  if (EnableDebugging) {
    Serial.begin(9600);
    Serial.println("Begin setup");
  }
  pinMode(FlashLEDPin, OUTPUT);
  pinMode(A2, OUTPUT);    // Using A2 and A3 to power RTC
  digitalWrite(A2, HIGH);
  pinMode(A3, OUTPUT);
  digitalWrite(A3, LOW);
  leds[0].HardwarePin = 9;
  leds[0].CurrentIntensity = 0;
  leds[1].HardwarePin = 10;
  leds[1].CurrentIntensity = 0;
  pumps[0].HardwarePin = pumpMainChannel;
  pumps[0].CurrentlyOn = false;
  pumps[0].StartTime = 0;
  pumps[0].RunDuration = 0;
  pumps[1].HardwarePin = pumpCirc1Channel;
  pumps[1].CurrentlyOn = false;
  pumps[1].StartTime = 0;
  pumps[1].RunDuration = 0;
  pumps[2].HardwarePin = pumpCirc2Channel;
  pumps[2].CurrentlyOn = false;
  pumps[2].StartTime = 0;
  pumps[2].RunDuration = 0;
  pinMode(leds[0].HardwarePin, OUTPUT);
  pinMode(leds[1].HardwarePin, OUTPUT);
  for (int i = 0; i < 3; i++) {
     pinMode(pumps[i].HardwarePin, OUTPUT);
     digitalWrite(pumps[i].HardwarePin, LOW);
  }
  watchdogSetup();
  if (EnableDebugging){DisplayMessage("Finished setup");}
  inLoop = false;
}

void loop() {
  if (!inLoop){
    inLoop = true;
    currentMillisecond = millis();
    if (lastRun > currentMillisecond){lastRun = currentMillisecond;}  //We've rolled over
      lastRun = currentMillisecond;
      setDateTimeVars();
      checkForButtonOverrides();
      runLights();
      runPumps();
    if (EnableStatusDisplayOutput) {
       if (lastStatusUpdate > currentMillisecond){lastStatusUpdate = currentMillisecond;}  //We've rolled over
       if ((currentMillisecond - lastStatusUpdate > 600000) || (lastStatusUpdate==0)){  //10 minutes
          lastStatusUpdate = currentMillisecond;
          DisplayStatus();
       }
    }
    if (EnableFlashSignaling) {
       if (errorPH || errorPump || errorTemp) {
         runFlashErrorSequence();
       } else if (overridePumps || overrideLights){
         runFlashOverrideSettings();
       }else {
         runFlashTime();
       }
    }
    doggieTickle();
    delay(100); //Run every 100 milliseconds
    inLoop = false;
  }
}
