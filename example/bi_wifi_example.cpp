/**
 * bi_wifi_example.cpp
 * Ejemplo de uso de la clase bi_wifi para ESP32-C3
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "bi_wifi.hpp"

static const char *TAG = "bi_wifi_example";

// Callback para los cambios de estado de la conexión WiFi
void onWiFiStateChanged(WiFiManager::WiFiState state, void* data) {
    switch (state) {
        case WiFiManager::WiFiState::DISCONNECTED:
            ESP_LOGI(TAG, "WiFi desconectado");
            break;
        case WiFiManager::WiFiState::CONNECTING:
            ESP_LOGI(TAG, "WiFi conectando...");
            break;
        case WiFiManager::WiFiState::CONNECTED:
        {
            ESP_LOGI(TAG, "WiFi conectado!");
            
            // Obtén una referencia al WiFiManager
            WiFiManager* wifi = static_cast<WiFiManager*>(data);
            if (wifi) {
                ESP_LOGI(TAG, "Conectado a la red: %s", wifi->getSSID().c_str());
                ESP_LOGI(TAG, "Dirección IP: %s", wifi->getIPAddress().c_str());
            }
            break;
        }
        case WiFiManager::WiFiState::PROVISIONING:
            ESP_LOGI(TAG, "Modo de provisioning WiFi activo");
            break;
        case WiFiManager::WiFiState::ERROR:
            ESP_LOGE(TAG, "Error en la conexión WiFi");
            break;
    }
}
void bi_wifi_example(void) {
    ESP_LOGI(TAG, "Iniciando aplicación...");
    
    // Crear instancia de WiFiManager
    WiFiManager wifi_manager("wifi");
    
    // Inicializar el gestor WiFi
    if (!wifi_manager.init()) {
        ESP_LOGE(TAG, "Error al inicializar WiFi Manager");
        return;
    }
    
    // Configurar callback para cambios de estado
    wifi_manager.setConnectionCallback(onWiFiStateChanged, &wifi_manager);
    
    // Método 1: Conectar usando credenciales almacenadas o iniciar provisioning
    wifi_manager.connect();
    
    // Método 2: Conectar directamente con credenciales conocidas
    // wifi_manager.connect("MI_SSID", "MI_PASSWORD", true);
    
    // Método 3: Iniciar directamente el modo de provisioning
    // wifi_manager.startProvisioning("ESP32-C3_DEVICE", 1, "12345678");
    
    // Loop principal
    while (1) {
        // Tu código principal aquí
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}