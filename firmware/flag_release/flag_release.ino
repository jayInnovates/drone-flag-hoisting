/*
  Drone-based Flag Hoisting System - flag release controller
  -----------------------------------------------------------
  Board   : ESP8266 (NodeMCU / ESP-12E)
  Actuator: hobby servo driving the latch that holds the furled flag
  Control : Firebase Realtime Database over Wi-Fi

  The servo latch is driven by one integer in the database:

      /flag/state = 0  -> CLOSED (flag held furled under the drone)
      /flag/state = 1  -> OPEN   (latch releases, flag unfurls)

  The operator flips the value from the Firebase console while the drone
  hovers over the venue. The controller is independent of the flight
  controller: it only moves the latch.

  Fail-safe behaviour
  - Boots CLOSED and stays CLOSED until it reads an explicit 1.
  - Read errors, Wi-Fi loss or unexpected values never open the latch;
    the servo simply holds its last commanded position.
  - Setting the value back to 0 closes the latch again (for ground resets).

  Libraries (Arduino IDE / arduino-cli)
  - ESP8266 Arduino core (includes ESP8266WiFi and Servo)
  - "Firebase Arduino Client Library for ESP8266 and ESP32" by Mobizt
    (header: Firebase_ESP_Client.h)

  Copy secrets.example.h to secrets.h and fill in your own values.
  secrets.h is git-ignored so credentials never reach the repository.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Servo.h>
#include <Firebase_ESP_Client.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD, DATABASE_URL, DATABASE_SECRET

// ---------- hardware ----------
const uint8_t SERVO_PIN = D5;          // GPIO14
const int     ANGLE_CLOSED = 0;        // latch locked  (tune on the bench)
const int     ANGLE_OPEN   = 90;       // latch released (tune on the bench)

// ---------- database ----------
const char *STATE_PATH = "/flag/state";   // written by the operator: 0 or 1
const char *ACK_PATH   = "/flag/servo";   // written by the board: position it applied

const unsigned long POLL_MS = 250;     // how often to read the command

Servo latch;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

int appliedState = 0;                  // what the servo is doing now
unsigned long lastPoll = 0;

void applyState(int state) {
  latch.write(state == 1 ? ANGLE_OPEN : ANGLE_CLOSED);
  appliedState = state;
  Serial.printf("[latch] %s\n", state == 1 ? "OPEN - flag released" : "CLOSED - flag held");
  // Echo the applied position so the operator can confirm it in the console.
  if (!Firebase.RTDB.setInt(&fbdo, ACK_PATH, appliedState)) {
    Serial.printf("[firebase] ack failed: %s\n", fbdo.errorReason().c_str());
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[wifi] connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print('.');
  }
  Serial.printf("\n[wifi] connected, IP %s, RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // Lock the latch before anything else, so a slow network can never
  // leave the flag unsecured.
  latch.attach(SERVO_PIN, 500, 2400);
  latch.write(ANGLE_CLOSED);

  connectWiFi();

  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;
  Firebase.reconnectWiFi(true);
  fbdo.setBSSLBufferSize(4096, 1024);   // keeps TLS within ESP8266 RAM
  fbdo.setResponseSize(1024);
  Firebase.begin(&config, &auth);

  applyState(0);                        // publish CLOSED as the starting state
}

void loop() {
  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();

  if (!Firebase.ready()) return;

  if (Firebase.RTDB.getInt(&fbdo, STATE_PATH)) {
    int cmd = fbdo.to<int>();
    if ((cmd == 0 || cmd == 1) && cmd != appliedState) {
      applyState(cmd);
    }
    // Any other value is ignored: the latch keeps its last position.
  } else {
    // Network or database error: hold position, report, try again next poll.
    Serial.printf("[firebase] read failed: %s\n", fbdo.errorReason().c_str());
  }
}
