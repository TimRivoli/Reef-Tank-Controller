# Reef-Tank-Controller
Arduino project for controlling LED lights and cycling pumps for a salt water aquarium

The hardware I used for this project was:
-Arduino UNO R3 board with DIP ATmega328P 
-SainSmart Tiny RTC I2C DS1307 AT24C32 24C32 memory Real Time Clock Module for Arduino
-microtivity IM206 6x6x6mm Tact Switch 
-SainSmart 4-Channel Relay Module
-Custom Biocube LED light system from Typhon

The program controls three water pumps via the relay circuits to control the 110V power to the pumps.  It cycles the pumps
to create variable water flow through the tank with quiet periods in the morning and evening for feeding without filtration.
It also controls the lights, slowly bringing up the blue lights in the morning, followed by the white lights to simulate dawn.
Throughout the day the lights are adjusted to create interesting effects.  Generally, I keep the white lights low to inhibit
Algae growth but sometimes it is nice to see the tank with more white light for brief periods.  In the evening, the lights fade
again to blue before turning off for the day.

Three buttons are used for temporary overrides.
-Button1 temporarily overrides the light setting to Low/Medium/High intensity for 30 minutes
-Button2 temporarily overrides the pumps cycling to either all off or all on for 30 minutes
-Button3 enables an LED signaling system that will flash you the current system time and light intensity

Feel free to customize and enjoy!

-Tim
