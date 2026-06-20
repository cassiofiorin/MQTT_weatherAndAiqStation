#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "RTClib.h"

RTC_DS3231 rtc;

char days[7][12] = {"Domingo", "Segunda", "Terca", "Quarta", "Quinta", "Sexta", "Sabado"};

Adafruit_SSD1306 display = Adafruit_SSD1306(128, 64, &Wire, -1);

void setup() {
  Serial.begin(115200);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  if (! rtc.begin()) {
    Serial.println("Could not find RTC! Check circuit.");
    while (1);
  }

  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(0, 20);
  display.print("Relogio");
  display.display();
  delay(1000);
  display.clearDisplay();
}

void loop() {
  

DateTime now = rtc.now();
  display.display();
  display.clearDisplay();

  display.setTextSize(3);
  display.setCursor(89, 20);
  display.println(now.second(), DEC);
  display.setTextSize(3);
  display.setCursor(77, 20);
  display.println(":");
  display.setTextSize(3);
  display.setCursor(47, 20);
  display.println(now.minute(), DEC);
  display.setTextSize(3);
  display.setCursor(35, 20);
  display.println(":");
  display.setTextSize(3);
  display.setCursor(5, 20);
  display.println(now.hour(), DEC);
  display.setTextSize(1);
  display.setCursor(65, 0);
  display.print(" - ");
  display.print(days[now.dayOfTheWeek()]);
  display.setTextSize(1);
  display.setCursor(42, 0);
  display.print(now.year(), DEC);
  display.setTextSize(1);
  display.setCursor(36, 0);
  display.print("/");
  display.setTextSize(1);
  display.setCursor(26, 0);
  display.print(now.month(), DEC);
  display.setTextSize(1);
  display.setCursor(20, 0);
  display.print("/");
  display.setTextSize(1);
  display.setCursor(8, 0);
  display.print(now.day(), DEC);
}