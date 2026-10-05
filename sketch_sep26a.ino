#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Encoder.h>
#include <DHT.h>
#include <driver/i2s.h>
#include "driver/rtc_io.h"
#include "room_details_audio.h"
#include <ESP32Servo.h>

// ============================================================
// MASTER PIN DEFINITIONS
// ============================================================
#define WAKE_UP_PIN GPIO_NUM_4
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define ENC_CLK 5
#define ENC_DT 6
#define ENC_SW 7
#define BUZZER_PIN 10 
#define DHT_PIN 15
#define DHT_TYPE DHT11

// Signal Pin sending HIGH/LOW to the Arduino Uno
#define FLAG_SIGNAL_PIN 18

// I2S Microphone (Input - Port 0)
#define I2S_SCK 16
#define I2S_WS  11
#define I2S_SD  12
#define I2S_PORT I2S_NUM_0

// I2S Amplifier (Output - Port 1)
#define SPK_BCLK 17 
#define SPK_LRCK 21 
#define SPK_DIN  14 
#define I2S_PORT_SPK I2S_NUM_1

// ============================================================
// GLOBAL OBJECTS & VARIABLES
// ============================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
ESP32Encoder encoder;
DHT dht(DHT_PIN, DHT_TYPE);

// Config for the Microphone
i2s_config_t i2s_mic_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = 16000,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 4,
  .dma_buf_len = 512,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

i2s_pin_config_t mic_pin_config = {
  .bck_io_num = I2S_SCK,
  .ws_io_num = I2S_WS,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_SD
};

// Config for the Speaker
i2s_config_t i2s_spk_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = 16000, 
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 4,
  .dma_buf_len = 512,
  .use_apll = false,
  .tx_desc_auto_clear = true, 
  .fixed_mclk = 0
};

i2s_pin_config_t spk_pin_config = {
  .bck_io_num = SPK_BCLK,
  .ws_io_num = SPK_LRCK,
  .data_out_num = SPK_DIN,
  .data_in_num = I2S_PIN_NO_CHANGE
};

enum DeskbotState { SLEEPING_FACE, AWAKE_FACE, SETTING_TIMER, COUNTDOWN, TIME_UP };
DeskbotState currentState = AWAKE_FACE;

unsigned long lastInteractionTime = 0;
unsigned long lastTickTime = 0;
unsigned long timeUpStartTime = 0; 
const unsigned long IDLE_TIMEOUT = 900000; 

int timerMinutes = 25; 
int timerSeconds = 0;
long lastEncoderCount = 25;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;

bool showingRoomDetails = false;
unsigned long roomDetailsStartTime = 0;
float temperature = 0.0;
float humidity = 0.0;
bool sensorOK = false;

// --- Streaming Variables ---
bool isRecording = false;
int bytesSent = 0;
const int BYTES_TO_SEND = 96000; 
float RMS_NOISE_GATE = 1500.0; 
short sampleBuffer[512];

// ============================================================
// SYSTEM & DRAWING FUNCTIONS
// ============================================================
void drawAwakeFace() {
  display.clearDisplay();
  display.fillRoundRect(28, 15, 24, 24, 4, SSD1306_WHITE);
  display.fillRoundRect(76, 15, 24, 24, 4, SSD1306_WHITE);
  display.fillRect(44, 50, 40, 6, SSD1306_WHITE);
  display.display();
}

void drawSleepingFace() {
  display.clearDisplay();
  display.fillRect(28, 25, 24, 4, SSD1306_WHITE);
  display.fillRect(76, 25, 24, 4, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(95, 10);
  display.print("Z");
  display.setCursor(105, 5);
  display.print("z");
  display.display();
}

void drawTimerSetup(int mins) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(25, 10);
  display.print("Set Timer (Min)");
  display.setTextSize(3);
  display.setCursor(mins < 10 ? 55 : 45, 30);
  display.print(mins);
  display.display();
}

void drawCountdown(int mins, int secs) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(30, 10);
  display.print("Working...");
  display.setTextSize(3);
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", mins, secs);
  display.setCursor(20, 30);
  display.print(timeStr);
  display.display();
}

void drawTimeUp() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 25);
  display.print("TIME'S UP!");
  display.display();
}

void drawRoomDetails() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(28, 5);
  display.print("ROOM DETAILS");
  display.drawLine(10, 15, 118, 15, SSD1306_WHITE);

  if (!sensorOK) {
    display.setCursor(25, 35);
    display.print("Sensor Error!");
  } else {
    display.setCursor(15, 30);
    display.print("Temp:  "); display.print(temperature, 1); display.print(" C");
    display.setCursor(15, 50);
    display.print("Humid: "); display.print(humidity, 1); display.print(" %");
  }
  display.display();
}

void refreshCurrentStateUI() {
  if (currentState == SLEEPING_FACE) drawSleepingFace();
  else if (currentState == AWAKE_FACE) drawAwakeFace();
  else if (currentState == SETTING_TIMER) drawTimerSetup(timerMinutes);
  else if (currentState == COUNTDOWN) drawCountdown(timerMinutes, timerSeconds);
  else if (currentState == TIME_UP) drawTimeUp();
}

void playAudioArray() {
  size_t bytes_written;
  i2s_write(I2S_PORT_SPK, converted_wav, converted_wav_len, &bytes_written, portMAX_DELAY);
}

void triggerRoomDetails() {
  humidity = dht.readHumidity();
  temperature = dht.readTemperature();
  sensorOK = !(isnan(humidity) || isnan(temperature));
  showingRoomDetails = true;
  
  drawRoomDetails();
  playAudioArray();

  roomDetailsStartTime = millis();
  lastInteractionTime = millis(); 
}

float calculateBufferRMS(short* buffer, size_t length) {
  int64_t sumSquares = 0;
  for (size_t i = 0; i < length; i++) {
    sumSquares += (int32_t)buffer[i] * (int32_t)buffer[i];
  }
  return sqrt((float)sumSquares / (float)length);
}

void handleWakeUpEvent() {
  lastInteractionTime = millis();
  showingRoomDetails = false;
  if (currentState == SLEEPING_FACE) {
    currentState = AWAKE_FACE;
    refreshCurrentStateUI();
  }
}

// ============================================================
// MAIN SETUP
// ============================================================
void setup() {
  Serial.begin(921600); 

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 

  // Configure the signal pin to talk to the Uno
  pinMode(FLAG_SIGNAL_PIN, OUTPUT);
  digitalWrite(FLAG_SIGNAL_PIN, LOW);

  pinMode(WAKE_UP_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED failed!");
    for(;;);
  }

  ESP32Encoder::useInternalWeakPullResistors = puType::up; 
  encoder.attachHalfQuad(ENC_DT, ENC_CLK);
  encoder.setCount(25); 
  pinMode(ENC_SW, INPUT_PULLUP);

  dht.begin();
  
  // Install Mic on Port 0
  i2s_driver_install(I2S_PORT, &i2s_mic_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &mic_pin_config);
  i2s_zero_dma_buffer(I2S_PORT);

  // Install Speaker on Port 1
  i2s_driver_install(I2S_PORT_SPK, &i2s_spk_config, 0, NULL);
  i2s_set_pin(I2S_PORT_SPK, &spk_pin_config);
  i2s_zero_dma_buffer(I2S_PORT_SPK);

  lastInteractionTime = millis();
  drawAwakeFace();
  Serial.println("DeskBEE Ready.");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {

  // ==========================================
  // 1. CHECK VIBRATION SENSOR (GPIO 4) OR SERIAL
  // ==========================================
  if (digitalRead(WAKE_UP_PIN) == LOW) {
    handleWakeUpEvent();
    delay(50); 
  }

  if (Serial.available() > 0) {
    char key = Serial.read();
    
    if (key == 'z' || key == 'Z') {
      handleWakeUpEvent();
    }
    // Room details logic
    else if ((key == 'r' || key == 'R') && currentState != SLEEPING_FACE) {
      triggerRoomDetails();
    }
    // New Uno Trigger Logic
    else if (key == '1') {
      digitalWrite(FLAG_SIGNAL_PIN, HIGH);
    }
    else if (key == '0') {
      digitalWrite(FLAG_SIGNAL_PIN, LOW);
    }
  }

  // ==========================================
  // 2. I2S MICROPHONE STREAMING LOGIC
  // ==========================================
  size_t bytes_read = 0;
  i2s_read(I2S_PORT, (void*)sampleBuffer, sizeof(sampleBuffer), &bytes_read, portMAX_DELAY);

  bool isLockoutActive = (currentState == SLEEPING_FACE) || (currentState == COUNTDOWN && timerMinutes == 0) || (currentState == TIME_UP);

  if (!showingRoomDetails && !isLockoutActive) {
    if (isRecording) {
      int bytesToSend = bytes_read;
      if (bytesSent + bytesToSend > BYTES_TO_SEND) bytesToSend = BYTES_TO_SEND - bytesSent;
      
      Serial.write((uint8_t*)sampleBuffer, bytesToSend);
      bytesSent += bytesToSend;
      
      if (bytesSent >= BYTES_TO_SEND) {
        isRecording = false;
        bytesSent = 0;
      }
    } else {
      float rms = calculateBufferRMS(sampleBuffer, bytes_read / 2);
      if (rms > RMS_NOISE_GATE) {
        Serial.println("START_AUDIO"); 
        isRecording = true;
        bytesSent = 0;
        int bytesToSend = bytes_read;
        Serial.write((uint8_t*)sampleBuffer, bytesToSend);
        bytesSent += bytesToSend;
      }
    }
  }

  // ==========================================
  // 3. READ BUTTON & ENCODER
  // ==========================================
  bool buttonPressed = false;
  bool currentButton = digitalRead(ENC_SW);
  if (currentButton != lastButtonState) {
    if (millis() - lastDebounceTime > 50) { 
      if (currentButton == LOW) {
        buttonPressed = true;
        handleWakeUpEvent(); 
      }
    }
    lastDebounceTime = millis();
  }
  lastButtonState = currentButton;

  long currentEncoderCount = encoder.getCount();
  if (currentEncoderCount != lastEncoderCount) {
    handleWakeUpEvent(); 
    
    if (currentEncoderCount < 1) { encoder.setCount(1); currentEncoderCount = 1; }
    if (currentEncoderCount > 99) { encoder.setCount(99); currentEncoderCount = 99; }
    
    if (currentState == AWAKE_FACE || currentState == TIME_UP) {
      currentState = SETTING_TIMER;
      digitalWrite(BUZZER_PIN, LOW); 
    }
    timerMinutes = currentEncoderCount;
    lastEncoderCount = currentEncoderCount;
    if (currentState == SETTING_TIMER) drawTimerSetup(timerMinutes);
  }

  // ==========================================
  // 4. OVERLAYS & STATE MACHINE
  // ==========================================
  if (showingRoomDetails && (millis() - roomDetailsStartTime >= 5000)) {
    showingRoomDetails = false;
    refreshCurrentStateUI(); 
  }

  switch (currentState) {
    case SLEEPING_FACE:
      break;

    case AWAKE_FACE:
    case SETTING_TIMER:
      if (millis() - lastInteractionTime > IDLE_TIMEOUT) {
        currentState = SLEEPING_FACE;
        refreshCurrentStateUI();
      }
      
      if (currentState == SETTING_TIMER && buttonPressed) {
        timerSeconds = 0;
        currentState = COUNTDOWN;
        if (!showingRoomDetails) drawCountdown(timerMinutes, timerSeconds);
        lastTickTime = millis();
      }
      break;

    case COUNTDOWN:
      if (millis() - lastTickTime >= 1000) {
        lastTickTime = millis();
        if (timerSeconds == 0) {
          if (timerMinutes == 0) {
            digitalWrite(BUZZER_PIN, HIGH); 
            currentState = TIME_UP;
            showingRoomDetails = false;
            drawTimeUp();
            timeUpStartTime = millis(); 
          } else {
            timerMinutes--;
            timerSeconds = 59;
            if (!showingRoomDetails) drawCountdown(timerMinutes, timerSeconds);
          }
        } else {
          timerSeconds--;
          if (!showingRoomDetails) drawCountdown(timerMinutes, timerSeconds);
        }
        if (timerMinutes == 1 && timerSeconds == 0) digitalWrite(BUZZER_PIN, HIGH); 
        else if (timerMinutes == 0 && timerSeconds == 59) digitalWrite(BUZZER_PIN, LOW);  
        else if (timerMinutes == 0 && timerSeconds == 30) digitalWrite(BUZZER_PIN, HIGH); 
        else if (timerMinutes == 0 && timerSeconds == 29) digitalWrite(BUZZER_PIN, LOW);  
      }
      if (buttonPressed) {
        digitalWrite(BUZZER_PIN, LOW); 
        currentState = AWAKE_FACE;
        if (!showingRoomDetails) drawAwakeFace();
        encoder.setCount(25); 
        lastInteractionTime = millis(); 
      }
      break;

    case TIME_UP:
      if (millis() - timeUpStartTime >= 3000 || buttonPressed) {
        digitalWrite(BUZZER_PIN, LOW); 
        currentState = AWAKE_FACE;
        refreshCurrentStateUI();
        encoder.setCount(25);
        lastInteractionTime = millis(); 
      }
      break;
  }
}