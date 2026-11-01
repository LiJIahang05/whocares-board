/*
 * 第3周 · 墨水屏桌面看板 v2
 * 板子: LilyGo T5 V2.3.2 / 2.13寸 DEPG0213BN (SSD1680)
 *
 * 功能：
 *   1. 显示时间、日期（自动联网对时）
 *   2. 显示宁波实时天气（温度 + 天气状况 + 今日最高最低）
 *   3. 手机连同一个WiFi，用浏览器打开屏幕上显示的地址，打字发到屏幕上
 *
 * 刷新规则：每 5 分钟自动刷新一次；收到手机消息立即刷新
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// ===== 只有这两行可能要改 =====
const char* WIFI_SSID = "你的WiFi名";   // 必须是 2.4G 频段
const char* WIFI_PASS = "你的WiFi密码";
// =============================

// 宁波坐标（换城市改这两个数就行）
const char* CITY_LAT = "29.87";
const char* CITY_LON = "121.55";

GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT>
  display(GxEPD2_213_BN(/*CS=*/5, /*DC=*/17, /*RST=*/16, /*BUSY=*/4));

U8G2_FOR_ADAFRUIT_GFX u8g2;
WebServer server(80);

// ---- 状态 ----
float curTemp = NAN, maxTemp = NAN, minTemp = NAN;
int   wxCode = -1;
bool  weatherOk = false;
String userMsg = "手机连同一个WiFi, 浏览器打开下面的地址";
String ipStr   = "";

// ---------- 工具：居中显示英文/数字（GFX字体） ----------
void centerGfx(const char* s, int16_t y, const GFXfont* f) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setFont(f);
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((122 - w) / 2 - x1, y);
  display.print(s);
}

// ---------- 工具：居中显示中文（u8g2字体） ----------
void centerCn(const String& s, int16_t y) {
  int w = u8g2.getUTF8Width(s.c_str());
  u8g2.setCursor((122 - w) / 2, y);
  u8g2.print(s);
}

// ---------- 工具：中文自动换行 ----------
int drawWrapped(const String& text, int x, int yStart, int maxWidth, int lineH, int maxLines) {
  int line = 0;
  String cur = "";
  int i = 0;
  int len = text.length();
  while (i < len) {
    uint8_t c = (uint8_t)text[i];
    int clen = 1;
    if ((c & 0x80) == 0)        clen = 1;
    else if ((c & 0xE0) == 0xC0) clen = 2;
    else if ((c & 0xF0) == 0xE0) clen = 3;
    else                         clen = 4;
    String ch = text.substring(i, i + clen);
    if (ch == "\n") {
      u8g2.setCursor(x, yStart + line * lineH);
      u8g2.print(cur);
      line++; cur = "";
      if (line >= maxLines) return line;
      i += clen;
      continue;
    }
    String test = cur + ch;
    if (u8g2.getUTF8Width(test.c_str()) > maxWidth && cur.length() > 0) {
      u8g2.setCursor(x, yStart + line * lineH);
      u8g2.print(cur);
      line++;
      if (line >= maxLines) return line;
      cur = ch;
    } else {
      cur = test;
    }
    i += clen;
  }
  if (cur.length() > 0 && line < maxLines) {
    u8g2.setCursor(x, yStart + line * lineH);
    u8g2.print(cur);
    line++;
  }
  return line;
}

// ---------- 工具：从 JSON 里取 key 后面的第一个数字 ----------
// 自动跳过 key 与数字之间可能出现的 : " [ 和空格，不用手数偏移量（数错一位就会读错温度）
float numAfter(const String& s, const String& key, int from) {
  int p = s.indexOf(key, from);
  if (p < 0) return NAN;
  p += key.length();
  while (p < (int)s.length()) {
    char c = s[p];
    if (c == ':' || c == ' ' || c == '[' || c == '"') { p++; continue; }
    break;
  }
  return s.substring(p).toFloat();
}

// ---------- 天气代码转中文 ----------
String wxText(int code) {
  if (code == 0)                    return "晴";
  if (code == 1 || code == 2)       return "多云";
  if (code == 3)                    return "阴";
  if (code == 45 || code == 48)     return "雾";
  if (code >= 51 && code <= 57)     return "毛毛雨";
  if (code >= 61 && code <= 67)     return "下雨";
  if (code >= 71 && code <= 77)     return "下雪";
  if (code >= 80 && code <= 82)     return "阵雨";
  if (code >= 85 && code <= 86)     return "阵雪";
  if (code >= 95)                   return "雷阵雨";
  return "未知";
}

// ---------- 画整屏 ----------
void drawScreen() {
  struct tm t;
  bool timeOk = getLocalTime(&t, 50);

  char timeBuf[8] = "--:--";
  char dateBuf[32] = "";
  if (timeOk) {
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &t);
    static const char* wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    snprintf(dateBuf, sizeof(dateBuf), "%d月%d日 %s", t.tm_mon + 1, t.tm_mday, wd[t.tm_wday]);
  }

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    u8g2.setFontMode(1);
    u8g2.setFontDirection(0);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.setBackgroundColor(GxEPD_WHITE);

    // ---- 标题 ----
    // 用 12px 中文等宽字体，宽度约 85px，122px 屏放得下（原来用 9pt 英文字体会超宽换行）
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    centerCn("WhoCares BOARD", 18);
    display.drawLine(6, 25, 116, 25, GxEPD_BLACK);

    // ---- 时间（大字）----
    centerGfx(timeBuf, 62, &FreeSansBold18pt7b);

    // ---- 日期（中文小字）----
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    if (timeOk) centerCn(dateBuf, 82);
    display.drawLine(6, 92, 116, 92, GxEPD_BLACK);

    // ---- 天气 ----
    if (weatherOk) {
      char top[24];
      snprintf(top, sizeof(top), "%s %d°C", wxText(wxCode).c_str(), (int)round(curTemp));
      u8g2.setFont(u8g2_font_wqy16_t_gb2312);
      centerCn(top, 118);

      char bot[28];
      snprintf(bot, sizeof(bot), "最高%d°  最低%d°", (int)round(maxTemp), (int)round(minTemp));
      u8g2.setFont(u8g2_font_wqy12_t_gb2312);
      centerCn(bot, 136);
    } else {
      u8g2.setFont(u8g2_font_wqy12_t_gb2312);
      centerCn("天气获取中...", 118);
    }
    display.drawLine(6, 148, 116, 148, GxEPD_BLACK);

    // ---- 留言区 ----
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    drawWrapped(userMsg, 6, 168, 110, 16, 4);
    display.drawLine(6, 234, 116, 234, GxEPD_BLACK);

    // ---- 底部：IP 地址（手机用这个地址发消息）----
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    if (ipStr.length() > 0) centerCn(ipStr, 247);
    else                    centerCn("WiFi未连接", 247);

  } while (display.nextPage());

  Serial.println("屏幕已刷新");
}

// ---------- 联网取天气 ----------
bool fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();                 // 跳过证书校验，简单可靠
  HTTPClient https;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + CITY_LAT +
               "&longitude=" + CITY_LON +
               "&current=temperature_2m,weather_code" +
               "&daily=temperature_2m_max,temperature_2m_min" +
               "&timezone=Asia/Shanghai&forecast_days=1";

  if (!https.begin(client, url)) { Serial.println("天气: 连接失败"); return false; }
  https.setTimeout(10000);
  int code = https.GET();
  if (code != 200) {
    Serial.printf("天气: HTTP %d\n", code);
    https.end();
    return false;
  }
  String p = https.getString();
  https.end();

  // 解析 current 段
  int curPos = p.indexOf("\"current\":");
  if (curPos < 0) return false;

  curTemp = numAfter(p, "\"temperature_2m\":", curPos);
  wxCode  = (int)numAfter(p, "\"weather_code\":", curPos);
  if (isnan(curTemp)) return false;

  // 解析 daily 段
  int dp = p.indexOf("\"daily\":");
  if (dp > 0) {
    maxTemp = numAfter(p, "\"temperature_2m_max\":", dp);
    minTemp = numAfter(p, "\"temperature_2m_min\":", dp);
  }
  // 拿不到高低温就不显示那两个数，别显示 "最高0°"
  if (isnan(maxTemp)) maxTemp = curTemp;
  if (isnan(minTemp)) minTemp = curTemp;

  weatherOk = !isnan(curTemp);
  Serial.printf("天气: %s %.1f°C 最高%.1f 最低%.1f\n",
                wxText(wxCode).c_str(), curTemp, maxTemp, minTemp);
  return weatherOk;
}

// ---------- 手机网页 ----------
String htmlPage() {
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>桌面看板</title><style>";
  h += "body{font-family:-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:24px}";
  h += "h2{font-size:20px;margin:0 0 16px}";
  h += "textarea{width:100%;height:110px;font-size:17px;padding:10px;box-sizing:border-box;";
  h += "background:#1e293b;color:#e2e8f0;border:1px solid #334155;border-radius:10px;resize:none}";
  h += "button{width:100%;padding:15px;font-size:18px;margin-top:14px;background:#3b82f6;";
  h += "color:#fff;border:0;border-radius:10px}";
  h += ".tip{color:#94a3b8;font-size:14px;margin-top:18px;line-height:1.6}";
  h += "</style></head><body>";
  h += "<h2>发给桌面看板</h2>";
  h += "<form action='/msg' method='get'>";
  h += "<textarea name='t' maxlength='80' placeholder='说点什么，最多80个字'></textarea>";
  h += "<button type='submit'>发送到屏幕</button></form>";
  h += "<div class='tip'>屏幕上现在显示：<br>" + userMsg + "</div>";
  h += "</body></html>";
  return h;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

void handleMsg() {
  String t = server.arg("t");
  t.trim();
  if (t.length() > 0) {
    userMsg = t;
    Serial.println("收到消息: " + userMsg);
    server.send(200, "text/html; charset=utf-8",
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>body{background:#0f172a;color:#e2e8f0;font-family:sans-serif;"
      "padding:40px 24px;text-align:center}h2{font-size:22px}"
      "a{color:#3b82f6;font-size:17px;text-decoration:none}</style></head><body>"
      "<h2>已发送 ✓</h2><p>墨水屏正在刷新，大约 3 秒后就能看到</p>"
      "<p style='margin-top:30px'><a href='/'>再发一条</a></p></body></html>");
    drawScreen();     // 立即刷屏
  } else {
    server.send(200, "text/html; charset=utf-8",
      "<meta charset='utf-8'><body style='background:#0f172a;color:#e2e8f0;padding:40px'>"
      "内容是空的，<a href='/' style='color:#3b82f6'>返回</a></body>");
  }
}

// ---------- 启动 ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== 墨水屏看板 v2 启动 ===");

  SPI.begin(18, 19, 23);
  display.init(115200, true, 2, false);
  display.setRotation(0);
  u8g2.begin(display);

  // 先给个开机画面
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    u8g2.setFontMode(1);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    centerCn("开机中...", 60);
    centerCn("正在连WiFi", 80);
  } while (display.nextPage());

  // ---- 连 WiFi ----
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("连接 WiFi: %s\n", WIFI_SSID);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 60) { delay(500); Serial.print("."); tries++; }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    ipStr = WiFi.localIP().toString();
    Serial.println("WiFi 已连上，板子 IP: " + ipStr);
  } else {
    ipStr = "";
    Serial.println("WiFi 没连上");
  }

  // ---- 对时 ----
  configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org");
  delay(1000);

  // ---- 天气 ----
  if (WiFi.status() == WL_CONNECTED) fetchWeather();

  // ---- 网页服务（手机发消息用）----
  server.on("/", handleRoot);
  server.on("/msg", handleMsg);
  server.begin();
  Serial.println("网页服务已启动，手机浏览器打开: http://" + ipStr);

  drawScreen();
}

void loop() {
  server.handleClient();          // 处理手机发来的请求

  // 每 5 分钟刷新一次（时间 + 天气）
  static unsigned long lastRefresh = millis();
  if (millis() - lastRefresh > 5UL * 60UL * 1000UL) {
    lastRefresh = millis();
    // WiFi 掉了就重连，不然板子会一直卡在"WiFi未连接"
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi 断开，重连中...");
      WiFi.reconnect();
      delay(3000);
    }
    // 每次都重新取天气（原来只在第一次成功前取，之后永远停在开机那一刻）
    if (WiFi.status() == WL_CONNECTED) fetchWeather();
    drawScreen();
  }
}
