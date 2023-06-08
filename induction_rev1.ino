/* Induction anneal controller
 *  Board TTGO 17 V1.4 Min32
 *  Port usbserial
 *  TFT display to manage the duration
 *  3 buttons - to operate
 *  Temperature OneWire DS18s20 - to track overheat - realised via independent ticker
 *  Store all data into EEPROM
 *  Power meter CS5460 - TODO, not yet implemented.I tried to connect to SD card socket, but without success.
 *  09-June-2021
 *  11-Nov-2021
 *  kot.dnz@gmail.com
*/
#include <TFT_eSPI.h> // Graphics and font library for ST7735 driver chip
#include <SPI.h>
#include <math.h>
#include <time.h>
#include <Button2.h>
#include <CS5460.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <Ticker.h>

#define DEBUG 0

// the current address in the EEPROM (i.e. which byte
// we're going to write to next)
int addr = 0;
size_t sizeEEPROM = 8;

#define POWER_RELAY_PIN 0
#define TEMP_DS  33
#define TEMP_Vcc 27

// our buttons
#define BUTTON_A_PIN 34
#define BUTTON_B_PIN 35
#define BUTTON_C_PIN 39
Button2 button_Ok = Button2(BUTTON_A_PIN);
Button2 button_Up = Button2(BUTTON_B_PIN);
Button2 button_Dn = Button2(BUTTON_C_PIN);

// Invoke library, pins defined in User_Setup.h
// (16, 17, 23, 5, 9); // CS,A0,SDA,SCK,RESET
// pin 27 == BL
TFT_eSPI tft = TFT_eSPI();  

// Yellow - CS - 13
// Green  - DI 2/15
// Blue   - VD+ +3.3v
// Violet - SCLK - 14
// Gray   - Gnd
// White  - DO 15 / 2
// DO == MOsi, DI == MIso

// TODO: add this feature
// specify the cs
// CS5460 powerMeter(13);

// OneWire DS18s20
OneWire  ds(TEMP_DS);  // on pin 33 (a 4.7K resistor is necessary)
# define TEMP_MAX 80
bool tAlarm = false;        // if overheat

/* create a hardware timer for reading the temp*/
Ticker tReader;
const float readPeriod = 1; //seconds

unsigned long curTime = 0, start_time = 0; // milliseconds from start

#define DUR_DEF 6.5     // Set the default duration 
#define DUR_MIN 1       // Set mininmum possile duration
#define DUR_MAX 15      // Maximum possible
#define POW_DEF 700     // Set the default power in watt
#define POW_MIN 100     // Minimum value
#define POW_MAX 999     // Max value
#define AMP_DEF 1       // for the displaying purposes
#define VLT_DEF 1       // for the displaying purposes
#define WAIT 700        // Pause in milliseconds for the long click

float duration_sec = DUR_DEF, old_dur = 0, dur_def = 0;
int power = POW_DEF, old_pow = 0, pow_def = POW_DEF;
float amper = AMP_DEF, old_amp = 0;
float volt = VLT_DEF, old_volt = 0;
float celsius = 0, old_celsius = 0;
bool sec = true;   // if false - we will use power instead of duration in sec
bool started = false, edit_sec = false, edit_pow = false;
long long_btn_Up = 0, long_btn_Dn = 0;

void read_temp(void){
  // variables for the OneWire
  byte data[12];
  byte present = 0;  // variables for the OneWire
  float cels;
  byte addr_s[8], type_s;     // OneWire sensor data
  
  if ( !ds.search(addr_s)) {
    if(DEBUG){
      Serial.println("No more addresses on a bus");
      Serial.println();
    }
    ds.reset_search();
    delay(250);
    return;
  }
  if (OneWire::crc8(addr_s, 7) != addr_s[7]) {
      if(DEBUG) Serial.println("CRC is not valid!");
      return ;
  }
    // the first ROM byte indicates which chip
  switch (addr_s[0]) {
    case 0x10:
      if(DEBUG) Serial.println("  Chip = DS18S20");  // or old DS1820
      type_s = 1;
      break;
    case 0x28:
      if(DEBUG) Serial.println("  Chip = DS18B20");
      type_s = 0;
      break;
    case 0x22:
      if(DEBUG) Serial.println("  Chip = DS1822");
      type_s = 0;
      break;
    default:
      if(DEBUG) Serial.println("Device is not a DS18x20 family device.");
      return;
  } 
  ds.reset();
  ds.select(addr_s);
  ds.write(0x44, 1);        // start conversion, with parasite power on at the end
  
  delay(800);     // maybe 750ms is enough, maybe not
  
  // read temp data - we are expecting only one sensor on a bus
  present = ds.reset();
  ds.select(addr_s);    
  ds.write(0xBE);         // Read Scratchpad
  for ( int i = 0; i < 9; i++) {           // we need 9 bytes
    data[i] = ds.read();
  }

  // Convert the data to actual temperature
  // because the result is a 16 bit signed integer, it should
  // be stored to an "int16_t" type, which is always 16 bits
  // even when compiled on a 32 bit processor.
  int16_t raw = (data[1] << 8) | data[0];
  if (type_s) {
    raw = raw << 3; // 9 bit resolution default
    if (data[7] == 0x10) {
      // "count remain" gives full 12 bit resolution
      raw = (raw & 0xFFF0) + 12 - data[6];
    }
  } else {
    byte cfg = (data[4] & 0x60);
    // at lower res, the low bits are undefined, so let's zero them
    if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
    else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
    else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
    //// default is 12 bit resolution, 750 ms conversion time
  }
  celsius = (float)raw / 16.0;
  return;
}

void setup() {  
  // setup the serial
  Serial.begin(115200);

  //setup the pin for power relay snd switch it OFF
  pinMode(POWER_RELAY_PIN, OUTPUT);
  digitalWrite(POWER_RELAY_PIN, LOW);  

  // init EEPROM
  EEPROM.begin(sizeEEPROM);
 
 // read stored duration
  dur_def = read_from_eeprom(0); 
  if(DEBUG) {
    Serial.println(dur_def);
    Serial.println(" bytes read from Flash . Values are:");
    for (int i = 0; i < sizeEEPROM; i++)
    {
      Serial.print(byte(EEPROM.read(i))); Serial.print(" ");
    }
    Serial.println();
  }
  dur_def > 0 ? duration_sec = dur_def : dur_def = DUR_DEF;
  
  // setup the display
  tft.init();
  tft.setRotation(3);
  // First we test them with a background colour set
  tft.setTextSize(1);
  show_display();
  show_values();
  show_box();

  button_Up.setTapHandler(btn_click);
  button_Dn.setTapHandler(btn_click);
  button_Ok.setTapHandler(btnOk_clock);
  
  button_Up.setDoubleClickHandler(btn_handler);
  button_Dn.setDoubleClickHandler(btn_handler);

//button_Up.setDebounceTime(10); // set debounce time to 50 milliseconds
//button_Dn.setDebounceTime(10); 
//button_Ok.setDebounceTime(10); 
  button_Up.setDoubleClickTime(300); 
  button_Dn.setDoubleClickTime(300); 
//button_Ok.setDoubleClickTime(100); 


  // scp, miso, most, ss
  //powerMeter.init();
  //powerMeter.startMultiConvert();
  //powerMeter.calibrateVoltageOffset(); 

  // OneWire section
  // switch on power to DS18s20  
  pinMode(TEMP_Vcc, OUTPUT);
  digitalWrite(TEMP_Vcc, HIGH); 
  tReader.attach(readPeriod, read_temp);
}

void loop() {
  curTime = millis();
  button_Up.loop();
  button_Ok.loop();
  button_Dn.loop();
  
  if(edit_sec || edit_pow){
    if (button_Up.isPressed()) {
      // long_btn_Up >0 mean the long press during eddit
      if(long_btn_Up == 0) long_btn_Up = curTime;
      if (curTime > (long_btn_Up + WAIT)) {
        btn_click(button_Up);
      }
    } else {
      long_btn_Up  = 0;
    }
    
    if (button_Dn.isPressed()) {
      // long_btn_Dn >0 mean the long press during edit
      if(long_btn_Dn == 0) long_btn_Dn = curTime;
      if (curTime > (long_btn_Dn + WAIT)) {
        btn_click(button_Dn);
      }
    } else {
      long_btn_Dn  = 0;
    }
  }
  
  if (started) {
    duration_sec = duration_sec - (float)(curTime - start_time) / 1000.00;
    start_time = curTime;
    if (duration_sec <= 0.0){
      start_time = 0;
      duration_sec = dur_def;
      started = false;
      // set PIN off
      digitalWrite(POWER_RELAY_PIN, LOW); 
      show_box();
    }
  }

  // read power meter data
  //volt = powerMeter.getVoltage();
  //amper = powerMeter.getCurrent();
  //  power = powerMeter.getPower();
  
  show_values();
  delay(10);  // 10 ms
}

void btn_handler(Button2& btn) {
  // we don't handle the long press under edit mode here
  // please see function in main loop
  if(edit_sec || edit_pow) return;
  
  if (btn == button_Up) {
    sec = true;
    edit_sec = true;
    old_dur = 0;
  } else {
    sec = false;
    edit_pow = true;
    old_pow = 0;
  }
  show_display();
  show_box();
}

void btn_click(Button2& btn) {
  // if we a re not in EDIT mode
    if(!edit_sec and !edit_pow){     
      if(!started){
        sec = !sec;    
      }
    // EDIT mode
    } else {
      // adjust seconds
      if(edit_sec){
        if(btn == button_Up){
          // increase to limit
          duration_sec = duration_sec + 0.05;
          if (duration_sec >= DUR_MAX)  duration_sec = DUR_MAX ;
        } else {
          // decrease to limit
          duration_sec = duration_sec - 0.05;
          if(duration_sec <= DUR_MIN) duration_sec = DUR_MIN ;
        }
      // adjust power
      } else {
        // edit power
        if(btn == button_Up){
          // increase to limit
          power = power + 5;
          if (power >= POW_MAX)  power = POW_MAX ;
        } else {
          // decrease to limit
          power = power - 5;
          if (power <= POW_MIN)  power = POW_MIN ;
        }        
      }
    }
    show_box();
} 

void btnOk_clock(Button2& btn) {
  if (!edit_sec && !edit_pow) {
    started = !started;
    if (sec){
      if (started) {
        start_time = curTime;
        // set PIN on
        digitalWrite(POWER_RELAY_PIN, HIGH); 
      } else {
        start_time = 0;
        duration_sec = dur_def;
        // set PIN off
        digitalWrite(POWER_RELAY_PIN, LOW); 
      }  
    } else {
      // right now no action - we don't calculate the power
      started = false;
    }
  } else {
    // go out from edit mode
    if (edit_sec) {
      edit_sec = false;
      dur_def = duration_sec;
      // store parater in eeprom
      save_to_eeprom(0, dur_def);
    }
    if (edit_pow) {
      edit_pow = false;
      pow_def = power;
    }
  }
  show_display();
  old_dur = -1;
  old_pow = -1;
  old_amp = -1;
  old_volt = -1;
  show_box();
}
    

// display resolution 160x128
void show_display(){
  tft.fillScreen(TFT_BLACK);
  int cx = 145, cy = 12, i = 6;
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.fillTriangle(cx, cy - i, cx - i, cy + i, cx + i, cy + i, TFT_YELLOW);
  cy = 108;
  tft.fillTriangle(cx, cy + i, cx + i, cy - i, cx - i, cy - i, TFT_YELLOW);
  if (!edit_sec && !edit_pow) {
    tft.setTextColor(TFT_BLUE);
    tft.setCursor(5,10,1);
    tft.print("doubleclick to edit s");
    tft.setCursor(90,100,1);
    tft.print("edit W");
  } else {
    tft.setTextColor(TFT_RED);
    tft.drawString(" EDIT mode ", 25, 5, 2);
  }
}

void show_values(){
  tft.setTextColor(TFT_GREEN, TFT_BLACK);

  if(celsius != old_celsius){
    old_celsius = celsius;
    if(celsius <= TEMP_MAX) {
      // to run once when alarm off
      if (tAlarm == true){
          old_dur = old_amp = old_pow = old_amp = 0;
          tft.setTextColor(TFT_GREEN, TFT_BLACK);
          show_display();
          show_box();
      }
      tAlarm = false;
      tft.setCursor(75, 110, 2);
      tft.print(celsius);  tft.print(" C");
    } else {
      tAlarm = true;
      tft.fillScreen(TFT_RED);
      tft.setTextColor(TFT_YELLOW, TFT_RED);
      tft.setCursor(10, 50, 7);
      tft.print(celsius);
      return;
    }
  }

  // display only when changed
  if(duration_sec != old_dur){
     old_dur=duration_sec;
     tft.setCursor(60, 30, 4);
     tft.print(duration_sec); tft.print(" s");
  }
  
  // display only when changed
  if(power != old_pow){
    old_pow = power;
    tft.setCursor(60, 64, 4);
    tft.print(power);  tft.print(" W"); 
  }

  if(volt != old_volt){
    old_volt = volt;
    tft.setCursor(10, 95, 2);
    tft.print(volt);  tft.print(" V");
  }

  if(amper != old_amp){
    old_amp = amper;
    tft.setCursor(10, 110, 2);
    tft.print(amper);  tft.print(" A");
  }
}

void show_box(){
  unsigned int rect_pos, endX = 93;
  rect_pos = (sec) ? 25 : 58;
  // clear all boxes
  tft.drawRect(52, 25, endX, 33, TFT_BLACK);
  tft.drawRect(52, 58, endX, 33, TFT_BLACK);
  // diplay new
  tft.drawRect(52, rect_pos, endX, 33, TFT_GREEN);
  tft.setTextColor(TFT_RED);
  // clear text
  tft.fillRect(1, 24, 50, 60, TFT_BLACK);
  // display new
  String st = (started) ? "Stop" : "Start";
  tft.drawString(st, 5, rect_pos+13, 2);
}

float read_from_eeprom(unsigned int address) { //(address)
  float val;
  for (byte i = 0; i < sizeof(val); i++) { // size of config is 4
    reinterpret_cast<byte*>(&val)[i] = EEPROM.read(address + i);
  }
  return val;
}

void save_to_eeprom(unsigned int address, float val) { // (address, value)
  for (byte i = 0; i < sizeof(val); i++) { // size of config is 4
    EEPROM.write(address + i, reinterpret_cast<byte*>(&val)[i]);
  }
  EEPROM.commit();
}
