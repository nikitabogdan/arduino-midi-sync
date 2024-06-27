/*
 * Arduino Midi Master Clock v0.3
 * MIDI master/slave clock/sync/divider for MIDI instruments, Pocket Operators and Korg Volca.
 * by Eunjae Im https://github.com/ejlabs/arduino-midi-sync

 * v.03 fork made by Nikita Bogdan https://github.com/nikitabogdan/arduino-midi-sync
 * - added slave functionality (for audio sync-in)
 * - added switch between master and slave modes
 * - fixed minor defects: added resetting timer after stop action, stopped midi clock after stop action
 *
 * Required libraries
 *    TimerOne https://playground.arduino.cc/Code/Timer1
 *    Encoder https://www.pjrc.com/teensy/td_libs_Encoder.html
 *    MIDI https://github.com/FortySevenEffects/arduino_midi_library
 *    Adafruit SSD1306 https://github.com/adafruit/Adafruit_SSD1306
 *    ArduinoTapTempo https://github.com/dxinteractive/ArduinoTapTempo
 *    ezButton https://github.com/bmcdonnell/Arduino-ezButton
 *******************************************************************************
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *******************************************************************************
 */

#include <Adafruit_SSD1306.h>
#include <ArduinoTapTempo.h>
#include <TimerOne.h>
#include <EEPROM.h>
#include <Encoder.h>
#include <ezButton.h>
#include <MIDI.h>

#define OLED_RESET 4
Adafruit_SSD1306 display(OLED_RESET);

#define LED_PIN1 7 // Tempo LED
#define SYNC_OUTPUT_PIN 6 // Audio sync-out digital pin
#define SYNC_OUTPUT_PIN2 8 // 2nd audio sync-out pin
#define SWITCH_MODE_PIN 10 // Master/Slave state pin
#define SYNC_INPUT_PIN 11 // Audio sync-in digital pin
#define BUTTON_START 4 // Start/Stop push button
#define BUTTON_ROTARY 5 // Rotary Encoder button

#define CLOCKS_PER_BEAT 24 // MIDI clock ticks
#define AUDIO_SYNC 12 // Audio sync ticks
#define AUDIO_SYNC2 12 // 2nd audio sync ticks

#define MINIMUM_BPM 10
#define MAXIMUM_BPM 300

#define BLINK_TIME 4 // LED blink time
#define TEXT_SIZE 4 // Display text size
bool external_bpm = false; // Display 'e' mark after bpm

ArduinoTapTempo tapTempo; // ArduinoTapTempo object
ezButton toggleSwitch(SWITCH_MODE_PIN); // Master/Slave mode switch

volatile int  blinkCount = 0,
              blinkCount2 = 0,
              AudioSyncCount = 0,
              AudioSyncCount2 = 0;

long intervalMicroSeconds,
      bpm,
      audio_sync2;

boolean playing = false,
      sync_editing = false,
      display_update = false;

Encoder myEnc(2, 3); // Rotary Encoder Pin 2,3 

MIDI_CREATE_DEFAULT_INSTANCE();

void setup(void) {
  MIDI.begin(); // MIDI init
  MIDI.turnThruOff();

  bpm = EEPROMReadInt(0);
  if (bpm > MAXIMUM_BPM || bpm < MINIMUM_BPM) {
    bpm = 120;
  }
  audio_sync2 = EEPROMReadInt(3);
  if (audio_sync2 > 64 || audio_sync2 < 2) {
    audio_sync2 = 12;
  }
   
  Timer1.initialize(intervalMicroSeconds);
  Timer1.setPeriod(60L * 1000 * 1000 / bpm / CLOCKS_PER_BEAT);
  Timer1.attachInterrupt(sendClockPulse); 

  toggleSwitch.setDebounceTime(50);

  tapTempo.setMinBPM((float)MINIMUM_BPM);
  tapTempo.setMaxBPM((float)MAXIMUM_BPM);
  tapTempo.setMinBeatLengthMS(200); // 300 BPM
  tapTempo.setMaxBeatLengthMS(6000); // 10 BPM
  tapTempo.setBPM((float)bpm);
  
  pinMode(BUTTON_START,INPUT_PULLUP);
  pinMode(BUTTON_ROTARY,INPUT_PULLUP);
  pinMode(SYNC_INPUT_PIN,INPUT);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(WHITE);  
  display.setTextSize(TEXT_SIZE);
  display.setCursor(0,0);
  display.print(bpm);
  display.display();
}

void EEPROMWriteInt(int p_address, int p_value)
     {
     byte lowByte = ((p_value >> 0) & 0xFF);
     byte highByte = ((p_value >> 8) & 0xFF);

     EEPROM.write(p_address, lowByte);
     EEPROM.write(p_address + 1, highByte);
     }

unsigned int EEPROMReadInt(int p_address)
     {
     byte lowByte = EEPROM.read(p_address);
     byte highByte = EEPROM.read(p_address + 1);

     return ((lowByte << 0) & 0xFF) + ((highByte << 8) & 0xFF00);
}

void bpm_display() {
  updateBpm();
  EEPROMWriteInt(0,bpm);  
  display.setTextSize(TEXT_SIZE);
  display.setCursor(0,0);  
  display.setTextColor(WHITE, BLACK);
  display.print("     ");
  display.setCursor(0,0);
  display.print(bpm);
  if (external_bpm) {
    display.print(" e");
  }
  display.display();
  display_update = false;
}

void sync_display() {
  EEPROMWriteInt(3,audio_sync2);
  
  int sync_current;
  sync_current = audio_sync2 - 12;  
  
  if (sync_current < 0) {    
    sync_current = abs(sync_current);
  } else if (sync_current > 0) {
    sync_current = -sync_current;
  }
    
  display.setTextSize(TEXT_SIZE);
  display.setCursor(0,0);
  display.setTextColor(WHITE, BLACK);
  display.print("     ");  
  display.setCursor(0,0);
  display.print(sync_current);
  display.display();
}

void startOrStop() {
  if (!playing) {
    MIDI.sendRealTime(midi::Start);
    Timer1.restart(); 
  } else {
    all_off();
  }
  playing = !playing;
}

int oldPosition;
int oldSwitchState;

void loop(void) {
  byte i = 0;
  byte p = 0;
  boolean clockIn = 0;

  toggleSwitch.loop();

  if (digitalRead(BUTTON_START) == LOW) {
    startOrStop();
    delay(300); // ugly but just make life easier, no need to check debounce
  } else if (digitalRead(BUTTON_ROTARY) == LOW) {    
    p = 1;
    delay(200);
  }
  
  int newPosition = (myEnc.read()/4);
  if (newPosition != oldPosition) {    
    if (oldPosition < newPosition) {
      i = 2;
    } else if (oldPosition > newPosition) {
      i = 1;
    }
    oldPosition = newPosition;
  }

  int newSwitchState = toggleSwitch.getState();
  if (newSwitchState == HIGH) { // check if mode is slave
    clockIn = digitalRead(SYNC_INPUT_PIN) == HIGH; // get the state of the sync input pin
    tapTempo.update(clockIn); // update ArduinoTapTempo
    if (clockIn == 1) {
          bpm = round(tapTempo.getBPM() * 10 / 10.0); // round bpm to bigger digit
    }
  } else if (newSwitchState != oldSwitchState) { // remove 'e' mark from display when mode is switched back to master
    external_bpm = false;
    bpm_display();          
  }

  oldSwitchState = newSwitchState;

  if (!sync_editing) {      
      if (i == 2) {
        bpm++;
        if (bpm > MAXIMUM_BPM) {
          bpm = MAXIMUM_BPM;
        }
        external_bpm = false;
        bpm_display();          
      } else if (i == 1) {
        bpm--;
        if (bpm < MINIMUM_BPM) {
          bpm = MINIMUM_BPM;
        }
        external_bpm = false;
        bpm_display();
      } else if (p == 1) {
        sync_display();
        sync_editing = true;
      } else if (clockIn == 1) {
        external_bpm = true;
        bpm_display();
      }
  } else  { // 2nd jack audio sync speed
      if (p == 1) {      
        bpm_display();
        sync_editing = false;
      } else if (i == 1) {      
        audio_sync2++;
        if (audio_sync2 > 64) { audio_sync2 = 64; }
        sync_display();
      } else if (i == 2) {
        audio_sync2--;
        if (audio_sync2 < 2) { audio_sync2 = 2; }
        sync_display();
      }      
  }
}

void all_off() { // make sure all sync, led pin stat to low
  MIDI.sendRealTime(midi::Stop);
  digitalWrite(SYNC_OUTPUT_PIN, LOW);
  digitalWrite(SYNC_OUTPUT_PIN2, LOW);
  digitalWrite(LED_PIN1, LOW);
}

void sendClockPulse() {  
  if (playing) {  
    MIDI.sendRealTime(midi::Clock); // sending midi clock
    blinkCount = (blinkCount + 1) % CLOCKS_PER_BEAT;
    blinkCount2 = (blinkCount2 + 1) % (CLOCKS_PER_BEAT / 2);
    AudioSyncCount = (AudioSyncCount + 1) % AUDIO_SYNC;
    AudioSyncCount2 = (AudioSyncCount2 + 1) % audio_sync2;
    if (AudioSyncCount == 0) {
      digitalWrite(SYNC_OUTPUT_PIN, HIGH); 
    } else {        
      if (AudioSyncCount == 1) {     
        digitalWrite(SYNC_OUTPUT_PIN, LOW);
      }
    }  

    if (AudioSyncCount2 == 0) {
      digitalWrite(SYNC_OUTPUT_PIN2, HIGH);
    } else {        
      if (AudioSyncCount2 == 1) {
        digitalWrite(SYNC_OUTPUT_PIN2, LOW);
      }
    }
  
    if (blinkCount == 0) {
        digitalWrite(LED_PIN1, HIGH);      
    } else {
       if (blinkCount == BLINK_TIME) {
         digitalWrite(LED_PIN1, LOW);      
       }
    }
  }
}

void updateBpm() { // update BPM function (on the fly)
  long interval = 60L * 1000 * 1000 / bpm / CLOCKS_PER_BEAT;  
  Timer1.setPeriod(interval);
}
