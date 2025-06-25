#include <profe_apruebeme_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include <NimBLEDevice.h>

// Pines para la cámara AI Thinker
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

// UUIDs BLE (servicio + característica)
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Tamaño de imagen para inferencia
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS   320   
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS   240
#define EI_CAMERA_FRAME_BYTE_SIZE         3

// Variables globales
static bool is_initialised = false;
static bool debug_nn = false;
uint8_t *snapshot_buf;

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pCharacteristic = nullptr;

// Configuración de la cámara
static camera_config_t camera_config = {
    .pin_pwdn       = PWDN_GPIO_NUM,
    .pin_reset      = RESET_GPIO_NUM,
    .pin_xclk       = XCLK_GPIO_NUM,
    .pin_sscb_sda   = SIOD_GPIO_NUM,
    .pin_sscb_scl   = SIOC_GPIO_NUM,
    .pin_d7         = Y9_GPIO_NUM,
    .pin_d6         = Y8_GPIO_NUM,
    .pin_d5         = Y7_GPIO_NUM,
    .pin_d4         = Y6_GPIO_NUM,
    .pin_d3         = Y5_GPIO_NUM,
    .pin_d2         = Y4_GPIO_NUM,
    .pin_d1         = Y3_GPIO_NUM,
    .pin_d0         = Y2_GPIO_NUM,
    .pin_vsync      = VSYNC_GPIO_NUM,
    .pin_href       = HREF_GPIO_NUM,
    .pin_pclk       = PCLK_GPIO_NUM,
    .xclk_freq_hz   = 20000000,
    .ledc_timer     = LEDC_TIMER_0,
    .ledc_channel   = LEDC_CHANNEL_0,
    .pixel_format   = PIXFORMAT_JPEG,
    .frame_size     = FRAMESIZE_QVGA,
    .jpeg_quality   = 12,
    .fb_count       = 1,
    .fb_location    = CAMERA_FB_IN_PSRAM,
    .grab_mode      = CAMERA_GRAB_WHEN_EMPTY,
};

// Inicializa la cámara
bool ei_camera_init() {
    if (is_initialised) return true;

    if (esp_camera_init(&camera_config) != ESP_OK) {
        ei_printf("❌ Error al iniciar cámara\n");
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
    }

    is_initialised = true;
    return true;
}

// Captura imagen RGB888 desde la cámara
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    if (!is_initialised) return false;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return false;

    bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);
    esp_camera_fb_return(fb);
    if (!converted) return false;

    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS) || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        ei::image::processing::crop_and_interpolate_rgb888(
            out_buf,
            EI_CAMERA_RAW_FRAME_BUFFER_COLS,
            EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
            out_buf,
            img_width,
            img_height
        );
    }

    return true;
}

// Prepara los datos de imagen para la inferencia
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
    size_t pixel_ix = offset * 3;
    size_t out_ptr_ix = 0;

    while (length--) {
        out_ptr[out_ptr_ix++] = (snapshot_buf[pixel_ix + 2] << 16) +
                                (snapshot_buf[pixel_ix + 1] << 8) +
                                snapshot_buf[pixel_ix];
        pixel_ix += 3;
    }

    return 0;
}

// Mide la distancia en cm usando sensor ultrasónico
long medirDistanciaCM() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duracion = pulseIn(ECHO_PIN, HIGH, 30000);  // Máximo 30ms de espera
    long distancia = duracion * 0.034 / 2;

    if (distancia <= 0 || distancia > 400) return -1; // Rango inválido
    return distancia;
}

void setup() {
    Serial.begin(115200);
    while (!Serial);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // BLE setup
    NimBLEDevice::init("ESP32-CAM BLE");
    pServer = NimBLEDevice::createServer();
    NimBLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pCharacteristic->setValue("Esperando señal...");
    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setAppearance(0x00);
    pAdvertising->setName("ESP32-CAM BLE");
    pAdvertising->start();

    Serial.println("✅ BLE iniciado");

    if (!ei_camera_init()) {
        ei_printf("❌ Error al inicializar cámara\n");
    } else {
        ei_printf("✅ Cámara inicializada\n");
    }

    ei_sleep(2000);
}

void loop() {
    // Medir distancia al frente
    long distancia = medirDistanciaCM();
    static unsigned long ultimaProximidadMs = 0;
    const unsigned long intervaloProximidadMs = 3000;

    // Si hay un objeto a menos de 1 metro, enviar alerta BLE
    if (distancia > 0 && distancia <= 100) {
        unsigned long ahora = millis();
        if (ahora - ultimaProximidadMs > intervaloProximidadMs) {
            pCharacteristic->setValue(" Objeto detectado ");
            pCharacteristic->notify();
            Serial.println("📡 Proximidad detectada, enviando alerta...");
            ultimaProximidadMs = ahora;
        }
    }

    // No procesar imagen si no hay nada cerca
    if (distancia < 0 || distancia > 100) {
        ei_printf("📷 Nada cercano, salto inferencia...\n");
        delay(500);
        return;
    }

    // Preparar cámara e inferencia
    if (ei_sleep(5) != EI_IMPULSE_OK) return;

    snapshot_buf = (uint8_t*)malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);
    if (!snapshot_buf) {
        ei_printf("❌ Memoria insuficiente para imagen\n");
        return;
    }

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &ei_camera_get_data;

    if (!ei_camera_capture(EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf)) {
        ei_printf("❌ Fallo al capturar imagen\n");
        free(snapshot_buf);
        return;
    }

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        ei_printf("❌ Error en inferencia (%d)\n", err);
        free(snapshot_buf);
        return;
    }

    ei_printf("🔎 Buscando objetos...\n");
    bool detectado = false;

    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) continue;

        ei_printf("  %s - Distancia: %ld cm\n", bb.label, bb.value, distancia);
        String mensaje = String(bb.label) + " (" + String(bb.value, 2) + ") - Dist: " + String(distancia) + " cm";
        pCharacteristic->setValue(mensaje.c_str());
        pCharacteristic->notify();
        detectado = true;
    }

    if (!detectado) {
        ei_printf("🚫 Ningún objeto detectado con confianza suficiente\n");
    }

    free(snapshot_buf);
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Modelo incorrecto para sensor de cámara"
#endif
