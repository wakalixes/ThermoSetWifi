#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <EEPROM.h>

#define PIN_CSN          16
#define PIN_DATA         14
#define PIN_CLK          12
#define DAC_MINVAL       0
#define DAC_MAXVAL       16384-1
#define DAC_WAIT         1
#define TEMP_RESET       20.0f
#define LINEARIZE_K      (float)-87.5
#define LINEARIZE_D      (float)6400
#define PARSE_PERIOD     60*60
#define NTP_PERIOD       60*60
#define DAC_PERIOD       1
#define MAX_SRV_CLIENTS  2
#define EEPROM_ADDR_CONF 0

const char* CMD_HIGH       =         "on";
const char* CMD_LOW        =        "off";
const char* CMD_SEP        =          " ";
const char* CMD_START      =          "#";
const char* CMD_GETSTATUS  =     "status";
const char* CMD_RESET      =      "reset";
const char* CMD_RESETWIFI  =  "resetwifi";
const char* CMD_SETDAC     =     "setdac";
const char* CMD_CONNECT    =    "connect";
const char* CMD_DISCONNECT = "disconnect";
const char* CMD_SETTEMP    =    "settemp";
const char* CMD_PARSETEMP  =  "parsetemp";
const char* CMD_UPDATETEMP = "updatetemp";
const char* CMD_GETLOC     =     "getloc";
const char* CMD_SETLOC     =     "setloc";
const char* CMD_GETTIME    =    "gettime";
const char* CMD_UPDATETIME = "updatetime";

#include "Config.h"

// locIDs:
//    Stuttgart    2825297
//    Innsbruck    6949518
//    Christchurch 2192362
const unsigned int localPort = 2390;
const int NTP_PACKET_SIZE  = 48; // NTP time stamp is in the first 48 bytes of the message
const char* NTP_SERVER     = "pool.ntp.org";

int dacval;
long oldparsetime, oldntptime, olddactime;
float tempextval;
time_t tempexttime;
String inputString     = "";
String commandString   = "";
String valueString     = "";
char formatString[20];
boolean stringComplete = false;
boolean commandAck     = false;

WiFiClient client;
WiFiServer server(23);   // telnet server
WiFiClient serverClients[MAX_SRV_CLIENTS];
WiFiUDP udp;             // for NTP client
byte packetBuffer[NTP_PACKET_SIZE];

struct {
  long parseInterval;
  char* locationID;
  char* locationName;
} thermoConfig;

void setup() {
  pinMode(PIN_CSN, OUTPUT);
  pinMode(PIN_DATA, OUTPUT);
  pinMode(PIN_CLK, OUTPUT);
  loadConfig();
  Serial.begin(115200);
  initWifi();
  server.begin();
  server.setNoDelay(true);
  udp.begin(localPort);
  resetDAC();
  setTemp(TEMP_RESET);
  oldparsetime = millis();
}

void loop() {
  serialEvent();
  serialCommands();
  telnetEvent();
  telnetCommands();
  delay(1);
  if ((millis()-oldparsetime)>thermoConfig.parseInterval*1000) {
    oldparsetime = millis();
    parseTemp();
    setTemp(tempextval);
  }
  if ((millis()-oldntptime)>NTP_PERIOD*1000) {
    oldntptime = millis();
    syncTimeNTP();
  }
  if ((millis()-olddactime)>DAC_PERIOD*1000) {
    olddactime = millis();
    outputDACval(dacval);
  }
}

void resetDAC() {
  dacval = 4200;
  setDACval(dacval);
}

int convTempToDAC(float temp) {
  int val;
  val = LINEARIZE_K*temp+LINEARIZE_D;
  return val;
}

void clockDAC() {
  delayMicroseconds(DAC_WAIT);
  digitalWrite(PIN_CLK, HIGH);
  delayMicroseconds(DAC_WAIT);
  digitalWrite(PIN_CLK, LOW);
}
    
void setDACval(int val) {
  if (val<DAC_MINVAL) { val = 0; }
  if (val>DAC_MAXVAL) { val = DAC_MAXVAL; }
  consolePrint("setting DAC value to ");
  consolePrintLn(val);
  outputDACval(val);
}

void outputDACval(int val) {
  noInterrupts();
  // init pin states
  digitalWrite(PIN_CSN, LOW);
  digitalWrite(PIN_DATA, LOW);
  digitalWrite(PIN_CLK, LOW);
  // send 14 data bits
  for (int i=13;i>=0;i--) {
    if ((val&((int)1<<i))>0) digitalWrite(PIN_DATA, HIGH);
    else digitalWrite(PIN_DATA, LOW);
    clockDAC();
  }
  // send two dummy bits
  clockDAC();
  clockDAC();
  // load data into DAC shift register
  digitalWrite(PIN_CSN, HIGH);
  delayMicroseconds(DAC_WAIT);
  digitalWrite(PIN_CSN, LOW);
  interrupts();
}

void setTemp(float temp) {
  consolePrint(F("setting temp to "));
  consolePrint(temp);
  consolePrintLn(F("°C"));
  dacval = convTempToDAC(temp);
  setDACval(dacval);
}

void parseTemp() {
  int ret = 0;
  ret += connectHTTP();
  ret += requestHTTP();
  tempextval = parseJSON();
  disconnectHTTP();
  if (ret!=0) {
    consolePrintLn(F("Error parsing temperature!"));
    tempextval = TEMP_RESET;
  } else {
    // print values
    getLocation();
    consolePrint(F("parsed time: "));
    consolePrintTime(tempexttime);
    consolePrint(F("parsed temperature: "));
    consolePrint(tempextval);
    consolePrintLn(F("°C"));
  }
}

void initWifi() {
  consolePrintLn("");
  consolePrint(F("Connecting to "));
  consolePrintLn(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    consolePrint(".");
  } 
  consolePrintLn("");
  consolePrintLn(F("WiFi connected"));  
  consolePrint(F("IP address: "));
  IPAddress ip = WiFi.localIP();
  sprintf(formatString, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  consolePrintLn(formatString);
}

void stopWifi() {
  consolePrintLn(F("Disconnecting WiFi ..."));
  WiFi.disconnect();
}

int connectHTTP() {
  consolePrintLn(F("Connecting..."));
  // Connect to HTTP server
  client.setTimeout(10000);
  if (!client.connect(URL_SERVER, 80)) {
    consolePrintLn(F("Connection failed"));
    return 1;
  }
  consolePrintLn(F("Connected!"));    
  return 0;
}

void disconnectHTTP() {
  client.stop();
}

int requestHTTP() {
  char urlBase[60];
  // send HTTP request
  client.print(F("GET "));
  sprintf(urlBase, URL_BASE, thermoConfig.locationID);
  client.print(urlBase);
  client.print(URL_API_KEY);
  client.println(F(" HTTP/1.0"));
  client.print(F("Host: "));
  client.println(URL_SERVER);
  client.println(F("Connection: close"));
  if (client.println() == 0) {
    consolePrintLn(F("Failed to send request"));
    return 1;
  }
  // check HTTP status
  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  if (strcmp(status, "HTTP/1.1 200 OK") != 0) {
    consolePrint(F("Unexpected response: "));
    consolePrintLn(status);
    return 1;
  }
  // skip HTTP headers
  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    consolePrintLn(F("Invalid response"));
    return 1;
  }
  return 0;
}

float parseJSON() {
  float tempext;
  // allocate JsonBuffer
  // use arduinojson.org/assistant to compute the capacity.
  const size_t capacity = JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(1) + 2*JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(5) + JSON_OBJECT_SIZE(6) + JSON_OBJECT_SIZE(12) + 390;
  DynamicJsonBuffer jsonBuffer(capacity);
  // parse JSON object
  JsonObject& root = jsonBuffer.parseObject(client);
  if (!root.success()) {
    consolePrintLn(F("Parsing failed!"));
    return TEMP_RESET;
  }
  // extract values
  tempexttime = root["dt"];
  tempext = root["main"]["temp"];
  thermoConfig.locationName = strdup(root["name"]);
  return tempext;
}

void getStatus() {
  getLocation();
  consolePrint(F("time: "));
  consolePrintTime(tempexttime);
  consolePrint(F("temperature: "));
  consolePrint(tempextval);
  consolePrintLn(F("°C"));
  consolePrint(F("DAC value: "));
  consolePrintLn(dacval);
}

void getLocation() {
  consolePrint(F("locid: "));
  consolePrint(thermoConfig.locationID);
  consolePrint(F(" - "));
  consolePrintLn(thermoConfig.locationName);
}

void setLocation(char* loc) {
  thermoConfig.locationID = loc;
  consolePrint(F("new locid: "));
  consolePrintLn(thermoConfig.locationID);
}

template <class T> void consolePrint(T output) {
  uint8_t i;
  if (Serial) Serial.print(output);
  for(i = 0; i < MAX_SRV_CLIENTS; i++) {
    if (serverClients[i] && serverClients[i].connected()) {
      serverClients[i].print(output);
      delay(1);
    }
  }
}

template <class T> void consolePrint(T output, int type) {
  uint8_t i;
  if (Serial) Serial.print(atoi(output), type);
  for(i = 0; i < MAX_SRV_CLIENTS; i++) {
    if (serverClients[i] && serverClients[i].connected()) {
      serverClients[i].print(atoi(output), type);
      delay(1);
    }
  }
}

template <class T> void consolePrintLn(T output) {
  consolePrint(output);
  consolePrint("\n");
}

template <class T> void consolePrintLn(T output, int type) {
  consolePrint(output, type);
  consolePrint("\n");
}

void consolePrintTime(time_t t) {
  sprintf(formatString, "%d", t);
  consolePrint(formatString);
  consolePrint(F(" - UTC "));
  sprintf(formatString, "%02d.%02d.%04d %02d:%02d:%02d", day(t), month(t), year(t), hour(t), minute(t), second(t));
  consolePrintLn(formatString);
}

void procCommands() {
  commandAck = false;
  if (inputString.startsWith(CMD_RESET)) {
    resetDAC();
    thermoConfig.parseInterval = PARSE_PERIOD;
    thermoConfig.locationID = LOC_ID;
    saveConfig();
    setTemp(TEMP_RESET);
    commandAck = true;
  }    
  if (inputString.startsWith(CMD_RESETWIFI)) {
    stopWifi();
    initWifi();
    commandAck = true;
  }
  if (inputString.startsWith(CMD_SETDAC)) {
    valueString = inputString.substring(inputString.indexOf(CMD_SEP)+1,inputString.length());
    dacval = valueString.toInt();
    setDACval(dacval);
    commandAck = true;
  }
  if (inputString.startsWith(CMD_CONNECT)) {
    initWifi();
    commandAck = true;
  }
  if (inputString.startsWith(CMD_SETTEMP)) {
    valueString = inputString.substring(inputString.indexOf(CMD_SEP)+1,inputString.length());
    if (valueString.length()==0) {
      setTemp(tempextval);
    } else {
      setTemp(strtof(valueString.c_str(), NULL));
    }
    commandAck = true;
  }
  if (inputString.startsWith(CMD_PARSETEMP)) {
    parseTemp();
    commandAck = true;
  }
  if (inputString.startsWith(CMD_UPDATETEMP)) {
    parseTemp();
    setTemp(tempextval);
    commandAck = true;
  }
  if (inputString.startsWith(CMD_GETSTATUS)) {
    getStatus();
    commandAck = true;
  }
  if (inputString.startsWith(CMD_GETLOC)) {
    getLocation();
    commandAck = true;
  }
  if (inputString.startsWith(CMD_SETLOC)) {
    valueString = inputString.substring(inputString.indexOf(CMD_SEP)+1,inputString.length());
    setLocation(&valueString[0u]);
    saveConfig();
    commandAck = true;
  }
  if (inputString.startsWith(CMD_GETTIME)) {
    consolePrint(F("current time: "));
    consolePrintTime(now());
    commandAck = true;
  }
  if (inputString.startsWith(CMD_UPDATETIME)) {
    syncTimeNTP();
    commandAck = true;
  }
}

void serialEvent() {
  while (Serial.available()) {
    char inChar = (char)Serial.read(); 
    inputString += inChar;
    if (inChar == '\n') {
      inputString.replace("\n", "");
      inputString.replace("\r", "");
      stringComplete = true;
    } 
  }
}

void serialCommands() {
  if (stringComplete) {
    procCommands();
    Serial.println(inputString);
    if (commandAck) Serial.println("ACK");
    else Serial.println("ERR");
    Serial.println();
    inputString = "";
    stringComplete = false;
  }
}

void telnetEvent() {
  uint8_t i;
  //check if there are any new clients
  if (server.hasClient()){
    for(i = 0; i < MAX_SRV_CLIENTS; i++){
      //find free/disconnected spot
      if (!serverClients[i] || !serverClients[i].connected()){
        if(serverClients[i]) serverClients[i].stop();
        serverClients[i] = server.available();
        Serial.print("New client: "); 
        Serial.println(i);
        inputString = "";
        stringComplete = false;
        continue;
      }
    }
    //no free/disconnected spot so reject
    WiFiClient serverClient = server.available();
    serverClient.stop();
  }
  //check clients for data
  for(i = 0; i < MAX_SRV_CLIENTS; i++){
    if (serverClients[i] && serverClients[i].connected()){
      if(serverClients[i].available()){
        //get data from the telnet client and push it to the UART
        while(serverClients[i].available()) {
          char inChar = serverClients[i].read(); 
          inputString += inChar;
          if (inChar == '\n') {
            inputString = inputString.substring(inputString.indexOf(CMD_START)+1,inputString.length());
            inputString.replace("\n", "");
            inputString.replace("\r", "");
            stringComplete = true;
          } 
        }
      }
    }
  }
}

void telnetCommands() {
  uint8_t i;
  if (stringComplete) {
    procCommands();
    consolePrintLn(inputString);
    if (commandAck) consolePrintLn("ACK");
    else consolePrintLn("ERR");
    consolePrintLn("");
    inputString = "";
    stringComplete = false;
  }
}

void syncTimeNTP() {
  time_t currentTime;
  IPAddress timeServerIP;
  WiFi.hostByName(NTP_SERVER, timeServerIP); 
  sendNTPpacket(timeServerIP);
  delay(1000);
  int cb = udp.parsePacket();
  consolePrint("paket size ");
  consolePrintLn(cb);
  if (!cb) {
    consolePrintLn(F("no NTP packet received"));
    return;
  } else {
    consolePrint(F("NTP packet received, length: "));
    consolePrintLn(cb);
    udp.read(packetBuffer, NTP_PACKET_SIZE); 
    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
    // this is NTP time (seconds since Jan 1 1900):
    unsigned long timeNTP = highWord << 16 | lowWord;
    // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
    const unsigned long unixOffset = 2208988800UL;
    currentTime = timeNTP - unixOffset;
    consolePrint(F("local time offset: "));
    consolePrintLn(now()-currentTime);
    consolePrint(F("new time: "));
    consolePrintTime(currentTime);
    setTime(currentTime);
  }
}

unsigned long sendNTPpacket(IPAddress& address) {
  consolePrintLn(F("sending NTP packet..."));
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;            // Stratum, or type of clock
  packetBuffer[2] = 6;            // Polling Interval
  packetBuffer[3] = 0xEC;         // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;
  udp.beginPacket(address, 123);  // NTP requests on port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}

void loadConfig() {
  EEPROM_readAnything(0, thermoConfig);
  thermoConfig.parseInterval = PARSE_PERIOD;
  thermoConfig.locationID = LOC_ID;
}

void saveConfig() {
  EEPROM_writeAnything(0, thermoConfig);
}

template <class T> int EEPROM_writeAnything(int ee, const T& value)
{
  const byte* p = (const byte*)(const void*)&value;
  unsigned int i;
  for (i = 0; i < sizeof(value); i++)
    EEPROM.write(ee++, *p++);
  return i;
}

template <class T> int EEPROM_readAnything(int ee, T& value)
{
  byte* p = (byte*)(void*)&value;
  unsigned int i;
  for (i = 0; i < sizeof(value); i++)
    *p++ = EEPROM.read(ee++);
  return i;
}
