#define BLYNK_TEMPLATE_ID "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "PostureSense"
#define BLYNK_AUTH_TOKEN "YOUR_AUTH_TOKEN"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// ==============================
// Wi-Fi
// ==============================

const char* ssid = "iQOO Z10 5G";
const char* password = "YOUR_HOTSPOT_PASSWORD";

// ==============================
// OLED
// ==============================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

// ==============================
// Pins
// ==============================

#define TRIG_PIN 5
#define ECHO_PIN 18
#define BUZZER_PIN 23
#define GREEN_LED 25
#define RED_LED 26

// ==============================
// Posture thresholds
// ==============================

#define CLOSE_ENTER 39.0
#define CLOSE_EXIT 41.0

#define FAR_ENTER 56.0
#define FAR_EXIT 54.0

// ==============================
// Timing
// ==============================

const unsigned long SENSOR_INTERVAL = 400;

const unsigned long BEEP_INTERVAL = 1000;
const unsigned long BEEP_DURATION = 200;

const unsigned long SAVE_INTERVAL = 60000;

// Notification settings
const unsigned long NOTIFICATION_CONFIRM_TIME = 4000;
const unsigned long NOTIFICATION_COOLDOWN = 30000;

// ==============================
// Posture states
// ==============================

enum PostureState {
  GOOD,
  TOO_CLOSE,
  TOO_FAR,
  SENSOR_ERROR
};

PostureState currentState = SENSOR_ERROR;

// ==============================
// Timers
// ==============================

unsigned long lastSensorTime = 0;
unsigned long lastBlynkTime = 0;

// ==============================
// Buzzer
// ==============================

unsigned long lastBeepTime = 0;
unsigned long beepStartTime = 0;

bool buzzerActive = false;

// ==============================
// Statistics
// ==============================

Preferences preferences;

unsigned long long goodTime = 0;
unsigned long long tooCloseTime = 0;
unsigned long long tooFarTime = 0;

unsigned long lastStatsTime = 0;
unsigned long lastSaveTime = 0;

// ==============================
// Notification variables
// ==============================

unsigned long badPostureStartTime = 0;

bool badPostureTimerActive = false;
bool notificationSentForCurrentPosture = false;

unsigned long lastNotificationTime = 0;
bool notificationCooldownActive = false;

// ==============================
// Sensor
// ==============================

float currentDistance = -1;

// ==============================
// Blynk Timer
// ==============================

BlynkTimer timer;


// =====================================================
// BUZZER FUNCTIONS
// =====================================================

void buzzerOn() {
  digitalWrite(BUZZER_PIN, LOW);
  buzzerActive = true;
  beepStartTime = millis();
}

void buzzerOff() {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerActive = false;
}


// =====================================================
// READ DISTANCE
// =====================================================

float readDistanceCm() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration =
    pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1;
  }

  float distance =
    duration * 0.0343 / 2.0;

  return distance;
}


// =====================================================
// FILTERED DISTANCE
// =====================================================

float getFilteredDistance() {

  const int NUM_READINGS = 5;

  float readings[NUM_READINGS];

  int validCount = 0;

  for (int i = 0; i < NUM_READINGS; i++) {

    float distance = readDistanceCm();

    if (distance > 0) {

      readings[validCount] = distance;
      validCount++;
    }

    delay(40);
  }

  if (validCount == 0) {
    return -1;
  }

  // Sort readings
  for (int i = 0; i < validCount - 1; i++) {

    for (int j = i + 1; j < validCount; j++) {

      if (readings[j] < readings[i]) {

        float temp = readings[i];

        readings[i] = readings[j];
        readings[j] = temp;
      }
    }
  }

  // Median
  if (validCount % 2 == 1) {

    return readings[validCount / 2];

  } else {

    return (
      readings[validCount / 2 - 1] +
      readings[validCount / 2]
    ) / 2.0;
  }
}


// =====================================================
// NOTIFICATION FUNCTION
// =====================================================

void sendPostureNotification(PostureState state) {

  unsigned long now = millis();

  // Check cooldown
  if (notificationCooldownActive) {

    if (now - lastNotificationTime <
        NOTIFICATION_COOLDOWN) {

      Serial.println(
        "Notification blocked by 30-second cooldown."
      );

      return;
    }

    notificationCooldownActive = false;
  }


  // TOO CLOSE
  if (state == TOO_CLOSE) {

    Serial.println();
    Serial.println(
      ">>> SENDING TOO CLOSE NOTIFICATION <<<"
    );

    Blynk.logEvent(
      "posture_too_close",
      "You are sitting too close. Please move back."
    );

    Serial.println(
      "Blynk event: posture_too_close"
    );

    lastNotificationTime = now;

    notificationCooldownActive = true;

    notificationSentForCurrentPosture = true;
  }


  // TOO FAR
  else if (state == TOO_FAR) {

    Serial.println();
    Serial.println(
      ">>> SENDING TOO FAR NOTIFICATION <<<"
    );

    Blynk.logEvent(
      "posture_too_far",
      "You are sitting too far. Please move closer."
    );

    Serial.println(
      "Blynk event: posture_too_far"
    );

    lastNotificationTime = now;

    notificationCooldownActive = true;

    notificationSentForCurrentPosture = true;
  }
}


// =====================================================
// UPDATE POSTURE
// =====================================================

void updatePosture(float distance) {

  PostureState oldState = currentState;


  // Sensor error
  if (distance < 0) {

    currentState = SENSOR_ERROR;
  }

  else {

    switch (currentState) {

      // --------------------------
      // GOOD
      // --------------------------

      case GOOD:

        if (distance < CLOSE_ENTER) {

          currentState = TOO_CLOSE;
        }

        else if (distance > FAR_ENTER) {

          currentState = TOO_FAR;
        }

        break;


      // --------------------------
      // TOO CLOSE
      // --------------------------

      case TOO_CLOSE:

        if (distance >= CLOSE_EXIT) {

          currentState = GOOD;
        }

        break;


      // --------------------------
      // TOO FAR
      // --------------------------

      case TOO_FAR:

        if (distance <= FAR_EXIT) {

          currentState = GOOD;
        }

        break;


      // --------------------------
      // SENSOR ERROR
      // --------------------------

      case SENSOR_ERROR:

        if (distance < CLOSE_ENTER) {

          currentState = TOO_CLOSE;
        }

        else if (distance > FAR_ENTER) {

          currentState = TOO_FAR;
        }

        else {

          currentState = GOOD;
        }

        break;
    }
  }


  // =================================================
  // POSTURE CHANGED
  // =================================================

  if (currentState != oldState) {

    Serial.println();

    Serial.print("Posture changed: ");

    if (currentState == GOOD) {

      Serial.println("GOOD");
    }

    else if (currentState == TOO_CLOSE) {

      Serial.println("TOO CLOSE");
    }

    else if (currentState == TOO_FAR) {

      Serial.println("TOO FAR");
    }

    else {

      Serial.println("SENSOR ERROR");
    }
  }


  // =================================================
  // GOOD
  // =================================================

  if (currentState == GOOD) {

    if (badPostureTimerActive) {

      Serial.println(
        "Bad posture timer RESET."
      );
    }

    badPostureTimerActive = false;

    notificationSentForCurrentPosture = false;
  }


  // =================================================
  // SENSOR ERROR
  // =================================================

  else if (currentState == SENSOR_ERROR) {

    badPostureTimerActive = false;

    notificationSentForCurrentPosture = false;
  }


  // =================================================
  // TOO CLOSE / TOO FAR
  // =================================================

  else if (
    currentState == TOO_CLOSE ||
    currentState == TOO_FAR
  ) {

    // Start timer if not already running
    if (!badPostureTimerActive) {

      badPostureStartTime = millis();

      badPostureTimerActive = true;

      Serial.println(
        "Bad posture timer STARTED."
      );
    }


    // If notification has already been sent
    if (notificationSentForCurrentPosture) {

      return;
    }


    unsigned long badPostureDuration =
      millis() - badPostureStartTime;


    // Print progress every sensor cycle
    Serial.print(
      "Bad posture duration: "
    );

    Serial.print(
      badPostureDuration / 1000.0
    );

    Serial.println(" sec");


    // =================================================
    // 4 SECOND CONFIRMATION
    // =================================================

    if (
      badPostureDuration >=
      NOTIFICATION_CONFIRM_TIME
    ) {

      sendPostureNotification(currentState);
    }
  }
}


// =====================================================
// UPDATE OLED
// =====================================================

void updateOLED(float distance) {

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);


  // Distance
  display.setTextSize(1);

  display.setCursor(0, 0);

  display.print("Distance: ");

  if (distance < 0) {

    display.println("--");
  }

  else {

    display.print(distance, 1);
    display.println(" cm");
  }


  // Posture
  display.setCursor(0, 18);

  display.setTextSize(1);

  display.print("Posture:");


  display.setTextSize(2);

  display.setCursor(0, 30);


  if (currentState == GOOD) {

    display.println("GOOD");
  }

  else if (currentState == TOO_CLOSE) {

    display.println("TOO CLOSE");
  }

  else if (currentState == TOO_FAR) {

    display.println("TOO FAR");
  }

  else {

    display.println("ERROR");
  }


  display.display();
}


// =====================================================
// LED
// =====================================================

void updateLEDs() {

  if (currentState == GOOD) {

    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(RED_LED, LOW);
  }

  else if (
    currentState == TOO_CLOSE ||
    currentState == TOO_FAR
  ) {

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, HIGH);
  }

  else {

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, LOW);
  }
}


// =====================================================
// BUZZER UPDATE
// =====================================================

void updateBuzzer() {

  unsigned long now = millis();


  // GOOD / ERROR
  if (
    currentState == GOOD ||
    currentState == SENSOR_ERROR
  ) {

    buzzerOff();

    return;
  }


  // Start beep
  if (!buzzerActive) {

    if (
      now - lastBeepTime >=
      BEEP_INTERVAL
    ) {

      lastBeepTime = now;

      buzzerOn();
    }
  }


  // Stop beep
  else {

    if (
      now - beepStartTime >=
      BEEP_DURATION
    ) {

      buzzerOff();
    }
  }
}


// =====================================================
// LOAD STATISTICS
// =====================================================

void loadStatistics() {

  preferences.begin(
    "posture",
    false
  );

  goodTime =
    preferences.getULong64(
      "good",
      0
    );

  tooCloseTime =
    preferences.getULong64(
      "close",
      0
    );

  tooFarTime =
    preferences.getULong64(
      "far",
      0
    );

  preferences.end();


  Serial.println(
    "Statistics loaded."
  );

  Serial.print("Good time: ");
  Serial.println(
    (unsigned long long)goodTime
  );

  Serial.print("Too close: ");
  Serial.println(
    (unsigned long long)tooCloseTime
  );

  Serial.print("Too far: ");
  Serial.println(
    (unsigned long long)tooFarTime
  );
}


// =====================================================
// SAVE STATISTICS
// =====================================================

void saveStatistics() {

  preferences.begin(
    "posture",
    false
  );

  preferences.putULong64(
    "good",
    goodTime
  );

  preferences.putULong64(
    "close",
    tooCloseTime
  );

  preferences.putULong64(
    "far",
    tooFarTime
  );

  preferences.end();


  Serial.println(
    "Statistics saved."
  );
}


// =====================================================
// UPDATE STATISTICS
// =====================================================

void updateStatistics() {

  unsigned long now = millis();


  if (lastStatsTime == 0) {

    lastStatsTime = now;

    return;
  }


  unsigned long elapsed =
    now - lastStatsTime;

  lastStatsTime = now;


  switch (currentState) {

    case GOOD:

      goodTime += elapsed;

      break;


    case TOO_CLOSE:

      tooCloseTime += elapsed;

      break;


    case TOO_FAR:

      tooFarTime += elapsed;

      break;


    case SENSOR_ERROR:

      break;
  }


  // Save every 60 seconds
  if (
    now - lastSaveTime >=
    SAVE_INTERVAL
  ) {

    saveStatistics();

    lastSaveTime = now;
  }
}


// =====================================================
// SEND DATA TO BLYNK
// =====================================================

void sendBlynkData() {

  // Distance
  if (currentDistance >= 0) {

    Blynk.virtualWrite(
      V0,
      currentDistance
    );
  }


  // Posture
  if (currentState == GOOD) {

    Blynk.virtualWrite(
      V1,
      "GOOD"
    );
  }

  else if (currentState == TOO_CLOSE) {

    Blynk.virtualWrite(
      V1,
      "TOO CLOSE"
    );
  }

  else if (currentState == TOO_FAR) {

    Blynk.virtualWrite(
      V1,
      "TOO FAR"
    );
  }

  else {

    Blynk.virtualWrite(
      V1,
      "SENSOR ERROR"
    );
  }


  // Wi-Fi status
  if (WiFi.status() == WL_CONNECTED) {

    Blynk.virtualWrite(
      V2,
      "Connected"
    );
  }

  else {

    Blynk.virtualWrite(
      V2,
      "Disconnected"
    );
  }


  // Statistics
  unsigned long long totalTime =
    goodTime +
    tooCloseTime +
    tooFarTime;


  if (totalTime > 0) {

    float goodPercent =
      (goodTime * 100.0) /
      totalTime;

    float closePercent =
      (tooCloseTime * 100.0) /
      totalTime;

    float farPercent =
      (tooFarTime * 100.0) /
      totalTime;


    Blynk.virtualWrite(
      V3,
      goodPercent
    );

    Blynk.virtualWrite(
      V4,
      closePercent
    );

    Blynk.virtualWrite(
      V5,
      farPercent
    );
  }
}


// =====================================================
// RESET STATISTICS
// =====================================================

void resetStatistics() {

  goodTime = 0;
  tooCloseTime = 0;
  tooFarTime = 0;


  preferences.begin(
    "posture",
    false
  );

  preferences.putULong64(
    "good",
    0
  );

  preferences.putULong64(
    "close",
    0
  );

  preferences.putULong64(
    "far",
    0
  );

  preferences.end();


  lastStatsTime = millis();
  lastSaveTime = millis();


  Serial.println();
  Serial.println(
    "=============================="
  );

  Serial.println(
    "     STATISTICS RESET"
  );

  Serial.println(
    "=============================="
  );

  Serial.println();

  Serial.println(
    "Good time: 0"
  );

  Serial.println(
    "Too close: 0"
  );

  Serial.println(
    "Too far: 0"
  );


  Blynk.virtualWrite(
    V3,
    0
  );

  Blynk.virtualWrite(
    V4,
    0
  );

  Blynk.virtualWrite(
    V5,
    0
  );

  Blynk.virtualWrite(
    V6,
    0
  );
}


// =====================================================
// BLYNK RESET BUTTON
// =====================================================

BLYNK_WRITE(V6) {

  int resetValue =
    param.asInt();


  if (resetValue == 1) {

    resetStatistics();
  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(2000);


  Serial.println();
  Serial.println(
    "=============================="
  );

  Serial.println(
    "      PostureSense Boot"
  );

  Serial.println(
    "=============================="
  );


  // Pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);


  buzzerOff();

  digitalWrite(
    GREEN_LED,
    LOW
  );

  digitalWrite(
    RED_LED,
    LOW
  );


  // OLED
  Wire.begin(
    21,
    22
  );


  if (
    !display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDRESS
    )
  ) {

    Serial.println(
      "OLED initialization failed!"
    );
  }

  else {

    Serial.println(
      "OLED initialized."
    );


    display.clearDisplay();

    display.setTextColor(
      SSD1306_WHITE
    );

    display.setTextSize(2);

    display.setCursor(10, 20);

    display.println(
      "Booting..."
    );

    display.display();
  }


  // Statistics
  loadStatistics();


  // Wi-Fi
  Serial.println();

  Serial.println(
    "Connecting to WiFi..."
  );


  WiFi.begin(
    ssid,
    password
  );


  while (
    WiFi.status() != WL_CONNECTED
  ) {

    delay(500);

    Serial.print(".");
  }


  Serial.println();

  Serial.println(
    "WiFi connected."
  );

  Serial.print(
    "IP address: "
  );

  Serial.println(
    WiFi.localIP()
  );


  // OLED Wi-Fi
  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(0, 20);

  display.println(
    "WiFi Connected"
  );

  display.display();


  // Blynk
  Serial.println(
    "Connecting to Blynk..."
  );


  Blynk.begin(
    BLYNK_AUTH_TOKEN,
    ssid,
    password
  );


  Serial.println();

  Serial.println(
    "Blynk connected."
  );


  // OLED
  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(0, 15);

  display.println(
    "WiFi Connected"
  );

  display.setCursor(0, 30);

  display.println(
    "Blynk Connected"
  );

  display.setCursor(0, 45);

  display.println(
    "Monitoring..."
  );

  display.display();


  // Initialize statistics timer
  lastStatsTime = millis();
  lastSaveTime = millis();


  // Blynk data timer
  timer.setInterval(
    2000L,
    sendBlynkData
  );


  Serial.println();

  Serial.println(
    "=============================="
  );

  Serial.println(
    "     Monitoring Started"
  );

  Serial.println(
    "=============================="
  );

  Serial.println();
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  Blynk.run();

  timer.run();


  unsigned long now = millis();


  // =================================================
  // SENSOR UPDATE
  // =================================================

  if (
    now - lastSensorTime >=
    SENSOR_INTERVAL
  ) {

    lastSensorTime = now;


    currentDistance =
      getFilteredDistance();


    Serial.print(
      "Distance: "
    );


    if (currentDistance < 0) {

      Serial.println(
        "SENSOR ERROR"
      );
    }

    else {

      Serial.print(
        currentDistance,
        1
      );

      Serial.println(
        " cm"
      );
    }


    // Update posture
    updatePosture(
      currentDistance
    );


    // OLED
    updateOLED(
      currentDistance
    );


    // LEDs
    updateLEDs();
  }


  // =================================================
  // BUZZER
  // =================================================

  updateBuzzer();


  // =================================================
  // STATISTICS
  // ============================

  updateStatistics();
}
