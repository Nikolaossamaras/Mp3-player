#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Audio.h>   // ESP32-audioI2S

// ---------- OLED ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Pins ----------
const int POT_PIN = 34;
const int PREVIOUS_BUTTON = 32;
const int PLAY_PAUSE_BUTTON = 33;
const int NEXT_BUTTON = 16;

#define SD_CS     5     // SD uses default SPI: SCK 18, MISO 19, MOSI 23
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  27

// ---------- Audio ----------
Audio audio;
std::vector<String> songs;
int currentSong = 0;

// Shared between the audio task and the display (protected by mutex)
SemaphoreHandle_t dataMutex;
String songTitle = "No song";
String songArtist = "";

volatile int  cmdChange = 0;      // +1 next, -1 previous
volatile bool cmdPause = false;
volatile bool isPaused = false;
volatile int  targetVolume = 10;  // 0-21

// ---------- Helpers ----------
String cleanText(String s) {
  // OLED font is ASCII only
  for (size_t i = 0; i < s.length(); i++) {
    if ((uint8_t)s[i] < 32 || (uint8_t)s[i] > 126) s[i] = '?';
  }
  return s;
}

void setTitle(const String &t, const String &a) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  songTitle = cleanText(t);
  songArtist = cleanText(a);
  xSemaphoreGive(dataMutex);
}

void scanSongs() {
  File root = SD.open("/");
  while (true) {
    File f = root.openNextFile();
    if (!f) break;
    String name = f.name();
    if (name.startsWith("/")) name = name.substring(1);
    if (!f.isDirectory() && !name.startsWith(".") && name.endsWith(".mp3")) {
      songs.push_back("/" + name);
    }
    f.close();
  }
  root.close();
  Serial.printf("Found %d songs\n", (int)songs.size());
}

void startSong(int index) {
  if (songs.empty()) return;
  currentSong = (index + songs.size()) % songs.size();
  String path = songs[currentSong];
  String name = path.substring(1, path.length() - 4); // fallback title = file name
  setTitle(name, "");
  isPaused = false;
  audio.connecttoFS(SD, path.c_str());
  Serial.println("Playing: " + path);
}

// Library callbacks (called from audio.loop)
void audio_id3data(const char *info) {
  String s(info);
  Serial.println(s);
  if (s.startsWith("Title: ")) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    songTitle = cleanText(s.substring(7));
    xSemaphoreGive(dataMutex);
  } else if (s.startsWith("Artist: ")) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    songArtist = cleanText(s.substring(8));
    xSemaphoreGive(dataMutex);
  }
}

void audio_eof_mp3(const char *info) {
  cmdChange = 1; // song finished -> next
}

// Audio runs in its own task so the OLED never makes it stutter
void audioTask(void *) {
  int lastVol = -1;
  for (;;) {
    if (cmdChange != 0) {
      int c = cmdChange;
      cmdChange = 0;
      startSong(currentSong + c);
    }
    if (cmdPause) {
      cmdPause = false;
      audio.pauseResume();
      isPaused = !isPaused;
    }
    if (targetVolume != lastVol) {
      lastVol = targetVolume;
      audio.setVolume(lastVol);
    }
    audio.loop();
    vTaskDelay(1);
  }
}

// ---------- Buttons ----------
bool prevLast = HIGH, ppLast = HIGH, nextLast = HIGH;
unsigned long lastPress = 0;

// ---------- Display ----------
unsigned long lastDisplayUpdate = 0;

void drawScrolling(const String &text, int y) {
  const int charW = 6;
  const int maxChars = SCREEN_WIDTH / charW; // 21
  if ((int)text.length() <= maxChars) {
    display.setCursor(0, y);
    display.print(text);
    return;
  }
  String padded = text + "    ";
  int totalPx = padded.length() * charW;
  int offset = (millis() / 40) % totalPx;
  display.setCursor(-offset, y);
  display.print(padded);
  display.setCursor(-offset + totalPx, y);
  display.print(padded);
}

void drawScreen() {
  String title, artist;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  title = songTitle;
  artist = songArtist;
  xSemaphoreGive(dataMutex);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextWrap(false);

  display.setCursor(0, 0);
  display.print(isPaused ? "|| Paused" : "> Playing");
  display.setCursor(84, 0);
  display.printf("%d/%d", songs.empty() ? 0 : currentSong + 1, (int)songs.size());
  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  drawScrolling(title, 16);
  drawScrolling(artist, 28);

  int pct = map(targetVolume, 0, 21, 0, 100);
  display.setCursor(0, 42);
  display.printf("Vol %d%%", pct);
  display.drawRect(0, 54, 128, 10, SSD1306_WHITE);
  display.fillRect(2, 56, map(targetVolume, 0, 21, 0, 124), 6, SSD1306_WHITE);

  display.display();
}

void setup() {
  Serial.begin(115200);
  dataMutex = xSemaphoreCreateMutex();

  pinMode(PREVIOUS_BUTTON, INPUT_PULLUP);
  pinMode(PLAY_PAUSE_BUTTON, INPUT_PULLUP);
  pinMode(NEXT_BUTTON, INPUT_PULLUP);
  analogReadResolution(12);

  Wire.begin(21, 22);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED initialization failed!");
    while (true) delay(100);
  }

  if (!SD.begin(SD_CS)) {
    setTitle("SD card error", "");
    drawScreen();
    Serial.println("SD init failed");
    while (true) delay(100);
  }
  scanSongs();
  if (songs.empty()) {
    setTitle("No mp3 files", "");
    drawScreen();
    while (true) delay(100);
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(targetVolume);
  startSong(0);

  xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 3, NULL, 0);
}

void loop() {
  // Volume from pot (smoothed)
  static float smooth = analogRead(POT_PIN);
  smooth = smooth * 0.9f + analogRead(POT_PIN) * 0.1f;
  targetVolume = constrain(map((int)smooth, 0, 4095, 0, 21), 0, 21);

  // Buttons (with 200 ms lockout)
  bool prevNow = digitalRead(PREVIOUS_BUTTON);
  bool ppNow = digitalRead(PLAY_PAUSE_BUTTON);
  bool nextNow = digitalRead(NEXT_BUTTON);

  if (millis() - lastPress > 200) {
    if (prevLast == HIGH && prevNow == LOW)  { cmdChange = -1; lastPress = millis(); }
    if (ppLast == HIGH && ppNow == LOW)      { cmdPause = true; lastPress = millis(); }
    if (nextLast == HIGH && nextNow == LOW)  { cmdChange = 1;  lastPress = millis(); }
  }
  prevLast = prevNow;
  ppLast = ppNow;
  nextLast = nextNow;

  if (millis() - lastDisplayUpdate >= 50) {
    lastDisplayUpdate = millis();
    drawScreen();
  }
}