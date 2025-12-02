#include <WiFi.h>
#include <WebServer.h>
#include <Arduino.h>
#include "esp_camera.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model_data.h" 

// ---------------- MEMORY CONFIG (PSRAM) ----------------
// We use 1MB of PSRAM to ensure the model fits without crashing
constexpr int kTensorArenaSize = 1024 * 1024; 
uint8_t* tensorArena; 

// ---------------- CAMERA PINS (XIAO ESP32S3) ----------------
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

// ---------------- MOTOR PINS (Your Hardware Config) ----------------
// Motor A = Steering (Left/Right)
#define EN1 9 
#define IN1 8
#define IN2 7

// Motor B = Throttle (Forward/Backward)
#define IN3 4
#define IN4 5
#define EN2 6 // PWM Speed Control

// Speed Settings
int speedVal = 220; // 0-255 (Adjust if too fast/slow)

// ---------------- TFLite Objects ----------------
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
tflite::AllOpsResolver resolver; 

// ---------------- WIFI & SERVER ----------------
const char* ssid = "ESP32-CAR";
const char* password = "12345678";
WebServer server(80);

// ---------------- MOTOR FUNCTIONS ----------------
void setupMotors() {
    pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
    pinMode(EN1, OUTPUT); pinMode(EN2, OUTPUT);

    // EN1 is for Steering - Always HIGH for max torque
    digitalWrite(EN1, HIGH);

    // EN2 is for Throttle - PWM for speed control
    // IMPORTANT: Use Channel 2 to avoid conflict with Camera (Channel 0)
    ledcAttachPin(EN2, 2); 
    ledcSetup(2, 20000, 8);
    ledcWrite(2, 0); // Start stopped
}

// Steering Control (Motor A)
void turnLeft() { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); }
void turnRight() { digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH); }
void steerStraight() { digitalWrite(IN1, LOW); digitalWrite(IN2, LOW); }

// Throttle Control (Motor B)
void moveForward() {
    digitalWrite(IN3, HIGH);
    digitalWrite(IN4, LOW);

    int dutyCycle = 255; // keep above stall
    int pulse = 100;     // how long to run PWM
    int pause = 50;      // how long to pause motor

    // run motor in pulses to reduce average speed
    ledcWrite(2, dutyCycle);
    delay(pulse);
    ledcWrite(2, 0);
    delay(pause);
}


void stopCar() {
    digitalWrite(IN3, LOW); 
    digitalWrite(IN4, LOW);
    steerStraight();
    ledcWrite(2, 0);
}

// ---------------- PREPROCESS ----------------
void preprocessFrame(camera_fb_t* fb, TfLiteTensor* input){
    int start_x = (fb->width - 96) / 2;
    int start_y = (fb->height - 96) / 2;
    
    for (int y = 0; y < 96; y++) {
        for (int x = 0; x < 96; x++) {
            int src_idx = ((start_y + y) * fb->width) + (start_x + x);
            // Convert 0-255 to -128 to 127 for INT8 model
            int8_t pixel = (int8_t)(fb->buf[src_idx] - 128); 
            
            // Replicate grayscale to RGB channels (Model expects 3 channels)
            input->data.int8[(y * 96 * 3) + (x * 3) + 0] = pixel; 
            input->data.int8[(y * 96 * 3) + (x * 3) + 1] = pixel; 
            input->data.int8[(y * 96 * 3) + (x * 3) + 2] = pixel; 
        }
    }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  
  // 1. Setup Motors
  setupMotors();

  // 2. Setup PSRAM
  if(psramFound()){
      Serial.println("PSRAM Found! Allocating Arena...");
      tensorArena = (uint8_t*)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM);
  } else {
      Serial.println("Error: PSRAM not found! System halted.");
      return; 
  }

  // 3. Setup Camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; 
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM; config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE; 
  config.frame_size = FRAMESIZE_QVGA; 
  config.jpeg_quality = 12; 
  config.fb_count = 1;
  
  if (esp_camera_init(&config) != ESP_OK) { Serial.println("Camera init failed"); return; }

  // 4. Setup TFLite
  const tflite::Model* model = tflite::GetModel(TinyCamel_Q8_tflite); 
  if (model->version() != TFLITE_SCHEMA_VERSION) {
      Serial.println("Model schema mismatch!");
      return;
  }
  interpreter = new tflite::MicroInterpreter(model, resolver, tensorArena, kTensorArenaSize);
  if (interpreter->AllocateTensors() != kTfLiteOk) {
      Serial.println("AllocateTensors failed");
      return;
  }
  input = interpreter->input(0);
  
  // 5. Setup WiFi
  WiFi.softAP(ssid, password);
  server.begin();

  Serial.println("Tiny Camel Ready: Steer-Throttle Mode");
}

// ---------------- LOOP ----------------
void loop() {
    // 1. Capture
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return;

    // 2. Preprocess
    preprocessFrame(fb, input);

    // 3. Inference
    if (interpreter->Invoke() != kTfLiteOk) {
        Serial.println("Invoke failed");
    }

    // 4. Post-process
    TfLiteTensor* output = interpreter->output(0);
    int8_t max_score = -128;
    int pred_class = 0;
    
    // Find class (0=Forward, 1=Left, 2=Right)
    for (int i = 0; i < 3; i++) {
        if (output->data.int8[i] > max_score) {
            max_score = output->data.int8[i];
            pred_class = i;
        }
    }
    
    Serial.printf("Class: %d (Conf: %d)\n", pred_class, max_score);

    // 5. Motor Logic (Steer-Throttle Config)
    switch (pred_class) {
        case 0: // Forward
            steerStraight(); // Turn wheels straight
            moveForward();   // Apply throttle
            break;
            
        case 1: // Left
            turnLeft();      // Turn wheels Left
            moveForward();   // Apply throttle
            break;
            
        case 2: // Right
            turnRight();     // Turn wheels Right
            moveForward();   // Apply throttle
            break;
            
        default:
            stopCar();
            break;
    }

    // 6. Cleanup
    esp_camera_fb_return(fb);
}