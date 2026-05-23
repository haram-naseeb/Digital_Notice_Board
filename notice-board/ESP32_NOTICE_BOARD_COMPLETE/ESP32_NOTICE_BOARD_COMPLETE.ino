#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Lib.h>
#include <Ressources/Font6x8.h>
#include <HTTPClient.h>

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
typedef struct {
  const char* ssid;
  const char* serverIP;
} NetworkConfig;

NetworkConfig networks[] = {
  { "Office",            ""              },
  { "Fast net fiber n",  "192.168.0.109" },
  { "HomeWiFi",          "192.168.1.100" },
};
const int NUM_NETWORKS = sizeof(networks) / sizeof(networks[0]);

const char* serverIP = "192.168.0.137";

void resolveServerIP() {
  String currentSSID = WiFi.SSID();
  Serial.printf("[HTTP] Connected SSID: %s\n", currentSSID.c_str());
  for (int i = 0; i < NUM_NETWORKS; i++) {
    if (currentSSID == networks[i].ssid) {
      serverIP = networks[i].serverIP;
      Serial.printf("[HTTP] Matched — server: %s\n", serverIP);
      return;
    }
  }
  Serial.printf("[HTTP] No match, using default: %s\n", serverIP);
}

// ================= COLORS =================
int BLACK, WHITE, BLUE, YELLOW, CYAN, GRAY, LIGHT_BLUE, DARK_BLUE, RED, GREEN;

// Cyberpunk Theme Colors — initialized after vga.init()
int NEON_PINK;
int NEON_BLUE;
int DARK_CYBER;
int CARD_DARK;
int NEON_GREEN;
int NEON_YELLOW;
int NEON_PURPLE;
int DIM_BLUE;
int GLOW_CYAN;
int NEON_CYAN;

// ================= UI HELPERS =================
void drawMenu();

// --- Glow text (offset shadow + bright main) ---
void drawGlowText(int x, int y, const char* text, int color, int bg) {
  vga.setTextColor(vga.RGB(0, 0, 0), bg);
  vga.setCursor(x - 1, y); vga.print(text);
  vga.setCursor(x + 1, y); vga.print(text);
  vga.setCursor(x, y - 1); vga.print(text);
  vga.setCursor(x, y + 1); vga.print(text);
  vga.setTextColor(color, bg);
  vga.setCursor(x, y); vga.print(text);
}

// --- Pulsing badge for NEW notices ---
int pulseSize = 8;
bool pulseDirection = true;
unsigned long lastPulseUpdate = 0;

void updatePulse() {
  if (millis() - lastPulseUpdate > 80) {
    if (pulseDirection) {
      pulseSize += 2;
      if (pulseSize >= 14) pulseDirection = false;
    } else {
      pulseSize -= 2;
      if (pulseSize <= 6) pulseDirection = true;
    }
    lastPulseUpdate = millis();
  }
}

void drawPulseBadge(int x, int y) {
  vga.fillCircle(x, y, pulseSize, RED);
  vga.fillCircle(x, y, pulseSize - 3, WHITE);
}

// --- Shadow box ---
void drawShadowBox(int x, int y, int w, int h, int mainColor, int borderColor) {
  vga.fillRect(x + 3, y + 3, w, h, vga.RGB(10, 10, 20));
  vga.fillRect(x, y, w, h, mainColor);
  vga.fillRect(x, y, w, 2, borderColor);
  vga.fillRect(x, y + h - 2, w, 2, borderColor);
  vga.fillRect(x, y, 2, h, borderColor);
  vga.fillRect(x + w - 2, y, 2, h, borderColor);
}

// --- Sound effects ---
unsigned long lastBeepTime = 0;
const int     BEEP_COOL    = 500;

void beep(int times) {
  if (millis() - lastBeepTime < BEEP_COOL) return;
  for (int i = 0; i < times; i++) {
    digitalWrite(buzzer, HIGH); delay(100);
    digitalWrite(buzzer, LOW);
    if (i < times - 1) delay(100);
  }
  lastBeepTime = millis();
}

void playSelectSound() {
  tone(buzzer, 900, 40);
  delay(40);
  noTone(buzzer);
}

void playConfirmSound() {
  tone(buzzer, 1200, 80);
  delay(80);
  tone(buzzer, 1800, 80);
  delay(80);
  noTone(buzzer);
}

enum UIState {
  MENU,
  URGENT_VIEW, GENERAL_VIEW, EVENT_VIEW, OFFICE_VIEW,
  TT_YEAR, TT_SESSION, TT_SECTION, TT_DISPLAY,
  OFFICE_FLOOR,
  OFFICE_TEACHERS,
  OFFICE_MAP,
  FF_TEACHER_LIST,
  FF_FLOOR_MAP,
  FF_CABIN2_MAP,
  FF_CABIN3_MAP
};

UIState currentState = MENU;

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

bool hasSeenAnnouncement(int annId) {
  for (int i = 0; i < seenCount; i++)
    if (seenAnnouncements[i] == annId) return true;
  return false;
}

void markAnnouncementAsSeen(int annId) {
  if (!hasSeenAnnouncement(annId) && seenCount < MAX_SEEN_ANNOUNCEMENTS)
    seenAnnouncements[seenCount++] = annId;
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

  if (currentTT.isEvening) {
    ttGrid.slotStart = 13;
    ttGrid.slotCount = 7;
  } else {
    ttGrid.slotStart = 8;
    ttGrid.slotCount = 8;
  }

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

    strncpy(ttGrid.cellCode[dayIdx][startSlot], e.code, 9);
    ttGrid.cellCode[dayIdx][startSlot][9] = '\0';
    strncpy(ttGrid.cellRoom[dayIdx][startSlot], e.room, 9);
    ttGrid.cellRoom[dayIdx][startSlot][9] = '\0';
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
  switch (hash % 6) {
    case 0: return { vga.RGB(0,   0,   150), vga.RGB(140, 200, 255) };
    case 1: return { vga.RGB(140, 0,   0),   vga.RGB(255, 160, 160) };
    case 2: return { vga.RGB(0,   110, 50),  vga.RGB(160, 255, 200) };
    case 3: return { vga.RGB(120, 65,  0),   vga.RGB(255, 210, 140) };
    case 4: return { vga.RGB(90,  0,   140), vga.RGB(220, 160, 255) };
    case 5: return { vga.RGB(0,   100, 110), vga.RGB(160, 255, 255) };
    default:return { vga.RGB(70,  70,  70),  vga.RGB(220, 220, 220) };
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

// ================= NAVIGATION STATE =================
int selOfficeTeacher    = 0;
int officeTeacherScroll = 0;
int officeHighlightRoom = -1;

int selFFTeacher    = 0;
int ffTeacherScroll = 0;
int ffSelectedTeacherIdx  = -1;
int ffHighlightCabin      = 0;
int ffHighlightRoom       = -1;
int selCabin2Slot = 0;
int selCabin3Slot = 0;

// ================= UI STATE =================

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
unsigned long lastAnnFetch = -31000UL;
const int     ANN_FETCH_INTERVAL = 30000;

void updateLEDs();

// ========================================
// CORE DRAWING PRIMITIVES
// ========================================
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
  vga.fillRect(x,         y,         w, 2, color);
  vga.fillRect(x,         y + h - 2, w, 2, color);
  vga.fillRect(x,         y,         2, h, color);
  vga.fillRect(x + w - 2, y,         2, h, color);
}

// Glow box — outer rings + dark interior
void drawGlowBox(int x, int y, int w, int h, int color, int intensity = 3) {
  for (int i = 1; i <= intensity; i++) {
    drawBox(x - i, y - i, w + (i * 2), h + (i * 2), color);
  }
  vga.fillRect(x, y, w, h, DARK_CYBER);
  drawBox(x, y, w, h, color);
}

// ========================================
// HEADER & FOOTER
// ========================================
void drawSystemStats() {
  if (client.connected()) {
    vga.fillCircle(375, 25, 6, NEON_GREEN);
    vga.fillCircle(375, 25, 3, vga.RGB(200, 255, 200));
  } else {
    vga.fillCircle(375, 25, 6, vga.RGB(120, 0, 0));
    vga.fillCircle(375, 25, 3, vga.RGB(60, 0, 0));
  }
  vga.fillRect(348, 10, 3, 3, NEON_PINK);
  vga.fillRect(354, 10, 3, 3, NEON_BLUE);
  vga.fillRect(360, 10, 3, 3, NEON_GREEN);
}

void drawHeader() {
  vga.fillRect(0, 0, 400, 52, DARK_CYBER);

  for (int x = 0; x < 400; x += 20) {
    vga.fillRect(x, 0, 1, 52, vga.RGB(0, 40, 50));
  }

  vga.fillRect(0, 49, 400, 1, vga.RGB(0, 80, 100));
  vga.fillRect(0, 50, 400, 2, NEON_BLUE);

  vga.fillRect(8, 8, 36, 36, NEON_PINK);
  drawBox(8, 8, 36, 36, NEON_BLUE);
  vga.setTextColor(DARK_CYBER, NEON_PINK);
  vga.setCursor(16, 22);
  vga.print("NB");

  drawGlowText(54, 14, ">> NEURAL INTERFACE <<", NEON_PINK, DARK_CYBER);
  vga.setTextColor(NEON_BLUE, DARK_CYBER);
  vga.setCursor(60, 32);
  vga.print("[ DEPT NOTICE BOARD ]");

  drawSystemStats();
}

void drawFooter(const char* hint) {
  vga.fillRect(0, 284, 400, 16, DARK_CYBER);
  vga.fillRect(0, 284, 400, 1, vga.RGB(0, 80, 100));
  vga.fillRect(0, 285, 400, 1, NEON_BLUE);

  vga.fillRect(8,  288, 55, 10, vga.RGB(30, 40, 60));
  vga.setTextColor(NEON_BLUE, vga.RGB(30, 40, 60));
  vga.setCursor(12, 290); vga.print("UP/DOWN");

  vga.fillRect(70, 288, 45, 10, vga.RGB(30, 40, 60));
  vga.setTextColor(NEON_PINK, vga.RGB(30, 40, 60));
  vga.setCursor(74, 290); vga.print("ENTER");

  vga.fillRect(122, 288, 35, 10, vga.RGB(30, 40, 60));
  vga.setTextColor(vga.RGB(255, 120, 0), vga.RGB(30, 40, 60));
  vga.setCursor(126, 290); vga.print("BACK");

  if (client.connected()) {
    vga.setTextColor(NEON_GREEN, DARK_CYBER);
    vga.setCursor(245, 290); vga.print("[MQTT:OK]");
  } else {
    vga.setTextColor(vga.RGB(220, 50, 50), DARK_CYBER);
    vga.setCursor(240, 290); vga.print("[OFFLINE]");
  }

  if (strlen(hint) > 0) {
    vga.setTextColor(vga.RGB(90, 90, 130), DARK_CYBER);
    vga.setCursor(164, 290);
    vga.print(hint);
  }
}

void drawBase(const char* hint) {
  vga.clear(BLACK);
  drawHeader();
  drawFooter(hint);
}

// ========================================
// LOADING SCREEN
// ========================================
void showLoading(const char* msg) {
  vga.clear(BLACK);
  vga.fillRect(0, 0, 400, 52, DARK_CYBER);
  for (int x = 0; x < 400; x += 20) vga.fillRect(x, 0, 1, 52, vga.RGB(0, 40, 50));
  vga.fillRect(0, 50, 400, 2, NEON_BLUE);

  drawBox(20, 100, 360, 80, NEON_BLUE);
  drawBox(22, 102, 356, 76, vga.RGB(0, 40, 80));

  int msgX = (400 - strlen(msg) * 6) / 2;
  vga.setTextColor(NEON_BLUE, BLACK);
  vga.setCursor(msgX, 128);
  vga.print(msg);

  vga.fillRect(60, 148, 280, 10, vga.RGB(20, 20, 40));
  drawBox(60, 148, 280, 10, vga.RGB(0, 60, 100));

  vga.fillRect(62, 150, 168, 6, NEON_BLUE);

  for (int i = 0; i < 3; i++) {
    vga.fillCircle(178 + i * 14, 170, 3, (i == 1) ? NEON_PINK : vga.RGB(40, 40, 80));
  }
}

// ========================================
// LED MANAGEMENT
// ========================================
void updateLEDs() {
  bool u = false, g = false, e = false;
  for (int i = 0; i < urgent.count;   i++) if (urgent.items[i].isNew)   u = true;
  for (int i = 0; i < general.count;  i++) if (general.items[i].isNew)  g = true;
  for (int i = 0; i < eventCat.count; i++) if (eventCat.items[i].isNew) e = true;
  digitalWrite(redLED,    u);
  digitalWrite(greenLED,  g);
  digitalWrite(yellowLED, e);
}

// ========================================
// ANNOUNCEMENT HELPERS
// ========================================
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
  cat.items[cat.count].isNew = hasSeenAnnouncement(annId) ? false : isNewAnn;
  if (isNewAnn) markAnnouncementAsSeen(annId);
  cat.count++;
}

void clearAllCategories() {
  urgent.count   = 0; urgent.index   = 0;
  general.count  = 0; general.index  = 0;
  eventCat.count = 0; eventCat.index = 0;
  office.count   = 0; office.index   = 0;
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
  if (WiFi.status() != WL_CONNECTED) return;

  int yearId    = years[yIdx].id;
  int sessionId = sessions[sIdx].id;
  int sectionId = sections[secIdx].id;

  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/timetable/" + yearId + "/" + sessionId + "/" + sectionId;
  if (!http.begin(httpClient, url)) return;
  http.setConnectTimeout(5000); http.setTimeout(10000);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(6144);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error && doc["success"] == true) {
      currentTT.count      = 0;
      currentTT.yearIdx    = yIdx;
      currentTT.sessionIdx = sIdx;
      currentTT.sectionIdx = secIdx;

      strncpy(currentTT.semester,     doc["semester"]     | "Unknown", 19);
      strncpy(currentTT.session_name, doc["session_name"] | "Unknown", 39);
      strncpy(currentTT.term,         doc["term"]         | "Unknown", 19);
      currentTT.semester[19]     = '\0';
      currentTT.session_name[39] = '\0';
      currentTT.term[19]         = '\0';

      currentTT.isEvening = detectEvening(currentTT.session_name);

      for (JsonObject e : doc["data"].as<JsonArray>()) {
        if (currentTT.count >= MAX_TT_ENTRIES) break;
        int i = currentTT.count;

        strncpy(currentTT.entries[i].startTime, e["start_time"]   | "00:00:00", 9);
        strncpy(currentTT.entries[i].endTime,   e["end_time"]     | "00:00:00", 9);
        strncpy(currentTT.entries[i].subject,   e["subject_name"] | "", 29);
        strncpy(currentTT.entries[i].code,      e["subject_code"] | "", 9);
        strncpy(currentTT.entries[i].teacher,   e["teacher_name"] | "", 29);
        strncpy(currentTT.entries[i].room,      e["room_number"]  | "", 9);
        strncpy(currentTT.entries[i].dayName,   e["day_name"]     | "MON", 3);

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

      Serial.printf("Loaded %d TT entries | %s - %s | %s\n",
        currentTT.count, currentTT.semester, currentTT.term, currentTT.session_name);
      Serial.printf("Mode: %s\n", currentTT.isEvening ? "Evening (13-19)" : "Morning (8-15)");
    }
  }
  http.end();
}

// -----------------------------------------------------------------------
// fetchOfficeTeachers — Ground Floor
// -----------------------------------------------------------------------
void fetchOfficeTeachers(const char* floor) {
  if (WiFi.status() != WL_CONNECTED) return;

  for (int i = 0; i < OFFICE_ROOM_COUNT; i++)
    strncpy(officeTeacher[i], "----", 31);
  officeTeacherLoaded = 0;

  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/offices";
  if (!http.begin(httpClient, url)) {
    Serial.println("[OFFICES] http.begin() failed");
    return;
  }
  http.setConnectTimeout(5000);
  http.setTimeout(10000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OFFICES] Bad response: %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[OFFICES] JSON error: %s\n", err.c_str());
    return;
  }

  for (JsonObject o : doc.as<JsonArray>()) {
    const char* floorStr = o["floor"]       | "";
    const char* roomStr  = o["room_number"] | "-1";
    const char* teacher  = o["teacher_name"]| "";

    int roomNum = atoi(roomStr);

    if (strcmp(floorStr, floor) != 0) continue;
    if (strlen(teacher) == 0) continue;

    int slot = -1;
    switch (roomNum) {
      case 1: slot = 4; break;
      case 2: slot = 2; break;
      case 3: slot = 0; break;
      case 4: slot = 1; break;
      case 5: slot = 3; break;
      case 6: slot = 5; break;
      default: break;
    }

    if (slot >= 0) {
      strncpy(officeTeacher[slot], teacher, 31);
      officeTeacher[slot][31] = '\0';
    }
  }

  officeTeacherLoaded = 1;
}

// -----------------------------------------------------------------------
// fetchFirstFloorTeachers
// -----------------------------------------------------------------------
void fetchFirstFloorTeachers() {
  if (WiFi.status() != WL_CONNECTED) return;

  ffTeacherCount = 0;
  ffTeacherLoaded = 0;
  for (int i = 0; i < FF_STANDALONE_COUNT; i++)
    strncpy(ffStandaloneTeacher[i], "----", 31);
  for (int i = 0; i < CABIN2_ROOM_COUNT; i++)
    strncpy(cabin2Teacher[i], "----", 31);
  for (int i = 0; i < CABIN3_ROOM_COUNT; i++)
    strncpy(cabin3Teacher[i], "----", 31);

  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/offices";
  if (!http.begin(httpClient, url)) return;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[FF] HTTP error: %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, payload)) {
    Serial.println("[FF] JSON parse failed");
    return;
  }

  for (JsonObject o : doc.as<JsonArray>()) {
    const char* floorStr = o["floor"] | "";
    const char* roomStr  = o["room_number"] | "0";
    const char* teacher  = o["teacher_name"] | "";

    if (strcmp(floorStr, "1st Floor") != 0) continue;
    if (strlen(teacher) == 0) continue;

    int roomNum = atoi(roomStr);

    int cabinNum = 0;
    if (roomNum >= 7 && roomNum <= 12) {
      cabinNum = 2;
    } else if (roomNum >= 16 && roomNum <= 18) {
      cabinNum = 3;
    } else if (roomNum == 13 || roomNum == 14 || roomNum == 15 || roomNum == 19 || roomNum == 20) {
      cabinNum = 0;
    } else {
      continue;
    }

    if (ffTeacherCount < FF_MAX_TEACHERS) {
      strncpy(ffTeachers[ffTeacherCount].name, teacher, 31);
      ffTeachers[ffTeacherCount].name[31] = '\0';
      ffTeachers[ffTeacherCount].roomNumber = roomNum;
      ffTeachers[ffTeacherCount].cabinNumber = cabinNum;
      ffTeacherCount++;
    }

    if (cabinNum == 0) {
      for (int i = 0; i < FF_STANDALONE_COUNT; i++) {
        if (FF_STANDALONE_ROOMS[i] == roomNum) {
          strncpy(ffStandaloneTeacher[i], teacher, 31);
          ffStandaloneTeacher[i][31] = '\0';
          break;
        }
      }
    }

    if (cabinNum == 2) {
      for (int s = 0; s < CABIN2_ROOM_COUNT; s++) {
        if (CABIN2_SLOT_TO_ROOM[s] == roomNum) {
          strncpy(cabin2Teacher[s], teacher, 31);
          cabin2Teacher[s][31] = '\0';
          break;
        }
      }
    }

    if (cabinNum == 3) {
      for (int s = 0; s < CABIN3_ROOM_COUNT; s++) {
        if (CABIN3_SLOT_TO_ROOM[s] == roomNum) {
          strncpy(cabin3Teacher[s], teacher, 31);
          cabin3Teacher[s][31] = '\0';
          break;
        }
      }
    }
  }

  ffTeacherLoaded = 1;
  Serial.printf("[FF] Loaded %d first floor teachers\n", ffTeacherCount);
}

// -----------------------------------------------------------------------
// resolveFFTeacherHighlight
// -----------------------------------------------------------------------
void resolveFFTeacherHighlight(int teacherIdx) {
  if (teacherIdx < 0 || teacherIdx >= ffTeacherCount) return;

  int roomNum  = ffTeachers[teacherIdx].roomNumber;
  int cabinNum = ffTeachers[teacherIdx].cabinNumber;

  if (cabinNum == 2) {
    ffHighlightCabin = 2;
    for (int s = 0; s < CABIN2_ROOM_COUNT; s++) {
      if (CABIN2_SLOT_TO_ROOM[s] == roomNum) {
        ffHighlightRoom = s;
        selCabin2Slot   = s;
        break;
      }
    }
  } else if (cabinNum == 3) {
    ffHighlightCabin = 3;
    for (int s = 0; s < CABIN3_ROOM_COUNT; s++) {
      if (CABIN3_SLOT_TO_ROOM[s] == roomNum) {
        ffHighlightRoom = s;
        selCabin3Slot   = s;
        break;
      }
    }
  } else {
    ffHighlightCabin = 0;
    for (int i = 0; i < FF_STANDALONE_COUNT; i++) {
      if (FF_STANDALONE_ROOMS[i] == roomNum) {
        ffHighlightRoom = i;
        break;
      }
    }
  }
}

// ========================================
// TIMETABLE DISPLAY
// ========================================
void drawTimetable() {
  vga.clear(BLACK);

  vga.fillRect(0, 0, 400, 46, DARK_BLUE);
  drawBox(2, 2, 396, 42, CYAN);

  char hdr[80];
  sprintf(hdr, "%s | SEC: %s - TIMETABLE",
          currentTT.semester,
          sections[currentTT.sectionIdx].name);
  int hdrX = (400 - (int)strlen(hdr) * 6) / 2;
  if (hdrX < 5) hdrX = 5;
  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(hdrX, 18);
  vga.print(hdr);

  if (currentTT.count == 0) {
    vga.fillRect(20, 60, 360, 200, GRAY);
    drawBox(20, 60, 360, 200, LIGHT_BLUE);
    vga.setTextColor(RED, GRAY);
    vga.setCursor(80, 150);
    vga.print("NO TIMETABLE DATA");
    vga.fillRect(0, 285, 400, 15, DARK_BLUE);
    drawBox(10, 285, 380, 14, CYAN);
    leftText(12, 290, "[E:Menu]  [B:Back]", LIGHT_BLUE, DARK_BLUE);
    return;
  }

  buildTimetableGrid();

  const int DAY_COL_W = 34;
  const int GRID_X    = 0;
  const int HDR_Y     = 46;
  const int HDR_H     = 21;
  const int ROW_Y     = HDR_Y + HDR_H;
  const int ROW_H     = 44;
  const int FOOTER_Y  = 287;

  int nSlots = ttGrid.slotCount;
  int cellW  = (400 - DAY_COL_W) / nSlots;

  vga.fillRect(GRID_X, HDR_Y, DAY_COL_W, HDR_H, DARK_BLUE);
  drawBox(GRID_X, HDR_Y, DAY_COL_W, HDR_H, CYAN);
  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(GRID_X + 4, HDR_Y + 7);
  vga.print("DAY");

  for (int s = 0; s < nSlots; s++) {
    int x = GRID_X + DAY_COL_W + s * cellW;
    int w = (s == nSlots - 1) ? (400 - x) : cellW;
    vga.fillRect(x, HDR_Y, w, HDR_H, DARK_BLUE);
    drawBox(x, HDR_Y, w, HDR_H, CYAN);
    char label[8];
    sprintf(label, "%d:00", ttGrid.slotStart + s);
    int lx = x + (w - (int)strlen(label) * 6) / 2;
    vga.setTextColor(CYAN, DARK_BLUE);
    vga.setCursor(lx, HDR_Y + 7);
    vga.print(label);
  }

  int FREE_BG     = vga.RGB(0,  90, 0);
  int FREE_BORDER = vga.RGB(0, 140, 0);
  int FREE_TEXT   = vga.RGB(0, 200, 0);

  for (int d = 0; d < MAX_DAYS; d++) {
    int y = ROW_Y + d * ROW_H;

    vga.fillRect(GRID_X, y, DAY_COL_W, ROW_H, DARK_BLUE);
    drawBox(GRID_X, y, DAY_COL_W, ROW_H, CYAN);
    vga.setTextColor(YELLOW, DARK_BLUE);
    vga.setCursor(GRID_X + 4, y + ROW_H / 2 - 4);
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
        vga.fillRect(x, y, fillW, ROW_H, FREE_BG);
        drawBox(x, y, fillW, ROW_H, FREE_BORDER);
        int cx = x + fillW / 2 - 3;
        vga.setTextColor(FREE_TEXT, FREE_BG);
        vga.setCursor(cx, y + ROW_H / 2 - 4);
        vga.print("-");
        s++;
      } else {
        const char* code = ttGrid.cellCode[d][s];
        const char* room = ttGrid.cellRoom[d][s];
        SubjectColor sc  = getSubjectColor(code);

        vga.fillRect(x, y, fillW, ROW_H, sc.bg);
        drawBox(x, y, fillW, ROW_H, LIGHT_BLUE);

        int maxChars = (fillW - 6) / 6;
        if (maxChars < 1) maxChars = 1;

        char codeLine[16] = {0};
        strncpy(codeLine, code, min(maxChars, 15));
        char roomLine[16] = {0};
        strncpy(roomLine, room, min(maxChars, 15));

        int codeLx = x + (fillW - (int)strlen(codeLine) * 6) / 2;
        int roomLx = x + (fillW - (int)strlen(roomLine) * 6) / 2;

        vga.setTextColor(sc.text, sc.bg);
        vga.setCursor(codeLx, y + ROW_H / 2 - 10);
        vga.print(codeLine);

        vga.setTextColor(vga.RGB(190, 190, 190), sc.bg);
        vga.setCursor(roomLx, y + ROW_H / 2 + 2);
        vga.print(roomLine);

        s += actualSpan;
      }
    }
  }

  vga.fillRect(0, FOOTER_Y, 400, 300 - FOOTER_Y, DARK_BLUE);
  drawBox(2, FOOTER_Y, 396, 300 - FOOTER_Y, CYAN);
  leftText(8, FOOTER_Y + 4, "[E:Main Menu]  [B:Back to Section]", LIGHT_BLUE, DARK_BLUE);
}

// ========================================
// MAIN MENU
// ========================================
void drawMenu() {
  drawBase("");

  const char* items[5] = { "URGENT NOTICES", "GENERAL NOTICES", "EVENT NOTICES", "TEACHER OFFICES", "TIME TABLE" };
  int colors[5] = {
    vga.RGB(255, 60,  60),
    vga.RGB(0,   220, 100),
    vga.RGB(255, 200, 0),
    NEON_BLUE,
    NEON_PINK
  };
  Category* cats[4] = { &urgent, &general, &eventCat, &office };

  for (int gx = 10; gx < 400; gx += 18) {
    for (int gy = 58; gy < 283; gy += 18) {
      vga.fillRect(gx, gy, 1, 1, vga.RGB(0, 25, 35));
    }
  }

  updatePulse();

  for (int i = 0; i < 5; i++) {
    int y   = 60 + i * 44;
    bool sel = (i == selectedMenu);

    if (sel) {
      drawBox(20, y,     360, 40, vga.RGB(colors[i] >> 2 & 0x3F, 0, 0));
      drawBox(22, y + 1, 356, 38, colors[i]);
      vga.fillRect(24, y + 2, 352, 36, vga.RGB(12, 18, 30));
      vga.fillRect(24, y + 2, 4, 36, colors[i]);
      
      // Center the text
      int textLen = strlen(items[i]);
      int textX = 38 + (352 - textLen * 6) / 2;
      drawGlowText(textX, y + 14, items[i], colors[i], vga.RGB(12, 18, 30));
      
      vga.setTextColor(colors[i], vga.RGB(12, 18, 30));
      vga.setCursor(348, y + 14); vga.print(">");
    } else {
      vga.fillRect(24, y + 2, 352, 36, vga.RGB(18, 20, 32));
      drawBox(24, y + 2, 352, 36, vga.RGB(35, 40, 60));
      vga.fillRect(24, y + 2, 2, 36, vga.RGB(40, 50, 80));
      
      // Center the text
      int textLen = strlen(items[i]);
      int textX = (400 - textLen * 6) / 2;
      vga.setTextColor(vga.RGB(150, 160, 200), vga.RGB(18, 20, 32));
      vga.setCursor(textX, y + 14);
      vga.print(items[i]);
    }

    if (i < 4) {
      char cnt[8];
      sprintf(cnt, "%d", cats[i]->count);
      int badgeW = strlen(cnt) * 6 + 10;
      int badgeX = 316 - badgeW;

      if (sel) {
        vga.fillRect(badgeX, y + 12, badgeW, 14, colors[i]);
        vga.setTextColor(BLACK, colors[i]);
      } else {
        vga.fillRect(badgeX, y + 12, badgeW, 14, vga.RGB(35, 40, 60));
        vga.setTextColor(vga.RGB(120, 130, 160), vga.RGB(35, 40, 60));
      }
      vga.setCursor(badgeX + 5, y + 15);
      vga.print(cnt);

      bool hasNew = false;
      for (int j = 0; j < cats[i]->count; j++) {
        if (cats[i]->items[j].isNew) { hasNew = true; break; }
      }
      if (hasNew) {
        drawPulseBadge(sel ? 330 : 333, y + 20);
      }
    }
  }

  updateLEDs();
}

// ========================================
// ANNOUNCEMENT / NOTICE VIEW
// ========================================
void showCategory(Category &cat, const char* title) {
  drawBase("U/D:Navigate  B:Back");

  if (cat.count == 0) {
    vga.fillRect(40, 100, 320, 80, vga.RGB(18, 20, 32));
    drawBox(40, 100, 320, 80, vga.RGB(40, 50, 80));
    centerText(130, "NO ANNOUNCEMENTS", NEON_BLUE, vga.RGB(18, 20, 32));
    centerText(148, "in this category", vga.RGB(80, 90, 120), vga.RGB(18, 20, 32));
    return;
  }

  if (cat.index >= cat.count) cat.index = 0;
  if (cat.index < 0)          cat.index = cat.count - 1;

  vga.fillRect(15, 56, 370, 42, vga.RGB(10, 14, 28));
  drawBox(15, 56, 370, 42, NEON_BLUE);
  vga.fillRect(17, 58, 4, 38, NEON_PINK);
  drawGlowText(28, 62, title, NEON_PINK, vga.RGB(10, 14, 28));
  vga.setTextColor(vga.RGB(180, 190, 220), vga.RGB(10, 14, 28));
  vga.setCursor(28, 80);
  vga.print(cat.items[cat.index].title);

  vga.fillRect(15, 104, 370, 158, vga.RGB(16, 18, 30));
  drawBox(15, 104, 370, 158, vga.RGB(40, 50, 80));

  vga.setTextColor(vga.RGB(200, 210, 230), vga.RGB(16, 18, 30));
  const char* msg = cat.items[cat.index].message;
  int lineY = 114, pos = 0, len = strlen(msg);
  
  // Center each line of the message
  while (pos < len && lineY < 254) {
    char line[53] = {0};
    int charsToCopy = min(52, len - pos);
    strncpy(line, msg + pos, charsToCopy);
    line[charsToCopy] = '\0';
    
    // Center the line
    int lineLen = strlen(line);
    int lineX = (400 - lineLen * 6) / 2;
    if (lineX < 22) lineX = 22;
    
    vga.setCursor(lineX, lineY);
    vga.print(line);
    pos += 52;
    lineY += 16;
  }

  char nav[40];
  sprintf(nav, "Notice %d of %d", cat.index + 1, cat.count);
  centerText(268, nav, NEON_CYAN, BLACK);

  cat.items[cat.index].isNew = false;
  updateLEDs();
}

// ========================================
// TIMETABLE SELECTION SCREENS
// ========================================
void drawYearSelect() {
  drawBase("U/D:Navigate  E:Select  B:Back");

  vga.fillRect(15, 55, 370, 18, vga.RGB(10, 14, 28));
  drawBox(15, 55, 370, 18, NEON_BLUE);
  drawGlowText(80, 60, "SELECT YEAR / BATCH", YELLOW, vga.RGB(10, 14, 28));

  if (totalYears == 0) {
    centerText(160, "No years found!", NEON_BLUE, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = min(yearScrollOff + VISIBLE, totalYears);

  for (int i = yearScrollOff; i < end; i++) {
    int y   = 80 + (i - yearScrollOff) * 38;
    bool sel = (i == selYear);

    if (sel) {
      vga.fillRect(50, y, 300, 33, vga.RGB(10, 14, 28));
      drawBox(50, y, 300, 33, YELLOW);
      vga.fillRect(50, y, 4, 33, YELLOW);
      drawGlowText(64, y + 12, years[i].name, YELLOW, vga.RGB(10, 14, 28));
    } else {
      vga.fillRect(50, y, 300, 33, vga.RGB(18, 20, 32));
      drawBox(50, y, 300, 33, vga.RGB(40, 50, 70));
      centerText(y + 12, years[i].name, vga.RGB(160, 170, 200), vga.RGB(18, 20, 32));
    }
  }

  if (yearScrollOff > 0)
    centerText(74, "^ scroll up", NEON_CYAN, BLACK);
  if (yearScrollOff + VISIBLE < totalYears)
    centerText(276, "v scroll down", NEON_CYAN, BLACK);
}

void drawSessionSelect() {
  drawBase("U/D:Navigate  E:Select  B:Back");

  vga.fillRect(15, 55, 370, 18, vga.RGB(10, 14, 28));
  drawBox(15, 55, 370, 18, NEON_BLUE);
  char header[60];
  sprintf(header, "%s - SELECT SESSION", years[selYear].name);
  drawGlowText(30, 60, header, YELLOW, vga.RGB(10, 14, 28));

  if (totalSessions == 0) {
    centerText(160, "No sessions found!", NEON_BLUE, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = min(sessionScrollOff + VISIBLE, totalSessions);

  for (int i = sessionScrollOff; i < end; i++) {
    int y   = 80 + (i - sessionScrollOff) * 38;
    bool sel = (i == selSession);

    if (sel) {
      vga.fillRect(30, y, 340, 33, vga.RGB(10, 14, 28));
      drawBox(30, y, 340, 33, YELLOW);
      vga.fillRect(30, y, 4, 33, YELLOW);
      drawGlowText(42, y + 12, sessions[i].name, YELLOW, vga.RGB(10, 14, 28));
    } else {
      vga.fillRect(30, y, 340, 33, vga.RGB(18, 20, 32));
      drawBox(30, y, 340, 33, vga.RGB(40, 50, 70));
      vga.setTextColor(vga.RGB(160, 170, 200), vga.RGB(18, 20, 32));
      vga.setCursor(42, y + 12);
      vga.print(sessions[i].name);
    }
  }

  if (sessionScrollOff > 0)
    centerText(74, "^ scroll up", NEON_CYAN, BLACK);
  if (sessionScrollOff + VISIBLE < totalSessions)
    centerText(276, "v scroll down", NEON_CYAN, BLACK);
}

void drawSectionSelect() {
  drawBase("U/D:Navigate  E:Select  B:Back");

  vga.fillRect(15, 55, 370, 18, vga.RGB(10, 14, 28));
  drawBox(15, 55, 370, 18, NEON_BLUE);
  char title[60];
  sprintf(title, "%s - SELECT SECTION", sessions[selSession].name);
  drawGlowText(18, 60, title, YELLOW, vga.RGB(10, 14, 28));

  if (totalSections == 0) {
    centerText(160, "No sections found!", NEON_BLUE, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = min(sectionScrollOff + VISIBLE, totalSections);

  for (int i = sectionScrollOff; i < end; i++) {
    int y   = 80 + (i - sectionScrollOff) * 38;
    bool sel = (i == selSection);

    if (sel) {
      vga.fillRect(100, y, 200, 33, vga.RGB(10, 20, 14));
      drawBox(100, y, 200, 33, GREEN);
      vga.fillRect(100, y, 4, 33, GREEN);
      drawGlowText(114, y + 12, sections[i].name, GREEN, vga.RGB(10, 20, 14));
    } else {
      vga.fillRect(100, y, 200, 33, vga.RGB(18, 20, 32));
      drawBox(100, y, 200, 33, vga.RGB(40, 50, 70));
      centerText(y + 12, sections[i].name, vga.RGB(160, 170, 200), vga.RGB(18, 20, 32));
    }
  }

  if (sectionScrollOff > 0)
    centerText(74, "^ scroll up", NEON_CYAN, BLACK);
  if (sectionScrollOff + VISIBLE < totalSections)
    centerText(276, "v scroll down", NEON_CYAN, BLACK);
}

// ========================================
// OFFICE FLOOR SELECTION
// ========================================
void drawOfficeFloorSelect() {
  drawBase("U/D:Navigate  E:Select  B:Menu");

  vga.fillRect(15, 55, 370, 18, vga.RGB(10, 14, 28));
  drawBox(15, 55, 370, 18, NEON_BLUE);
  drawGlowText(130, 60, "SELECT FLOOR", YELLOW, vga.RGB(10, 14, 28));

  for (int i = 0; i < OFFICE_FLOOR_COUNT; i++) {
    int  y   = 100 + i * 50;
    bool sel = (i == selOfficeFloor);

    if (sel) {
      vga.fillRect(50, y, 300, 38, vga.RGB(10, 14, 28));
      drawBox(50, y, 300, 38, YELLOW);
      vga.fillRect(50, y, 4, 38, YELLOW);
      drawGlowText(68, y + 14, OFFICE_FLOORS[i], YELLOW, vga.RGB(10, 14, 28));
    } else {
      vga.fillRect(50, y, 300, 38, vga.RGB(18, 20, 32));
      drawBox(50, y, 300, 38, vga.RGB(40, 50, 70));
      centerText(y + 14, OFFICE_FLOORS[i], vga.RGB(160, 170, 200), vga.RGB(18, 20, 32));
    }
  }
}

// ========================================
// OFFICE TEACHER SELECT (GROUND FLOOR)
// ========================================
void drawOfficeTeacherSelect() {
  drawBase("U/D:Navigate  E:View Map  B:Back");

  vga.fillRect(15, 55, 370, 18, vga.RGB(10, 14, 28));
  drawBox(15, 55, 370, 18, NEON_BLUE);
  char header[50];
  sprintf(header, "%s - SELECT TEACHER", OFFICE_FLOORS[selOfficeFloor]);
  drawGlowText(30, 60, header, YELLOW, vga.RGB(10, 14, 28));

  if (!officeTeacherLoaded) {
    centerText(160, "Loading...", NEON_CYAN, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = min(officeTeacherScroll + VISIBLE, OFFICE_ROOM_COUNT);

  for (int i = officeTeacherScroll; i < end; i++) {
    int  y   = 78 + (i - officeTeacherScroll) * 40;
    bool sel = (i == selOfficeTeacher);

    if (sel) {
      vga.fillRect(15, y, 370, 34, vga.RGB(10, 14, 28));
      drawBox(15, y, 370, 34, NEON_CYAN);
      vga.fillRect(15, y, 4, 34, NEON_CYAN);
      drawGlowText(28, y + 12, officeTeacher[i], YELLOW, vga.RGB(10, 14, 28));
      vga.setTextColor(NEON_CYAN, vga.RGB(10, 14, 28));
      vga.setCursor(358, y + 12); vga.print(">");
    } else {
      vga.fillRect(15, y, 370, 34, vga.RGB(18, 20, 32));
      drawBox(15, y, 370, 34, vga.RGB(40, 50, 70));
      int nameLen = strlen(officeTeacher[i]);
      int nameX   = (400 - nameLen * 6) / 2;
      if (nameX < 28) nameX = 28;
      vga.setTextColor(vga.RGB(160, 170, 200), vga.RGB(18, 20, 32));
      vga.setCursor(nameX, y + 12);
      vga.print(officeTeacher[i]);
    }
  }

  if (officeTeacherScroll > 0)
    centerText(71, "^ scroll up", NEON_CYAN, BLACK);
  if (officeTeacherScroll + VISIBLE < OFFICE_ROOM_COUNT)
    centerText(278, "v scroll down", NEON_CYAN, BLACK);
}

// ========================================
// GROUND FLOOR MAP
// ========================================
void drawOfficeMap() {
  vga.clear(BLACK);

  const int pairLeft[3]  = { 0, 2, 4 };
  const int pairRight[3] = { 1, 3, 5 };

  vga.fillRect(0, 0, 400, 18, DARK_BLUE);
  drawBox(0, 0, 400, 18, CYAN);
  centerText(5, "GROUND FLOOR - TEACHER OFFICES", YELLOW, DARK_BLUE);

  const int BOX_W      = 185;
  const int BOX_H      = 82;
  const int LEFT_X     = 5;
  const int RIGHT_X    = 210;
  const int FIRST_Y    = 22;
  const int ROW_STRIDE = 86;

  const char* personArt[5] = {
    "     ____         ",
    "     |    |        ",
    "    |____| O     ",
    "      |  /||\\   ",
    "     (  ) _/     "
  };

  for (int p = 0; p < 3; p++) {
    int lSlot = pairLeft[p];
    int rSlot = pairRight[p];
    int boxY  = FIRST_Y + p * ROW_STRIDE;

    for (int side = 0; side < 2; side++) {
      int   slot    = (side == 0) ? lSlot : rSlot;
      int   boxX    = (side == 0) ? LEFT_X : RIGHT_X;
      bool  isHL    = (slot == officeHighlightRoom);
      int   borderC = isHL ? YELLOW     : vga.RGB(40, 50, 80);
      int   bgC     = isHL ? vga.RGB(0, 0, 60) : vga.RGB(18, 20, 32);
      int   textC   = isHL ? YELLOW     : NEON_CYAN;

      vga.fillRect(boxX, boxY, BOX_W, BOX_H, bgC);
      drawBox(boxX, boxY, BOX_W, BOX_H, borderC);
      if (isHL) drawBox(boxX + 2, boxY + 2, BOX_W - 4, BOX_H - 4, YELLOW);

      int artStartY = boxY + 4;
      for (int r = 0; r < 5; r++) {
        int artLen = strlen(personArt[r]);
        int artX   = boxX + (BOX_W - artLen * 6) / 2;
        vga.setTextColor(isHL ? vga.RGB(255, 200, 0) : vga.RGB(120, 130, 150), bgC);
        vga.setCursor(artX, artStartY + r * 9);
        vga.print(personArt[r]);
      }

      const char* tname    = officeTeacher[slot];
      int         tnameLen = strlen(tname);
      int         tnameX   = boxX + (BOX_W - tnameLen * 6) / 2;
      if (tnameX < boxX + 4) tnameX = boxX + 4;
      vga.setTextColor(textC, bgC);
      vga.setCursor(tnameX, boxY + BOX_H - 14);
      char nameDisplay[28] = {0};
      int  maxChars = (BOX_W - 8) / 6;
      strncpy(nameDisplay, tname, min(maxChars, 27));
      vga.print(nameDisplay);
    }
  }

  vga.setTextColor(vga.RGB(40, 50, 70), BLACK);
  vga.setCursor(195, 140); vga.print("|");
  vga.setCursor(195, 150); vga.print("|");
  vga.setCursor(195, 160); vga.print("|");

  drawFooter("[E:Main Menu]  [B:Back to List]");
}

// ========================================
// FIRST FLOOR TEACHER LIST
// ========================================
void drawFFTeacherList() {
  drawBase("U/D:Navigate  E:Find  B:Back");

  vga.fillRect(15, 55, 370, 18, vga.RGB(10, 14, 28));
  drawBox(15, 55, 370, 18, NEON_BLUE);
  drawGlowText(40, 60, "FIRST FLOOR - SELECT TEACHER", YELLOW, vga.RGB(10, 14, 28));

  if (!ffTeacherLoaded) {
    centerText(160, "Loading...", NEON_CYAN, BLACK);
    return;
  }
  if (ffTeacherCount == 0) {
    centerText(160, "No teachers found!", NEON_CYAN, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = min(ffTeacherScroll + VISIBLE, ffTeacherCount);

  for (int i = ffTeacherScroll; i < end; i++) {
    int  y   = 78 + (i - ffTeacherScroll) * 40;
    bool sel = (i == selFFTeacher);

    if (sel) {
      vga.fillRect(10, y, 380, 35, vga.RGB(10, 14, 28));
      drawBox(10, y, 380, 35, NEON_CYAN);
      vga.fillRect(10, y, 4, 35, NEON_CYAN);
      drawGlowText(22, y + 13, ffTeachers[i].name, YELLOW, vga.RGB(10, 14, 28));
      vga.setTextColor(NEON_CYAN, vga.RGB(10, 14, 28));
      vga.setCursor(365, y + 13); vga.print(">");
    } else {
      vga.fillRect(10, y, 380, 35, vga.RGB(18, 20, 32));
      drawBox(10, y, 380, 35, vga.RGB(40, 50, 70));
      int nameLen = strlen(ffTeachers[i].name);
      int nameX   = (400 - nameLen * 6) / 2;
      if (nameX < 20) nameX = 20;
      vga.setTextColor(vga.RGB(160, 170, 200), vga.RGB(18, 20, 32));
      vga.setCursor(nameX, y + 13);
      vga.print(ffTeachers[i].name);
    }

    char roomLabel[12];
    if (ffTeachers[i].cabinNumber > 0)
      sprintf(roomLabel, "C%d-R%d", ffTeachers[i].cabinNumber, ffTeachers[i].roomNumber);
    else
      sprintf(roomLabel, "R%d", ffTeachers[i].roomNumber);
    vga.setTextColor(sel ? NEON_CYAN : vga.RGB(80, 90, 120), sel ? vga.RGB(10, 14, 28) : vga.RGB(18, 20, 32));
    vga.setCursor(340, y + 13);
    vga.print(roomLabel);
  }

  if (ffTeacherScroll > 0)
    centerText(71, "^ up", NEON_CYAN, BLACK);
  if (ffTeacherScroll + VISIBLE < ffTeacherCount)
    centerText(278, "v down", NEON_CYAN, BLACK);
}

// ========================================
// FIRST FLOOR MAP
// ========================================
void drawFFFloorMap() {
  vga.clear(BLACK);

  vga.fillRect(0, 0, 400, 18, DARK_BLUE);
  drawBox(0, 0, 400, 18, CYAN);
  centerText(5, "FIRST FLOOR MAP", YELLOW, DARK_BLUE);

  drawFooter("[E:Open Cabin]  [B:Back to List]");

  const int SR_W = 120;
  const int SR_H = 36;
  const int CB_W = 108;
  const int CB_H = 36;

  const int ROW0_Y = 24;
  const int ROW1_Y = 68;
  const int ROW2_Y = 148;
  const int ROW3_Y = 195;

  const int ROW0_LEFT_X   = 5;
  const int ROW0_GAP_X    = 88;
  const int ROW0_CABIN2_X = 280;
  const int ROW3_CABIN3_X = 5;
  const int ROW3_R19_X    = 130;
  const int ROW3_R20_X    = 270;
  const int ROW3_STAIRS_X = 600;

  auto drawRoomBox = [&](int bx, int by, int bw, int bh,
                         bool isHL, bool isCabin,
                         const char* label, const char* teacher) {
    int bgC    = isHL ? vga.RGB(0, 0, 80)     : vga.RGB(18, 20, 32);
    int border = isHL ? YELLOW                 : (isCabin ? NEON_CYAN : vga.RGB(40, 50, 80));
    int labelC = isHL ? YELLOW                 : (isCabin ? NEON_CYAN : vga.RGB(150, 160, 190));
    int nameC  = isHL ? vga.RGB(255, 220, 100) : vga.RGB(120, 130, 150);

    vga.fillRect(bx, by, bw, bh, bgC);
    drawBox(bx, by, bw, bh, border);
    if (isHL) drawBox(bx + 2, by + 2, bw - 4, bh - 4, YELLOW);

    int lblLen = strlen(label);
    int lblX   = bx + (bw - lblLen * 6) / 2;
    if (lblX < bx + 2) lblX = bx + 2;
    vga.setTextColor(labelC, bgC);
    vga.setCursor(lblX, by + 6);
    vga.print(label);

    int maxCh = (bw - 6) / 6;
    char tname[28] = {0};
    strncpy(tname, teacher, min(maxCh, 27));
    int tnameLen = strlen(tname);
    int tnameX   = bx + (bw - tnameLen * 6) / 2;
    if (tnameX < bx + 2) tnameX = bx + 2;
    vga.setTextColor(nameC, bgC);
    vga.setCursor(tnameX, by + bh - 14);
    vga.print(tname);

    if (isCabin && isHL) {
      vga.setTextColor(YELLOW, bgC);
      vga.setCursor(bx + bw - 18, by + bh / 2 - 4);
      vga.print(">");
    }
  };

  bool hlRoom13  = (ffHighlightCabin == 0 && ffHighlightRoom == 0);
  bool hlRoom14  = (ffHighlightCabin == 0 && ffHighlightRoom == 1);
  bool hlRoom15  = (ffHighlightCabin == 0 && ffHighlightRoom == 2);
  bool hlRoom19  = (ffHighlightCabin == 0 && ffHighlightRoom == 3);
  bool hlRoom20  = (ffHighlightCabin == 0 && ffHighlightRoom == 4);
  bool hlCabin2  = (ffHighlightCabin == 2);
  bool hlCabin3  = (ffHighlightCabin == 3);

  drawRoomBox(ROW0_LEFT_X, ROW0_Y, SR_W, SR_H, hlRoom13, false, "Room 13", ffStandaloneTeacher[0]);

  vga.setTextColor(vga.RGB(40, 50, 70), BLACK);
  vga.setCursor(ROW0_GAP_X + 4, ROW0_Y + 14); vga.print("|||");

  drawRoomBox(ROW0_CABIN2_X, ROW0_Y, CB_W, CB_H, hlCabin2, true, "CABIN 2", hlCabin2 ? "[ ENTER ]" : "Rms 7-12");

  vga.fillRect(82, ROW0_Y + SR_H / 2, ROW0_CABIN2_X - 82, 2, vga.RGB(30, 35, 55));

  drawRoomBox(ROW0_LEFT_X, ROW1_Y, SR_W, SR_H, hlRoom14, false, "Room 14", ffStandaloneTeacher[1]);
  drawRoomBox(ROW0_LEFT_X, ROW2_Y, SR_W, SR_H, hlRoom15, false, "Room 15", ffStandaloneTeacher[2]);

  vga.fillRect(ROW0_LEFT_X + SR_W, ROW0_Y + SR_H,
               2, ROW2_Y - (ROW0_Y + SR_H) + SR_H, vga.RGB(30, 35, 55));

  vga.setTextColor(vga.RGB(40, 50, 70), BLACK);
  vga.setCursor(10, ROW2_Y + SR_H + 4);
  vga.print("- - - - - - - - - - - - - - - - - - - -");

  drawRoomBox(ROW3_CABIN3_X, ROW3_Y, CB_W - 12, CB_H, hlCabin3, true, "CABIN 3", hlCabin3 ? "[ ENTER ]" : "Rms 16-18");
  drawRoomBox(ROW3_R19_X,    ROW3_Y, SR_W,       SR_H, hlRoom19, false, "Room 19", ffStandaloneTeacher[3]);
  drawRoomBox(ROW3_R20_X,    ROW3_Y, SR_W,       SR_H, hlRoom20, false, "Room 20", ffStandaloneTeacher[4]);

  int stBg = vga.RGB(12, 12, 18);
  vga.fillRect(ROW3_STAIRS_X, ROW3_Y, 90, CB_H, stBg);
  drawBox(ROW3_STAIRS_X, ROW3_Y, 90, CB_H, vga.RGB(40, 50, 70));
  vga.setTextColor(vga.RGB(60, 70, 90), stBg);
  vga.setCursor(ROW3_STAIRS_X + 18, ROW3_Y + 6);
  vga.print("STAIRS");

  char hint[40];
  if (ffHighlightCabin == 2)
    sprintf(hint, ">> CABIN 2 selected - press ENTER");
  else if (ffHighlightCabin == 3)
    sprintf(hint, ">> CABIN 3 selected - press ENTER");
  else
    sprintf(hint, "Teacher location shown above");

  vga.setTextColor(YELLOW, BLACK);
  int hx = (400 - strlen(hint) * 6) / 2;
  if (hx < 5) hx = 5;
  vga.setCursor(hx, 245);
  vga.print(hint);
}

// ========================================
// CABIN 2 MAP
// ========================================
void drawCabin2Map() {
  vga.clear(BLACK);

  vga.fillRect(0, 0, 400, 18, DARK_BLUE);
  drawBox(0, 0, 400, 18, CYAN);
  centerText(5, "CABIN 2 - ROOMS 7 TO 12", YELLOW, DARK_BLUE);

  drawFooter("[B:Back to Floor Map]");

  const int pairLeft[3]  = { 0, 2, 4 };
  const int pairRight[3] = { 1, 3, 5 };

  const int BOX_W      = 185;
  const int BOX_H      = 78;
  const int LEFT_X     = 5;
  const int RIGHT_X    = 210;
  const int FIRST_Y    = 22;
  const int ROW_STRIDE = 82;

  const char* personArt[5] = {
    "     ____         ",
    "     |    |        ",
    "    |____| O     ",
    "      |  /||\\   ",
    "     (  ) _/     "
  };

  for (int p = 0; p < 3; p++) {
    int lSlot = pairLeft[p];
    int rSlot = pairRight[p];
    int boxY  = FIRST_Y + p * ROW_STRIDE;

    for (int side = 0; side < 2; side++) {
      int   slot    = (side == 0) ? lSlot : rSlot;
      int   boxX    = (side == 0) ? LEFT_X : RIGHT_X;
      bool  isHL    = (slot == ffHighlightRoom);
      int   borderC = isHL ? YELLOW : vga.RGB(40, 50, 80);
      int   bgC     = isHL ? vga.RGB(0, 0, 60) : vga.RGB(18, 20, 32);
      int   textC   = isHL ? YELLOW : NEON_CYAN;

      vga.fillRect(boxX, boxY, BOX_W, BOX_H, bgC);
      drawBox(boxX, boxY, BOX_W, BOX_H, borderC);
      if (isHL) drawBox(boxX + 2, boxY + 2, BOX_W - 4, BOX_H - 4, YELLOW);

      char roomLabel[10];
      sprintf(roomLabel, "Room %d", CABIN2_SLOT_TO_ROOM[slot]);
      vga.setTextColor(isHL ? YELLOW : vga.RGB(80, 90, 110), bgC);
      vga.setCursor(boxX + 4, boxY + 3);
      vga.print(roomLabel);

      int artStartY = boxY + 14;
      for (int r = 0; r < 5; r++) {
        int artLen = strlen(personArt[r]);
        int artX   = boxX + (BOX_W - artLen * 6) / 2;
        vga.setTextColor(isHL ? vga.RGB(255, 200, 0) : vga.RGB(80, 90, 110), bgC);
        vga.setCursor(artX, artStartY + r * 8);
        vga.print(personArt[r]);
      }

      const char* tname    = cabin2Teacher[slot];
      int         tnameLen = strlen(tname);
      int         tnameX   = boxX + (BOX_W - tnameLen * 6) / 2;
      if (tnameX < boxX + 4) tnameX = boxX + 4;
      char nameDisplay[28] = {0};
      int  maxChars = (BOX_W - 8) / 6;
      strncpy(nameDisplay, tname, min(maxChars, 27));
      vga.setTextColor(textC, bgC);
      vga.setCursor(tnameX, boxY + BOX_H - 12);
      vga.print(nameDisplay);
    }
  }

  vga.setTextColor(vga.RGB(40, 50, 70), BLACK);
  vga.setCursor(197, 90);  vga.print("|");
  vga.setCursor(197, 100); vga.print("|");
  vga.setCursor(197, 110); vga.print("|");
}

// ========================================
// CABIN 3 MAP
// ========================================
void drawCabin3Map() {
  vga.clear(BLACK);

  vga.fillRect(0, 0, 400, 18, DARK_BLUE);
  drawBox(0, 0, 400, 18, CYAN);
  centerText(5, "CABIN 3 - ROOMS 16 TO 18", YELLOW, DARK_BLUE);

  drawFooter("[B:Back to Floor Map]");

  const int BOX_W   = 260;
  const int BOX_H   = 72;
  const int BOX_X   = (400 - BOX_W) / 2;
  const int FIRST_Y = 24;
  const int STRIDE  = 78;

  const char* personArt[5] = {
    "     ____         ",
    "     |    |        ",
    "    |____| O     ",
    "      |  /||\\   ",
    "     (  ) _/     "
  };

  for (int slot = 0; slot < CABIN3_ROOM_COUNT; slot++) {
    int  boxY = FIRST_Y + slot * STRIDE;
    bool isHL = (slot == ffHighlightRoom);

    int bgC    = isHL ? vga.RGB(0, 0, 60)  : vga.RGB(18, 20, 32);
    int border = isHL ? YELLOW              : vga.RGB(40, 50, 80);
    int textC  = isHL ? YELLOW              : NEON_CYAN;

    vga.fillRect(BOX_X, boxY, BOX_W, BOX_H, bgC);
    drawBox(BOX_X, boxY, BOX_W, BOX_H, border);
    if (isHL) drawBox(BOX_X + 2, boxY + 2, BOX_W - 4, BOX_H - 4, YELLOW);

    char roomLabel[10];
    sprintf(roomLabel, "Room %d", CABIN3_SLOT_TO_ROOM[slot]);
    vga.setTextColor(isHL ? YELLOW : vga.RGB(80, 90, 110), bgC);
    vga.setCursor(BOX_X + 4, boxY + 3);
    vga.print(roomLabel);

    int artStartY = boxY + 14;
    for (int r = 0; r < 3; r++) {
      int artLen = strlen(personArt[r]);
      int artX   = BOX_X + (BOX_W - artLen * 6) / 2;
      vga.setTextColor(isHL ? vga.RGB(255, 200, 0) : vga.RGB(80, 90, 110), bgC);
      vga.setCursor(artX, artStartY + r * 9);
      vga.print(personArt[r]);
    }

    const char* tname    = cabin3Teacher[slot];
    int         tnameLen = strlen(tname);
    int         tnameX   = BOX_X + (BOX_W - tnameLen * 6) / 2;
    if (tnameX < BOX_X + 4) tnameX = BOX_X + 4;
    char nameDisplay[38] = {0};
    int  maxChars = (BOX_W - 8) / 6;
    strncpy(nameDisplay, tname, min(maxChars, 37));
    vga.setTextColor(textC, bgC);
    vga.setCursor(tnameX, boxY + BOX_H - 12);
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
    else if (strcmp(category, "office")  == 0) { addToCategory(office,   annId, title, message, true); beep(1); }
    markAnnouncementAsSeen(annId);
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

  if (up || down) playSelectSound();

  if (currentState == MENU) {
    if (up)   { selectedMenu--; if (selectedMenu < 0) selectedMenu = 4; drawMenu(); }
    if (down) { selectedMenu++; if (selectedMenu > 4) selectedMenu = 0; drawMenu(); }
    if (enter) {
      playConfirmSound();
      if      (selectedMenu == 0) { currentState = URGENT_VIEW;  showCategory(urgent,   "URGENT NOTICE");  }
      else if (selectedMenu == 1) { currentState = GENERAL_VIEW; showCategory(general,  "GENERAL NOTICE"); }
      else if (selectedMenu == 2) { currentState = EVENT_VIEW;   showCategory(eventCat, "EVENT NOTICE");   }
      else if (selectedMenu == 3) {
        selOfficeFloor    = 0;
        officeFloorScroll = 0;
        currentState = OFFICE_FLOOR;
        drawOfficeFloorSelect();
      }
      else if (selectedMenu == 4) {
        if (selYear >= totalYears && totalYears > 0) selYear = 0;
        yearScrollOff = (selYear >= 5) ? selYear - 4 : 0;
        currentState = TT_YEAR;
        showLoading("Loading years...");
        fetchYears();
        drawYearSelect();
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
    if (up)   { if (selYear > 0)              { selYear--; if (selYear < yearScrollOff) yearScrollOff = selYear; } drawYearSelect(); }
    if (down) { if (selYear < totalYears - 1) { selYear++; if (selYear >= yearScrollOff + 5) yearScrollOff = selYear - 4; } drawYearSelect(); }
    if (enter) {
      if (selSession >= totalSessions) selSession = 0;
      sessionScrollOff = (selSession >= 5) ? selSession - 4 : 0;
      showLoading("Loading sessions...");
      fetchSessions(years[selYear].id);
      if (selSession >= totalSessions) { selSession = 0; sessionScrollOff = 0; }
      currentState = TT_SESSION;
      drawSessionSelect();
    }
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == TT_SESSION) {
    if (up)   { if (selSession > 0)              { selSession--; if (selSession < sessionScrollOff) sessionScrollOff = selSession; } drawSessionSelect(); }
    if (down) { if (selSession < totalSessions-1) { selSession++; if (selSession >= sessionScrollOff + 5) sessionScrollOff = selSession - 4; } drawSessionSelect(); }
    if (enter) {
      if (selSection >= totalSections) selSection = 0;
      sectionScrollOff = (selSection >= 5) ? selSection - 4 : 0;
      showLoading("Loading sections...");
      fetchSections(sessions[selSession].id);
      if (selSection >= totalSections) { selSection = 0; sectionScrollOff = 0; }
      currentState = TT_SECTION;
      drawSectionSelect();
    }
    if (back) { currentState = TT_YEAR; drawYearSelect(); }
  }
  else if (currentState == TT_SECTION) {
    if (up)   { if (selSection > 0)              { selSection--; if (selSection < sectionScrollOff) sectionScrollOff = selSection; } drawSectionSelect(); }
    if (down) { if (selSection < totalSections-1) { selSection++; if (selSection >= sectionScrollOff + 5) sectionScrollOff = selSection - 4; } drawSectionSelect(); }
    if (enter) {
      showLoading("Loading timetable...");
      fetchTimetable(selYear, selSession, selSection);
      currentState = TT_DISPLAY;
      drawTimetable();
    }
    if (back) { currentState = TT_SESSION; drawSessionSelect(); }
  }
  else if (currentState == TT_DISPLAY) {
    if (enter) { currentState = MENU; drawMenu(); }
    if (back)  { currentState = TT_SECTION; drawSectionSelect(); }
  }

  else if (currentState == OFFICE_FLOOR) {
    if (up)   { if (selOfficeFloor > 0) selOfficeFloor--; drawOfficeFloorSelect(); }
    if (down) { if (selOfficeFloor < OFFICE_FLOOR_COUNT - 1) selOfficeFloor++; drawOfficeFloorSelect(); }
    if (enter) {
      playConfirmSound();
      if (selOfficeFloor == 0) {
        showLoading("Loading office data...");
        fetchOfficeTeachers(OFFICE_FLOORS[0]);
        selOfficeTeacher    = 0;
        officeTeacherScroll = 0;
        currentState = OFFICE_TEACHERS;
        drawOfficeTeacherSelect();
      } else {
        showLoading("Loading first floor data...");
        fetchFirstFloorTeachers();
        selFFTeacher    = 0;
        ffTeacherScroll = 0;
        ffHighlightCabin = 0;
        ffHighlightRoom  = -1;
        currentState = FF_TEACHER_LIST;
        drawFFTeacherList();
      }
    }
    if (back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == OFFICE_TEACHERS) {
    if (up) {
      if (selOfficeTeacher > 0) {
        selOfficeTeacher--;
        if (selOfficeTeacher < officeTeacherScroll) officeTeacherScroll = selOfficeTeacher;
      }
      drawOfficeTeacherSelect();
    }
    if (down) {
      if (selOfficeTeacher < OFFICE_ROOM_COUNT - 1) {
        selOfficeTeacher++;
        if (selOfficeTeacher >= officeTeacherScroll + 5) officeTeacherScroll = selOfficeTeacher - 4;
      }
      drawOfficeTeacherSelect();
    }
    if (enter) {
      officeHighlightRoom = selOfficeTeacher;
      currentState = OFFICE_MAP;
      drawOfficeMap();
    }
    if (back) {
      currentState = OFFICE_FLOOR;
      drawOfficeFloorSelect();
    }
  }
  else if (currentState == OFFICE_MAP) {
    if (enter) { officeHighlightRoom = -1; currentState = MENU; drawMenu(); }
    if (back)  { officeHighlightRoom = -1; currentState = OFFICE_TEACHERS; drawOfficeTeacherSelect(); }
  }
  else if (currentState == FF_TEACHER_LIST) {
    if (up) {
      if (selFFTeacher > 0) {
        selFFTeacher--;
        if (selFFTeacher < ffTeacherScroll) ffTeacherScroll = selFFTeacher;
      }
      drawFFTeacherList();
    }
    if (down) {
      if (selFFTeacher < ffTeacherCount - 1) {
        selFFTeacher++;
        if (selFFTeacher >= ffTeacherScroll + 5) ffTeacherScroll = selFFTeacher - 4;
      }
      drawFFTeacherList();
    }
    if (enter) {
      ffSelectedTeacherIdx = selFFTeacher;
      resolveFFTeacherHighlight(selFFTeacher);
      currentState = FF_FLOOR_MAP;
      drawFFFloorMap();
    }
    if (back) { currentState = OFFICE_FLOOR; drawOfficeFloorSelect(); }
  }
  else if (currentState == FF_FLOOR_MAP) {
    if (enter) {
      if (ffHighlightCabin == 2) {
        currentState = FF_CABIN2_MAP;
        drawCabin2Map();
      } else if (ffHighlightCabin == 3) {
        currentState = FF_CABIN3_MAP;
        drawCabin3Map();
      }
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
// WiFi EVENT HANDLERS
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

  for (int i = 0; i < OFFICE_ROOM_COUNT;    i++) strncpy(officeTeacher[i],       "----", 31);
  for (int i = 0; i < FF_STANDALONE_COUNT;  i++) strncpy(ffStandaloneTeacher[i], "----", 31);
  for (int i = 0; i < CABIN2_ROOM_COUNT;    i++) strncpy(cabin2Teacher[i],       "----", 31);
  for (int i = 0; i < CABIN3_ROOM_COUNT;    i++) strncpy(cabin3Teacher[i],       "----", 31);

  vga.init(vga.MODE400x300, redPin, greenPin, bluePin, hsyncPin, vsyncPin);
  vga.setFont(Font6x8);

  BLACK      = vga.RGB(0,   0,   0);
  WHITE      = vga.RGB(255, 255, 255);
  RED        = vga.RGB(255, 0,   0);
  GREEN      = vga.RGB(0,   200, 0);
  BLUE       = vga.RGB(0,   0,   255);
  YELLOW     = vga.RGB(255, 255, 0);
  CYAN       = vga.RGB(0,   255, 255);
  GRAY       = vga.RGB(60,  65,  80);
  LIGHT_BLUE = vga.RGB(100, 120, 255);
  DARK_BLUE  = vga.RGB(0,   0,   80);

  NEON_PINK   = vga.RGB(255, 20,  147);
  NEON_BLUE   = vga.RGB(0,   200, 255);
  DARK_CYBER  = vga.RGB(10,  12,  22);
  CARD_DARK   = vga.RGB(18,  20,  32);
  NEON_GREEN  = vga.RGB(0,   220, 100);
  NEON_YELLOW = vga.RGB(255, 240, 50);
  NEON_PURPLE = vga.RGB(180, 50,  255);
  DIM_BLUE    = vga.RGB(0,   40,  80);
  GLOW_CYAN   = vga.RGB(0,   180, 230);
  NEON_CYAN   = vga.RGB(0,   230, 200);

  vga.clear(BLACK);
  vga.fillRect(0, 0, 400, 300, DARK_CYBER);
  drawBox(10, 10, 380, 280, NEON_BLUE);
  drawBox(14, 14, 372, 272, vga.RGB(0, 40, 80));

  drawGlowText(80, 90, ">> NEURAL INTERFACE <<", NEON_PINK, DARK_CYBER);
  drawGlowText(110, 115, "DEPT NOTICE BOARD", NEON_BLUE, DARK_CYBER);

  vga.setTextColor(vga.RGB(80, 90, 120), DARK_CYBER);
  vga.setCursor(60, 150); vga.print("WiFiManager Starting...");
  vga.setCursor(60, 168); vga.print("Connect: NoticeBoardSetup");
  vga.setCursor(60, 186); vga.print("Open: 192.168.4.1");

  vga.fillRect(14, 260, 372, 2, NEON_BLUE);
  vga.setTextColor(vga.RGB(60, 70, 100), DARK_CYBER);
  vga.setCursor(80, 265); vga.print("[ SYSTEM BOOT SEQUENCE ]");

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.onEvent(onWiFiEvent);

  WiFi.begin("Fast net fiber n", "03004721644aa");
  if (WiFi.waitForConnectResult(15000) != WL_CONNECTED) {
    wifiManager.setConfigPortalTimeout(0);
    wifiManager.autoConnect("NoticeBoardSetup");
  }

  resolveServerIP();

  vga.clear(DARK_CYBER);
  drawBox(10, 10, 380, 280, NEON_GREEN);
  drawGlowText(100, 100, "WiFi Connected!", NEON_GREEN, DARK_CYBER);
  vga.setTextColor(NEON_BLUE, DARK_CYBER);
  vga.setCursor(60, 130); vga.print("SSID: "); vga.print(WiFi.SSID().c_str());
  vga.setCursor(60, 148); vga.print("IP:   "); vga.print(WiFi.localIP().toString().c_str());
  delay(1500);

  client.setServer(mqttServer, mqttPort);
  client.setBufferSize(512);
  client.setCallback(onMessage);

  showLoading("Connecting to MQTT...");
  connectMQTT();
  delay(500);

  showLoading("Loading announcements...");
  fetchAnnouncements();

  showLoading("Loading years...");
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
  updatePulse();

  if (millis() - lastAnnFetch > ANN_FETCH_INTERVAL) {
    lastAnnFetch = millis();
    if (currentState == MENU && WiFi.status() == WL_CONNECTED) {
      fetchAnnouncements();
    }
  }

  if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  delay(10);
}
