#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---- Wi-Fi y ThingSpeak ----
const char* ssid     = "";
const char* password = "";
const char* apiKey   = "";

// ---- Telegram ----
const char* BOT_TOKEN = "";
const char* CHAT_ID   = "";

// ---- Alerta de temperatura ----
const float TEMP_MAX = 30.0;                       // °C: umbral de alerta (ajústalo)
const float HISTERESIS = 2.0;                      // la alerta se rearma al bajar de TEMP_MAX - 2
const unsigned long REENVIO_ALERTA = 200000;       // 5 min entre avisos si sigue alta

// ---- Pines ----
#define PIN_V 32
#define PIN_I 34
#define PIN_T 25   // DATA del DS18B20

const float DIV_ACS = 20.0 / 30.0;       // divisor 10k/20k
const float K_VOLT  = 5.0;               // módulo 25 V (ajustar al calibrar)
const unsigned long INTERVALO = 18000;   // 18 s (rango válido 15-30 s)
const int MAX_FALLOS = 5;                // fallos de conexión seguidos antes de reiniciar

float offsetI;
unsigned long ultimoEnvio = 0;
int fallosSeguidos = 0;

bool alertaActiva = false;
unsigned long ultimoAviso = 0;

OneWire oneWire(PIN_T);
DallasTemperature sensorT(&oneWire);

float leerMv(int pin, int n = 200) {
  long s = 0;
  for (int k = 0; k < n; k++) { s += analogReadMilliVolts(pin); delayMicroseconds(500); }
  return s / (float)n;
}

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Conectando a Wi-Fi");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    Serial.print("\nConectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nNo se pudo conectar");
  }
}

// ---------- ThingSpeak (HTTP, puerto 80) ----------
// Devuelve: 1 = aceptado, 0 = rechazado por ThingSpeak, -1 = fallo de conexión
int intentarEnvio(const String& path) {
  WiFiClient client;
  client.setTimeout(10);
  if (!client.connect("api.thingspeak.com", 80)) {
    Serial.println("ThingSpeak: no se pudo abrir conexion al puerto 80");
    return -1;
  }

  client.print("GET " + path + " HTTP/1.0\r\n"
               "Host: api.thingspeak.com\r\n"
               "User-Agent: ESP32\r\n"
               "Accept: */*\r\n"
               "Connection: close\r\n\r\n");

  String resp = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 10000) {
    while (client.available()) resp += (char)client.read();
    if (!client.connected() && !client.available()) break;
    delay(10);
  }
  client.stop();

  if (resp.length() == 0) {
    Serial.println("ThingSpeak: sin respuesta del servidor");
    return -1;
  }

  int pos = resp.indexOf(" 200 ");
  bool http200 = (pos > 0 && pos < 15);

  String cuerpo = resp;
  cuerpo.trim();
  int ult = cuerpo.lastIndexOf('\n');
  if (ult >= 0) cuerpo = cuerpo.substring(ult + 1);
  cuerpo.trim();

  long entrada = cuerpo.toInt();   // 0 = ThingSpeak rechazó el dato
  if (!http200) {
    Serial.println("ThingSpeak: respuesta HTTP inesperada");
    return -1;
  }
  if (entrada > 0) {
    Serial.printf("ThingSpeak: HTTP 200 | entrada nº %ld (OK)\n", entrada);
    return 1;
  }
  Serial.println("ThingSpeak: HTTP 200 | entrada nº 0 (rechazado, se envia en el proximo ciclo)");
  return 0;
}

// Reintenta solo si falló la conexión; un rechazo (0) NO se reintenta
int enviarThingSpeak(float v, float i, float t, float p, bool tempValida) {
  String path = "/update?api_key=" + String(apiKey)
              + "&field1=" + String(v, 2)
              + "&field2=" + String(i, 2)
              + "&field4=" + String(p, 1);
  if (tempValida) path += "&field3=" + String(t, 1);

  int r = intentarEnvio(path);
  if (r == -1) {
    delay(16000);   // espera más de 15 s por si el primer envío sí se guardó
    r = intentarEnvio(path);
  }
  return r;
}

// ---------- Telegram (HTTPS, puerto 443) ----------
String urlEncode(const String& s) {
  String out = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t k = 0; k < s.length(); k++) {
    uint8_t c = (uint8_t)s[k];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

bool enviarTelegram(const String& mensaje) {
  WiFiClientSecure client;
  client.setInsecure();   // cifra, pero no valida el certificado
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN)
             + "/sendMessage?chat_id=" + String(CHAT_ID)
             + "&text=" + urlEncode(mensaje);
  if (!http.begin(client, url)) {
    Serial.println("Telegram: no se pudo iniciar la conexion");
    return false;
  }
  http.setTimeout(10000);
  int code = http.GET();
  http.end();
  Serial.printf("Telegram: HTTP %d %s\n", code, code == 200 ? "(enviado)" : "(fallo)");
  return code == 200;
}

// Lógica de alerta con histéresis y reenvío periódico
void revisarAlerta(float t, float v, float i) {
  if (!alertaActiva && t >= TEMP_MAX) {
    String msg = "ALERTA: temperatura alta en el sistema\n"
                 "Temperatura: " + String(t, 1) + " C (limite " + String(TEMP_MAX, 1) + " C)\n"
                 "Voltaje: " + String(v, 2) + " V\n"
                 "Corriente: " + String(i, 2) + " A";
    if (enviarTelegram(msg)) {
      alertaActiva = true;
      ultimoAviso = millis();
    }   // si falla, reintenta en el siguiente ciclo
  }
  else if (alertaActiva && t >= TEMP_MAX && millis() - ultimoAviso >= REENVIO_ALERTA) {
    String msg = "SIGUE ALTA: " + String(t, 1) + " C (limite " + String(TEMP_MAX, 1) + " C)";
    if (enviarTelegram(msg)) ultimoAviso = millis();
  }
  else if (alertaActiva && t <= TEMP_MAX - HISTERESIS) {
    String msg = "Temperatura normalizada: " + String(t, 1) + " C";
    if (enviarTelegram(msg)) alertaActiva = false;
  }
}

void setup() {
  Serial.begin(115200);
  analogSetAttenuation(ADC_11db);
  sensorT.begin();
  delay(500);

  // Calibración del offset con 0 A: carga desconectada al arrancar
  offsetI = leerMv(PIN_I, 500) / DIV_ACS;
  Serial.printf("Offset ACS712: %.0f mV (esperado 2300-2600)\n", offsetI);
  if (offsetI < 2300 || offsetI > 2600) {
    Serial.println("ALERTA: offset fuera de rango. Revisa alimentacion/divisor y reinicia con la carga desconectada.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  conectarWiFi();
  delay(3000);   // deja que la red se estabilice

  if (WiFi.status() == WL_CONNECTED) {
    enviaGit: ClonerTelegram("Sistema de monitoreo iniciado. Limite de temperatura: " + String(TEMP_MAX, 1) + " C");
  }

  ultimoEnvio = millis() - INTERVALO;   // primer envío inmediato
}

void loop() {
  if (millis() - ultimoEnvio >= INTERVALO) {
    ultimoEnvio = millis();

    float v = leerMv(PIN_V) / 1000.0 * K_VOLT;
    float i = (offsetI - leerMv(PIN_I) / DIV_ACS) / 100.0;   // signo corregido, 100 mV/A
    sensorT.requestTemperatures();
    float t = sensorT.getTempCByIndex(0);
    bool tempValida = (t != DEVICE_DISCONNECTED_C);          // -127 = sensor desconectado
    float p = v * i;

    Serial.printf("V=%.2f V | I=%.2f A | P=%.1f W | T=%.1f C%s\n",
                  v, i, p, t, tempValida ? "" : " (sensor no detectado)");

    conectarWiFi();
    int r = -1;
    if (WiFi.status() == WL_CONNECTED) {
      r = enviarThingSpeak(v, i, t, p, tempValida);
      if (tempValida) revisarAlerta(t, v, i);
    }

    if (r >= 0) {
      fallosSeguidos = 0;   // 1 = aceptado, 0 = rechazado pero hay conexión
    } else if (++fallosSeguidos >= MAX_FALLOS) {
      Serial.println("Demasiados fallos de conexion seguidos, reiniciando...");
      delay(500);
      ESP.restart();
    }
  }
}