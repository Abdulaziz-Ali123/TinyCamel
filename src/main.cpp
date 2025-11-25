#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"

// ---------------- CAMERA PINS (XIAO ESP32C3) ----------------
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15

#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// ---------------- MOTOR PINS ----------------
#define EN1 9
#define IN1 8
#define IN2 7

#define IN3 4
#define IN4 5
#define EN2 6 // PWM speed control

// ---------------- WIFI AP ----------------
const char* ssid = "ESP32-CAR";
const char* password = "12345678";

WebServer server(80);

bool streaming = false;
WiFiClient streamClient;

unsigned long lastStreamTime = 0;
const unsigned long streamInterval = 40;  // 25 FPS

// Label + frame tracking
int pwmValue = 200;
uint32_t frameCounter = 0;

int label_fw   = 0;
int label_left = 0;
int label_right= 0;
int label_back = 0;

// -----------------------------------------------------------
// HTML PAGE (MULTI-KEY HANDLING)
// -----------------------------------------------------------
String htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>ESP32-C3 RC Car</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { background:#111; color:white; text-align:center; font-family:Arial; }
img { width:92%; border-radius:12px; margin-top:10px; }
input { width:80%; }
</style>
</head>
<body>

<h2>ESP32-C3 RC CAR</h2>
<img src="/stream">

<h3>Speed</h3>
<input type="range" min="0" max="255" value="200" id="speedSlider"
       oninput="updateSpeed(this.value)">
<p>Speed: <span id="speedVal">200</span></p>

<script>
let keys = { w:0, a:0, s:0, d:0 };
let lastSent = "";

function getCommand() {
    let fw = keys.w;
    let bk = keys.s;
    let lt = keys.a;
    let rt = keys.d;

    if (fw && lt) return "fl";
    if (fw && rt) return "fr";
    if (bk && lt) return "bl";
    if (bk && rt) return "br";

    if (fw) return "f";
    if (bk) return "b";
    if (lt) return "l";
    if (rt) return "r";

    return "s";
}

function updateCommand() {
    let cmd = getCommand();
    if (cmd !== lastSent) {
        fetch("/cmd?dir=" + cmd);
        lastSent = cmd;
    }
}

document.addEventListener("keydown", e => {
    let k = e.key.toLowerCase();
    if (k === "a"){
        keys.w = 1;
        keys.a = 1;
    }else if (k === "d"){
        keys.w = 1;
        keys.d = 1;
    }else if (k in keys) {
        keys[k] = 1;
    }
    updateCommand();
});

document.addEventListener("keyup", e => {
    let k = e.key.toLowerCase();
    if (k === "a"){
        keys.a = 0;
        keys.w = 0;
    }else if (k === "d"){
        keys.d = 0;
        keys.w = 0;
    }else if (k in keys) {
        keys[k] = 0;    
    }
    updateCommand();
});

function updateSpeed(v){
  document.getElementById("speedVal").innerText = v;
  fetch("/speed?val=" + v);
}
</script>

</body>
</html>
)rawliteral";

// -----------------------------------------------------------
// MOTOR + LABEL CONTROL
// -----------------------------------------------------------
void resetLabels() {
  label_fw = label_left = label_right = label_back = 0;
}

void applyLabel(String dir) {
  resetLabels();
  if (dir == "f") label_fw = 1;
  if (dir == "b") label_back = 1;
  if (dir == "l") label_left = 1;
  if (dir == "r") label_right = 1;
  if (dir == "fl") { label_fw=1; label_left=1; }
  if (dir == "fr") { label_fw=1; label_right=1; }
  if (dir == "bl") { label_back=1; label_left=1; }
  if (dir == "br") { label_back=1; label_right=1; }
}

void stopMotors() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void forward()  { digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW); }
void backward() { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
void left()     { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); }
void right()    { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); }

void fwLeft() { left(); forward(); }
void fwRight(){ right(); forward(); }
void bwLeft() { left(); backward(); }
void bwRight(){ right(); backward(); }

// -----------------------------------------------------------
// STREAM FRAME WITH LABELS
// -----------------------------------------------------------
void streamOneFrame() {
  if (!streaming) return;
  if (!streamClient.connected()) { streaming = false; return; }

  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return;

  // Build metadata filename string
  char labelString[80];
  sprintf(labelString, "%lu %d %d %d %d %d.jpg",
          frameCounter,
          label_fw, label_left, label_right, label_back,
          pwmValue);

  // Serial debug print
  Serial.print("[FRAME] ");
  Serial.print(labelString);
  Serial.print(" | Size: ");
  Serial.print(fb->len);
  Serial.println(" bytes");

  // Send MJPEG header
  streamClient.printf(
    "--frame\r\n"
    "Content-Type:image/jpeg\r\n"
    "X-Label:%s\r\n"
    "Content-Length:%u\r\n\r\n",
    labelString, fb->len
  );

  // Send JPEG image
  streamClient.write(fb->buf, fb->len);
  streamClient.write("\r\n");

  frameCounter++;

  esp_camera_fb_return(fb);
}

// -----------------------------------------------------------
// HANDLE COMMANDS (LABEL + MOTORS)
// -----------------------------------------------------------
void handle_cmd() {
  String dir = server.arg("dir");

  applyLabel(dir);

  if (dir == "f") forward();
  else if (dir == "b") backward();
  else if (dir == "l") left();
  else if (dir == "r") right();
  else if (dir == "fl") fwLeft();
  else if (dir == "fr") fwRight();
  else if (dir == "bl") bwLeft();
  else if (dir == "br") bwRight();
  else stopMotors();

  server.send(200, "text/plain", "OK");
}

// -----------------------------------------------------------
void handle_speed() {
  pwmValue = constrain(server.arg("val").toInt(), 0, 255);
  ledcWrite(0, pwmValue);
  server.send(200, "text/plain", "OK");
}

// -----------------------------------------------------------
void handle_jpg_stream() {
  streamClient = server.client();
  streaming = true;

  streamClient.println("HTTP/1.1 200 OK");
  streamClient.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  streamClient.println();

  resetLabels();
}

// -----------------------------------------------------------
// SETUP
// -----------------------------------------------------------
void setup() {
  delay(1500);
  Serial.begin(115200);
  delay(300);

  Serial.println("\n=== ESP32-C3 RC CAR STARTING ===");

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pinMode(EN1, OUTPUT);
  digitalWrite(EN1, HIGH);

  ledcAttachPin(EN2, 0);
  ledcSetup(0, 20000, 8);
  ledcWrite(0, pwmValue);

  stopMotors();

  WiFi.softAP(ssid, password);
  Serial.print("[WiFi] ");
  Serial.println(WiFi.softAPIP());

  // Camera init
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  esp_camera_init(&config);

  // Routes
  server.on("/", [](){ server.send(200, "text/html", htmlPage); });
  server.on("/cmd", handle_cmd);
  server.on("/stream", handle_jpg_stream);
  server.on("/speed", handle_speed);

  server.begin();
  Serial.println("[Server] Running");
}

// -----------------------------------------------------------
void loop() {
  server.handleClient();

  if (streaming) {
    unsigned long now = millis();
    if (now - lastStreamTime >= streamInterval) {
      lastStreamTime = now;
      streamOneFrame();
    }
  }
}
