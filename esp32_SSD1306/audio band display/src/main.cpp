
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <Wire.h>
#include <arduinoFFT.h>

/* ================= 硬件与定义 ================= */
#define SCREEN_WIDTH 128 // SSD1306 屏幕宽度
#define SCREEN_HEIGHT 64 // SSD1306 屏幕高度
#define HEADER_H 10      // 顶部文字高度

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// FFT 相关
#define SAMPLES 128        // FFT采样点数 必须为2的幂
#define SAMPLING_FREQ 4000 // 采样频率 (Hz)
#define BAND_NUM 16        // 频段数量
#define BLOCK_HIGHT 7      // 垂直方块高度

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
float globalMaxDb = -60;         // 过去一段时间最大分贝值
float globalMaxFreq = 0;         // 过去一段时间最大分贝值对应频率值
uint16_t peakTimeInterval = 500; // 峰值更新周期 (ms)

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
}

unsigned long lastPeakUpdate = 0;
float delayMs = 1000000 / SAMPLING_FREQ;

void loop() {

  // FFT 核心逻辑
  unsigned long start = micros();
  for (int i = 0; i < SAMPLES; i++) {
    vReal[i] = (float)analogRead(MIC_ADC) - 2048.0;
    vImag[i] = 0;
    while (micros() - start < delayMs)
      ;
    start += delayMs;
  }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  display.clearDisplay();

  // 2. 峰值信息展示 (格式化：数字右对齐)
  float frameMax = 0;
  int frameBin = 0;
  for (int i = 4; i < SAMPLES / 2; i++) {
    if (vReal[i] > frameMax) {
      frameMax = vReal[i];
      frameBin = i;
    }
  }

  unsigned long now = millis();
  if (now - lastPeakUpdate > peakTimeInterval) {
    globalMaxDb = 20 * log10(frameMax + 1);
    globalMaxFreq = frameBin * (SAMPLING_FREQ / SAMPLES);
    lastPeakUpdate = now;
  }

  display.setCursor(0, 0);
  display.printf("%4.1f dB", globalMaxDb);
  display.setCursor(46, 0);
  display.printf("%4d Hz", (int)globalMaxFreq); // 5位宽度实现右对齐

  // 3. 频谱计算
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
      // 上升
      bandDb[i] = db * smoothUp + oldBandDb[i] * (1 - smoothUp);
    } else {
      // 下降
      bandDb[i] = db * smoothDown + oldBandDb[i] * (1 - smoothDown);
    }
    oldBandDb[i] = bandDb[i];

    // 绘制频谱柱状方块图
    int totalHeight = map(bandDb[i], 0, 100, 0, SCREEN_HEIGHT - HEADER_H - 2);
    int numBlocks = totalHeight / BLOCK_HIGHT;
    for (int b = 0; b < numBlocks; b++) {
      display.fillRect(i * barWidth, SCREEN_HEIGHT - (b + 1) * BLOCK_HIGHT,
                       barWidth - 2, BLOCK_HIGHT - 2, SSD1306_WHITE);
    }

    // 峰值线
    if (bandDb[i] > peakDb[i])
      peakDb[i] = bandDb[i];
    else
      peakDb[i] -= peakFall;
    int peakY = map(peakDb[i], 0, 100, SCREEN_HEIGHT, HEADER_H + 1);
    peakY = constrain(peakY, HEADER_H + 1, SCREEN_HEIGHT - 1);
    display.drawFastHLine(i * barWidth, peakY, barWidth - 2, SSD1306_WHITE);
  }

  display.display();
}