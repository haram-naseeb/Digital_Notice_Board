#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Lib.h>
#include <Ressources/Font6x8.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
Preferences prefs;
// ================= FORWARD DECLARATIONS =================
struct Announcement;
struct Category;
struct SubjectColor;
struct YearInfo;
struct SessionInfo;
struct SectionInfo;
struct TTEntry;
struct TimetableData;
struct TTGrid;

// ================= WIFIMANAGER =================
WiFiManager wifiManager;
unsigned long lastWiFiCheck   = 0;
const unsigned long WIFI_CHECK_INTERVAL = 10000;

// ================= MQTT (HiveMQ Broker) =================
const char* mqttServer = "broker.hivemq.com";
const int   mqttPort   = 1883;

unsigned long lastMQTTAttempt = 0;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
int mqttConnectionAttempts = 0;
const int MAX_MQTT_ATTEMPTS = 10;

// ================= VGA =================
const int redPin   = 14;
const int greenPin = 19;
const int bluePin  = 21;
const int hsyncPin = 23;
const int vsyncPin = 22;

// ================= BUTTONS =================
const int upBtn    = 32;
const int downBtn  = 33;
const int enterBtn = 27;
const int backBtn  = 4;

// ================= OUTPUT =================
const int buzzer    = 18;
const int redLED    = 25;
const int greenLED  = 26;
const int yellowLED = 5;

VGA3Bit vga;

WiFiClient   httpClient;
WiFiClient   mqttClient;
PubSubClient client(mqttClient);

// ================= MULTI-NETWORK HTTP CONFIG =================
const char* serverIP = "192.168.0.112";

// ================= 8 VGA COLORS =================
// VGA3Bit only outputs 8 real colors — all vga.RGB() calls quantize to these.
int C_BLACK;    // 0,0,0
int C_RED;      // 255,0,0
int C_GREEN;    // 0,255,0
int C_YELLOW;   // 255,255,0
int C_BLUE;     // 0,0,255
int C_MAGENTA;  // 255,0,255
int C_CYAN;     // 0,255,255
int C_WHITE;    // 255,255,255

// ================= ANNOUNCEMENTS =================
struct Announcement {
  int  id;
  char title[60];
  char message[200];
  bool isNew;
};

const int MAX_PER_CAT = 5;

struct Category {
  Announcement items[MAX_PER_CAT];
  int count;
  int index;
};

Category urgent   = {{}, 0, 0};
Category general  = {{}, 0, 0};
Category eventCat = {{}, 0, 0};
Category office   = {{}, 0, 0};

const int MAX_SEEN_ANNOUNCEMENTS = 50;
int seenAnnouncements[MAX_SEEN_ANNOUNCEMENTS] = {0};
int seenCount = 0;

int notifiedAnnouncements[MAX_SEEN_ANNOUNCEMENTS] = {0};
int notifiedCount = 0;

bool hasSeenAnnouncement(int annId) {
  for (int i = 0; i < seenCount; i++)
    if (seenAnnouncements[i] == annId) return true;
  return false;
}

void markAnnouncementAsSeen(int annId) {
  if (!hasSeenAnnouncement(annId) && seenCount < MAX_SEEN_ANNOUNCEMENTS)
    seenAnnouncements[seenCount++] = annId;
}

void loadNotifiedFromFlash() {
  prefs.begin("notified", false);
  notifiedCount = prefs.getInt("count", 0);
  for (int i = 0; i < notifiedCount; i++) {
    char key[10];
    sprintf(key, "id%d", i);
    notifiedAnnouncements[i] = prefs.getInt(key, 0);
  }
  prefs.end();
}

bool hasNotifiedUser(int annId) {
  for (int i = 0; i < notifiedCount; i++)
    if (notifiedAnnouncements[i] == annId) return true;
  return false;
}

void markNotifiedUser(int annId) {
  if (hasNotifiedUser(annId)) return;
  if (notifiedCount >= MAX_SEEN_ANNOUNCEMENTS) return;
  notifiedAnnouncements[notifiedCount] = annId;
  prefs.begin("notified", false);
  char key[10];
  sprintf(key, "id%d", notifiedCount);
  prefs.putInt(key, annId);
  notifiedCount++;
  prefs.putInt("count", notifiedCount);
  prefs.end();
}

// ================= YEARS/SESSIONS/SECTIONS =================
const int MAX_YEARS    = 10;
const int MAX_SESSIONS = 10;
const int MAX_SECTIONS = 10;

struct YearInfo    { int id; char name[10]; };
struct SessionInfo { int id; char name[40]; };
struct SectionInfo { int id; char name[10]; };

YearInfo    years[MAX_YEARS];
SessionInfo sessions[MAX_SESSIONS];
SectionInfo sections[MAX_SECTIONS];
int totalYears    = 0;
int totalSessions = 0;
int totalSections = 0;

// ================= TIMETABLE =================
struct TTEntry {
  char startTime[10];
  char endTime[10];
  char subject[30];
  char code[10];
  char teacher[30];
  char room[10];
  int  dayId;
  char dayName[4];
};

const int MAX_TT_ENTRIES = 40;

struct TimetableData {
  TTEntry entries[MAX_TT_ENTRIES];
  int     count;
  int     yearIdx;
  int     sessionIdx;
  int     sectionIdx;
  char    semester[20];
  char    session_name[40];
  char    term[20];
  bool    isEvening;
};

TimetableData currentTT = {{}, 0, -1, -1, -1, "", "", "", false};

// ================= TIMETABLE GRID =================
const int MAX_DAYS  = 5;
const int MAX_SLOTS = 8;

const char* DAY_NAMES[5] = { "MON", "TUE", "WED", "THU", "FRI" };

struct TTGrid {
  int  slotCount;
  int  slotStart;
  char cellCode[MAX_DAYS][MAX_SLOTS][10];
  char cellRoom[MAX_DAYS][MAX_SLOTS][10];
  int  cellSpan[MAX_DAYS][MAX_SLOTS];
};

TTGrid ttGrid;

bool detectEvening(const char* sessionName) {
  String s = String(sessionName);
  s.toLowerCase();
  return s.indexOf("evening") >= 0;
}

void buildTimetableGrid() {
  memset(&ttGrid, 0, sizeof(ttGrid));
  if (currentTT.isEvening) { ttGrid.slotStart = 13; ttGrid.slotCount = 7; }
  else                      { ttGrid.slotStart = 8;  ttGrid.slotCount = 8; }

  for (int d = 0; d < MAX_DAYS; d++)
    for (int s = 0; s < MAX_SLOTS; s++) {
      ttGrid.cellCode[d][s][0] = '\0';
      ttGrid.cellRoom[d][s][0] = '\0';
      ttGrid.cellSpan[d][s]    = 0;
    }

  for (int i = 0; i < currentTT.count; i++) {
    TTEntry& e = currentTT.entries[i];
    int dayIdx = e.dayId - 1;
    if (dayIdx < 0 || dayIdx >= MAX_DAYS) continue;
    int startHour = 0, endHour = 0;
    sscanf(e.startTime, "%d:", &startHour);
    sscanf(e.endTime,   "%d:", &endHour);
    int startSlot = startHour - ttGrid.slotStart;
    int span      = endHour - startHour;
    if (span < 1) span = 1;
    if (startSlot < 0 || startSlot >= ttGrid.slotCount) continue;
    if (startSlot + span > ttGrid.slotCount) span = ttGrid.slotCount - startSlot;
    strncpy(ttGrid.cellCode[dayIdx][startSlot], e.code, 9); ttGrid.cellCode[dayIdx][startSlot][9] = '\0';
    strncpy(ttGrid.cellRoom[dayIdx][startSlot], e.room, 9); ttGrid.cellRoom[dayIdx][startSlot][9] = '\0';
    ttGrid.cellSpan[dayIdx][startSlot] = span;
    for (int j = 1; j < span; j++)
      if (startSlot + j < ttGrid.slotCount)
        ttGrid.cellSpan[dayIdx][startSlot + j] = -1;
  }
}

// ================= SUBJECT COLOR PALETTE =================
struct SubjectColor { int bg; int text; };

SubjectColor getSubjectColor(const char* code) {
  unsigned int hash = 0;
  for (int i = 0; code[i]; i++) hash = hash * 31 + (unsigned char)code[i];
  switch (hash % 3) {
    case 0: return { C_CYAN,  C_BLACK };
    case 1: return { C_BLUE,  C_WHITE };
    case 2: return { C_WHITE, C_BLACK };
    default: return { C_BLUE, C_WHITE };
  }
}

// ================= GROUND FLOOR TEACHER OFFICES =================
const int  OFFICE_ROOM_COUNT  = 6;
char       officeTeacher[OFFICE_ROOM_COUNT][32];
int        officeTeacherLoaded = 0;

// ================= FIRST FLOOR DATA =================
const int FF_STANDALONE_COUNT = 5;
const int FF_STANDALONE_ROOMS[FF_STANDALONE_COUNT] = {13, 14, 15, 19, 20};

const int CABIN2_ROOM_COUNT = 6;
const int CABIN2_SLOT_TO_ROOM[6] = {9, 10, 8, 11, 7, 12};

const int CABIN3_ROOM_COUNT = 3;
const int CABIN3_SLOT_TO_ROOM[3] = {16, 17, 18};

char ffStandaloneTeacher[FF_STANDALONE_COUNT][32];
char cabin2Teacher[CABIN2_ROOM_COUNT][32];
char cabin3Teacher[CABIN3_ROOM_COUNT][32];

const int FF_MAX_TEACHERS = 20;
struct FFTeacher {
  char name[32];
  int  roomNumber;
  int  cabinNumber;
};
FFTeacher ffTeachers[FF_MAX_TEACHERS];
int       ffTeacherCount = 0;
int       ffTeacherLoaded = 0;

// ================= FLOOR SELECTION =================
const char* OFFICE_FLOORS[]    = { "Ground Floor", "First Floor" };
const int   OFFICE_FLOOR_COUNT = 2;
int         selOfficeFloor     = 0;
int         officeFloorScroll  = 0;

// ================= GROUND FLOOR NAV =================
int selOfficeTeacher    = 0;
int officeTeacherScroll = 0;
int officeHighlightRoom = -1;

// ================= FIRST FLOOR NAV =================
int selFFTeacher    = 0;
int ffTeacherScroll = 0;
int ffSelectedTeacherIdx  = -1;
int ffHighlightCabin      = 0;
int ffHighlightRoom       = -1;
int selCabin2Slot = 0;
int selCabin3Slot = 0;

// ================= UI STATE =================
enum UIState {
  MENU,
  URGENT_VIEW, GENERAL_VIEW, EVENT_VIEW, OFFICE_VIEW,
  TT_YEAR, TT_SESSION, TT_SECTION, TT_DISPLAY,
  OFFICE_FLOOR,
  OFFICE_TEACHERS, OFFICE_MAP,
  FF_TEACHER_LIST, FF_FLOOR_MAP, FF_CABIN2_MAP, FF_CABIN3_MAP
};

UIState currentState     = MENU;
int     selectedMenu     = 0;
int     selYear          = 0;
int     selSession       = 0;
int     selSection       = 0;
int     yearScrollOff    = 0;
int     sessionScrollOff = 0;
int     sectionScrollOff = 0;

// ================= TIMING =================
unsigned long lastPress    = 0;
const int     DEBOUNCE     = 250;
unsigned long lastBeepTime = 0;
const int     BEEP_COOL    = 500;
unsigned long lastAnnFetch = -31000UL;
const int     ANN_FETCH_INTERVAL = 30000;

void drawMenu();
void updateLEDs();

// ========================================
// HELPERS
// ========================================
void beep(int times) {
  if (millis() - lastBeepTime < BEEP_COOL) return;
  for (int i = 0; i < times; i++) {
    digitalWrite(buzzer, HIGH); delay(100);
    digitalWrite(buzzer, LOW);
    if (i < times - 1) delay(100);
  }
  lastBeepTime = millis();
}

void centerText(int y, const char* txt, int color, int bg) {
  int x = (400 - strlen(txt) * 6) / 2;
  if (x < 5) x = 5;
  vga.setTextColor(color, bg);
  vga.setCursor(x, y);
  vga.print(txt);
}

void leftText(int x, int y, const char* txt, int color, int bg) {
  vga.setTextColor(color, bg);
  vga.setCursor(x, y);
  vga.print(txt);
}

void drawBox(int x, int y, int w, int h, int color) {
  vga.fillRect(x,         y,         w, 1, color);
  vga.fillRect(x,         y + h - 1, w, 1, color);
  vga.fillRect(x,         y,         1, h, color);
  vga.fillRect(x + w - 1, y,         1, h, color);
}

void drawBoxDouble(int x, int y, int w, int h, int outerC, int innerC) {
  drawBox(x,     y,     w,     h,     outerC);
  drawBox(x + 2, y + 2, w - 4, h - 4, innerC);
}

// -----------------------------------------------------------------------
// HEADER
// -----------------------------------------------------------------------
void drawHeader() {
  vga.fillRect(0, 0, 400, 46, C_BLUE);
  vga.fillRect(0, 0, 400, 3, C_YELLOW);
  vga.fillRect(0, 44, 400, 2, C_WHITE);

  vga.setTextColor(C_WHITE, C_BLUE);
  vga.setCursor(8, 8);
  vga.print("UET CS DEPT");

  centerText(22, "DEPARTMENT NOTICE BOARD", C_WHITE, C_BLUE);
}

// -----------------------------------------------------------------------
// FOOTER
// -----------------------------------------------------------------------
void drawFooter(const char* hint) {
  vga.fillRect(0, 286, 400, 14, C_BLUE);
  vga.fillRect(0, 285, 400, 1,  C_WHITE);

  if (client.connected()) {
    vga.fillRect(6, 288, 28, 10, C_GREEN);
    vga.setTextColor(C_WHITE, C_GREEN);
    vga.setCursor(8, 290); vga.print("LIVE");
  } else {
    vga.fillRect(6, 288, 28, 10, C_RED);
    vga.setTextColor(C_WHITE, C_RED);
    vga.setCursor(8, 290); vga.print("OFF ");
  }

  leftText(42, 290, hint, C_WHITE, C_BLUE);
}

// -----------------------------------------------------------------------
// Base screen
// -----------------------------------------------------------------------
void drawBase(const char* hint) {
  vga.clear(C_WHITE);
  drawHeader();
  drawFooter(hint);
}

// -----------------------------------------------------------------------
// Loading screen
// -----------------------------------------------------------------------
void showLoading(const char* msg) {
  vga.clear(C_BLACK);
  drawHeader();

  vga.fillRect(60, 110, 280, 70, C_BLACK);
  drawBox(60, 110, 280, 70, C_CYAN);
  vga.fillRect(60, 110, 280, 3, C_BLUE);

  centerText(130, msg,              C_WHITE, C_BLACK);
  centerText(148, "Please wait...", C_CYAN,  C_BLACK);

  drawFooter("");
}

void updateLEDs() {
  bool u = false, g = false, e = false;
  for (int i = 0; i < urgent.count;   i++) if (urgent.items[i].isNew)   u = true;
  for (int i = 0; i < general.count;  i++) if (general.items[i].isNew)  g = true;
  for (int i = 0; i < eventCat.count; i++) if (eventCat.items[i].isNew) e = true;
  digitalWrite(redLED,    u);
  digitalWrite(greenLED,  g);
  digitalWrite(yellowLED, e);
}

void addToCategory(Category &cat, int annId, const char* title, const char* msg, bool isNewAnn = true) {
  if (cat.count >= MAX_PER_CAT) {
    for (int i = 0; i < MAX_PER_CAT - 1; i++) cat.items[i] = cat.items[i + 1];
    cat.count = MAX_PER_CAT - 1;
  }
  strncpy(cat.items[cat.count].title,   title, 59);
  strncpy(cat.items[cat.count].message, msg,   199);
  cat.items[cat.count].title[59]    = '\0';
  cat.items[cat.count].message[199] = '\0';
  cat.items[cat.count].id    = annId;
  cat.items[cat.count].isNew = !hasNotifiedUser(annId);   // your fixed line

  // ADD THIS — jump index to newest if it's a fresh arrival via MQTT
  if (isNewAnn && !hasNotifiedUser(annId))
    cat.index = cat.count;

  if (isNewAnn) markAnnouncementAsSeen(annId);
  cat.count++;
}

void clearAllCategories() {
  urgent.count   = 0; urgent.index   = 0;
  general.count  = 0; general.index  = 0;
  eventCat.count = 0; eventCat.index = 0;
  office.count   = 0; office.index   = 0;
}
void pruneNotifiedFlash() {
  // Build list of currently active announcement IDs
  int activeIds[MAX_PER_CAT * 4];
  int activeCount = 0;
  for (int i = 0; i < urgent.count;   i++) activeIds[activeCount++] = urgent.items[i].id;
  for (int i = 0; i < general.count;  i++) activeIds[activeCount++] = general.items[i].id;
  for (int i = 0; i < eventCat.count; i++) activeIds[activeCount++] = eventCat.items[i].id;
  for (int i = 0; i < office.count;   i++) activeIds[activeCount++] = office.items[i].id;

  // Keep only IDs that still exist
  int kept[MAX_SEEN_ANNOUNCEMENTS];
  int keptCount = 0;
  for (int i = 0; i < notifiedCount; i++) {
    for (int j = 0; j < activeCount; j++) {
      if (notifiedAnnouncements[i] == activeIds[j]) {
        kept[keptCount++] = notifiedAnnouncements[i];
        break;
      }
    }
  }

  // Rewrite flash only if something was pruned
  if (keptCount < notifiedCount) {
    for (int i = 0; i < keptCount; i++) notifiedAnnouncements[i] = kept[i];
    notifiedCount = keptCount;
    prefs.begin("notified", false);
    prefs.clear();
    prefs.putInt("count", notifiedCount);
    for (int i = 0; i < notifiedCount; i++) {
      char key[10]; sprintf(key, "id%d", i);
      prefs.putInt(key, notifiedAnnouncements[i]);
    }
    prefs.end();
  }
}
// ========================================
// HTTP FUNCTIONS
// ========================================
void fetchAnnouncements() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/announcements/approved/all";
  if (!http.begin(httpClient, url)) return;
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, payload)) { http.end(); return; }
    clearAllCategories();
    for (JsonObject ann : doc.as<JsonArray>()) {
      int         annId    = ann["announcement_id"] | 0;
      const char* title    = ann["title"]    | "";
      const char* message  = ann["message"]  | "";
      const char* category = ann["category"] | "";
      if      (strcmp(category, "urgent")  == 0) addToCategory(urgent,   annId, title, message, false);
      else if (strcmp(category, "general") == 0) addToCategory(general,  annId, title, message, false);
      else if (strcmp(category, "event")   == 0) addToCategory(eventCat, annId, title, message, false);
      else if (strcmp(category, "office")  == 0) addToCategory(office,   annId, title, message, false);
      markAnnouncementAsSeen(annId);
    }
    updateLEDs();
    pruneNotifiedFlash();
    if (currentState == MENU) drawMenu();
  }
  http.end();
}

void fetchYears() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/years";
  if (!http.begin(httpClient, url)) return;
  http.setConnectTimeout(5000); http.setTimeout(10000);
  if (http.GET() == 200) {
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, http.getString())) {
      totalYears = 0;
      for (JsonObject y : doc.as<JsonArray>()) {
        if (totalYears >= MAX_YEARS) break;
        years[totalYears].id = y["year_id"];
        strncpy(years[totalYears].name, y["year_name"] | "?", 9);
        years[totalYears].name[9] = '\0';
        totalYears++;
      }
    }
  }
  http.end();
}

void fetchSessions(int yearId) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/sessions/" + yearId;
  if (!http.begin(httpClient, url)) return;
  http.setConnectTimeout(5000); http.setTimeout(10000);
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, http.getString())) {
      totalSessions = 0;
      for (JsonObject s : doc.as<JsonArray>()) {
        if (totalSessions >= MAX_SESSIONS) break;
        sessions[totalSessions].id = s["session_id"];
        strncpy(sessions[totalSessions].name, s["session_name"] | "?", 39);
        sessions[totalSessions].name[39] = '\0';
        totalSessions++;
      }
    }
  }
  http.end();
}

void fetchSections(int sessionId) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/sections/" + sessionId;
  if (!http.begin(httpClient, url)) return;
  http.setConnectTimeout(5000); http.setTimeout(10000);
  if (http.GET() == 200) {
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, http.getString())) {
      totalSections = 0;
      for (JsonObject s : doc.as<JsonArray>()) {
        if (totalSections >= MAX_SECTIONS) break;
        sections[totalSections].id = s["section_id"];
        strncpy(sections[totalSections].name, s["section_name"] | "?", 9);
        sections[totalSections].name[9] = '\0';
        totalSections++;
      }
    }
  }
  http.end();
}

void fetchTimetable(int yIdx, int sIdx, int secIdx) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - cannot fetch timetable");
    currentTT.count = 0;
    return;
  }

  if (yIdx < 0 || yIdx >= totalYears || sIdx < 0 || sIdx >= totalSessions || secIdx < 0 || secIdx >= totalSections) {
    currentTT.count = 0;
    return;
  }

  int yearId    = years[yIdx].id;
  int sessionId = sessions[sIdx].id;
  int sectionId = sections[secIdx].id;

  WiFiClient timetableClient;
  String path = String("/timetable/") + yearId + "/" + sessionId + "/" + sectionId;
  String url  = String("http://") + serverIP + ":3000" + path;

  Serial.println("================ TIMETABLE FETCH ================");
  Serial.print("Selected year: ");    Serial.print(yearId);    Serial.print(" ("); Serial.print(years[yIdx].name);       Serial.println(")");
  Serial.print("Selected session: "); Serial.print(sessionId); Serial.print(" ("); Serial.print(sessions[sIdx].name);    Serial.println(")");
  Serial.print("Selected section: "); Serial.print(sectionId); Serial.print(" ("); Serial.print(sections[secIdx].name);  Serial.println(")");
  Serial.print("URL: "); Serial.println(url);

  if (!timetableClient.connect(serverIP, 3000)) {
    currentTT.count = 0;
    return;
  }

  timetableClient.setTimeout(10000);
  timetableClient.print(String("GET ") + path + " HTTP/1.0\r\n");
  timetableClient.print(String("Host: ") + serverIP + "\r\n");
  timetableClient.print("Connection: close\r\n");
  timetableClient.print("Accept: application/json\r\n\r\n");

  unsigned long startWait = millis();
  while (!timetableClient.available() && millis() - startWait < 10000) delay(10);

  String statusLine = timetableClient.readStringUntil('\n');
  statusLine.trim();
  int code = 0;
  if (statusLine.startsWith("HTTP/")) {
    int firstSpace = statusLine.indexOf(' ');
    if (firstSpace > 0) code = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  }

  currentTT.count = 0;
  currentTT.yearIdx    = yIdx;
  currentTT.sessionIdx = sIdx;
  currentTT.sectionIdx = secIdx;
  strncpy(currentTT.semester,     "Unknown",           19);
  strncpy(currentTT.session_name, sessions[sIdx].name, 39);
  strncpy(currentTT.term,         "Unknown",           19);
  currentTT.semester[19]     = '\0';
  currentTT.session_name[39] = '\0';
  currentTT.term[19]         = '\0';
  currentTT.isEvening = detectEvening(currentTT.session_name);

  if (code != 200) {
    timetableClient.stop();
    return;
  }

  int contentLength = -1;
  while (timetableClient.connected()) {
    String header = timetableClient.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) break;
    if (header.startsWith("Content-Length:") || header.startsWith("content-length:"))
      contentLength = header.substring(header.indexOf(':') + 1).toInt();
  }

  String payload;
  if (contentLength > 0) payload.reserve(contentLength + 1);

  unsigned long lastRead = millis();
  while ((timetableClient.connected() || timetableClient.available()) && millis() - lastRead < 10000) {
    while (timetableClient.available()) {
      payload += (char)timetableClient.read();
      lastRead = millis();
    }
    delay(1);
  }
  timetableClient.stop();

  Serial.print("Payload length: "); Serial.println(payload.length());

  DynamicJsonDocument doc(24576);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) { Serial.print("JSON parse failed: "); Serial.println(error.c_str()); return; }

  bool ok = doc["success"] | false;
  if (!ok) { Serial.println("Server returned success=false"); return; }

  strncpy(currentTT.semester,     doc["semester"]     | "Unknown",          19);
  strncpy(currentTT.session_name, doc["session_name"] | sessions[sIdx].name,39);
  strncpy(currentTT.term,         doc["term"]         | "Unknown",          19);
  currentTT.semester[19]     = '\0';
  currentTT.session_name[39] = '\0';
  currentTT.term[19]         = '\0';
  currentTT.isEvening = detectEvening(currentTT.session_name);

  for (JsonObject e : doc["data"].as<JsonArray>()) {
    if (currentTT.count >= MAX_TT_ENTRIES) break;
    int i = currentTT.count;

    const char* startTime = e["start_time"]    | "00:00:00";
    const char* endTime   = e["end_time"]      | "00:00:00";
    const char* subject   = e["subject_name"]  | "";
    const char* codeText  = e["subject_code"]  | "";
    const char* teacher   = e["teacher_name"]  | "";
    const char* dayName   = e["day_name"]      | "MON";

    strncpy(currentTT.entries[i].startTime, startTime, 9);
    strncpy(currentTT.entries[i].endTime,   endTime,   9);
    strncpy(currentTT.entries[i].subject,   subject,   29);
    strncpy(currentTT.entries[i].code,      codeText,  9);
    strncpy(currentTT.entries[i].teacher,   teacher,   29);
    strncpy(currentTT.entries[i].dayName,   dayName,   3);

    String roomStr;
    if (e["room_number"].is<const char*>()) roomStr = String((const char*)e["room_number"]);
    else roomStr = String(e["room_number"].as<int>());
    strncpy(currentTT.entries[i].room, roomStr.c_str(), 9);

    currentTT.entries[i].startTime[9] = '\0';
    currentTT.entries[i].endTime[9]   = '\0';
    currentTT.entries[i].subject[29]  = '\0';
    currentTT.entries[i].code[9]      = '\0';
    currentTT.entries[i].teacher[29]  = '\0';
    currentTT.entries[i].room[9]      = '\0';
    currentTT.entries[i].dayName[3]   = '\0';
    currentTT.entries[i].dayId = e["day_id"] | 1;
    currentTT.count++;
  }

  if (!currentTT.isEvening && currentTT.count > 0) {
    int firstHour = 0;
    sscanf(currentTT.entries[0].startTime, "%d:", &firstHour);
    if (firstHour >= 13) currentTT.isEvening = true;
  }

  Serial.print("Loaded into currentTT: "); Serial.println(currentTT.count);
}

void fetchOfficeTeachers(const char* floor) {
  if (WiFi.status() != WL_CONNECTED) return;
  for (int i = 0; i < OFFICE_ROOM_COUNT; i++) strncpy(officeTeacher[i], "----", 31);
  officeTeacherLoaded = 0;
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/offices";
  if (!http.begin(httpClient, url)) return;
  http.setConnectTimeout(5000); http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String payload = http.getString();
  http.end();
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, payload)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    const char* floorStr = o["floor"]       | "";
    const char* roomStr  = o["room_number"] | "-1";
    const char* teacher  = o["teacher_name"]| "";
    int roomNum = atoi(roomStr);
    if (strcmp(floorStr, floor) != 0) continue;
    if (strlen(teacher) == 0) continue;
    int slot = -1;
    // Room-to-slot mapping matches physical left-to-right floor plan layout
    switch (roomNum) {
      case 1: slot = 4; break; case 2: slot = 2; break; case 3: slot = 0; break;
      case 4: slot = 1; break; case 5: slot = 3; break; case 6: slot = 5; break;
    }
    if (slot >= 0) { strncpy(officeTeacher[slot], teacher, 31); officeTeacher[slot][31] = '\0'; }
  }
  officeTeacherLoaded = 1;
}

void fetchFirstFloorTeachers() {
  if (WiFi.status() != WL_CONNECTED) return;
  ffTeacherCount = 0; ffTeacherLoaded = 0;
  for (int i = 0; i < FF_STANDALONE_COUNT; i++) strncpy(ffStandaloneTeacher[i], "----", 31);
  for (int i = 0; i < CABIN2_ROOM_COUNT;   i++) strncpy(cabin2Teacher[i],        "----", 31);
  for (int i = 0; i < CABIN3_ROOM_COUNT;   i++) strncpy(cabin3Teacher[i],        "----", 31);
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/offices";
  if (!http.begin(httpClient, url)) return;
  http.setConnectTimeout(5000); http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String payload = http.getString();
  http.end();
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, payload)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    const char* floorStr = o["floor"] | "";
    const char* roomStr  = o["room_number"] | "0";
    const char* teacher  = o["teacher_name"] | "";
    if (strcmp(floorStr, "1st Floor") != 0) continue;
    if (strlen(teacher) == 0) continue;
    int roomNum = atoi(roomStr);
    int cabinNum = 0;
    if      (roomNum >= 7  && roomNum <= 12) cabinNum = 2;
    else if (roomNum >= 16 && roomNum <= 18) cabinNum = 3;
    else if (roomNum==13||roomNum==14||roomNum==15||roomNum==19||roomNum==20) cabinNum = 0;
    else continue;
    if (ffTeacherCount < FF_MAX_TEACHERS) {
      strncpy(ffTeachers[ffTeacherCount].name, teacher, 31);
      ffTeachers[ffTeacherCount].name[31] = '\0';
      ffTeachers[ffTeacherCount].roomNumber  = roomNum;
      ffTeachers[ffTeacherCount].cabinNumber = cabinNum;
      ffTeacherCount++;
    }
    if (cabinNum == 0) {
      for (int i = 0; i < FF_STANDALONE_COUNT; i++)
        if (FF_STANDALONE_ROOMS[i] == roomNum) { strncpy(ffStandaloneTeacher[i], teacher, 31); ffStandaloneTeacher[i][31]='\0'; break; }
    }
    if (cabinNum == 2) {
      for (int s = 0; s < CABIN2_ROOM_COUNT; s++)
        if (CABIN2_SLOT_TO_ROOM[s] == roomNum) { strncpy(cabin2Teacher[s], teacher, 31); cabin2Teacher[s][31]='\0'; break; }
    }
    if (cabinNum == 3) {
      for (int s = 0; s < CABIN3_ROOM_COUNT; s++)
        if (CABIN3_SLOT_TO_ROOM[s] == roomNum) { strncpy(cabin3Teacher[s], teacher, 31); cabin3Teacher[s][31]='\0'; break; }
    }
  }
  ffTeacherLoaded = 1;
}

void resolveFFTeacherHighlight(int teacherIdx) {
  if (teacherIdx < 0 || teacherIdx >= ffTeacherCount) return;
  int roomNum  = ffTeachers[teacherIdx].roomNumber;
  int cabinNum = ffTeachers[teacherIdx].cabinNumber;
  if (cabinNum == 2) {
    ffHighlightCabin = 2;
    for (int s = 0; s < CABIN2_ROOM_COUNT; s++)
      if (CABIN2_SLOT_TO_ROOM[s] == roomNum) { ffHighlightRoom = s; selCabin2Slot = s; break; }
  } else if (cabinNum == 3) {
    ffHighlightCabin = 3;
    for (int s = 0; s < CABIN3_ROOM_COUNT; s++)
      if (CABIN3_SLOT_TO_ROOM[s] == roomNum) { ffHighlightRoom = s; selCabin3Slot = s; break; }
  } else {
    ffHighlightCabin = 0;
    for (int i = 0; i < FF_STANDALONE_COUNT; i++)
      if (FF_STANDALONE_ROOMS[i] == roomNum) { ffHighlightRoom = i; break; }
  }
}

// ========================================
// TIMETABLE DISPLAY
// ========================================
void drawTimetable() {
  vga.clear(C_BLACK);

  vga.fillRect(0, 0, 400, 44, C_BLUE);
  vga.fillRect(0, 0, 400, 3,  C_YELLOW);
  vga.fillRect(0, 43, 400, 1, C_CYAN);

  char hdr[80];
  sprintf(hdr, "SEC %s", sections[currentTT.sectionIdx].name);
  vga.setTextColor(C_CYAN, C_BLUE);
  vga.setCursor(8, 8); vga.print("TIMETABLE");

  int hdrX = (400 - (int)strlen(hdr) * 6) / 2;
  if (hdrX < 8) hdrX = 8;
  vga.setTextColor(C_WHITE, C_BLUE);
  vga.setCursor(hdrX, 24); vga.print(hdr);

  if (currentTT.count == 0) {
    vga.fillRect(40, 100, 320, 120, C_WHITE);
    drawBox(40, 100, 320, 120, C_CYAN);
    vga.fillRect(40, 100, 320, 3, C_YELLOW);
    centerText(148, "NO TIMETABLE DATA",       C_BLACK, C_WHITE);
    centerText(164, "Check server connection",  C_BLUE,  C_WHITE);
    vga.fillRect(0, 286, 400, 14, C_BLUE);
    vga.fillRect(0, 285, 400, 1,  C_CYAN);
    leftText(8, 290, "[ENTER: Main Menu]  [BACK: Section]", C_CYAN, C_BLUE);
    return;
  }

  buildTimetableGrid();

  const int DAY_COL_W = 32;
  const int HDR_Y     = 44;
  const int HDR_H     = 18;
  const int ROW_Y     = HDR_Y + HDR_H;
  const int ROW_H     = 45;
  const int FOOTER_Y  = 286;
  const int GRID_X    = 0;

  int nSlots = ttGrid.slotCount;
  int cellW  = (400 - DAY_COL_W) / nSlots;

  // Column headers
  vga.fillRect(GRID_X, HDR_Y, DAY_COL_W, HDR_H, C_BLACK);
  drawBox(GRID_X, HDR_Y, DAY_COL_W, HDR_H, C_CYAN);
  vga.setTextColor(C_CYAN, C_BLACK);
  vga.setCursor(GRID_X + 4, HDR_Y + 5);
  vga.print("DAY");

  for (int s = 0; s < nSlots; s++) {
    int x = GRID_X + DAY_COL_W + s * cellW;
    int w = (s == nSlots - 1) ? (400 - x) : cellW;
    vga.fillRect(x, HDR_Y, w, HDR_H, C_BLACK);
    drawBox(x, HDR_Y, w, HDR_H, C_CYAN);
    char label[8];
    sprintf(label, "%d:00", ttGrid.slotStart + s);
    int lx = x + (w - (int)strlen(label) * 6) / 2;
    vga.setTextColor(C_CYAN, C_BLACK);
    vga.setCursor(lx, HDR_Y + 5);
    vga.print(label);
  }

  // Grid rows
  for (int d = 0; d < MAX_DAYS; d++) {
    int y = ROW_Y + d * ROW_H;

    vga.fillRect(GRID_X, y, DAY_COL_W, ROW_H, C_BLACK);
    drawBox(GRID_X, y, DAY_COL_W, ROW_H, C_CYAN);
    vga.setTextColor(C_CYAN, C_BLACK);
    vga.setCursor(GRID_X + 3, y + ROW_H / 2 - 4);
    vga.print(DAY_NAMES[d]);

    int s = 0;
    while (s < nSlots) {
      int x    = GRID_X + DAY_COL_W + s * cellW;
      int span = ttGrid.cellSpan[d][s];

      if (span == -1) { s++; continue; }

      int actualSpan = (span > 0) ? span : 1;
      if (s + actualSpan > nSlots) actualSpan = nSlots - s;
      int fillW = actualSpan * cellW;
      if (s + actualSpan == nSlots) fillW = 400 - x;

      if (span == 0) {
        vga.fillRect(x, y, fillW, ROW_H, C_BLACK);
        drawBox(x, y, fillW, ROW_H, C_CYAN);
        vga.setTextColor(C_CYAN, C_BLACK);
        vga.setCursor(x + fillW / 2 - 3, y + ROW_H / 2 - 4);
        vga.print("-");
        s++;
      } else {
        const char* code = ttGrid.cellCode[d][s];
        const char* room = ttGrid.cellRoom[d][s];
        SubjectColor sc  = getSubjectColor(code);

        vga.fillRect(x, y, fillW, ROW_H, sc.bg);
        vga.fillRect(x, y, 3, ROW_H, sc.bg);
        drawBox(x, y, fillW, ROW_H, C_BLACK);

        int maxChars = (fillW - 10) / 6;
        if (maxChars < 1) maxChars = 1;
        char codeLine[16] = {0}; strncpy(codeLine, code, min(maxChars, 15));
        char roomLine[16] = {0}; strncpy(roomLine, room, min(maxChars, 15));

        int codeLx = x + 5 + (fillW - 5 - (int)strlen(codeLine) * 6) / 2;
        int roomLx = x + 5 + (fillW - 5 - (int)strlen(roomLine) * 6) / 2;

        vga.setTextColor(sc.text, sc.bg);
        vga.setCursor(codeLx, y + ROW_H / 2 - 10);
        vga.print(codeLine);
        vga.setCursor(roomLx, y + ROW_H / 2 + 2);
        vga.print(roomLine);

        s += actualSpan;
      }
    }
  }

  vga.fillRect(0, FOOTER_Y, 400, 300 - FOOTER_Y, C_BLUE);
  vga.fillRect(0, FOOTER_Y - 1, 400, 1, C_CYAN);
  leftText(8, FOOTER_Y + 4, "[ENTER:Menu]  [BACK:Section]", C_CYAN, C_BLUE);
}

// ========================================
// MENU
// ========================================
void drawMenu() {
  vga.clear(C_BLACK);
  drawHeader();
  drawFooter("UP/DOWN: Navigate   ENTER: Select");

  struct MenuItem {
    const char* label;
    int         accentColor;
    int         iconBg;
    Category*   cat;
  };

  MenuItem items[5] = {
    { "URGENT NOTICES",  C_RED,     C_WHITE, &urgent   },
    { "GENERAL NOTICES", C_GREEN,    C_WHITE, &general  },
    { "EVENT NOTICES",   C_YELLOW,  C_WHITE, &eventCat },
    { "TEACHER OFFICES", C_BLUE,   C_WHITE, nullptr   },
    { "TIME TABLE",      C_MAGENTA, C_WHITE, nullptr   },
  };

  const int CARD_X = 20;
  const int CARD_W = 360;
  const int CARD_H = 38;
  const int FIRST_Y = 62;
  const int GAP     = 4;

  for (int i = 0; i < 5; i++) {
    int y   = FIRST_Y + i * (CARD_H + GAP);
    bool sel = (i == selectedMenu);

    int cardBg = sel ? C_BLUE : C_WHITE;
    vga.fillRect(CARD_X, y, CARD_W, CARD_H, cardBg);
    drawBox(CARD_X, y, CARD_W, CARD_H, sel ? C_WHITE : C_CYAN);

    // Left accent stripe
    vga.fillRect(CARD_X, y, 3, CARD_H, items[i].accentColor);

    // Icon badge
    int iconBg = sel ? C_WHITE : items[i].iconBg;
    vga.fillRect(CARD_X + 8, y + 10, 16, 16, iconBg);
    drawBox(CARD_X + 8, y + 10, 16, 16, items[i].accentColor);
    char initBuf[2] = { items[i].label[0], '\0' };
    vga.setTextColor(items[i].accentColor, iconBg);
    vga.setCursor(CARD_X + 11, y + 14);
    vga.print(initBuf);

    // Label
    vga.setTextColor(sel ? C_WHITE : C_BLACK, cardBg);
    vga.setCursor(CARD_X + 30, y + 14);
    vga.print(items[i].label);

    // Count badge
    if (items[i].cat != nullptr) {
      char cnt[8]; sprintf(cnt, "(%d)", items[i].cat->count);
      int cntX = CARD_X + CARD_W - strlen(cnt)*6 - (items[i].cat->count > 0 ? 44 : 10);
      vga.setTextColor(sel ? C_CYAN : C_BLUE, cardBg);
      vga.setCursor(cntX, y + 14);
      vga.print(cnt);

      bool hasNew = false;
      for (int j = 0; j < items[i].cat->count; j++)
        if (items[i].cat->items[j].isNew) { hasNew = true; break; }
      if (hasNew) {
        int badgeX = CARD_X + CARD_W - 42;
        vga.fillRect(badgeX, y + 11, 30, 14, C_RED);
        vga.setTextColor(C_WHITE, C_RED);
        vga.setCursor(badgeX + 5, y + 14);
        vga.print("NEW");
      }
    }

    if (sel) {
      vga.setTextColor(C_WHITE, cardBg);
      vga.setCursor(CARD_X + CARD_W - 12, y + 14);
      vga.print(">");
    }
  }

  updateLEDs();
}

// ========================================
// ANNOUNCEMENT VIEWER
// ========================================
void showCategory(Category &cat, const char* title) {
  vga.clear(C_BLACK);
  drawHeader();
  drawFooter("UP/DOWN: Browse   BACK: Menu");

  if (cat.count == 0) {
    vga.fillRect(60, 100, 280, 100, C_WHITE);
    drawBox(60, 100, 280, 100, C_CYAN);
    vga.fillRect(60, 100, 280, 3, C_BLUE);
    centerText(138, "NO ANNOUNCEMENTS",         C_BLACK, C_WHITE);
    centerText(156, "Nothing in this category", C_BLUE,  C_WHITE);
    return;
  }

  if (cat.index >= cat.count) cat.index = 0;
  if (cat.index < 0)          cat.index = cat.count - 1;

  // Category bar
  vga.fillRect(10, 50, 380, 22, C_BLUE);
  drawBox(10, 50, 380, 22, C_CYAN);
  vga.fillRect(10, 50, 3, 22, C_YELLOW);
  vga.setTextColor(C_WHITE, C_BLUE);
  vga.setCursor(18, 56);
  vga.print(title);

  char idxBuf[20]; sprintf(idxBuf, "%d / %d", cat.index + 1, cat.count);
  int idxX = 390 - strlen(idxBuf) * 6 - 4;
  vga.setTextColor(C_WHITE, C_BLUE);
  vga.setCursor(idxX, 58);
  vga.print(idxBuf);

  // Title card
  vga.fillRect(10, 76, 380, 30, C_WHITE);
  drawBox(10, 76, 380, 30, C_CYAN);
  vga.fillRect(10, 76, 3, 30, C_BLUE);
  vga.setTextColor(C_BLACK, C_WHITE);
  char titleLine[60] = {0};
  strncpy(titleLine, cat.items[cat.index].title, 58);
  vga.setCursor(18, 86);
  vga.print(titleLine);

  // Message card
  vga.fillRect(10, 110, 380, 162, C_WHITE);
  drawBox(10, 110, 380, 162, C_CYAN);
  vga.fillRect(10, 110, 3, 162, C_CYAN);

  vga.setTextColor(C_BLUE, C_WHITE);
  vga.setCursor(18, 118);
  vga.print("MESSAGE");
  vga.fillRect(18, 127, 50, 1, C_BLUE);

  // Word-wrapped message rendering
  vga.setTextColor(C_BLACK, C_WHITE);
  const char* msg = cat.items[cat.index].message;

  const int TEXT_START_X = 18;
  const int TEXT_END_X   = 388;
  const int CHAR_W       = 6;
  const int MAX_CHARS    = (TEXT_END_X - TEXT_START_X) / CHAR_W;  // 61

  int lineY = 133;
  int pos   = 0;
  int len   = strlen(msg);

  while (pos < len && lineY < 266) {
    int remaining = len - pos;
    int take      = (remaining > MAX_CHARS) ? MAX_CHARS : remaining;

    // Walk back to last space to avoid cutting mid-word
    if (pos + take < len) {
      int breakAt = take;
      while (breakAt > 0 && msg[pos + breakAt] != ' ') breakAt--;
      if (breakAt > 0) take = breakAt;
    }

    char line[64] = {0};
    strncpy(line, msg + pos, take);
    vga.setCursor(TEXT_START_X, lineY);
    vga.print(line);

    pos += take;
    if (pos < len && msg[pos] == ' ') pos++;  // skip the space we broke on

    lineY += 14;
  }

  markNotifiedUser(cat.items[cat.index].id);
  cat.items[cat.index].isNew = false;
  updateLEDs();
}

// ========================================
// GENERIC SELECTION LIST
// ========================================
void drawSelectList(const char* pageTitle, const char* subTitle,
                    int selIdx, int scrollOff, int total,
                    int accentColor,
                    const char* (*getName)(int)) {
  vga.clear(C_BLACK);
  drawHeader();
  drawFooter("UP/DOWN: Navigate   ENTER: Select   BACK: Go Back");

  vga.fillRect(10, 50, 380, 20, C_WHITE);
  drawBox(10, 50, 380, 20, C_CYAN);
  vga.fillRect(10, 50, 3, 20, accentColor);

  vga.setTextColor(C_BLUE, C_WHITE);
  vga.setCursor(18, 55);
  vga.print(pageTitle);
  if (subTitle && strlen(subTitle) > 0) {
    vga.setTextColor(C_BLACK, C_WHITE);
    int subX = 390 - strlen(subTitle)*6 - 8;
    vga.setCursor(subX, 55);
    vga.print(subTitle);
  }

  if (total == 0) {
    vga.fillRect(40, 100, 320, 60, C_WHITE);
    drawBox(40, 100, 320, 60, C_CYAN);
    centerText(124, "No items found", C_BLUE, C_WHITE);
    return;
  }

  const int VISIBLE = 5;
  const int ITEM_H  = 37;
  const int ITEM_X  = 20;
  const int ITEM_W  = 360;
  const int FIRST_Y = 76;

  if (scrollOff > 0)
    centerText(71, "^ more above", C_CYAN, C_BLACK);

  int end = min(scrollOff + VISIBLE, total);
  for (int i = scrollOff; i < end; i++) {
    int  y   = FIRST_Y + (i - scrollOff) * (ITEM_H + 3);
    bool sel = (i == selIdx);

    int bgColor = sel ? C_BLUE : C_WHITE;
    vga.fillRect(ITEM_X, y, ITEM_W, ITEM_H, bgColor);
    drawBox(ITEM_X, y, ITEM_W, ITEM_H, sel ? C_WHITE : C_CYAN);
    vga.fillRect(ITEM_X, y, 3, ITEM_H, accentColor);

    const char* name = getName(i);
    int nameLen = strlen(name);
    int nameX   = ITEM_X + 12 + (ITEM_W - 14 - nameLen*6) / 2;
    if (nameX < ITEM_X + 12) nameX = ITEM_X + 12;
    vga.setTextColor(sel ? C_WHITE : C_BLACK, bgColor);
    vga.setCursor(nameX, y + 13);
    vga.print(name);

    if (sel) {
      vga.setTextColor(C_WHITE, bgColor);
      vga.setCursor(ITEM_X + ITEM_W - 14, y + 13);
      vga.print(">");
    }
  }

  if (scrollOff + VISIBLE < total)
    centerText(276, "v more below", C_CYAN, C_BLACK);
}

const char* getYearName(int i)    { return years[i].name; }
const char* getSessionName(int i) { return sessions[i].name; }
const char* getSectionName(int i) { return sections[i].name; }

void drawYearSelect() {
  char sub[30]; sprintf(sub, "%d total", totalYears);
  drawSelectList("SELECT YEAR / BATCH", sub, selYear, yearScrollOff, totalYears, C_BLUE, getYearName);
}

void drawSessionSelect() {
  char hdr[30]; sprintf(hdr, "%s", years[selYear].name);
  drawSelectList("SELECT SESSION", hdr, selSession, sessionScrollOff, totalSessions, C_GREEN, getSessionName);
}

void drawSectionSelect() {
  char hdr[40]; sprintf(hdr, "%s", sessions[selSession].name);
  drawSelectList("SELECT SECTION", hdr, selSection, sectionScrollOff, totalSections, C_YELLOW, getSectionName);
}

// ========================================
// FLOOR SELECT
// ========================================
void drawOfficeFloorSelect() {
  vga.clear(C_BLACK);
  drawHeader();
  drawFooter("UP/DOWN: Navigate   ENTER: Select   BACK: Menu");

  vga.fillRect(10, 50, 380, 20, C_WHITE);
  drawBox(10, 50, 380, 20, C_CYAN);
  vga.fillRect(10, 50, 3, 20, C_GREEN);
  vga.setTextColor(C_BLUE, C_WHITE);
  vga.setCursor(18, 55);
  vga.print("TEACHER OFFICES -- SELECT FLOOR");

  int accents[2] = { C_BLUE, C_GREEN };
  for (int i = 0; i < OFFICE_FLOOR_COUNT; i++) {
    int  y   = 82 + i * 50;
    bool sel = (i == selOfficeFloor);
    int  bg  = sel ? C_BLUE : C_WHITE;
    vga.fillRect(30, y, 340, 42, bg);
    drawBox(30, y, 340, 42, sel ? C_WHITE : C_CYAN);
    vga.fillRect(30, y, 3, 42, accents[i]);
    centerText(y + 15, OFFICE_FLOORS[i], sel ? C_WHITE : C_BLACK, bg);
    if (sel) {
      vga.setTextColor(C_WHITE, bg);
      vga.setCursor(360, y + 15);
      vga.print(">");
    }
  }
}

// ========================================
// GROUND FLOOR: TEACHER LIST
// ========================================
void drawOfficeTeacherSelect() {
  vga.clear(C_BLACK);
  drawHeader();
  drawFooter("UP/DOWN: Navigate   ENTER: View Map   BACK: Floors");

  vga.fillRect(10, 50, 380, 20, C_WHITE);
  drawBox(10, 50, 380, 20, C_CYAN);
  vga.fillRect(10, 50, 3, 20, C_BLUE);
  vga.setTextColor(C_BLUE, C_WHITE);
  vga.setCursor(18, 55);
  vga.print("GROUND FLOOR -- OFFICE TEACHERS");

  if (!officeTeacherLoaded) {
    vga.fillRect(40, 100, 320, 60, C_WHITE);
    drawBox(40, 100, 320, 60, C_CYAN);
    centerText(124, "Loading...", C_BLUE, C_WHITE);
    return;
  }

  const int VISIBLE = 5;
  const int ITEM_H  = 36;
  const int ITEM_X  = 15;
  const int ITEM_W  = 370;
  int end = min(officeTeacherScroll + VISIBLE, OFFICE_ROOM_COUNT);

  if (officeTeacherScroll > 0) centerText(73, "^ more above", C_CYAN, C_BLACK);

  for (int i = officeTeacherScroll; i < end; i++) {
    int  y   = 76 + (i - officeTeacherScroll) * (ITEM_H + 3);
    bool sel = (i == selOfficeTeacher);
    int  bg  = sel ? C_BLUE : C_WHITE;
    vga.fillRect(ITEM_X, y, ITEM_W, ITEM_H, bg);
    drawBox(ITEM_X, y, ITEM_W, ITEM_H, sel ? C_WHITE : C_CYAN);
    vga.fillRect(ITEM_X, y, 3, ITEM_H, C_BLUE);

    char roomLabel[10]; sprintf(roomLabel, "R%d", i + 1);
    vga.setTextColor(sel ? C_CYAN : C_BLUE, bg);
    vga.setCursor(ITEM_X + 8, y + 13);
    vga.print(roomLabel);

    vga.setTextColor(sel ? C_WHITE : C_BLACK, bg);
    int nameLen = strlen(officeTeacher[i]);
    int nameX   = ITEM_X + 30 + (ITEM_W - 30 - nameLen*6) / 2;
    if (nameX < ITEM_X + 30) nameX = ITEM_X + 30;
    vga.setCursor(nameX, y + 13);
    vga.print(officeTeacher[i]);

    if (sel) {
      vga.setTextColor(C_WHITE, bg);
      vga.setCursor(ITEM_X + ITEM_W - 14, y + 13);
      vga.print(">");
    }
  }

  if (officeTeacherScroll + VISIBLE < OFFICE_ROOM_COUNT)
    centerText(278, "v more below", C_CYAN, C_BLACK);
}

// ========================================
// GROUND FLOOR: MAP
// ========================================
void drawOfficeMap() {
  vga.clear(C_BLACK);
  vga.fillRect(0, 0, 400, 30, C_BLUE);
  vga.fillRect(0, 0, 400, 3, C_YELLOW);
  vga.fillRect(0, 29, 400, 1, C_CYAN);
  vga.setTextColor(C_CYAN, C_BLUE);
  vga.setCursor(8, 8);
  vga.print("GROUND FLOOR");
  centerText(14, "TEACHER OFFICE PLAN", C_WHITE, C_BLUE);
  const int pairLeft[3]  = { 0, 2, 4 };
  const int pairRight[3] = { 1, 3, 5 };
  const int BOX_W      = 188;
  const int BOX_H      = 78;
  const int LEFT_X     = 3;
  const int RIGHT_X    = 208;
  const int FIRST_Y    = 34;
  const int ROW_STRIDE = 82;
  for (int p = 0; p < 3; p++) {
    int boxY = FIRST_Y + p * ROW_STRIDE;
    for (int side = 0; side < 2; side++) {
      int  slot = (side == 0) ? pairLeft[p] : pairRight[p];
      int  boxX = (side == 0) ? LEFT_X : RIGHT_X;
      bool isHL = (slot == officeHighlightRoom);
      int bgC    = isHL ? C_BLUE  : C_BLACK;   // BLACK background
      int border = isHL ? C_WHITE : C_CYAN;
      int textC  = isHL ? C_WHITE : C_WHITE;   // WHITE text on black
      int dimC   = isHL ? C_CYAN  : C_CYAN;    // CYAN label on black
      vga.fillRect(boxX, boxY, BOX_W, BOX_H, bgC);
      drawBox(boxX, boxY, BOX_W, BOX_H, border);
      vga.fillRect(boxX, boxY, BOX_W, 3, isHL ? C_YELLOW : C_BLUE);
      if (isHL) drawBox(boxX + 2, boxY + 3, BOX_W - 4, BOX_H - 5, C_CYAN);
      char rLabel[8]; sprintf(rLabel, "Room %d", slot + 1);
      vga.setTextColor(dimC, bgC);
      vga.setCursor(boxX + 4, boxY + 6);
      vga.print(rLabel);
      // Person icon
      int personX = boxX + BOX_W / 2 - 8;
      int personY = boxY + 20;
      vga.fillRect(personX + 3, personY,      10, 10, isHL ? C_YELLOW : C_CYAN);  // HEAD: CYAN
      vga.fillRect(personX,     personY + 11, 16, 14, C_CYAN);                    // BODY
      vga.fillRect(personX - 4, personY + 13,  4,  8, C_CYAN);                    // LEFT ARM
      vga.fillRect(personX +16, personY + 13,  4,  8, C_CYAN);                    // RIGHT ARM
      const char* tname = officeTeacher[slot];
      int maxChars = (BOX_W - 8) / 6;
      char nameDisplay[28] = {0};
      strncpy(nameDisplay, tname, min(maxChars, 27));
      int tnameLen = strlen(nameDisplay);
      int tnameX   = boxX + (BOX_W - tnameLen * 6) / 2;
      if (tnameX < boxX + 4) tnameX = boxX + 4;
      vga.setTextColor(textC, bgC);
      vga.setCursor(tnameX, boxY + BOX_H - 13);
      vga.print(nameDisplay);
    }
    // Corridor
    vga.fillRect(LEFT_X + BOX_W + 2, boxY + 8,
                 RIGHT_X - (LEFT_X + BOX_W + 4), BOX_H - 16, C_BLACK);
    vga.setTextColor(C_BLUE, C_BLACK);
    vga.setCursor(LEFT_X + BOX_W + 2, boxY + BOX_H/2 - 4);
    vga.print("||");
  }
  vga.fillRect(0, 286, 400, 14, C_BLUE);
  vga.fillRect(0, 285, 400, 1, C_CYAN);
  leftText(8, 290, "[ENTER: Main Menu]  [BACK: Teacher List]", C_CYAN, C_BLUE);
}

// ========================================
// FIRST FLOOR: TEACHER LIST
// ========================================
void drawFFTeacherList() {
  vga.clear(C_BLACK);
  drawHeader();
  drawFooter("UP/DOWN: Navigate   ENTER: Locate   BACK: Floors");

  vga.fillRect(10, 50, 380, 20, C_WHITE);
  drawBox(10, 50, 380, 20, C_CYAN);
  vga.fillRect(10, 50, 3, 20, C_GREEN);
  vga.setTextColor(C_BLUE, C_WHITE);
  vga.setCursor(18, 55);
  vga.print("FIRST FLOOR -- OFFICE TEACHERS");

  if (!ffTeacherLoaded) {
    vga.fillRect(40, 100, 320, 60, C_WHITE);
    drawBox(40, 100, 320, 60, C_CYAN);
    centerText(124, "Loading...", C_BLUE, C_WHITE);
    return;
  }
  if (ffTeacherCount == 0) {
    vga.fillRect(40, 100, 320, 60, C_WHITE);
    drawBox(40, 100, 320, 60, C_CYAN);
    centerText(124, "No teachers found", C_BLUE, C_WHITE);
    return;
  }

  const int VISIBLE = 5;
  const int ITEM_H  = 36;
  const int ITEM_X  = 15;
  const int ITEM_W  = 370;
  int end = min(ffTeacherScroll + VISIBLE, ffTeacherCount);

  if (ffTeacherScroll > 0) centerText(73, "^ more above", C_CYAN, C_BLACK);

  for (int i = ffTeacherScroll; i < end; i++) {
    int  y   = 76 + (i - ffTeacherScroll) * (ITEM_H + 3);
    bool sel = (i == selFFTeacher);
    int  bg  = sel ? C_BLUE : C_WHITE;
    vga.fillRect(ITEM_X, y, ITEM_W, ITEM_H, bg);
    drawBox(ITEM_X, y, ITEM_W, ITEM_H, sel ? C_WHITE : C_CYAN);
    vga.fillRect(ITEM_X, y, 3, ITEM_H, C_GREEN);

    vga.setTextColor(sel ? C_WHITE : C_BLACK, bg);
    int nameLen = strlen(ffTeachers[i].name);
    int nameX   = ITEM_X + 12 + (ITEM_W - 12 - nameLen*6) / 2;
    if (nameX < ITEM_X + 12) nameX = ITEM_X + 12;
    vga.setCursor(nameX, y + 13);
    vga.print(ffTeachers[i].name);

    char roomLabel[12];
    if (ffTeachers[i].cabinNumber > 0)
      sprintf(roomLabel, "C%d R%d", ffTeachers[i].cabinNumber, ffTeachers[i].roomNumber);
    else
      sprintf(roomLabel, "Rm %d", ffTeachers[i].roomNumber);
    vga.setTextColor(sel ? C_CYAN : C_BLUE, bg);
    vga.setCursor(ITEM_X + ITEM_W - strlen(roomLabel)*6 - 18, y + 13);
    vga.print(roomLabel);

    if (sel) {
      vga.setTextColor(C_WHITE, bg);
      vga.setCursor(ITEM_X + ITEM_W - 14, y + 13);
      vga.print(">");
    }
  }

  if (ffTeacherScroll + VISIBLE < ffTeacherCount)
    centerText(278, "v more below", C_CYAN, C_BLACK);
}

// ========================================
// FIRST FLOOR: FLOOR MAP
// ========================================
void drawFFFloorMap() {
  vga.clear(C_BLACK);

  vga.fillRect(0, 0, 400, 30, C_BLUE);
  vga.fillRect(0, 0, 400, 3, C_YELLOW);
  vga.fillRect(0, 29, 400, 1, C_CYAN);
  vga.setTextColor(C_CYAN, C_BLUE);
  vga.setCursor(8, 8);
  vga.print("FIRST FLOOR");
  centerText(14, "FLOOR PLAN", C_WHITE, C_BLUE);

  drawFooter("[ENTER: Open Cabin]  [BACK: Teacher List]");

  const int SR_W = 118;
  const int SR_H = 34;
  const int CB_W = 106;
  const int CB_H = 34;

  const int ROW0_Y = 49;
  const int ROW1_Y = 90;
  const int ROW2_Y = 164;
  const int ROW3_Y = 208;

  const int LEFT_X     = 4;
  const int CABIN2_X   = 288;
  const int ROW3_C3_X  = 4;
  const int ROW3_R19_X = 128;
  const int ROW3_R20_X = 266;

  bool hlRoom13 = (ffHighlightCabin==0 && ffHighlightRoom==0);
  bool hlRoom14 = (ffHighlightCabin==0 && ffHighlightRoom==1);
  bool hlRoom15 = (ffHighlightCabin==0 && ffHighlightRoom==2);
  bool hlRoom19 = (ffHighlightCabin==0 && ffHighlightRoom==3);
  bool hlRoom20 = (ffHighlightCabin==0 && ffHighlightRoom==4);
  bool hlCabin2 = (ffHighlightCabin==2);
  bool hlCabin3 = (ffHighlightCabin==3);

  auto drawRoomCard = [&](int bx, int by, int bw, int bh,
                          bool isHL, bool isCabin,
                          const char* label, const char* teacher,
                          int accentC) {
    int bgC    = isHL ? C_BLUE  : C_WHITE;
    int border = isHL ? C_WHITE : C_BLUE;
    int labelC = isHL ? C_WHITE : C_BLACK;
    int nameC  = isHL ? C_CYAN  : C_BLUE;

    vga.fillRect(bx, by, bw, bh, bgC);
    drawBox(bx, by, bw, bh, border);
    vga.fillRect(bx, by, bw, 3, isHL ? C_YELLOW : accentC);
    if (isHL) drawBox(bx+2, by+3, bw-4, bh-5, C_CYAN);

    int lblLen = strlen(label);
    int lblX   = bx + (bw - lblLen*6) / 2;
    if (lblX < bx+2) lblX = bx+2;
    vga.setTextColor(labelC, bgC);
    vga.setCursor(lblX, by + 7);
    vga.print(label);

    int maxCh = (bw - 6) / 6;
    char tname[28] = {0};
    strncpy(tname, teacher, min(maxCh, 27));
    int tnameLen = strlen(tname);
    int tnameX   = bx + (bw - tnameLen*6) / 2;
    if (tnameX < bx+2) tnameX = bx+2;
    vga.setTextColor(nameC, bgC);
    vga.setCursor(tnameX, by + bh - 12);
    vga.print(tname);
  };

  drawRoomCard(LEFT_X, ROW0_Y, SR_W, SR_H, hlRoom13, false, "Room 13", ffStandaloneTeacher[0], C_BLUE);

  vga.fillRect(LEFT_X + SR_W + 2, ROW0_Y + 4,
               CABIN2_X - (LEFT_X + SR_W + 4), SR_H - 8, C_BLACK);
  vga.setTextColor(C_CYAN, C_BLACK);
  vga.setCursor(LEFT_X + SR_W + 10, ROW0_Y + 12);
  vga.print("CORRIDOR");

  drawRoomCard(CABIN2_X, ROW0_Y, CB_W, CB_H, hlCabin2, true, "CABIN 2", hlCabin2 ? "[ ENTER ]" : "Rms 7-12", C_BLUE);

  drawRoomCard(LEFT_X, ROW1_Y, SR_W, SR_H, hlRoom14, false, "Room 14", ffStandaloneTeacher[1], C_BLUE);

  vga.fillRect(LEFT_X + SR_W, ROW0_Y + SR_H, 1, ROW2_Y - (ROW0_Y + SR_H), C_CYAN);

  vga.setTextColor(C_CYAN, C_BLACK);
  vga.setCursor(4, ROW2_Y - 9);
  vga.print("- - - - - - - - - - - - - - - - - - - - -");

  drawRoomCard(LEFT_X, ROW2_Y, SR_W, SR_H, hlRoom15, false, "Room 15", ffStandaloneTeacher[2], C_BLUE);

  drawRoomCard(ROW3_C3_X, ROW3_Y, CB_W - 10, CB_H, hlCabin3, true, "CABIN 3", hlCabin3 ? "[ ENTER ]" : "Rms 16-18", C_BLUE);
  drawRoomCard(ROW3_R19_X, ROW3_Y, SR_W, SR_H, hlRoom19, false, "Room 19", ffStandaloneTeacher[3], C_BLUE);
  drawRoomCard(ROW3_R20_X, ROW3_Y, SR_W, SR_H, hlRoom20, false, "Room 20", ffStandaloneTeacher[4], C_BLUE);

  // Stairs
  vga.fillRect(ROW3_R20_X + SR_W + 2, ROW3_Y, 84, CB_H, C_BLACK);
  drawBox(ROW3_R20_X + SR_W + 2, ROW3_Y, 84, CB_H, C_BLUE);
  int stX = ROW3_R20_X + SR_W + 2;
  vga.setTextColor(C_CYAN, C_BLACK);
  vga.setCursor(stX + 16, ROW3_Y + 6);
  vga.print("STAIRS");
  for (int step = 0; step < 4; step++)
    vga.fillRect(stX + 6 + step*16, ROW3_Y + 20, 14, 3, C_CYAN);

  char hint[50];
  if (ffHighlightCabin == 2)      sprintf(hint, "CABIN 2 selected -- press ENTER to open");
  else if (ffHighlightCabin == 3) sprintf(hint, "CABIN 3 selected -- press ENTER to open");
  else                            sprintf(hint, "Room highlighted above");

  vga.fillRect(4, 255, 392, 14, C_BLACK);
  drawBox(4, 255, 392, 14, C_CYAN);
  vga.fillRect(4, 255, 3, 14, C_CYAN);
  vga.setTextColor(C_WHITE, C_BLACK);
  int hx = 10 + (390 - strlen(hint)*6) / 2;
  if (hx < 10) hx = 10;
  vga.setCursor(hx, 259);
  vga.print(hint);
}

// ========================================
// CABIN 2 MAP
// ========================================
void drawCabin2Map() {
  vga.clear(C_BLACK);

  vga.fillRect(0, 0, 400, 30, C_BLUE);
  vga.fillRect(0, 0, 400, 3, C_YELLOW);
  vga.fillRect(0, 29, 400, 1, C_CYAN);
  vga.setTextColor(C_CYAN, C_BLUE);
  vga.setCursor(8, 8);
  vga.print("FIRST FLOOR  /  CABIN 2");
  centerText(14, "ROOMS 7 - 12", C_WHITE, C_BLUE);

  drawFooter("[BACK: Floor Map]");

  const int pairLeft[3]  = { 0, 2, 4 };
  const int pairRight[3] = { 1, 3, 5 };
  const int BOX_W      = 188;
  const int BOX_H      = 78;
  const int LEFT_X     = 3;
  const int RIGHT_X    = 207;
  const int FIRST_Y    = 34;
  const int ROW_STRIDE = 82;

  for (int p = 0; p < 3; p++) {
    int boxY = FIRST_Y + p * ROW_STRIDE;
    for (int side = 0; side < 2; side++) {
      int  slot = (side == 0) ? pairLeft[p] : pairRight[p];
      int  boxX = (side == 0) ? LEFT_X : RIGHT_X;
      bool isHL = (slot == ffHighlightRoom);

      int bgC    = isHL ? C_BLUE  : C_BLACK;   // BLACK background
      int border = isHL ? C_WHITE : C_CYAN;
      int textC  = isHL ? C_WHITE : C_WHITE;   // WHITE text on black
      int dimC   = isHL ? C_CYAN  : C_CYAN;    // CYAN label on black

      vga.fillRect(boxX, boxY, BOX_W, BOX_H, bgC);
      drawBox(boxX, boxY, BOX_W, BOX_H, border);
      vga.fillRect(boxX, boxY, BOX_W, 3, isHL ? C_YELLOW : C_BLUE);
      if (isHL) drawBox(boxX+2, boxY+3, BOX_W-4, BOX_H-5, C_CYAN);

      char rLabel[10]; sprintf(rLabel, "Room %d", CABIN2_SLOT_TO_ROOM[slot]);
      vga.setTextColor(dimC, bgC);
      vga.setCursor(boxX + 4, boxY + 6);
      vga.print(rLabel);

      // Person icon
      int pX = boxX + BOX_W/2 - 7;
      int pY = boxY + 18;
      vga.fillRect(pX+2, pY,    10, 9,  isHL ? C_YELLOW : C_CYAN);  // HEAD: CYAN
      vga.fillRect(pX,   pY+10, 14, 12, C_CYAN);                    // BODY
      vga.fillRect(pX-3, pY+12,  3,  7, C_CYAN);                    // LEFT ARM
      vga.fillRect(pX+14,pY+12,  3,  7, C_CYAN);                    // RIGHT ARM

      const char* tname = cabin2Teacher[slot];
      int maxCh = (BOX_W - 8) / 6;
      char nameDisplay[28]={0}; strncpy(nameDisplay, tname, min(maxCh,27));
      int tnLen = strlen(nameDisplay);
      int tnX   = boxX + (BOX_W - tnLen*6) / 2;
      if (tnX < boxX+4) tnX = boxX+4;
      vga.setTextColor(textC, bgC);
      vga.setCursor(tnX, boxY + BOX_H - 12);
      vga.print(nameDisplay);
    }

    // Corridor
    vga.fillRect(LEFT_X + BOX_W + 1, boxY + 6,
                 RIGHT_X - (LEFT_X + BOX_W + 2), BOX_H - 12, C_BLACK);
    vga.setTextColor(C_BLUE, C_BLACK);
    vga.setCursor(LEFT_X + BOX_W + 2, boxY + BOX_H/2 - 4);
    vga.print("||");
  }
}

// ========================================
// CABIN 3 MAP
// ========================================
void drawCabin3Map() {
  vga.clear(C_BLACK);

  vga.fillRect(0, 0, 400, 30, C_BLUE);
  vga.fillRect(0, 0, 400, 3, C_YELLOW);
  vga.fillRect(0, 29, 400, 1, C_CYAN);
  vga.setTextColor(C_CYAN, C_BLUE);
  vga.setCursor(8, 8);
  vga.print("FIRST FLOOR  /  CABIN 3");
  centerText(14, "ROOMS 16 - 18", C_WHITE, C_BLUE);

  drawFooter("[BACK: Floor Map]");

  const int BOX_W   = 270;
  const int BOX_H   = 78;
  const int BOX_X   = (400 - BOX_W) / 2;
  const int FIRST_Y = 38;
  const int STRIDE  = 82;

  for (int slot = 0; slot < CABIN3_ROOM_COUNT; slot++) {
    int  boxY = FIRST_Y + slot * STRIDE;
    bool isHL = (slot == ffHighlightRoom);

    int bgC    = isHL ? C_BLUE  : C_BLACK;   // BLACK background
    int border = isHL ? C_WHITE : C_CYAN;
    int textC  = isHL ? C_WHITE : C_WHITE;   // WHITE text on black
    int dimC   = isHL ? C_CYAN  : C_CYAN;    // CYAN label on black

    vga.fillRect(BOX_X, boxY, BOX_W, BOX_H, bgC);
    drawBox(BOX_X, boxY, BOX_W, BOX_H, border);
    vga.fillRect(BOX_X, boxY, BOX_W, 3, isHL ? C_YELLOW : C_BLUE);
    if (isHL) drawBox(BOX_X+2, boxY+3, BOX_W-4, BOX_H-5, C_CYAN);

    char rLabel[10]; sprintf(rLabel, "Room %d", CABIN3_SLOT_TO_ROOM[slot]);
    vga.setTextColor(dimC, bgC);
    vga.setCursor(BOX_X + 4, boxY + 6);
    vga.print(rLabel);

    // Person icon
    int pX = BOX_X + BOX_W/2 - 7;
    int pY = boxY + 16;
    vga.fillRect(pX+2, pY,    10, 9,  isHL ? C_YELLOW : C_CYAN);  // HEAD: CYAN
    vga.fillRect(pX,   pY+10, 14, 12, C_CYAN);                    // BODY

    const char* tname = cabin3Teacher[slot];
    int maxCh = (BOX_W - 8) / 6;
    char nameDisplay[38]={0}; strncpy(nameDisplay, tname, min(maxCh,37));
    int tnLen = strlen(nameDisplay);
    int tnX   = BOX_X + (BOX_W - tnLen*6) / 2;
    if (tnX < BOX_X+4) tnX = BOX_X+4;
    vga.setTextColor(textC, bgC);
    vga.setCursor(tnX, boxY + BOX_H - 12);
    vga.print(nameDisplay);
  }
}

// ========================================
// MQTT
// ========================================
void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg = "";
  for (int i = 0; i < (int)len; i++) msg += (char)payload[i];
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, msg)) return;
  const char* action   = doc["action"]          | "";
  int         annId    = doc["announcement_id"] | 0;
  const char* title    = doc["title"]           | "";
  const char* message  = doc["message"]         | "";
  const char* category = doc["category"]        | "";
  if (strcmp(action, "deleted") == 0) { fetchAnnouncements(); return; }
  if (!hasSeenAnnouncement(annId)) {
    if      (strcmp(category, "urgent")  == 0) { addToCategory(urgent,   annId, title, message, true); beep(3); }
    else if (strcmp(category, "general") == 0) { addToCategory(general,  annId, title, message, true); beep(1); }
    else if (strcmp(category, "event")   == 0) { addToCategory(eventCat, annId, title, message, true); beep(2); }
    markAnnouncementAsSeen(annId);

    // ADD THIS — if user is already viewing that category, jump to the new one
    if (currentState == URGENT_VIEW  && strcmp(category, "urgent")  == 0) showCategory(urgent,   "URGENT NOTICE");
    if (currentState == GENERAL_VIEW && strcmp(category, "general") == 0) showCategory(general,  "GENERAL NOTICE");
    if (currentState == EVENT_VIEW   && strcmp(category, "event")   == 0) showCategory(eventCat, "EVENT NOTICE");
  }
  updateLEDs();
  if (currentState == MENU) drawMenu();
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (client.connected()) return;
  if (millis() - lastMQTTAttempt < MQTT_RETRY_INTERVAL) return;
  lastMQTTAttempt = millis();
  if (mqttConnectionAttempts >= MAX_MQTT_ATTEMPTS) { mqttConnectionAttempts = 0; return; }
  char clientId[30];
  uint64_t mac = ESP.getEfuseMac();
  sprintf(clientId, "NoticeBrd_%04X%04X", (uint16_t)(mac >> 32), (uint16_t)(mac & 0xFFFF));
  if (client.connect(clientId)) {
    client.subscribe("department/notices");
    mqttConnectionAttempts = 0;
  } else {
    mqttConnectionAttempts++;
  }
}

// ========================================
// BUTTON HANDLING
// ========================================
void handleButtons() {
  if (millis() - lastPress < DEBOUNCE) return;
  bool up    = (digitalRead(upBtn)    == LOW);
  bool down  = (digitalRead(downBtn)  == LOW);
  bool enter = (digitalRead(enterBtn) == LOW);
  bool back  = (digitalRead(backBtn)  == LOW);
  if (!up && !down && !enter && !back) return;
  lastPress = millis();

  if (currentState == MENU) {
    if (up)   { selectedMenu--; if (selectedMenu < 0) selectedMenu = 4; drawMenu(); }
    if (down) { selectedMenu++; if (selectedMenu > 4) selectedMenu = 0; drawMenu(); }
    if (enter) {
      if      (selectedMenu == 0) { currentState = URGENT_VIEW;  showCategory(urgent,   "URGENT NOTICE");  }
      else if (selectedMenu == 1) { currentState = GENERAL_VIEW; showCategory(general,  "GENERAL NOTICE"); }
      else if (selectedMenu == 2) { currentState = EVENT_VIEW;   showCategory(eventCat, "EVENT NOTICE");   }
      else if (selectedMenu == 3) {
        selOfficeFloor = 0; officeFloorScroll = 0;
        currentState = OFFICE_FLOOR; drawOfficeFloorSelect();
      }
      else if (selectedMenu == 4) {
        if (selYear >= totalYears && totalYears > 0) selYear = 0;
        yearScrollOff = (selYear >= 5) ? selYear - 4 : 0;
        currentState = TT_YEAR;
        showLoading("Loading years..."); fetchYears(); drawYearSelect();
      }
    }
  }
  else if (currentState == URGENT_VIEW) {
    if (up)   { urgent.index--; if (urgent.index < 0) urgent.index = urgent.count - 1; showCategory(urgent, "URGENT NOTICE"); }
    if (down) { urgent.index++; if (urgent.index >= urgent.count) urgent.index = 0;    showCategory(urgent, "URGENT NOTICE"); }
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == GENERAL_VIEW) {
    if (up)   { general.index--; if (general.index < 0) general.index = general.count - 1; showCategory(general, "GENERAL NOTICE"); }
    if (down) { general.index++; if (general.index >= general.count) general.index = 0;    showCategory(general, "GENERAL NOTICE"); }
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == EVENT_VIEW) {
    if (up)   { eventCat.index--; if (eventCat.index < 0) eventCat.index = eventCat.count - 1; showCategory(eventCat, "EVENT NOTICE"); }
    if (down) { eventCat.index++; if (eventCat.index >= eventCat.count) eventCat.index = 0;    showCategory(eventCat, "EVENT NOTICE"); }
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == OFFICE_VIEW) {
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == TT_YEAR) {
    if (up)    { if (selYear > 0) { selYear--; if (selYear < yearScrollOff) yearScrollOff = selYear; } drawYearSelect(); }
    if (down)  { if (selYear < totalYears-1) { selYear++; if (selYear >= yearScrollOff+5) yearScrollOff=selYear-4; } drawYearSelect(); }
    if (enter) {
      sessionScrollOff = (selSession >= 5) ? selSession-4 : 0;
      showLoading("Loading sessions..."); fetchSessions(years[selYear].id);
      if (selSession >= totalSessions) { selSession = 0; sessionScrollOff = 0; }
      currentState = TT_SESSION; drawSessionSelect();
    }
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == TT_SESSION) {
    if (up)    { if (selSession > 0) { selSession--; if (selSession < sessionScrollOff) sessionScrollOff=selSession; } drawSessionSelect(); }
    if (down)  { if (selSession < totalSessions-1) { selSession++; if (selSession >= sessionScrollOff+5) sessionScrollOff=selSession-4; } drawSessionSelect(); }
    if (enter) {
      sectionScrollOff = (selSection >= 5) ? selSection-4 : 0;
      showLoading("Loading sections..."); fetchSections(sessions[selSession].id);
      if (selSection >= totalSections) { selSection = 0; sectionScrollOff = 0; }
      currentState = TT_SECTION; drawSectionSelect();
    }
    if (back) { currentState = TT_YEAR; drawYearSelect(); }
  }
  else if (currentState == TT_SECTION) {
    if (up)    { if (selSection > 0) { selSection--; if (selSection < sectionScrollOff) sectionScrollOff=selSection; } drawSectionSelect(); }
    if (down)  { if (selSection < totalSections-1) { selSection++; if (selSection >= sectionScrollOff+5) sectionScrollOff=selSection-4; } drawSectionSelect(); }
    if (enter) { showLoading("Loading timetable..."); fetchTimetable(selYear,selSession,selSection); currentState=TT_DISPLAY; drawTimetable(); }
    if (back)  { currentState = TT_SESSION; drawSessionSelect(); }
  }
  else if (currentState == TT_DISPLAY) {
    if (enter) { currentState = MENU; drawMenu(); }
    if (back)  { currentState = TT_SECTION; drawSectionSelect(); }
  }
  else if (currentState == OFFICE_FLOOR) {
    if (up)   { if (selOfficeFloor > 0) selOfficeFloor--; drawOfficeFloorSelect(); }
    if (down) { if (selOfficeFloor < OFFICE_FLOOR_COUNT-1) selOfficeFloor++; drawOfficeFloorSelect(); }
    if (enter) {
      if (selOfficeFloor == 0) {
        showLoading("Loading office data..."); fetchOfficeTeachers(OFFICE_FLOORS[0]);
        selOfficeTeacher = 0; officeTeacherScroll = 0;
        currentState = OFFICE_TEACHERS; drawOfficeTeacherSelect();
      } else {
        showLoading("Loading first floor data..."); fetchFirstFloorTeachers();
        selFFTeacher = 0; ffTeacherScroll = 0;
        ffHighlightCabin = 0; ffHighlightRoom = -1;
        currentState = FF_TEACHER_LIST; drawFFTeacherList();
      }
    }
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == OFFICE_TEACHERS) {
    if (up)   { if (selOfficeTeacher > 0) { selOfficeTeacher--; if (selOfficeTeacher < officeTeacherScroll) officeTeacherScroll=selOfficeTeacher; } drawOfficeTeacherSelect(); }
    if (down) { if (selOfficeTeacher < OFFICE_ROOM_COUNT-1) { selOfficeTeacher++; if (selOfficeTeacher >= officeTeacherScroll+5) officeTeacherScroll=selOfficeTeacher-4; } drawOfficeTeacherSelect(); }
    if (enter) { officeHighlightRoom = selOfficeTeacher; currentState = OFFICE_MAP; drawOfficeMap(); }
    if (back)  { currentState = OFFICE_FLOOR; drawOfficeFloorSelect(); }
  }
  else if (currentState == OFFICE_MAP) {
    if (enter) { officeHighlightRoom = -1; currentState = MENU; drawMenu(); }
    if (back)  { officeHighlightRoom = -1; currentState = OFFICE_TEACHERS; drawOfficeTeacherSelect(); }
  }
  else if (currentState == FF_TEACHER_LIST) {
    if (up)   { if (selFFTeacher > 0) { selFFTeacher--; if (selFFTeacher < ffTeacherScroll) ffTeacherScroll=selFFTeacher; } drawFFTeacherList(); }
    if (down) { if (selFFTeacher < ffTeacherCount-1) { selFFTeacher++; if (selFFTeacher >= ffTeacherScroll+5) ffTeacherScroll=selFFTeacher-4; } drawFFTeacherList(); }
    if (enter) {
      ffSelectedTeacherIdx = selFFTeacher;
      resolveFFTeacherHighlight(selFFTeacher);
      currentState = FF_FLOOR_MAP; drawFFFloorMap();
    }
    if (back) { currentState = OFFICE_FLOOR; drawOfficeFloorSelect(); }
  }
  else if (currentState == FF_FLOOR_MAP) {
    if (enter) {
      if      (ffHighlightCabin == 2) { currentState = FF_CABIN2_MAP; drawCabin2Map(); }
      else if (ffHighlightCabin == 3) { currentState = FF_CABIN3_MAP; drawCabin3Map(); }
    }
    if (back) { currentState = FF_TEACHER_LIST; drawFFTeacherList(); }
  }
  else if (currentState == FF_CABIN2_MAP) {
    if (back) { currentState = FF_FLOOR_MAP; drawFFFloorMap(); }
  }
  else if (currentState == FF_CABIN3_MAP) {
    if (back) { currentState = FF_FLOOR_MAP; drawFFFloorMap(); }
  }
}

// ========================================
// WiFi EVENTS
// ========================================
void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] Got IP: %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] Disconnected — auto-reconnecting...");
      break;
    default: break;
  }
}


void showCredits() {
  vga.clear(C_BLACK);
  vga.fillRect(0, 0, 400, 4, C_YELLOW);

  // Outer blue container box
  vga.fillRect(10, 10, 380, 278, C_BLUE);
  drawBox(10, 10, 380, 278, C_WHITE);

  // Header white box — full width like footer
  vga.fillRect(16, 20, 368, 30, C_WHITE);
  drawBox(16, 20, 368, 30, C_WHITE);
  centerText(30, "UET CS DEPARTMENT", C_BLUE, C_WHITE);

  // NOTICE BOARD below header box on blue bg
  centerText(58, "NOTICE BOARD", C_WHITE, C_BLUE);

  // Cyan divider
  vga.fillRect(20, 72, 360, 1, C_CYAN);

  // "DEVELOPED BY" badge
  int dbw = 11 * 6 + 16;
  int dbx = (400 - dbw) / 2;
  vga.fillRect(dbx, 78, dbw, 14, C_WHITE);
  drawBox(dbx, 78, dbw, 14, C_WHITE);
  vga.setTextColor(C_BLACK, C_WHITE);
  vga.setCursor(dbx + 8, 82);
  vga.print("DEVELOPED BY");

  // Card data
  const char* names[4]   = { "Haram Naseeb", "Hania Bukhari", "Wareesha Ameer Khan", "Muntaha Fatima" };
  const char* initials[4]= { "HN", "HB", "WA", "MF" };
  const char* rollnos[4] = { "2024-CS-230", "2024-CS-220", "2024-CS-202", "2024-CS-196" };

  const int CARD_W = 172;
  const int CARD_H = 52;
  const int xs[4]  = { 16, 206, 16, 206 };
  const int ys[4]  = { 100, 100, 162, 162 };

  for (int i = 0; i < 4; i++) {
    int x = xs[i], y = ys[i];

    vga.fillRect(x, y, CARD_W, CARD_H, C_BLACK);
    drawBox(x, y, CARD_W, CARD_H, C_CYAN);
    vga.fillRect(x, y, CARD_W, 3, C_YELLOW);

    vga.fillRect(x + 6, y + 12, 26, 26, C_BLUE);
    drawBox(x + 6, y + 12, 26, 26, C_CYAN);
    vga.setTextColor(C_WHITE, C_BLUE);
    vga.setCursor(x + 11, y + 21);
    vga.print(initials[i]);

    vga.setTextColor(C_WHITE, C_BLACK);
    vga.setCursor(x + 38, y + 16);
    vga.print(names[i]);

    vga.setTextColor(C_CYAN, C_BLACK);
    vga.setCursor(x + 38, y + 30);
    vga.print(rollnos[i]);
  }

  // Footer white box — close to bottom border
  vga.fillRect(16, 230, 368, 18, C_WHITE);
  drawBox(16, 230, 368, 18, C_WHITE);
  centerText(234, "2024-2025 | CS DEPT PROJECT", C_BLACK, C_WHITE);

  delay(4000);

  for (int y = 0; y < 300; y += 4) {
    vga.fillRect(0, y, 400, 4, C_BLUE);
    delay(6);
  }
}

// ========================================
// SETUP
// ========================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(upBtn,    INPUT_PULLUP);
  pinMode(downBtn,  INPUT_PULLUP);
  pinMode(enterBtn, INPUT_PULLUP);
  pinMode(backBtn,  INPUT_PULLUP);
  pinMode(buzzer,    OUTPUT); digitalWrite(buzzer,    LOW);
  pinMode(redLED,    OUTPUT); digitalWrite(redLED,    LOW);
  pinMode(greenLED,  OUTPUT); digitalWrite(greenLED,  LOW);
  pinMode(yellowLED, OUTPUT); digitalWrite(yellowLED, LOW);

  currentTT.count = 0;
  currentTT.yearIdx = currentTT.sessionIdx = currentTT.sectionIdx = -1;

  for (int i = 0; i < OFFICE_ROOM_COUNT;   i++) strncpy(officeTeacher[i],       "----", 31);
  for (int i = 0; i < FF_STANDALONE_COUNT; i++) strncpy(ffStandaloneTeacher[i], "----", 31);
  for (int i = 0; i < CABIN2_ROOM_COUNT;   i++) strncpy(cabin2Teacher[i],        "----", 31);
  for (int i = 0; i < CABIN3_ROOM_COUNT;   i++) strncpy(cabin3Teacher[i],        "----", 31);

  vga.init(vga.MODE400x300, redPin, greenPin, bluePin, hsyncPin, vsyncPin);
  vga.setFont(Font6x8);

  // 8 VGA colors — initialize after vga.init()
  C_BLACK   = vga.RGB(  0,   0,   0);
  C_RED     = vga.RGB(255,   0,   0);
  C_GREEN   = vga.RGB(  0, 255,   0);
  C_YELLOW  = vga.RGB(255, 255,   0);
  C_BLUE    = vga.RGB(  0,   0, 255);
  C_MAGENTA = vga.RGB(255,   0, 255);
  C_CYAN    = vga.RGB(  0, 255, 255);
  C_WHITE   = vga.RGB(255, 255, 255);

  loadNotifiedFromFlash();
  // Boot splash
  vga.clear(C_BLUE);
  vga.fillRect(0, 0, 400, 4, C_YELLOW);
  vga.fillRect(60, 80, 280, 130, C_BLACK);
  drawBox(60, 80, 280, 130, C_CYAN);
  vga.fillRect(60, 80, 280, 3, C_CYAN);

  centerText(100, "UET CS DEPARTMENT", C_CYAN,  C_BLACK);
  centerText(120, "NOTICE BOARD",      C_WHITE, C_BLACK);
  centerText(148, "Connecting to WiFi...",        C_CYAN,  C_BLACK);
  centerText(164, "Connect to: NoticeBoardSetup", C_WHITE, C_BLACK);
  centerText(180, "Open: 192.168.4.1 in browser", C_WHITE, C_BLACK);

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.onEvent(onWiFiEvent);

  WiFi.begin("Fast net fiber n", "03004721644aa");
  if (WiFi.waitForConnectResult(15000) != WL_CONNECTED) {
    wifiManager.setConfigPortalTimeout(0);
    wifiManager.autoConnect("NoticeBoardSetup");
  }
  MDNS.begin("esp32board");

  resolveFFTeacherHighlight(-1);

   // Connected splash
  vga.clear(C_BLUE);
  vga.fillRect(0, 0, 400, 4, C_YELLOW);
  vga.fillRect(60, 90, 280, 110, C_BLACK);
  drawBox(60, 90, 280, 110, C_CYAN);
  vga.fillRect(60, 90, 280, 3, C_GREEN);

  centerText(110, "WiFi Connected",                    C_GREEN, C_BLACK);
  centerText(130, WiFi.SSID().c_str(),                 C_WHITE, C_BLACK);
  centerText(148, WiFi.localIP().toString().c_str(),   C_CYAN,  C_BLACK);
  delay(1500);

  client.setServer(mqttServer, mqttPort);
  client.setBufferSize(512);
  client.setCallback(onMessage);

   // MQTT splash
  vga.clear(C_BLUE);
  vga.fillRect(0, 0, 400, 4, C_YELLOW);
  vga.fillRect(60, 110, 280, 70, C_BLACK);
  drawBox(60, 110, 280, 70, C_CYAN);
  vga.fillRect(60, 110, 280, 3, C_CYAN);
  centerText(130, "Connecting to MQTT...", C_WHITE, C_BLACK);
  centerText(148, "broker.hivemq.com",     C_CYAN,  C_BLACK);
  connectMQTT();
  delay(500);
 showCredits();
  fetchAnnouncements();
  fetchYears();

 
  drawMenu();
  Serial.println("[SETUP] Ready!");
}

// ========================================
// LOOP
// ========================================
void loop() {
  if (!client.connected() && WiFi.status() == WL_CONNECTED) connectMQTT();
  if (client.connected()) client.loop();
  handleButtons();

  if (millis() - lastAnnFetch > ANN_FETCH_INTERVAL) {
    lastAnnFetch = millis();
    if (currentState == MENU && WiFi.status() == WL_CONNECTED)
      fetchAnnouncements();
  }

  if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  delay(10);
}
