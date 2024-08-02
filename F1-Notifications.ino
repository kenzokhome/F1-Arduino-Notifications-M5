// ----------------------------
// Library Defines - Need to be defined before library import
// ----------------------------

#define ESP_DRD_USE_SPIFFS true

// ----------------------------
// Standard Libraries
// ----------------------------

#include <WiFi.h>

#include <WiFiClientSecure.h>

#include <FS.h>
#include "SPIFFS.h"

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

#include <WiFiManager.h>
// Captive portal for configuring the WiFi

// If installing from the library manager (Search for "WifiManager")
// https://github.com/tzapu/WiFiManager

#include <ESP_DoubleResetDetector.h>
// A library for checking if the reset button has been pressed twice
// Can be used to enable config mode
// Can be installed from the library manager (Search for "ESP_DoubleResetDetector")
// https://github.com/khoih-prog/ESP_DoubleResetDetector

#include <ArduinoJson.h>
// Library used for parsing Json from the API responses

// Search for "Arduino Json" in the Arduino Library manager
// https://github.com/bblanchon/ArduinoJson

#include <ezTime.h>
// Library used for getting the time and converting session time
// to users timezone

// Search for "ezTime" in the Arduino Library manager
// https://github.com/ropg/ezTime

#include <UniversalTelegramBot.h>
// Library used to send Telegram Message

// Search for "Universal Telegram" in the Arduino Library manager
// https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot

#include <FileFetcher.h>
// Library used to get files or images

// Not on library manager yet
// https://github.com/witnessmenow/file-fetcher-arduino

// ----------------------------
// Internal includes
// ----------------------------

#include "githubCert.h"

#include "display.h"

#include "config.h"

#include "raceLogic.h"

#include "wifiManagerHandler.h"

WiFiClientSecure secured_client;

FileFetcher fileFetcher(secured_client);

// ----------------------------
// Display Handling Code
// ----------------------------

#include "m5LCD.h"
M5StackDisplay cyd;
F1Display *f1Display = &cyd;

// ----------------------------

UniversalTelegramBot bot("", secured_client);

F1Config f1Config;

void setup() {
  // put your setup code here, to run once:

  Serial.begin(115200);

  f1Display->displaySetup();

  bool forceConfig = false;

  drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
  if (drd->detectDoubleReset()) {
    Serial.println(F("Forcing config mode as there was a Double reset detected"));
    forceConfig = true;
  }

  // Initialise SPIFFS, if this fails try .begin(true)
  // NOTE: I believe this formats it though it will erase everything on
  // spiffs already! In this example that is not a problem.
  // I have found once I used the true flag once, I could use it
  // without the true flag after that.
  bool spiffsInitSuccess = SPIFFS.begin(false) || SPIFFS.begin(true);
  if (!spiffsInitSuccess) {
    Serial.println("SPIFFS initialisation failed!");
    while (1)
      yield();  // Stay here twiddling thumbs waiting
  }
  Serial.println("\r\nInitialisation done.");

  if (!f1Config.fetchConfigFile()) {
    // Failed to fetch config file, need to launch Wifi Manager
    forceConfig = true;
  }

  setupWiFiManager(forceConfig, f1Config, f1Display);
  raceLogicSetup(f1Config);
  bot.updateToken(f1Config.botToken);

  // Set WiFi to station mode and disconnect from an AP if it was Previously
  // connected
  // WiFi.mode(WIFI_STA);
  // WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  secured_client.setCACert(github_server_cert);
  while (fetchRaceJson(fileFetcher) != 1) {
    Serial.println("failed to get Race Json");
    Serial.println("will try again in 10 seconds");
    delay(1000 * 10);
  }

  Serial.println("Fetched races.json File");

  Serial.println("Waiting for time sync");

  waitForSync();

  Serial.println();
  Serial.println("UTC:             " + UTC.dateTime());

  myTZ.setLocation(f1Config.timeZone);
  Serial.print(f1Config.timeZone);
  Serial.print(F(":     "));
  Serial.println(myTZ.dateTime());
  Serial.println("-------------------------");

  // sendNotificationOfNextRace(&bot, f1Config.roundOffset);
}

bool notificaitonEventRaised = false;

void sendNotification() {
  // Cause it could be set to the image one
  if (f1Config.isTelegramConfigured()) {
    secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    Serial.println("Sending notifcation");
    f1Config.currentRaceNotification = sendNotificationOfNextRace(&bot);
    if (!f1Config.currentRaceNotification) {
      // Notificaiton failed, raise event again
      Serial.println("Notfication failed");
      setEvent(sendNotification, getNotifyTime());
    } else {
      notificaitonEventRaised = false;
      f1Config.saveConfigFile();
    }
  } else {

    Serial.println("Would have sent Notification now, but telegram is not configured");

    notificaitonEventRaised = false;
    f1Config.currentRaceNotification = true;
    f1Config.saveConfigFile();
  }
}

bool first = true;

int minuteCounter = 60;  // kick off fetch first time

void loop() {
  delay(10);
  // Update the button state
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
    if (!backlightEnabled) {
      // Enable the backlight
      M5.Lcd.setBrightness(255);
      backlightEnabled = true;
      Serial.println("Backlight enabled");
    }

    // Update the last button press time
    lastButtonPressTime = millis();
  }
  M5.update();
  // Check if the backlight timeout has been reached
  if (backlightEnabled && (millis() - lastButtonPressTime >= backlightTimeout)) {
    // Disable the backlight
    M5.Lcd.setBrightness(0);
    backlightEnabled = false;
    Serial.println("Backlight disabled due to timeout");
  }
  drd->loop();

  // Every hour we will refresh the Race JSON from Github
  if (minuteCounter >= 60) {
    secured_client.setCACert(github_server_cert);
    while (fetchRaceJson(fileFetcher) != 1) {
      Serial.println("failed to get Race Json");
      Serial.println("will try again in 10 seconds");
      delay(1000 * 10);
    }
    minuteCounter = 0;
  }

  if (first || minuteChanged()) {
    minuteCounter++;
    bool newRace = getNextRace(f1Config.roundOffset, f1Config.currentRaceNotification, f1Display, first);
    if (newRace) {
      f1Config.saveConfigFile();
    }
    if (!f1Config.currentRaceNotification && !notificaitonEventRaised) {
      // we have never notified about this race yet, so we'll raise an event
      setEvent(sendNotification, getNotifyTime());
      notificaitonEventRaised = true;
      Serial.print("Raised event for: ");
      Serial.println(myTZ.dateTime(getNotifyTime(), UTC_TIME, f1Config.timeFormat));
    }
    first = false;
  }

  events();
}
