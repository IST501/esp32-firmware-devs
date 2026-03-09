#include <Arduino.h>

#define SENSOR_PIN 4

void setup(){
    Serial.begin(115200);
    pinMode(SENSOR_PIN, INPUT_PULLUP);
}

void loop(){
    int estado = digitalRead(SENSOR_PIN);

    Serial.println(estado);
    delay(500);
}