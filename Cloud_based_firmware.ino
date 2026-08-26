#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h> // For sine wave generation

// --- Cloud Server Settings ---
const char* server_url = "my-ai-api.com"; // Your Cloud domain or ngrok URL
const int server_port = 443;              // 443 is standard for secure cloud (WSS)

// --- Pin Definitions ---
#define PTT_BTN_PIN    15   
#define RESET_BTN_PIN  4    
#define SD_PIN         18   

// I2S Speaker Pins (TX)
#define I2S_SPK_BCLK   5
#define I2S_SPK_LRC    6
#define I2S_SPK_DOUT   7

// I2S Microphone Pins (RX)
#define I2S_MIC_BCLK   12
#define I2S_MIC_LRC    13
#define I2S_MIC_DIN    14

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
// --- OLED Setup (ESP32-S3 Specific) ---
#define I2C_SDA        8    
#define I2C_SCL        9    
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

WebSocketsClient webSocket;
WiFiManager wm;

enum AIState { IDLE, LISTENING, SPEAKING };
AIState currentState = IDLE;
unsigned long lastBlinkTime = 0;
bool isBlinking = false;
bool isRecording = false; 

// Track connection states to play alerts only once
bool wasWifiConnected = false;
bool wasServerConnected = false;

// --- System Audio Synthesizer (Runs Locally) ---
void playTone(float frequency, int duration_ms) {
    int sample_rate = 24000;
    int num_samples = (sample_rate * duration_ms) / 1000;
    int16_t sample[2]; // Stereo buffer, but we use mono channel format
    size_t bytes_written;
    float amplitude = 8000.0; // Adjust volume (Max 32767)

    for (int i = 0; i < num_samples; i++) {
        int16_t val = (int16_t)(amplitude * sin(2.0 * PI * frequency * i / sample_rate));
        sample[0] = val; // Left
        sample[1] = val; // Right
        i2s_write(I2S_NUM_0, sample, sizeof(sample), &bytes_written, portMAX_DELAY);
    }
}

// Chime Presets
void chimeBoot()       { playTone(440, 150); playTone(554, 150); playTone(659, 300); } // Rising A Major
void chimeWifiConn()   { playTone(523, 100); playTone(784, 250); }                     // Happy C-G
void chimeServerConn() { playTone(784, 100); playTone(1046, 250); }                    // High G-C
void chimeError()      { playTone(440, 200); playTone(349, 400); }                     // Falling sad tone

// --- I2S Initialization ---
void setupI2S() {
    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);

    i2s_config_t tx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 24000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // Use standard stereo config for math tones
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t tx_pins = {
        .bck_io_num = I2S_SPK_BCLK,
        .ws_io_num = I2S_SPK_LRC,
        .data_out_num = I2S_SPK_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_0, &tx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &tx_pins);

    i2s_config_t rx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false
    };
    i2s_pin_config_t rx_pins = {
        .bck_io_num = I2S_MIC_BCLK,
        .ws_io_num = I2S_MIC_LRC,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_DIN
    };
    i2s_driver_install(I2S_NUM_1, &rx_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &rx_pins);
}

// --- Face Rendering ---
void updateFace() {
    display.clearDisplay();
    int centerX1 = 40, centerX2 = 88, centerY = 32;

    if (currentState == LISTENING) {
        display.fillCircle(centerX1, centerY, 18, SSD1306_WHITE);
        display.fillCircle(centerX2, centerY, 18, SSD1306_WHITE);
        display.fillCircle(centerX1, centerY - 6, 6, SSD1306_BLACK);
        display.fillCircle(centerX2, centerY - 6, 6, SSD1306_BLACK);
    } 
    else if (currentState == SPEAKING) {
        display.drawCircleHelper(centerX1, centerY + 5, 15, 1, SSD1306_WHITE);
        display.drawCircleHelper(centerX1, centerY + 5, 15, 2, SSD1306_WHITE);
        display.drawCircleHelper(centerX2, centerY + 5, 15, 1, SSD1306_WHITE);
        display.drawCircleHelper(centerX2, centerY + 5, 15, 2, SSD1306_WHITE);
    } 
    else {
        if (millis() - lastBlinkTime > 4000) { isBlinking = true; lastBlinkTime = millis(); }
        if (isBlinking && millis() - lastBlinkTime > 150) { isBlinking = false; }
        if (isBlinking) {
            display.fillRect(centerX1 - 12, centerY, 24, 4, SSD1306_WHITE);
            display.fillRect(centerX2 - 12, centerY, 24, 4, SSD1306_WHITE);
        } else {
            display.fillCircle(centerX1, centerY, 12, SSD1306_WHITE);
            display.fillCircle(centerX2, centerY, 12, SSD1306_WHITE);
        }
    }
    display.display();
}

// --- WebSocket Event Handler ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            if (wasServerConnected) {
                Serial.println("[🔴] Server Disconnected");
                chimeError(); // Play sad tone
                wasServerConnected = false;
            }
            currentState = IDLE;
            break;

        case WStype_CONNECTED: {
            Serial.println("[🟢] Connected to Cloud Server!");
            chimeServerConn(); // Play high happy tone
            wasServerConnected = true;
            String regMsg = "REGISTER_DEVICE:" + WiFi.macAddress();
            webSocket.sendTXT(regMsg);
            break;
        }

        case WStype_BIN: {
            currentState = SPEAKING; 
            int16_t* pcm_samples = (int16_t*)payload;
            int num_samples = length / 2;
            float multiplier = 2.0; 
            
            for (int i = 0; i < num_samples; i++) {
                int32_t amplified = pcm_samples[i] * multiplier;
                if (amplified > 32767) amplified = 32767;
                if (amplified < -32768) amplified = -32768;
                
                // Duplicate mono AI voice to both stereo channels
                int16_t stereo[2] = {(int16_t)amplified, (int16_t)amplified};
                size_t bytes_written;
                i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &bytes_written, portMAX_DELAY);
            }
            break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(PTT_BTN_PIN, INPUT_PULLUP);
    pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    Wire.begin(I2C_SDA, I2C_SCL);
    
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println("[⚠️] OLED failed"); }
    display.clearDisplay(); display.display();

    setupI2S();
    chimeBoot(); // Announce hardware is ON

    if (digitalRead(RESET_BTN_PIN) == LOW) {
        wm.resetSettings();
        delay(1000);
    }

    wm.autoConnect("ESP32-Audio-Setup", "12345678");

    // Secure WebSocket Connection for Cloud environments
    webSocket.beginSSL(server_url, server_port, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}

void loop() {
    webSocket.loop();
    updateFace();

    // Track Wi-Fi State Changes
    if (WiFi.status() == WL_CONNECTED && !wasWifiConnected) {
        chimeWifiConn();
        wasWifiConnected = true;
    } else if (WiFi.status() != WL_CONNECTED && wasWifiConnected) {
        chimeError();
        wasWifiConnected = false;
    }

    if (digitalRead(RESET_BTN_PIN) == LOW) {
        delay(2000); 
        if (digitalRead(RESET_BTN_PIN) == LOW) { wm.resetSettings(); ESP.restart(); }
    }

    // PTT Logic
    bool buttonPressed = (digitalRead(PTT_BTN_PIN) == LOW);
    if (buttonPressed && !isRecording && wasServerConnected) {
        isRecording = true;
        currentState = LISTENING;
        webSocket.sendTXT("START_RECORDING");
    } 
    else if (!buttonPressed && isRecording) {
        isRecording = false;
        currentState = IDLE;
        webSocket.sendTXT("STOP_RECORDING");
    }

    if (isRecording) {
        size_t bytes_read = 0;
        uint8_t mic_buffer[1024];
        i2s_read(I2S_NUM_1, mic_buffer, sizeof(mic_buffer), &bytes_read, portMAX_DELAY);
        if (bytes_read > 0) webSocket.sendBIN(mic_buffer, bytes_read);
    }
}
