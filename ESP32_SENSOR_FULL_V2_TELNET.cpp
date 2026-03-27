/*---------------------------------------------------------------------------------*/
/*                  Script REQUEST-POST ESP32                                      */
/*                                                                                 */
/* 01 - Conecta o ESP32 na rede WiFi, especificada                                 */
/* 02 - Ao enviar o sinal de GND para o pino 4 é enviado um POST para API          */
/* Versão 1.06 do esp32 board                                                      */
/* --------------------------------------------------------------------------------*/

/*

  Tamanho caixas patola, conforme display:
  
  Display Pequeno: 120 x 80 x 40 mm
  Display Grande: 147 x 97 x 55 mm

*/

/*
     payload = {
        'method_key': insert_data / delete_data
        'work_station': 'Máquina de teste 1',
        'status': '1',
        'production_note_type': '1',
        'user_name': '',
        'operation': ''
    }
*/

#include <Arduino.h>

#include <Keypad.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include "SPIFFS.h"
#include <ArduinoJson.h>

#include "esp_task_wdt.h"  // Biblioteca para usar o WDT no ESP32

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define WDT_TIMEOUT 60  // Tempo limite do watchdog em segundos

// Configuração do hotspot (AP)
char apSSID[20];  // Buffer para armazenar o SSID dinâmico
const char* apPassword = "12345678";

// Criando servidor web
WebServer server(80);

// Serão exibidos esses valores default para a primeira vez do hardware
String WORK_STATION = "SUA_WORK_STATION";                                                                                 
String SSID_NAME = "SEU_SSID";                             
String PASSWORD = "SEU_PASSWORD";
String SERVER_IP = "IP_SERVIDOR";
String SERVER_PORT = "8000";
String SENSOR_INTERVAL = "1000"; // Intervalo com padrão de 1 seg para cada leitura do sensor de ciclo
String DISPLAY_TYPE = "oled_096"; // Display 0.96 polegadas como default
String SENSOR_READ_TYPE = "pulse_signal"; // Tipo de leitura: pulse_signal | pulse_signal_linked | continuos_signal
String ENABLE_TELNET = "0"; // Flag para habilitar o servidor Telnet

WiFiServer telnetServer(23);
WiFiClient telnetClient;

template <typename T>
void sysPrint(T msg) {
  Serial .print(msg);
  if (ENABLE_TELNET == "1" && telnetClient && telnetClient.connected()) {
    String tStr = String(msg);
    tStr.replace("\r\n", "\n"); // Normaliza
    tStr.replace("\n", "\r\n"); // Força o retorno de carro
    telnetClient.print(tStr);
  }
}

void sysPrintln() {
  Serial .println();
  if (ENABLE_TELNET == "1" && telnetClient && telnetClient.connected()) {
    telnetClient.println();
  }
}

template <typename T>
void sysPrintln(T msg) {
  sysPrint(msg);
  sysPrintln();
}

void sysPrintf(const char *format, ...) {
  char loc_buf[256];
  char * temp = loc_buf;
  va_list arg;
  va_list copy;
  va_start(arg, format);
  va_copy(copy, arg);
  int len = vsnprintf(temp, sizeof(loc_buf), format, copy);
  va_end(copy);
  if(len < 0) {
    va_end(arg);
    return;
  };
  if(len >= sizeof(loc_buf)){
    temp = (char*)malloc(len+1);
    if(temp == NULL) {
      va_end(arg);
      return;
    }
    vsnprintf(temp, len+1, format, arg);
  }
  va_end(arg);
  
  Serial .print(temp);
  if (ENABLE_TELNET == "1" && telnetClient && telnetClient.connected()) {
    String tStr = String(temp);
    tStr.replace("\r\n", "\n"); // Normalize in case it already has \r\n
    tStr.replace("\n", "\r\n"); // Force \r\n
    telnetClient.print(tStr);
  }
  if(temp != loc_buf){
    free(temp);
  }
}

void telnetLog(String msg) {
  sysPrintln(msg);
}

void handleTelnet() {
  if (ENABLE_TELNET != "1") return;
  
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) telnetClient.stop();
      telnetClient = telnetServer.available();
      telnetClient.println("=== Production Hub Telnet Connected ===");
    } else {
      WiFiClient newClient = telnetServer.available();
      newClient.stop(); // Rejeita múltiplas conexões
    }
  }
}

int step = 0;
bool lastSignalState = false; // Usada pelo modo continuos_signal

// O valor de "" é por conta de não existir nenhum id no banco que tenha o valor "", com isso, não encontrando nenhuma operação, nenhum user e nenhuma PRODUCTION_ORDER
String USER_CODE = "";
String USER_NAME = "";
String OPERATION = "";                                                                                                                
String PRODUCTION_ORDER = "";


// Estrutura para armazenar dados de um Part Number
struct PartNumberData {
    int id;
    String name;
    int parts_to_produce;
    int good_parts_produced;
    int bad_parts_produced;
};

// Array para armazenar múltiplos part numbers (até 10, ajuste conforme necessário)
#define MAX_PART_NUMBERS 10
PartNumberData PART_NUMBERS[MAX_PART_NUMBERS];
int PART_NUMBERS_COUNT = 0;  // Quantidade de PNs atualmente armazenados
int CURRENT_PN_INDEX = 0;     // PN atualmente selecionado


// MARK: Keypad variaveis
bool inputMode = false; // Variável utilizada para não atualizar o display quando estiver em modo de inputMode
unsigned long previousInputMillis = 0; // Variável que vai assumir o valor de millis toda vez que for acionada uma condição de input
const unsigned long inputModeInterval = 30000; // Variável de tempo máximo de 30 para digitar os inputs desejados


bool nonConfirmingPartsTypeInputMode = false;
bool nonConfirmingPartsQuantInputMode = false;
String nonConfirmingPartsTypeCode = "";
String nonConfirmingPartsQuant = "";

bool deletePartsTypeInputMode = false;
bool deletePartsQuantInputMode = false;
String deletePartsTypeCode = "";
String deletePartsQuant = "";

bool operatorInputMode = false; // Variável para entrar no modo de input do operador

bool operationInputMode = false; // Variável para entrar no modo input da operação
bool workOrderInputMode = false; // Variável para entrar no modo input da work order após digitar uma operação correta
bool partsToProdInputMode = false; // Variável para entrar no modo input de quantidade de peças para essa ordem caso tenha sido digitada
bool partsQuantityPerPNInputMode = false;  // Modo de input de quantidade por PN
int currentPNInputIndex = 0;  // Índice do PN atual sendo configurado
int partsQuantityPerPN[MAX_PART_NUMBERS];  // Array para armazenar quantidades de cada PN

bool interventionInputMode = false;
String interventionCode = "";

bool validateConfigs = false;

// Configurações Default do Display
#define OLED_RESET -1
int SCREEN_WIDTH  = 128; // valor default, sobrescrito pelo initDisplay()
int SCREEN_HEIGHT = 64;
Adafruit_SSD1306* display = nullptr; // ponteiro, instanciado após carregar config

// Variável para indicar quando o display está disponível para ser atualizado
bool displayEnabled = true; 
String lastDisplayedText = "";

// Keypad Configuração
#define ROW_NUM     4 // Quatro linhas
#define COLUMN_NUM  4 // Quatro colunas

char keys[ROW_NUM][COLUMN_NUM] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte pin_rows[ROW_NUM]      = {32, 33, 25, 26};
byte pin_column[COLUMN_NUM] = {27, 14, 12, 13};

Keypad keypad = Keypad(makeKeymap(keys), pin_rows, pin_column, ROW_NUM, COLUMN_NUM);

#define DEBUG_LED 2           // Digital Output para o led de debug (azul)
#define INTERVENTION_LED 15   // Digital Output para o led de intervenções (vermelho)
#define SENSOR_PIN_1 4          // Digital Input para realizar as leituras de peça
#define SENSOR_PIN_2 19        // Digital Input para realizar as leituras de peça

/*
currentWorkStationState = -1 (quando aperta * para totalmente o funcionamento do sensor, voltando apenas quando apertar novamente o *)
currentWorkStationState = 0 (o sensor abre uma intervenção e ele fecha a intervenção quando for apertado o #)
currentWorkStationState = 1 (sensor no funcionamento normal de coleta de sinal de ciclo)
*/
enum WorkStationState {
  STANDBY = -1,
  INTERVENTION = 0,
  RUNNING = 1
};

WorkStationState currentWorkStationState = RUNNING;

const unsigned long read_interval = 10; // Intervalo de tempo de 10 milissegundos para cada leitura
const unsigned long debounce_keypad = 1000; // Intervalo de 1 seg de debounce para os botões no teclado
const unsigned long message_interval = 3000; // Intervalo de 3 seg de exibição da mensagem no display após interação do usuário

const unsigned long starKeyResetInterval = 3000; // Intervalo de tempo colocado para caso o botão * fique segurado por mais de 3 segundos ele reinicia
unsigned long starKeyPressedMillis = 0; // Variável que armazena o momento em que o botão * foi apertado

const unsigned long zeroKeyClearInterval = 3000; // Segurar 0 por 3 segundos executa o clear_setup
unsigned long zeroKeyPressedMillis = 0; // Variável que armazena o momento em que o botão 0 foi apertado

// Variável para armazenar o tempo da última solicitação
unsigned long previousReadMillis = 0;
unsigned long previousPartMillis = 0;
unsigned long previousDebounceMillis = 0;
unsigned long previousMessageMillis = 0;

const char* filename = "/config.txt"; // Arquivo de Configuração


// ============================================================
// FIX 2 — Função centralizada para resetar todos os input modes
// Limpa todas as flags e dados de todos os fluxos (A, B, C, D, #)
// sem tocar no inputMode — quem chama decide o valor dele.
// ============================================================
void resetAllInputModes() {

  // Fluxo A
  nonConfirmingPartsTypeInputMode = false;
  nonConfirmingPartsQuantInputMode = false;
  nonConfirmingPartsTypeCode = "";
  nonConfirmingPartsQuant = "";

  // Fluxo B
  deletePartsTypeInputMode = false;
  deletePartsQuantInputMode = false;
  deletePartsTypeCode = "";
  deletePartsQuant = "";

  // Fluxo C
  operatorInputMode = false;

  // Fluxo D
  operationInputMode = false;
  workOrderInputMode = false;
  partsToProdInputMode = false;
  partsQuantityPerPNInputMode = false;
  currentPNInputIndex = 0;
  for (int i = 0; i < MAX_PART_NUMBERS; i++) {
    partsQuantityPerPN[i] = 0;
  }

  // Fluxo #
  interventionInputMode = false;
  interventionCode = "";
}


// Função para salvar apenas um parâmetro específico
void saveConfig(String param, String value) {
  
    if(param == "WORK_STATION"){
      WORK_STATION = value;
    }
    else if (param == "SSID_NAME"){
      SSID_NAME = value;
    }
    else if (param == "PASSWORD"){
      PASSWORD = value;
    }
    else if (param == "SERVER_IP"){
      SERVER_IP = value;
    }
    else if (param == "SERVER_PORT"){
        SERVER_PORT = value;
    }
    else if (param == "SENSOR_INTERVAL"){
      SENSOR_INTERVAL = value;
    }
    else if (param == "DISPLAY_TYPE"){
      DISPLAY_TYPE = value;
    }
    else if (param == "SENSOR_READ_TYPE") {
      SENSOR_READ_TYPE = value;
    }
    else if (param == "ENABLE_TELNET") {
      ENABLE_TELNET = value;
    }
    else if (param == "USER_CODE") {
      USER_CODE = value;
    }
    else {
      sysPrintln("Parâmetro inválido!");
      return;
    }
  
    File file = SPIFFS.open(filename, FILE_WRITE);
    if (!file) {
      sysPrintln("Erro ao abrir o arquivo para escrita!");
      return;
    }
  
    file.println("WORK_STATION=" + WORK_STATION);
    file.println("SSID_NAME=" + SSID_NAME);
    file.println("PASSWORD=" + PASSWORD);
    file.println("SERVER_IP=" + SERVER_IP);
    file.println("SERVER_PORT=" + SERVER_PORT);
    file.println("SENSOR_INTERVAL=" + SENSOR_INTERVAL);
    file.println("DISPLAY_TYPE=" + DISPLAY_TYPE);
    file.println("SENSOR_READ_TYPE=" + SENSOR_READ_TYPE);
    file.println("ENABLE_TELNET=" + ENABLE_TELNET);
    
    file.close();
  
    sysPrintln("Parâmetro " + param + " atualizado com sucesso!");
}


// Função para carregar configurações do arquivo
void loadConfig() {
    File file = SPIFFS.open(filename, FILE_READ);
    if (!file) {
      sysPrintln("Arquivo de configuração não encontrado! Criando um novo...");
      
      saveConfig("WORK_STATION", WORK_STATION);
      saveConfig("SSID_NAME", SSID_NAME);
      saveConfig("PASSWORD", PASSWORD);
      saveConfig("SERVER_IP", SERVER_IP);
      saveConfig("SERVER_PORT", SERVER_PORT);
      saveConfig("SENSOR_INTERVAL", SENSOR_INTERVAL);
      saveConfig("DISPLAY_TYPE", DISPLAY_TYPE);
      saveConfig("SENSOR_READ_TYPE", SENSOR_READ_TYPE);
  
      return;
    }
    
    while (file.available()) {
      String line = file.readStringUntil('\n'); 
      line.trim();
  
      if (line.startsWith("WORK_STATION=")) {
        WORK_STATION = line.substring(13);
      }  
      else if (line.startsWith("SSID_NAME=")) {
        SSID_NAME = line.substring(10);
      } 
      else if (line.startsWith("PASSWORD=")) {
        PASSWORD = line.substring(9);
      }
      else if (line.startsWith("SERVER_IP=")) {
        SERVER_IP = line.substring(10);
      }  
      else if (line.startsWith("SERVER_PORT=")) {
        SERVER_PORT = line.substring(12);
      }
      else if (line.startsWith("SENSOR_INTERVAL=")){
        SENSOR_INTERVAL = line.substring(16);
      }
      else if (line.startsWith("DISPLAY_TYPE=")){
        DISPLAY_TYPE = line.substring(13);
      }
      else if (line.startsWith("SENSOR_READ_TYPE=")) {
        SENSOR_READ_TYPE = line.substring(17);
      }
      else if (line.startsWith("ENABLE_TELNET=")) {
        ENABLE_TELNET = line.substring(14);
      }
    }
    
    file.close();
  
    sysPrintln("Configurações carregadas da SPIFFS:");
    sysPrintln("WORK_STATION: " + WORK_STATION);
    sysPrintln("SSID_NAME: " + SSID_NAME);
    sysPrintln("PASSWORD: " + PASSWORD);
    sysPrintln("SERVER_IP: " + SERVER_IP);
    sysPrintln("SERVER_PORT: " + SERVER_PORT);
    sysPrintln("SENSOR_INTERVAL: " + SENSOR_INTERVAL);
    sysPrintln("DISPLAY_TYPE: " + DISPLAY_TYPE);
    sysPrintln("SENSOR_READ_TYPE: " + SENSOR_READ_TYPE);
    sysPrintln("ENABLE_TELNET: " + ENABLE_TELNET);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////// Funções para trabalhar com o struct do PART_NUMBERS ///////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////

// Função auxiliar para limpar o array de part numbers
void clearPartNumbers() {
    for (int i = 0; i < MAX_PART_NUMBERS; i++) {
        PART_NUMBERS[i].id = 0;
        PART_NUMBERS[i].name = "";
        PART_NUMBERS[i].parts_to_produce = 0;
        PART_NUMBERS[i].good_parts_produced = 0;
        PART_NUMBERS[i].bad_parts_produced = 0;
    }
    PART_NUMBERS_COUNT = 0;
}

// Função para adicionar um part number ao array
bool addPartNumber(int id, String name, int parts_to_produce, int good_parts, int bad_parts) {
    if (PART_NUMBERS_COUNT >= MAX_PART_NUMBERS) {
        sysPrintln("Limite de Part Numbers atingido!");
        return false;
    }
    
    PART_NUMBERS[PART_NUMBERS_COUNT].id = id;
    PART_NUMBERS[PART_NUMBERS_COUNT].name = name;
    PART_NUMBERS[PART_NUMBERS_COUNT].parts_to_produce = parts_to_produce;
    PART_NUMBERS[PART_NUMBERS_COUNT].good_parts_produced = good_parts;
    PART_NUMBERS[PART_NUMBERS_COUNT].bad_parts_produced = bad_parts;
    
    PART_NUMBERS_COUNT++;
    return true;
}

// Função para obter o part number atual
PartNumberData* getCurrentPartNumber() {
    if (PART_NUMBERS_COUNT == 0) return nullptr;
    return &PART_NUMBERS[CURRENT_PN_INDEX];
}

// Função para mudar para o próximo part number
void selectNextPartNumber() {
    if (PART_NUMBERS_COUNT > 0) {
        CURRENT_PN_INDEX = (CURRENT_PN_INDEX + 1) % PART_NUMBERS_COUNT;
    }
}

// Função para mudar para o part number anterior
void selectPreviousPartNumber() {
    if (PART_NUMBERS_COUNT > 0) {
        CURRENT_PN_INDEX--;
        if (CURRENT_PN_INDEX < 0) {
            CURRENT_PN_INDEX = PART_NUMBERS_COUNT - 1;
        }
    }
}


void successfulResponse(){
    digitalWrite(DEBUG_LED, false);
    delay(100);
    digitalWrite(DEBUG_LED, true);
    delay(100);
    digitalWrite(DEBUG_LED, false);
    delay(100);
    digitalWrite(DEBUG_LED, true);
    delay(100);
}


String generateConfigPage() {
  
  loadConfig();

  String page = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>Config Page</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
          body { font-family: Arial, sans-serif; text-align: center; margin: 40px; }
          input, label { padding: 8px; margin: 5px; width: 80%; max-width: 300px; }
          button { padding: 10px 20px; background-color: #28a745; color: white; border: none; cursor: pointer; }
          input, select {padding: 8px; margin: 5px auto; width: 80%; max-width: 300px; box-sizing: border-box;}
          button:hover { background-color: #218838; }
      </style>
    </head>
    <body>
      
      <form action="/save" method="POST">
      <h2>Wi-Fi Settings</h2>
          <label>SSID:</label><br>
          <input type="text" name="ssid" value=")rawliteral" + SSID_NAME + R"rawliteral("><br><br>
          <label>Password:</label><br>
          <input type="password" id="pwd" name="password" value=")rawliteral" + PASSWORD + R"rawliteral("><br>
          <label style="font-size: 12px; font-weight: normal; margin: 0; display: inline-block; cursor: pointer;">
              <input type="checkbox" style="width: auto; margin-right: 5px;" onclick="document.getElementById('pwd').type = this.checked ? 'text' : 'password'"> Show Password 👁️
          </label><br><br>
  
      <h2>Production Hub Settings</h2>
          <label>Work Station Name:</label><br>
          <input type="text" name="work-station" value=")rawliteral" + WORK_STATION + R"rawliteral("><br><br>
          <label>Server IP:</label><br>
          <input type="text" name="server-ip" value=")rawliteral" + SERVER_IP + R"rawliteral("><br><br>
          <label>Server Port:</label><br>
          <input type="text" name="server-port" value=")rawliteral" + SERVER_PORT + R"rawliteral("><br><br>

          <label>Sensor Read Type:</label><br>
          <select name="sensor_read_type">
            <option value="pulse_signal" )rawliteral" + (SENSOR_READ_TYPE == "pulse_signal" ? "selected" : "") + R"rawliteral(>Pulse Signal</option>
            <option value="pulse_signal_linked" )rawliteral" + (SENSOR_READ_TYPE == "pulse_signal_linked" ? "selected" : "") + R"rawliteral(>Pulse Signal + Linked Steps</option>
            <option value="continuos_signal" )rawliteral" + (SENSOR_READ_TYPE == "continuos_signal" ? "selected" : "") + R"rawliteral(>Continuos Signal</option>
          </select><br><br>

          <label>Sensor Interval (s):</label><br>
          <input type="number" name="sensor-interval" min="0.5" step="0.5" value=")rawliteral" + String(SENSOR_INTERVAL.toFloat()/1000.0, 1) + R"rawliteral("><br><br>

          <label>Telnet Support:</label><br>
          <select name="enable_telnet">
            <option value="0" )rawliteral" + (ENABLE_TELNET == "0" ? "selected" : "") + R"rawliteral(>Disabled</option>
            <option value="1" )rawliteral" + (ENABLE_TELNET == "1" ? "selected" : "") + R"rawliteral(>Enabled</option>
          </select><br><br>

          <button type="submit">Save and Restart</button>
      </form>
    </body>
    </html>)rawliteral";

  return page;
}

// Função para exibir a página de configuração Wi-Fi
void handleRoot() {
  server.send(200, "text/html", generateConfigPage());
}

// Função para salvar as novas configurações da AP
void handleSave() {
  String newSSID = server.arg("ssid");
  String newPass = server.arg("password");
  String newWorkStation = server.arg("work-station");
  String newServerIp = server.arg("server-ip");
  String newServerPort = server.arg("server-port");
  String newSensorInterval = String(server.arg("sensor-interval").toFloat() * 1000);
  String newDisplayType = server.arg("display");
  String newSensorReadType = server.arg("sensor_read_type");
  String newEnableTelnet = server.arg("enable_telnet");

  if (newSSID.length() > 0 && newPass.length() > 0 && newWorkStation.length() > 0 && newServerIp.length() > 0 && newServerPort.length() > 0) {
            
      saveConfig("SSID_NAME", newSSID);
      saveConfig("PASSWORD", newPass);
      saveConfig("WORK_STATION", newWorkStation);
      saveConfig("SERVER_IP", newServerIp);
      saveConfig("SERVER_PORT", newServerPort);
      saveConfig("SENSOR_INTERVAL", newSensorInterval);
      if (newDisplayType.length() > 0) {
          saveConfig("DISPLAY_TYPE", newDisplayType);
      }
      saveConfig("SENSOR_READ_TYPE", newSensorReadType);
      saveConfig("ENABLE_TELNET", newEnableTelnet);

      server.send(200, "text/html", "<h2>Configuration saved! Restarting...</h2>");
      delay(2000);
      ESP.restart();
  } else {
      server.send(400, "text/html", "<h2>Error: values cannot be empty</h2>");
  }
}

void initDisplay() {
    if (display != nullptr) delete display;

    SCREEN_WIDTH  = 128;
    SCREEN_HEIGHT = (DISPLAY_TYPE == "oled_24") ? 128 : 64;

    display = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

    if (!display->begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        sysPrintln("Falha ao inicializar o display OLED");
        while (1);
    }

    display->clearDisplay();
    display->display();
    sysPrintln("Display iniciado: " + String(SCREEN_WIDTH) + "x" + String(SCREEN_HEIGHT));
}

void drawCenteredText(const char* text, uint8_t textSize) {
  String newText = String(text);
  if (newText != lastDisplayedText) {
    sysPrintln("\n[DISPLAY]:\n" + newText + "\n-----------------");
    lastDisplayedText = newText;
  }

  display->clearDisplay();
  display->setTextSize(textSize);
  display->setTextColor(WHITE);

  int16_t x1, y1;
  uint16_t textWidth, textHeight;

  char textBuffer[128]; 
  strncpy(textBuffer, text, sizeof(textBuffer));  
  textBuffer[sizeof(textBuffer) - 1] = '\0';

  char* line = strtok(textBuffer, "\n");
  int lineCount = 0;
  char* lines[10];

  while (line != NULL && lineCount < 10) {
    lines[lineCount++] = line;
    line = strtok(NULL, "\n");
  }

  int totalHeight = lineCount * (8 * textSize);
  int16_t startY = (SCREEN_HEIGHT - totalHeight) / 2;

  for (int i = 0; i < lineCount; i++) {
    display->getTextBounds(lines[i], 0, 0, &x1, &y1, &textWidth, &textHeight);
    int16_t x = (SCREEN_WIDTH - textWidth) / 2;
    int16_t y = startY + i * (8 * textSize);
    display->setCursor(x, y);
    display->println(lines[i]);
  }

  display->display();
}

void drawMainView() {
    display->clearDisplay();

    String fullString = "AP:" + String(apSSID) + "\n" +
                        "----------- \n" +
                        "User:" + USER_NAME + "\n" +
                        "OP:" + OPERATION + "\n" +
                        "OS:" + PRODUCTION_ORDER + "\n";
    
    if (PART_NUMBERS_COUNT > 0) {
        PartNumberData* currentPN = getCurrentPartNumber();
        if (currentPN != nullptr) {
            fullString += "PN(" + String(CURRENT_PN_INDEX + 1) + "/" + String(PART_NUMBERS_COUNT) + "): " + String(currentPN->parts_to_produce) + "/" + String(currentPN->good_parts_produced) + "/" + String(currentPN->bad_parts_produced) + "\n";
        }
    } else {
        fullString += "PN (0/0/0):";
    }

    if (fullString != lastDisplayedText) {
      sysPrintln("\n[DISPLAY MAIN]:\n" + fullString + "-----------------");
      lastDisplayedText = fullString;
    }

    char textBuffer[256];
    fullString.toCharArray(textBuffer, sizeof(textBuffer));

    char* lines[10];
    int lineCount = 0;
    char* line = strtok(textBuffer, "\n");
    while (line != NULL && lineCount < 10) {
        lines[lineCount++] = line;
        line = strtok(NULL, "\n");
    }

    uint8_t textSize = 1;
    int lineHeight = 8 * textSize;
    int lineSpacing = 1;
    int totalHeight = lineCount * lineHeight;
    int16_t startY = (SCREEN_HEIGHT - totalHeight) / 2;

    int16_t x1, y1;
    uint16_t textWidth, textHeight;

    for (int i = 0; i < lineCount; i++) {
        display->getTextBounds(lines[i], 0, 0, &x1, &y1, &textWidth, &textHeight);
        int16_t x = (i == 0 || i == 1) ? (SCREEN_WIDTH - textWidth) / 2 : 0;
        int16_t y = startY + i * (lineHeight + lineSpacing);

        display->setTextSize(textSize);
        display->setTextColor(WHITE);
        display->setCursor(x, y);
        display->println(lines[i]);
    }

    display->display();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////// Função para centralizar todos os POSTS na API em um único lugar ///////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct ApiResult {
	int httpCode;
	bool hasJson;
	StaticJsonDocument<1024> json;
};

ApiResult sendApiRequest(JsonDocument& payload, const String& route) {

	ApiResult result;
	result.httpCode = -1;
	result.hasJson = false;

	// Campos comuns
  payload["work_station"] = WORK_STATION;
  payload["user_name"] = USER_CODE.length() ? USER_CODE.toInt() : 0;
  payload["operation"] = OPERATION.length() ? OPERATION.toInt() : 0;
  payload["production_order_code"] = PRODUCTION_ORDER;
  payload["rssi"] = WiFi.RSSI();

	String requestUrl = "http://" + SERVER_IP + ":" + SERVER_PORT + "/" + route;

	HTTPClient http;
	if (!http.begin(requestUrl)) {
		return result; // network-error
	}

	http.addHeader("Content-Type", "application/json");

	String body;
	serializeJson(payload, body);

	result.httpCode = http.POST(body);

	if (result.httpCode > 0) {
		String response = http.getString();
		DeserializationError err = deserializeJson(result.json, response);
		if (!err) {
			result.hasJson = true;
		}
	}

	http.end();
	return result;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////// Handlers //////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool handleSimpleApiResult(ApiResult& result, bool showSuccessMessage = true, bool successfulBlink = true) {

    if (result.httpCode < 0) {
      sysPrintln("network-error");
      return false;
    }

    JsonObject obj = result.json.as<JsonObject>();
    const char* msg = obj["message"];

    if (msg) {

        if (result.httpCode < 200 || result.httpCode >= 300) {
            sysPrint("HTTP error: ");
            sysPrintln(result.httpCode);

            sysPrintln(msg);
            drawCenteredText(msg, 1);
            previousMessageMillis = millis();
            return false;
        }
        else {
            if (showSuccessMessage){
                drawCenteredText(msg, 1);
                previousMessageMillis = millis();
            }

            if (successfulBlink) {
              successfulResponse();
            }
                           
            sysPrint("Response JSON: ");
            serializeJsonPretty(obj, Serial);
            sysPrintln();
            return true;
        }
    }
    else{
        sysPrintln("There is not response.");
        drawCenteredText("There is not response.", 1);
        previousMessageMillis = millis();
        return false;
    }
}

void handleChangeOperation(ApiResult& result) {

    if (result.httpCode < 0) {

        OPERATION = "";
        PRODUCTION_ORDER = "";

        inputMode = false;
        operationInputMode = false;
        workOrderInputMode = false;
        partsToProdInputMode = false;

        sysPrintln(result.httpCode);
        sysPrintln("NETWORK ERROR!");
        drawCenteredText("NETWORK ERROR!", 1);
        previousMessageMillis = millis();
        return;
    }

    JsonObject obj = result.json.as<JsonObject>();
    const char* msg = obj["message"];

    if (!msg) {
        sysPrintln("No message in response");
        return;
    }

    if (result.httpCode < 200 || result.httpCode >= 300) {

        OPERATION = "";
        PRODUCTION_ORDER = "";

        inputMode = false;
        operationInputMode = false;
        workOrderInputMode = false;
        partsToProdInputMode = false;

        sysPrintln(result.httpCode);
        sysPrintln(msg);
        drawCenteredText(msg, 1);
        previousMessageMillis = millis();
        return;
    }

    sysPrintln("HANDLE CHANGE OPERATION OK");

    if (PRODUCTION_ORDER == "") {

        inputMode = false;
        operationInputMode = false;
        workOrderInputMode = false;
        partsToProdInputMode = false;
    }
    else if (obj["get_more_info"] == 1) {

        partsToProdInputMode = true;
        inputMode = true;              

        operationInputMode = false;
        workOrderInputMode = false;
    }
    else if (obj["get_more_info"] == 0) {

        partsToProdInputMode = false;
        inputMode = false;

        operationInputMode = false;
        workOrderInputMode = false;
    }

    saveConfig("OPERATION", OPERATION);
    saveConfig("PRODUCTION_ORDER", PRODUCTION_ORDER);

    sysPrintln(result.httpCode);
    sysPrintln(msg);
    drawCenteredText(msg, 1);
    previousMessageMillis = millis();
}

void handlestopIntervention(ApiResult& result) {

    if (result.httpCode < 0) {

        sysPrintln(result.httpCode);
        sysPrintln("NETWORK ERROR!");
        drawCenteredText("NETWORK ERROR!", 1);
        
        return;
    }

    JsonObject obj = result.json.as<JsonObject>();
    const char* msg = obj["message"];

    if (!msg) {

        sysPrintln("No message in response");
        drawCenteredText("No message in response", 1);

        return;
    }

    if (result.httpCode < 200 || result.httpCode >= 300) {

        sysPrintln(result.httpCode);
        sysPrintln(msg);
        drawCenteredText(msg, 1);
    }
    else {
        currentWorkStationState = RUNNING;

        digitalWrite(DEBUG_LED, HIGH);
        digitalWrite(INTERVENTION_LED, LOW);

        sysPrintln(result.httpCode);
        sysPrintln(msg);
        drawCenteredText(msg, 1);
    }
}

void handleSyncWorkStation(ApiResult& result) {

    bool previousValidateState = validateConfigs;
    yield();

    if (result.httpCode < 0) {
        validateConfigs = false;
        drawCenteredText("SERVER \n NOT FOUND!", 1);
        sysPrintln("SERVER NOT FOUND!");
        return;
    }

    JsonObject obj = result.json.as<JsonObject>();
    const char* msg = obj["message"];

    if (!msg) {
        validateConfigs = false;
        drawCenteredText("Invalid response", 1);
        sysPrintln("Invalid response from server!");
        return;
    }

    if (result.httpCode < 200 || result.httpCode >= 300) {
        validateConfigs = false;
        drawCenteredText(msg, 1);
        sysPrintln("HTTP error: " + String(result.httpCode));
        return;
    }

    validateConfigs = true;
    sysPrintln(msg);

    // Exibe mensagem de sincronização na primeira vez que conecta com sucesso,
    // mas NÃO retorna — continua para popular os dados normalmente.
    if (!previousValidateState) {
        drawCenteredText(msg, 1);
        sysPrintln("Workstation synchronized successfully!");
    }

    if (!obj["intervention_data"].isNull()) {
        currentWorkStationState = INTERVENTION;

        digitalWrite(DEBUG_LED, LOW);
        digitalWrite(INTERVENTION_LED, HIGH);

        drawCenteredText(msg, 1);
        sysPrintln("Workstation in intervention state!");
        return;
    }
    else if (currentWorkStationState != RUNNING) {
        currentWorkStationState = RUNNING;
        
        digitalWrite(DEBUG_LED, HIGH);
        digitalWrite(INTERVENTION_LED, LOW);
        
        drawCenteredText("Intervention closed remotely", 1);
        sysPrintln("Intervention closed remotely");
    }

    clearPartNumbers();

    USER_CODE = obj["user_data"]["id"].isNull()
        ? ""
        : String(obj["user_data"]["id"].as<int>());

    USER_NAME = obj["user_data"]["name"].isNull()
        ? ""
        : obj["user_data"]["name"].as<String>();

    OPERATION = obj["operation_data"]["id"].isNull()
        ? ""
        : String(obj["operation_data"]["id"].as<int>());

    // sysPrintln("zibs");

    JsonObject opPNs = obj["operation_data"]["part_numbers"].as<JsonObject>();

    for (JsonPair kv : opPNs) {

        if (PART_NUMBERS_COUNT >= MAX_PART_NUMBERS) break;

        JsonObject pn = kv.value().as<JsonObject>();

        PART_NUMBERS[PART_NUMBERS_COUNT].id = pn["id"].as<int>();
        PART_NUMBERS[PART_NUMBERS_COUNT].name = pn["name"].as<String>();
        PART_NUMBERS[PART_NUMBERS_COUNT].good_parts_produced = pn["good_parts_quantity"].as<int>();
        PART_NUMBERS[PART_NUMBERS_COUNT].bad_parts_produced = pn["bad_parts_quantity"].as<int>();
        PART_NUMBERS[PART_NUMBERS_COUNT].parts_to_produce = 0;

        PART_NUMBERS_COUNT++;
    }

    if (PART_NUMBERS_COUNT <= CURRENT_PN_INDEX){
      CURRENT_PN_INDEX = 0;
    }
    
    JsonVariant poVar = obj["production_order_data"];

    if (!poVar.isNull()) {

        PRODUCTION_ORDER = poVar["production_order_code"].as<String>();

        JsonObject poPNs = poVar["part_numbers"].as<JsonObject>();

        for (JsonPair kv : poPNs) {

            JsonObject poPn = kv.value().as<JsonObject>();
            int pnId = poPn["id"].as<int>();

            for (int i = 0; i < PART_NUMBERS_COUNT; i++) {
                if (PART_NUMBERS[i].id == pnId) {
                    PART_NUMBERS[i].parts_to_produce =
                        poPn["parts_to_produce"].as<int>();
                    break;
                }
            }
        }

    } else {
        PRODUCTION_ORDER = "";
    }

    for (int i = 0; i < PART_NUMBERS_COUNT; i++) {
        sysPrintf(
            "PN %d | %s | ToProd:%d | OK:%d | NOK:%d \n",
            PART_NUMBERS[i].id,
            PART_NUMBERS[i].name.c_str(),
            PART_NUMBERS[i].parts_to_produce,
            PART_NUMBERS[i].good_parts_produced,
            PART_NUMBERS[i].bad_parts_produced
        );
    }
}

void sync_workstation(){

  DynamicJsonDocument payload(256);

  ApiResult result = sendApiRequest(payload, "sync_workstation/");
  sysPrintln("SYNC WORKSTATION RESULT: " + String(result.httpCode));

  /* // Log temporário para debug do zeramento
  String rawSync;
  serializeJson(result.json, rawSync);
  sysPrintln("SYNC RAW: " + rawSync); */

  // Guarda se o JSON foi parseado com sucesso antes de processar.
  // Evita zerar variáveis locais quando a deserialização falha silenciosamente.
  if (result.httpCode > 0 && !result.hasJson) {
    sysPrintln("SYNC: JSON parse failed, skipping to avoid clearing local state.");
    return;
  }

  handleSyncWorkStation(result);
}

void setup() {

  Serial.begin(115200);

  esp_task_wdt_init(WDT_TIMEOUT, true);  
  esp_task_wdt_add(NULL);

  if (!SPIFFS.begin(true)) {
    sysPrintln("Erro ao montar SPIFFS!");
    return;
  }

  loadConfig();
  initDisplay();
  
  pinMode(DEBUG_LED, OUTPUT);
  pinMode(INTERVENTION_LED, OUTPUT);
  pinMode(SENSOR_PIN_1, INPUT_PULLUP);
  pinMode(SENSOR_PIN_2, INPUT_PULLUP);

  bool ledState = false;

  WiFi.begin(SSID_NAME.c_str(), PASSWORD.c_str());
  sysPrintln("Connecting to WiFi...");
  drawCenteredText("Connecting to WiFi...", 1);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    ledState = !ledState;
    digitalWrite(DEBUG_LED, ledState);
    delay(1000);
    sysPrint(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    sysPrintln("\nConectado à rede WiFi!");
    sysPrint("Endereço IP Local: ");
    sysPrintln(WiFi.localIP());
    
    if (ENABLE_TELNET == "1") {
      sysPrintln("--------------------------------------------------");
      sysPrintln("Servidor Telnet ATIVO.");
      sysPrintln("Para visualizar os logs via Wi-Fi, abra o CMD ou PowerShell do Windows e digite:");
      sysPrint("telnet ");
      sysPrintln(WiFi.localIP());
      sysPrintln("--------------------------------------------------");
    }
  } else {
    sysPrintln("\nFalha ao conectar na rede WiFi. Iniciando AP...");
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);

  sprintf(apSSID, "ESP32_%02X%02X%02X", mac[3], mac[4], mac[5]);

  WiFi.softAP(apSSID, apPassword);
  sysPrintln("Hotspot ativo!");
  sysPrint("Endereço IP do AP: ");
  sysPrintln(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  
  if (ENABLE_TELNET == "1") {
    telnetServer.begin();
    sysPrintln("Telnet server started on port 23");
  }
  
  sync_workstation();

  if (currentWorkStationState == INTERVENTION) {
      digitalWrite(DEBUG_LED, LOW);
      digitalWrite(INTERVENTION_LED, HIGH);
  } else {
      digitalWrite(DEBUG_LED, HIGH);
      digitalWrite(INTERVENTION_LED, LOW);
  }
}

// MARK: LOOP
void loop() {

  server.handleClient();
  handleTelnet();
  esp_task_wdt_reset();

  unsigned long currentMillis = millis();
	if (currentMillis - previousReadMillis < read_interval) {
		return;
	}
	previousReadMillis = currentMillis;

  // ========================================
  // MODO STANDBY - BLOQUEIA TUDO
  // ========================================
  if (currentWorkStationState == STANDBY) {
      static unsigned long lastStandbyDisplay = 0;
      if (currentMillis - lastStandbyDisplay > 5000) {
          drawCenteredText("STANDBY MODE\n\nPress * to\nresume", 1);
          lastStandbyDisplay = currentMillis;
      }
      
      char key = keypad.getKey();
      if (key == '*') {
          currentWorkStationState = RUNNING;
          
          digitalWrite(DEBUG_LED, HIGH);
          digitalWrite(INTERVENTION_LED, LOW);
          
          sysPrintln("Returning to Normal Mode");
          drawCenteredText("Returning to\nNormal Mode", 1);
          previousMessageMillis = currentMillis;
      }
      
      return;
  }


  if (WiFi.status() != WL_CONNECTED) {

      static unsigned long lastWifiRetryMillis = 0;

      if (currentMillis - previousMessageMillis > message_interval){
        drawCenteredText(("AP:" + String(apSSID) + "\n --- \n Wifi disconnected!").c_str(), 1);
      }

      if (currentMillis - lastWifiRetryMillis >= 10000) {
          lastWifiRetryMillis = currentMillis;
          previousMessageMillis = currentMillis;

          sysPrintln("Trying to reconnect WiFi...");
          drawCenteredText(("Trying to reconnect \n WiFi..."), 1);
          WiFi.disconnect();
          WiFi.begin(SSID_NAME.c_str(), PASSWORD.c_str());
      }

      return;
  }

  static unsigned long lastValidateMillis = 0;

  if (!validateConfigs) {
      if (currentMillis - lastValidateMillis >= 500) {
          lastValidateMillis = currentMillis;
          sync_workstation();
      }
      return;
  }
  else {
      if (currentMillis - lastValidateMillis >= 2000 && !inputMode) {
          lastValidateMillis = currentMillis;
          sync_workstation();
      }
  }

  if (inputMode && currentMillis - previousInputMillis > inputModeInterval) {

    resetAllInputModes();
    inputMode = false;

    OPERATION = "";
    PRODUCTION_ORDER = "";
    USER_CODE = "";
    clearPartNumbers();
  }

  int sensorState1 = digitalRead(SENSOR_PIN_1);
  int sensorState2 = digitalRead(SENSOR_PIN_2);
  char key = keypad.getKey();

  if (key && (key == 'A' || key == 'B' || key == 'C' || key == 'D')) {
    telnetLog("Letter Key Pressed: " + String(key));
  }


  if (currentWorkStationState == INTERVENTION) {
    if (key == '#') {
      
      StaticJsonDocument<1024> payload;
      payload["method_key"] = "stop_intervention";

      ApiResult result = sendApiRequest(payload, "api/");
      handlestopIntervention(result);
    }

  }
  else if (currentWorkStationState == RUNNING) {

    if (currentMillis - previousMessageMillis > message_interval && !inputMode && starKeyPressedMillis == 0) {
        drawMainView();
    }

    // ========================================
    // MARK: LEITURA DO SENSOR 1
    // ========================================
    if (sensorState1 == LOW && currentMillis - previousPartMillis >= SENSOR_INTERVAL.toInt()) {

      // --- Pulse Signal (modo simples) ---
      if (SENSOR_READ_TYPE == "pulse_signal") {

        sysPrintln("Peça Conforme, modo simples!");

        StaticJsonDocument<1024> payload;
        payload["method_key"] = "insert_data";
        payload["status"] = "1";
        payload["production_note_type"] = "1";

        ApiResult result = sendApiRequest(payload, "api/");
        handleSimpleApiResult(result, false);

        previousPartMillis = currentMillis;
      }

      // --- Pulse Signal + Linked Steps ---
      else if (SENSOR_READ_TYPE == "pulse_signal_linked") {

        if (step == 0) {
          sysPrintln("Peça passou pelo primeiro estágio do modo step!");

          StaticJsonDocument<1024> payload;
          payload["method_key"] = "insert_data";
          payload["status"] = "1";
          payload["production_note_type"] = "1";

          ApiResult result = sendApiRequest(payload, "api/");
          handleSimpleApiResult(result, false);

          step = 1;
          previousPartMillis = currentMillis;
        }

        else if (step == 1) {
          sysPrintln("Última peça não conforme pelo modo step!");

          StaticJsonDocument<1024> payload;
          ApiResult result;

          payload["method_key"] = "insert_data";
          payload["status"] = "0";
          payload["production_note_type"] = "1";
          result = sendApiRequest(payload, "api/");
          handleSimpleApiResult(result, false);

          payload.clear();

          sysPrintln("Peça passou pelo primeiro estágio do modo step!");
          payload["method_key"] = "insert_data";
          payload["status"] = "1";
          payload["production_note_type"] = "1";
          result = sendApiRequest(payload, "api/");
          handleSimpleApiResult(result, false);

          step = 1;
          previousPartMillis = currentMillis;
        }
      }

      // --- Continuos Signal ---
      else if (SENSOR_READ_TYPE == "continuos_signal") {
        if (lastSignalState == false) {
          sysPrintln("Peça Conforme, modo contínuo!");

          StaticJsonDocument<1024> payload;
          payload["method_key"] = "insert_data";
          payload["status"] = "1";
          payload["production_note_type"] = "1";

          ApiResult result = sendApiRequest(payload, "api/");
          handleSimpleApiResult(result, false);

          lastSignalState = true;
          previousPartMillis = currentMillis;
        }
      }
    }

    if (sensorState1 == HIGH && lastSignalState == true && SENSOR_READ_TYPE == "continuos_signal") {
      lastSignalState = false;
    }

    // ========================================
    // MARK: LEITURA DO SENSOR 2
    // ========================================
    if (sensorState2 == LOW && currentMillis - previousPartMillis >= SENSOR_INTERVAL.toInt()) {
      if (SENSOR_READ_TYPE == "pulse_signal_linked") {
        if (step == 1) {
          sysPrintln("Peça passou pela verificação do step!");
          step = 0;
        }
        else if (step == 0) {
          sysPrintln("Não passou pela primeira etapa!");
        }
      }
      else {
        sysPrintln("Sensor 2 ignorado: modo pulse_signal_linked desligado.");
      }
      previousPartMillis = currentMillis;
    }


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////// INPUT MODES ////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    if (inputMode){

      //////////////////////////////////
      //////// INPUTS da tecla A ///////
      //////////////////////////////////

      if (nonConfirmingPartsTypeInputMode) {
        
        String pnList = "Select PN and\npress A:\n-----\n";
        
        for (int i = 0; i < PART_NUMBERS_COUNT; i++) {
          pnList += String(i + 1) + "-" + PART_NUMBERS[i].name + "\n";
        }
        
        drawCenteredText(pnList.c_str(), 1);
        
        if (key >= '1' && key <= '9') {
          int selectedIndex = (key - '0') - 1;
          
          if (selectedIndex < PART_NUMBERS_COUNT) {
            nonConfirmingPartsTypeCode = String(PART_NUMBERS[selectedIndex].id);
            
            sysPrint("PN selecionado para não conforme: ");
            sysPrintln(PART_NUMBERS[selectedIndex].name);
            
            nonConfirmingPartsTypeInputMode = false;
            nonConfirmingPartsQuantInputMode = true;
            previousDebounceMillis = currentMillis;
          }
        }
      }

      else if (nonConfirmingPartsQuantInputMode) {
        
        String selectedPNName = "";
        for (int i = 0; i < PART_NUMBERS_COUNT; i++) {
          if (String(PART_NUMBERS[i].id) == nonConfirmingPartsTypeCode) {
            selectedPNName = PART_NUMBERS[i].name;
            break;
          }
        }
        
        drawCenteredText(("PN: " + selectedPNName + "\nType quantity\nand press A:\n-----\n" + nonConfirmingPartsQuant).c_str(), 1);
        
        if (isdigit(key)){
          nonConfirmingPartsQuant += key;
        }
        else if (key == 'A'){
          inputMode = false;
          nonConfirmingPartsQuantInputMode = false;

          StaticJsonDocument<1024> payload;
          ApiResult result;

          payload["method_key"] = "insert_data";
          payload["status"] = "0";
          payload["production_note_type"] = "0";
          payload["quantity"] = nonConfirmingPartsQuant;
          payload["part_number"] = nonConfirmingPartsTypeCode;

          result = sendApiRequest(payload, "api/");
          handleSimpleApiResult(result, true);

          previousPartMillis = currentMillis;
          nonConfirmingPartsQuant = "";
          nonConfirmingPartsTypeCode = "";
        }
      }

      //////////////////////////////////
      //////// INPUTS da tecla B ///////
      //////////////////////////////////

      else if (deletePartsTypeInputMode) {
        
        String pnList = "Select PN and\npress B:\n-----\n";
        
        for (int i = 0; i < PART_NUMBERS_COUNT; i++) {
          pnList += String(i + 1) + "-" + PART_NUMBERS[i].name + "\n";
        }
        
        drawCenteredText(pnList.c_str(), 1);
        
        if (key >= '1' && key <= '9') {
          int selectedIndex = (key - '0') - 1;
          
          if (selectedIndex < PART_NUMBERS_COUNT) {
            deletePartsTypeCode = String(PART_NUMBERS[selectedIndex].id);
            
            sysPrint("PN selecionado para deletar: ");
            sysPrintln(PART_NUMBERS[selectedIndex].name);
            
            deletePartsTypeInputMode = false;
            deletePartsQuantInputMode = true;
            previousDebounceMillis = currentMillis;
          }
        }
      }
      
      else if (deletePartsQuantInputMode) {
        
        String selectedPNName = "";
        for (int i = 0; i < PART_NUMBERS_COUNT; i++) {
          if (String(PART_NUMBERS[i].id) == deletePartsTypeCode) {
            selectedPNName = PART_NUMBERS[i].name;
            break;
          }
        }
        
        drawCenteredText(("PN: " + selectedPNName + "\nType quantity\nand press B:\n-----\n" + deletePartsQuant).c_str(), 1);
        
        if (isdigit(key)){
          deletePartsQuant += key;
        }
        else if (key == 'B'){
          inputMode = false;
          deletePartsQuantInputMode = false;

          StaticJsonDocument<1024> payload;
          ApiResult result;

          payload["method_key"] = "delete_data";
          payload["quantity"] = deletePartsQuant;
          payload["part_number"] = deletePartsTypeCode;

          result = sendApiRequest(payload, "api/");
          handleSimpleApiResult(result, true);

          previousPartMillis = currentMillis;
          deletePartsQuant = "";
          deletePartsTypeCode = "";
        }
      }


      //////////////////////////////////
      //////// INPUTS da tecla C ///////
      //////////////////////////////////

      else if (operatorInputMode) {
        drawCenteredText(("Type operator ID \n and press C: \n ----- \n" + USER_CODE).c_str(), 1);
        if (isdigit(key)){
          USER_CODE += key;
        }
        else if (key == 'C') {
          inputMode = false;
          operatorInputMode = false;

          StaticJsonDocument<1024> payload;
          ApiResult result;

          payload["method_key"] = "change_user";

          result = sendApiRequest(payload, "setup_controller/");
          if (!handleSimpleApiResult(result, true)) {
            USER_CODE = ""; 
          }
          saveConfig("USER_CODE", USER_CODE);
        }
      }

      //////////////////////////////////
      //////// INPUTS da tecla D ///////
      //////////////////////////////////

      // Passo 1: Digitar código da operação
      if (operationInputMode){
        drawCenteredText(("Type operation ID \n and press D: \n --- \n" + OPERATION).c_str(), 1);
        
        if (isdigit(key)){
          OPERATION += key;
        }
        else if (key == 'D') {
          operationInputMode = false;
          workOrderInputMode = true;
          previousDebounceMillis = currentMillis;
        }
      }

      // Passo 2: Digitar código da ordem de produção (ou pular)
      else if (workOrderInputMode) {

        drawCenteredText(
                        ("Type work order code\n"
                        "and press D or just \n"
                        "press D to continue:\n  ------ \n" + PRODUCTION_ORDER).c_str(), 1);

        if (isdigit(key)) {
          PRODUCTION_ORDER += key;
          sysPrintln(PRODUCTION_ORDER);
        }

        else if (key == 'D'){
          workOrderInputMode = false;

          StaticJsonDocument<1024> payload;
          ApiResult result;

          payload["method_key"] = "change_operation";
          payload["final_step"] = 0;

          result = sendApiRequest(payload, "setup_controller/");
          
          if (result.httpCode >= 200 && result.httpCode < 300) {
            JsonObject obj = result.json.as<JsonObject>();
            
            if (obj["get_more_info"] == 1) {

              // Popula PART_NUMBERS a partir da resposta do servidor,
              // garantindo que o array está atualizado antes de verificar a quantidade
              clearPartNumbers();
              JsonObject pns = obj["part_numbers"].as<JsonObject>();
              for (JsonPair kv : pns) {
                if (PART_NUMBERS_COUNT >= MAX_PART_NUMBERS) break;
                JsonObject pn = kv.value().as<JsonObject>();
                PART_NUMBERS[PART_NUMBERS_COUNT].id                = pn["id"].as<int>();
                PART_NUMBERS[PART_NUMBERS_COUNT].name              = pn["name"].as<String>();
                PART_NUMBERS[PART_NUMBERS_COUNT].parts_to_produce  = 0;
                PART_NUMBERS[PART_NUMBERS_COUNT].good_parts_produced = 0;
                PART_NUMBERS[PART_NUMBERS_COUNT].bad_parts_produced  = 0;
                PART_NUMBERS_COUNT++;
              }
              sysPrintln("PNs carregados da resposta: " + String(PART_NUMBERS_COUNT));

              if (PART_NUMBERS_COUNT > 1) {
                partsQuantityPerPNInputMode = true;
                currentPNInputIndex = 0;
                
                for (int i = 0; i < MAX_PART_NUMBERS; i++) {
                  partsQuantityPerPN[i] = 0;
                }
                
                inputMode = true;
                sysPrintln("Iniciando input de quantidades por PN...");
              } 
              else if (PART_NUMBERS_COUNT == 1) {
                partsToProdInputMode = true;
                inputMode = true;
              }
              else {
                inputMode = false;
                drawCenteredText("No PNs available!", 1);
                previousMessageMillis = currentMillis;
              }
              
            } else {
              inputMode = false;
            }
            
            sysPrintln(result.httpCode);
            sysPrintln(obj["message"].as<const char*>());
            
          } else {
            OPERATION = "";
            PRODUCTION_ORDER = "";
            inputMode = false;
            
            if (result.httpCode < 0) {
              drawCenteredText("NETWORK ERROR!", 1);
            } else {
              JsonObject obj = result.json.as<JsonObject>();
              drawCenteredText(obj["message"].as<const char*>(), 1);
            }
            previousMessageMillis = currentMillis;
          }
        }
      }

      // Passo 3a: Input de quantidade para cada PN (múltiplos PNs)
      else if (partsQuantityPerPNInputMode) {
        
        if (currentPNInputIndex < PART_NUMBERS_COUNT) {
          
          PartNumberData* currentPN = &PART_NUMBERS[currentPNInputIndex];
          
          String displayText = "PN " + String(currentPNInputIndex + 1) + "/" + String(PART_NUMBERS_COUNT) + ":\n";
          displayText += currentPN->name.substring(0, 12) + "\n";
          displayText += "Type quantity\nand press D:\n-----\n";
          displayText += String(partsQuantityPerPN[currentPNInputIndex]);
          
          drawCenteredText(displayText.c_str(), 1);
          
          if (isdigit(key)) {
            partsQuantityPerPN[currentPNInputIndex] = 
              partsQuantityPerPN[currentPNInputIndex] * 10 + (key - '0');
            
            sysPrint("PN ");
            sysPrint(currentPN->name);
            sysPrint(" - Quantidade: ");
            sysPrintln(partsQuantityPerPN[currentPNInputIndex]);
          }
          else if (key == 'D') {
            currentPNInputIndex++;
            previousDebounceMillis = currentMillis;
            
            if (currentPNInputIndex >= PART_NUMBERS_COUNT) {
              partsQuantityPerPNInputMode = false;
              inputMode = false;
              
              StaticJsonDocument<1024> payload;
              ApiResult result;

              payload["method_key"] = "change_operation";
              payload["final_step"] = 1;
              
              JsonObject pnQuantities = payload.createNestedObject("part_numbers_quantities");
              
              for (int i = 0; i < PART_NUMBERS_COUNT; i++) {
                String pnId = String(PART_NUMBERS[i].id);
                pnQuantities[pnId] = partsQuantityPerPN[i];
                
                sysPrint("Enviando - PN ID ");
                sysPrint(pnId);
                sysPrint(": ");
                sysPrint(partsQuantityPerPN[i]);
                sysPrintln(" peças");
              }

              result = sendApiRequest(payload, "setup_controller/");
              handleChangeOperation(result);
            }
          }
        }
      }

      // Passo 3b: Input de quantidade única (somente 1 PN)
      else if (partsToProdInputMode) {
        drawCenteredText(
                  ("Type parts quantity to \n"
                  "production and press D\n\n"
                  "or just press D to\n"
                  "quantity = 0 \n ------ \n" + String(partsQuantityPerPN[0])).c_str(), 1);

        if (isdigit(key)) {
          partsQuantityPerPN[0] = partsQuantityPerPN[0] * 10 + (key - '0');
        }

        else if (key == 'D'){
          partsToProdInputMode = false;
          inputMode = false;

          StaticJsonDocument<1024> payload;
          ApiResult result;

          payload["method_key"] = "change_operation";
          payload["final_step"] = 1;
          
          JsonObject pnQuantities = payload.createNestedObject("part_numbers_quantities");
          String pnId = String(PART_NUMBERS[0].id);
          pnQuantities[pnId] = partsQuantityPerPN[0];

          result = sendApiRequest(payload, "setup_controller/");
          handleChangeOperation(result);
        }
      }

      //////////////////////////////////
      //////// INPUTS da tecla # ///////
      //////////////////////////////////
      else if (interventionInputMode) {
        drawCenteredText(("Type intervention \n code \n and press #: \n ----- \n" + interventionCode).c_str(), 1);
                    
        if (isdigit(key)){
          interventionCode += key;
        }
        else if (key == '#'){
          inputMode = false;
          interventionInputMode = false;

          StaticJsonDocument<1024> payload;
          ApiResult result;

          payload["method_key"] = "start_intervention";
          payload["identification_number"] = interventionCode;
          
          result = sendApiRequest(payload, "api/");

          if (handleSimpleApiResult(result, true)) {
            currentWorkStationState = INTERVENTION;

            digitalWrite(DEBUG_LED, LOW);
            digitalWrite(INTERVENTION_LED, HIGH); 
          }

          previousPartMillis = currentMillis;
          interventionCode = "";
        }
      }

      // Não deixa chegar nas teclas caso tenha algum input ativo
      return;
    }


    /////////////////////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////// KEYPAD ////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////

    if (key >= '1' && key <= '9' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      
      int keyNumber = key - '0';
      int targetIndex = keyNumber - 1;
      
      if (targetIndex < PART_NUMBERS_COUNT) {
        CURRENT_PN_INDEX = targetIndex;
        
        PartNumberData* selectedPN = getCurrentPartNumber();
        if (selectedPN != nullptr) {
          sysPrint("Part Number selecionado: ");
          sysPrint(selectedPN->name);
          sysPrint(" (Index: ");
          sysPrint(CURRENT_PN_INDEX);
          sysPrintln(")");
          
          String message = "PN " + String(CURRENT_PN_INDEX + 1) + "/" + String(PART_NUMBERS_COUNT) + ":\n" + selectedPN->name;
          drawCenteredText(message.c_str(), 1);
          previousMessageMillis = currentMillis;
        }
      } 
      else {
        sysPrint("Botão ");
        sysPrint(keyNumber);
        sysPrintln(" - Nenhum PN disponível neste índice");
        
        String message = "PN " + String(keyNumber) + "\n não existe!";
        drawCenteredText(message.c_str(), 1);
        previousMessageMillis = currentMillis;
      }
      
      previousDebounceMillis = currentMillis;
    }

    // Tecla A - Peças não conformes
    else if (key == 'A' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      resetAllInputModes(); // FIX 2: garante estado limpo antes de abrir novo fluxo
      inputMode = true;
      nonConfirmingPartsTypeInputMode = true;

      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }

    // Tecla B - Deletar peças
    else if (key == 'B' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      resetAllInputModes(); // FIX 2: garante estado limpo antes de abrir novo fluxo
      inputMode = true;
      deletePartsTypeInputMode = true;

      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }
    
    // Tecla C - Trocar operador
    else if (key == 'C' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      resetAllInputModes(); // FIX 2: garante estado limpo antes de abrir novo fluxo
      inputMode = true;
      operatorInputMode = true;
      USER_CODE = "";
      
      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }

    // Tecla D - Setup de operação / ordem de produção
    else if (key == 'D' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      resetAllInputModes(); // FIX 2: garante estado limpo antes de abrir novo fluxo
      inputMode = true;
      operationInputMode = true;
      OPERATION = "";
      PRODUCTION_ORDER = "";

      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }
    
    // Tecla # - Abertura de Intervenção
    else if (key == '#' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      resetAllInputModes(); // FIX 2: garante estado limpo antes de abrir novo fluxo
      inputMode = true;
      interventionInputMode = true;
      
      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }

    // =====================================================
    // DETECÇÃO DO PRIMEIRO TOQUE no botão *
    // =====================================================
    else if (key == '*' && currentMillis - previousDebounceMillis >= debounce_keypad) {
        starKeyPressedMillis = currentMillis;
        previousDebounceMillis = currentMillis;
    }

    // =====================================================
    // DETECÇÃO DO PRIMEIRO TOQUE no botão 0
    // 0 curto → não faz nada
    // 0 longo (3s) → envia clear_setup para a API
    // =====================================================
    else if (key == '0' && currentMillis - previousDebounceMillis >= debounce_keypad) {
        zeroKeyPressedMillis = currentMillis;
        previousDebounceMillis = currentMillis;
    }

    // =====================================================
    // MONITORAMENTO DO BOTÃO * ENQUANTO ESTIVER SEGURADO
    // =====================================================
    if (starKeyPressedMillis > 0) {

        bool starHeld = false;
        keypad.getKeys();
        for (int i = 0; i < LIST_MAX; i++) {
            if (keypad.key[i].kchar == '*' &&
                (keypad.key[i].kstate == PRESSED || keypad.key[i].kstate == HOLD)) {
                starHeld = true;
                break;
            }
        }

        if (!starHeld) {
            if (currentMillis - starKeyPressedMillis < starKeyResetInterval) {
                currentWorkStationState = STANDBY;
                digitalWrite(DEBUG_LED, false);
                digitalWrite(INTERVENTION_LED, false);
                sysPrintln("Standby Mode");
                drawCenteredText("Standby Mode", 1);
            }
            starKeyPressedMillis = 0;
        }
        else if (currentMillis - starKeyPressedMillis >= starKeyResetInterval) {
            sysPrintln("Reiniciando ESP32...");
            drawCenteredText("Restarting...", 1);
            delay(1000);
            ESP.restart();
        }
        else {
            int segundosRestantes = (starKeyResetInterval / 1000) - ((currentMillis - starKeyPressedMillis) / 1000);
            drawCenteredText(("Hold to restart\n" + String(segundosRestantes) + "s...").c_str(), 1);
        }
    }

    // =====================================================
    // MONITORAMENTO DO BOTÃO 0 ENQUANTO ESTIVER SEGURADO
    // =====================================================
    if (zeroKeyPressedMillis > 0) {

        bool zeroHeld = false;
        keypad.getKeys();
        for (int i = 0; i < LIST_MAX; i++) {
            if (keypad.key[i].kchar == '0' &&
                (keypad.key[i].kstate == PRESSED || keypad.key[i].kstate == HOLD)) {
                zeroHeld = true;
                break;
            }
        }

        if (!zeroHeld) {
            // Solto antes de 3s → não faz nada
            zeroKeyPressedMillis = 0;
        }
        else if (currentMillis - zeroKeyPressedMillis >= zeroKeyClearInterval) {
            // Segurado 3s+ → envia clear_setup para a API
            zeroKeyPressedMillis = 0;

            sysPrintln("Enviando clear_setup...");
            drawCenteredText("Clearing\nsetup...", 1);

            StaticJsonDocument<1024> payload;
            payload["method_key"] = "clear_setup";

            ApiResult result = sendApiRequest(payload, "setup_controller/");

            if (handleSimpleApiResult(result, true)) {
                USER_CODE = "";
                USER_NAME = "";
                OPERATION = "";
                PRODUCTION_ORDER = "";
                clearPartNumbers();
                sysPrintln("Setup cleared!");
            }

            previousMessageMillis = currentMillis;
        }
        else {
            // Ainda segurado, ainda não chegou em 3s → mostra contagem regressiva
            int segundosRestantes = (zeroKeyClearInterval / 1000) - ((currentMillis - zeroKeyPressedMillis) / 1000);
            drawCenteredText(("Hold to clear\nsetup\n" + String(segundosRestantes) + "s...").c_str(), 1);
        }
    }
  }
}