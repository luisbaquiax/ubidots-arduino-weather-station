#include <SoftwareSerial.h>
#include <avr/pgmspace.h>
#include <DHTStable.h>

// --- Versión del programa ---
#define VERSION "v17.4.0"  // Versión optimizada para PMS5003 y múltiples SoftwareSerial

// --- Configuración WiFi y Ubidots (en PROGMEM) ---
const char WIFI_SSID[] PROGMEM = "HUAWEI Y9 Prime 2019";
const char WIFI_PASS[] PROGMEM = "rudyreyes";
const char UBIDOTS_TOKEN[] PROGMEM = "BBUS-1AgE3imgxz5DDJIRwt8HKBw3W7e2nE";
const char DEVICE_LABEL[] PROGMEM = "arduino-caja-meteorologica";
const char UBIDOTS_SERVER[] PROGMEM = "industrial.api.ubidots.com";

// --- Pines ---
#define WIFI_RX_PIN 2     // Mantener pins 2,3 para WiFi por prioridad
#define WIFI_TX_PIN 3
#define DHTPIN 4
#define PMS_RX_PIN 10    
#define PMS_TX_PIN 11    
#define MQ7_PIN A0
#define MQ135_PIN A1
#define MQ131_PIN A2

// --- Objetos ---
SoftwareSerial wifiSerial(WIFI_RX_PIN, WIFI_TX_PIN);
SoftwareSerial pmsSerial(PMS_RX_PIN, PMS_TX_PIN);
DHTStable dht;

// --- Tiempos ---
#define SENSOR_INTERVAL 30000  // 30 segundos entre envíos de datos
#define WIFI_CHECK_INTERVAL 15000  // 15 segundos para comprobar WiFi
#define DHT_READ_INTERVAL 3000  // 3 segundos para lecturas DHT
unsigned long prevSensorMillis, prevWifiMillis, prevDhtMillis;

// --- Estado ---
bool isWifiConnected = false;
bool dhtReadSuccess = false;
bool lastPMSReadSuccess = false;

// --- Variables de sensores ---
float temperatura, humedad;
float ppm_nh3, ppm_nox, ppm_co2;
int adc_mq7, adc_mq135, adc_mq131;

// --- Variables de PMS5003 ---
struct PMSData {
  uint16_t pm1_0;
  uint16_t pm2_5;
  uint16_t pm10_0;
  bool valid = false;
};
PMSData pmsData;

// --- Buffer compartidos ---
char buffer[400];  // Buffer principal compartido

// --- Prototipos ---
bool attemptAutoConnect();
bool checkWifiConnection();
bool sendCommand(const char* cmd, unsigned long timeout, const char* expectedResponse, bool printOutput = false);
void sendDataToUbidots();
bool readDhtWithRetry();
bool readPMSDataToGlobal();
void calculateMQ135Gases(int adc_value);
void getStringFromProgmem(const char* progmemString, char* ramBuffer);

void setup() {
  Serial.begin(9600);
  
  // Inicializar comunicación con WiFi
  wifiSerial.begin(9600);
  
  Serial.print(F("\n--- ARDUINO ESTACIÓN METEO ("));
  Serial.print(F(VERSION));
  Serial.println(F(" - Con Sensor PMS5003 Optimizado) ---"));
  
  // Esperar un momento para que el módulo WiFi se inicie
  delay(1000);
  
  // Verificar comunicación con ESP8266
  byte attempts = 0;
  bool espResponding = false;
  
  while (attempts < 3 && !espResponding) {
    Serial.print(F("Comprobando ESP8266 (intento "));
    Serial.print(attempts + 1);
    Serial.println(F(")..."));
    
    // Limpiar buffer y datos pendientes
    while (wifiSerial.available()) wifiSerial.read();
    
    // Enviar comando AT y esperar respuesta
    wifiSerial.println("AT");
    delay(500);
    
    // Leer respuesta
    byte idx = 0;
    memset(buffer, 0, sizeof(buffer));
    unsigned long startCheck = millis();
    
    while (millis() - startCheck < 1000) {
      if (wifiSerial.available()) {
        char c = wifiSerial.read();
        if (idx < sizeof(buffer) - 1) buffer[idx++] = c;
        buffer[idx] = '\0';
      }
    }
    
    if (strstr(buffer, "OK") != NULL) {
      espResponding = true;
      Serial.println(F("ESP8266 respondiendo OK"));
    } else {
      Serial.print(F("No respuesta. Recibido: "));
      Serial.println(buffer);
      attempts++;
      delay(1000);
    }
  }
  
  // Intentar conectar WiFi solo si el ESP responde
  if (espResponding) {
    isWifiConnected = attemptAutoConnect();
    Serial.println(isWifiConnected ? F("WiFi conectado") : F("WiFi falló"));
  } else {
    Serial.println(F("ERROR: ESP8266 no responde. Verifique conexiones."));
    isWifiConnected = false;
  }
  
  // Lectura inicial del sensor DHT
  readDhtWithRetry();
  
  // Inicializar datos PMS5003 como no válidos hasta primera lectura exitosa
  pmsData.valid = false;
  
  // Inicializar temporizadores para evitar lecturas inmediatas
  prevSensorMillis = millis();
  prevWifiMillis = millis();
  prevDhtMillis = millis();
}

void loop() {
  unsigned long currentMillis = millis();
  
  // --- Passthrough Serial <-> WiFi para facilitar debugging (sólo cuando no está leyendo PMS5003) ---
  if (wifiSerial.available()) Serial.write(wifiSerial.read());
  if (Serial.available()) {
    byte bytesRead = Serial.readBytesUntil('\n', buffer, 127);
    if (bytesRead > 0) {
      if (buffer[bytesRead-1] == '\r') bytesRead--;
      buffer[bytesRead] = '\0';
      wifiSerial.println(buffer);
    }
  }
  
  // --- Lectura DHT ---
  if (currentMillis - prevDhtMillis >= DHT_READ_INTERVAL) {
    prevDhtMillis = currentMillis;
    dhtReadSuccess = readDhtWithRetry();
  }
  
  // --- Chequeo WiFi ---
  if (currentMillis - prevWifiMillis >= WIFI_CHECK_INTERVAL) {
    prevWifiMillis = currentMillis;
    
    bool prevWifiState = isWifiConnected;
    isWifiConnected = checkWifiConnection();
    
    // Si el estado cambió, mostrarlo
    if (prevWifiState != isWifiConnected) {
      Serial.print(F("Estado WiFi: "));
      Serial.println(isWifiConnected ? F("CONECTADO") : F("DESCONECTADO"));
      
      // Si perdimos conexión, intentar reconectar
      if (!isWifiConnected) {
        Serial.println(F("Intentando reconexión..."));
        isWifiConnected = attemptAutoConnect();
      }
    }
  }
  
  // --- Lectura sensores y envío ---
  if (currentMillis - prevSensorMillis >= SENSOR_INTERVAL) {
    prevSensorMillis = currentMillis;
    
    Serial.println(F("\n--- Ciclo de Lectura y Envío ---"));
    
    // Leer sensores analógicos
    adc_mq7 = analogRead(MQ7_PIN);
    adc_mq135 = analogRead(MQ135_PIN);
    adc_mq131 = analogRead(MQ131_PIN);
    calculateMQ135Gases(adc_mq135);
    
    // Intento final de lectura DHT si falló antes
    if (!dhtReadSuccess) {
      Serial.println(F("Reintentando lectura DHT..."));
      dhtReadSuccess = readDhtWithRetry();
    }
    
    // Log de sensores
    Serial.print(F("T:")); Serial.print(temperatura, 1);
    Serial.print(F("°C H:")); Serial.print(humedad, 1);
    Serial.print(F("%"));
    Serial.print(F(" MQ7:")); Serial.print(adc_mq7);
    Serial.print(F(" MQ135:")); Serial.print(adc_mq135);
    Serial.print(F(" MQ131:")); Serial.println(adc_mq131);
    
    // Mostrar gases calculados
    Serial.print(F("NH3:")); Serial.print(ppm_nh3, 1);
    Serial.print(F(" NOx:")); Serial.print(ppm_nox, 1);
    Serial.print(F(" CO2:")); Serial.print(ppm_co2, 1);
    Serial.println(F(" ppm"));
    
    // IMPORTANTE: Leer PMS5003 ANTES de intentar cualquier comunicación WiFi
    Serial.println(F("Leyendo datos del sensor PMS5003..."));
    
    // Realizar lectura PMS5003
    lastPMSReadSuccess = readPMSDataToGlobal();
    
    // Mostrar datos PMS5003
    if (lastPMSReadSuccess) {
      Serial.print(F("PM1.0:")); Serial.print(pmsData.pm1_0);
      Serial.print(F(" PM2.5:")); Serial.print(pmsData.pm2_5);
      Serial.print(F(" PM10:")); Serial.print(pmsData.pm10_0);
      Serial.println(F(" μg/m³"));
    } else {
      Serial.println(F("ERROR: No se pudo leer datos de PMS5003"));
    }
    
    // Enviar datos si WiFi conectado
    if (isWifiConnected) {
      Serial.println(F("Enviando datos a Ubidots..."));
      sendDataToUbidots();
    } else {
      Serial.println(F("WiFi desconectado - no se pueden enviar datos"));
      
      // Intento de reconexión si falló WiFi
      Serial.println(F("Intentando reconexión WiFi..."));
      isWifiConnected = attemptAutoConnect();
    }
  }
}

bool readPMSDataToGlobal() {
  // Antes de iniciar la comunicación, deshabilitamos la escucha de ESP8266
  wifiSerial.end();
  delay(10);
  
  // Iniciar comunicación con PMS5003
  pmsSerial.begin(9600);
  delay(100); // Breve pausa para estabilizar

  // Realizar lectura
  const unsigned long timeout = 3000; // 3 segundos
  unsigned long startTime = millis();
  uint8_t buffer[32];
  int index = 0;
  bool success = false;
  
  // Vaciar buffer serial
  while (pmsSerial.available()) pmsSerial.read();
  
  Serial.println(F("Esperando datos del PMS5003..."));
  
  while (millis() - startTime < timeout && !success) {
    while (pmsSerial.available()) {
      uint8_t c = pmsSerial.read();
      
      // Buscar cabecera del paquete: 0x42 0x4D
      if (index == 0 && c != 0x42) continue;
      if (index == 1 && c != 0x4D) {
        index = 0;
        continue;
      }
      
      if (index < 32) buffer[index++] = c;
      
      // Cuando se recibe paquete completo
      if (index == 32) {
        // Calcular checksum
        uint16_t checksum = 0;
        for (uint8_t i = 0; i < 30; i++) {
          checksum += buffer[i];
        }
        
        // Obtener checksum recibido
        uint16_t receivedChecksum = (buffer[30] << 8) | buffer[31];
        
        if (checksum == receivedChecksum) {
          pmsData.pm1_0 = (buffer[4] << 8) | buffer[5];
          pmsData.pm2_5 = (buffer[6] << 8) | buffer[7];
          pmsData.pm10_0 = (buffer[8] << 8) | buffer[9];
          pmsData.valid = true;
          success = true;
          Serial.println(F("Datos PMS5003 recibidos correctamente"));
        } else {
          Serial.println(F("Error de checksum PMS5003!"));
        }
        break;
      }
    }
    
    // Pequeña pausa para no saturar el CPU
    delay(10);
  }
  
  // Terminamos la comunicación con PMS5003
  pmsSerial.end();
  delay(10);
  
  // Restaurar la comunicación con WiFi
  wifiSerial.begin(9600);
  delay(50); // Dar tiempo para que SoftwareSerial se estabilice
  
  if (!success) {
    Serial.println(F("Timeout: PMS5003 no envió datos completos"));
  }
  
  return success;
}

bool readDhtWithRetry() {
  for (byte i = 0; i < 3; i++) {
    if (dht.read11(DHTPIN) == 0) {
      humedad = dht.getHumidity();
      temperatura = dht.getTemperature();
      if (temperatura > -40 && temperatura < 80 && humedad >= 0 && humedad <= 100) {
        return true;
      }
    }
    delay(500);
  }
  return false;
}

void calculateMQ135Gases(int adc_value) {
  float voltaje = adc_value * (5.0 / 1023.0);
  ppm_nh3 = constrain(voltaje * 10.0, 0, 200);
  ppm_nox = constrain(voltaje * 15.0, 0, 300);
  ppm_co2 = constrain(350.0 + (voltaje * 1000.0), 350, 10000);
}

void getStringFromProgmem(const char* progmemString, char* ramBuffer) {
  strcpy_P(ramBuffer, progmemString);
}

bool checkWifiConnection() {
  return sendCommand("AT+CWJAP?", 2000, "+CWJAP:", false);
}

bool attemptAutoConnect() {
  char ssid[30], pass[30];
  
  // Reinicio de módulo para garantizar estado limpio
  Serial.println(F("Reiniciando módulo WiFi..."));
  if (!sendCommand("AT+RST", 3000, "ready", true)) {
    Serial.println(F("Módulo no respondió al reset, continuando de todos modos"));
  }
  delay(1500); // Esperar a que termine el reinicio
  
  // Verificar que el módulo responda a comandos básicos
  Serial.println(F("Verificando comunicación con módulo..."));
  if (!sendCommand("AT", 2000, "OK", true)) {
    Serial.println(F("ERROR: Módulo no responde a AT después del reset"));
    return false;
  }
  
  // Establecer modo de operación
  Serial.println(F("Configurando modo estación..."));
  if (!sendCommand("AT+CWMODE=1", 2000, "OK", true)) {
    Serial.println(F("Advertencia: No se pudo configurar el modo, continuando"));
  }
  
  // Desconectar de cualquier AP previo
  Serial.println(F("Desconectando de AP previos..."));
  sendCommand("AT+CWQAP", 1000, "OK", false);
  
  // Obtener credenciales de WiFi
  getStringFromProgmem(WIFI_SSID, ssid);
  getStringFromProgmem(WIFI_PASS, pass);
  
  // Intentar conexión
  Serial.print(F("Conectando a red: "));
  Serial.println(ssid);
  
  sprintf(buffer, "AT+CWJAP=\"%s\",\"%s\"", ssid, pass);
  bool connected = sendCommand(buffer, 20000, "WIFI GOT IP", true);
  
  if (connected) {
    Serial.println(F("Conexión WiFi exitosa"));
  } else {
    Serial.println(F("Falló la conexión WiFi"));
  }
  
  return connected;
}

void sendDataToUbidots() {
  char serverStr[40], tokenStr[40], labelStr[40];
  char tempBuffer[300]; // Buffer temporal para construir el payload JSON
  
  // Obtener strings desde PROGMEM
  getStringFromProgmem(UBIDOTS_SERVER, serverStr);
  getStringFromProgmem(DEVICE_LABEL, labelStr);
  getStringFromProgmem(UBIDOTS_TOKEN, tokenStr);
  
  // Verificar estado de conexión WiFi primero
  Serial.println(F("Verificando estado de conexión WiFi..."));
  if (!checkWifiConnection()) {
    Serial.println(F("Error: WiFi no conectado, abortando envío"));
    isWifiConnected = false; // Actualizar estado global
    return;
  }
  
  // Preparar datos de sensores como strings con formato adecuado
  char tempStr[8], humStr[8], coStr[8], o3Str[8];
  char nh3Str[8], noxStr[8], co2Str[8];
  char pm1Str[8], pm25Str[8], pm10Str[8]; // Strings para PMS5003
  
  // Convertir valores a strings con decimales fijos
  dtostrf(temperatura, 4, 1, tempStr);
  dtostrf(humedad, 4, 1, humStr);
  
  // Calcular valores para sensores MQ7 y MQ131
  float ppm_co = adc_mq7 * (5.0 / 1023.0) * 20.0;
  float ppm_o3 = adc_mq131 * (5.0 / 1023.0) * 10.0;
  
  dtostrf(ppm_co, 4, 1, coStr);
  dtostrf(ppm_o3, 4, 1, o3Str);
  dtostrf(ppm_nh3, 4, 1, nh3Str);
  dtostrf(ppm_nox, 4, 1, noxStr);
  dtostrf(ppm_co2, 5, 1, co2Str);
  
  // Convertir valores del PMS5003
  if (pmsData.valid) {
    itoa(pmsData.pm1_0, pm1Str, 10);
    itoa(pmsData.pm2_5, pm25Str, 10);
    itoa(pmsData.pm10_0, pm10Str, 10);
  } else {
    strcpy(pm1Str, "0");
    strcpy(pm25Str, "0");
    strcpy(pm10Str, "0");
  }
  
  // Paso 1: Verificar que el módulo ESP8266 responda
  Serial.println(F("Verificando respuesta del ESP8266..."));
  if (!sendCommand("AT", 1000, "OK", true)) {
    Serial.println(F("Error: ESP8266 no responde, abortando envío"));
    return;
  }
  
  // Paso 2: Cerrar cualquier conexión previa que pudiera estar abierta
  Serial.println(F("Cerrando conexiones previas..."));
  sendCommand("AT+CIPCLOSE", 1000, NULL, false);
  
  // Paso 3: Conectar al servidor
  Serial.print(F("Conectando a "));
  Serial.print(serverStr);
  Serial.println(F("..."));
  
  // Limpiar buffers para evitar contaminación de mensajes anteriores
  while (wifiSerial.available()) wifiSerial.read();
  
  // Construir comando de conexión explícitamente y mostrarlo
  memset(buffer, 0, sizeof(buffer));
  sprintf(buffer, "AT+CIPSTART=\"TCP\",\"%s\",80", serverStr);
  Serial.print(F("Enviando: "));
  Serial.println(buffer);
  
  // Intentar conexión hasta 3 veces
  bool connected = false;
  for (byte attempt = 0; attempt < 3 && !connected; attempt++) {
    Serial.print(F("Intento "));
    Serial.print(attempt + 1);
    Serial.print(F(" de 3... "));
    
    // Enviar el comando directamente, sin usar sendCommand todavía
    wifiSerial.println(buffer);
    
    // Esperar respuesta
    unsigned long startTime = millis();
    memset(buffer, 0, sizeof(buffer));
    byte idx = 0;
    bool errorFound = false;
    bool connectFound = false;
    
    while (millis() - startTime < 10000 && !connectFound && !errorFound) {
      if (wifiSerial.available()) {
        char c = wifiSerial.read();
        
        if (idx < sizeof(buffer) - 1) {
          buffer[idx++] = c;
          buffer[idx] = '\0';
        }
        
        // Buscar respuestas
        if (strstr(buffer, "CONNECT") != NULL) {
          connectFound = true;
        }
        if (strstr(buffer, "ERROR") != NULL) {
          errorFound = true;
        }
      }
    }
    
    // Verificar resultado
    if (connectFound) {
      connected = true;
      Serial.println(F("\nConexión establecida!"));
    } else {
      Serial.println(F("\nFalló la conexión, esperando antes de reintentar..."));
      delay(2000);
    }
  }
  
  if (!connected) {
    Serial.println(F("Error: No se pudo conectar al servidor después de múltiples intentos"));
    return;
  }
  
  // Paso 4: Construir payload JSON en buffer temporal
  memset(tempBuffer, 0, sizeof(tempBuffer));
  sprintf(tempBuffer, "{\"temperatura\":%s,\"humedad\":%s,\"co\":%s,\"o3\":%s,\"mq135_adc\":%d,\"nh3\":%s,\"nox\":%s,\"co2\":%s,\"pm1\":%s,\"pm25\":%s,\"pm10\":%s}",
          tempStr, humStr, coStr, o3Str, adc_mq135, nh3Str, noxStr, co2Str, pm1Str, pm25Str, pm10Str);
  
  int payloadLen = strlen(tempBuffer);
  Serial.print(F("Payload JSON ("));
  Serial.print(payloadLen);
  Serial.println(F(" bytes)"));
  
  // Paso 5: Preparar encabezado de solicitud HTTP
  memset(buffer, 0, sizeof(buffer));
  
  strcpy(buffer, "POST /api/v1.6/devices/");
  strcat(buffer, labelStr);
  strcat(buffer, " HTTP/1.1\r\nHost: ");
  strcat(buffer, serverStr);
  strcat(buffer, "\r\nContent-Type: application/json\r\nX-Auth-Token: ");
  strcat(buffer, tokenStr);
  strcat(buffer, "\r\nContent-Length: ");
  
  char lenStr[6];
  itoa(payloadLen, lenStr, 10);
  strcat(buffer, lenStr);
  strcat(buffer, "\r\n\r\n");
  
  int headerLen = strlen(buffer);
  int totalLen = headerLen + payloadLen;
  
  Serial.print(F("Longitud total de solicitud: "));
  Serial.print(totalLen);
  Serial.println(F(" bytes"));
  
  // Paso 6: Enviar comando CIPSEND
  Serial.println(F("Enviando comando CIPSEND..."));
  
  // Limpiar buffer antes de enviar el comando
  while (wifiSerial.available()) wifiSerial.read();
  
  memset(buffer, 0, sizeof(buffer));
  sprintf(buffer, "AT+CIPSEND=%d", totalLen);
  Serial.print(F("Comando: "));
  Serial.println(buffer);
  
  // Enviar comando directamente
  wifiSerial.println(buffer);
  
  // Esperar el prompt ">"
  unsigned long startTime = millis();
  bool gotPrompt = false;
  memset(buffer, 0, sizeof(buffer));
  byte idx = 0;
  
  while (millis() - startTime < 5000 && !gotPrompt) {
    if (wifiSerial.available()) {
      char c = wifiSerial.read();
      
      if (idx < sizeof(buffer) - 1) {
        buffer[idx++] = c;
        buffer[idx] = '\0';
      }
      
      if (c == '>') {
        gotPrompt = true;
      }
    }
  }
  
  if (!gotPrompt) {
    Serial.println(F("\nError: No se recibió prompt '>' para envío de datos"));
    sendCommand("AT+CIPCLOSE", 1000, NULL, false);
    return;
  }
  
  // Paso 7: Enviar el request HTTP
  Serial.println(F("\nEnviando datos HTTP..."));
  
  // Primero el encabezado HTTP
  memset(buffer, 0, sizeof(buffer));
  strcpy(buffer, "POST /api/v1.6/devices/");
  strcat(buffer, labelStr);
  strcat(buffer, " HTTP/1.1\r\nHost: ");
  strcat(buffer, serverStr);
  strcat(buffer, "\r\nContent-Type: application/json\r\nX-Auth-Token: ");
  strcat(buffer, tokenStr);
  strcat(buffer, "\r\nContent-Length: ");
  strcat(buffer, lenStr);
  strcat(buffer, "\r\n\r\n");
  
  wifiSerial.print(buffer);
  wifiSerial.print(tempBuffer); // Payload JSON
  
  // Paso 8: Esperar y procesar la respuesta
  Serial.println(F("Esperando respuesta..."));
  startTime = millis();
  bool success = false;
  bool closed = false;
  
  memset(buffer, 0, sizeof(buffer));
  idx = 0;
  
  while (millis() - startTime < 15000 && !closed) { // 15 segundos de timeout
    if (wifiSerial.available()) {
      char c = wifiSerial.read();
      
      if (idx < sizeof(buffer) - 1) {
        buffer[idx++] = c;
        buffer[idx] = '\0';
      }
      
      // Verificar respuesta exitosa
      if (strstr(buffer, "HTTP/1.1 20") != NULL) {
        success = true;
      }
      
      // Verificar conexión cerrada
      if (strstr(buffer, "CLOSED") != NULL) {
        closed = true;
      }
    }
    
   // Si el buffer está casi lleno, mantener solo la última parte
    if (idx > sizeof(buffer) * 0.8) {
      int halfSize = sizeof(buffer) / 2;
      memmove(buffer, buffer + halfSize, idx - halfSize);
      idx -= halfSize;
      buffer[idx] = '\0';
    }
  }
  
  // Reportar resultados
  if (success) {
    Serial.println(F("\n--- Datos enviados correctamente ---"));
  } else {
    Serial.println(F("\n--- Error al enviar datos ---"));
    if (idx > 0) {
      Serial.println(F("Respuesta recibida:"));
      Serial.println(buffer);
    } else {
      Serial.println(F("No se recibió respuesta (timeout)"));
    }
  }
  
  // Asegurar que la conexión esté cerrada
  if (!closed) {
    Serial.println(F("Cerrando conexión..."));
    sendCommand("AT+CIPCLOSE", 2000, NULL, false);
  }
}

bool sendCommand(const char* cmd, unsigned long timeout, const char* expectedResponse, bool printOutput) {
  // Limpiar buffer serial del ESP8266
  while (wifiSerial.available()) wifiSerial.read();
  
  // Limpiar buffer de respuesta
  memset(buffer, 0, sizeof(buffer));
  
  // Enviar comando y imprimir si se requiere debug
  if (printOutput) {
    Serial.print(F("CMD: "));
    Serial.println(cmd);
  }
  
  wifiSerial.println(cmd);
  unsigned long startTime = millis();
  byte responseIdx = 0;
  
  // Esperar respuesta hasta timeout
  while (millis() - startTime < timeout) {
    while (wifiSerial.available()) {
      char c = wifiSerial.read();
      if (responseIdx < 79) buffer[responseIdx++] = c;
      buffer[responseIdx] = '\0';
      
      // Imprimir caracteres recibidos en tiempo real si debug activado
      if (printOutput) Serial.write(c);
    }
    
    // Verificar respuesta esperada
    if (strstr(buffer, expectedResponse) != NULL) {
      if (printOutput) {
        Serial.print(F("Respuesta OK: "));
        Serial.println(expectedResponse);
      }
      return true;
    }
    
    // Verificar error
    if (strstr(buffer, "ERROR") != NULL) {
      if (printOutput) {
        Serial.println(F("ERROR detectado en respuesta"));
      }
      return false;
    }
  }
  
  // Timeout
  if (printOutput) {
    Serial.print(F("Timeout esperando: "));
    Serial.println(expectedResponse);
    Serial.print(F("Recibido: "));
    Serial.println(buffer);
  }
  
  return false;
}
