#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>
#include <ArduinoJson.h>
#include <WebServer.h>
WebServer server(80);

// ===== WiFi 信息 =====
const char* ssid     = "MYWIFI";
const char* password = "12222222";

// ===== NTP =====
const char* ntpServer = "203.107.6.88";
const long  gmtOffset_sec = 8 * 3600;
const int   daylightOffset_sec = 0;

// ===== OLED =====
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  U8X8_PIN_NONE
);

// ===== 显示参数 =====
const int titleHeight = 15;
const int lineHeight  = 14;
int screenHeight      = 64;
int scrollY = 0;

// ===== 时间 =====
char timeStr[9] = "--:--:--";
unsigned long lastTimeUpdate = 0;

String titleText = "模拟夜间灯光";
String contentText = R"rawliteral(


请开启前照灯
↑近光灯
同方向近距离跟车行驶
↑近光灯
与机动车会车
↑近光灯
在有路灯 照明良好的道路上行驶
↑近光灯
通过有交通信号灯控制的路口
↑近光灯

进入无照明的道路行驶
↑远光灯
夜间在照明不良的道路行驶
↑远光灯

通过没有交通信号灯控制的路口
↑交替远近光灯3次
通过人行横道
↑交替远近光灯3次
通过急弯
↑交替远近光灯3次
通过坡路
↑交替远近光灯3次
通过拱桥
↑交替远近光灯3次

在路边临时停车
↑示廓灯 报警灯

超车
↑左 远近交替 右
)rawliteral";

bool enableScroll = true;
int scrollSpeed = 30;   // ms
int screenRotation = 0; // 0,1,2,3
int wifi_status = 100;

void connectWiFi() {
  WiFi.begin(ssid, password);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(0, 32, "WiFi 连接中...");
  u8g2.sendBuffer();

  while (wifi_status > 0 && WiFi.status() != WL_CONNECTED) {
    delay(100);
    wifi_status --;
  }
  u8g2.clearBuffer();
  if (wifi_status == 0) {
    u8g2.drawUTF8(0, 20, "WiFi 连接失败");
  } else {
    u8g2.drawUTF8(0, 20, "WiFi 连接成功");
    IPAddress ip = WiFi.localIP();
    u8g2.drawUTF8(0, 40, ip.toString().c_str());
  }
  u8g2.sendBuffer();
  delay(2000);
}

boolean updateTime() {
  struct tm timeinfo;
  boolean res = getLocalTime(&timeinfo);
  if (res) {
    snprintf(timeStr, sizeof(timeStr),
             "%02d:%02d:%02d",
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);
  }
  return res;
}

void applyRotation(int rot) {
  switch (rot) {
    case 1:
      u8g2.setDisplayRotation(U8G2_R1);
      screenHeight = 128;
      break;
    case 2:
      u8g2.setDisplayRotation(U8G2_R2);
      screenHeight = 64;
      break;
    case 3:
      u8g2.setDisplayRotation(U8G2_R3);
      screenHeight = 128;
      break;
    default:
      u8g2.setDisplayRotation(U8G2_R0);
      screenHeight = 64;
      break;
  }
}

void setupWebServer() {

  server.on("/", []() {
  String html = R"rawliteral(
  <!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 OLED 控制台</title><link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet"></head>
  <body class="bg-light">
  <div class="container mt-4">
    <h4 class="mb-3">📟 OLED 控制面板</h4>
    <form id="cfgForm">
      <div class="mb-3">
        <label class="form-label">标题</label>
        <input class="form-control" id="title">
      </div>
      <div class="mb-3">
        <label class="form-label">内容</label>
        <textarea class="form-control" id="content" rows="4"></textarea>
      </div>
      <div class="mb-3">
        <label class="form-label">方向</label>
        <select class="form-select" id="rot">
          <option value="0">0°</option>
          <option value="1">90°</option>
          <option value="2">180°</option>
          <option value="3">270°</option>
        </select>
      </div>
      <div class="mb-3">
        <label class="form-label">滚动速度 (ms 数字越大滚动越慢)</label>
        <input type="number" class="form-control" id="speed">
      </div>
      <div class="form-check mb-3">
        <input class="form-check-input" type="checkbox" id="scrollText">
        <label class="form-check-label">启用滚动</label>
      </div>
      <button type="button" class="btn btn-primary w-100" onclick="apply()">应用设置</button>
    </form>
    <div id="msg" class="alert alert-success d-none" style="margin-top: 1em;">设置已更新</div>
  </div>
  <script>
  function loadStatus() {
    fetch('/status')
      .then(r => r.json())
      .then(j => {
        title.value = j.title;
        content.value = j.content.trim();
        rot.value = j.rot;
        speed.value = j.speed;
        scrollText.checked = j.scrollText;
      });
  }
  function apply() {
    const data = new URLSearchParams();
    data.append("title", title.value);
    data.append("content", content.value.trim());
    data.append("rot", rot.value);
    data.append("speed", speed.value);
    if (scrollText.checked) data.append("scrollText", "1");
    fetch("/set", { method: "POST", body: data })
      .then(r => r.json())
      .then(() => {
        const msg = document.getElementById("msg");
        msg.classList.remove("d-none");
        setTimeout(() => msg.classList.add("d-none"), 2000);
      });
  }
  window.onload = loadStatus;
  </script></body></html>
  )rawliteral";

    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/status", []() {
    JsonDocument doc;
    doc["title"]   = titleText;
    doc["content"] = contentText;
    doc["scrollText"]  = enableScroll;
    doc["speed"]   = scrollSpeed;
    doc["rot"]     = screenRotation;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/set", []() {

    if (server.hasArg("title") && server.arg("title").length() > 0) {
      titleText = server.arg("title");
    }

    enableScroll = server.hasArg("scrollText");

    if (server.hasArg("content") && server.arg("content").length() > 0) {
      contentText = (enableScroll ? "\n\n\n" : "") + server.arg("content");
    }

    if (server.hasArg("speed") && server.arg("speed").length() > 0) {
      scrollSpeed = server.arg("speed").toInt();
    }
    scrollY = 0;

    if (server.hasArg("rot") && server.arg("rot").length() > 0) {
      applyRotation(server.arg("rot").toInt());
    }

    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
}

void setup() {

#ifdef OLED_GND
  pinMode(OLED_GND, OUTPUT);
  digitalWrite(OLED_GND, LOW);
#endif

#ifdef OLED_VDD
  pinMode(OLED_VDD, OUTPUT);
  digitalWrite(OLED_VDD, HIGH);
#endif

  Wire.begin(OLED_SDA, OLED_SCK);
  u8g2.begin();
  u8g2.enableUTF8Print();

  connectWiFi();
  u8g2.clearBuffer();

  if (wifi_status > 0) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    u8g2.clearBuffer();
    u8g2.drawUTF8(0, 32, "获取时间...");
    u8g2.sendBuffer();
    delay(500);
    updateTime();
  }
  setupWebServer();
}

void drawContent() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);

  // 标题
  u8g2.drawUTF8(0, 12, titleText.c_str());

  if (wifi_status > 0) {
    // 时间
    int tw = u8g2.getUTF8Width(timeStr);
    u8g2.drawUTF8(128 - tw, 12, timeStr);
  }

  u8g2.drawHLine(0, titleHeight, 128);

  // 内容绘制
  int y = titleHeight + lineHeight - scrollY;
  int start = 0;
  int lineCount = 0;
  while (true) {
    int end = contentText.indexOf('\n', start);
    if (end == -1) {
      end = contentText.length();
    }

    String line = contentText.substring(start, end);
    if (y >= titleHeight + lineHeight - 2 && y < screenHeight + lineHeight) {
      u8g2.drawUTF8(0, y, line.c_str());
    }
    y += lineHeight;
    if (end >= contentText.length()) {
      break;
    }
    lineCount ++;
    start = end + 1;
  }

  u8g2.sendBuffer();

  if (enableScroll) {
    scrollY++;
    if (scrollY > lineCount * lineHeight) {
      scrollY = 0;
    }
  }

  delay(scrollSpeed);
}

void loop() {

  if (wifi_status > 0 && millis() - lastTimeUpdate >= 1000) {
    server.handleClient();
    updateTime();
    lastTimeUpdate = millis();
  }

  drawContent();
}