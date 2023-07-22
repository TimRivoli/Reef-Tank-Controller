#include <Wire.h>
#include <Button.h>
#include <avr/wdt.h>
#include <Time.h>
// #include <Timezone.h>
#include <DS1307RTC.h>

//Time schedules
#define PumpModeNormal 0
#define PumpModeFeeding 1
#define PumpModeQuietTime 2

typedef struct {
 unsigned int StartMinute;
 unsigned int EndMinute;
 unsigned int PumpModeID; //pump function to run
}
ScheduleType;
#define sumpScheduleLength 2
ScheduleType sumpLightSchedule[sumpScheduleLength];
#define pumpScheduleLength 4
ScheduleType pumpSchedule[pumpScheduleLength];

//Global Constants
#define EnableDebugging true
#define buttonNotPressed  0
#define buttonUniquePress 1
#define buttonIsPressed   2
#define LEDBitOn 20            //LED intensity to indicate a flash on, max is 255 but that is too bright
#define LEDBitTransition 3     //LED intensity to indicate a transition between bits
#define LEDDigitTransition 3   //LED intensity to indicate a transition between digits
#define LEDGetAttention 100    //LED intensity to indicate start of sequence
#define FlashLEDMaxSequenceLength 50    //Max length of digits for a flash sequence
#define overridePumpDuration 30         // duration of override in minutes

//Physical channels
#define FlashLEDPin 5             //LED used for flash signaling sequences, pwm channel, only needs to be pwn if we are dimming
#define FlashErrorLEDPin 6        //LED used for flash signaling errors, pwm channel, only needs to be pwn if we are dimming
#define pumpMainChannel    11     //aka relays[0], non pwm channel for pump relay
#define pumpCirc1Channel   12     //aka relays[1], non pwm channel for pump relay
#define pumpCirc2Channel   13     //aka relays[2], non pwm channel for pump relay
#define sumpLightChannel   8     //aka relays[3], non pwm channel for sump light relay

boolean EnableFlashSignaling = false;
boolean EnableStatusDisplayOutput =true;
Button btnDisplay   = Button(4,PULLUP);
Button btnrelays    = Button(7,PULLUP); 
//Button btnLights      = Button(2,PULLUP);

//Pump Variables
boolean overridePumpSchedule = false;
byte overridePumpMode = 0;        // 0 Scheduler, 1 feeding, 2 On, 3 off
unsigned int overridePumpScheduleStartTime = 0;    // minute of override start
unsigned int currentPumpSchedule = 0;  //daytime=0, nighttime=1, quiet=2, feeding=3, manual=4

typedef struct {
 unsigned int HardwarePin;       //  channel pin
 unsigned int StartTime;         //  time in minutes it started
 unsigned int RunDuration;       //  duration it should run in minutes
 boolean CurrentlyOn;     //  current state
// Button *btn;           //  Button associated with the pump
}
relaysType;
#define relaysCount 4
relaysType relays[relaysCount];

//Global dynamic variables
#define bedTimeStart 6000;  //loop iterations until we begin a slow sleep mode
unsigned int bedTimeCounter = 0;  //counter for starting slow mode
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

unsigned int ledFlashSequence[FlashLEDMaxSequenceLength];  // Array of integers used for flash signaling
unsigned long ledFlashRunStartTime=0;  // Time in milliseconds the run began
unsigned int FlashLEDCurrentSequenceLength = 0;

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
   if (overridePumpSchedule) {DisplayMessage("Pump override until " + String(overridePumpScheduleStartTime + overridePumpDuration));}
   if (!EnableFlashSignaling) {DisplayMessage("LED signaling is off.");}
   DisplayMessage("Pump0: " + String(relays[0].CurrentlyOn) + " " + String(relays[0].StartTime) + ":" + String(relays[0].RunDuration));
   DisplayMessage("Pump1: " + String(relays[1].CurrentlyOn) + " " + String(relays[1].StartTime) + ":" + String(relays[1].RunDuration));
   DisplayMessage("Pump2: " + String(relays[2].CurrentlyOn) + " " + String(relays[2].StartTime) + ":" + String(relays[2].RunDuration));
   DisplayMessage("Sump Light: " + String(relays[3].CurrentlyOn) + " " + String(relays[3].StartTime) + ":" + String(relays[3].RunDuration));
   if (currentPumpSchedule == 0) {
      DisplayMessage("Schedule mode: Day time.");
   } else if (currentPumpSchedule == 1) {
      DisplayMessage("Schedule mode: Night time.");
   } else if (currentPumpSchedule == 2) {
      DisplayMessage("Schedule mode: Quiet time.");
   } else if (currentPumpSchedule == 3) {
      DisplayMessage("Schedule mode: Feeding time.");
   } else {
      DisplayMessage("Schedule mode: Manual.");
   }
   DisplayNewLine();
}

//Watchdog setup, the watchdogs is probably overkill since I'm not using interrupts but I like overkill
unsigned long resetTime = 0;
#define TIMEOUTPERIOD 100000             // You can make this time as long as you want,
#define doggieTickle() resetTime = millis();  // This macro will reset the timer
void(* resetFunc) (void) = 0; //declare reset function @ address 0
void kickDoggie(){  bedTimeCounter = bedTimeStart; }
  
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
//    if ((tm.Month > 2) && (tm.Month < 11) && (currentMinute > 60)){  /**** rough adjustment for DST ***/
//      currentMinute += 60;
//      currentTime = ZeroFill(tm.Hour+1) + ":" + ZeroFill(tm.Minute) + ":" + ZeroFill(tm.Second);
//    }
  }
}

/****** Flash LED Functions ******/
void ackknowledgeButtonPress(){
   analogWrite(FlashLEDPin, LEDGetAttention);
   delay(250);
   analogWrite(FlashLEDPin, 0);
   if (EnableFlashSignaling){ledFlashRunStartTime=0;}  //Eliminates the delay in finding out what the new settings are after a button press
   kickDoggie();
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
     d1 = ((overridePumpScheduleStartTime + overridePumpDuration - currentMinute) / 10);
     d2 = ((overridePumpScheduleStartTime + overridePumpDuration - currentMinute) % 10);
     d3 = 0;
     d4 = 0;  
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

void checkForButtonOverrides(){
  if(checkButtonAction(&btnDisplay)){ 
    EnableFlashSignaling = !EnableFlashSignaling;
    ledFlashRunStartTime=0;
    DisplayStatus();  
    ackknowledgeButtonPress();
   }
  if(checkButtonAction(&btnrelays)){
   if (!overridePumpSchedule){
     overridePumpMode = 1;
   } else {
     overridePumpMode += 1;
     if (overridePumpMode > 3){overridePumpMode=0;}
   }
   overridePumpSchedule = true;
   overridePumpScheduleStartTime = currentMinute;
   clearRelaySchedule();
   if (overridePumpScheduleStartTime + overridePumpDuration >= 1440) {overridePumpScheduleStartTime = 1;}
   switch (overridePumpMode) {
     case 0:   //return to Schedule
       if (EnableDebugging) {DisplayMessage("Pump override selected. Return to Scheduler."); }      
       overridePumpSchedule = false;
       break;
     case 1:  // Off
        if (EnableDebugging) {DisplayMessage("Pump override selected. All off."); }      
        for (int i = 0; i<3; i++){
           relays[i].StartTime = currentMinute + overridePumpDuration;
           relays[i].RunDuration = 5;
           activateRelay(i, false);
        }
       break;
     case 2:  //Feeding
        if (EnableDebugging) {DisplayMessage("Pump override selected. Feeding Schedule."); }      
        for (int i = 1; i<3; i++){
           relays[i].StartTime = currentMinute;
           relays[i].RunDuration = 1;
           activateRelay(i, false);
        }
        break;
     case 3:  //All on
        if (EnableDebugging) {DisplayMessage("Pump override selected. All on."); }      
        for (int i = 0; i < relaysCount; i++){
           relays[i].StartTime = currentMinute;
           relays[i].RunDuration = overridePumpDuration;
           activateRelay(i, true);
        }
       break;
   }
   ackknowledgeButtonPress();
 }
}

void activateRelay(int channel, boolean state){
  if (relays[channel].CurrentlyOn != state) {
     relays[channel].CurrentlyOn = state;
     if (state){
       if (EnableDebugging) {DisplayMessage("Turned relay " + String(channel) + " on for " + String(relays[channel].RunDuration) + " minutes."); }
       digitalWrite(relays[channel].HardwarePin, HIGH);
       if (EnableFlashSignaling){ackknowledgeHarewareActivation(true);}
       delay(5000); //Delay to prevent activating all relays at once, excess startup power draw
     } else {
       if (EnableDebugging) {DisplayMessage("Turned relay " + String(channel) + " off"); }
       digitalWrite(relays[channel].HardwarePin, LOW);//This is not always working.
       if (EnableFlashSignaling){ackknowledgeHarewareActivation(false);}
       delay(5000); //Delay to prevent de-activating all relays at once, breaker popping
     }
  } 
}

void clearRelaySchedule(){
   for (int i=0; i < relaysCount; i++){
      relays[i].StartTime = 0;
      relays[i].RunDuration = 0;
   }
}
void executeNormalPumpSchedule(){
  //Normal daytime Schedule
  if (relays[0].StartTime == 0) { //Main pump, normally on
     relays[0].StartTime = currentMinute;
     relays[0].RunDuration = 30;
  }
  if (relays[1].StartTime ==0){ //Circulation 1: 20 minutes, every 24 minutes
     relays[1].StartTime = currentMinute + 4;
     relays[1].RunDuration = 20;
  }       
  if (relays[2].StartTime ==0){ //Circulation 2: 15 minutes, every 20 minutes
     relays[2].StartTime = currentMinute + 5;
     relays[2].RunDuration = 15;
  }       
}

void executeQuietTimeSchedule(){ 
  if (relays[0].StartTime == 0) { //Main pump
     relays[0].StartTime = currentMinute +10;
     relays[0].RunDuration = 5;
  }
  if (relays[1].StartTime ==0){ //Skimmer, off
     relays[1].StartTime = 0;
     relays[1].RunDuration = 0;
  }       
  if (relays[2].StartTime ==0){ //Front pump, 15 minutes, every 20 minutes
     relays[2].StartTime = currentMinute + 15;
     relays[2].RunDuration = 5;
  }       
 }

void executeFeedingPumpSchedule(){
   if (relays[0].RunDuration > 2){//Turn off main pump, and any ongoing Schedule
     relays[0].StartTime = 0;
     relays[0].RunDuration = 0;
     relays[1].StartTime = 0;
     relays[1].RunDuration = 0;
     relays[2].StartTime = 0;
     relays[2].RunDuration = 0;
    }
   if (relays[1].StartTime == 0){
      relays[1].StartTime = currentMinute + 10; //Schedule next stirring
      relays[1].RunDuration = 1;
      relays[2].StartTime = currentMinute+4;
      relays[2].RunDuration = 1;
 //     activateRelay(2, true);
 //     delay(5000);
 //     activateRelay(3, true);
 //     delay(5000);
   }  
}

void executeSumpLightSchedule(){   
 
}

void runScheduler(){
  //  DisplayMessage("Running Scheduler");
  //Check override status and clear invalid conditions
  if (overridePumpSchedule && (currentMinute >= overridePumpScheduleStartTime + overridePumpDuration)) {
    currentPumpSchedule = 4;
    overridePumpScheduleStartTime = 0;
    overridePumpSchedule = false;
    if (EnableDebugging) {DisplayMessage("Pump override has expired."); }
  }
  for (int i=0; i < relaysCount; i++){
    if (relays[i].StartTime > currentMinute){//Safety check, if the relays aren't scheduled to go for over 120 minutes then clear Schedule
       if (relays[i].StartTime - currentMinute > 120 || relays[i].StartTime > 1440) {  //note since these variables are unsigned 0-10 = a very high int
          relays[i].StartTime = 0;
          relays[i].RunDuration = 0;
       }
    }
    if (relays[i].CurrentlyOn && (relays[i].StartTime + relays[i].RunDuration < currentMinute)) {
       if (EnableDebugging) { DisplayMessage("Relay " + String(i) + " Schedule has expired.");        }
       relays[i].StartTime = 0;
       relays[i].RunDuration = 0;
    }
  }

  //Sump lights
  boolean LightShouldBeOn = false  
  for (int i = 0; i<sumpScheduleLength ; i++){
   if (sumpLightSchedule[i].StartMinute > sumpLightSchedule[i].EndMinute){ //End of day time wraps around
     if (currentMinute > sumpLightSchedule[i].StartMinute || currentMinute < sumpLightSchedule[i].EndMinute){
        LightShouldBeOn = true
     }
   } else {
     if ((currentMinute > sumpLightSchedule[i].StartMinute && currentMinute < sumpLightSchedule[i].EndMinute)){
        LightShouldBeOn = true
     }
   }
  }
  if (LightShouldBeOn) 
        relays[3].StartTime = currentMinute;
        relays[3].RunDuration = 5;
  } else {
        relays[3].StartTime = 0;
        relays[3].RunDuration = 0;
  }

  //Pumps
  if (overridePumpSchedule){
    currentPumpSchedule = overridePumpMode
  } else {                                            //Schedule
    for (int i = 0; i < pumpScheduleLength ; i++){
     if (pumpSchedule[i].StartMinute > pumpSchedule[i].EndMinute){ //End of day time wraps around
       if (currentMinute > pumpSchedule[i].StartMinute || currentMinute < pumpSchedule[i].EndMinute){
        currentPumpSchedule = pumpSchedule[i].PumpModeID
       }
     } else {
       if ((currentMinute > sumpLightSchedule[i].StartMinute && currentMinute < sumpLightSchedule[i].EndMinute)){
        currentPumpSchedule = pumpSchedule[i].PumpModeID
       }
     }
    }
   
  if (currentPumpSchedule == PumpModeNormal){ executeNormalPumpSchedule();  }
  if (currentPumpSchedule == PumpModeFeeding){ executeFeedingPumpSchedule();  }
  if (currentPumpSchedule == PumpModeQuietTime){ executeQuietTimeSchedule();  }
  bool a = false;
  bool b = false;
  for (int i=0; i < relaysCount; i++){
    a = (relays[i].StartTime <= currentMinute);
    b = ((relays[i].RunDuration > 0) && (currentMinute <= (relays[i].StartTime + relays[i].RunDuration)));
    activateRelay(i, a && b); 
  }
}


void setup() {
  if (EnableDebugging) {
    Serial.begin(9600);
    Serial.println("Begin setup");
  }
  pinMode(FlashErrorLEDPin, OUTPUT);
  analogWrite(FlashErrorLEDPin, LEDGetAttention);   //Start red LED on until setup successfully completes
  pinMode(FlashLEDPin, OUTPUT);
  pinMode(A2, OUTPUT);    // Using A2 and A3 to power RTC
  digitalWrite(A2, HIGH);
  pinMode(A3, OUTPUT);
  digitalWrite(A3, LOW);
  pinMode(A1, OUTPUT);
  digitalWrite(A1, LOW);

  sumpLightSchedule[0].StartMinute = 1320;
  sumpLightSchedule[0].EndMinute = 420;
  sumpLightSchedule[1].StartMinute = 600;
  sumpLightSchedule[1].EndMinute = 1140;

  pumpSchedule[0].StartMinute = 450;
  pumpSchedule[0].EndMinute = 510;
  pumpSchedule[0].ModeID = PumpModeFeeding;
  pumpSchedule[1].StartMinute = 510;
  pumpSchedule[1].EndMinute = 1110;
  pumpSchedule[1].ModeID = PumpModeNormal;
  pumpSchedule[2].StartMinute =1110;
  pumpSchedule[2].EndMinute = 1170;
  pumpSchedule[2].ModeID = PumpModeFeeding;
  pumpSchedule[3].StartMinute = 1170;
  pumpSchedule[3].EndMinute = 450;
  pumpSchedule[3].ModeID = PumpModeNormal;

  relays[0].HardwarePin = pumpMainChannel;
  relays[0].CurrentlyOn = false;
  relays[0].StartTime = 0;
  relays[0].RunDuration = 0;
  relays[1].HardwarePin = pumpCirc1Channel;
  relays[1].CurrentlyOn = false;
  relays[1].StartTime = 0;
  relays[1].RunDuration = 0;
  relays[2].HardwarePin = pumpCirc2Channel;
  relays[2].CurrentlyOn = false;
  relays[2].StartTime = 0;
  relays[2].RunDuration = 0;
  relays[3].HardwarePin = sumpLightChannel;
  relays[3].CurrentlyOn = false;
  relays[3].StartTime = 0;
  relays[3].RunDuration = 0;
  for (int i = 0; i < relaysCount; i++) {
     pinMode(relays[i].HardwarePin, OUTPUT);
     digitalWrite(relays[i].HardwarePin, LOW);  //Start Off
  }
  watchdogSetup();
  if (EnableDebugging){DisplayMessage("Checking the clock");}
  setDateTimeVars();
  if (EnableDebugging){DisplayMessage("Finished setup");}
  analogWrite(FlashErrorLEDPin, 0);
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
      runScheduler();
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
       } else if (overridePumpSchedule){
         runFlashOverrideSettings();
       }else {
         runFlashTime();
       }
    }
    doggieTickle();
    if (bedTimeCounter > 0){
        bedTimeCounter --;
       delay(100); //Run every 100 milliseconds
    } else {
       EnableFlashSignaling = false;
       delay(3000); //Run every 3 seconds unless there was a button press or a light action recently.
    }
    inLoop = false;
  }
}
