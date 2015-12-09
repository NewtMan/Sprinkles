#include <EEPROM.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <Time.h>
#include <Timezone.h>

#define WEBDUINO_FAVICON_DATA ""
#include <WebServer.h>

#define VERSION   "1.0"

// TimeZone stuff
TimeChangeRule myDST = {"PDT", Second, Sun, Mar, 2, -420};    //Pacific Daylight time = UTC - 7 hours
TimeChangeRule mySTD = {"PST", First, Sun, Nov, 2, -480};     //Pacific Standard time = UTC - 8 hours
Timezone myTZ(myDST, mySTD);

#define MAX_ZONE_NAME   8

typedef struct {
  boolean on;                         // ON/OFF state
  byte pin;                           // Control pin (digital)
  char name[MAX_ZONE_NAME];           // Zone name
} WaterZone; 

const byte NUM_ZONES = 4;
WaterZone zoneList[NUM_ZONES] = {     // Watering zones
  { false, 8, "NORTH" },
  { false, 6, "SOUTH" },
  { false, 7, "BACK" },
  { false, 5, "FRONT" }
};

typedef enum {
  IDLE_MODE = 0,               // nothing going on
  MANUAL_MODE,                 // operating manually
  AUTO_MODE,                   // operating based upon schedule
} SystemMode;

typedef struct {
  SystemMode mode;             // current operating mode
} SystemStatus;

SystemStatus sysStatus;

// Zone log calculations
#define ZONE_LOG_LENGTH         ((NUM_ZONES * 2 * 3) + 1)     // 2 entries/zone cycle (ON & OFF) and 3 complete cycles, plus the "throwaway"
#define ZONE_LOG_ELEM_SIZE      (sizeof(time_t) + sizeof(byte) + sizeof(boolean))

// EEPROM addresses
#define EEADDR_WATER_CYCLE      0
#define EEADDR_ZONE_LOG_BASE    100
#define EEADDR_ZONE_LOG_START   (EEADDR_ZONE_LOG_BASE + (2 *sizeof(int)))
#define EEADDR_ZONE_LOG_END     (EEADDR_ZONE_LOG_START + (ZONE_LOG_LENGTH * ZONE_LOG_ELEM_SIZE))


// HTML strings/tools
P(htmlDocStyle) = "<head><style>td,th{padding:3px;text-align:center;}.on{background-color:#66cc66}</style></head>\n";
P(htmlTableHead) = "<table border=\"1\"><tr>\n";
P(htmlNextRow) = "</tr><tr>\n";
P(htmlTDHead) = "<td>\n";
P(htmlTDTail) = "</td>\n";
P(htmlTHHead) = "<th>\n";
P(htmlTHTail) = "</th>\n";
P(htmlTableTail) = "</tr></table>\n";
P(htmlFormTail) = "</form>\n";
P(htmlSelectTail) = "</select></td>\n";
P(htmlPara) = "<p>\n";

class ZoneLog {
  int head, tail;

public:
  void begin() {

    // Initialize pointers
    EEPROM.get(EEADDR_ZONE_LOG_BASE, head);
    EEPROM.get(EEADDR_ZONE_LOG_BASE + sizeof(int), tail);
  }

  // Clear out the log
  void clear() {
    head = tail = EEADDR_ZONE_LOG_START;
    EEPROM.put(EEADDR_ZONE_LOG_BASE, head);
    EEPROM.put(EEADDR_ZONE_LOG_BASE + sizeof(int), tail);
  }

  // Add log entry
  void add(byte zone, boolean state) {
    
    time_t tm = myTZ.toLocal(now());    
    EEPROM.put(head, tm);
    head += sizeof(tm);
    EEPROM.put(head, zone);
    head += sizeof(zone);
    EEPROM.put(head, state);
    head += sizeof(state);

    if (head >= EEADDR_ZONE_LOG_END) {
      head = EEADDR_ZONE_LOG_START;
    }
    if (head == tail) {
      tail = head + ZONE_LOG_ELEM_SIZE;
      if (tail >= EEADDR_ZONE_LOG_END) {
        tail = EEADDR_ZONE_LOG_START;
      }
    }
    EEPROM.put(EEADDR_ZONE_LOG_BASE, head);
    EEPROM.put(EEADDR_ZONE_LOG_BASE + sizeof(int), tail);
  }

  // Dump entire log (in HTML format) in reverse order
  void dumpHTML(WebServer *server) {
    time_t tm;
    byte zone;
    boolean state;

    // Header row
    server->printP(htmlTHHead);
    server->print("Time");
    server->printP(htmlTHTail);
    server->printP(htmlTHHead);
    server->print("Zone");
    server->printP(htmlTHTail);
    server->printP(htmlTHHead);
    server->print("State");
    server->printP(htmlTHTail);
    server->printP(htmlNextRow);

    if (head == tail) {
      server->print("<td colspan=\"3\">");
      server->print("-- empty --");
      server->printP(htmlTDTail);
    } else {
      int addr = head;
      while (addr != tail) {
        addr -= ZONE_LOG_ELEM_SIZE;
        if (addr < EEADDR_ZONE_LOG_START) {
          addr = EEADDR_ZONE_LOG_END - ZONE_LOG_ELEM_SIZE;
        }
                
        EEPROM.get(addr, tm);
        addr += sizeof(tm);
        server->printP(htmlTDHead);
        htmlTimeStr(tm, server);
        server->printP(htmlTDTail);
  
        EEPROM.get(addr, zone);
        addr += sizeof(zone);
        server->printP(htmlTDHead);
        server->print(zone + 1);
        server->printP(htmlTDTail);
        
        EEPROM.get(addr, state);
        addr += sizeof(state);
        server->printP(htmlTDHead);
        server->print(state ? "ON" : "OFF");
        server->printP(htmlTDTail);
        server->printP(htmlNextRow);

        addr -= ZONE_LOG_ELEM_SIZE;
      }
    }
  }
};

ZoneLog zoneLog;

typedef struct {
  int startTime;                      // Minutes from start of day
  byte zone_duration[NUM_ZONES];      // "ON" time (minutes)
  byte activeDays;                    // Active days of week (bitfield)
  bool enabled;                       // Cycle is enabled (yes/no)
} WaterCycle;

WaterCycle curCycle;

typedef struct {
  time_t startTime;
  time_t zoneStopTime[NUM_ZONES];
} Event;

#define NUM_SCHED_EVENTS    1

class Schedule {
  Event eventList[NUM_SCHED_EVENTS];

public:

  // Build execution schedule from current watering cycle
  void build(WaterCycle *cycle) {

    // Clear out the schedule
    memset(eventList, 0, sizeof(eventList));

    // If at least one day is active in cycle...
    if (cycle->activeDays != 0) {

      // Find start of first event following current time
      time_t localTime = myTZ.toLocal(now());
      time_t eventStart = previousMidnight(localTime) + (cycle->startTime * SECS_PER_MIN);
      while (eventStart < localTime) {
        eventStart += SECS_PER_DAY;
      }

      //Create events to fill schedule
      int i = 0;
      while (i < NUM_SCHED_EVENTS) {
        if (cycle->activeDays & (1 << weekday(eventStart))) {
          time_t t = eventList[i].startTime = eventStart;
          for (int j = 0; j < NUM_ZONES; j++) {
            eventList[i].zoneStopTime[j] = t + (cycle->zone_duration[j] * SECS_PER_MIN);
            t = eventList[i].zoneStopTime[j];
          }
          i++;
        }
        eventStart += SECS_PER_DAY;
      }
    }
  }

  // Scan schedule for an active watering zone
  int activeZone(WaterCycle *cycle) {

    // If watering cycle is enabled...
    if (cycle->enabled) {
          
      time_t localTime = myTZ.toLocal(now());
      for (int i = 0; i < NUM_SCHED_EVENTS; i++) {
        if (localTime >= eventList[i].startTime) {
          for (int j = 0; j < NUM_ZONES; j++) {
            if (localTime < eventList[i].zoneStopTime[j]) {
              return (j);
            }
          }
        }
      }
    }

    // No event/zone is active
    return (-1);
  }

  // return the next watering event on the schedule
  time_t nextRun(void) {
    return eventList[0].startTime;
  }
};

Schedule execSchedule;

// zoneOn() - Turn on a watering zone (only one ON at a time)
void zoneOn(byte zone) {
  if (! zoneList[zone].on) {
    for (int i = 0; i < NUM_ZONES; i++) {
      zoneOff(i);
    }
    digitalWrite(zoneList[zone].pin, HIGH);
    zoneList[zone].on = true;
    zoneLog.add(zone, true);
  }
}

// zoneOff() - Turn off a watering zone
void zoneOff(byte zone) {
  if (zoneList[zone].on) {
    zoneList[zone].on = false;
    digitalWrite(zoneList[zone].pin, LOW);
    zoneLog.add(zone, false);
  }
}

// allZonesOff() - Turn all watering zones off
void allZonesOff() {
  for (byte i = 0; i < NUM_ZONES; i++) {
    zoneOff(i);
  }
}

// MAC address is arbitrary
byte mac[] = {
  0x00, 0xAA, 0xBB, 0xCC, 0xDE, 0x02
};

// UDP stuff
const byte NTP_PACKET_SIZE = 48;      // NTP time stamp is in the first 48 bytes of the message
byte ntpPacket[NTP_PACKET_SIZE];      // buffer to hold incoming and outgoing packets
EthernetUDP Udp;                      // A UDP instance to let us send and receive packets over UDP
char timeServer[] = "10.0.0.1";       // local NTP server

// Initialize the Webduino server (port 80)
WebServer webserver("", 80);

void htmlZoneHeaders(WebServer &server) {
  for (int i = 0; i < NUM_ZONES; i++) {
    server.printP(htmlTHHead);
    server.print(zoneList[i].name);
    server.printP(PSTR("<br>(# "));
    server.print(intToStr(i + 1, 0));
    server.printP(PSTR(")</th>"));
  }
}

void htmlTimeStr(time_t t, WebServer *server) {
  server->print(dayStr(weekday(t)));
  server->printP(PSTR(" "));
  server->print(intToStr(month(t), 2));
  server->printP(PSTR("/"));
  server->print(intToStr(day(t), 2));
  server->printP(PSTR("/"));
  server->print(intToStr(year(t) - 2000, 2));
  server->printP(PSTR(" "));
  server->print(intToStr(hourFormat12(t), 2));
  server->printP(PSTR(":"));
  server->print(intToStr(minute(t), 2));
  server->printP(PSTR(" "));
  server->print(isAM(t) ? "AM " : "PM ");
  server->print(myTZ.locIsDST(t) ? myDST.abbrev : mySTD.abbrev) ;
}

// Webserver callback - "Home" page
void homeCmd(WebServer &server, WebServer::ConnectionType type, char *, bool) {
  
  // HTTP OK header
  server.httpSuccess();

  // If a HEAD request, do nothing else
  if (type != WebServer::HEAD) {
    server.printP(htmlDocStyle);
    server.printP(PSTR("<h1>Sprinkler Controller</h1>"));

    // Time status
    server.printP(htmlTableHead);
    server.printP(PSTR("<th>Time</th><td>"));
    if (timeStatus() == timeNotSet) {
      server.printP(PSTR("Not Set"));
    } else {
      time_t localTime = myTZ.toLocal(now());
      htmlTimeStr(localTime, &server);
      if (timeStatus() == timeNeedsSync) {
        server.printP(PSTR(" (STALE)"));
      }
    }
    server.printP(htmlTDTail);

    server.printP(htmlNextRow);
    server.printP(PSTR("<th>Next Cycle</th>"));
    server.printP(htmlTDHead);
    if (curCycle.enabled) {
      if (execSchedule.nextRun() == 0) {
        server.printP(PSTR("<em>None</em>"));
      } else {
        htmlTimeStr(execSchedule.nextRun(), &server);
      }
    } else {
      server.printP(PSTR("<em>Disabled</em>"));
    }
    server.printP(htmlTDTail);
    server.printP(htmlNextRow);
    server.printP(PSTR("<th>System Status</th>"));
    switch (sysStatus.mode) {
      case IDLE_MODE:
        server.printP(PSTR("<td>IDLE</td>"));
        break;
      case MANUAL_MODE:
        server.printP(PSTR("<td>MANUAL</td>"));
        break;
      case AUTO_MODE:
        server.printP(PSTR("<td>AUTO</td>"));
        break;
    }

    server.printP(htmlTableTail);
    server.printP(htmlPara);

    // Zone status and manual controls
    server.printP(PSTR("<h3>Zone Status</h3>"));
    server.printP(PSTR("<form action=\"manual.html\">"));
    server.printP(htmlTableHead);
    server.printP(PSTR("<th rowspan=\"2\"><input type=\"submit\" value=\"ALL OFF\" name=\"ALL\"></th>"));
    htmlZoneHeaders(server);
    server.printP(htmlNextRow);
    for (int i = 0; i < NUM_ZONES; i++) {
      const char *btnVal;
      if (zoneList[i].on) {
        server.printP(PSTR("<td class=\"on\">ON"));
        btnVal = "OFF";
      } else {
        server.printP(PSTR("<td class=\"off\">OFF"));
        btnVal = "ON";
      }
      server.printP(PSTR("<br><input type=\"submit\" value=\""));
      server.print(btnVal);
      server.printP(PSTR("\" name=\"Z"));
      server.print(intToStr(i, 0));
      server.printP(PSTR("\"></td>"));
    }
    server.printP(htmlTableTail);
    server.printP(htmlFormTail);

    // Watering cycle
    server.printP(PSTR("<h3>Watering Cycle</h3>"));
    server.printP(PSTR("<form action=\"cycle.html\">"));
    server.printP(htmlTableHead);
    server.printP(PSTR("<th>Enabled</th>"));
    server.printP(PSTR("<th>Start<br>Time</th>"));
    htmlZoneHeaders(server);
    server.printP(PSTR("<th colspan=\"7\">WATERING DAYS</th>"));
    server.printP(htmlNextRow);
    server.printP(PSTR("<td><input type=\"checkbox\" "));
    if (curCycle.enabled) {
      server.printP(PSTR("checked "));
    }
    server.printP(PSTR("name=\"E\"></td>"));
    server.printP(PSTR("<td><select name=\"time\">"));
    for (int j = 0; j < 24; j++) {
      int hr = (j < 13) ? j : j - 12;
      const char *tail = (j < 12) ? "AM" : "PM";
      server.printP(PSTR("<option "));
      if (curCycle.startTime == j * 60) {
        server.printP(PSTR("selected "));
      }
      server.printP(PSTR("value=\""));
      server.print(intToStr(j * 60, 0));
      server.printP(PSTR("\">"));
      server.print(intToStr(hr, 2));
      server.printP(PSTR(":00 "));
      server.print(tail);
    }
    server.printP(htmlSelectTail);
    for (int j = 0; j < NUM_ZONES; j++) {
      server.printP(PSTR("<td><select name=\"Z"));
      server.print(intToStr(j, 0));
      server.printP(PSTR("\">"));
      for (int k = 0; k < 35; k += 5) {
        server.printP(PSTR("<option "));
        if (curCycle.zone_duration[j] == k) {
          server.printP(PSTR("selected "));
        }
        server.printP(PSTR("value=\""));
        server.print(intToStr(k, 0));
        server.printP(PSTR("\">"));
        server.print(intToStr(k, 2));
        server.printP(PSTR(" mins"));
      }
      server.printP(htmlSelectTail);
    }

    server.printP(htmlTDHead);
    for (int j = 0; j < DAYS_PER_WEEK; j++) {
      server.printP(PSTR("<input type=\"checkbox\" "));
      if (curCycle.activeDays & (1 << j + 1)) {
        server.printP(PSTR("checked "));
      }
      server.printP(PSTR("name=\"D"));
      server.print(intToStr(j + 1, 0));
      server.printP(PSTR("\">"));
      server.print(dayShortStr(j + 1));
    }
    server.printP(htmlTDTail);

    server.printP(htmlTableTail);
    server.printP(htmlPara);
    server.printP(PSTR("<input type=\"submit\" value=\"SAVE\">"));
    server.printP(htmlFormTail);

    // Activity Log
    server.printP(PSTR("<h3>Activity Log</h3>"));
    server.printP(PSTR("<form action=\"log.html\">"));
    server.printP(htmlTableHead);
    zoneLog.dumpHTML(&server);
    server.printP(htmlTableTail);
    server.printP(htmlPara);
    server.printP(PSTR("<input type=\"submit\" name=\"CLEAR\" value=\"CLEAR\">"));
    server.printP(htmlFormTail);
  }
}

#define LEN   10

// Webserver callback - Process "manual" controls
void manualCmd(WebServer &server, WebServer::ConnectionType type, char *tail, bool) {
  char pName[LEN];
  char pValue[LEN];
  
  // If a HEAD request, do nothing else
  if (type != WebServer::HEAD) {

    if (server.nextURLparam(&tail, pName, LEN, pValue, LEN) != URLPARAM_EOS) {
      if (pName[0] == 'Z') {
        int zone = pName[1] - '0';
        if (strcmp(pValue, "ON") == 0) {
          zoneOn(zone);
          sysStatus.mode = MANUAL_MODE;
        } else if (strcmp(pValue, "OFF") == 0) {
          zoneOff(zone);
          sysStatus.mode = IDLE_MODE;
        }
      } else if (strcmp(pName, "ALL") == 0) {
        allZonesOff();
        sysStatus.mode = IDLE_MODE;
      }
    }
  }
  server.httpSeeOther("index.html");    // redirect back to "home" page
}

// Webserver callback - Process watering cycle
void cycleCmd(WebServer &server, WebServer::ConnectionType type, char *tail, bool) {
  char pName[LEN];
  char pValue[LEN];
  
  // If a HEAD request, do nothing else
  if (type != WebServer::HEAD) {
    curCycle.activeDays = 0;    // Initialize attributes
    curCycle.enabled = false;
    
    while (strlen(tail)) {
      if (server.nextURLparam(&tail, pName, LEN, pValue, LEN) != URLPARAM_EOS) {
        if (strcmp(pName, "time") == 0) {
          curCycle.startTime = strToInt(pValue);
  
        } else if (pName[0] == 'Z') {
          int zone = pName[1] - '0';
          curCycle.zone_duration[zone] = strToInt(pValue);
          
        } else if (pName[0] == 'D') {
          int day = pName[1] - '0';
          curCycle.activeDays |= (1 << day);
          
        } else if (pName[0] == 'E') {
          curCycle.enabled = true;
        }
      }
    }
  }
  
  EEPROM.put(EEADDR_WATER_CYCLE, curCycle);     // update EEPROM

  // re-build execution schedule
  execSchedule.build(&curCycle);

  server.httpSeeOther("index.html");              // redirect back to "home" page
}

// Webserver callback - Process activity log
void logCmd(WebServer &server, WebServer::ConnectionType type, char *tail, bool) {
  char pName[LEN];
  char pValue[LEN];
  
  // If a HEAD request, do nothing else
  if (type != WebServer::HEAD) {
   
    while (strlen(tail)) {
      if (server.nextURLparam(&tail, pName, LEN, pValue, LEN) != URLPARAM_EOS) {
        if (strcmp(pName, "CLEAR") == 0) {
          zoneLog.clear();                       // Clear zone log
        } 
      }
    }
  }
  server.httpSeeOther("index.html");             // redirect back to "home" page
}
  
void setup() {

  // Come up in idle mode
  sysStatus.mode = IDLE_MODE;
   
  //initialize zone control pins
  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(zoneList[i].pin, OUTPUT);
  }
  
  allZonesOff();    // make sure all zones are off

  // Disable SD card on Ethernet shield
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);
  
  // Open serial communications
  // Serial.begin(9600);

  // Start ethernet connection using DHCP for IP address
  if (Ethernet.begin(mac) == 0) {
    // Serial.println("DHCP Failed!");
    while (1) ;
  }
  // Serial.print("IP is ");
  // Serial.println(Ethernet.localIP());

  // Setup UDP 
  Udp.begin(8888);      // arbitrary local port

  // Initialize Time to NTP server
  setSyncProvider(getNTPtime);
  setSyncInterval(SECS_PER_DAY);    // sync. with NTP once per day

  // Initialize web server
  webserver.setDefaultCommand(&homeCmd);
  webserver.addCommand("index.html", &homeCmd);
  webserver.addCommand("manual.html", &manualCmd);
  webserver.addCommand("cycle.html", &cycleCmd);
  webserver.addCommand("log.html", &logCmd);
  webserver.begin();
  
  // Initialize variables from EEPROM
  EEPROM.get(EEADDR_WATER_CYCLE, curCycle);

  // build schedule
  execSchedule.build(&curCycle);
  
  // Initialize zone log
  zoneLog.begin();
}

void loop() {
  char buff[100];
  int len = 100;
  int z;

  /* process incoming web connections one at a time */
  webserver.processConnection(buff, &len);

  // If not operating manually...
  if (sysStatus.mode != MANUAL_MODE) {

    // Check schedule for automatic watering activity
    if ((z = execSchedule.activeZone(&curCycle)) != -1) {
  
      // Turn on active zone
      zoneOn(z);
      sysStatus.mode = AUTO_MODE;
         
    } else {
  
      // No active zones
      allZonesOff();
      sysStatus.mode = IDLE_MODE;
  
      // Re-build schedule when idle
      execSchedule.build(&curCycle);
    }
  }
  
  // pause 1 sec.
  delay(500);
}

// send an NTP request to the time server at the given address
void sendNTPrequest(char* address)
{
  // set all bytes in the buffer to 0
  memset(ntpPacket, 0, NTP_PACKET_SIZE);
  
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  ntpPacket[0] = 0b11100011;   // LI, Version, Mode
  ntpPacket[1] = 0;            // Stratum, or type of clock
  ntpPacket[2] = 6;            // Polling Interval
  ntpPacket[3] = 0xEC;         // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  ntpPacket[12]  = 49;
  ntpPacket[13]  = 0x4E;
  ntpPacket[14]  = 49;
  ntpPacket[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(ntpPacket, NTP_PACKET_SIZE);
  Udp.endPacket();
}

// Parse response from NTP server
unsigned long getNTPtime() {
  
  // Discard any previous packets 
  while (Udp.parsePacket() > 0) ;    
   
  // Request time from NTP server
  sendNTPrequest(timeServer); // send an NTP packet to a time server

  // Look for NTP server response
  unsigned long beginWait = millis();
  while (millis() - beginWait < 1500) {   // Wait no more than 1.5 secs.
    if (Udp.parsePacket()) {
      
      // We've received a packet, read the data from it
      Udp.read(ntpPacket, NTP_PACKET_SIZE); // read the packet into the buffer
    
      //the timestamp starts at byte 40 of the received packet and is four bytes,
      // or two words, long. First, esxtract the two words:
      unsigned long highWord = word(ntpPacket[40], ntpPacket[41]);
      unsigned long lowWord = word(ntpPacket[42], ntpPacket[43]);
      
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;
    
      // now convert NTP time into everyday time
      // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
      return (secsSince1900 - 2208988800UL);
    }
  }

  // Serial.println("No NTP response!");
  return (0);
}

// Convert a string to an integer

int strToInt(char *str) {
  int val = 0;
  for (int i = 0; i < strlen(str); i++) {
    val = (val *10) + (str[i] - '0');
  }
  return val;
}

// Convert an integer to a string (with optional left zero padding)

char *intToStr(int v, int zeroPad) {
  static char s[LEN];
  int i = LEN - 1;

  s[i--] = '\0';
  while (v > 9 && i > 0) {
    s[i--] = (v % 10) + '0';
    v /= 10;
  }
  s[i] = v + '0';
  
  while (LEN - (i + 1) < zeroPad) {
    s[--i] = '0';
  }
  return s + i;
}
