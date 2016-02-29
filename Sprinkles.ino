#include <avr/wdt.h>
#include <EEPROM.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <Time.h>
#include <Timezone.h>

#define WEBDUINO_FAVICON_DATA ""
#include <WebServer.h>

#define VERSION   "1.3"

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

// My watering zones
const byte NUM_ZONES = 4;
WaterZone gZoneList[NUM_ZONES] = {     // Watering zones
  { false, 8, "SOUTH" },
  { false, 6, "NORTH" },
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
  time_t resetTime;            // last reset timestamp
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
P(htmlDocStyle) = "<head><style>\
                      body{background-color:#b0c4de;}\
                      td,th{padding:3px;text-align:center;background-color:#ffffff;}\
                      .on{background-color:#66cc66}\
                      .title{font-size:xx-large;font-weight:bold}\
                      .vers{font-size:small}\
                   </style></head>\n";
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

ZoneLog gZoneLog;

typedef struct {
  int startTime;                      // Minutes from start of day
  byte zone_duration[NUM_ZONES];      // "ON" time (minutes)
  byte activeDays;                    // Active days of week (bitfield)
  bool enabled;                       // Cycle is enabled (yes/no)
} WaterCycle;

WaterCycle gCurCycle;

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

      // If cycle is enabled and at least one day is active...
      if (cycle->enabled && cycle->activeDays != 0) {

        // Find start of first event following current time
        time_t localTime = myTZ.toLocal(now());
        time_t eventStart = previousMidnight(localTime) + (cycle->startTime * SECS_PER_MIN);
        while (eventStart < localTime) {
          eventStart += SECS_PER_DAY;
        }

        // Create events to fill schedule
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
    int activeZone(void) {

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

      // No event/zone is active
      return (-1);
    }

    // return the start time of next scheduled watering event
    time_t nextEventStart(time_t curTime) {
      for (int i = 0; i < NUM_SCHED_EVENTS; i++) {
        if (curTime < eventList[i].startTime) {
          return eventList[i].startTime;
        }
      }
    }

    // Is the current schedule "stale" (i.e. older than current time)?
    boolean isStale(void) {
      time_t localTime = myTZ.toLocal(now());
      
      if (localTime > eventList[NUM_SCHED_EVENTS-1].zoneStopTime[NUM_ZONES-1]) {
        return (true);
      } else {
        return (false);
      }
    }
};

Schedule gExecSchedule;

/***********************************************************************
 * Zone Control Functions
 **********************************************************************/

// zoneOn() - Turn on a watering zone (only one ON at a time)
void zoneOn(byte zone) {
  if (! gZoneList[zone].on) {
    for (int i = 0; i < NUM_ZONES; i++) {
      zoneOff(i);
    }
    gZoneList[zone].on = true;
    digitalWrite(gZoneList[zone].pin, HIGH);
    delay(20);
    gZoneLog.add(zone, true);
  }
}

// zoneOff() - Turn off a watering zone
void zoneOff(byte zone) {
  if (gZoneList[zone].on) {
    gZoneList[zone].on = false;
    digitalWrite(gZoneList[zone].pin, LOW);
    delay(20);
    gZoneLog.add(zone, false);
  }
}

// allZonesOff() - Turn all watering zones off
void allZonesOff() {
  for (byte i = 0; i < NUM_ZONES; i++) {
    zoneOff(i);
  }
}

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
    server.print(gZoneList[i].name);
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

void htmlUptimeStr(time_t t, WebServer *server) {
  int d;

  if ((d = t / SECS_PER_WEEK) > 0) {
    server->print(intToStr(d, 0));
    server->printP(PSTR(" weeks, "));
    t -= d * SECS_PER_WEEK;
  }
  if ((d = t / SECS_PER_DAY) > 0) {
    server->print(intToStr(d, 0));
    server->printP(PSTR(" days, "));
    t -= d * SECS_PER_DAY;
  }
  if ((d = t / SECS_PER_HOUR) > 0) {
    server->print(intToStr(d, 0));
    server->printP(PSTR(" hrs, "));
    t -= d * SECS_PER_HOUR;
  }
  if ((d = t / SECS_PER_MIN) > 0) {
    server->print(intToStr(d, 0));
    server->printP(PSTR(" mins, "));
    t -= d * SECS_PER_MIN;
  }
  server->print(intToStr((int)t, 0));
  server->printP(PSTR(" secs"));
}

// Webserver callback - "Home" page
void homeCmd(WebServer &server, WebServer::ConnectionType type, char *, bool) {
  time_t localTime = myTZ.toLocal(now());

  // HTTP OK header
  server.httpSuccess();

  // If a HEAD request, do nothing else
  if (type != WebServer::HEAD) {
    server.printP(htmlDocStyle);
    server.printP(PSTR("<div class=\"title\">Sprinkler Controller</div>"));
    server.printP(PSTR("<span class=\"vers\">Version "));
    server.print(VERSION);
    server.printP(PSTR("</span><p>"));

    // Current time
    server.printP(htmlTableHead);
    server.printP(PSTR("<th>Time</th><td>"));
    if (timeStatus() == timeNotSet) {
      server.printP(PSTR("Not Set"));
    } else {
      htmlTimeStr(localTime, &server);
      if (timeStatus() == timeNeedsSync) {
        server.printP(PSTR(" (STALE)"));
      }
    }
    server.printP(htmlTDTail);

    // Uptime
    server.printP(htmlNextRow);
    server.printP(PSTR("<th>Up Time</th>"));
    server.printP(htmlTDHead);
    htmlUptimeStr(localTime - sysStatus.resetTime, &server);
    server.printP(htmlTDTail);

    // Next cycle runtime
    server.printP(htmlNextRow);
    server.printP(PSTR("<th>Next Cycle</th>"));
    server.printP(htmlTDHead);
    if (gCurCycle.enabled) {
      if (gExecSchedule.nextEventStart(localTime) == 0) {
        server.printP(PSTR("<em>None</em>"));
      } else {
        htmlTimeStr(gExecSchedule.nextEventStart(localTime), &server);
      }
    } else {
      server.printP(PSTR("<em>Disabled</em>"));
    }
    server.printP(htmlTDTail);

    // System status
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
    server.printP(htmlTDTail);
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
      if (gZoneList[i].on) {
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
    if (gCurCycle.enabled) {
      server.printP(PSTR("checked "));
    }
    server.printP(PSTR("name=\"E\"></td>"));
    server.printP(PSTR("<td><select name=\"time\">"));
    for (int j = 0; j < 24; j++) {
      int hr = (j < 13) ? j : j - 12;
      const char *tail = (j < 12) ? "AM" : "PM";
      server.printP(PSTR("<option "));
      if (gCurCycle.startTime == j * 60) {
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

    // Cycle zone durations
    int cycleUsage = 0;
    for (int j = 0; j < NUM_ZONES; j++) {
      cycleUsage += gCurCycle.zone_duration[j];
      server.printP(PSTR("<td><select name=\"Z"));
      server.print(intToStr(j, 0));
      server.printP(PSTR("\">"));
      for (int k = 0; k < 35; k += 5) {
        server.printP(PSTR("<option "));
        if (gCurCycle.zone_duration[j] == k) {
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

    // Watering days/week
    int weekUsage = 0;
    server.printP(htmlTDHead);
    for (int j = 0; j < DAYS_PER_WEEK; j++) {
      server.printP(PSTR("<input type=\"checkbox\" "));
      if (gCurCycle.activeDays & (1 << j + 1)) {
        weekUsage += cycleUsage;
        server.printP(PSTR("checked "));
      }
      server.printP(PSTR("name=\"D"));
      server.print(intToStr(j + 1, 0));
      server.printP(PSTR("\">"));
      server.print(dayShortStr(j + 1));
    }
    server.printP(htmlTDTail);

    // Total cycle usage
    server.printP(htmlNextRow);
    server.printP(PSTR("<th>Usage</th><td colspan=\"99\">"));
    server.print(intToStr(cycleUsage, 0));
    server.printP(PSTR(" mins/cycle, "));
    server.print(intToStr(weekUsage, 0));
    server.printP(PSTR(" mins/week"));
    server.printP(htmlTDTail);

    server.printP(htmlTableTail);
    server.printP(htmlPara);
    server.printP(PSTR("<input type=\"submit\" value=\"SAVE\">"));
    server.printP(htmlFormTail);

    // Activity Log
    server.printP(PSTR("<h3>Activity Log</h3>"));
    server.printP(PSTR("<form action=\"log.html\">"));
    server.printP(htmlTableHead);
    gZoneLog.dumpHTML(&server);
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

// Webserver callback - Process watering cycle edit
void cycleCmd(WebServer &server, WebServer::ConnectionType type, char *tail, bool) {
  char pName[LEN];
  char pValue[LEN];

  // If a HEAD request, do nothing else
  if (type != WebServer::HEAD) {
    gCurCycle.activeDays = 0;    // Initialize attributes
    gCurCycle.enabled = false;

    while (strlen(tail)) {
      if (server.nextURLparam(&tail, pName, LEN, pValue, LEN) != URLPARAM_EOS) {
        if (strcmp(pName, "time") == 0) {
          gCurCycle.startTime = strToInt(pValue);

        } else if (pName[0] == 'Z') {
          int zone = pName[1] - '0';
          gCurCycle.zone_duration[zone] = strToInt(pValue);

        } else if (pName[0] == 'D') {
          int day = pName[1] - '0';
          gCurCycle.activeDays |= (1 << day);

        } else if (pName[0] == 'E') {
          gCurCycle.enabled = true;
        }
      }
    }
  }

  EEPROM.put(EEADDR_WATER_CYCLE, gCurCycle);     // update EEPROM

  // re-build execution schedule
  gExecSchedule.build(&gCurCycle);

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
          gZoneLog.clear();                       // Clear zone log
        }
      }
    }
  }
  server.httpSeeOther("index.html");             // redirect back to "home" page
}

// MAC address is arbitrary
byte mac[] = {
  0x00, 0xAA, 0xBB, 0xCC, 0xDE, 0x02
};
  
/***********************************************************************
 * SETUP()
 **********************************************************************/
void setup() {

  // Disable SD card on Ethernet shield
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);

  // Open serial communications
  // Serial.begin(9600);

  // Start ethernet connection using DHCP to obtain IP address
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

  // Initialize zone log
  gZoneLog.begin();

  // Mark RESET in log
  gZoneLog.add(99, false);

  // Come up in idle mode
  sysStatus.mode = IDLE_MODE;

  // Log reset time
  sysStatus.resetTime = myTZ.toLocal(now());

  //initialize zone control pins
  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(gZoneList[i].pin, OUTPUT);
  }

  allZonesOff();    // make sure all zones are off

  // Initialize variables from EEPROM
  EEPROM.get(EEADDR_WATER_CYCLE, gCurCycle);

  // build schedule
  gExecSchedule.build(&gCurCycle);

  // enable watchdog timer
  wdt_enable(WDTO_4S);      // 4 secs.
}

/***********************************************************************
 * LOOP()
 **********************************************************************/
void loop() {
  char buff[100];
  int len = 100;
  int z;

  /* reset watchdog - still alive! */
  wdt_reset();
  
  /* process incoming web connections one at a time */
  webserver.processConnection(buff, &len);

  // If not operating manually...
  if (sysStatus.mode != MANUAL_MODE) {

    // Check schedule for automatic watering activity
    if ((z = gExecSchedule.activeZone()) != -1) {

      // Turn on active zone
      zoneOn(z);
      sysStatus.mode = AUTO_MODE;

    } else {

      // No active zones
      allZonesOff();
      sysStatus.mode = IDLE_MODE;
    }

    // Build new schedule if current one is "stale"
    if (gExecSchedule.isStale()) {
      gExecSchedule.build(&gCurCycle);
    }
  }

  // pause 1/4 sec.
  delay(250);
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
    val = (val * 10) + (str[i] - '0');
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
