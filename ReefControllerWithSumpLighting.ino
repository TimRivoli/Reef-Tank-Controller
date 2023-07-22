 #include <Wire.h>
#include <Button.h>
#include <avr/wdt.h>
#include <Time.h>
#include <RtcDS3231.h>
#define MY_TIMEZONE -8
#define MY_TIMEZONE_IN_SECONDS (MY_TIMEZONE * ONE_HOUR)
#define myWire TwoWire
#define I2C Wire
RtcDS3231<myWire> Rtc(I2C);

//Global Constants
#define EnableDebugging true
#define SumpAcclimationMode false
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
#define sumpLightChannel   10     //aka relays[3], non pwm channel for sump light relay - sump light
#define pump1Channel   11     //aka relays[0], non pwm channel for pump relay - tank circulation pump
#define pump2Channel   12     //aka relays[1], non pwm channel for pump relay - tank circulation pump
#define pump3Channel   13     //aka relays[2], non pwm channel for pump relay - sump circulation pump

boolean EnableFlashSignaling = false;
boolean EnableStatusDisplayOutput =true;
Button btnDisplay   = Button(4,PULLUP);
Button btnrelays    = Button(7,PULLUP); 
//Button btnLights      = Button(2,PULLUP);

//Pump mode IDs
#define PumpModeNormal 0
#define PumpModeFeeding 1
#define PumpModeAllOn 2
#define PumpModeAllOff 3
 #define PumpModeQuietTime 4
#define PumpModeNight 5

typedef struct {
 unsigned int StartMinute;
 unsigned int EndMinute;
 unsigned int PumpModeID; //pump mode function to run
}
ScheduleType;
#define sumpScheduleLength 2 //number of time frame elements in the sump light schedule
ScheduleType sumpLightSchedule[sumpScheduleLength];
#define pumpScheduleLength 5 //number of time frame elements in the pump schedule
ScheduleType pumpSchedule[pumpScheduleLength];

//Pump Variables
boolean overridePumpSchedule = false;              //Button has been pressed to override normal pump schedule
unsigned int overridePumpScheduleStartTime = 0;    // minute the schedule override started, it will go for 30 minutes
unsigned int currentPumpSchedule = PumpModeNormal; 

typedef struct {                //relay definiation
 unsigned int HardwarePin;      // channel pin
 unsigned int StartTime;        // time in minutes it started
 unsigned int RunDuration;      // duration it should run in minutes
 boolean CurrentlyOn;          //  current state
}
relaysType;
#define relaysCount 4
relaysType relays[relaysCount];

//Global dynamic variables
#define bedTimeStart 6000;  //loop iterations until we begin a slow sleep mode
unsigned int bedTimeCounter = 0;  //counter for starting slow mode
bool inLoop = false;  //Lock to prevent looping while processing the main loop
unsigned long currentMillisecond = 0;  //These will be set by loop and used globally
unsigned long lastRun = 0;
unsigned long lastStatusUpdate = 0;
unsigned int currentMinute = 0;  
unsigned int currentTemp = 0;
String currentDate = "";
String currentTime = "";
String currentClockTemp = "";

unsigned long ledFlashRunStartTime=0;  // Time in milliseconds the run began

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
void DisplayPumpMode(){
   if (overridePumpSchedule) {
    DisplayMessage("Pump override until " + String(overridePumpScheduleStartTime + overridePumpDuration));
   }
  switch (currentPumpSchedule) {
     case PumpModeNormal :   
       DisplayMessage("Pump Mode: Normal");       
       break;
     case PumpModeFeeding:  
        DisplayMessage("Pump Mode: Feeding");      
        break;
     case PumpModeAllOn: 
        DisplayMessage("Pump Mode: All on");  
        break;
     case PumpModeAllOff:  // Off
        DisplayMessage("Pump Mode: All off");    
         break;
     case PumpModeQuietTime:  //Quiet Time
        DisplayMessage("Pump Mode: Quiet Time");     
       break;
   }
}

void DisplayStatus(){
   DisplayNewLine();
   DisplayMessage("Current status at " + String(currentMinute) + " minutes.");
   if (!EnableFlashSignaling) {DisplayMessage("LED signaling is off.");}
   DisplayMessage("Pump0: " + String(relays[0].CurrentlyOn) + " " + String(relays[0].StartTime) + ":" + String(relays[0].RunDuration));
   DisplayMessage("Pump1: " + String(relays[1].CurrentlyOn) + " " + String(relays[1].StartTime) + ":" + String(relays[1].RunDuration));
   DisplayMessage("Pump2: " + String(relays[2].CurrentlyOn) + " " + String(relays[2].StartTime) + ":" + String(relays[2].RunDuration));
   DisplayMessage("Sump Light: " + String(relays[3].CurrentlyOn) + " " + String(relays[3].StartTime) + ":" + String(relays[3].RunDuration));
   DisplayPumpMode();
   DisplayMessage("Clock Temp: " + currentClockTemp);
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
  if (Rtc.IsDateTimeValid()) {
    //struct tm utc_tm;
    struct tm local_tm;
    char local_timestamp[20];
    Rtc.GetLocalTime(&local_tm);                  // GetLocalTime() compiles a "struct tm" pointer with local time
    strcpy(local_timestamp, isotime(&local_tm));  // We use the standard isotime() function to build the ISO timestamp
    //if (EnableDebugging) {Serial.println("Date: " + String(local_timestamp));}

    //Local timestamp: 2022-11-30 21:16:31 parse this
    currentMinute = (local_tm.tm_min + (local_tm.tm_hour * 60));
    currentDate = ZeroFill(local_tm.tm_mon) + "\\" + ZeroFill(local_tm.tm_mday) + "\\" + ZeroFill(local_tm.tm_year);
    currentTime = ZeroFill(local_tm.tm_hour) + ":" + ZeroFill(local_tm.tm_min) + ":" + ZeroFill(local_tm.tm_sec);

    float temperature = Rtc.GetTemperature();
    //if (EnableDebugging) {Serial.println("Clock Temperature: " + String(temperature * 1.8 + 32) + "F");}
    currentClockTemp = String(temperature * 1.8 + 32) + "F";
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
     clearRelaySchedule();
     currentPumpSchedule += 1;
     if (currentPumpSchedule > PumpModeQuietTime){currentPumpSchedule=PumpModeNormal;}
     overridePumpSchedule = !(currentPumpSchedule == 0 );
     if (overridePumpSchedule) { 
      overridePumpScheduleStartTime = currentMinute;
      if (overridePumpScheduleStartTime + overridePumpDuration >= 1440) {overridePumpScheduleStartTime = 1;}
     }
     if (EnableDebugging) {
      DisplayNewLine();
      DisplayPumpMode();
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
       delay(1000); //Delay to prevent activating all relays at once, excess startup power draw
     } else {
       if (EnableDebugging) {DisplayMessage("Turned relay " + String(channel) + " off"); }
       digitalWrite(relays[channel].HardwarePin, LOW);//This is not always working.
       if (EnableFlashSignaling){ackknowledgeHarewareActivation(false);}
       delay(1000); //Delay to prevent de-activating all relays at once, breaker popping
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
  if (relays[0].StartTime == 0) { //Circulation pump 1
     relays[0].StartTime = currentMinute + 5;
     relays[0].RunDuration = 15;
  }
  if (relays[1].StartTime ==0){ //Circulation pump 2: 20 minutes, every 24 minutes
     relays[1].StartTime = currentMinute +4;
     relays[1].RunDuration = 20;
  }       
  if (relays[2].StartTime ==0){ //Circulation pump sump
     relays[2].StartTime = currentMinute + 1;
     relays[2].RunDuration = 30;
  }       
}
void executeNightPumpSchedule(){
  //Normal daytime Schedule
  if (relays[0].StartTime == 0) { //Circulation 1
     relays[0].StartTime = currentMinute+3;
     relays[0].RunDuration = 10;
  }
  if (relays[1].StartTime ==0){ //Circulation 2: 20 minutes, every 24 minutes
     relays[1].StartTime = currentMinute + 4;
     relays[1].RunDuration = 20;
  }       
  if (relays[2].StartTime ==0){ //Circulation Sump
     relays[2].StartTime = currentMinute ;
     relays[2].RunDuration = 30;
  }       
}

void executeQuietTimeSchedule(){ 
  if (relays[0].StartTime == 0) { //Circulation 1
     relays[0].StartTime = currentMinute + 5;
     relays[0].RunDuration = 1;
  }
  if (relays[1].StartTime ==0){ //Circulation 2
     relays[1].StartTime = currentMinute + 7;
     relays[1].RunDuration = 1;
  }       
  if (relays[2].StartTime ==0){ //Circulation Sump
     relays[2].StartTime = currentMinute;
     relays[2].RunDuration = 30;
  }       
 }

void executeFeedingPumpSchedule(){
   if (relays[0].RunDuration > 2){//Turn off circulation pumps
     relays[0].StartTime = 0;
     relays[0].RunDuration = 0;
     relays[1].StartTime = 0;
     relays[1].RunDuration = 0;
     //relays[2].StartTime = 0;
     //relays[2].RunDuration = 0;
    }
   if (relays[0].StartTime == 0){
      relays[0].StartTime = currentMinute + 4; //Schedule next stirring
      relays[0].RunDuration = 1;
      relays[1].StartTime = currentMinute + 8;
      relays[1].RunDuration = 1;
 //     activateRelay(2, true);
 //     delay(5000);
 //     activateRelay(3, true);
 //     delay(5000);
   }  
}

void executePumpsManual(bool TurnOn){
 int HowLong = overridePumpDuration;
 if (HowLong == 0) {HowLong = 5;}
 if (TurnOn) {
  for (int i = 0; i < relaysCount-1; i++){
     relays[i].StartTime = currentMinute;
     relays[i].RunDuration = overridePumpDuration;
     activateRelay(i, true);
  }
 } else {
  for (int i = 0; i < relaysCount-1; i++){
     relays[i].StartTime = currentMinute + overridePumpDuration;
     relays[i].RunDuration = 5;
     activateRelay(i, false);
  }
 }
}
void runScheduler(){
  //  DisplayMessage("Running Scheduler");
  //Check override status and clear invalid conditions
  if (overridePumpSchedule && (currentMinute >= overridePumpScheduleStartTime + overridePumpDuration)) {
    currentPumpSchedule = PumpModeNormal;
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
  boolean LightShouldBeOn = false; 
  for (int i = 0; i < sumpScheduleLength; i++){
   if (sumpLightSchedule[i].StartMinute > sumpLightSchedule[i].EndMinute){ //End of day time wraps around
     if (currentMinute > sumpLightSchedule[i].StartMinute || currentMinute < sumpLightSchedule[i].EndMinute){
        LightShouldBeOn = true;
     }
   } else {
     if ((currentMinute > sumpLightSchedule[i].StartMinute && currentMinute < sumpLightSchedule[i].EndMinute)){
        LightShouldBeOn = true;
     }
   }
  }
  if (LightShouldBeOn) {
        relays[3].StartTime = currentMinute;
        relays[3].RunDuration = 5;
  } else {
        relays[3].StartTime = 0;
        relays[3].RunDuration = 0;
  }

  //Pumps
  if (!overridePumpSchedule){                                            
    for (int i = 0; i < pumpScheduleLength ; i++){
     if (pumpSchedule[i].StartMinute > pumpSchedule[i].EndMinute){ //End of day time wraps around
       if (currentMinute > pumpSchedule[i].StartMinute || currentMinute < pumpSchedule[i].EndMinute){
        currentPumpSchedule = pumpSchedule[i].PumpModeID;
       }
     } else {
       if ((currentMinute > sumpLightSchedule[i].StartMinute && currentMinute < sumpLightSchedule[i].EndMinute)){
        currentPumpSchedule = pumpSchedule[i].PumpModeID;
       }
     }
    }
  }

  if (currentPumpSchedule == PumpModeNormal){ executeNormalPumpSchedule();  }
  if (currentPumpSchedule == PumpModeFeeding){ executeFeedingPumpSchedule();  }
  if (currentPumpSchedule == PumpModeAllOn){ executePumpsManual(true);  }
  if (currentPumpSchedule == PumpModeAllOff){ executePumpsManual(false);  }
  if (currentPumpSchedule == PumpModeQuietTime){ executeQuietTimeSchedule();  }
  if (currentPumpSchedule == PumpModeNight){ executeNightPumpSchedule();  }
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
  digitalWrite(A2, LOW);
  pinMode(A3, OUTPUT);
  digitalWrite(A3, HIGH);
  pinMode(A1, OUTPUT);
  digitalWrite(A1, LOW);
  
  Rtc.Begin();
  set_zone(MY_TIMEZONE_IN_SECONDS);

  //setTime();

  if (SumpAcclimationMode) { //10pm to 6AM, 8hrs
    sumpLightSchedule[0].StartMinute = 1320;
    sumpLightSchedule[0].EndMinute = 360;
    sumpLightSchedule[0].StartMinute = 1200;
    sumpLightSchedule[0].EndMinute = 600;
  } else { // pm to 10am hours on, 16 hrs, 8 hours off
    sumpLightSchedule[0].StartMinute = 1200;
    sumpLightSchedule[0].EndMinute = 600;
  }

  pumpSchedule[0].StartMinute = 450;
  pumpSchedule[0].EndMinute = 510;
  pumpSchedule[0].PumpModeID = PumpModeFeeding;
  pumpSchedule[1].StartMinute = pumpSchedule[0].EndMinute;
  pumpSchedule[1].EndMinute = 1110;
  pumpSchedule[1].PumpModeID = PumpModeNormal;
  pumpSchedule[2].StartMinute =pumpSchedule[1].EndMinute;
  pumpSchedule[2].EndMinute = 1170;
  pumpSchedule[2].PumpModeID = PumpModeFeeding;
  pumpSchedule[3].StartMinute = pumpSchedule[2].EndMinute;
  pumpSchedule[3].EndMinute = 1380;
  pumpSchedule[3].PumpModeID = PumpModeNormal;
  pumpSchedule[4].StartMinute = pumpSchedule[3].EndMinute;
  pumpSchedule[4].EndMinute = 450;
  pumpSchedule[4].PumpModeID = PumpModeNight;

  relays[0].HardwarePin = pump1Channel;
  relays[0].CurrentlyOn = false;
  relays[0].StartTime = 0;
  relays[0].RunDuration = 0;
  relays[1].HardwarePin = pump2Channel;
  relays[1].CurrentlyOn = false;
  relays[1].StartTime = 0;
  relays[1].RunDuration = 0;
  relays[2].HardwarePin = pump3Channel;
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
    doggieTickle();
    if (bedTimeCounter > 0){
        bedTimeCounter --;
       delay(100); //Run every 100 milliseconds
    } else {
       delay(3000); //Run every 3 seconds unless there was a button press or a light action recently.
    }
    inLoop = false;
  }
}
