#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "time.h"

// Credenciales WiFi
const char* nombreWiFi = " ";
const char* contrasenaWiFi = " ";

// Servidor NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

// Palabras clave de seguridad
const String palabrasClaveSeguridad[] = {
  "zona segura", "acceso restringido", "riesgo electrico",
  "materiales inflamables", "escaleras", "salida", "extintor"
};
const int numPalabrasClave = sizeof(palabrasClaveSeguridad) / sizeof(palabrasClaveSeguridad[0]);

// Pines de la cámara AI Thinker
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Pines del sensor ultrasónico
#define TRIG_PIN 13
#define ECHO_PIN 12

WebServer server(80);
String ultimoMensajeClave = "";

void detectarSenalConGemini();
String codificacionBase64(const uint8_t* fotoImagen, size_t cantidadBytes);
long medirDistanciaCM();
String ontenerHoraActual();

void setup() {
  Serial.begin(115200);
  Serial.println("Iniciando...");

  // Inicializar WiFi
  WiFi.begin(nombreWiFi, contrasenaWiFi);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[+] WiFi conectado");
  Serial.println("[+] IP: " + WiFi.localIP().toString());

  // Inicializar pines del sensor ultrasónico
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Configurar la cámara
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 20;
  config.fb_count = 1;

  if (esp_camera_init(&config) == ESP_OK) {
    Serial.println("[+] Cámara iniciada");
  } else {
    Serial.println("[-] Error al iniciar cámara");
    ESP.restart();
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  server.on("/data", HTTP_GET, []() {
    String json = "{\"mensaje\": \"" + ultimoMensajeClave + "\"}";
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println("[+] Servidor iniciado en http://" + WiFi.localIP().toString() + "/data");

  // Lanzar tarea de Gemini
  xTaskCreate([](void*) {
    while (true) {
      detectarSenalConGemini();
      delay(1000);
    }
  }, "GeminiTask", 8192, nullptr, 1, nullptr);
}

void loop() {
  server.handleClient();

  long distancia = medirDistanciaCM();
  if (distancia > 0 && distancia <= 100) {
    ultimoMensajeClave = "objeto detectado";
  }
}

long medirDistanciaCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duracion = pulseIn(ECHO_PIN, HIGH, 30000);
  long distancia = duracion * 0.034 / 2;
  if (distancia <= 0 || distancia > 400) return -1;
  return distancia;
}

void detectarSenalConGemini() {
  if (ultimoMensajeClave == "objeto detectado") return; // No sobrescribir si está activo

  Serial.println("[+] Capturando imagen...");
  camera_fb_t* foto = esp_camera_fb_get();
  if (!foto) {
    Serial.println("[-] Error al capturar imagen");
    return;
  }
  String fotoBase64 = codificacionBase64(foto->buf, foto->len);
  esp_camera_fb_return(foto);

  HTTPClient http;
  String apiKey = "AIzaSyCNPtrZLhVMvf-eQoSx5MLSbgXYpFCEhrA";
  String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" + apiKey;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String mensajeJSON = "{\"contents\":[{\"parts\":[{";
  mensajeJSON += "\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"" + fotoBase64 + "\"}},";
  mensajeJSON += "{\"text\":\"Describe brevemente la imagen en espanol. Se claro y conciso.\"}]}]}";

  int httpCode = http.POST(mensajeJSON);
  if (httpCode > 0) {
    String respuesta = http.getString();
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, respuesta)) {
      Serial.println("[-] Error al procesar JSON");
      return;
    }

    String resultado = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    resultado.trim();
    String lower = resultado;
    lower.toLowerCase();

    bool detectado = false;
    for (int i = 0; i < numPalabrasClave; i++) {
      if (lower.indexOf(palabrasClaveSeguridad[i]) >= 0) {
        ultimoMensajeClave = palabrasClaveSeguridad[i];
        detectado = true;
        break;
      }
    }

    Serial.println("\n=== Resultado ===");
    Serial.println("Fecha y hora: " + ontenerHoraActual());
    if (detectado) {
      Serial.println("Senal de seguridad detectada: " + resultado);
    } else {
      Serial.println("No se detecto señal de seguridad.");
      ultimoMensajeClave = "";
    }
    Serial.println("=================\n");
  } else {
    Serial.println("[-] Error HTTP: " + String(httpCode));
  }
  http.end();
}

String codificacionBase64(const uint8_t* fotoImagen, size_t cantidadBytes) {
  const char listaCaracterBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String fotoCodificadoB64 = "";
  int i = 0;
  uint8_t array_3[3], array_4[4];
  while (cantidadBytes--) {
    array_3[i++] = *(fotoImagen++);
    if (i == 3) {
      array_4[0] = (array_3[0] & 0xfc) >> 2;
      array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
      array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);
      array_4[3] = array_3[2] & 0x3f;
      for (i = 0; i < 4; i++) fotoCodificadoB64 += listaCaracterBase64[array_4[i]];
      i = 0;
    }
  }
  if (i) {
    for (int j = i; j < 3; j++) array_3[j] = '\0';
    array_4[0] = (array_3[0] & 0xfc) >> 2;
    array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
    array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);
    array_4[3] = array_3[2] & 0x3f;
    for (int j = 0; j < i + 1; j++) fotoCodificadoB64 += listaCaracterBase64[array_4[j]];
    while (i++ < 3) fotoCodificadoB64 += '=';
  }
  return fotoCodificadoB64;
}

String ontenerHoraActual() {
  struct tm tiempo;
  if (!getLocalTime(&tiempo)) return "Time Error";
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tiempo);
  return String(buffer);
}
