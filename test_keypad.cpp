#include <Arduino.h>
#include <Keypad.h>

// ===== Configuração do Keypad =====
#define ROW_NUM     4
#define COLUMN_NUM  4

char keys[ROW_NUM][COLUMN_NUM] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Mesmos pinos do seu projeto
byte pin_rows[ROW_NUM]      = {13, 12, 14, 27}; 
byte pin_column[COLUMN_NUM] = {26, 25, 33, 32};

Keypad keypad = Keypad(makeKeymap(keys), pin_rows, pin_column, ROW_NUM, COLUMN_NUM);

void setup() {
  Serial.begin(115200);
  Serial.println("Teste de Keypad iniciado...");
}

void loop() {
  char key = keypad.getKey();

  if (key) { // Se alguma tecla foi pressionada
    Serial.print("Tecla pressionada: ");
    Serial.println(key);
  }
}
