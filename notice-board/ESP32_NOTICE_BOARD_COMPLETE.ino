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
const char* mqttServer = "192.168.0.109";  // Your PC's local IP (Mosquitto)
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
const int backBtn  = 4;

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
  int id;  // Track announcement ID to avoid re-marking as new
};

const int MAX_PER_CAT = 5;
const int MAX_SEEN_ANNOUNCEMENTS = 50;  // Track seen announcement IDs

struct Category {
  Announcement items[MAX_PER_CAT];
  int count;
  int index;
};

Category urgent  = {{}, 0, 0};
Category general = {{}, 0, 0};
Category eventCat= {{}, 0, 0};
Category office  = {{}, 0, 0};

// Track which announcements we've already seen
int seenAnnouncements[MAX_SEEN_ANNOUNCEMENTS] = {0};
int seenCount = 0;

bool hasSeenAnnouncement(int annId) {
  for (int i = 0; i < seenCount; i++) {
    if (seenAnnouncements[i] == annId) return true;
  }
  return false;
}

void markAnnouncementAsSeen(int annId) {
  if (!hasSeenAnnouncement(annId) && seenCount < MAX_SEEN_ANNOUNCEMENTS) {
    seenAnnouncements[seenCount++] = annId;
  }
}

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

YearInfo years[MAX_YEARS];
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
  char    semester[20];     // e.g., "4th Semester"
  char    session_name[40]; // e.g., "Fall 2024"
  char    term[20];         // e.g., "Spring"
};

TimetableData currentTT = {{}, 0, -1, -1, -1, "", "", ""};

// ================= TIMETABLE FILTERS =================
int selectedDayFilter = -1;  // -1 = All days, 0-4 = specific day (MON-FRI)
const char* dayNames[5] = { "MON", "TUE", "WED", "THU", "FRI" };

// ================= TIMETABLE GRID =================
struct TTGrid {
  char days[5][10];          // Day names: MON, TUE, WED, etc.
  char slots[10][20];        // Time slots: "8:00", "9:00", "10:00", etc.
  char cells[5][10][15];     // Grid cells [day][hour] - subject code + room
  int rowspan[5][10];        // How many hours this cell spans (1, 2, 3, ...)
  int slotCount;             // Number of hour slots
  int dayCount;              // Number of days with classes
};

TTGrid ttGrid = {{}, {}, {}, {}, 0, 0};

// Helper: Convert time string "HH:MM" to hour integer (e.g., "09:30" -> 9)
int timeToHour(const char* timeStr) {
  int hour = 0;
  sscanf(timeStr, "%d:", &hour);
  return hour;
}

// Helper: Calculate duration in hours between two times
int getHoursDuration(const char* startTime, const char* endTime) {
  int startHour = timeToHour(startTime);
  int endHour = timeToHour(endTime);
  int duration = endHour - startHour;
  if (duration <= 0) duration = 1;  // Minimum 1 hour
  if (duration > 5) duration = 1;   // Cap at reasonable max
  return duration;
}

void buildTimetableGrid() {
  memset(&ttGrid, 0, sizeof(ttGrid));
  ttGrid.slotCount = 0;
  ttGrid.dayCount = 0;

  // Step 1: Collect unique days (filtered if selected)
  for (int i = 0; i < currentTT.count; i++) {
    bool dayMatches = (selectedDayFilter == -1);
    if (selectedDayFilter >= 0) {
      dayMatches = (strcmp(currentTT.entries[i].day, dayNames[selectedDayFilter]) == 0);
    }
    if (!dayMatches) continue;

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

  // Step 2: Generate hourly time slots from 8:00 to 16:00
  ttGrid.slotCount = 0;
  for (int h = 8; h <= 16 && ttGrid.slotCount < 10; h++) {
    sprintf(ttGrid.slots[ttGrid.slotCount], "%d:00", h);
    ttGrid.slotCount++;
  }

  // Step 3: Initialize grid - all free slots
  for (int d = 0; d < ttGrid.dayCount; d++) {
    for (int s = 0; s < ttGrid.slotCount; s++) {
      strcpy(ttGrid.cells[d][s], "-");
      ttGrid.rowspan[d][s] = 0;  // Not yet filled
    }
  }

  // Step 4: Fill grid with courses - handling multi-hour spans
  for (int i = 0; i < currentTT.count; i++) {
    bool dayMatches = (selectedDayFilter == -1);
    if (selectedDayFilter >= 0) {
      dayMatches = (strcmp(currentTT.entries[i].day, dayNames[selectedDayFilter]) == 0);
    }
    if (!dayMatches) continue;

    // Find day index
    int dayIdx = -1;
    for (int d = 0; d < ttGrid.dayCount; d++) {
      if (strcmp(ttGrid.days[d], currentTT.entries[i].day) == 0) {
        dayIdx = d;
        break;
      }
    }
    if (dayIdx < 0) continue;

    // Find start hour index
    int startHour = timeToHour(currentTT.entries[i].time);
    int startSlot = -1;
    for (int s = 0; s < ttGrid.slotCount; s++) {
      if (timeToHour(ttGrid.slots[s]) == startHour) {
        startSlot = s;
        break;
      }
    }
    if (startSlot < 0) continue;

    // Calculate duration in hours
    int duration = getHoursDuration(currentTT.entries[i].time, currentTT.entries[i].time + 5);
    // Extract end time from entries (format: "start-end")
    char endTimeStr[10] = "";
    const char* dashPos = strchr(currentTT.entries[i].time, '-');
    if (dashPos) {
      strncpy(endTimeStr, dashPos + 1, 5);
      duration = getHoursDuration(currentTT.entries[i].time, endTimeStr);
    }
    if (duration < 1) duration = 1;
    if (duration > 5) duration = 1;

    // Fill cells for this course (spanning multiple hours if needed)
    if (startSlot + duration <= ttGrid.slotCount) {
      // First cell shows subject code + room only (no teacher)
      sprintf(ttGrid.cells[dayIdx][startSlot], "%.4s(%s)", 
              currentTT.entries[i].code,
              currentTT.entries[i].room);
      ttGrid.rowspan[dayIdx][startSlot] = duration;

      // Mark subsequent cells as "continuation" (rowspan handled in drawing)
      for (int j = 1; j < duration; j++) {
        if (startSlot + j < ttGrid.slotCount) {
          strcpy(ttGrid.cells[dayIdx][startSlot + j], "^");  // Continuation marker
          ttGrid.rowspan[dayIdx][startSlot + j] = -1;  // Mark as continuation
        }
      }
    }
  }
}

// ================= UI STATE =================
enum UIState {
  MENU,
  URGENT_VIEW, GENERAL_VIEW, EVENT_VIEW, OFFICE_VIEW,
  TT_YEAR, TT_SESSION, TT_SECTION, TT_FILTER_MODE, TT_DAY_FILTER, TT_DISPLAY
};

UIState currentState       = MENU;
int     selectedMenu       = 0;
int     selYear            = 0;
int     selSession         = 0;
int     selSection         = 0;
int     ttStartIdx         = 0;
int     yearScrollOff      = 0;
int     sessionScrollOff   = 0;
int     sectionScrollOff   = 0;
int     filterModeSelection = 0;  // 0: Complete Timetable, 1: Filter by Day
int     dayFilterSelection  = -1;  // Selected day (-1 = all days)

// ================= TIMING =================
unsigned long lastPress    = 0;
const int     DEBOUNCE     = 250;
unsigned long lastBeepTime = 0;
const int     BEEP_COOL    = 500;
unsigned long lastAnnFetch = -31000;  // Initialize so first refresh happens immediately
const int     ANN_FETCH_INTERVAL = 30000;  // Fetch announcements every 30 seconds

// ================= FORWARD DECLARATIONS =================
void drawMenu();
void updateLEDs();
bool hasTTNew();

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
bool hasTTNew() {
  return (currentTT.sessionIdx >= 0);
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

// ========================================
// CATEGORY HELPERS
// ========================================
void addToCategory(Category &cat, int annId, const char* title, const char* msg, bool isNewAnnouncement = true) {
  if (cat.count >= MAX_PER_CAT) {
    for (int i = 0; i < MAX_PER_CAT - 1; i++)
      cat.items[i] = cat.items[i + 1];
    cat.count = MAX_PER_CAT - 1;
  }
  strncpy(cat.items[cat.count].title,   title, 59);
  strncpy(cat.items[cat.count].message, msg,   199);
  cat.items[cat.count].title[59]   = '\0';
  cat.items[cat.count].message[199] = '\0';
  cat.items[cat.count].id = annId;
  
  // Check if we've seen this announcement before
  if (hasSeenAnnouncement(annId)) {
    // Already seen - don't mark as new
    cat.items[cat.count].isNew = false;
  } else {
    // New announcement - mark as new only if it came from MQTT (isNewAnnouncement=true)
    cat.items[cat.count].isNew = isNewAnnouncement;
    if (isNewAnnouncement) {
      markAnnouncementAsSeen(annId);
    }
  }
  cat.count++;
}

void clearAllCategories() {
  urgent.count   = 0; urgent.index   = 0;
  general.count  = 0; general.index  = 0;
  eventCat.count = 0; eventCat.index = 0;
  office.count   = 0; office.index   = 0;
}

// ========================================
// HTTP - FETCH ANNOUNCEMENTS FROM DATABASE
// ========================================
void fetchAnnouncements() {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/announcements/approved/all";
  Serial.println("\n=== FETCHING ANNOUNCEMENTS ===");
  Serial.printf("URL: %s\n", url.c_str());
  
  if (!http.begin(espClient, url)) {
    Serial.println("HTTP begin failed!");
    return;
  }

  int code = http.GET();
  Serial.printf("HTTP Code: %d\n", code);
  
  if (code == 200) {
    String payload = http.getString();
    Serial.printf("Payload size: %d bytes\n", payload.length());
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.printf("JSON parse error: %s\n", error.c_str());
      http.end();
      return;
    }
    
    // Clear categories before refilling
    clearAllCategories();
    
    int loaded = 0;
    for (JsonObject ann : doc.as<JsonArray>()) {
      int annId        = ann["announcement_id"] | 0;
      const char* title    = ann["title"]    | "";
      const char* message  = ann["message"]  | "";
      const char* category = ann["category"] | "";
      
      Serial.printf("  - Ann ID: %d | Title: %s | Cat: %s\n", annId, title, category);

      // Load from database: isNew = false (only MQTT updates set isNew = true)
      // Pass false to indicate this is from database, not MQTT
      if      (strcmp(category, "urgent")  == 0) { addToCategory(urgent,   annId, title, message, false); loaded++; }
      else if (strcmp(category, "general") == 0) { addToCategory(general,  annId, title, message, false); loaded++; }
      else if (strcmp(category, "event")   == 0) { addToCategory(eventCat, annId, title, message, false); loaded++; }
      else if (strcmp(category, "office")  == 0) { addToCategory(office,   annId, title, message, false); loaded++; }
      
      // Mark as seen so it won't be re-marked as new on next fetch
      markAnnouncementAsSeen(annId);
    }
    
    Serial.printf("✅ Loaded %d announcements\n", loaded);
    Serial.printf("Categories: Urgent=%d, General=%d, Event=%d, Office=%d\n",
      urgent.count, general.count, eventCat.count, office.count);
    
    updateLEDs();
    if (currentState == MENU) drawMenu();
  } else {
    Serial.printf("HTTP Error: %d\n", code);
  }
  http.end();
}

void fetchYears() {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/years";
  if (!http.begin(espClient, url)) return;

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    totalYears = 0;
    for (JsonObject y : doc.as<JsonArray>()) {
      if (totalYears >= MAX_YEARS) break;
      years[totalYears].id = y["year_id"];
      strncpy(years[totalYears].name, y["year_name"] | "Unknown", 9);
      years[totalYears].name[9] = '\0';
      totalYears++;
    }
    Serial.printf("Loaded %d years\n", totalYears);
  }
  http.end();
}

void fetchSessions(int yearId) {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/sessions/" + yearId;
  if (!http.begin(espClient, url)) return;

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);
    totalSessions = 0;
    for (JsonObject s : doc.as<JsonArray>()) {
      if (totalSessions >= MAX_SESSIONS) break;
      sessions[totalSessions].id = s["session_id"];
      strncpy(sessions[totalSessions].name,
              s["session_name"] | "Unknown", 39);
      sessions[totalSessions].name[39] = '\0';
      totalSessions++;
    }
    Serial.printf("Loaded %d sessions for year %d\n", totalSessions, yearId);
  }
  http.end();
}

void fetchSections(int sessionId) {
  HTTPClient http;
  String url = String("http://") + serverIP + ":3000/sections/" + sessionId;
  if (!http.begin(espClient, url)) return;

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    totalSections = 0;
    for (JsonObject s : doc.as<JsonArray>()) {
      if (totalSections >= MAX_SECTIONS) break;
      sections[totalSections].id = s["section_id"];
      strncpy(sections[totalSections].name,
              s["section_name"] | "?", 9);
      sections[totalSections].name[9] = '\0';
      totalSections++;
    }
    Serial.printf("Loaded %d sections\n", totalSections);
  }
  http.end();
}

void fetchTimetable(int yIdx, int sIdx, int secIdx) {
  int yearId    = years[yIdx].id;
  int sessionId = sessions[sIdx].id;
  int sectionId = sections[secIdx].id;

  HTTPClient http;
  String url = String("http://") + serverIP +
               ":3000/timetable/" + yearId + "/" + sessionId + "/" + sectionId;

  // Append day_id if a filter is selected
  if (selectedDayFilter != -1) {
    // day_id in the database is 1-5 for Mon-Fri, and our filter is 0-4.
    url += "/" + String(selectedDayFilter + 1);
  }

  if (!http.begin(espClient, url)) return;

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, payload);

    if (doc["success"] == true) {
      currentTT.count      = 0;
      currentTT.yearIdx    = yIdx;
      currentTT.sessionIdx = sIdx;
      currentTT.sectionIdx = secIdx;
      selectedDayFilter    = -1;  // Reset day filter

      // Extract semester, session_name, and term from response
      strncpy(currentTT.semester, doc["semester"] | "Unknown", 19);
      strncpy(currentTT.session_name, doc["session_name"] | "Unknown", 39);
      strncpy(currentTT.term, doc["term"] | "Unknown", 19);
      currentTT.semester[19] = '\0';
      currentTT.session_name[39] = '\0';
      currentTT.term[19] = '\0';

      for (JsonObject e : doc["data"].as<JsonArray>()) {
        if (currentTT.count >= MAX_TT_ENTRIES) break;
        int i = currentTT.count;

        String t = String(e["start_time"] | "") + "-" +
                   String(e["end_time"]   | "");
        strncpy(currentTT.entries[i].time,    t.c_str(),              19);
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
      Serial.printf("Loaded %d timetable entries | %s - %s (%s)\n", 
                    currentTT.count, currentTT.semester, currentTT.term, currentTT.session_name);
    }
  }
  http.end();
}

// ========================================
// SCREENS - DRAW MENU
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

    if (i == selectedMenu) {
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

    if (i == selectedMenu) {
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

  // Title box
  vga.fillRect(20, 58, 360, 45, DARK_BLUE);
  drawBox(20, 58, 360, 45, CYAN);
  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(30, 65);
  vga.print(title);
  vga.setTextColor(WHITE, DARK_BLUE);
  vga.setCursor(30, 80);
  vga.print(cat.items[cat.index].title);

  // Message box
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

  // Mark as viewed
  cat.items[cat.index].isNew = false;
  markAnnouncementAsSeen(cat.items[cat.index].id);
  updateLEDs();
}

void drawYearSelect() {
  drawBase("U/D:Navigate  E:Select");
  centerText(58, "SELECT YEAR / BATCH", YELLOW, BLACK);

  if (totalYears == 0) {
    centerText(150, "No years found!", CYAN, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = (yearScrollOff + VISIBLE < totalYears) ? 
            (yearScrollOff + VISIBLE) : totalYears;

  for (int i = yearScrollOff; i < end; i++) {
    int slot = i - yearScrollOff;
    int y    = 75 + slot * 38;
    bool sel = (i == selYear);

    vga.fillRect(50, y, 300, 33, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 33, sel ? YELLOW : LIGHT_BLUE);
    centerText(y + 12, years[i].name,
               sel ? YELLOW : WHITE,
               sel ? DARK_BLUE : GRAY);
  }

  if (yearScrollOff > 0)
    centerText(68, "^ scroll up", CYAN, BLACK);
  if (yearScrollOff + VISIBLE < totalYears)
    centerText(272, "v scroll down", CYAN, BLACK);
}

void drawSessionSelect() {
  drawBase("U/D:Navigate  E:Select");
  
  char header[60];
  sprintf(header, "%s - SELECT SESSION", years[selYear].name);
  centerText(58, header, YELLOW, BLACK);

  if (totalSessions == 0) {
    centerText(150, "No sessions found!", CYAN, BLACK);
    return;
  }

  const int VISIBLE = 5;
  int end = (sessionScrollOff + VISIBLE < totalSessions) ? 
            (sessionScrollOff + VISIBLE) : totalSessions;

  for (int i = sessionScrollOff; i < end; i++) {
    int slot = i - sessionScrollOff;
    int y    = 85 + slot * 38;
    bool sel = (i == selSession);

    vga.fillRect(50, y, 300, 33, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 33, sel ? YELLOW : LIGHT_BLUE);
    centerText(y + 12, sessions[i].name,
               sel ? YELLOW : WHITE,
               sel ? DARK_BLUE : GRAY);
  }

  if (sessionScrollOff > 0)
    centerText(78, "^ scroll up", CYAN, BLACK);
  if (sessionScrollOff + VISIBLE < totalSessions)
    centerText(272, "v scroll down", CYAN, BLACK);
}

void drawSectionSelect() {
  drawBase("U/D:Navigate  E:Select");

  char title[60];
  sprintf(title, "%s - SELECT SECTION", sessions[selSession].name);
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
    int y    = 85 + slot * 38;
    bool sel = (i == selSection);

    vga.fillRect(100, y, 200, 33, sel ? DARK_BLUE : GRAY);
    drawBox(100, y, 200, 33, sel ? GREEN : LIGHT_BLUE);
    centerText(y + 12, sections[i].name,
               sel ? GREEN : WHITE,
               sel ? DARK_BLUE : GRAY);
  }

  if (sectionScrollOff > 0)
    centerText(78, "^ scroll up", CYAN, BLACK);
  if (sectionScrollOff + VISIBLE < totalSections)
    centerText(272, "v scroll down", CYAN, BLACK);
}

void drawFilterMode() {
  drawBase("U/D:Navigate  E:Select");
  centerText(58, "SELECT VIEW MODE", YELLOW, BLACK);

  const char* options[2] = {
    "COMPLETE TIMETABLE",
    "FILTER BY DAY"
  };

  for (int i = 0; i < 2; i++) {
    int y = 100 + i * 70;
    bool sel = (i == filterModeSelection);

    vga.fillRect(40, y, 320, 55, sel ? DARK_BLUE : GRAY);
    drawBox(40, y, 320, 55, sel ? CYAN : LIGHT_BLUE);
    centerText(y + 20, options[i],
               sel ? YELLOW : WHITE,
               sel ? DARK_BLUE : GRAY);
  }
}

void drawDayFilter() {
  drawBase("U/D:Select Day  E:View");
  centerText(58, "SELECT DAY", YELLOW, BLACK);

  for (int i = 0; i < 5; i++) {
    int y = 85 + i * 38;
    bool sel = (i == dayFilterSelection);

    vga.fillRect(50, y, 300, 33, sel ? DARK_BLUE : GRAY);
    drawBox(50, y, 300, 33, sel ? GREEN : LIGHT_BLUE);
    centerText(y + 12, dayNames[i],
               sel ? GREEN : WHITE,
               sel ? DARK_BLUE : GRAY);
  }
}

void drawTimetable() {
  vga.clear(BLACK);

  // ========== HEADER ==========
  vga.fillRect(0, 0, 400, 60, DARK_BLUE);
  drawBox(10, 5, 380, 50, CYAN);

  char headerTitle[80];
  sprintf(headerTitle, "%s | Sec: %s",
          currentTT.semester,
          sections[selSection].name);

  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(20, 13);
  vga.print(headerTitle);

  char dayInfo[60] = "All Days";

  if (selectedDayFilter >= 0) {
    sprintf(dayInfo, "%s Only", dayNames[selectedDayFilter]);
  }

  vga.setTextColor(CYAN, DARK_BLUE);
  vga.setCursor(20, 32);
  vga.print(dayInfo);

  // ========== CHECK DATA ==========
  if (currentTT.count == 0) {
    vga.fillRect(20, 80, 360, 180, GRAY);
    drawBox(20, 80, 360, 180, LIGHT_BLUE);

    vga.setTextColor(RED, GRAY);
    vga.setCursor(80, 130);
    vga.print("NO TIMETABLE DATA");
    return;
  }

  buildTimetableGrid();

  if (ttGrid.dayCount == 0 || ttGrid.slotCount == 0) {
    vga.fillRect(20, 80, 360, 180, GRAY);
    drawBox(20, 80, 360, 180, LIGHT_BLUE);

    vga.setTextColor(RED, GRAY);
    vga.setCursor(100, 130);
    vga.print("NO SCHEDULE");
    return;
  }

  // =========================================
  // NEW ORIENTATION
  // DAYS = VERTICAL
  // TIMES = HORIZONTAL
  // =========================================

  int numDays = ttGrid.dayCount;
  if (numDays > 5) numDays = 5;

  int timeCols = ttGrid.slotCount;
  if (timeCols > 8) timeCols = 8;

  int startX = 5;
  int startY = 70;

  int dayColW = 42;
  int cellW = 42;
  int rowH = 30;

  // ========== TOP LEFT ==========
  vga.fillRect(startX, startY, dayColW, rowH, DARK_BLUE);
  drawBox(startX, startY, dayColW, rowH, CYAN);

  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(startX + 6, startY + 10);
  vga.print("DAY");

  // ========== TIME HEADERS ==========
  for (int t = 0; t < timeCols; t++) {

    int x = startX + dayColW + t * cellW;

    vga.fillRect(x, startY, cellW, rowH, DARK_BLUE);
    drawBox(x, startY, cellW, rowH, CYAN);

    vga.setTextColor(CYAN, DARK_BLUE);
    vga.setCursor(x + 2, startY + 10);

    char shortTime[6];
    strncpy(shortTime, ttGrid.slots[t], 5);
    shortTime[5] = '\0';

    vga.print(shortTime);
  }

  // ========== DAY ROWS ==========
  for (int d = 0; d < numDays; d++) {

    int y = startY + rowH + d * rowH;

    // DAY NAME
    vga.fillRect(startX, y, dayColW, rowH, DARK_BLUE);
    drawBox(startX, y, dayColW, rowH, CYAN);

    vga.setTextColor(YELLOW, DARK_BLUE);
    vga.setCursor(startX + 4, y + 10);
    vga.print(ttGrid.days[d]);

    // ========== CELLS ==========
    for (int s = 0; s < timeCols; s++) {

      int x = startX + dayColW + s * cellW;

      const char* content = ttGrid.cells[d][s];

      int cellBg;
      int cellText;

      // FREE SLOT
      if (strcmp(content, "-") == 0 ||
          strcmp(content, "") == 0) {

        cellBg = vga.RGB(0, 120, 0);
        cellText = WHITE;
      }

      // CONTINUATION BLOCK
      else if (strcmp(content, "^") == 0) {

        cellBg = BLUE;
        cellText = WHITE;
      }

      // CLASS CELL
      else {

        cellBg = BLUE;
        cellText = WHITE;
      }

      vga.fillRect(x, y, cellW, rowH, cellBg);
      drawBox(x, y, cellW, rowH, LIGHT_BLUE);

      vga.setTextColor(cellText, cellBg);
      vga.setCursor(x + 1, y + 10);

      char txt[10];
      strncpy(txt, content, 8);
      txt[8] = '\0';

      vga.print(txt);
    }
  }

  // ========== FOOTER ==========
  vga.fillRect(0, 285, 400, 15, DARK_BLUE);
  drawBox(10, 285, 380, 14, CYAN);

  leftText(12, 290,
           "[UP/DN:Filter] [E:Menu] [B:Back]",
           LIGHT_BLUE,
           DARK_BLUE);
}
// ========================================
// MQTT - RECEIVE REAL-TIME UPDATES
// ========================================
void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg = "";
  for (int i = 0; i < (int)len; i++) msg += (char)payload[i];

  DynamicJsonDocument doc(512);
  deserializeJson(doc, msg);

  const char* action   = doc["action"]   | "";
  int annId            = doc["announcement_id"] | 0;
  const char* title    = doc["title"]    | "";
  const char* message  = doc["message"]  | "";
  const char* category = doc["category"] | "";

  // Handle deletion - refresh announcements from server
  if (strcmp(action, "deleted") == 0) {
    Serial.printf("Announcement deleted: %s\n", title);
    fetchAnnouncements();  // Refresh all announcements
    return;
  }

  // Handle regular announcements (new ones from MQTT)
  // Only add if not already seen (to avoid duplicates)
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
  bool back  = (digitalRead(backBtn)  == LOW);

  if (!up && !down && !enter && !back) return;
  lastPress = millis();

  if (currentState == MENU) {
    if (up)    { selectedMenu--; if (selectedMenu < 0)  selectedMenu = 4; drawMenu(); }
    if (down)  { selectedMenu++; if (selectedMenu > 4)  selectedMenu = 0; drawMenu(); }
    if (enter) {
      if      (selectedMenu == 0) { currentState = URGENT_VIEW;  showCategory(urgent,   "URGENT NOTICE");  }
      else if (selectedMenu == 1) { currentState = GENERAL_VIEW; showCategory(general,  "GENERAL NOTICE"); }
      else if (selectedMenu == 2) { currentState = EVENT_VIEW;   showCategory(eventCat, "EVENT NOTICE");   }
      else if (selectedMenu == 3) { currentState = OFFICE_VIEW;  showCategory(office,   "OFFICE NOTICE");  }
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
    if (up)    { urgent.index--;  if (urgent.index < 0)            urgent.index = urgent.count - 1;   showCategory(urgent,   "URGENT NOTICE");  }
    if (down)  { urgent.index++;  if (urgent.index >= urgent.count) urgent.index = 0;                  showCategory(urgent,   "URGENT NOTICE");  }
    if (enter) { currentState = MENU; drawMenu(); }
    if (back)  { currentState = MENU; drawMenu(); }
  }
  else if (currentState == GENERAL_VIEW) {
    if (up)    { general.index--; if (general.index < 0)             general.index = general.count - 1; showCategory(general,  "GENERAL NOTICE"); }
    if (down)  { general.index++; if (general.index >= general.count) general.index = 0;                showCategory(general,  "GENERAL NOTICE"); }
    if (enter) { currentState = MENU; drawMenu(); }
    if (back)  { currentState = MENU; drawMenu(); }
  }
  else if (currentState == EVENT_VIEW) {
    if (up)    { eventCat.index--; if (eventCat.index < 0)               eventCat.index = eventCat.count - 1; showCategory(eventCat, "EVENT NOTICE");   }
    if (down)  { eventCat.index++; if (eventCat.index >= eventCat.count)  eventCat.index = 0;                 showCategory(eventCat, "EVENT NOTICE");   }
    if (enter) { currentState = MENU; drawMenu(); }
    if (back)  { currentState = MENU; drawMenu(); }
  }
  else if (currentState == OFFICE_VIEW) {
    if (up)    { office.index--; if (office.index < 0)             office.index = office.count - 1; showCategory(office, "OFFICE NOTICE"); }
    if (down)  { office.index++; if (office.index >= office.count)  office.index = 0;               showCategory(office, "OFFICE NOTICE"); }
    if (enter) { currentState = MENU; drawMenu(); }
    if (back)  { currentState = MENU; drawMenu(); }
  }

  else if (currentState == TT_YEAR) {
    if (up) {
      if (selYear > 0) {
        selYear--;
        if (selYear < yearScrollOff) yearScrollOff = selYear;
      }
      drawYearSelect();
    }
    if (down) {
      if (selYear < totalYears - 1) {
        selYear++;
        if (selYear >= yearScrollOff + 5) yearScrollOff = selYear - 4;
      }
      drawYearSelect();
    }
    if (enter) {
      showLoading("Loading sessions...");
      delay(500);
      fetchSessions(years[selYear].id);
      selSession = 0; sessionScrollOff = 0;
      currentState = TT_SESSION;
      drawSessionSelect();
    }
    if (back) {
      currentState = MENU;
      selectedMenu = 4;  // Keep cursor at TIME TABLE
      drawMenu();
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
    if (back) {
      currentState = TT_YEAR;
      selYear = 0; yearScrollOff = 0;
      drawYearSelect();
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
      fetchTimetable(selYear, selSession, selSection);
      filterModeSelection = 0;
      dayFilterSelection = -1;
      currentState = TT_FILTER_MODE;
      drawFilterMode();
    }
    if (back) {
      currentState = TT_SESSION;
      selSession = 0; sessionScrollOff = 0;
      drawSessionSelect();
    }
  }

  else if (currentState == TT_FILTER_MODE) {
    if (up) {
      if (filterModeSelection > 0) filterModeSelection--;
      drawFilterMode();
    }
    if (down) {
      if (filterModeSelection < 1) filterModeSelection++;
      drawFilterMode();
    }
    if (enter) {
      if (filterModeSelection == 0) {
        // Complete Timetable - show all days
        selectedDayFilter = -1;
        ttStartIdx = 0;
        currentState = TT_DISPLAY;
        drawTimetable();
      } else {
        // Filter by Day - show day selection
        dayFilterSelection = 0;
        currentState = TT_DAY_FILTER;
        drawDayFilter();
      }
    }
    if (back) {
      currentState = TT_SECTION;
      selSection = 0; sectionScrollOff = 0;
      drawSectionSelect();
    }
  }

  else if (currentState == TT_DAY_FILTER) {
    if (up) {
      if (dayFilterSelection > 0) dayFilterSelection--;
      drawDayFilter();
    }
    if (down) {
      if (dayFilterSelection < 4) dayFilterSelection++;
      drawDayFilter();
    }
    if (enter) {
      selectedDayFilter = dayFilterSelection;
      ttStartIdx = 0;
      currentState = TT_DISPLAY;
      drawTimetable();
    }
    if (back) {
      filterModeSelection = 1;  // Go back to filter mode at "Filter by Day"
      currentState = TT_FILTER_MODE;
      drawFilterMode();
    }
  }

  else if (currentState == TT_DISPLAY) {
    if (up || down) {
      if (selectedDayFilter >= 0) {
        // If viewing single day, go back to day selection
        dayFilterSelection = selectedDayFilter;
        currentState = TT_DAY_FILTER;
        drawDayFilter();
      } else {
        // If viewing complete timetable, go back to filter mode
        filterModeSelection = 0;
        currentState = TT_FILTER_MODE;
        drawFilterMode();
      }
    }
    if (enter) { currentState = MENU; selectedMenu = 4; drawMenu(); }
    if (back) {
      if (selectedDayFilter >= 0) {
        // If viewing single day, go back to day selection
        dayFilterSelection = selectedDayFilter;
        currentState = TT_DAY_FILTER;
        drawDayFilter();
      } else {
        // If viewing complete timetable, go back to filter mode
        filterModeSelection = 0;
        currentState = TT_FILTER_MODE;
        drawFilterMode();
      }
    }
  }
}

// ========================================
// SETUP
// ========================================
void setup() {
  Serial.begin(115200);

  pinMode(upBtn,    INPUT_PULLUP);
  pinMode(downBtn,  INPUT_PULLUP);
  pinMode(enterBtn, INPUT_PULLUP);
  pinMode(backBtn,  INPUT_PULLUP);
  pinMode(buzzer,    OUTPUT);
  pinMode(redLED,    OUTPUT);
  pinMode(greenLED,  OUTPUT);
  pinMode(yellowLED, OUTPUT);

  digitalWrite(buzzer,    LOW);
  digitalWrite(redLED,    LOW);
  digitalWrite(greenLED,  LOW);
  digitalWrite(yellowLED, LOW);

  currentTT.count      = 0;
  currentTT.yearIdx    = -1;
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
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("WiFi OK!");

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
  Serial.println("Ready!");
}

// ========================================
// LOOP - MAIN PROGRAM
// ========================================
void loop() {
  if (!client.connected()) connectMQTT();
  client.loop();
  handleButtons();
  
  // Periodically fetch fresh announcements from server (every 30 seconds)
  if (millis() - lastAnnFetch > ANN_FETCH_INTERVAL) {
    lastAnnFetch = millis();
    if (currentState == MENU) {
      Serial.println("Refreshing announcements from server...");
      fetchAnnouncements();
    }
  }
  
  delay(10);
}
