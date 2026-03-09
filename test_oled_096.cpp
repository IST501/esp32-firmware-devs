#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================= OLED CONFIG =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1  // Não usado
#define OLED_ADDRESS  0x3C  // Endereço mais comum

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup() {
  Serial.begin(115200);
  Serial.println("Iniciando teste do OLED...");

  // Inicializa I2C
  Wire.begin(21, 22); // SDA, SCL

  // Inicializa display
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("❌ Falha ao iniciar OLED");
    while (true); // trava se falhar
  }

  Serial.println("✅ OLED iniciado com sucesso");

  // Limpa a tela
  display.clearDisplay();

  // Configura texto
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);

  // Escreve no display
  display.println("Teste OLED 0.96\"");
  display.println("----------------");
  display.println("ESP32 OK!");
  display.println("Display OK!");

  // Mostra na tela
  display.display();
}

void loop() {
  // Nada aqui, apenas teste visual
}
