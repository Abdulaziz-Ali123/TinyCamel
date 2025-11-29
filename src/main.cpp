#include <WiFi.h>
#include <WebServer.h>
#include <Arduino.h>
#include "esp_camera.h"
#include "tensorflow/lite/micro/all_ops_resolver.h" // Easier for debugging, switch to mutable if space is tight
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model_data.h" 

// ---------------- MEMORY CONFIG (PSRAM) ----------------
// We use 1MB of PSRAM for the arena. 
// This fits MobileNetV2 easily.
constexpr int kTensorArenaSize = 1024 * 1024; 
uint8_t* tensorArena; 

// ---------------- CAMERA PINS (XIAO ESP32S3) ----------------
// ... (Your existing pin definitions remain the same) ...
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
#define EN2 6 

// ---------------- TFLite Objects ----------------
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
// MobileNetV2 requires more Ops than your previous list.
// Using AllOpsResolver ensures we don't crash due to missing Ops like PAD or RELU6.
tflite::AllOpsResolver resolver; 

// ... (Keep your WiFi, WebServer, and Motor helper functions here) ...
const char* ssid = "ESP32-CAR2";
const char* password = "12345678";
WebServer server(80);
int pwmValue = 200;

// ---------------- PREPROCESS ----------------
// Optimized to crop center 96x96 from QVGA (320x240)
void preprocessFrame(camera_fb_t* fb, TfLiteTensor* input){
    int start_x = (fb->width - 96) / 2;
    int start_y = (fb->height - 96) / 2;
    
    for (int y = 0; y < 96; y++) {
        for (int x = 0; x < 96; x++) {
            // Calculate index in the source QVGA buffer
            int src_idx = ((start_y + y) * fb->width) + (start_x + x);
            
            // MobileNetV2 (ImageNet) expects 3 channels (RGB) even if grayscale image
            // We replicate the grayscale pixel to R, G, and B channels.
            // INT8 Quantization usually expects range [-128, 127]
            // We take uint8 (0-255) -> subtract 128 -> int8
            int8_t pixel = (int8_t)(fb->buf[src_idx] - 128); 
            
            input->data.int8[(y * 96 * 3) + (x * 3) + 0] = pixel; // R
            input->data.int8[(y * 96 * 3) + (x * 3) + 1] = pixel; // G
            input->data.int8[(y * 96 * 3) + (x * 3) + 2] = pixel; // B
        }
    }
}

void setup() {
  Serial.begin(115200);
  
  // 1. Initialize PSRAM for Tensor Arena
  if(psramFound()){
      Serial.println("PSRAM Found! Allocating Arena...");
      tensorArena = (uint8_t*)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM);
  } else {
      Serial.println("Error: PSRAM not found! Model will not fit.");
      return; 
  }

  // 2. Camera Init (Use QVGA to be safe, then crop)
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; 
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM; config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE; 
  config.frame_size = FRAMESIZE_QVGA; // 320x240
  config.jpeg_quality = 12; 
  config.fb_count = 1;
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("Camera init failed 0x%x", err); return; }

  // 3. TFLite Init
  const tflite::Model* model = tflite::GetModel(TinyCamel_Q8_tflite); // Ensure this matches your header array name
  if (model->version() != TFLITE_SCHEMA_VERSION) {
      Serial.println("Model schema mismatch!");
      return;
  }
  
  interpreter = new tflite::MicroInterpreter(model, resolver, tensorArena, kTensorArenaSize);
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
      Serial.println("AllocateTensors() failed");
      return;
  }
  input = interpreter->input(0);
  
  Serial.println("Setup Complete");
}

void loop() {
    // ... (Your WiFi handling logic) ...

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return;

    // Preprocess: Crop 96x96 from center and normalize
    preprocessFrame(fb, input);

    // Inference
    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
        Serial.println("Invoke failed");
    }

    // Output parsing (INT8)
    TfLiteTensor* output = interpreter->output(0);
    // Values are -128 to 127. The index with the highest value is the prediction.
    int8_t max_score = -128;
    int pred_class = -1;
    
    for (int i = 0; i < 3; i++) {
        if (output->data.int8[i] > max_score) {
            max_score = output->data.int8[i];
            pred_class = i;
        }
    }
    
    Serial.printf("Class: %d (Score: %d)\n", pred_class, max_score);
    

    esp_camera_fb_return(fb);
}

