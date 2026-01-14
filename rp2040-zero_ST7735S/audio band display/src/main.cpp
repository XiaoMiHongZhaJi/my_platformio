#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include <Arduino.h>
#include <arduinoFFT.h>

/* ================= 硬件引脚定义 ================= */
#define TFT_SPI spi0 // 使用 SPI0
#define PIN_SCK 2    // SPI 时钟引脚
#define PIN_MOSI 3   // SPI 主出从入引脚
#define PIN_CS 1     // 片选引脚
#define PIN_DC 5     // 数据/命令引脚
#define PIN_RST 4    // 复位引脚
#define MIC_PIN 26   // ADC0 麦克风输入引脚

#define TFT_BAUD 40000000 // SPI 时钟 20/40 MHz
#define TFT_WIDTH 160     // ST7735 屏幕宽度
#define TFT_HEIGHT 80     // ST7735 屏幕高度
#define HEADER_H 12       // 顶部文字高度

/* ================= 频谱参数 ================= */
#define BAND_NUM 16        // 频段数量
#define BLOCK_HIGHT 7      // 垂直方块高度
#define SAMPLES 128        // FFT采样点数 必须为2的幂
#define SAMPLING_FREQ 4000 // 采样频率 (Hz)

#define noiseFloor 30  // 噪声抑制 越大抑制程度越高
#define dbMult 8.0     // 放大倍数 越大越灵敏
#define peakFall 2.0   // 峰值线下落速度 越大下落越快
#define smoothUp 0.9   // 上升平滑系数 0~1 越大上升响应越快
#define smoothDown 0.3 // 下降平滑系数 0~1 越大下降响应越快

double globalMaxDb = 0.0;   // 过去一段时间最大分贝值
double globalMaxFreq = 0.0; // 过去一段时间最大分贝值对应频率值
uint16_t peakTimeInterval = 500;    // 峰值更新周期 (ms)
uint8_t color_offset = 55;  // 颜色偏移 底部绿色 顶部红色

double vReal[SAMPLES];         // FFT 输入数组
double vImag[SAMPLES];         // FFT 输出数组
double bandDb[BAND_NUM];       // 当前显示的频谱数据
double oldBandDb[BAND_NUM];    // 上次显示的频谱数据
double peakDb[BAND_NUM];       // 频谱顶点数据
arduinoFFT FFT = arduinoFFT(); // FFT 对象

int bin_indices[17] = { // 频段对应的 FFT bin 索引
    2, 3, 4, 5, 7, 9, 11, 13, 16, 19, 23, 28, 34, 41, 49, 58, 64};

/* ================= ST7735 驱动底层 ================= */
// 设置引脚电平状态
inline void tft_dc(bool level) {
  gpio_put(PIN_DC, level);
}
// 片选引脚
inline void tft_cs(bool level) {
  gpio_put(PIN_CS, level);
}
// 复位引脚
inline void tft_rst(bool level) {
  gpio_put(PIN_RST, level);
}
// 发送命令和数据
void tft_write_cmd(uint8_t cmd) {
  tft_dc(0);
  tft_cs(0);
  spi_write_blocking(TFT_SPI, &cmd, 1);
  tft_cs(1);
}
// 发送数据
void tft_write_data(uint8_t data) {
  tft_dc(1);
  tft_cs(0);
  spi_write_blocking(TFT_SPI, &data, 1);
  tft_cs(1);
}
// 设置绘图窗口
void tft_set_addr_window(int x1, int y1, int x2, int y2) {
  // 针对 160x80 偏移处理 (通常 ST7735 160x80 需要偏移)
  // x1 += 1;
  // x2 += 1;
  y1 += 24;
  y2 += 24;

  tft_write_cmd(0x2A); // Column Address Set
  tft_write_data(x1 >> 8);
  tft_write_data(x1 & 0xFF);
  tft_write_data(x2 >> 8);
  tft_write_data(x2 & 0xFF);

  tft_write_cmd(0x2B); // Row Address Set
  tft_write_data(y1 >> 8);
  tft_write_data(y1 & 0xFF);
  tft_write_data(y2 >> 8);
  tft_write_data(y2 & 0xFF);

  tft_write_cmd(0x2C); // Memory Write
}
// 初始化 TFT 屏幕
void tft_init() {
  tft_rst(1);
  delay(10);

  tft_rst(0);
  delay(10);

  tft_rst(1);
  delay(100);

  tft_write_cmd(0x01);
  delay(150); // Software reset

  tft_write_cmd(0x11);
  delay(150); // Sleep out

  tft_write_cmd(0x3A);
  tft_write_data(0x05); // 16-bit color

  tft_write_cmd(0x36);
  // tft_write_data(0xA8); // 横屏
  // tft_write_data(0x00); // 竖屏
  tft_write_data(0x60); // 横屏翻转

  // tft_write_cmd(0x21); // 颜色反转
  tft_write_cmd(0x29); // Display ON
}
// 填充矩形区域
void tft_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (x >= TFT_WIDTH || y >= TFT_HEIGHT)
    return;
  if (x + w > TFT_WIDTH)
    w = TFT_WIDTH - x;
  if (y + h > TFT_HEIGHT)
    h = TFT_HEIGHT - y;
  if (w <= 0 || h <= 0)
    return;

  tft_set_addr_window(x, y, x + w - 1, y + h - 1);
  uint8_t data[2] = {(uint8_t)(color >> 8), (uint8_t)(color & 0xFF)};

  tft_dc(1);
  tft_cs(0);
  for (int i = 0; i < w * h; i++) {
    spi_write_blocking(TFT_SPI, data, 2);
  }
  tft_cs(1);
}
// 填充全屏
void tft_fill_screen(uint16_t color) {
  tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

// 颜色轮转换函数 (输入0-255 输出RGB565颜色)
uint16_t wheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85) {
    // RGB565 approximation
    return (((255 - pos * 3) & 0xF8) << 8) | ((pos * 3 >> 2) << 5) | (0);
  }
  if (pos < 170) {
    pos -= 85;
    return (0 << 11) | (((pos * 3) >> 2) << 5) | ((255 - pos * 3) >> 3);
  }
  pos -= 170;
  return (((pos * 3) & 0xF8) << 8) | (((255 - pos * 3) >> 2) << 5) | 0;
}
// 音频采样函数
void sampleAudio() {
  unsigned long start = micros();
  for (int i = 0; i < SAMPLES; i++) {
    vReal[i] = adc_read() - 2048.0;
    vImag[i] = 0;
    while (micros() - start < 250);
    start += 250;
  }
}
// 计算频段函数
void calc_band() {
  FFT.Windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.Compute(vReal, vImag, SAMPLES, FFT_FORWARD);
  FFT.ComplexToMagnitude(vReal, vImag, SAMPLES);
}

// 简易 5x7 字体点阵 (部分常用字符: 0-9, A-Z, '.', ' ', 'd', 'B', 'H', 'z')
const uint8_t font5x7[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x00, 0x00, 0x00, 0x00, 0x00}, // space
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x38, 0x44, 0x44, 0x44, 0x7F}, // d
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x44, 0x7D, 0x40, 0x00, 0x00}, // i
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
};

// 字符绘制函数
void tft_draw_char(int x, int y, char c, uint16_t color) {
  int idx = -1;
  if (c >= '0' && c <= '9')
    idx = c - '0';
  else if (c == '.')
    idx = 10;
  else if (c == ' ')
    idx = 11;
  else if (c == 'A')
    idx = 12;
  else if (c == 'B')
    idx = 13;
  else if (c == 'd')
    idx = 14;
  else if (c == 'F')
    idx = 15;
  else if (c == 'H')
    idx = 16;
  else if (c == 'i')
    idx = 17;
  else if (c == 'z')
    idx = 18;
  if (idx == -1)
    return;

  for (int i = 0; i < 5; i++) {
    uint8_t line = font5x7[idx][i];
    for (int j = 0; j < 7; j++) {
      if (line & (1 << j)) {
        tft_fill_rect(x + i, y + j, 1, 1, color);
      }
    }
  }
}
// 字符串绘制函数
void tft_draw_string(int x, int y, const char *s, uint16_t color) {
  while (*s) {
    tft_draw_char(x, y, *s++, color);
    x += 6;
  }
}

unsigned long lastPeakUpdate = 0;

// 计算方块高度
int usableHeight = TFT_HEIGHT - HEADER_H;

// 频谱绘制函数
void draw_spectrum() {
  // 1. 查找当前帧最大值及其对应频率
  double frameMax = 0;
  int frameBin = 0;
  for (int i = 4; i < SAMPLES / 2; i ++) {
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
    // color_offset += 5; // 动态颜色滚动
  }

  // 2. 绘制顶部文字区域
  tft_fill_rect(0, 0, TFT_WIDTH, HEADER_H, 0x0000);
  char buf[20];
  sprintf(buf, "%4.1f dB", globalMaxDb);
  tft_draw_string(5, 2, buf, 0xFFFF);
  sprintf(buf, "%4d Hz", (int)globalMaxFreq);
  tft_draw_string(90, 2, buf, 0xFFFF);

  // 3. 绘制频谱条与峰值线
  int barWidth = TFT_WIDTH / BAND_NUM;
  for (int i = 0; i < BAND_NUM; i++) {
    double maxAmp = 0;
    for (int j = bin_indices[i]; j < bin_indices[i + 1]; j++) {
      if (vReal[j] > maxAmp)
        maxAmp = vReal[j];
    }

    // 计算当前分贝并平滑
    double norm = constrain((maxAmp - noiseFloor) / 2048.0 * dbMult, 0, 1);
    double db = norm * 100;
    if (db > oldBandDb[i]) {
      // 上升
      bandDb[i] = db * smoothUp + oldBandDb[i] * (1 - smoothUp);
    } else {
      // 下降
      bandDb[i] = db * smoothDown + oldBandDb[i] * (1 - smoothDown);
    }
    oldBandDb[i] = bandDb[i];

    // 局部清空该列
    tft_fill_rect(i * barWidth, HEADER_H, barWidth - 1, usableHeight, 0x0000);
    int totalPx = map(bandDb[i], 0, 100, 0, usableHeight - 4);
    int numBlocks = totalPx / BLOCK_HIGHT;

    // 绘制彩色方块
    for (int b = 0; b < numBlocks; b++) {
      uint16_t c = wheel(b * BLOCK_HIGHT * 2 + color_offset);
      tft_fill_rect(i * barWidth, TFT_HEIGHT - (b + 1) * BLOCK_HIGHT, barWidth - 2, BLOCK_HIGHT - 2, c);
    }

    // 4. 峰值线逻辑 (下坠)
    if (bandDb[i] > peakDb[i]) {
      peakDb[i] = bandDb[i];
    } else {
      peakDb[i] -= peakFall;
    }

    // 映射峰值 Y 坐标
    int peakY = map(peakDb[i], 0, 100, TFT_HEIGHT, HEADER_H + 1);
    peakY = constrain(peakY, HEADER_H + 1, TFT_HEIGHT - 1);

    // 绘制白色顶点线
    tft_fill_rect(i * barWidth, peakY, barWidth - 2, 1, 0xFFFF);
  }
}

void setup() {
  Serial.begin(115200);

  // 初始化 SPI
  spi_init(TFT_SPI, TFT_BAUD);
  gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
  gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

  // 初始化 GPIO
  gpio_init(PIN_CS);
  gpio_set_dir(PIN_CS, GPIO_OUT);
  gpio_init(PIN_DC);
  gpio_set_dir(PIN_DC, GPIO_OUT);
  gpio_init(PIN_RST);
  gpio_set_dir(PIN_RST, GPIO_OUT);

  // 初始化 ADC
  adc_init();
  adc_gpio_init(MIC_PIN);
  adc_select_input(0); // GP26 is ADC0

  tft_init();
  tft_fill_screen(0x0000);
}

void loop() {
  // 采样音频
  sampleAudio();
  // FFT 计算
  calc_band();
  // 绘制频谱
  draw_spectrum();
}