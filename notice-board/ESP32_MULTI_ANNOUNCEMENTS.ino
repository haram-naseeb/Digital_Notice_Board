#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Lib.h>
#include <Ressources/Font6x8.h>
#include <HTTPClient.h>

// ================= WIFI =================
const char* ssid = "Fast net fiber n";
const char* password = "03004721644aa";
const char* serverIP = "192.168.1.6";  // 🔴 UPDATE THIS TO YOUR PC IP ADDRESS

// ================= MQTT =================
const char* mqttServer = "broker.emqx.io";
const int mqttPort = 1883;

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

// ================= MQTT GLOBAL =================
WiFiClient espClient;
PubSubClient client(espClient);

// ================= COLORS =================
int BLACK, WHITE, BLUE, YELLOW, CYAN, GRAY, LIGHT_BLUE, DARK_BLUE, RED, GREEN;

// ================= ANNOUNCEMENT STRUCTURE =================
struct Announcement {
  String title;
  String message;
  bool isNew;
};

// ================= STORAGE FOR MULTIPLE ANNOUNCEMENTS PER CATEGORY =================
const int MAX_PER_CATEGORY = 20;  // Max 20 announcements per category (reduced for ESP32 DRAM)

struct Category {
  Announcement announcements[MAX_PER_CATEGORY];
  int count;  // Number of announcements in this category
  int currentIndex;  // Currently viewing announcement index
};

Category urgent   = {{}, 0, 0};
Category general  = {{}, 0, 0};
Category event    = {{}, 0, 0};
Category office   = {{}, 0, 0};

// ================= TIMETABLE STRUCTURE =================
struct TimeTableEntry {
  String time;
  String subject;
  String teacher;
  String room;
};

struct TimeTable {
  TimeTableEntry entries[5];  // Reduced from 10 to save DRAM
  int count;
  bool isNew;
};

struct Session {
  int session_id;
  String session_name;
};

struct Section {
  int section_id;
  String section_name;
};

TimeTable timeTables[5][5];  // Reduced from [10][10] to save ~60KB DRAM
bool timetableLoaded[5][5] = {{false}};

// Session storage
const int MAX_SESSIONS = 10;
const int MAX_SECTIONS = 10;
Session availableSessions[MAX_SESSIONS];
Section availableSections[MAX_SECTIONS];
int totalSessions = 0;
int totalSections = 0;
int selectedSessionIndex = 0;  // Currently selected session (after ENTER pressed)

// ================= UI STATE =================
enum UIState { 
  MENU, 
  URGENT_VIEW, GENERAL_VIEW, EVENT_VIEW, OFFICE_VIEW,
  TIMETABLE_SESSION_SELECT, TIMETABLE_SECTION_SELECT, TIMETABLE_VIEW_SELECT, TIMETABLE_DISPLAY
};
UIState currentState = MENU;
int selectedMenu = 0;
int timetableSelectedSession = 0; // Which session user is selecting in menu
int timetableSelectedSection = 0;  // 0=A, 1=B, 2=C
int timetableViewType = 0;         // 0=Today, 1=Full Week
bool showingFullWeekTimetable = false;
int timetableStartIndex = 0;       // For scrolling through timetable entries
unsigned long lastPress = 0;
const int debounceDelay = 200;

// ================= TIMING =================
unsigned long lastBeepTime = 0;
const int beepCooldown = 500;

// ===================================================
// FETCH SECTIONS FOR SELECTED SESSION
// ===================================================
void fetchSectionsForSession(int sessionId) {
  if (!espClient.connected()) {
    Serial.println("❌ WiFi not connected!");
    return;
  }
  
  Serial.print("[HTTP] Fetching sections for session ");
  Serial.println(sessionId);
  
  HTTPClient http;
  String url = "http://";
  url += serverIP;
  url += ":3000/sections/";
  url += String(sessionId);
  
  if (!http.begin(espClient, url)) {
    Serial.println("❌ HTTP begin failed");
    return;
  }
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("[HTTP] Sections: " + payload);
    
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.println("❌ JSON parse error!");
      http.end();
      return;
    }
    
    JsonArray sectionsArray = doc.as<JsonArray>();
    
    totalSections = 0;
    for (JsonObject section : sectionsArray) {
      if (totalSections >= MAX_SECTIONS) break;
      
      availableSections[totalSections].section_id = section["section_id"];
      availableSections[totalSections].section_name = (const char*)section["section_name"];
      
      Serial.print("✅ Section: ");
      Serial.print(availableSections[totalSections].section_id);
      Serial.print(" - ");
      Serial.println(availableSections[totalSections].section_name.c_str());
      
      totalSections++;
    }
    
    Serial.print("✅ Loaded ");
    Serial.print(totalSections);
    Serial.println(" sections");
  } else {
    Serial.print("❌ HTTP Error: ");
    Serial.println(httpCode);
  }
  
  http.end();
}
void fetchSessions() {
  if (!espClient.connected()) {
    Serial.println("❌ WiFi not connected!");
    return;
  }
  
  Serial.println("[HTTP] Fetching sessions from server...");
  
  HTTPClient http;
  String url = "http://";
  url += serverIP;
  url += ":3000/sessions";
  
  if (!http.begin(espClient, url)) {
    Serial.println("❌ HTTP begin failed");
    return;
  }
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("[HTTP] Sessions: " + payload);
    
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.println("❌ JSON parse error!");
      http.end();
      return;
    }
    
    JsonArray sessionsArray = doc.as<JsonArray>();
    
    totalSessions = 0;
    for (JsonObject session : sessionsArray) {
      if (totalSessions >= MAX_SESSIONS) break;
      
      availableSessions[totalSessions].session_id = session["session_id"];
      availableSessions[totalSessions].session_name = (const char*)session["session_name"];
      
      Serial.print("✅ Session: ");
      Serial.print(availableSessions[totalSessions].session_id);
      Serial.print(" - ");
      Serial.println(availableSessions[totalSessions].session_name.c_str());
      
      totalSessions++;
    }
    
    Serial.print("✅ Loaded ");
    Serial.print(totalSessions);
    Serial.println(" sessions");
  } else {
    Serial.print("❌ HTTP Error: ");
    Serial.println(httpCode);
  }
  
  http.end();
}
void initTimeTables() {
  for (int s = 0; s < 5; s++) {
    for (int t = 0; t < 5; t++) {
      timeTables[s][t].count = 0;
      timeTables[s][t].isNew = false;
    }
  }
}

// ===================================================
// FETCH TIMETABLE DATA FROM SERVER
// ===================================================
void fetchTimetableData(int sessionId, int sectionId) {
  if (!espClient.connected()) {
    Serial.println("❌ WiFi not connected!");
    return;
  }
  
  Serial.print("[HTTP] Fetching timetable for session ");
  Serial.print(sessionId);
  Serial.print(", section ");
  Serial.println(sectionId);
  
  HTTPClient http;
  String url = "http://";
  url += serverIP;
  url += ":3000/timetable/";
  url += String(sessionId);
  url += "/";
  url += String(sectionId);
  
  if (!http.begin(espClient, url)) {
    Serial.println("❌ HTTP begin failed");
    return;
  }
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("[HTTP] Response: " + payload);
    
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.println("❌ JSON parse error!");
      http.end();
      return;
    }
    
    if (doc["success"] == true) {
      JsonArray dataArray = doc["data"].as<JsonArray>();
      
      // Find timetable for current session/section combo
      int sessionIdx = 0;  // We'll use simple indexing
      int sectionIdx = sectionId - 1;  // 0-based
      
      // Clear existing data
      timeTables[sessionIdx][sectionIdx].count = 0;
      
      // Add entries (max 5 to save DRAM)
      int entryIndex = 0;
      for (JsonObject entry : dataArray) {
        if (entryIndex >= 5) break;
        
        String time = String((const char*)entry["start_time"]) + " - " + String((const char*)entry["end_time"]);
        String subject = (const char*)entry["subject_name"];
        String teacher = (const char*)entry["teacher_name"];
        String room = (const char*)entry["room_number"];
        
        timeTables[sessionIdx][sectionIdx].entries[entryIndex].time = time;
        timeTables[sessionIdx][sectionIdx].entries[entryIndex].subject = subject;
        timeTables[sessionIdx][sectionIdx].entries[entryIndex].teacher = teacher;
        timeTables[sessionIdx][sectionIdx].entries[entryIndex].room = room;
        
        entryIndex++;
      }
      
      timeTables[sessionIdx][sectionIdx].count = entryIndex;
      timeTables[sessionIdx][sectionIdx].isNew = true;
      
      Serial.print("✅ Loaded ");
      Serial.print(entryIndex);
      Serial.println(" timetable entries");
    }
  } else {
    Serial.print("❌ HTTP Error: ");
    Serial.println(httpCode);
  }
  
  http.end();
}

// ===================================================
// LED CONTROL - Check if any category has NEW
// ===================================================
void updateLEDs() {
  bool hasUrgentNew = false, hasGeneralNew = false, hasEventNew = false;
  
  for (int i = 0; i < urgent.count; i++) {
    if (urgent.announcements[i].isNew) hasUrgentNew = true;
  }
  for (int i = 0; i < general.count; i++) {
    if (general.announcements[i].isNew) hasGeneralNew = true;
  }
  for (int i = 0; i < event.count; i++) {
    if (event.announcements[i].isNew) hasEventNew = true;
  }
  
  bool hasOfficeNew = false;
  for (int i = 0; i < office.count; i++) {
    if (office.announcements[i].isNew) hasOfficeNew = true;
  }
  
  bool hasTimetableNew = false;
  for (int s = 0; s < 5; s++) {
    for (int t = 0; t < 5; t++) {
      if (timeTables[s][t].isNew) hasTimetableNew = true;
    }
  }
  
  digitalWrite(redLED, hasUrgentNew);
  digitalWrite(greenLED, hasGeneralNew);
  digitalWrite(yellowLED, (hasEventNew || hasOfficeNew) || hasTimetableNew);
}

// ===================================================
// ADD ANNOUNCEMENT TO CATEGORY
// ===================================================
void addAnnouncementToCategory(Category &cat, String title, String message) {
  if (cat.count < MAX_PER_CATEGORY) {
    cat.announcements[cat.count].title = title;
    cat.announcements[cat.count].message = message;
    cat.announcements[cat.count].isNew = true;
    cat.count++;
    Serial.println("[ADD] Added to category. Total: " + String(cat.count));
  } else {
    Serial.println("[WARNING] Category full! Max " + String(MAX_PER_CATEGORY) + " announcements");
  }
}

// ===================================================
// CLEAR CATEGORY
// ===================================================
void clearCategory(Category &cat) {
  cat.count = 0;
  cat.currentIndex = 0;
}

// ===================================================
// BUZZER PATTERNS
// ===================================================
void beepOnce() {
  if (millis() - lastBeepTime < beepCooldown) return;
  digitalWrite(buzzer, HIGH);
  delay(100);
  digitalWrite(buzzer, LOW);
  lastBeepTime = millis();
}

void beepTwice() {
  if (millis() - lastBeepTime < beepCooldown) return;
  for (int i = 0; i < 2; i++) {
    digitalWrite(buzzer, HIGH);
    delay(100);
    digitalWrite(buzzer, LOW);
    delay(100);
  }
  lastBeepTime = millis();
}

void beepThrice() {
  if (millis() - lastBeepTime < beepCooldown) return;
  for (int i = 0; i < 3; i++) {
    digitalWrite(buzzer, HIGH);
    delay(100);
    digitalWrite(buzzer, LOW);
    if (i < 2) delay(100);
  }
  lastBeepTime = millis();
}

// ===================================================
// TEXT HELPERS
// ===================================================
void centerText(int y, const char* txt, int color, int bgColor = BLACK) {
  int len = strlen(txt);
  int x = (400 - len * 6) / 2;
  vga.setTextColor(color, bgColor);
  vga.setCursor(x, y);
  vga.print(txt);
}

void leftText(int x, int y, const char* txt, int color, int bgColor = BLACK) {
  vga.setTextColor(color, bgColor);
  vga.setCursor(x, y);
  vga.print(txt);
}

// ===================================================
// DRAW RECTANGULAR BORDER
// ===================================================
void drawBorder(int x, int y, int w, int h, int color) {
  vga.fillRect(x, y, w, 2, color);  // Top
  vga.fillRect(x, y + h - 2, w, 2, color);  // Bottom
  vga.fillRect(x, y, 2, h, color);  // Left
  vga.fillRect(x + w - 2, y, 2, h, color);  // Right
}

// ===================================================
// HEADER
// ===================================================
void drawHeader() {
  vga.fillRect(0, 0, 400, 50, DARK_BLUE);
  drawBorder(10, 5, 380, 40, CYAN);
  centerText(20, "DEPARTMENT NOTICE BOARD", YELLOW, DARK_BLUE);
}

// ===================================================
// FOOTER
// ===================================================
void drawFooter() {
  vga.fillRect(0, 285, 400, 15, DARK_BLUE);
  drawBorder(10, 285, 380, 15, CYAN);
  
  if (client.connected()) {
    leftText(20, 290, "[ONLINE]", GREEN, DARK_BLUE);
  } else {
    leftText(20, 290, "[OFFLINE]", RED, DARK_BLUE);
  }
  
  leftText(200, 290, "U/D: Nav  E: Select/Back", LIGHT_BLUE, DARK_BLUE);
}

// ===================================================
// BASE SCREEN
// ===================================================
void drawBase() {
  vga.clear(BLACK);
  drawHeader();
  drawFooter();
}

// ===================================================
// MENU - CATEGORY LIST (NOW WITH 5 ITEMS)
// ===================================================
void drawMenu() {
  drawBase();
  
  const char* items[5] = {
    "URGENT NOTICES",
    "GENERAL NOTICES",
    "EVENT NOTICES",
    "TEACHER OFFICES",
    "TIME TABLE"
  };
  
  Category* categories[4] = {&urgent, &general, &event, &office};
  int colors[5] = {RED, GREEN, YELLOW, CYAN, BLUE};
  
  for (int i = 0; i < 5; i++) {
    int yPos = 60 + i * 40;
    int boxWidth = 340;
    int boxHeight = 35;
    
    if (i == selectedMenu) {
      vga.fillRect(30, yPos, boxWidth, boxHeight, DARK_BLUE);
      drawBorder(30, yPos, boxWidth, boxHeight, colors[i]);
      vga.setTextColor(YELLOW, DARK_BLUE);
    } else {
      vga.fillRect(30, yPos, boxWidth, boxHeight, GRAY);
      drawBorder(30, yPos, boxWidth, boxHeight, LIGHT_BLUE);
      vga.setTextColor(WHITE, GRAY);
    }
    
    vga.fillRect(42, yPos + 5, 22, 22, colors[i]);
    
    vga.setCursor(75, yPos + 13);
    vga.print(items[i]);
    
    if (i < 4) {
      char countText[10];
      sprintf(countText, "(%d)", categories[i]->count);
      vga.setCursor(265, yPos + 13);
      vga.print(countText);
      
      bool hasNew = false;
      for (int j = 0; j < categories[i]->count; j++) {
        if (categories[i]->announcements[j].isNew) {
          hasNew = true;
          break;
        }
      }
      
      if (hasNew) {
        vga.fillRect(310, yPos + 7, 40, 20, RED);
        vga.setTextColor(WHITE, RED);
        vga.setCursor(318, yPos + 14);
        vga.print("NEW");
      }
    } else {
      if (hasTimetableNew()) {
        vga.fillRect(310, yPos + 7, 40, 20, RED);
        vga.setTextColor(WHITE, RED);
        vga.setCursor(318, yPos + 14);
        vga.print("NEW");
      }
    }
    
    if (i == selectedMenu) {
      vga.setTextColor(CYAN, DARK_BLUE);
      vga.setCursor(360, yPos + 13);
      vga.print(">");
    }
  }
  
  updateLEDs();
}

// ===================================================
// CHECK IF TIMETABLE HAS NEW DATA
// ===================================================
bool hasTimetableNew() {
  for (int s = 0; s < 5; s++) {
    for (int t = 0; t < 5; t++) {
      if (timeTables[s][t].isNew) return true;
    }
  }
  return false;
}

// ===================================================
// SHOW NOTICE DETAILS (MULTIPLE IN CATEGORY)
// ===================================================
void showCategoryView(Category &cat, const char* title) {
  if (cat.count == 0) {
    drawBase();
    centerText(150, "NO ANNOUNCEMENTS", CYAN, BLACK);
    centerText(170, "in this category", LIGHT_BLUE, BLACK);
    return;
  }
  
  drawBase();
  
  // Ensure valid index
  if (cat.currentIndex >= cat.count) cat.currentIndex = 0;
  
  Announcement &current = cat.announcements[cat.currentIndex];
  
  // Title box
  vga.fillRect(30, 70, 340, 50, DARK_BLUE);
  drawBorder(30, 70, 340, 50, CYAN);
  
  vga.setTextColor(YELLOW, DARK_BLUE);
  vga.setCursor(50, 80);
  vga.print(title);
  
  vga.setTextColor(LIGHT_BLUE, DARK_BLUE);
  vga.setCursor(50, 100);
  vga.print(current.title.c_str());
  
  // Message box
  vga.fillRect(30, 130, 340, 140, GRAY);
  drawBorder(30, 130, 340, 140, LIGHT_BLUE);
  
  vga.setTextColor(WHITE, GRAY);
  
  if (current.message.length() > 0) {
    String msg = current.message;
    int line = 0;
    int pos = 0;
    int maxChars = 50;
    
    while (pos < msg.length() && line < 6) {
      int endPos = pos + maxChars;
      if (endPos > msg.length()) endPos = msg.length();
      
      String lineText = msg.substring(pos, endPos);
      vga.setCursor(38, 145 + line * 18);
      vga.print(lineText.c_str());
      
      pos = endPos;
      line++;
    }
  } else {
    vga.setCursor(38, 175);
    vga.print("No message available");
  }
  
  // Mark as read
  current.isNew = false;
  updateLEDs();
  
  // Navigation info at bottom
  char navText[60];
  sprintf(navText, "Announcement %d of %d | U/D: Navigate | E: Back", 
          cat.currentIndex + 1, cat.count);
  
  vga.setTextColor(CYAN, BLACK);
  vga.setCursor(10, 265);
  vga.print(navText);
}

// ===================================================
// TIMETABLE SESSION SELECTION SCREEN
// ===================================================
void drawSessionSelection() {
  drawBase();
  
  centerText(70, "SELECT SESSION/BATCH", YELLOW, BLACK);
  
  if (totalSessions == 0) {
    centerText(150, "LOADING SESSIONS...", CYAN, BLACK);
    fetchSessions();
    if (totalSessions == 0) {
      centerText(170, "No sessions found", LIGHT_BLUE, BLACK);
      return;
    }
  }
  
  int yPos = 130;
  const int boxHeight = 35;
  int boxesPerScreen = 3;  // Show max 3 sessions
  
  for (int i = 0; i < totalSessions && i < boxesPerScreen; i++) {
    int yBoxPos = yPos + i * 50;
    
    if (i == selectedSessionIndex) {
      vga.fillRect(80, yBoxPos, 240, boxHeight, DARK_BLUE);
      drawBorder(80, yBoxPos, 240, boxHeight, YELLOW);
      vga.setTextColor(YELLOW, DARK_BLUE);
    } else {
      vga.fillRect(80, yBoxPos, 240, boxHeight, GRAY);
      drawBorder(80, yBoxPos, 240, boxHeight, LIGHT_BLUE);
      vga.setTextColor(WHITE, GRAY);
    }
    
    centerText(yBoxPos + 13, availableSessions[i].session_name.c_str(), 
               (i == selectedSessionIndex ? YELLOW : WHITE),
               (i == selectedSessionIndex ? DARK_BLUE : GRAY));
  }
}

// ===================================================
// TIMETABLE SECTION SELECTION SCREEN
// ===================================================
void drawSectionSelection() {
  drawBase();
  
  char title[50];
  sprintf(title, "SELECT SECTION - %s", availableSessions[selectedSessionIndex].session_name.c_str());
  centerText(70, title, YELLOW, BLACK);
  
  if (totalSections == 0) {
    centerText(150, "LOADING SECTIONS...", CYAN, BLACK);
    return;
  }
  
  int yPos = 130;
  const int boxHeight = 35;
  int boxesPerScreen = 3;  // Show max 3 sections
  
  for (int i = 0; i < totalSections && i < boxesPerScreen; i++) {
    int yBoxPos = yPos + i * 50;
    
    if (i == timetableSelectedSection) {
      vga.fillRect(80, yBoxPos, 240, boxHeight, DARK_BLUE);
      drawBorder(80, yBoxPos, 240, boxHeight, GREEN);
      vga.setTextColor(YELLOW, DARK_BLUE);
    } else {
      vga.fillRect(80, yBoxPos, 240, boxHeight, GRAY);
      drawBorder(80, yBoxPos, 240, boxHeight, LIGHT_BLUE);
      vga.setTextColor(WHITE, GRAY);
    }
    
    centerText(yBoxPos + 13, availableSections[i].section_name.c_str(), 
               (i == timetableSelectedSection ? GREEN : WHITE),
               (i == timetableSelectedSection ? DARK_BLUE : GRAY));
  }
}

// ===================================================
// TIMETABLE VIEW TYPE SELECTION SCREEN
// ===================================================
void drawViewTypeSelection() {
  drawBase();
  
  char title[60];
  sprintf(title, "%s - %s", availableSessions[selectedSessionIndex].session_name.c_str(), 
          availableSections[timetableSelectedSection].section_name.c_str());
  centerText(70, title, YELLOW, BLACK);
  centerText(90, "SELECT VIEW", LIGHT_BLUE, BLACK);
  
  const char* viewTypes[2] = {"TODAY TIMETABLE", "FULL WEEK"};
  int viewColors[2] = {CYAN, BLUE};
  
  for (int i = 0; i < 2; i++) {
    int yPos = 140 + i * 70;
    
    if (i == timetableViewType) {
      vga.fillRect(60, yPos, 280, 50, DARK_BLUE);
      drawBorder(60, yPos, 280, 50, viewColors[i]);
      vga.setTextColor(YELLOW, DARK_BLUE);
    } else {
      vga.fillRect(60, yPos, 280, 50, GRAY);
      drawBorder(60, yPos, 280, 50, LIGHT_BLUE);
      vga.setTextColor(WHITE, GRAY);
    }
    
    centerText(yPos + 20, viewTypes[i], (i == timetableViewType ? viewColors[i] : WHITE), (i == timetableViewType ? DARK_BLUE : GRAY));
  }
}

// ===================================================
// DISPLAY TIMETABLE
// ===================================================
void displayTimetable() {
  drawBase();
  
  // Get the actual session and section IDs from database
  int sessionId = availableSessions[selectedSessionIndex].session_id;
  int sectionId = availableSections[timetableSelectedSection].section_id;
  
  TimeTable &tt = timeTables[sessionId][sectionId];
  
  // Fetch data if not already loaded
  if (tt.count == 0) {
    centerText(150, "LOADING TIMETABLE...", CYAN, BLACK);
    fetchTimetableData(sessionId, sectionId);
    delay(500);
  }
  
  if (tt.count == 0) {
    centerText(150, "NO TIMETABLE DATA", CYAN, BLACK);
    centerText(170, "Available for this selection", LIGHT_BLUE, BLACK);
    return;
  }
  
  char header[60];
  sprintf(header, "%s - %s", availableSessions[selectedSessionIndex].session_name.c_str(), 
          availableSections[timetableSelectedSection].section_name.c_str());
  
  centerText(70, header, YELLOW, BLACK);
  
  // Reset start index if out of bounds
  if (timetableStartIndex >= tt.count) timetableStartIndex = 0;
  
  int yPos = 100;
  int displayedCount = 0;
  const int maxDisplay = 6;
  
  for (int i = timetableStartIndex; i < tt.count && displayedCount < maxDisplay; i++) {
    vga.setTextColor(LIGHT_BLUE, BLACK);
    vga.setCursor(40, yPos);
    vga.print(tt.entries[i].time.c_str());
    
    vga.setTextColor(WHITE, BLACK);
    vga.setCursor(40, yPos + 12);
    vga.print(tt.entries[i].subject.c_str());
    
    yPos += 24;
    displayedCount++;
  }
  
  // Show pagination info
  char pageInfo[50];
  sprintf(pageInfo, "Entry %d of %d | U/D: Scroll | E: Back", timetableStartIndex + 1, tt.count);
  vga.setTextColor(CYAN, BLACK);
  vga.setCursor(10, 265);
  vga.print(pageInfo);
  
  tt.isNew = false;
  updateLEDs();
}

// ===================================================
// MQTT CALLBACK
// ===================================================
void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg = "";
  for (int i = 0; i < len; i++) msg += (char)payload[i];
  
  Serial.println("[MQTT] Received: " + msg);
  
  StaticJsonDocument<512> doc;
  deserializeJson(doc, msg);
  
  String title = doc["title"] | "";
  String message = doc["message"] | "";
  String category = doc["category"] | "";
  
  // Handle cleared/deleted - clear ALL categories
  if (category == "cleared" || category == "deleted") {
    Serial.println("🧹 Display cleared");
    clearCategory(urgent);
    clearCategory(general);
    clearCategory(event);
    clearCategory(office);
    updateLEDs();
    currentState = MENU;
    drawMenu();
    return;
  }
  
  // Add announcement to appropriate category
  if (category == "urgent") {
    addAnnouncementToCategory(urgent, title, message);
    beepThrice();
    Serial.println("🚨 Urgent notice added");
  }
  else if (category == "general") {
    addAnnouncementToCategory(general, title, message);
    beepOnce();
    Serial.println("📢 General notice added");
  }
  else if (category == "event") {
    addAnnouncementToCategory(event, title, message);
    beepTwice();
    Serial.println("🎉 Event notice added");
  }
  else if (category == "office") {
    addAnnouncementToCategory(office, title, message);
    Serial.println("🏢 Office notice added");
  }
  else if (category == "timetable") {
    int session = doc["session"] | 0;
    int section = doc["section"] | 0;
    
    if (session >= 0 && session < 5 && section >= 0 && section < 5) {
      TimeTable &tt = timeTables[session][section];
      int entry = doc["entry"] | -1;
      
      if (entry == -1) {
        tt.count = 0;
      } else if (entry >= 0 && entry < 5) {
        tt.entries[entry].time = (const char*)doc["time"];
        tt.entries[entry].subject = (const char*)doc["subject"];
        tt.entries[entry].teacher = (const char*)doc["teacher"];
        tt.entries[entry].room = (const char*)doc["room"];
        if (entry + 1 > tt.count) tt.count = entry + 1;
      }
      
      tt.isNew = true;
      Serial.println("📅 Timetable updated");
    }
  }
  
  updateLEDs();
  if (currentState == MENU) drawMenu();
}

// ===================================================
// BUTTON HANDLING
// ===================================================
void handleButtons() {
  if (millis() - lastPress < debounceDelay) return;
  
  if (digitalRead(upBtn) == LOW) {
    if (currentState == MENU) {
      selectedMenu--;
      if (selectedMenu < 0) selectedMenu = 4;
      drawMenu();
    }
    else if (currentState == URGENT_VIEW) {
      urgent.currentIndex--;
      if (urgent.currentIndex < 0) urgent.currentIndex = urgent.count - 1;
      showCategoryView(urgent, "URGENT NOTICE");
    }
    else if (currentState == GENERAL_VIEW) {
      general.currentIndex--;
      if (general.currentIndex < 0) general.currentIndex = general.count - 1;
      showCategoryView(general, "GENERAL NOTICE");
    }
    else if (currentState == EVENT_VIEW) {
      event.currentIndex--;
      if (event.currentIndex < 0) event.currentIndex = event.count - 1;
      showCategoryView(event, "EVENT NOTICE");
    }
    else if (currentState == OFFICE_VIEW) {
      office.currentIndex--;
      if (office.currentIndex < 0) office.currentIndex = office.count - 1;
      showCategoryView(office, "OFFICE NOTICE");
    }
    else if (currentState == TIMETABLE_SESSION_SELECT) {
      selectedSessionIndex--;
      if (selectedSessionIndex < 0) selectedSessionIndex = totalSessions - 1;
      drawSessionSelection();
    }
    else if (currentState == TIMETABLE_SECTION_SELECT) {
      timetableSelectedSection--;
      if (timetableSelectedSection < 0) timetableSelectedSection = 2;
      drawSectionSelection();
    }
    else if (currentState == TIMETABLE_VIEW_SELECT) {
      timetableViewType--;
      if (timetableViewType < 0) timetableViewType = 1;
      drawViewTypeSelection();
    }
    else if (currentState == TIMETABLE_DISPLAY) {
      TimeTable &tt = timeTables[timetableSelectedSession][timetableSelectedSection];
      timetableStartIndex--;
      if (timetableStartIndex < 0) timetableStartIndex = 0;
      displayTimetable();
    }
    lastPress = millis();
  }
  else if (digitalRead(downBtn) == LOW) {
    if (currentState == MENU) {
      selectedMenu++;
      if (selectedMenu > 4) selectedMenu = 0;
      drawMenu();
    }
    else if (currentState == URGENT_VIEW) {
      urgent.currentIndex++;
      if (urgent.currentIndex >= urgent.count) urgent.currentIndex = 0;
      showCategoryView(urgent, "URGENT NOTICE");
    }
    else if (currentState == GENERAL_VIEW) {
      general.currentIndex++;
      if (general.currentIndex >= general.count) general.currentIndex = 0;
      showCategoryView(general, "GENERAL NOTICE");
    }
    else if (currentState == EVENT_VIEW) {
      event.currentIndex++;
      if (event.currentIndex >= event.count) event.currentIndex = 0;
      showCategoryView(event, "EVENT NOTICE");
    }
    else if (currentState == OFFICE_VIEW) {
      office.currentIndex++;
      if (office.currentIndex >= office.count) office.currentIndex = 0;
      showCategoryView(office, "OFFICE NOTICE");
    }
    else if (currentState == TIMETABLE_SESSION_SELECT) {
      selectedSessionIndex++;
      if (selectedSessionIndex >= totalSessions) selectedSessionIndex = 0;
      drawSessionSelection();
    }
    else if (currentState == TIMETABLE_SECTION_SELECT) {
      timetableSelectedSection++;
      if (timetableSelectedSection > 2) timetableSelectedSection = 0;
      drawSectionSelection();
    }
    else if (currentState == TIMETABLE_VIEW_SELECT) {
      timetableViewType++;
      if (timetableViewType > 1) timetableViewType = 0;
      drawViewTypeSelection();
    }
    else if (currentState == TIMETABLE_DISPLAY) {
      TimeTable &tt = timeTables[timetableSelectedSession][timetableSelectedSection];
      timetableStartIndex++;
      if (timetableStartIndex >= tt.count) timetableStartIndex = tt.count - 1;
      displayTimetable();
    }
    lastPress = millis();
  }
  else if (digitalRead(enterBtn) == LOW) {
    if (currentState == MENU) {
      if (selectedMenu == 0) {
        currentState = URGENT_VIEW;
        showCategoryView(urgent, "URGENT NOTICE");
      }
      else if (selectedMenu == 1) {
        currentState = GENERAL_VIEW;
        showCategoryView(general, "GENERAL NOTICE");
      }
      else if (selectedMenu == 2) {
        currentState = EVENT_VIEW;
        showCategoryView(event, "EVENT NOTICE");
      }
      else if (selectedMenu == 3) {
        currentState = OFFICE_VIEW;
        showCategoryView(office, "OFFICE NOTICE");
      }
      else if (selectedMenu == 4) {
        timetableSelectedSession = 0;
        timetableSelectedSection = 0;
        timetableViewType = 0;
        currentState = TIMETABLE_SESSION_SELECT;
        drawSessionSelection();
      }
    }
    else if (currentState == TIMETABLE_SESSION_SELECT) {
      // Get selected session and fetch its sections
      selectedSessionIndex = timetableSelectedSession;
      int sessionId = availableSessions[selectedSessionIndex].session_id;
      fetchSectionsForSession(sessionId);
      
      currentState = TIMETABLE_SECTION_SELECT;
      timetableSelectedSection = 0;
      drawSectionSelection();
    }
    else if (currentState == TIMETABLE_SECTION_SELECT) {
      currentState = TIMETABLE_VIEW_SELECT;
      timetableViewType = 0;
      drawViewTypeSelection();
    }
    else if (currentState == TIMETABLE_VIEW_SELECT) {
      currentState = TIMETABLE_DISPLAY;
      timetableStartIndex = 0;
      displayTimetable();
    }
    else if (currentState != MENU) {
      currentState = MENU;
      selectedMenu = 0;
      drawMenu();
    }
    lastPress = millis();
  }
}

// ===================================================
// MQTT CONNECT
// ===================================================
void connectMQTT() {
  while (!client.connected()) {
    Serial.print("[MQTT] Connecting...");
    if (client.connect("ESP32Board")) {
      Serial.println(" OK!");
      client.subscribe("department/notices");
      Serial.println("[MQTT] Subscribed to department/notices");
    } else {
      Serial.print(".");
      delay(2000);
    }
  }
}

// ===================================================
// SETUP
// ===================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(upBtn, INPUT_PULLUP);
  pinMode(downBtn, INPUT_PULLUP);
  pinMode(enterBtn, INPUT_PULLUP);
  
  pinMode(buzzer, OUTPUT);
  pinMode(redLED, OUTPUT);
  pinMode(greenLED, OUTPUT);
  pinMode(yellowLED, OUTPUT);
  
  digitalWrite(buzzer, LOW);
  
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
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("[WiFi] Connected!");
  
  client.setServer(mqttServer, mqttPort);
  client.setCallback(onMessage);
  connectMQTT();
  
  initTimeTables();
  fetchSessions();  // Load all available sessions from database
  drawMenu();
  Serial.println("✅ Setup complete!");
}

// ===================================================
// LOOP
// ===================================================
void loop() {
  if (!client.connected()) connectMQTT();
  
  client.loop();
  handleButtons();
  delay(10);
}
