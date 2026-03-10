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

// Chave de API para autenticação nas requisições
const char* API_KEY = "9fK$29LmQx!7pR#2Zt8@Vy6WsXc3BnT"; 

// Criando servidor web
WebServer server(80);

// Serão exibidos esses valores default para a primeira vez do hardware
String WORK_STATION = "SUA_WORK_STATION";                                                                                 
String SSID_NAME = "SEU_SSID";                             
String PASSWORD = "SEU_PASSWORD";
String SERVER_IP = "IP_SERVIDOR";
String SERVER_PORT = "8000";
String SENSOR_READ_TYPE = "normal_signal"; // Tipo de leitura do sensor: normal_signal (FECHAMENTO GND + DIGITAL INPUT + DEBOUNCE) ou continuos_inverted_signal (FECHAMENTO 3v3 + DIGITAL INPUT + LÓFICA DE SUBIDA DE RAMPA)
String SENSOR_INTERVAL = "1000"; // Intevalo com padrão de 1 seg para cada leitura do sensor de ciclo
String DISPLAY_TYPE = "oled_096"; // Display 0.96 polegadas como default, opção de oled_128 para display 1.28 polegadas


// Variáveis para armazenar os dados de setup
String USER_CODE = "";
String PART_NUMBER = "";
String PARTS_PER_CYCLE = "";
String WORK_ORDER = "";

// Variáveis para armazenar a produção para aquele setup específico, qualquer alteração no botão D esses valores são zerados
String GOOD_PARTS_PRODUCED = "0";
String BADS_PARTS_PRODUCED = "0";


// MARK: Keypad variaveis
bool inputMode = false; // Variável utilizada para não atulizar o display quando estiver em modo de inputMode
unsigned long previousInputMillis = 0; // Variável que vai assumir o valor de millis toda vez que for acionada uma condição de input
const unsigned long inputModeInterval = 30000; // Variável de tempo máximo de 30 para digitar os inputs desejados


bool nonConfirmingPartsInputMode = false;
bool nonConfirmingPartsQuantInputMode = false;
String nonConfirmingPartsTypeCode = "";
String nonConfirmingPartsQuant = "";

bool deletePartsInputMode = false;
bool deletePartsQuantInputMode = false;
String deletePartsTypeCode = "";
String deletePartsQuant = "";

bool operatorInputMode = false; // Variável para entrar no modo de input do operador

bool partNumberInputMode = false; // Variável para entrar no modo input do código do produto
bool partsPerCycleInputMode = false; // Variável para entrar no modo input de quantidade de peças por ciclo
bool workOrderInputMode = false; // Variável para entrar no modo input da work order


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

#define DEBUG_LED 2             // Digital Output para o led de debug (azul)
#define INTERVENTION_LED 15     // Digital Ouput para o led de interveções (vermelho)
#define SENSOR_PIN_1 4          // Digital Input para realizar as leituras de peça
#define SENSOR_PIN_2 19         // Digital Input para realizar as leituras de peça com o linked_steps ligado
bool lastSignalState = 0;       // Variavel que vai ser utilizada no código da leitura de peças utilizando o sensor de presença

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
const unsigned long message_interval = 3000; // Intervalo de 2 seg de exibição da mensagem no display após intereção do usuário

const unsigned long starKeyResetInterval = 3000; // Intervalo de tempo colocado para caso o botão * fique segurado por mais de 3 segundos ele reinicia
unsigned long starKeyPressedMillis = 0; // Variável que armazena o momento em que o botão foi * foi apertado

// Variável para armazenar o tempo da última solicitação
unsigned long previousReadMillis = 0;
unsigned long previousPartMillis = 0;
unsigned long previousDebounceMillis = 0;
unsigned long previousMessageMillis = 0;

// Variáveis para utilizar na função checkServerHealth
bool serverConnected = false;
unsigned long lastServerCheckMillis = 0;
const unsigned long serverCheckInterval = 10000;


// Variáveis para controle dos arquivos de configuração
enum FileIndex {
    CONFIG     = 0,
    SETUP      = 1,
    PRODUCTION = 2,
    BUFFER     = 3
};

const char* files[] = {
    "/config.txt",      // Arquivo de Configuração
    "/setup.txt",       // Arquivo para armazenar as configurações de setup do pn que está sendo produzido   
    "/production.txt",  // Arquivo para armazenar a produção do dia, zerado a cada 24h
    "/buffer.txt"       // // Arquivo para armazenar as peças que não foram enviadas para o servidor por conta de falhas de conexão, para tentar enviar novamente a cada ciclo. Ainda não está implementado
};

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

/////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////// MARK: WRITE SPIFFS
/////////////////////////////////////////////////////////////////////////////////////////////////////

void parseLine(String line) {
    int separatorIndex = line.indexOf('=');
    if (separatorIndex == -1) return;

    String param = line.substring(0, separatorIndex);
    String value = line.substring(separatorIndex + 1);

    if      (param == "WORK_STATION")        WORK_STATION        = value;
    else if (param == "SSID_NAME")           SSID_NAME           = value;
    else if (param == "PASSWORD")            PASSWORD            = value;
    else if (param == "SERVER_IP")           SERVER_IP           = value;
    else if (param == "SERVER_PORT")         SERVER_PORT         = value;
    else if (param == "SENSOR_READ_TYPE")    SENSOR_READ_TYPE    = value;
    else if (param == "SENSOR_INTERVAL")     SENSOR_INTERVAL     = value;
    else if (param == "DISPLAY_TYPE")        DISPLAY_TYPE        = value;
    else if (param == "USER_CODE")           USER_CODE           = value;
    else if (param == "WORK_ORDER")    WORK_ORDER    = value;
    else if (param == "PART_NUMBER")         PART_NUMBER         = value;
    else if (param == "PARTS_PER_CYCLE")     PARTS_PER_CYCLE     = value;
    else if (param == "GOOD_PARTS_PRODUCED") GOOD_PARTS_PRODUCED = value;
    else if (param == "BADS_PARTS_PRODUCED") BADS_PARTS_PRODUCED = value;
}

void writeConfigFile() {
    File file = SPIFFS.open(files[CONFIG], FILE_WRITE);
    if (!file) { Serial.println("Erro ao abrir " + String(files[CONFIG]) + "!"); return; }

    file.println("WORK_STATION="      + WORK_STATION);
    file.println("SSID_NAME="         + SSID_NAME);
    file.println("PASSWORD="          + PASSWORD);
    file.println("SERVER_IP="         + SERVER_IP);
    file.println("SERVER_PORT="       + SERVER_PORT);
    file.println("SENSOR_READ_TYPE="  + SENSOR_READ_TYPE);
    file.println("SENSOR_INTERVAL="   + SENSOR_INTERVAL);
    file.println("DISPLAY_TYPE="      + DISPLAY_TYPE);

    file.close();
}

void writeSetupFile() {
    File file = SPIFFS.open(files[SETUP], FILE_WRITE);
    if (!file) { Serial.println("Erro ao abrir " + String(files[SETUP]) + "!"); return; }

    file.println("USER_CODE="        + USER_CODE);
    file.println("PART_NUMBER="      + PART_NUMBER);
    file.println("PARTS_PER_CYCLE="  + PARTS_PER_CYCLE);
    file.println("WORK_ORDER=" + WORK_ORDER);

    file.close();
}

void writeProductionFile() {
    File file = SPIFFS.open(files[PRODUCTION], FILE_WRITE);
    if (!file) { Serial.println("Erro ao abrir " + String(files[PRODUCTION]) + "!"); return; }

    file.println("GOOD_PARTS_PRODUCED=" + GOOD_PARTS_PRODUCED);
    file.println("BADS_PARTS_PRODUCED=" + BADS_PARTS_PRODUCED);

    file.close();
}

void saveConfig(String param, String value, FileIndex fileIndex) {
    parseLine(param + "=" + value);

    switch (fileIndex) {
        case CONFIG:     writeConfigFile();     break;
        case SETUP:      writeSetupFile();      break;
        case PRODUCTION: writeProductionFile(); break;
        default:
            Serial.println("Arquivo inválido: " + String(files[fileIndex]));
            return;
    }

    Serial.println("Parâmetro " + param + " atualizado com sucesso!");
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////// MARK: LOAD SPIFFS
/////////////////////////////////////////////////////////////////////////////////////////////////////

void createDefaultFile(FileIndex fileIndex) {
    Serial.println("Criando arquivo padrão: " + String(files[fileIndex]));

    switch (fileIndex) {
        case CONFIG:
            saveConfig("WORK_STATION",     WORK_STATION,     CONFIG);
            saveConfig("SSID_NAME",        SSID_NAME,        CONFIG);
            saveConfig("PASSWORD",         PASSWORD,         CONFIG);
            saveConfig("SERVER_IP",        SERVER_IP,        CONFIG);
            saveConfig("SERVER_PORT",      SERVER_PORT,      CONFIG);
            saveConfig("SENSOR_READ_TYPE", SENSOR_READ_TYPE, CONFIG);
            saveConfig("SENSOR_INTERVAL",  SENSOR_INTERVAL,  CONFIG);
            saveConfig("DISPLAY_TYPE",     DISPLAY_TYPE,     CONFIG);
            break;

        case SETUP:
            saveConfig("USER_CODE",        USER_CODE,        SETUP);
            saveConfig("WORK_ORDER", WORK_ORDER, SETUP);
            saveConfig("PART_NUMBER",      PART_NUMBER,      SETUP);
            saveConfig("PARTS_PER_CYCLE",  PARTS_PER_CYCLE,  SETUP);
            break;

        case PRODUCTION:
            saveConfig("GOOD_PARTS_PRODUCED", GOOD_PARTS_PRODUCED, PRODUCTION);
            saveConfig("BADS_PARTS_PRODUCED", BADS_PARTS_PRODUCED, PRODUCTION);
            break;

        default:
            Serial.println("Arquivo sem configuração padrão: " + String(files[fileIndex]));
            break;
    }
}

void loadConfig() {
    int files_count = sizeof(files) / sizeof(files[0]);

    for (int i = 0; i < files_count; i++) {
        File file = SPIFFS.open(files[i], FILE_READ);

        if (!file) {
            Serial.println("Arquivo " + String(files[i]) + " não encontrado!");
            createDefaultFile((FileIndex)i);
            continue; // continua para o próximo arquivo
        }

        while (file.available()) {
            String line = file.readStringUntil('\n');
            line.trim();
            parseLine(line);
        }

        file.close();
    }

    Serial.println("Configurações carregadas da SPIFFS:");
    Serial.println("WORK_STATION: "          + WORK_STATION);
    Serial.println("SSID_NAME: "             + SSID_NAME);
    Serial.println("PASSWORD: "              + PASSWORD);
    Serial.println("SERVER_IP: "             + SERVER_IP);
    Serial.println("SERVER_PORT: "           + SERVER_PORT);
    Serial.println("SENSOR_READ_TYPE: "      + SENSOR_READ_TYPE);
    Serial.println("SENSOR_INTERVAL: "       + SENSOR_INTERVAL);
    Serial.println("DISPLAY_TYPE: "          + DISPLAY_TYPE);
    Serial.println("USER_CODE: "             + USER_CODE);
    Serial.println("WORK_ORDER: "      + WORK_ORDER);
    Serial.println("PART_NUMBER: "           + PART_NUMBER);
    Serial.println("PARTS_PER_CYCLE: "       + PARTS_PER_CYCLE);
    Serial.println("GOOD_PARTS_PRODUCED: "   + GOOD_PARTS_PRODUCED);
    Serial.println("BADS_PARTS_PRODUCED: "   + BADS_PARTS_PRODUCED);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////// MARK: WEB SERVER
/////////////////////////////////////////////////////////////////////////////////////////////////////

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
          <input type="password" name="password" value=")rawliteral" + PASSWORD + R"rawliteral("><br><br>
  
      <h2>Production Hub Settings</h2>
          <label>Work Station Name:</label><br>
          <input type="text" name="work-station" value=")rawliteral" + WORK_STATION + R"rawliteral("><br><br>
          <label>Server IP:</label><br>
          <input type="text" name="server-ip" value=")rawliteral" + SERVER_IP + R"rawliteral("><br><br>
          <label>Server Port:</label><br>
          <input type="text" name="server-port" value=")rawliteral" + SERVER_PORT + R"rawliteral("><br><br
          
          
          <label>Sensor Read Type:</label><br>
          <select name="sensor_read_type" id="sensor_read_type">
            <option value="normal_signal" )rawliteral" + (SENSOR_READ_TYPE == "normal_signal" ? "selected" : "") + R"rawliteral(> Normal Signal" </option>
            <option value="continuos_signal" )rawliteral" + (SENSOR_READ_TYPE == "continuos_signal" ? "selected" : "") + R"rawliteral(> Continuos Signal"</option>
          </select><br><br>

          <label>Sensor Interval (s):</label><br>
          <input type="number" name="sensor-interval" min="0.5" step="0.5" value=")rawliteral" + String(SENSOR_INTERVAL.toFloat()/1000.0, 1) + R"rawliteral("><br><br>       
          </label><br><br>

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
  String newSensorReadType = server.arg("sensor_read_type");
  String newSensorInterval = String(server.arg("sensor-interval").toFloat() * 1000);
  String newDisplayType = server.arg("display");
  String newLinkedStepsMode = server.arg("linked-steps");

  if (newSSID.length() > 0 && newPass.length() > 0 && newWorkStation.length() > 0 && newServerIp.length() > 0 && newServerPort.length() > 0) {
            
      saveConfig("SSID_NAME", newSSID, CONFIG);
      saveConfig("PASSWORD", newPass, CONFIG);
      saveConfig("WORK_STATION", newWorkStation, CONFIG);
      saveConfig("SERVER_IP", newServerIp, CONFIG);
      saveConfig("SERVER_PORT", newServerPort, CONFIG);
      saveConfig("SENSOR_READ_TYPE", newSensorReadType, CONFIG);
      saveConfig("SENSOR_INTERVAL", newSensorInterval, CONFIG);
      saveConfig("DISPLAY_TYPE", newDisplayType, CONFIG);
      saveConfig("LINKED_STEPS_MODE", newLinkedStepsMode, CONFIG);

      server.send(200, "text/html", "<h2>Configuration saved! Restarting...</h2>");
      delay(2000);
      ESP.restart();
  } else {
      server.send(400, "text/html", "<h2>Error: values cannot be empty</h2>");
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////// MARK: DISPLAY
/////////////////////////////////////////////////////////////////////////////////////////////////////
void initDisplay() {
    if (display != nullptr) delete display;

    SCREEN_WIDTH  = 128;
    SCREEN_HEIGHT = (DISPLAY_TYPE == "oled_24") ? 128 : 64;

    display = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

    if (!display->begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("Falha ao inicializar o display OLED");
        while (1);
    }

    display->clearDisplay();
    display->display();
    Serial.println("Display iniciado: " + String(SCREEN_WIDTH) + "x" + String(SCREEN_HEIGHT));
}

void drawCenteredText(const char* text, uint8_t textSize) {
  display->clearDisplay();  // Limpa a tela
  display->setTextSize(textSize);
  display->setTextColor(WHITE);

  // Variáveis para armazenar cada linha do texto
  int16_t x1, y1;
  uint16_t textWidth, textHeight;

  // Criamos uma cópia mutável do texto para manipular as quebras de linha
  char textBuffer[128]; 
  strncpy(textBuffer, text, sizeof(textBuffer));  
  textBuffer[sizeof(textBuffer) - 1] = '\0'; // Garante terminação segura

  // Conta o número de linhas quebrando o texto nos espaços ('\n')
  char* line = strtok(textBuffer, "\n");
  int lineCount = 0;
  char* lines[10]; // Suporta até 10 linhas

  while (line != NULL && lineCount < 10) {
    lines[lineCount++] = line;
    line = strtok(NULL, "\n");
  }

  // Calcula a altura total do bloco de texto
  int totalHeight = lineCount * (8 * textSize); // Cada linha tem 8px de altura no tamanho 1

  // Posição Y inicial para centralizar todo o bloco de texto
  int16_t startY = (SCREEN_HEIGHT - totalHeight) / 2;

  // Desenha cada linha separadamente
  for (int i = 0; i < lineCount; i++) {
    display->getTextBounds(lines[i], 0, 0, &x1, &y1, &textWidth, &textHeight);
    int16_t x = (SCREEN_WIDTH - textWidth) / 2;
    int16_t y = startY + i * (8 * textSize); // Move cada linha para baixo
    display->setCursor(x, y);
    display->println(lines[i]);
  }

  display->display(); // Atualiza o display
}

void drawMainView() {
    display->clearDisplay();

    String lines[] = {
        "AP:" + String(apSSID),
        "-----------",
        "User:" + USER_CODE,
        "OS:" + WORK_ORDER,
        "PN (" + PARTS_PER_CYCLE + "):" + PART_NUMBER,
        "PARTS:" + GOOD_PARTS_PRODUCED + "/" + BADS_PARTS_PRODUCED
    };
    int lineCount = sizeof(lines) / sizeof(lines[0]);

    uint8_t textSize = 1;
    int lineHeight = 8 * textSize;
    int lineSpacing = 1;
    int totalHeight = lineCount * lineHeight;
    int16_t startY = (SCREEN_HEIGHT - totalHeight) / 2;

    int16_t x1, y1;
    uint16_t textWidth, textHeight;

    for (int i = 0; i < lineCount; i++) {
        const char* lineStr = lines[i].c_str();
        display->getTextBounds(lineStr, 0, 0, &x1, &y1, &textWidth, &textHeight);
        int16_t x = (i == 0 || i == 1) ? (SCREEN_WIDTH - textWidth) / 2 : 0;
        int16_t y = startY + i * (lineHeight + lineSpacing);

        display->setTextSize(textSize);
        display->setTextColor(WHITE);
        display->setCursor(x, y);
        display->println(lineStr);
    }

    display->display();
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////// MARK: API REQUESTS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct ApiResult {
	int httpCode;
	bool hasJson;
	StaticJsonDocument<1024> json;
};

ApiResult sendApiRequest(StaticJsonDocument<1024>& payload, const String& route) {

	ApiResult result;
	result.httpCode = -1;
	result.hasJson = false;

	// Campos comuns e aplicada a lógica de que caso algum valor seja "" ele envia 0 para que o servidor possa processar
  payload["nome_maquina"] = WORK_STATION;
  payload["cod_usuario"] = USER_CODE.length() ? USER_CODE : "0";
  payload["cod_produto"] = PART_NUMBER.length() ? PART_NUMBER : "0";
  payload["num_ordem"] = WORK_ORDER.length() ? WORK_ORDER : "0";
  payload["rssi"] = WiFi.RSSI();

	String requestUrl = "http://" + SERVER_IP + ":" + SERVER_PORT + "/" + route;

	HTTPClient http;
	if (!http.begin(requestUrl)) {
		return result; // network-error
	}

	http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

	String body;
	serializeJson(payload, body);

  // ✅ Print do request
  Serial.println("========== REQUEST ==========");
  Serial.println("POST " + requestUrl);
  Serial.println("Body: " + body);
  Serial.println("=============================");

	result.httpCode = http.POST(body);
  //Serial.println(result.json);


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


bool checkServerHealth() {
    HTTPClient http;
    String url = "http://" + SERVER_IP + ":" + SERVER_PORT + "/health";

    if (!http.begin(url)) { http.end(); return false; }

    http.addHeader("accept", "application/json");
    http.addHeader("X-API-Key", API_KEY);

    int httpCode = http.GET();
    http.end();

    return (httpCode == 200);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////// MARK: HANDLERS 
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool handleSimpleApiResult(ApiResult& result, bool showSuccessMessage = true, bool successfulBlink = true) {

    // === network-error ===
    if (result.httpCode < 0) {
      Serial.println("network-error");
      return false;
    }

    JsonObject obj = result.json.as<JsonObject>();

    // === erro HTTP ou erro 404 proposital do servidor ===
    if (result.httpCode < 200 || result.httpCode >= 300) {
        Serial.print("HTTP error: ");
        Serial.println(result.httpCode);

        Serial.println("Erro: " + result.httpCode);
        drawCenteredText("Erro: " + result.httpCode, 1);
        previousMessageMillis = millis();
        return false;
    }
    else {
        // Caso não tenha nenhum specific handle ele faz a lógica simples
        // Como não é todos os casos que eu quero exibir a mensagem do servidor, passo a flag como false
        if (showSuccessMessage){
            drawCenteredText("Não está configurado msg no servidor", 1);
            previousMessageMillis = millis();
        }

        // Ativa o piscar azul do led
        if (successfulBlink) {
          successfulResponse();
        }
                        
        // Mostrar na Serial o JSON de response enviado pelo servidor
        Serial.print("Response JSON: ");
        serializeJsonPretty(obj, Serial);
        Serial.println();
        return true;
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////// MARK: SETUP
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void setup() {

  Serial.begin(115200);

  // Inicializa o watchdog no core 0
  esp_task_wdt_init(WDT_TIMEOUT, true);  
  esp_task_wdt_add(NULL);  // Adiciona a tarefa atual ao watchdog

  if (!SPIFFS.begin(true)) {
    Serial.println("Erro ao montar SPIFFS!");
    return;
  }

  loadConfig();
  initDisplay();
  
  pinMode(DEBUG_LED, OUTPUT);        // Configura o led de debug
  pinMode(INTERVENTION_LED, OUTPUT); // Configura o led de intervenções

  
  pinMode(SENSOR_PIN_1, INPUT_PULLUP);    // Configura o pino da entrada com resistor pull-up (sinal HIGH)
  pinMode(SENSOR_PIN_2, INPUT_PULLUP);    // Configura o pino da entrada com resistor pull-up (sinal HIGH)

  bool ledState = false;  // Variável para armazenar o estado do LED

  // Conecte-se ao WiFi
  WiFi.begin(SSID_NAME.c_str(), PASSWORD.c_str());
  Serial.println("Connecting to WiFi...");
  drawCenteredText("Connecting to WiFi...", 1);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    ledState = !ledState;
    digitalWrite(DEBUG_LED, ledState);
    delay(1000);
    Serial.print(".");
  }

  // Obtendo o MAC Address
  uint8_t mac[6];
  WiFi.macAddress(mac);

  // Criando o SSID dinâmico com os 3 últimos bytes do MAC
  sprintf(apSSID, "ESP32_%02X%02X%02X", mac[3], mac[4], mac[5]);

  // Ativando o Access Point com o novo SSID
  WiFi.softAP(apSSID, apPassword);
  Serial.println("Hotspot ativo!");
  Serial.print("Endereço IP do AP: ");
  Serial.println(WiFi.softAPIP());

  // Configuração do servidor web
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
 

  // Define estado inicial dos LEDs baseado no estado atual
  if (currentWorkStationState == INTERVENTION) {
      digitalWrite(DEBUG_LED, LOW);
      digitalWrite(INTERVENTION_LED, HIGH);
  } else {
      digitalWrite(DEBUG_LED, HIGH);
      digitalWrite(INTERVENTION_LED, LOW);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////// MARK: LOOP
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void loop() {

  server.handleClient();
	esp_task_wdt_reset();

  // Determina a frequência do void loop
  unsigned long currentMillis = millis();
	if (currentMillis - previousReadMillis < read_interval) {
		return;
	}
	previousReadMillis = currentMillis;

  // ========================================
  // MODO STANDBY - BLOQUEIA TUDO
  // ========================================
  if (currentWorkStationState == STANDBY) {
      // Mantém display fixo
      static unsigned long lastStandbyDisplay = 0;
      if (currentMillis - lastStandbyDisplay > 5000) {
          drawCenteredText("STANDBY MODE\n\nPress * to\nresume", 1);
          lastStandbyDisplay = currentMillis;
      }
      
      // Só aceita tecla * para sair
      char key = keypad.getKey();
      if (key == '*') {
          currentWorkStationState = RUNNING;
          
          // Acende LED azul e apaga LED vermelho
          digitalWrite(DEBUG_LED, HIGH);
          digitalWrite(INTERVENTION_LED, LOW);
          
          Serial.println("Returning to Normal Mode");
          drawCenteredText("Returning to\nNormal Mode", 1);
          previousMessageMillis = currentMillis;
      }
      
      // Não faz mais nada
      return;
  }


  // Verifica se o Wifi ainda está funcionando, caso não, ele tenta reconectar a cada 10 segundos
  if (WiFi.status() != WL_CONNECTED) {

      static unsigned long lastWifiRetryMillis = 0;

      if (currentMillis - previousMessageMillis > message_interval){
        drawCenteredText(("AP:" + String(apSSID) + "\n --- \n Wifi disconnected!").c_str(), 1);
      }
        

      if (currentMillis - lastWifiRetryMillis >= 10000) { // tenta a cada 10s
          lastWifiRetryMillis = currentMillis;
          previousMessageMillis = currentMillis;

          Serial.println("Trying to reconnect WiFi...");
          drawCenteredText(("Trying to reconnect \n WiFi..."), 1);
          WiFi.disconnect();
          WiFi.begin(SSID_NAME.c_str(), PASSWORD.c_str());
      }

      return;
  }


  // Verifica conexão com o servidor a cada 10 segundos
  if (currentMillis - lastServerCheckMillis >= serverCheckInterval) {
      lastServerCheckMillis = currentMillis;
      serverConnected = checkServerHealth();
      Serial.println(serverConnected ? "Servidor OK" : "Servidor inacessível!");
  }

  if (!serverConnected) {
      if (currentMillis - previousMessageMillis > message_interval) {
          drawCenteredText(("AP:" + String(apSSID) + "\n ---\nServer offline!").c_str(), 1);
          previousMessageMillis = currentMillis;
      }
      return;
  }

  // MARK: PAREI AQUI
  // Caso fique mais de 30 segundos no modo input ele sai e zera o parâmetro que estava sendo digitado
  if (currentMillis - previousInputMillis > inputModeInterval){

    inputMode = false;

    // A
    if (nonConfirmingPartsInputMode) {
      nonConfirmingPartsInputMode = false;
      nonConfirmingPartsQuant = "";
    }

    // B
    if (deletePartsInputMode) {
      deletePartsInputMode = false;
      deletePartsQuant = "";
    } 

    // C
    if (operatorInputMode) {
      operatorInputMode = false;
      USER_CODE = "";
      saveConfig("USER_CODE", USER_CODE, SETUP);
    }

    // D
    if (partNumberInputMode || partsPerCycleInputMode || workOrderInputMode ){
      partNumberInputMode = false;
      partsPerCycleInputMode = false;
      workOrderInputMode = false;

      PART_NUMBER = "";
      PARTS_PER_CYCLE = ""; 
      WORK_ORDER = "";

      saveConfig("PART_NUMBER", PART_NUMBER, SETUP);
      saveConfig("PARTS_PER_CYCLE", PARTS_PER_CYCLE, SETUP);
      saveConfig("WORK_ORDER", WORK_ORDER, SETUP);
      
      saveConfig("GOOD_PARTS_PRODUCED", "0", PRODUCTION);
      saveConfig("BADS_PARTS_PRODUCED", "0", PRODUCTION);
    }

    // #
    if (interventionInputMode){
      interventionInputMode = false;
      interventionCode = "";
    }
  }

  // MARK: SENSORES
  int sensorState1 = digitalRead(SENSOR_PIN_1);
  int sensorState2 = digitalRead(SENSOR_PIN_2);
  char key = keypad.getKey();

  if (currentWorkStationState == RUNNING) {

    // Após o intervalo de exibição de uma mensagem de interação é exibida a mensagem principal
    if (currentMillis - previousMessageMillis > message_interval && !inputMode && starKeyPressedMillis == 0) {
        drawMainView();
    }

    if (SENSOR_READ_TYPE == "normal_signal"){
        // SENSOR 1 → peça conforme normal
        if (sensorState1 == LOW && currentMillis - previousPartMillis >= SENSOR_INTERVAL.toInt()) {

            Serial.println("Peça Conforme, modo simples!");

            StaticJsonDocument<1024> payload;
            payload["method_key"] = "insert_data";
            payload["status"] = "1";
            payload["production_note_type"] = "1";

            ApiResult result = sendApiRequest(payload, "api/");
            handleSimpleApiResult(result, false, true);

            previousPartMillis = currentMillis;
        }
    }

    else if (SENSOR_READ_TYPE == "continuos_signal"){
        if (sensorState1 == LOW && lastSignalState == 0 && currentMillis - previousPartMillis >= SENSOR_INTERVAL.toInt()){
          Serial.println("Nova Peça");
          
          GOOD_PARTS_PRODUCED = String(GOOD_PARTS_PRODUCED.toInt() + PARTS_PER_CYCLE.toInt());
          saveConfig("GOOD_PARTS_PRODUCED", GOOD_PARTS_PRODUCED, PRODUCTION);

          StaticJsonDocument<1024> payload;
          ApiResult result;

          // Deletar peças do PN selecionado
          payload["situacao"] = "1";
          payload["qtde"] = PARTS_PER_CYCLE;

          sendApiRequest(payload, "insert_data");
          handleSimpleApiResult(result, false, true);

          lastSignalState = 1;
        }

        else if (sensorState1 == HIGH && lastSignalState == 1 && currentMillis - previousPartMillis >= SENSOR_INTERVAL.toInt()){
          Serial.println("Pronto para ler a próxima peça");
          lastSignalState = 0;
        }
    }

    //

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////// MARK: INPUT MODES 
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    if (inputMode){

      
      //////////////////////////////////
      //////// MARK: INPUT A 
      //////////////////////////////////
      if (nonConfirmingPartsInputMode) {
        
        drawCenteredText(("Type quantity \n and press A: \n --- \n" + nonConfirmingPartsQuant).c_str(), 1);
        
        if (isdigit(key)){
          nonConfirmingPartsQuant += key;
        }
        else if (key == 'A'){
          inputMode = false;
          nonConfirmingPartsInputMode = false;

          GOOD_PARTS_PRODUCED = String(GOOD_PARTS_PRODUCED.toInt() - nonConfirmingPartsQuant.toInt());
          BADS_PARTS_PRODUCED = String(BADS_PARTS_PRODUCED.toInt() + nonConfirmingPartsQuant.toInt());

          saveConfig("GOOD_PARTS_PRODUCED", GOOD_PARTS_PRODUCED, PRODUCTION); // usa a variável já atualizada
          saveConfig("BADS_PARTS_PRODUCED", BADS_PARTS_PRODUCED, PRODUCTION); // usa a variável já atualizada

          StaticJsonDocument<1024> payload;
          ApiResult result;

          // Deletar peças do PN selecionado
          payload["situacao"] = "0";
          payload["qtde"] = nonConfirmingPartsQuant;
          
          result = sendApiRequest(payload, "insert_data");
          handleSimpleApiResult(result, true);

          previousPartMillis = currentMillis;
          nonConfirmingPartsQuant = "";
        }
      }

      //////////////////////////////////
      //////// MARK: INPUT B 
      //////////////////////////////////
      
      else if (deletePartsInputMode) {
        
        drawCenteredText(("Type quantity \n and press B: \n --- \n" + deletePartsQuant).c_str(), 1);
        
        if (isdigit(key)){
          deletePartsQuant += key;
        }
        else if (key == 'B'){
          inputMode = false;
          deletePartsInputMode = false;

          StaticJsonDocument<1024> payload;
          ApiResult result;

          // Deletar peças do PN selecionado
          payload["situacao"] = "-1";
          payload["qtde"] = deletePartsQuant;
          
          result = sendApiRequest(payload, "insert_data");
          handleSimpleApiResult(result, true);

          previousPartMillis = currentMillis;
          deletePartsQuant = "";
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
          saveConfig("USER_CODE", USER_CODE, SETUP);
        }
      }

      //////////////////////////////////
      //////// MARK: INPUT D
      /////////////////////////////////

      // Passo 1: Digitar código do partnumber
      if (partNumberInputMode){
        drawCenteredText(("Type partnumber code \n and press D: \n --- \n" + PART_NUMBER).c_str(), 1);
        
        if (isdigit(key)){
          PART_NUMBER += key;
        }
        else if (key == 'D') {
          partNumberInputMode = false;
          partsPerCycleInputMode = true;
          previousDebounceMillis = currentMillis;
        }
      }

      // Passo 2: Digitar quantidade de peças por ciclo
      else if (partsPerCycleInputMode){
        drawCenteredText(("Type parts per cycle \n and press D: \n --- \n" + PARTS_PER_CYCLE).c_str(), 1);
     
        if (isdigit(key)){
          PARTS_PER_CYCLE += key;
        }
        else if (key == 'D'){
          partsPerCycleInputMode = false;
          workOrderInputMode = true;
          previousDebounceMillis = currentMillis;
        }
      }

      // Passo 3: Digitar código da ordem de produção
      else if (workOrderInputMode) {

        drawCenteredText(("Type work order code \n and press D \n --- \n" + WORK_ORDER).c_str(), 1);
                    
        if (isdigit(key)) {
          WORK_ORDER += key;
        }

        else if (key == 'D'){
          workOrderInputMode = false;
          inputMode = false;

          saveConfig("PART_NUMBER", PART_NUMBER, SETUP);
          saveConfig("PARTS_PER_CYCLE", PARTS_PER_CYCLE, SETUP);
          saveConfig("WORK_ORDER", WORK_ORDER, SETUP);
        }
      }

      //////////////////////////////////
      //////// MARK: INPUT #
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

          // Verifica mudança de usuário
          payload["method_key"] = "start_intervention";
          payload["identification_number"] = interventionCode;
          
          result = sendApiRequest(payload, "api/");

          if (handleSimpleApiResult(result, true)) {
            currentWorkStationState = INTERVENTION;

            // Apaga LED azul e acende LED vermelho
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
    ///////////////////////////////////// MARK: KEYPAD
    /////////////////////////////////////////////////////////////////////////////////////////////

    // Tecla A - Peças não conformes
    if (key == 'A' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      inputMode = true;
      nonConfirmingPartsInputMode = true;  // Ativa seleção de PN primeiro
      nonConfirmingPartsQuantInputMode = false;
      nonConfirmingPartsTypeCode = "";
      nonConfirmingPartsQuant = "";

      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }

    // MARK: KEYPAD B
    // Deleta peças inseridas por engano, seguindo a mesma lógica de input da tecla A
    else if (key == 'B' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      inputMode = true;
      deletePartsInputMode = true; 
      deletePartsQuant = "";

      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }
    
    // MARK: KEYPAD C
    // Muudança de Operador
    else if (key == 'C' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      inputMode = true;
      operatorInputMode = true;
      USER_CODE = "";
      
      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }

    // MARK: KEYPAD D
    // Setup da peça, quantidade de peças e ordem de produção
    else if (key == 'D' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      inputMode = true;
      partNumberInputMode = true;

      PART_NUMBER = "";
      PARTS_PER_CYCLE = "";
      WORK_ORDER = "";

      GOOD_PARTS_PRODUCED = "0";
      BADS_PARTS_PRODUCED = "0";
    
      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }
    
    // MARK: KEYPAD #
    // Abertura de Intervenção (Não está implementado na API)
    else if (key == '#' && currentMillis - previousDebounceMillis >= debounce_keypad) {
      inputMode = true;
      interventionInputMode = true;

      interventionCode = "";
      
      previousInputMillis = currentMillis;
      previousDebounceMillis = currentMillis;
    }


    // MARK: KEYPAD *
    // Entrar em modo STANDBY ou reiniciar o ESP32 ao segurar por mais de 3 segundos
    else if (key == '*' && currentMillis - previousDebounceMillis >= debounce_keypad) {
        starKeyPressedMillis = currentMillis; // Anota o momento exato que o * foi pressionado
        previousDebounceMillis = currentMillis; // Reseta o debounce
    }

    // =====================================================
    // MONITORAMENTO DO BOTÃO * ENQUANTO ESTIVER SEGURADO
    // Esse bloco roda a cada ciclo do loop enquanto o *
    // tiver sido pressionado e ainda não foi resolvido
    // (starKeyPressedMillis > 0 significa "aguardando resolução")
    // =====================================================
    if (starKeyPressedMillis > 0) {

        // Verifica se o * ainda está fisicamente pressionado neste exato momento
        bool starHeld = false;
        keypad.getKeys(); // Atualiza o estado interno de todas as teclas
        for (int i = 0; i < LIST_MAX; i++) {
            if (keypad.key[i].kchar == '*' &&
                (keypad.key[i].kstate == PRESSED || keypad.key[i].kstate == HOLD)) {
                starHeld = true;
                break;
            }
        }

        if (!starHeld) {
            // -----------------------------------------------
            // BOTÃO FOI SOLTO antes de completar 3 segundos
            // → comportamento normal: vai para STANDBY
            // -----------------------------------------------
            if (currentMillis - starKeyPressedMillis < starKeyResetInterval) {
                currentWorkStationState = STANDBY;
                digitalWrite(DEBUG_LED, false);
                digitalWrite(INTERVENTION_LED, false);
                Serial.println("Standby Mode");
                drawCenteredText("Standby Mode", 1);
            }
            starKeyPressedMillis = 0; // Zera para parar de monitorar
        }
        else if (currentMillis - starKeyPressedMillis >= starKeyResetInterval) {
            // -----------------------------------------------
            // BOTÃO AINDA SEGURADO e já passou 3 segundos
            // → reinicia o ESP32
            // -----------------------------------------------
            Serial.println("Reiniciando ESP32...");
            drawCenteredText("Restarting...", 1);
            delay(1000);
            ESP.restart();
        }
        else {
            // -----------------------------------------------
            // BOTÃO AINDA SEGURADO mas ainda não chegou em 3s
            // → mostra contagem regressiva no display
            // -----------------------------------------------
            int segundosRestantes = (starKeyResetInterval / 1000) - ((currentMillis - starKeyPressedMillis) / 1000);
            drawCenteredText(("Hold to restart\n" + String(segundosRestantes) + "s...").c_str(), 1);
        }
    }
  }
}