#include <WiFi.h>
#include <esp_wpa2.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Lib.h>
#include <Ressources/Font6x8.h>
#include <HTTPClient.h>

// ================= WIFI (EDUROAM) =================
const char* ssid        = "eduroam";
const char* identity    = "2024cs230@student.uet.edu.pk";
const char* password    = "AU559YUH";
const char* serverIP = "10.5.116.90";

// ================= MQTT =================
const char* mqttServer = "10.5.116.90";
const int   mqttPort   = 1883;

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
const int backBtn   = 4;

// ================= OUTPUT =================
const int buzzer    = 18;
const int redLED    = 25;
const int greenLED  = 26;
const int yellowLED = 5;

VGA3Bit vga;
WiFiClient   espClient;
PubSubClient client(espClient);

// ================= COLORS =================
int BLACK, WHITE, BLUE, YELLOW, CYAN, GRAY, LIGHT_BLUE, DARK_BLUE, RED, GREEN;

// ================= ANNOUNCEMENTS =================
struct Announcement {
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

Category urgent  = {{}, 0, 0};
Category general = {{}, 0, 0};
Category eventCat= {{}, 0, 0};
Category office  = {{}, 0, 0};

// ================= YEARS/SESSIONS/SECTIONS =================
const int MAX_YEARS    = 10;
const int MAX_SESSIONS = 10;
const int MAX_SECTIONS = 10;

struct YearInfo {
  int  id;
  char name[10];
};

struct SessionInfo {
  int  id;
  char name[40];
};

struct SectionInfo {
  int  id;
  char name[10];
};

YearInfo    years[MAX_YEARS];
SessionInfo sessions[MAX_SESSIONS];
SectionInfo sections[MAX_SECTIONS];
int totalYears    = 0;
int totalSessions = 0;
int totalSections = 0;

// ================= TIMETABLE =================
struct TTEntry {
  char time[20];
  char subject[30];
  char code[10];
  char teacher[30];
  char room[10];
  char day[4];
};

const int MAX_TT_ENTRIES = 15;
struct TimetableData {
  TTEntry entries[MAX_TT_ENTRIES];
  int     count;
  int     yearIdx;
  int     sessionIdx;
  int     sectionIdx;
  char    semester[20];
  char    session_name[40];
  char    term[20];
};

TimetableData currentTT = {{}, 0, -1, -1, -1, "", "", ""};

// ================= TIMETABLE FILTERS =================
int selectedDayFilter = -1;
const char* dayNames[5] = { "MON", "TUE", "WED", "THU", "FRI" };

// ================= TIMETABLE GRID =================
struct TTGrid {
  char days[5][10];
  char slots[10][20];
  char cells[5][10][15];
  int  rowspan[5][10];
  int  slotCount;
  int  dayCount;
};

TTGrid ttGrid = {{}, {}, {}, {}, 0, 0};

// ================= SUBJECT COLOR PALETTE =================
struct SubjectColor { int bg; int text; };

SubjectColor getSubjectColor(const char* code) {
  unsigned int hash = 0;
  for (int i = 0; code[i]; i++) hash = hash * 31 + (unsigned char)code[i];
  int idx = hash % 6;
  switch (idx) {
    case 0: return { vga.RGB(0,   0,   180), vga.RGB(140, 200, 255) };
    case 1: return { vga.RGB(160, 0,   0),   vga.RGB(255, 160, 160) };
    case 2: return { vga.RGB(0,   120, 55),  vga.RGB(160, 255, 200) };
    case 3: return { vga.RGB(130, 75,  0),   vga.RGB(255, 210, 140) };
    case 4: return { vga.RGB(100, 0,   155), vga.RGB(220, 160, 255) };
    case 5: return { vga.RGB(0,   115, 125), vga.RGB(160, 255, 255) };
    default:return { vga.RGB(80,  80,  80),  vga.RGB(220, 220, 220) };
  }
}

void truncStr(char* dst, const char* src, int maxChars) {
  if (maxChars < 1) { dst[0] = '\0'; return; }
  int len = strlen(src);
  if (len <= maxChars) {
    strcpy(dst, src);
  } else {
    strncpy(dst, src, maxChars - 1);
    dst[maxChars - 1] = '>';
    dst[maxChars] = '\0';
  }
}

int timeToHour(const char* timeStr) {
  int hour = 0;
  sscanf(timeStr, "%d:", &hour);
  return hour;
}

int getHoursDuration(const char* startTime, const char* endTime) {
  int startHour = timeToHour(startTime);
  int endHour = timeToHour(endTime);
  int duration = endHour - startHour;
  if (duration <= 0) duration = 1;
  if (duration > 5) duration = 1;
  return duration;
}

void buildTimetableGrid() {
  memset(&ttGrid, 0, sizeof(ttGrid));
  ttGrid.slotCount = 0;
  ttGrid.dayCount = 0;

  for (int i = 0; i < currentTT.count; i++) {
    bool dayMatches = (selectedDayFilter == -1);
    if (selectedDayFilter >= 0)
      dayMatches = (strcmp(currentTT.entries[i].day, dayNames[selectedDayFilter]) == 0);
    if (!dayMatches) continue;

    bool found = false;
    for (int d = 0; d < ttGrid.dayCount; d++) {
      if (strcmp(ttGrid.days[d], currentTT.entries[i].day) == 0) { found = true; break; }
    }
    if (!found && ttGrid.dayCount < 5) {
      strcpy(ttGrid.days[ttGrid.dayCount], currentTT.entries[i].day);
      ttGrid.dayCount++;
    }
  }

  ttGrid.slotCount = 0;
  for (int h = 8; h <= 16 && ttGrid.slotCount < 10; h++) {
    sprintf(ttGrid.slots[ttGrid.slotCount], "%d:00", h);
    ttGrid.slotCount++;
  }

  for (int d = 0; d < ttGrid.dayCount; d++) {
    for (int s = 0; s < ttGrid.slotCount; s++) {
      strcpy(ttGrid.cells[d][s], "-");
      ttGrid.rowspan[d][s] = 0;
    }
  }

  for (int i = 0; i < currentTT.count; i++) {
    bool dayMatches = (selectedDayFilter == -1);
    if (selectedDayFilter >= 0)
      dayMatches = (strcmp(currentTT.entries[i].day, dayNames[selectedDayFilter]) == 0);
    if (!dayMatches) continue;

    int dayIdx = -1;
    for (int d = 0; d < ttGrid.dayCount; d++) {
      if (strcmp(ttGrid.days[d], currentTT.entries[i].day) == 0) { dayIdx = d; break; }
    }
    if (dayIdx < 0) continue;

    int startHour = timeToHour(currentTT.entries[i].time);
    int startSlot = -1;
    for (int s = 0; s < ttGrid.slotCount; s++) {
      if (timeToHour(ttGrid.slots[s]) == startHour) { startSlot = s; break; }
    }
    if (startSlot < 0) continue;

    int duration = 1;
    const char* dashPos = strchr(currentTT.entries[i].time, '-');
    if (dashPos) {
      char endTimeStr[10] = "";
      strncpy(endTimeStr, dashPos + 1, 5);
      endTimeStr[5] = '\0';
      duration = getHoursDuration(currentTT.entries[i].time, endTimeStr);
    }
    if (duration < 1) duration = 1;
    if (duration > 5) duration = 1;

    if (startSlot + duration <= ttGrid.slotCount) {
      sprintf(ttGrid.cells[dayIdx][startSlot], "%.4s(%.4s)",
              currentTT.entries[i].code,
              currentTT.entries[i].room);
      ttGrid.rowspan[dayIdx][startSlot] = duration;

      for (int j = 1; j < duration; j++) {
        if (startSlot + j < ttGrid.slotCount) {
          strcpy(ttGrid.cells[dayIdx][startSlot + j], "^");
          ttGrid.rowspan[dayIdx][startSlot + j] = -1;
        }
      }
    }
  }
}

// ================= UI STATE =================
enum UIState {
  MENU,
  URGENT_VIEW, GENERAL_VIEW, EVENT_VIEW, OFFICE_VIEW,
  TT_YEAR, TT_SESSION, TT_SECTION, TT_DAY_FILTER, TT_DISPLAY
};

UIState currentState     = MENU;
int     selectedMenu     = 0;
int     selYear          = 0;
int     selSession       = 0;
int     selSection       = 0;
int     ttStartIdx       = 0;
int     yearScrollOff    = 0;
int     sessionScrollOff = 0;
int     sectionScrollOff = 0;

// ================= TIMING =================
unsigned long lastPress    = 0;
const int     DEBOUNCE     = 250;
unsigned long lastBeepTime = 0;
const int     BEEP_COOL    = 500;
unsigned long lastAnnFetch = -31000;
const int     ANN_FETCH_INTERVAL = 30000;

// ================= FORWARD DECLARATIONS =================
void drawMenu();
void updateLEDs();
void buildTimetableGrid();

// ========================================
// BUZZER
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

// ========================================
// VGA HELPERS
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
  vga.fillRect(x, y, w, 2, color);
  vga.fillRect(x, y + h - 2, w, 2, color);
  vga.fillRect(x, y, 2, h, color);
  vga.fillRect(x + w - 2, y, 2, h, color);
}

void drawHeader() {
  vga.fillRect(0, 0, 400, 50, DARK_BLUE);
  drawBox(10, 5, 380, 40, CYAN);
  centerText(20, "DEPARTMENT NOTICE BOARD", YELLOW, DARK_BLUE);
}

void drawFooter(const char* hint) {
  vga.fillRect(0, 285, 400, 15, DARK_BLUE);
  drawBox(10, 285, 380, 14, CYAN);
  if (client.connected()) {
    leftText(12, 290, "[ON]", GREEN, DARK_BLUE);
  } else {
    leftText(12, 290, "[OFF]", RED, DARK_BLUE);
  }
  leftText(50, 290, hint, LIGHT_BLUE, DARK_BLUE);
}

void drawBase(const char* hint) {
  vga.clear(BLACK);
  drawHeader();
  drawFooter(hint);
}

void showLoading(const char* msg) {
  drawBase("");
  centerText(150, msg, CYAN, BLACK);
  centerText(168, "Please wait...", LIGHT_BLUE, BLACK);
}

// ========================================
// LEDs
// ========================================
void updateLEDs() {
  bool u = false, g = false, e = false;
  for (int i = 0; i < urgent.count; i++) if (urgent.items[i].isNew) u = true;
  for (int i = 0; i < general.count; i++) if (general.items[i].isNew) g = true;
  for (int i = 0; i < eventCat.count; i++) if (eventCat.items[i].isNew) e = true;
  digitalWrite(redLED, u);
  digitalWrite(greenLED, g);
  digitalWrite(yellowLED, e);
}

// ========================================
// CATEGORY HELPERS
// ========================================
void addToCategory(Category &cat, const char* title, const char* msg) {
  if (cat.count >= MAX_PER_CAT) {
    for (int i = 0; i < MAX_PER_CAT - 1; i++)
      cat.items[i] = cat.items[i + 1];
    cat.count = MAX_PER_CAT - 1;
  }
  strncpy(cat.items[cat.count].title, title, 59);
  strncpy(cat.items[cat.count].message, msg, 199);
  cat.items[cat.count].title[59] = '\0';
  cat.items[cat.count].message[199] = '\0';
  cat.items[cat.count].isNew = true;
  cat.count++;
}

void clearAllCategories() {
  urgent.count = 0; urgent.index = 0;
  general.count = 0; general.index = 0;
  eventCat.count = 0; eventCat.index = 0;
  office.count = 0; office.index = 0;
}

// ========================================
// HTTP
// ========================================
void fetchAnnouncements() {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/announcements/approved/all";
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) { http.end(); return; }
    
    clearAllCategories();
    int loaded = 0;
    for (JsonObject ann : doc.as<JsonArray>()) {
      const char* title = ann["title"] | "";
      const char* message = ann["message"] | "";
      const char* category = ann["category"] | "";

      if (strcmp(category, "urgent") == 0) { addToCategory(urgent, title, message); loaded++; }
      else if (strcmp(category, "general") == 0) { addToCategory(general, title, message); loaded++; }
      else if (strcmp(category, "event") == 0) { addToCategory(eventCat, title, message); loaded++; }
      else if (strcmp(category, "office") == 0) { addToCategory(office, title, message); loaded++; }
    }
    
    Serial.printf("✅ Loaded %d announcements\n", loaded);
    updateLEDs();
    if (currentState == MENU) drawMenu();
  }
  http.end();
}

void fetchYears() {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/years";
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    totalYears = 0;
    for (JsonObject y : doc.as<JsonArray>()) {
      if (totalYears >= MAX_YEARS) break;
      years[totalYears].id = y["year_id"];
      strncpy(years[totalYears].name, y["year_name"] | "Unknown", 9);
      years[totalYears].name[9] = '\0';
      totalYears++;
    }
    Serial.printf("✅ Loaded %d years\n", totalYears);
  }
  http.end();
}

void fetchSessions(int yearId) {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/sessions/" + yearId;
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    totalSessions = 0;
    for (JsonObject s : doc.as<JsonArray>()) {
      if (totalSessions >= MAX_SESSIONS) break;
      sessions[totalSessions].id = s["session_id"];
      strncpy(sessions[totalSessions].name, s["session_name"] | "Unknown", 39);
      sessions[totalSessions].name[39] = '\0';
      totalSessions++;
    }
    Serial.printf("✅ Loaded %d sessions\n", totalSessions);
  }
  http.end();
}

void fetchSections(int sessionId) {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/sections/" + sessionId;
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    totalSections = 0;
    for (JsonObject s : doc.as<JsonArray>()) {
      if (totalSections >= MAX_SECTIONS) break;
      sections[totalSections].id = s["section_id"];
      strncpy(sections[totalSections].name, s["section_name"] | "?", 9);
      sections[totalSections].name[9] = '\0';
      totalSections++;
    }
    Serial.printf("✅ Loaded %d sections\n", totalSections);
  }
  http.end();
}

void fetchTimetable(int yIdx, int sIdx, int secIdx) {
  int yearId = years[yIdx].id;
  int sessionId = sessions[sIdx].id;
  int sectionId = sections[secIdx].id;

  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/timetable/" + yearId + "/" + sessionId + "/" + sectionId;
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);

    if (doc["success"] == true) {
      currentTT.count = 0;
      currentTT.yearIdx = yIdx;
      currentTT.sessionIdx = sIdx;
      currentTT.sectionIdx = secIdx;
      selectedDayFilter = -1;

      strncpy(currentTT.semester, doc["semester"] | "Unknown", 19);
      strncpy(currentTT.session_name, doc["session_name"] | "Unknown", 39);
      strncpy(currentTT.term, doc["term"] | "Unknown", 19);
      currentTT.semester[19] = '\0';
      currentTT.session_name[39] = '\0';
      currentTT.term[19] = '\0';

      for (JsonObject e : doc["data"].as<JsonArray>()) {
        if (currentTT.count >= MAX_TT_ENTRIES) break;
        int i = currentTT.count;

        String t = String(e["start_time"] | "") + "-" + String(e["end_time"] | "");
        strncpy(currentTT.entries[i].time, t.c_str(), 19);
        strncpy(currentTT.entries[i].subject, e["subject_name"] | "", 29);
        strncpy(currentTT.entries[i].code, e["subject_code"] | "", 9);
        strncpy(currentTT.entries[i].teacher, e["teacher_name"] | "", 29);
        strncpy(currentTT.entries[i].room, e["room_number"] | "", 9);
        strncpy(currentTT.entries[i].day, e["day_name"] | "", 3);

        currentTT.entries[i].time[19] = '\0';
        currentTT.entries[i].subject[29] = '\0';
        currentTT.entries[i].code[9] = '\0';
        currentTT.entries[i].teacher[29] = '\0';
        currentTT.entries[i].room[9] = '\0';
        currentTT.entries[i].day[3] = '\0';

        currentTT.count++;
      }
      Serial.printf("✅ Loaded %d timetable entries\n", currentTT.count);
    }
  }
  http.end();
}

// ========================================
// SCREENS
// ========================================
void drawMenu() {
  drawBase("U/D:Navigate  E:Select");

  const char* items[5] = {
    "URGENT NOTICES",
    "GENERAL NOTICES",
    "EVENT NOTICES",
    "OFFICE NOTICES",
    "TIME TABLE"
  };

  int colors[5] = {RED, GREEN, YELLOW, CYAN, BLUE};
  Category* cats[4] = {&urgent, &general, &eventCat, &office};

  for (int i = 0; i < 5; i++) {
    int y = 58 + i * 42;
    bool sel = (i == selectedMenu);

    if (sel) {
      vga.fillRect(25, y, 350, 38, DARK_BLUE);
      drawBox(25, y, 350, 38, colors[i]);
      vga.setTextColor(YELLOW, DARK_BLUE);
    } else {
      vga.fillRect(25, y, 350, 38, GRAY);
      drawBox(25, y, 350, 38, LIGHT_BLUE);
      vga.setTextColor(WHITE, GRAY);
    }

    vga.fillRect(30, y + 9, 16, 16, colors[i]);
    vga.setCursor(55, y + 14);
    vga.print(items[i]);

    if (i < 4) {
      char cnt[8];
      sprintf(cnt, "(%d)", cats[i]->count);
      vga.setCursor(255, y + 14);
      vga.print(cnt);

      bool hasNew = false;
      for (int j = 0; j < cats[i]->count; j++)
        if (cats[i]->items[j].isNew) { hasNew = true; break; }
      if (hasNew) {
        vga.fillRect(308, y + 9, 35, 16, RED);
        vga.setTextColor(WHITE, RED);
        vga.setCursor(313, y + 14);
        vga.print("NEW");
      }
    }

    if (sel) {
      vga.setTextColor(CYAN, DARK_BLUE);
      vga.setCursor(368, y + 14);
      vga.print(">");
    }
  }
  updateLEDs();
}

void showCategory(Category &cat, const char* title) {
  drawBase("U/D:Navigate  E:Back");

  if (cat.count == 0) {
    centerText(150, "NO ANNOUNCEMENTS", CYAN, BLACK);
    centerText(168, "in this category", LIGHT_BLUE, BLACK);
    return;
  }

  if (cat.index >= cat.count) cat.index = 0;
  if (cat.index < 0) cat.index = cat.count - 1;

  vga.fillRect(20, 58, 360, 45, DARK_BLUE);
  drawBox(20, 58, 360, 45, CYAN);
  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(30, 65);
  vga.print(title);
  vga.setTextColor(WHITE, DARK_BLUE);
  vga.setCursor(30, 80);
  vga.print(cat.items[cat.index].title);

  vga.fillRect(20, 110, 360, 150, GRAY);
  drawBox(20, 110, 360, 150, LIGHT_BLUE);
  vga.setTextColor(WHITE, GRAY);

  const char* msg = cat.items[cat.index].message;
  int lineY = 120, pos = 0, len = strlen(msg);
  while (pos < len && lineY < 255) {
    char line[53] = {0};
    strncpy(line, msg + pos, 52);
    vga.setCursor(28, lineY);
    vga.print(line);
    pos += 52;
    lineY += 16;
  }

  char nav[40];
  sprintf(nav, "Notice %d of %d", cat.index + 1, cat.count);
  centerText(268, nav, CYAN, BLACK);

  cat.items[cat.index].isNew = false;
  updateLEDs();
}

void drawYearSelect() {
  drawBase("U/D:Navigate  E:Select");
  centerText(58, "SELECT YEAR / BATCH", YELLOW, BLACK);

  if (totalYears == 0) { centerText(150, "No years found!", CYAN, BLACK); return; }

  const int VISIBLE = 5;
  int end = (yearScrollOff + VISIBLE < totalYears) ? (yearScrollOff + VISIBLE) : totalYears;

  for (int i = yearScrollOff; i < end; i++) {
    int slot = i - yearScrollOff;
    int y = 75 + slot * 38;
    bool sel = (i == selYear);

    vga.fillRect(50, y, 300, 33, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 33, sel ? YELLOW : LIGHT_BLUE);
    centerText(y + 12, years[i].name, sel ? YELLOW : WHITE, sel ? DARK_BLUE : GRAY);
  }
}

void drawSessionSelect() {
  drawBase("U/D:Navigate  E:Select");

  char header[60];
  sprintf(header, "%s - SELECT SESSION", years[selYear].name);
  centerText(58, header, YELLOW, BLACK);

  if (totalSessions == 0) { centerText(150, "No sessions found!", CYAN, BLACK); return; }

  const int VISIBLE = 5;
  int end = (sessionScrollOff + VISIBLE < totalSessions) ? (sessionScrollOff + VISIBLE) : totalSessions;

  for (int i = sessionScrollOff; i < end; i++) {
    int slot = i - sessionScrollOff;
    int y = 85 + slot * 38;
    bool sel = (i == selSession);

    vga.fillRect(50, y, 300, 33, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 33, sel ? YELLOW : LIGHT_BLUE);
    centerText(y + 12, sessions[i].name, sel ? YELLOW : WHITE, sel ? DARK_BLUE : GRAY);
  }
}

void drawSectionSelect() {
  drawBase("U/D:Navigate  E:Select");

  char title[60];
  sprintf(title, "%s - SELECT SECTION", sessions[selSession].name);
  centerText(58, title, YELLOW, BLACK);

  if (totalSections == 0) { centerText(150, "No sections found!", CYAN, BLACK); return; }

  const int VISIBLE = 5;
  int end = (sectionScrollOff + VISIBLE < totalSections) ? (sectionScrollOff + VISIBLE) : totalSections;

  for (int i = sectionScrollOff; i < end; i++) {
    int slot = i - sectionScrollOff;
    int y = 85 + slot * 38;
    bool sel = (i == selSection);

    vga.fillRect(100, y, 200, 33, sel ? DARK_BLUE : GRAY);
    drawBox(100, y, 200, 33, sel ? GREEN : LIGHT_BLUE);
    centerText(y + 12, sections[i].name, sel ? GREEN : WHITE, sel ? DARK_BLUE : GRAY);
  }
}

void drawDayFilter() {
  drawBase("U/D:Select Day  E:View");
  centerText(58, "FILTER BY DAY", YELLOW, BLACK);

  int selectedOption = selectedDayFilter + 1;

  {
    int y = 75;
    bool sel = (selectedOption == 0);
    vga.fillRect(50, y, 300, 33, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 33, sel ? YELLOW : LIGHT_BLUE);
    centerText(y + 12, "ALL DAYS", sel ? YELLOW : WHITE, sel ? DARK_BLUE : GRAY);
  }

  for (int i = 0; i < 5; i++) {
    int y = 113 + i * 38;
    bool sel = (selectedOption == i + 1);
    vga.fillRect(50, y, 300, 33, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 33, sel ? GREEN : LIGHT_BLUE);
    centerText(y + 12, dayNames[i], sel ? GREEN : WHITE, sel ? DARK_BLUE : GRAY);
  }
}

void drawTimetable() {
  vga.clear(BLACK);

  vga.fillRect(0, 0, 400, 48, DARK_BLUE);
  drawBox(5, 3, 390, 42, CYAN);

  char hdr1[60];
  sprintf(hdr1, "%s | Sec:%s | %s",
          currentTT.semester,
          sections[selSection].name,
          currentTT.term);
  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(10, 10);
  vga.print(hdr1);

  char hdr2[60];
  if (selectedDayFilter >= 0)
    sprintf(hdr2, "Day: %s only", dayNames[selectedDayFilter]);
  else
    sprintf(hdr2, "All days | %s", currentTT.session_name);
  vga.setTextColor(CYAN, DARK_BLUE);
  vga.setCursor(10, 26);
  vga.print(hdr2);

  if (currentTT.count == 0) {
    vga.fillRect(20, 60, 360, 200, GRAY);
    drawBox(20, 60, 360, 200, LIGHT_BLUE);
    vga.setTextColor(RED, GRAY);
    vga.setCursor(80, 150);
    vga.print("NO DATA");
    goto tt_footer;
  }

  buildTimetableGrid();

  if (ttGrid.dayCount == 0) {
    vga.fillRect(20, 60, 360, 200, GRAY);
    drawBox(20, 60, 360, 200, LIGHT_BLUE);
    vga.setTextColor(RED, GRAY);
    vga.setCursor(100, 150);
    vga.print("NO SCHEDULE");
    goto tt_footer;
  }

  {
    int numDays = (ttGrid.dayCount > 5) ? 5 : ttGrid.dayCount;
    int numTimes = (ttGrid.slotCount > 9) ? 9 : ttGrid.slotCount;

    const int gridX = 5;
    const int gridY = 50;
    const int dayColW = 30;
    const int hdrH = 18;
    const int rowH = 28;

    int available = 395 - gridX - dayColW;
    int cellW = available / numTimes;
    if (cellW < 34) cellW = 34;
    if (cellW > 54) cellW = 54;

    int freeColor = vga.RGB(0, 100, 0);
    int freeBorder = vga.RGB(0, 150, 0);
    int freeTextCol = vga.RGB(0, 200, 0);

    vga.fillRect(gridX, gridY, dayColW, hdrH, DARK_BLUE);
    drawBox(gridX, gridY, dayColW, hdrH, CYAN);
    vga.setTextColor(YELLOW, DARK_BLUE);
    vga.setCursor(gridX + 2, gridY + 5);
    vga.print("DAY");

    for (int t = 0; t < numTimes; t++) {
      int x = gridX + dayColW + t * cellW;
      vga.fillRect(x, gridY, cellW, hdrH, DARK_BLUE);
      drawBox(x, gridY, cellW, hdrH, CYAN);
      vga.setTextColor(CYAN, DARK_BLUE);
      char tLabel[8];
      strncpy(tLabel, ttGrid.slots[t], 5);
      tLabel[5] = '\0';
      vga.setCursor(x + 2, gridY + 5);
      vga.print(tLabel);
    }

    for (int d = 0; d < numDays; d++) {
      int y = gridY + hdrH + d * rowH;

      vga.fillRect(gridX, y, dayColW, rowH, DARK_BLUE);
      drawBox(gridX, y, dayColW, rowH, CYAN);
      vga.setTextColor(YELLOW, DARK_BLUE);
      vga.setCursor(gridX + 2, y + (rowH / 2) - 4);
      vga.print(ttGrid.days[d]);

      int s = 0;
      while (s < numTimes) {
        int x = gridX + dayColW + s * cellW;
        const char* cell = ttGrid.cells[d][s];

        if (strcmp(cell, "^") == 0) { s++; continue; }

        int span = ttGrid.rowspan[d][s];
        if (span < 1) span = 1;
        if (s + span > numTimes) span = numTimes - s;
        int fillW = span * cellW;

        if (strcmp(cell, "-") == 0 || strcmp(cell, "") == 0) {
          vga.fillRect(x, y, fillW, rowH, freeColor);
          drawBox(x, y, fillW, rowH, freeBorder);
          vga.setTextColor(freeTextCol, freeColor);
          vga.setCursor(x + fillW / 2 - 3, y + rowH / 2 - 4);
          vga.print("-");

        } else {
          char codeOnly[10] = "";
          char roomOnly[10] = "";

          const char* paren = strchr(cell, '(');
          if (paren) {
            int clen = paren - cell;
            if (clen > 9) clen = 9;
            strncpy(codeOnly, cell, clen);
            codeOnly[clen] = '\0';

            const char* rEnd = strchr(paren, ')');
            int rlen = rEnd ? (rEnd - paren - 1) : (int)strlen(paren + 1);
            if (rlen > 9) rlen = 9;
            strncpy(roomOnly, paren + 1, rlen);
            roomOnly[rlen] = '\0';
          } else {
            strncpy(codeOnly, cell, 9);
            codeOnly[9] = '\0';
          }

          SubjectColor sc = getSubjectColor(codeOnly);
          vga.fillRect(x, y, fillW, rowH, sc.bg);
          drawBox(x, y, fillW, rowH, LIGHT_BLUE);

          int maxChars = (fillW - 6) / 6;
          if (maxChars < 2) maxChars = 2;

          char codeLine[12], roomLine[12];
          truncStr(codeLine, codeOnly, maxChars);
          truncStr(roomLine, roomOnly, maxChars);

          vga.setTextColor(sc.text, sc.bg);
          vga.setCursor(x + 3, y + 4);
          vga.print(codeLine);

          vga.setTextColor(vga.RGB(190, 190, 190), sc.bg);
          vga.setCursor(x + 3, y + 15);
          vga.print(roomLine);
        }

        s += span;
      }
    }
  }

  tt_footer:
  vga.fillRect(0, 285, 400, 15, DARK_BLUE);
  drawBox(10, 285, 380, 14, CYAN);
  leftText(12, 290, "[UP/DN: Day Filter]  [E: Main Menu]", LIGHT_BLUE, DARK_BLUE);
}

// ========================================
// MQTT
// ========================================
void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg = "";
  for (int i = 0; i < (int)len; i++) msg += (char)payload[i];

  JsonDocument doc;
  deserializeJson(doc, msg);

  const char* action = doc["action"] | "";
  const char* title = doc["title"] | "";
  const char* message = doc["message"] | "";
  const char* category = doc["category"] | "";

  if (strcmp(action, "deleted") == 0) {
    fetchAnnouncements();
    return;
  }

  if (strcmp(category, "urgent") == 0) { addToCategory(urgent, title, message); beep(3); }
  else if (strcmp(category, "general") == 0) { addToCategory(general, title, message); beep(1); }
  else if (strcmp(category, "event") == 0) { addToCategory(eventCat, title, message); beep(2); }
  else if (strcmp(category, "office") == 0) { addToCategory(office, title, message); beep(1); }

  updateLEDs();
  if (currentState == MENU) drawMenu();
}

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("MQTT...");
    if (client.connect("ESP32NoticeBoard")) {
      Serial.println("OK!");
      client.subscribe("department/notices");
    } else {
      delay(2000);
    }
  }
}

// ========================================
// BUTTON HANDLING
// ========================================
void handleButtons() {
  if (millis() - lastPress < DEBOUNCE) return;

  bool up = (digitalRead(upBtn) == LOW);
  bool down = (digitalRead(downBtn) == LOW);
  bool enter = (digitalRead(enterBtn) == LOW);
  bool back = (digitalRead(backBtn) == LOW);

  if (!up && !down && !enter && !back) return;
  lastPress = millis();

  if (currentState == MENU) {
    if (up) { selectedMenu--; if (selectedMenu < 0) selectedMenu = 4; drawMenu(); }
    if (down) { selectedMenu++; if (selectedMenu > 4) selectedMenu = 0; drawMenu(); }
    if (enter) {
      if (selectedMenu == 0) { currentState = URGENT_VIEW; showCategory(urgent, "URGENT NOTICE"); }
      else if (selectedMenu == 1) { currentState = GENERAL_VIEW; showCategory(general, "GENERAL NOTICE"); }
      else if (selectedMenu == 2) { currentState = EVENT_VIEW; showCategory(eventCat, "EVENT NOTICE"); }
      else if (selectedMenu == 3) { currentState = OFFICE_VIEW; showCategory(office, "OFFICE NOTICE"); }
      else if (selectedMenu == 4) {
        selYear = 0; yearScrollOff = 0;
        currentState = TT_YEAR;
        showLoading("Loading years...");
        delay(500);
        drawYearSelect();
      }
    }
  }
  else if (currentState == URGENT_VIEW) {
    if (up) { urgent.index--; if (urgent.index < 0) urgent.index = urgent.count - 1; showCategory(urgent, "URGENT NOTICE"); }
    if (down) { urgent.index++; if (urgent.index >= urgent.count) urgent.index = 0; showCategory(urgent, "URGENT NOTICE"); }
    if (enter || back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == GENERAL_VIEW) {
    if (up) { general.index--; if (general.index < 0) general.index = general.count - 1; showCategory(general, "GENERAL NOTICE"); }
    if (down) { general.index++; if (general.index >= general.count) general.index = 0; showCategory(general, "GENERAL NOTICE"); }
    if (enter || back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == EVENT_VIEW) {
    if (up) { eventCat.index--; if (eventCat.index < 0) eventCat.index = eventCat.count - 1; showCategory(eventCat, "EVENT NOTICE"); }
    if (down) { eventCat.index++; if (eventCat.index >= eventCat.count) eventCat.index = 0; showCategory(eventCat, "EVENT NOTICE"); }
    if (enter || back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == OFFICE_VIEW) {
    if (up) { office.index--; if (office.index < 0) office.index = office.count - 1; showCategory(office, "OFFICE NOTICE"); }
    if (down) { office.index++; if (office.index >= office.count) office.index = 0; showCategory(office, "OFFICE NOTICE"); }
    if (enter || back) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == TT_YEAR) {
    if (up) { if (selYear > 0) { selYear--; if (selYear < yearScrollOff) yearScrollOff = selYear; } drawYearSelect(); }
    if (down) { if (selYear < totalYears - 1) { selYear++; if (selYear >= yearScrollOff + 5) yearScrollOff = selYear - 4; } drawYearSelect(); }
    if (back) { currentState = MENU; selectedMenu = 0; drawMenu(); }
    if (enter) {
      showLoading("Loading sessions...");
      delay(500);
      fetchSessions(years[selYear].id);
      selSession = 0; sessionScrollOff = 0;
      currentState = TT_SESSION;
      drawSessionSelect();
    }
  }
  else if (currentState == TT_SESSION) {
    if (up) { if (selSession > 0) { selSession--; if (selSession < sessionScrollOff) sessionScrollOff = selSession; } drawSessionSelect(); }
    if (down) { if (selSession < totalSessions - 1) { selSession++; if (selSession >= sessionScrollOff + 5) sessionScrollOff = selSession - 4; } drawSessionSelect(); }
    if (back) { currentState = TT_YEAR; selYear = 0; yearScrollOff = 0; drawYearSelect(); }
    if (enter) {
      showLoading("Loading sections...");
      delay(500);
      fetchSections(sessions[selSession].id);
      selSection = 0; sectionScrollOff = 0;
      currentState = TT_SECTION;
      drawSectionSelect();
    }
  }
  else if (currentState == TT_SECTION) {
    if (up) { if (selSection > 0) { selSection--; if (selSection < sectionScrollOff) sectionScrollOff = selSection; } drawSectionSelect(); }
    if (down) { if (selSection < totalSections - 1) { selSection++; if (selSection >= sectionScrollOff + 5) sectionScrollOff = selSection - 4; } drawSectionSelect(); }
    if (back) { currentState = TT_SESSION; selSession = 0; sessionScrollOff = 0; drawSessionSelect(); }
    if (enter) {
      showLoading("Loading timetable...");
      delay(500);
      fetchTimetable(selYear, selSession, selSection);
      selectedDayFilter = -1;
      currentState = TT_DAY_FILTER;
      drawDayFilter();
    }
  }
  else if (currentState == TT_DAY_FILTER) {
    int selectedOption = selectedDayFilter + 1;
    if (up) { if (selectedOption > 0) { selectedOption--; selectedDayFilter = selectedOption - 1; } drawDayFilter(); }
    if (down) { if (selectedOption < 5) { selectedOption++; selectedDayFilter = selectedOption - 1; } drawDayFilter(); }
    if (back) { currentState = TT_SECTION; selSection = 0; sectionScrollOff = 0; drawSectionSelect(); }
    if (enter) { ttStartIdx = 0; currentState = TT_DISPLAY; drawTimetable(); }
  }
  else if (currentState == TT_DISPLAY) {
    if (up || down) { currentState = TT_DAY_FILTER; drawDayFilter(); }
    if (enter || back) { currentState = MENU; selectedMenu = 0; drawMenu(); }
  }
}

// ========================================
// SETUP
// ========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== NOTICE BOARD STARTING ===\n");

  pinMode(upBtn, INPUT_PULLUP);
  pinMode(downBtn, INPUT_PULLUP);
  pinMode(enterBtn, INPUT_PULLUP);
  pinMode(backBtn, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);
  pinMode(redLED, OUTPUT);
  pinMode(greenLED, OUTPUT);
  pinMode(yellowLED, OUTPUT);

  digitalWrite(buzzer, LOW);
  digitalWrite(redLED, LOW);
  digitalWrite(greenLED, LOW);
  digitalWrite(yellowLED, LOW);

  currentTT.count = 0;
  currentTT.yearIdx = -1;
  currentTT.sessionIdx = -1;
  currentTT.sectionIdx = -1;

  vga.init(vga.MODE400x300, redPin, greenPin, bluePin, hsyncPin, vsyncPin);
  vga.setFont(Font6x8);

  BLACK = vga.RGB(0, 0, 0);
  WHITE = vga.RGB(255, 255, 255);
  RED = vga.RGB(255, 0, 0);
  GREEN = vga.RGB(0, 255, 0);
  BLUE = vga.RGB(0, 0, 255);
  YELLOW = vga.RGB(255, 255, 0);
  CYAN = vga.RGB(0, 255, 255);
  GRAY = vga.RGB(80, 80, 80);
  LIGHT_BLUE = vga.RGB(100, 100, 255);
  DARK_BLUE = vga.RGB(0, 0, 100);

  vga.clear(BLACK);
  centerText(140, "Connecting to WiFi...", CYAN, BLACK);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    attempts++;
  }
  Serial.println("✅ WiFi OK!");

  vga.clear(BLACK);
  centerText(140, "Connecting to MQTT...", CYAN, BLACK);

  client.setServer(mqttServer, mqttPort);
  client.setBufferSize(512);
  client.setCallback(onMessage);
  connectMQTT();

  vga.clear(BLACK);
  centerText(140, "Loading announcements...", CYAN, BLACK);
  fetchAnnouncements();

  vga.clear(BLACK);
  centerText(140, "Loading years...", CYAN, BLACK);
  fetchYears();

  drawMenu();
  Serial.println("✅ Ready!\n");
}

// ========================================
// LOOP
// ========================================
void loop() {
  if (!client.connected()) connectMQTT();
  client.loop();
  handleButtons();

  if (millis() - lastAnnFetch > ANN_FETCH_INTERVAL) {
    lastAnnFetch = millis();
    if (currentState == MENU) {
      Serial.println("Refreshing announcements...");
      fetchAnnouncements();
    }
  }

  delay(10);
}