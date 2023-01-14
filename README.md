# Arduino gasmeter monitoring for Wemos D1 Mini (and battery shield) with mqtt support
  
- Setup A (recommended for battery): Reed switch connected to SWITCH OUT/IN on a MicroWakeupper battery shield (stacked to a Wemos D1 mini)

- Setup B (external power/USB): Inductive proximity sensor LJ12A3-4-Z/BX 5V connected with signal line to SWITCH IN (common GND) on a MicroWakeupper battery shield (stacked to a Wemos D1 mini). The sensor usually consumes about 50mA (always on).
  
---

There are different gasmeters in the wild - usually one with 
a) an internal magnet that is turing around - so we use a reed switch for counting each turnaround
or
b) an internal metal plate is turing around - so we use a proximity sensor LJ12A3-4-Z/BX 5V for counting each turnaround

Usually one turn around is euqal to 0,01m3 (or 10 liters) of gas - check your gasmeter.

The stacked MicroWakeupper shield is turning your Wemos D1 mini on and off. Recommended: onboard jumper J1 cutted!
  
The firmware will count each cycle, write the total amount to eeprom and sent a mqtt message to your broker with the total amount of gas (total_m3).
  
## Setup/Installation
1. Install the MicroWakeupper library (https://github.com/tstoegi/MicroWakeupper) to your Arduino IDE 
2. Update the sketch code: Setup your custom config data within the lines // $$$config$$$
3. Connect a Wemos D1 mini (or pro, with external antenna) via USB and upload the code (if the MicroWakeupper shield if already stacked, you have to press the onboard FLASH button during upload) 
   
   -> OTA: If the firmware is already installed, you can use OTA updates by setting (with retain) the mqtt value "/settings/turningOff" to "false" once. OTA only works as long as the Wemos is on.)
  
Warning: As long as you power the Wemos via USB (or external VIN) the MicroWakeupper shield cannot turn it off

All mqtt messages from the client (Wemos) are sent with flag "retain" - so you see the last messages, even if the device is currently off.
  
## faq
Q: Where can I buy the MicroWakeupper battery shield?
<br>
A: My store: https://www.tindie.com/stores/moreiolabs/

Q: How can I set an initial counter value?
<br>
A: Just send/publish a mqtt message (with retain!) to "haus/gasmeter/settings/total_m3" e.g. "202.23" - after receiving there is a response with "0" and the normal counter "total_m3" should be up to date

Q: How can I install OTA update (via Arduino IDE)?
<br>
A: Send/publish a mqtt message (with retain!) to "haus/gasmeter/settings/turningOff" e.g. "false" - after receiving the Wemos will stay online (until the next restart or external reset!). You will see a OTA device named "gasmeter_ip_address".

Q: How can I adjust (calibrate) the battery voltage value?
<br>
A: Send/publish a mqtt message (with retain!) to "haus/gasmeter/settings/voltageCalibration" e.g. "+0.3" or "-0.5" volt

(c) 2022, 2023 @tstoegi, Tobias Stöger, MIT license
  
