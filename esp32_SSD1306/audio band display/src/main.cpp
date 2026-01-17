
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include <WiFi.h>
#include <Wire.h>
#include <arduinoFFT.h>
#include <time.h>

/* ================= 硬件与定义 ================= */
#define SCREEN_WIDTH 128 // SSD1306 屏幕宽度
#define SCREEN_HEIGHT 64 // SSD1306 屏幕高度
#define HEADER_H 10      // 顶部文字高度

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/* ================= 新增功能配置区 ================= */
const char *ssid = "MYWIFI";       // WiFi SSID
const char *password = "12222222"; // WiFi 密码
bool wifiFeatureEnabled = true;    // WiFi功能总开关

const int idleThreshold = 36;            // 进入闲置的分贝阈值
const int wakeupThreshold = 40;          // 唤醒频谱的分贝阈值
const unsigned long idleDelay = 8000;    // 闲置判定时间 (ms)
const unsigned long showMsg = 500;       // 显示消息的时间 (ms)
const int wifiTimeout = 5;               // WiFi连接超时 (s)
const char *ntpServer = "162.159.200.1"; // NTP服务器
// FFT 相关
#define SAMPLES 128        // FFT采样点数 必须为2的幂
#define SAMPLING_FREQ 4000 // 采样频率 (Hz)
#define BAND_NUM 16        // 频段数量
#define BLOCK_HIGHT 5      // 垂直方块高度

#define noiseFloor 60  // 噪声抑制 越大抑制程度越高
#define dbMult 6.0     // 放大倍数 越大越灵敏
#define peakFall 2.0   // 峰值线下落速度 越大下落越快
#define smoothUp 0.9   // 上升平滑系数 0~1 越大上升响应越快
#define smoothDown 0.3 // 下降平滑系数 0~1 越大下降响应越快

/* ================= 全局变量 ================= */
float vReal[SAMPLES];      // FFT 输入数组
float vImag[SAMPLES];      // FFT 输出数组
float bandDb[BAND_NUM];    // 当前显示的频谱数据
float oldBandDb[BAND_NUM]; // 上次显示的频谱数据
float peakDb[BAND_NUM];    // 频谱顶点数据
ArduinoFFT<float> FFT(vReal, vImag, SAMPLES, SAMPLING_FREQ);

int bin_indices[17] = { // 频段对应的 FFT bin 索引
    2, 3, 4, 5, 7, 9, 11, 13, 16, 19, 23, 28, 34, 41, 49, 58, 64};

// 统计与时间
float maxDb = 0;   // 过去一段时间最大分贝值
float maxFreq = 0; // 过去一段时间最大分贝值对应频率值
uint16_t peakTimeInterval = 500; // 峰值更新周期 (ms)

unsigned long lastPeakUpdate = 0;
float delayMs = 1000000 / SAMPLING_FREQ;

enum SystemMode { MODE_SPECTRUM, MODE_IDLE_TIME };
SystemMode currentMode = MODE_SPECTRUM;

unsigned long lowVolumeStartTime = 0; // 记录持续低音量的开始时间
bool isTimeSynced = false;            // 时间是否已同步过
bool isWifiConnecting = false;        // WiFi 连接中标志

/* ================= 工具函数 ================= */

// 居中显示文字
void displayCenterText(String text, int textSize, int yOffset = 0) {
  display.clearDisplay();
  display.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2 + yOffset);
  display.print(text);
  display.display();
}

unsigned long startAttempt = 0;
// 同步时间逻辑
bool syncTime() {

  if (startAttempt == 0) {
    Serial.println("正在连接 Wi-Fi...");
    isWifiConnecting = true;
    WiFi.begin(ssid, password);
    startAttempt = millis();
  }

  if (WiFi.status() != WL_CONNECTED && millis() - startAttempt < wifiTimeout * 1000) {
    Serial.print(".");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED && millis() - startAttempt >= wifiTimeout * 1000) {
    Serial.println("\nWi-Fi 连接失败");
    displayCenterText("Wi-Fi error", 1);
    delay(showMsg);
    WiFi.disconnect(true);
    isWifiConnecting = false;
    wifiFeatureEnabled = false; // 连接失败后禁用 WiFi 功能
    return false;
  }

  Serial.println("\nWi-Fi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  displayCenterText(WiFi.localIP().toString(), 1);
  delay(showMsg);

  Serial.println("正在同步时间...");
  configTime(8 * 3600, 0, ntpServer); // 设置东八区

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("同步时间失败");
    displayCenterText("async time error", 1);
    delay(showMsg);
    WiFi.disconnect(true);
    isWifiConnecting = false;
    wifiFeatureEnabled = false; // 连接失败后禁用 WiFi 功能
    return false;
  }

  Serial.println("时间同步成功");
  isTimeSynced = true;
  isWifiConnecting = false;
  WiFi.disconnect(true); // 同步完关闭WiFi省电
  return true;
}

// 显示大字体时间
void updateTimeDisplay() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return;
  }

  display.clearDisplay();
  display.setFont(&FreeSans9pt7b); // 使用新字体

  char buf_hm[6]; // HH:mm
  char buf_s[3];  // ss
  sprintf(buf_hm, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  sprintf(buf_s, "%02d", timeinfo.tm_sec);

  // 计算布局
  // Size 2 的字符宽度约 12px，HH:mm 宽约 60px
  // Size 1 的字符宽度约 6px，ss 宽约 12px
  int hm_x = 28;
  int hm_y = 34;

  display.setTextSize(2);
  display.setCursor(0, hm_y);
  display.print(buf_hm);

  display.setTextSize(1);
  display.setCursor(hm_x + 70, hm_y); // 放在冒号右侧下方
  display.print(buf_s);

  display.setFont(); // 关键：渲染完时间后恢复默认字体，以免影响频谱模式
  display.display();
}

// 设置 OLED 对比度
void setOLEDContrast(uint8_t value) {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(value); // 0-255，值越小屏幕越暗
}

void showBand() {

    // 更新峰值信息（由于原代码中是500ms更新一次显示，这里保留逻辑）
    unsigned long now = millis();
    if (now - lastPeakUpdate > peakTimeInterval) {
      // 寻找频率
      int frameBin = 0;
      float fm = 0;
      for (int i = 4; i < SAMPLES / 2; i++) {
        if (vReal[i] > fm) {
          fm = vReal[i];
          frameBin = i;
        }
      }
      maxDb = 20 * log10(fm + 1);
      maxFreq = frameBin * (SAMPLING_FREQ / SAMPLES);
      lastPeakUpdate = now;
    }

    // 绘制频谱显示 (此处复用您的原绘制逻辑)
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.printf("%4.1fdB", maxDb);
    display.setCursor(42, 0);
    display.printf("%4dHz", (int)maxFreq);

    // --- 新增：右上角显示时间 ---
    if (isTimeSynced) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        display.setCursor(98, 0); // 靠近右侧边缘
        display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      }
    }

    int barWidth = SCREEN_WIDTH / BAND_NUM;
    for (int i = 0; i < BAND_NUM; i++) {
      float maxAmp = 0;
      for (int j = bin_indices[i]; j < bin_indices[i + 1]; j++)
        if (vReal[j] > maxAmp) {
          maxAmp = vReal[j];
        }

      float norm = constrain((maxAmp - noiseFloor) / 2048.0 * dbMult, 0, 1);
      float db = norm * 100;
      if (db > oldBandDb[i]) {
        bandDb[i] = db * smoothUp + oldBandDb[i] * (1 - smoothUp);
      } else {
        bandDb[i] = db * smoothDown + oldBandDb[i] * (1 - smoothDown);
      }
      oldBandDb[i] = bandDb[i];

      int totalHeight = map(bandDb[i], 0, 100, 0, SCREEN_HEIGHT - HEADER_H - 2);
      int numBlocks = totalHeight / BLOCK_HIGHT;
      for (int b = 0; b < numBlocks; b++) {
        display.fillRect(i * barWidth, SCREEN_HEIGHT - (b + 1) * BLOCK_HIGHT, barWidth - 2, BLOCK_HIGHT - 2, SSD1306_WHITE);
      }
      if (bandDb[i] > peakDb[i]) {
        peakDb[i] = bandDb[i];
      } else {
        peakDb[i] -= peakFall;
      }
      int peakY = map(peakDb[i], 0, 100, SCREEN_HEIGHT, HEADER_H + 1);
      peakY = constrain(peakY, HEADER_H + 1, SCREEN_HEIGHT - 1);
      display.drawFastHLine(i * barWidth, peakY, barWidth - 2, SSD1306_WHITE);
    }
    display.display();
}

/* ================= Setup & Loop ================= */

void setup() {
  Serial.begin(115200);

#ifdef OLED_GND
  pinMode(OLED_GND, OUTPUT);
  digitalWrite(OLED_GND, LOW);
#endif

#ifdef OLED_VDD
  pinMode(OLED_VDD, OUTPUT);
  digitalWrite(OLED_VDD, HIGH);
#endif

#ifdef MIC_GND
  pinMode(MIC_GND, OUTPUT);
  digitalWrite(MIC_GND, LOW);
#endif

#ifdef MIC_VDD
  pinMode(MIC_VDD, OUTPUT);
  digitalWrite(MIC_VDD, HIGH);
#endif

  Wire.begin(OLED_SDA, OLED_SCK);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  display.display();

  setOLEDContrast(10);
}

void loop() {

  // FFT 核心逻辑
  unsigned long start = micros();
  for (int i = 0; i < SAMPLES; i++) {
    vReal[i] = (float)analogRead(MIC_ADC) - 2048.0;
    vImag[i] = 0;
    while (micros() - start < delayMs) {
    }
    start += delayMs;
  }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // 计算当前帧最大分贝
  float frameMax = 0;
  for (int i = 4; i < SAMPLES / 2; i++) {
    if (vReal[i] > frameMax) {
      frameMax = vReal[i];
    }
  }
  float currentDb = 20 * log10(frameMax + 1);

  // 2. 状态机逻辑
  if (currentMode == MODE_SPECTRUM) {
    // --- 频谱模式 ---
    showBand();

    // 闲置检测
    if (currentDb < idleThreshold) {
      if (lowVolumeStartTime == 0) {
        lowVolumeStartTime = millis();
      }
      if (millis() - lowVolumeStartTime > idleDelay) {
        Serial.println("声音持续过低，准备进入闲置模式...");
        currentMode = MODE_IDLE_TIME;
        lowVolumeStartTime = 0;
      }
    } else {
      lowVolumeStartTime = 0; // 声音恢复，重置计时器
    }

  } else {
    // --- 闲置/时间模式 ---

    // 唤醒检测
    if (currentDb > wakeupThreshold && !isWifiConnecting) {
      Serial.printf("返回频谱模式... (%.1f dB)\n", currentDb);
      currentMode = MODE_SPECTRUM;
      return;
    }

    // 逻辑：判断是否已有时间
    if (isTimeSynced) {
      static unsigned long lastTimeUpdate = 0;
      if (millis() - lastTimeUpdate >= 800) {
        updateTimeDisplay();
        lastTimeUpdate = millis();
      }
    } else {
      // 需要尝试获取时间
      if (!wifiFeatureEnabled) {
        displayCenterText("no sound", 1);
      } else {
        showBand();
        syncTime();
      }
    }
    delay(450);
  }
}