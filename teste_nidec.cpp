
#include <WiFi.h>
#include <HTTPClient.h>

const char* ssid = "NMOB-Coletores";
const char* password = "6YAY#CbDBc2!";
const char* serverName = "https://projecthub.aecia.net/sync_workstation/";

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado ao WiFi!");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    WiFiClientSecure client;

    // IMPORTANTE: Como é HTTPS, o ESP32 precisa validar o certificado.
    // Para simplificar o teste, vamos ignorar a validação:
    client.setInsecure(); 

    http.begin(client, serverName);
    http.addHeader("Content-Type", "application/json");

    // JSON que funcionou no seu CMD (Atenção ao "T" maiúsculo)
    String httpRequestData = "{\"work_station\": \"Teste\"}";

    int httpResponseCode = http.POST(httpRequestData);

    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.println("Código HTTP: " + String(httpResponseCode));
      Serial.println("Resposta: " + response);
    } else {
      Serial.print("Erro na requisição: ");
      Serial.println(httpResponseCode);
      // Se der erro -1, verifique a conexão SSL
    }
    
    http.end();
  }
  delay(10000); // Aguarda 10 segundos para o próximo teste
}