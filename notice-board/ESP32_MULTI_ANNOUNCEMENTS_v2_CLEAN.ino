#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Lib.h>
#include <Ressources/Font6x8.h>
#include <HTTPClient.h>

// ================= WIFI =================
const char* ssid     = "Fast net fiber n";
const char* password = "03004721644aa";
const char* serverIP = "192.168.0.109";

// ================= MQTT =================
const char* mqttServer = "broker.emqx.io";
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

// ================= SESSIONS/SECTIONS =================
const int MAX_SESSIONS = 10;
const int MAX_SECTIONS = 10;

struct SessionInfo {
  int  id;
  char name[40];
};

struct SectionInfo {
  int  id;
  char name[10];
};

SessionInfo sessions[MAX_SESSIONS];
SectionInfo sections[MAX_SECTIONS];
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
  int     sessionIdx;
  int     sectionIdx;
};

TimetableData currentTT = {{}, 0, -1, -1};

// ================= TIMETABLE GRID =================
struct TTGrid {
  char days[5][10];
  char slots[8][20];
  char cells[5][8][25];
  int slotCount;
  int dayCount;
};

TTGrid ttGrid = {{}, {}, {}, 0, 0};

// ================= TEACHER OFFICE LOCATIONS =================
struct TeacherInfo {
  int id;
  char name[50];
  int roomNo;
  int cabinNo;
  int floor;
};

const int MAX_TEACHERS = 50;
TeacherInfo allTeachers[MAX_TEACHERS];
int totalTeachers = 0;

enum OfficeUIState {
  OFFICE_SELECT_FLOOR,
  OFFICE_SELECT_TEACHER,
  OFFICE_SHOW_MAP
};

OfficeUIState officeUIState = OFFICE_SELECT_FLOOR;
int selectedFloor = 0;
int selectedTeacherInList = 0;
int teacherListScrollOffset = 0;

// ================= UI STATE =================
enum UIState {
  MENU,
  URGENT_VIEW, GENERAL_VIEW, EVENT_VIEW, OFFICE_VIEW,
  TT_SESSION, TT_SECTION, TT_DISPLAY
};

UIState currentState       = MENU;
int     selectedMenu       = 0;
int     selSession         = 0;
int     selSection         = 0;
int     sessionScrollOff   = 0;
int     sectionScrollOff   = 0;

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
  vga.fillRect(x,         y,         w, 2,  color);
  vga.fillRect(x,         y + h - 2, w, 2,  color);
  vga.fillRect(x,         y,         2, h,  color);
  vga.fillRect(x + w - 2, y,         2, h,  color);
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
  for (int i = 0; i < urgent.count;   i++) if (urgent.items[i].isNew)   u = true;
  for (int i = 0; i < general.count;  i++) if (general.items[i].isNew)  g = true;
  for (int i = 0; i < eventCat.count; i++) if (eventCat.items[i].isNew) e = true;
  digitalWrite(redLED,    u);
  digitalWrite(greenLED,  g);
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
  strncpy(cat.items[cat.count].title,   title, 59);
  strncpy(cat.items[cat.count].message, msg,   199);
  cat.items[cat.count].title[59]   = '\0';
  cat.items[cat.count].message[199] = '\0';
  cat.items[cat.count].isNew = true;
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
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/announcements/approved/all";
  
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      http.end();
      return;
    }
    
    clearAllCategories();
    
    for (JsonObject ann : doc.as<JsonArray>()) {
      const char* title    = ann["title"]    | "";
      const char* message  = ann["message"]  | "";
      const char* category = ann["category"] | "";

      if      (strcmp(category, "urgent")  == 0) { addToCategory(urgent,   title, message); }
      else if (strcmp(category, "general") == 0) { addToCategory(general,  title, message); }
      else if (strcmp(category, "event")   == 0) { addToCategory(eventCat, title, message); }
      else if (strcmp(category, "office")  == 0) { addToCategory(office,   title, message); }
    }
    
    Serial.printf("✅ Loaded announcements: U=%d G=%d E=%d O=%d\n", 
      urgent.count, general.count, eventCat.count, office.count);
    
    updateLEDs();
    if (currentState == MENU) drawMenu();
  }
  http.end();
}

void fetchSessions() {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/sessions";
  
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
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
    DynamicJsonDocument doc(1024);
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

void fetchTimetable(int sIdx, int secIdx) {
  int sessionId = sessions[sIdx].id;
  int sectionId = sections[secIdx].id;

  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/timetable/" + sessionId + "/" + sectionId;
  
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, payload);

    if (doc["success"] == true) {
      currentTT.count      = 0;
      currentTT.sessionIdx = sIdx;
      currentTT.sectionIdx = secIdx;

      for (JsonObject e : doc["data"].as<JsonArray>()) {
        if (currentTT.count >= MAX_TT_ENTRIES) break;
        int i = currentTT.count;

        String t = String(e["start_time"] | "") + "-" + String(e["end_time"] | "");
        strncpy(currentTT.entries[i].time,    t.c_str(), 19);
        strncpy(currentTT.entries[i].subject, e["subject_name"] | "", 29);
        strncpy(currentTT.entries[i].code,    e["subject_code"] | "", 9);
        strncpy(currentTT.entries[i].teacher, e["teacher_name"] | "", 29);
        strncpy(currentTT.entries[i].room,    e["room_number"]  | "", 9);
        strncpy(currentTT.entries[i].day,     e["day_name"]     | "", 3);

        currentTT.entries[i].time[19]    = '\0';
        currentTT.entries[i].subject[29] = '\0';
        currentTT.entries[i].code[9]     = '\0';
        currentTT.entries[i].teacher[29] = '\0';
        currentTT.entries[i].room[9]     = '\0';
        currentTT.entries[i].day[3]      = '\0';

        currentTT.count++;
      }
      Serial.printf("✅ Loaded %d timetable entries\n", currentTT.count);
    }
  }
  http.end();
}

void fetchAllTeachers() {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/teachers/offices";
  
  if (!http.begin(espClient, url)) return;

  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, payload);
    
    totalTeachers = 0;
    for (JsonObject teacher : doc.as<JsonArray>()) {
      if (totalTeachers >= MAX_TEACHERS) break;
      
      allTeachers[totalTeachers].id = teacher["id"] | 0;
      strncpy(allTeachers[totalTeachers].name, teacher["name"] | "", 49);
      allTeachers[totalTeachers].roomNo = teacher["room_no"] | 0;
      allTeachers[totalTeachers].cabinNo = teacher["cabin_no"] | 0;
      allTeachers[totalTeachers].floor = teacher["floor"] | 0;
      allTeachers[totalTeachers].name[49] = '\0';
      totalTeachers++;
    }
    Serial.printf("✅ Loaded %d teachers\n", totalTeachers);
  }
  http.end();
}

// ========================================
// TIMETABLE GRID
// ========================================
void buildTimetableGrid() {
  memset(&ttGrid, 0, sizeof(ttGrid));
  ttGrid.slotCount = 0;
  ttGrid.dayCount = 0;

  for (int i = 0; i < currentTT.count; i++) {
    bool found = false;
    for (int d = 0; d < ttGrid.dayCount; d++) {
      if (strcmp(ttGrid.days[d], currentTT.entries[i].day) == 0) {
        found = true;
        break;
      }
    }
    if (!found && ttGrid.dayCount < 5) {
      strcpy(ttGrid.days[ttGrid.dayCount], currentTT.entries[i].day);
      ttGrid.dayCount++;
    }
  }

  for (int i = 0; i < currentTT.count; i++) {
    bool found = false;
    for (int s = 0; s < ttGrid.slotCount; s++) {
      if (strcmp(ttGrid.slots[s], currentTT.entries[i].time) == 0) {
        found = true;
        break;
      }
    }
    if (!found && ttGrid.slotCount < 8) {
      strcpy(ttGrid.slots[ttGrid.slotCount], currentTT.entries[i].time);
      ttGrid.slotCount++;
    }
  }

  for (int d = 0; d < ttGrid.dayCount; d++) {
    for (int s = 0; s < ttGrid.slotCount; s++) {
      strcpy(ttGrid.cells[d][s], "-");
      for (int i = 0; i < currentTT.count; i++) {
        if (strcmp(currentTT.entries[i].day, ttGrid.days[d]) == 0 &&
            strcmp(currentTT.entries[i].time, ttGrid.slots[s]) == 0) {
          sprintf(ttGrid.cells[d][s], "%.5s(%.2s)",
                  currentTT.entries[i].code,
                  currentTT.entries[i].room);
          break;
        }
      }
    }
  }
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
    "TEACHER OFFICES",
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
  if (cat.index < 0)          cat.index = cat.count - 1;

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
  int lineY = 120;
  int pos   = 0;
  int len   = strlen(msg);
  while (pos < len && lineY < 255) {
    char line[53] = {0};
    strncpy(line, msg + pos, 52);
    vga.setCursor(28, lineY);
    vga.print(line);
    pos   += 52;
    lineY += 16;
  }

  char nav[40];
  sprintf(nav, "Notice %d of %d", cat.index + 1, cat.count);
  centerText(268, nav, CYAN, BLACK);

  cat.items[cat.index].isNew = false;
  updateLEDs();
}

void drawFloorSelection() {
  drawBase("U/D:Select  E:Enter");
  centerText(58, "SELECT FLOOR", YELLOW, BLACK);
  
  const char* floors[2] = {"GROUND FLOOR", "FIRST FLOOR"};
  
  for (int i = 0; i < 2; i++) {
    int y = 110 + i * 65;
    bool sel = (i == selectedFloor);
    
    vga.fillRect(50, y, 300, 55, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 55, sel ? YELLOW : LIGHT_BLUE);
    centerText(y + 22, floors[i], sel ? YELLOW : WHITE, sel ? DARK_BLUE : GRAY);
    
    if (sel) {
      vga.setTextColor(CYAN, DARK_BLUE);
      vga.setCursor(360, y + 22);
      vga.print(">");
    }
  }
}

void drawTeacherListForFloor() {
  drawBase("U/D:Select  E:Map  B:Back");
  
  char title[60];
  sprintf(title, "%s FLOOR TEACHERS", selectedFloor == 0 ? "GROUND" : "FIRST");
  centerText(58, title, YELLOW, BLACK);
  
  int floorTeachers[MAX_TEACHERS];
  int floorTeacherCount = 0;
  for (int i = 0; i < totalTeachers; i++) {
    if (allTeachers[i].floor == selectedFloor) {
      floorTeachers[floorTeacherCount++] = i;
    }
  }
  
  if (floorTeacherCount == 0) {
    centerText(150, "NO TEACHERS FOUND", CYAN, BLACK);
    return;
  }
  
  const int VISIBLE = 4;
  int end = (teacherListScrollOffset + VISIBLE < floorTeacherCount) ? 
            (teacherListScrollOffset + VISIBLE) : floorTeacherCount;
  
  for (int i = teacherListScrollOffset; i < end; i++) {
    int slot = i - teacherListScrollOffset;
    int y = 90 + slot * 45;
    bool sel = (i == selectedTeacherInList);
    
    vga.fillRect(30, y, 340, 40, sel ? DARK_BLUE : GRAY);
    drawBox(30, y, 340, 40, sel ? YELLOW : LIGHT_BLUE);
    
    int teacherIndex = floorTeachers[i];
    
    vga.setTextColor(sel ? YELLOW : WHITE, sel ? DARK_BLUE : GRAY);
    vga.setCursor(40, y + 10);
    vga.print(allTeachers[teacherIndex].name);
    
    char info[40];
    sprintf(info, "Room: %d | Cabin: %d", 
            allTeachers[teacherIndex].roomNo, 
            allTeachers[teacherIndex].cabinNo);
    vga.setCursor(40, y + 25);
    vga.print(info);
    
    if (sel) {
      vga.setTextColor(CYAN, DARK_BLUE);
      vga.setCursor(360, y + 16);
      vga.print(">");
    }
  }
  
  if (teacherListScrollOffset > 0) {
    vga.setTextColor(CYAN, BLACK);
    vga.setCursor(190, 82);
    vga.print("^");
  }
  if (teacherListScrollOffset + VISIBLE < floorTeacherCount) {
    vga.setTextColor(CYAN, BLACK);
    vga.setCursor(190, 275);
    vga.print("v");
  }
}

void drawGroundFloorOfficeMap(int cabin) {
  vga.clear(BLACK);
  drawHeader();
  centerText(58, "GROUND FLOOR MAP", YELLOW, BLACK);
  
  int y = 85;
  vga.setTextColor(CYAN, BLACK);
  
  // Row 1
  leftText(30, y, "=== CABIN 3 ===        === CABIN 4 ===", CYAN, BLACK);
  
  // Row 1 highlight
  if (cabin == 3 || cabin == 4) {
    vga.setTextColor(cabin == 3 ? YELLOW : WHITE, BLACK);
    if (cabin == 3) leftText(30, y + 16, "   SELECTED", YELLOW, BLACK);
    else leftText(30, y + 16, "             ", WHITE, BLACK);
    
    vga.setTextColor(cabin == 4 ? YELLOW : WHITE, BLACK);
    if (cabin == 4) leftText(270, y + 16, "SELECTED", YELLOW, BLACK);
    else leftText(270, y + 16, "        ", WHITE, BLACK);
  }
  
  vga.setTextColor(LIGHT_BLUE, BLACK);
  leftText(30, y + 35, "===== CORRIDOR =====", LIGHT_BLUE, BLACK);
  
  // Row 2
  vga.setTextColor(CYAN, BLACK);
  leftText(30, y + 55, "=== CABIN 2 ===        === CABIN 5 ===", CYAN, BLACK);
  
  if (cabin == 2 || cabin == 5) {
    vga.setTextColor(cabin == 2 ? YELLOW : WHITE, BLACK);
    if (cabin == 2) leftText(30, y + 71, "   SELECTED", YELLOW, BLACK);
    else leftText(30, y + 71, "             ", WHITE, BLACK);
    
    vga.setTextColor(cabin == 5 ? YELLOW : WHITE, BLACK);
    if (cabin == 5) leftText(270, y + 71, "SELECTED", YELLOW, BLACK);
    else leftText(270, y + 71, "        ", WHITE, BLACK);
  }
  
  vga.setTextColor(LIGHT_BLUE, BLACK);
  leftText(30, y + 90, "===== CORRIDOR =====", LIGHT_BLUE, BLACK);
  
  // Row 3
  vga.setTextColor(CYAN, BLACK);
  leftText(30, y + 110, "=== CABIN 1 ===        === CABIN 6 ===", CYAN, BLACK);
  
  if (cabin == 1 || cabin == 6) {
    vga.setTextColor(cabin == 1 ? YELLOW : WHITE, BLACK);
    if (cabin == 1) leftText(30, y + 126, "   SELECTED", YELLOW, BLACK);
    else leftText(30, y + 126, "             ", WHITE, BLACK);
    
    vga.setTextColor(cabin == 6 ? YELLOW : WHITE, BLACK);
    if (cabin == 6) leftText(270, y + 126, "SELECTED", YELLOW, BLACK);
    else leftText(270, y + 126, "        ", WHITE, BLACK);
  }
  
  // Legend
  vga.setTextColor(YELLOW, BLACK);
  leftText(30, y + 160, "YELLOW = Your Location", YELLOW, BLACK);
  vga.setTextColor(LIGHT_BLUE, BLACK);
  leftText(30, y + 175, "ENTER to go back", LIGHT_BLUE, BLACK);
  
  drawFooter("Ground Floor Office Map");
}

void showTeacherOfficeMap() {
  int floorTeachers[MAX_TEACHERS];
  int floorTeacherCount = 0;
  for (int i = 0; i < totalTeachers; i++) {
    if (allTeachers[i].floor == selectedFloor) {
      floorTeachers[floorTeacherCount++] = i;
    }
  }
  
  if (selectedTeacherInList < floorTeacherCount) {
    int teacherIdx = floorTeachers[selectedTeacherInList];
    int cabinToHighlight = allTeachers[teacherIdx].cabinNo;
    
    if (selectedFloor == 0) {
      drawGroundFloorOfficeMap(cabinToHighlight);
    } else {
      drawBase("Coming Soon");
      centerText(150, "FIRST FLOOR MAP", YELLOW, BLACK);
      centerText(170, "Coming Soon!", CYAN, BLACK);
    }
  }
}

void drawSessionSelect() {
  drawBase("U/D:Navigate  E:Select");
  centerText(58, "SELECT SESSION", YELLOW, BLACK);

  if (totalSessions == 0) {
    centerText(150, "No sessions found!", CYAN, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = (sessionScrollOff + VISIBLE < totalSessions) ? 
            (sessionScrollOff + VISIBLE) : totalSessions;

  for (int i = sessionScrollOff; i < end; i++) {
    int slot = i - sessionScrollOff;
    int y    = 85 + slot * 35;
    bool sel = (i == selSession);

    vga.fillRect(50, y, 300, 30, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 30, sel ? YELLOW : LIGHT_BLUE);
    centerText(y + 17, sessions[i].name, sel ? YELLOW : WHITE, sel ? DARK_BLUE : GRAY);
  }
}

void drawSectionSelect() {
  drawBase("U/D:Navigate  E:Select");

  char title[50];
  sprintf(title, "%s - SECTION", sessions[selSession].name);
  centerText(58, title, YELLOW, BLACK);

  if (totalSections == 0) {
    centerText(150, "No sections found!", CYAN, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = (sectionScrollOff + VISIBLE < totalSections) ? 
            (sectionScrollOff + VISIBLE) : totalSections;

  for (int i = sectionScrollOff; i < end; i++) {
    int slot = i - sectionScrollOff;
    int y    = 85 + slot * 35;
    bool sel = (i == selSection);

    vga.fillRect(100, y, 200, 30, sel ? DARK_BLUE : GRAY);
    drawBox(100, y, 200, 30, sel ? GREEN : LIGHT_BLUE);
    centerText(y + 17, sections[i].name, sel ? GREEN : WHITE, sel ? DARK_BLUE : GRAY);
  }
}

void drawTimetable() {
  drawBase("E:Back to Menu");

  char header[80];
  sprintf(header, "%s - Section %s",
          sessions[selSession].name,
          sections[selSection].name);
  centerText(58, header, YELLOW, BLACK);

  if (currentTT.count == 0) {
    centerText(150, "NO TIMETABLE DATA", CYAN, BLACK);
    return;
  }

  buildTimetableGrid();

  int colW = 60;
  int rowH = 21;

  vga.fillRect(10, 73, 50, rowH, DARK_BLUE);
  drawBox(10, 73, 50, rowH, CYAN);
  leftText(18, 79, "DAY", YELLOW, DARK_BLUE);

  for (int s = 0; s < ttGrid.slotCount && s < 5; s++) {
    int x = 62 + s * colW;
    vga.fillRect(x, 73, colW, rowH, DARK_BLUE);
    drawBox(x, 73, colW, rowH, CYAN);

    char abbrev[10] = {0};
    const char* slot = ttGrid.slots[s];
    if (strlen(slot) >= 11) {
      sprintf(abbrev, "%.2s-%.2s", slot, slot + 6);
    } else {
      strncpy(abbrev, slot, 9);
    }
    leftText(x + 15, 79, abbrev, YELLOW, DARK_BLUE);
  }

  for (int d = 0; d < ttGrid.dayCount && d < 4; d++) {
    int y = 73 + (d + 1) * rowH;
    int bg = (d % 2 == 0) ? GRAY : vga.RGB(50, 50, 60);

    vga.fillRect(10, y, 50, rowH, bg);
    drawBox(10, y, 50, rowH, LIGHT_BLUE);
    leftText(16, y + 6, ttGrid.days[d], CYAN, bg);

    for (int s = 0; s < ttGrid.slotCount && s < 5; s++) {
      int x = 62 + s * colW;
      vga.fillRect(x, y, colW, rowH, bg);
      drawBox(x, y, colW, rowH, LIGHT_BLUE);

      const char* cell = ttGrid.cells[d][s];
      if (strcmp(cell, "-") != 0) {
        leftText(x + 12, y + 6, cell, WHITE, bg);
      }
    }
  }
}

// ========================================
// MQTT
// ========================================
void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg = "";
  for (int i = 0; i < (int)len; i++) msg += (char)payload[i];

  DynamicJsonDocument doc(512);
  deserializeJson(doc, msg);

  const char* action   = doc["action"]   | "";
  const char* title    = doc["title"]    | "";
  const char* message  = doc["message"]  | "";
  const char* category = doc["category"] | "";

  if (strcmp(action, "deleted") == 0) {
    fetchAnnouncements();
    return;
  }

  if      (strcmp(category, "urgent")  == 0) { addToCategory(urgent,   title, message); beep(3); }
  else if (strcmp(category, "general") == 0) { addToCategory(general,  title, message); beep(1); }
  else if (strcmp(category, "event")   == 0) { addToCategory(eventCat, title, message); beep(2); }
  else if (strcmp(category, "office")  == 0) { addToCategory(office,   title, message); beep(1); }

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

  bool up    = (digitalRead(upBtn)    == LOW);
  bool down  = (digitalRead(downBtn)  == LOW);
  bool enter = (digitalRead(enterBtn) == LOW);

  if (!up && !down && !enter) return;
  lastPress = millis();

  if (currentState == MENU) {
    if (up)    { selectedMenu--; if (selectedMenu < 0)  selectedMenu = 4; drawMenu(); }
    if (down)  { selectedMenu++; if (selectedMenu > 4)  selectedMenu = 0; drawMenu(); }
    if (enter) {
      if      (selectedMenu == 0) { currentState = URGENT_VIEW;  showCategory(urgent,   "URGENT NOTICE");  }
      else if (selectedMenu == 1) { currentState = GENERAL_VIEW; showCategory(general,  "GENERAL NOTICE"); }
      else if (selectedMenu == 2) { currentState = EVENT_VIEW;   showCategory(eventCat, "EVENT NOTICE");   }
      else if (selectedMenu == 3) { 
        currentState = OFFICE_VIEW;
        officeUIState = OFFICE_SELECT_FLOOR;
        selectedFloor = 0;
        selectedTeacherInList = 0;
        teacherListScrollOffset = 0;
        drawFloorSelection();
      }
      else if (selectedMenu == 4) {
        selSession = 0; sessionScrollOff = 0;
        currentState = TT_SESSION;
        showLoading("Loading sessions...");
        delay(500);
        drawSessionSelect();
      }
    }
  }
  else if (currentState == URGENT_VIEW) {
    if (up)    { urgent.index--;  if (urgent.index < 0)            urgent.index = urgent.count - 1;   showCategory(urgent,   "URGENT NOTICE");  }
    if (down)  { urgent.index++;  if (urgent.index >= urgent.count) urgent.index = 0;                  showCategory(urgent,   "URGENT NOTICE");  }
    if (enter) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == GENERAL_VIEW) {
    if (up)    { general.index--; if (general.index < 0)             general.index = general.count - 1; showCategory(general,  "GENERAL NOTICE"); }
    if (down)  { general.index++; if (general.index >= general.count) general.index = 0;                showCategory(general,  "GENERAL NOTICE"); }
    if (enter) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == EVENT_VIEW) {
    if (up)    { eventCat.index--; if (eventCat.index < 0)               eventCat.index = eventCat.count - 1; showCategory(eventCat, "EVENT NOTICE");   }
    if (down)  { eventCat.index++; if (eventCat.index >= eventCat.count)  eventCat.index = 0;                 showCategory(eventCat, "EVENT NOTICE");   }
    if (enter) { currentState = MENU; drawMenu(); }
  }
  else if (currentState == OFFICE_VIEW) {
    if (officeUIState == OFFICE_SELECT_FLOOR) {
      if (up)    { if (selectedFloor > 0) { selectedFloor--; drawFloorSelection(); } }
      if (down)  { if (selectedFloor < 1) { selectedFloor++; drawFloorSelection(); } }
      if (enter) { officeUIState = OFFICE_SELECT_TEACHER; selectedTeacherInList = 0; teacherListScrollOffset = 0; drawTeacherListForFloor(); }
    }
    else if (officeUIState == OFFICE_SELECT_TEACHER) {
      int floorTeacherCount = 0;
      for (int i = 0; i < totalTeachers; i++) {
        if (allTeachers[i].floor == selectedFloor) floorTeacherCount++;
      }
      
      if (up) {
        if (selectedTeacherInList > 0) {
          selectedTeacherInList--;
          if (selectedTeacherInList < teacherListScrollOffset) teacherListScrollOffset = selectedTeacherInList;
          drawTeacherListForFloor();
        }
      }
      if (down) {
        if (selectedTeacherInList < floorTeacherCount - 1) {
          selectedTeacherInList++;
          if (selectedTeacherInList >= teacherListScrollOffset + 4) teacherListScrollOffset = selectedTeacherInList - 3;
          drawTeacherListForFloor();
        }
      }
      if (enter) { officeUIState = OFFICE_SHOW_MAP; showTeacherOfficeMap(); }
    }
    else if (officeUIState == OFFICE_SHOW_MAP) {
      if (enter) { officeUIState = OFFICE_SELECT_TEACHER; drawTeacherListForFloor(); }
    }
  }
  else if (currentState == TT_SESSION) {
    if (up) {
      if (selSession > 0) {
        selSession--;
        if (selSession < sessionScrollOff) sessionScrollOff = selSession;
      }
      drawSessionSelect();
    }
    if (down) {
      if (selSession < totalSessions - 1) {
        selSession++;
        if (selSession >= sessionScrollOff + 5) sessionScrollOff = selSession - 4;
      }
      drawSessionSelect();
    }
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
    if (up) {
      if (selSection > 0) {
        selSection--;
        if (selSection < sectionScrollOff) sectionScrollOff = selSection;
      }
      drawSectionSelect();
    }
    if (down) {
      if (selSection < totalSections - 1) {
        selSection++;
        if (selSection >= sectionScrollOff + 5) sectionScrollOff = selSection - 4;
      }
      drawSectionSelect();
    }
    if (enter) {
      showLoading("Loading timetable...");
      delay(500);
      fetchTimetable(selSession, selSection);
      currentState = TT_DISPLAY;
      drawTimetable();
    }
  }
  else if (currentState == TT_DISPLAY) {
    if (enter) { currentState = MENU; selectedMenu = 0; drawMenu(); }
  }
}

// ========================================
// SETUP
// ========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== NOTICE BOARD SYSTEM STARTING ===\n");

  pinMode(upBtn,    INPUT_PULLUP);
  pinMode(downBtn,  INPUT_PULLUP);
  pinMode(enterBtn, INPUT_PULLUP);
  pinMode(buzzer,    OUTPUT);
  pinMode(redLED,    OUTPUT);
  pinMode(greenLED,  OUTPUT);
  pinMode(yellowLED, OUTPUT);

  digitalWrite(buzzer,    LOW);
  digitalWrite(redLED,    LOW);
  digitalWrite(greenLED,  LOW);
  digitalWrite(yellowLED, LOW);

  currentTT.count      = 0;
  currentTT.sessionIdx = -1;
  currentTT.sectionIdx = -1;

  vga.init(vga.MODE400x300, redPin, greenPin, bluePin, hsyncPin, vsyncPin);
  vga.setFont(Font6x8);

  BLACK      = vga.RGB(0,   0,   0);
  WHITE      = vga.RGB(255, 255, 255);
  RED        = vga.RGB(255, 0,   0);
  GREEN      = vga.RGB(0,   255, 0);
  BLUE       = vga.RGB(0,   0,   255);
  YELLOW     = vga.RGB(255, 255, 0);
  CYAN       = vga.RGB(0,   255, 255);
  GRAY       = vga.RGB(80,  80,  80);
  LIGHT_BLUE = vga.RGB(100, 100, 255);
  DARK_BLUE  = vga.RGB(0,   0,   100);

  vga.clear(BLACK);
  centerText(140, "Connecting to WiFi...", CYAN, BLACK);

  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi connected!");
  } else {
    Serial.println("❌ WiFi failed!");
    centerText(150, "WiFi Connection Failed!", RED, BLACK);
    delay(2000);
  }

  vga.clear(BLACK);
  centerText(140, "Connecting to MQTT...", CYAN, BLACK);

  client.setServer(mqttServer, mqttPort);
  client.setBufferSize(512);
  client.setCallback(onMessage);
  connectMQTT();

  vga.clear(BLACK);
  centerText(140, "Loading data...", CYAN, BLACK);
  delay(500);

  fetchAnnouncements();
  delay(500);
  fetchSessions();
  delay(500);
  fetchAllTeachers();
  delay(1000);

  Serial.println("\n=== SYSTEM READY ===\n");
  drawMenu();
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
      fetchAnnouncements();
    }
  }
  
  delay(10);
}
