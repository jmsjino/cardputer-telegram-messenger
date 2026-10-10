/*
 * Cardputer Telegram Messenger
 * Sends and receives Telegram messages on an M5Cardputer.
 * Use your own bot token. See README.md.
 */
#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // ArduinoJson v7
#include <Preferences.h>
#include <LittleFS.h>

// --- USER CONFIGURATION ---
// Create your own bot with @BotFather and paste its token here.
const char* BOT_TOKEN = "YOUR_BOT_TOKEN_HERE";

#define MAX_CONTACTS 10
#define MAX_TEXT     256
#define MAX_ID       24
#define MAX_WIFI     5
#define MAX_SCAN     20
#define MAX_ROWS     80
#define VISIBLE_ROWS 9
#define ROW_CHARS    38
#define LIST_ROWS    7
#define WIFI_ROWS    6
const unsigned long pollInterval = 3000;

struct Contact { String name; String chatId; };
struct OutMsg  { char chatId[MAX_ID]; char text[MAX_TEXT]; };
struct Row     { uint8_t type; String text; };   // 0 = me, 1 = them

Contact contacts[MAX_CONTACTS];
int contactCount = 0;
int activeContactIndex = 0;
int listTop = 0;

String wifiSsid[MAX_WIFI];
String wifiPass[MAX_WIFI];
int wifiCount = 0;

String scanSsid[MAX_SCAN];
int scanRssi[MAX_SCAN];
bool scanSecure[MAX_SCAN];
int scanCount = 0;
int scanSel = 0, scanTop = 0;
String pendingSsid = "";

enum AppState { STATE_CHAT, STATE_CONTACTS, STATE_ADD_NAME, STATE_ADD_ID, STATE_WIFI, STATE_WIFI_PASS };
AppState currentState = STATE_CHAT;

String inputMessage = "";
String tempContactName = "";
String statusLine = "";
int scrollOffset = 0;

Row rows[MAX_ROWS];
int rowCount = 0;

SemaphoreHandle_t dataMutex;
QueueHandle_t outQueue;
volatile bool uiDirty = false;
long lastUpdateId = 0;

#define LOCK()   xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(dataMutex)

// ---------- Helpers ----------
String clip(const String& s, int n) {
  return s.length() > n ? s.substring(0, n - 3) + "..." : s;
}

String tailOf(const String& s, int n) {
  return s.length() > n ? s.substring(s.length() - n) : s;
}

bool isValidChatId(const String& id) {
  if (id.length() == 0) return false;
  int start = (id[0] == '-') ? 1 : 0;
  if (start == (int)id.length()) return false;
  for (int i = start; i < (int)id.length(); i++) {
    if (!isDigit(id[i])) return false;
  }
  return true;
}

bool hasChar(const Keyboard_Class::KeysState& st, char target) {
  for (char c : st.word) if (c == target) return true;
  return false;
}

// Caller must hold the lock
int findContact(const String& id) {
  for (int i = 0; i < contactCount; i++) {
    if (contacts[i].chatId == id) return i;
  }
  return -1;
}

int savedWifiIndex(const String& ssid) {
  for (int i = 0; i < wifiCount; i++) {
    if (wifiSsid[i] == ssid) return i;
  }
  return -1;
}

void setStatus(const String& s) {
  LOCK();
  statusLine = s;
  UNLOCK();
  uiDirty = true;
}

// ---------- Storage ----------
// Caller must hold the lock
void saveContacts() {
  Preferences p;
  p.begin("tg_messenger", false);
  p.putInt("count", contactCount);
  for (int i = 0; i < contactCount; i++) {
    p.putString(("name_" + String(i)).c_str(), contacts[i].name);
    p.putString(("id_" + String(i)).c_str(), contacts[i].chatId);
  }
  p.end();
}

void loadContacts() {
  Preferences p;
  p.begin("tg_messenger", true);
  contactCount = p.getInt("count", 0);
  if (contactCount > MAX_CONTACTS) contactCount = MAX_CONTACTS;
  for (int i = 0; i < contactCount; i++) {
    contacts[i].name = p.getString(("name_" + String(i)).c_str(), "Unknown");
    contacts[i].chatId = p.getString(("id_" + String(i)).c_str(), "");
  }
  p.end();
}

void loadState() {
  Preferences p;
  p.begin("tg_state", true);
  lastUpdateId = p.getLong("upd", 0);
  p.end();
}

void saveUpdateId() {
  Preferences p;
  p.begin("tg_state", false);
  p.putLong("upd", lastUpdateId);
  p.end();
}

void loadWifi() {
  Preferences p;
  p.begin("tg_wifi", true);
  wifiCount = p.getInt("count", 0);
  if (wifiCount > MAX_WIFI) wifiCount = MAX_WIFI;
  for (int i = 0; i < wifiCount; i++) {
    wifiSsid[i] = p.getString(("s" + String(i)).c_str(), "");
    wifiPass[i] = p.getString(("p" + String(i)).c_str(), "");
  }
  p.end();
}

// Caller must hold the lock
void saveWifi() {
  Preferences p;
  p.begin("tg_wifi", false);
  p.clear();
  p.putInt("count", wifiCount);
  for (int i = 0; i < wifiCount; i++) {
    p.putString(("s" + String(i)).c_str(), wifiSsid[i]);
    p.putString(("p" + String(i)).c_str(), wifiPass[i]);
  }
  p.end();
}

// Caller must hold the lock
void removeSavedWifi(int n) {
  for (int i = n; i < wifiCount - 1; i++) {
    wifiSsid[i] = wifiSsid[i + 1];
    wifiPass[i] = wifiPass[i + 1];
  }
  wifiCount--;
  saveWifi();
}

// ---------- Chat history (LittleFS, one file per contact) ----------
String chatPath(const String& id) { return "/c" + id + ".txt"; }

// Caller must hold the lock. Each line is "M|text" (me) or "T|text" (them)
void appendLine(const String& id, char type, const String& text) {
  String clean = text;
  clean.replace('\n', ' ');
  clean.replace('\r', ' ');
  File f = LittleFS.open(chatPath(id), FILE_APPEND);
  if (!f) { Serial.println("History write failed"); return; }
  f.print(type);
  f.print('|');
  f.println(clean);
  f.close();
}

// Caller must hold the lock
void addWrapped(uint8_t type, const String& prefix, const String& body) {
  String full = prefix + body;
  for (int i = 0; i < (int)full.length(); i += ROW_CHARS) {
    if (rowCount == MAX_ROWS) {
      for (int j = 1; j < MAX_ROWS; j++) rows[j - 1] = rows[j];
      rowCount--;
    }
    rows[rowCount].type = type;
    rows[rowCount].text = full.substring(i, i + ROW_CHARS);
    rowCount++;
  }
}

// Caller must hold the lock. Loads the saved lines for one contact
void loadRows(const String& id) {
  rowCount = 0;
  File f = LittleFS.open(chatPath(id), FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 2) continue;
    String body = line.substring(2);
    if (line[0] == 'M') addWrapped(0, "Me: ", body);
    else                addWrapped(1, "Them: ", body);
  }
  f.close();
}

void deleteContact(int i) {
  LOCK();
  String id = contacts[i].chatId;
  for (int j = i; j < contactCount - 1; j++) contacts[j] = contacts[j + 1];
  contactCount--;
  saveContacts();
  UNLOCK();
  LittleFS.remove(chatPath(id));
  if (activeContactIndex >= contactCount) activeContactIndex = max(0, contactCount - 1);
  setStatus("Contact deleted");
}

// ---------- Network (background task, never blocks the UI) ----------
void doSend(const char* chatId, const char* text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("SEND SKIPPED: no WiFi");
    setStatus("No WiFi, not sent");
    return;
  }
  Serial.printf("SENDING to chat_id=%s\n", chatId);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);

  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
  String body;
  serializeJson(doc, body);

  http.begin(client, "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  Serial.printf("SEND result code=%d\n", code);

  if (code == 200) {
    LOCK();
    appendLine(chatId, 'M', text);
    UNLOCK();
    setStatus("Sent");
  } else {
    String resp = code > 0 ? http.getString() : HTTPClient::errorToString(code);
    Serial.println(resp);
    setStatus("Send failed: " + String(code));
  }
  http.end();
}

void pollIncoming() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN)
             + "/getUpdates?timeout=0&offset=" + String(lastUpdateId + 1);
  http.begin(client, url);
  int code = http.GET();
  if (code != 200) { http.end(); return; }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  if (!(doc["ok"] | false)) return;

  JsonArray results = doc["result"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return;

  bool changed = false;
  bool addedContact = false;

  LOCK();
  for (JsonObject upd : results) {
    long uid = upd["update_id"] | 0L;
    if (uid > lastUpdateId) lastUpdateId = uid;

    JsonObject msg = upd["message"];
    if (msg.isNull()) continue;

    String text = msg["text"] | "[non-text message]";
    long long rawId = msg["chat"]["id"] | 0LL;
    char idBuf[MAX_ID];
    snprintf(idBuf, sizeof(idBuf), "%lld", rawId);
    String senderId = idBuf;

    int idx = findContact(senderId);
    if (idx < 0 && contactCount < MAX_CONTACTS) {
      // Unknown person messaged the bot: add them as a contact
      contacts[contactCount].name = "Chat " + tailOf(senderId, 4);
      contacts[contactCount].chatId = senderId;
      idx = contactCount;
      contactCount++;
      addedContact = true;
    }
    if (idx >= 0) {
      appendLine(senderId, 'T', text);
      changed = true;
    }
  }
  if (addedContact) saveContacts();
  UNLOCK();

  saveUpdateId();
  if (changed) uiDirty = true;
}

void netTask(void*) {
  unsigned long lastPoll = 0;
  OutMsg m;
  for (;;) {
    while (xQueueReceive(outQueue, &m, 0) == pdTRUE) {
      doSend(m.chatId, m.text);
    }
    if (millis() - lastPoll > pollInterval) {
      lastPoll = millis();
      pollIncoming();
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ---------- Wi-Fi ----------
bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

// Tries to connect and, on success, saves the network. Caller must not hold the lock
bool connectTo(const String& ssid, const String& pass, unsigned long timeoutMs) {
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < timeoutMs) delay(200);

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    return false;
  }
  LOCK();
  int idx = savedWifiIndex(ssid);
  if (idx < 0 && wifiCount < MAX_WIFI) {
    idx = wifiCount++;
    wifiSsid[idx] = ssid;
  }
  if (idx >= 0) {
    wifiPass[idx] = pass;
    saveWifi();
  }
  UNLOCK();
  return true;
}

void wifiBegin() {
  loadWifi();
  if (wifiCount == 0) {
    statusLine = "No WiFi. G0 to set up";
    return;
  }
  for (int i = 0; i < wifiCount; i++) {
    statusLine = "Connecting: " + clip(wifiSsid[i], 16);
    M5Cardputer.Display.println(statusLine);
    if (connectTo(wifiSsid[i], wifiPass[i], 10000)) {
      statusLine = "Connected: " + WiFi.localIP().toString();
      return;
    }
  }
  statusLine = "WiFi failed. G0 to set up";
}

void wifiTick() {
  static unsigned long lastCheck = 0;
  if (wifiCount > 0 && !wifiConnected() && millis() - lastCheck > 15000) {
    lastCheck = millis();
    WiFi.reconnect();
  }
}

void doScan() {
  statusLine = "Scanning...";
  fullRedraw();
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();

  LOCK();
  scanCount = 0;
  for (int i = 0; i < n && scanCount < MAX_SCAN; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < scanCount; j++) if (scanSsid[j] == s) dup = true;
    if (dup) continue;
    scanSsid[scanCount] = s;
    scanRssi[scanCount] = WiFi.RSSI(i);
    scanSecure[scanCount] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    scanCount++;
  }
  WiFi.scanDelete();
  scanSel = 0;
  scanTop = 0;
  statusLine = scanCount > 0 ? "Found " + String(scanCount) + " networks" : "No networks found";
  UNLOCK();
}

// ---------- Drawing ----------
void drawInputLine() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 108, 240, 12, BLACK);
  d.setCursor(0, 110);
  d.setTextColor(YELLOW);
  d.print(tailOf(inputMessage, ROW_CHARS));
}

void drawChat() {
  auto& d = M5Cardputer.Display;
  d.setTextColor(CYAN);
  if (contactCount == 0) d.println("No contacts. Press G0 then a");
  else                   d.println("To: " + clip(contacts[activeContactIndex].name, 30));
  d.drawLine(0, 12, 240, 12, CYAN);

  rowCount = 0;
  if (contactCount > 0) loadRows(contacts[activeContactIndex].chatId);

  int maxScroll = max(0, rowCount - VISIBLE_ROWS);
  if (scrollOffset > maxScroll) scrollOffset = maxScroll;
  if (scrollOffset < 0) scrollOffset = 0;
  int bottom = rowCount - scrollOffset;
  int top = max(0, bottom - VISIBLE_ROWS);

  if (contactCount > 0 && rowCount == 0) {
    d.setCursor(0, 16);
    d.setTextColor(DARKGREY);
    d.print("No messages yet");
  }
  for (int i = top; i < bottom; i++) {
    d.setCursor(0, 16 + (i - top) * 9);
    if (rows[i].type == 0) d.setTextColor(CYAN);
    else                   d.setTextColor(WHITE);
    d.print(rows[i].text);
  }
  if (scrollOffset > 0) {
    d.setCursor(200, 0);
    d.setTextColor(ORANGE);
    d.print("^" + String(scrollOffset));
  }

  d.setCursor(0, 99);
  d.setTextColor(GREEN);
  d.print(clip(statusLine, 40));
  d.setCursor(0, 124);
  d.setTextColor(DARKGREY);
  d.print("Enter send  G0 menu  Fn+;/. scroll");
}

void drawContacts() {
  auto& d = M5Cardputer.Display;
  d.setTextColor(GREEN);
  d.println("=== Contacts ===");
  d.drawLine(0, 12, 240, 12, GREEN);
  if (contactCount == 0) {
    d.setCursor(0, 20);
    d.setTextColor(WHITE);
    d.println("No contacts yet. Press a");
  }
  for (int i = listTop; i < contactCount && i < listTop + LIST_ROWS; i++) {
    d.setCursor(0, 18 + (i - listTop) * 13);
    if (i == activeContactIndex) { d.setTextColor(YELLOW); d.print("> "); }
    else                         { d.setTextColor(WHITE);  d.print("  "); }
    d.print(clip(contacts[i].name, 16) + "  " + contacts[i].chatId);
  }
  d.setCursor(0, 99);
  d.setTextColor(GREEN);
  d.print(clip(statusLine, 40));
  d.setCursor(0, 124);
  d.setTextColor(DARKGREY);
  d.print("; . move  Enter chat  a add  x del");
}

void drawAddName() {
  auto& d = M5Cardputer.Display;
  d.setTextColor(ORANGE);
  d.println("=== Add Contact (1/2) ===");
  d.drawLine(0, 12, 240, 12, ORANGE);
  d.setCursor(0, 30);
  d.setTextColor(WHITE);
  d.println("Enter contact name:");
  d.setTextColor(YELLOW);
  d.println(tailOf(inputMessage, 36));
  d.setCursor(0, 99);
  d.setTextColor(GREEN);
  d.print(clip(statusLine, 40));
  d.setCursor(0, 124);
  d.setTextColor(DARKGREY);
  d.print("Enter next  G0 back");
}

void drawAddId() {
  auto& d = M5Cardputer.Display;
  d.setTextColor(ORANGE);
  d.println("=== Add Contact (2/2) ===");
  d.drawLine(0, 12, 240, 12, ORANGE);
  d.setCursor(0, 20);
  d.setTextColor(WHITE);
  d.print("Name: ");
  d.println(tempContactName);
  d.setCursor(0, 40);
  d.println("Enter Telegram chat ID:");
  d.setTextColor(YELLOW);
  d.println(tailOf(inputMessage, 36));
  d.setCursor(0, 99);
  d.setTextColor(GREEN);
  d.print(clip(statusLine, 40));
  d.setCursor(0, 124);
  d.setTextColor(DARKGREY);
  d.print("Enter save  G0 back");
}

void drawWifi() {
  auto& d = M5Cardputer.Display;
  d.setTextColor(MAGENTA);
  d.println("=== WiFi ===");
  d.drawLine(0, 12, 240, 12, MAGENTA);
  d.setCursor(0, 16);
  d.setTextColor(WHITE);
  if (wifiConnected()) d.print("On: " + clip(WiFi.SSID(), 18));
  else                 d.print("Not connected");

  if (scanCount == 0) {
    d.setCursor(0, 32);
    d.setTextColor(DARKGREY);
    d.print("No networks. Press r to scan");
  }
  for (int i = scanTop; i < scanCount && i < scanTop + WIFI_ROWS; i++) {
    d.setCursor(0, 32 + (i - scanTop) * 12);
    if (i == scanSel) { d.setTextColor(YELLOW); d.print("> "); }
    else              { d.setTextColor(WHITE);  d.print("  "); }
    d.print(clip(scanSsid[i], 18) + " " + String(scanRssi[i])
            + (scanSecure[i] ? " L" : " O")
            + (savedWifiIndex(scanSsid[i]) >= 0 ? " *" : ""));
  }
  d.setCursor(0, 99);
  d.setTextColor(GREEN);
  d.print(clip(statusLine, 40));
  d.setCursor(0, 124);
  d.setTextColor(DARKGREY);
  d.print("; . move Enter join r scan x forget");
}

void drawWifiPass() {
  auto& d = M5Cardputer.Display;
  d.setTextColor(ORANGE);
  d.println("=== WiFi Password ===");
  d.drawLine(0, 12, 240, 12, ORANGE);
  d.setCursor(0, 20);
  d.setTextColor(WHITE);
  d.print("Network: " + clip(pendingSsid, 22));
  d.setCursor(0, 40);
  d.println("Password:");
  String masked = "";
  for (int i = 0; i < (int)inputMessage.length(); i++) masked += '*';
  d.setTextColor(YELLOW);
  d.println(tailOf(masked, 36));
  d.setCursor(0, 99);
  d.setTextColor(GREEN);
  d.print(clip(statusLine, 40));
  d.setCursor(0, 124);
  d.setTextColor(DARKGREY);
  d.print("Enter connect  G0 back");
}

void fullRedraw() {
  LOCK();
  auto& d = M5Cardputer.Display;
  d.fillScreen(BLACK);
  d.setCursor(0, 0);
  d.setTextSize(1);
  switch (currentState) {
    case STATE_CHAT:      drawChat();     break;
    case STATE_CONTACTS:  drawContacts(); break;
    case STATE_ADD_NAME:  drawAddName();  break;
    case STATE_ADD_ID:    drawAddId();    break;
    case STATE_WIFI:      drawWifi();     break;
    case STATE_WIFI_PASS: drawWifiPass(); break;
  }
  UNLOCK();
  if (currentState == STATE_CHAT) drawInputLine();
}

// ---------- Keyboard ----------
void appendChars(const Keyboard_Class::KeysState& st) {
  for (char c : st.word) {
    if (inputMessage.length() >= MAX_TEXT - 1) break;
    if (st.shift && c >= 'a' && c <= 'z') c = c - 32;
    inputMessage += c;
  }
}

void sendFromInput() {
  if (inputMessage.length() == 0) return;

  LOCK();
  if (contactCount == 0) {
    UNLOCK();
    setStatus("Add a contact first");
    return;
  }
  OutMsg m{};
  strncpy(m.chatId, contacts[activeContactIndex].chatId.c_str(), sizeof(m.chatId) - 1);
  strncpy(m.text, inputMessage.c_str(), sizeof(m.text) - 1);
  UNLOCK();

  if (xQueueSend(outQueue, &m, 0) == pdTRUE) {
    inputMessage = "";
    scrollOffset = 0;
    setStatus("Sending...");
  } else {
    setStatus("Queue full, try again");
  }
}

void goTo(AppState s) {
  currentState = s;
  inputMessage = "";
  statusLine = "";
}

void handleKeys() {
  auto st = M5Cardputer.Keyboard.keysState();

  switch (currentState) {

    case STATE_CHAT:
      if (st.fn) {
        if (hasChar(st, ';')) scrollOffset++;
        else if (hasChar(st, '.') && scrollOffset > 0) scrollOffset--;
        fullRedraw();
      } else if (st.enter) {
        sendFromInput();
        fullRedraw();
      } else if (st.del) {
        if (inputMessage.length() > 0) inputMessage.remove(inputMessage.length() - 1);
        drawInputLine();
      } else {
        appendChars(st);
        drawInputLine();
      }
      break;

    case STATE_CONTACTS:
      if (hasChar(st, ';') && activeContactIndex > 0) {
        activeContactIndex--;
      } else if (hasChar(st, '.') && activeContactIndex < contactCount - 1) {
        activeContactIndex++;
      } else if (hasChar(st, 'a')) {
        tempContactName = "";
        goTo(STATE_ADD_NAME);
      } else if (hasChar(st, 'x') && contactCount > 0) {
        deleteContact(activeContactIndex);
      } else if (st.enter && contactCount > 0) {
        scrollOffset = 0;
        goTo(STATE_CHAT);
      }
      if (activeContactIndex < listTop) listTop = activeContactIndex;
      if (activeContactIndex >= listTop + LIST_ROWS) listTop = activeContactIndex - LIST_ROWS + 1;
      fullRedraw();
      break;

    case STATE_ADD_NAME:
      if (st.enter) {
        if (inputMessage.length() > 0) {
          tempContactName = inputMessage;
          inputMessage = "";
          statusLine = "";
          currentState = STATE_ADD_ID;
        }
      } else if (st.del) {
        if (inputMessage.length() > 0) inputMessage.remove(inputMessage.length() - 1);
      } else {
        appendChars(st);
      }
      fullRedraw();
      break;

    case STATE_ADD_ID:
      if (st.enter) {
        String id = inputMessage;
        id.trim();
        LOCK();
        if (!isValidChatId(id)) {
          statusLine = "Invalid ID: numbers only";
        } else if (findContact(id) >= 0) {
          statusLine = "That ID is already saved";
        } else if (contactCount >= MAX_CONTACTS) {
          statusLine = "Contact list is full";
        } else {
          contacts[contactCount].name = tempContactName;
          contacts[contactCount].chatId = id;
          activeContactIndex = contactCount;
          contactCount++;
          saveContacts();
          inputMessage = "";
          statusLine = "Contact saved";
          scrollOffset = 0;
          listTop = max(0, activeContactIndex - LIST_ROWS + 1);
          currentState = STATE_CHAT;
        }
        UNLOCK();
      } else if (st.del) {
        if (inputMessage.length() > 0) inputMessage.remove(inputMessage.length() - 1);
      } else {
        appendChars(st);
      }
      fullRedraw();
      break;

    case STATE_WIFI:
      if (hasChar(st, ';') && scanSel > 0) {
        scanSel--;
      } else if (hasChar(st, '.') && scanSel < scanCount - 1) {
        scanSel++;
      } else if (hasChar(st, 'r')) {
        doScan();
      } else if (hasChar(st, 'x') && scanCount > 0) {
        LOCK();
        int idx = savedWifiIndex(scanSsid[scanSel]);
        if (idx >= 0) {
          removeSavedWifi(idx);
          statusLine = "Forgot network";
        } else {
          statusLine = "Not saved";
        }
        UNLOCK();
      } else if (st.enter && scanCount > 0) {
        String ssid = scanSsid[scanSel];
        LOCK();
        int idx = savedWifiIndex(ssid);
        String savedPass = idx >= 0 ? wifiPass[idx] : "";
        UNLOCK();
        if (idx >= 0) {
          statusLine = "Connecting...";
          fullRedraw();
          statusLine = connectTo(ssid, savedPass, 15000) ? "Connected: " + WiFi.localIP().toString()
                                                         : "Failed. Check password";
        } else if (!scanSecure[scanSel]) {
          statusLine = "Connecting...";
          fullRedraw();
          statusLine = connectTo(ssid, "", 15000) ? "Connected: " + WiFi.localIP().toString()
                                                  : "Failed to connect";
        } else {
          pendingSsid = ssid;
          goTo(STATE_WIFI_PASS);
        }
      }
      if (scanSel < scanTop) scanTop = scanSel;
      if (scanSel >= scanTop + WIFI_ROWS) scanTop = scanSel - WIFI_ROWS + 1;
      fullRedraw();
      break;

    case STATE_WIFI_PASS:
      if (st.enter) {
        String pass = inputMessage;
        inputMessage = "";
        statusLine = "Connecting...";
        fullRedraw();
        statusLine = connectTo(pendingSsid, pass, 15000) ? "Connected: " + WiFi.localIP().toString()
                                                         : "Failed. Check password";
        currentState = STATE_WIFI;
      } else if (st.del) {
        if (inputMessage.length() > 0) inputMessage.remove(inputMessage.length() - 1);
      } else {
        appendChars(st);
      }
      fullRedraw();
      break;
  }
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("BOOT");

  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(GREEN);

  dataMutex = xSemaphoreCreateRecursiveMutex();
  outQueue  = xQueueCreate(8, sizeof(OutMsg));

  if (!LittleFS.begin(true)) Serial.println("LittleFS FAILED");

  loadContacts();
  loadState();
  Serial.printf("Loaded %d contacts, last update %ld\n", contactCount, lastUpdateId);

  wifiBegin();

  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);
  currentState = STATE_CHAT;
  fullRedraw();
}

void loop() {
  M5Cardputer.update();
  wifiTick();

  // G0 cycles: Chat -> Contacts -> WiFi -> Chat
  if (M5Cardputer.BtnA.wasPressed()) {
    switch (currentState) {
      case STATE_CHAT:      goTo(STATE_CONTACTS); break;
      case STATE_CONTACTS:  goTo(STATE_WIFI); doScan(); break;
      case STATE_WIFI:      goTo(STATE_CHAT); scrollOffset = 0; break;
      case STATE_WIFI_PASS: goTo(STATE_WIFI); break;
      case STATE_ADD_NAME:
      case STATE_ADD_ID:    goTo(STATE_CONTACTS); break;
    }
    fullRedraw();
  }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    handleKeys();
  }

  if (uiDirty) {
    uiDirty = false;
    fullRedraw();
  }

  delay(10);
}