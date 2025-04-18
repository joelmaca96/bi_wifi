/**
 * BI_WIFI.hpp
 * ESP32-C3 WiFi Manager for ESP-IDF
 * 
 * Manages WiFi connection, credentials storage in NVS, and WiFi provisioning via SoftAP
 */

#ifndef BI_WIFI_HPP
#define BI_WIFI_HPP

#include <string>
#include <memory>
#include <functional>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

class WiFiManager {
public:
    enum class WiFiState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        PROVISIONING,
        ERROR
    };

    using ConnectionCallback = std::function<void(WiFiState state, void* data)>;

    /**
     * Constructor
     * 
     * @param nvs_namespace Namespace for storing WiFi credentials in NVS
     */
    WiFiManager(const std::string& nvs_namespace = "wifi_config");
    
    /**
     * Destructor
     */
    ~WiFiManager();

    /**
     * Initialize WiFi subsystem
     * 
     * @return true if initialization was successful
     */
    bool init();

    /**
     * Connect to WiFi using stored credentials if available
     * or start provisioning if no credentials found
     * 
     * @return true if connection process started successfully
     */
    bool connect();

    /**
     * Connect to WiFi with the given credentials
     * 
     * @param ssid WiFi SSID
     * @param password WiFi password
     * @param save Whether to save the credentials in NVS
     * @return true if connection process started successfully
     */
    bool connect(const std::string& ssid, const std::string& password, bool save = true);

    /**
     * Disconnect from WiFi
     * 
     * @return true if disconnection was successful
     */
    bool disconnect();

    /**
     * Start WiFi provisioning mode using SoftAP
     * 
     * @param ap_ssid SSID for the SoftAP
     * @param ap_password Password for the SoftAP (empty for open network)
     * @param security Whether to use security during provisioning (0 for none, 1 for secure)
     * @param pop Proof of possession for secure provisioning
     * @return true if provisioning started successfully
     */
    bool startProvisioning(const std::string& ap_ssid, const std::string& ap_password = "", 
                          uint8_t security = 1, const std::string& pop = "abcd1234");

    /**
     * Stop WiFi provisioning mode
     * 
     * @return true if stopped successfully
     */
    bool stopProvisioning();

    /**
     * Set connection state change callback
     * 
     * @param callback Function to call when connection state changes
     */
    void setConnectionCallback(ConnectionCallback callback, void* user_data = nullptr);

    /**
     * Check if WiFi credentials are stored in NVS
     * 
     * @return true if credentials are found
     */
    bool hasStoredCredentials();

    /**
     * Clear stored WiFi credentials from NVS
     * 
     * @return true if credentials were cleared successfully
     */
    bool clearStoredCredentials();

    /**
     * Get current WiFi connection state
     * 
     * @return Current WiFi state
     */
    WiFiState getState() const;

    /**
     * Get current WiFi SSID
     * 
     * @return SSID of connected network or empty string if disconnected
     */
    std::string getSSID() const;

    /**
     * Get WiFi IP address
     * 
     * @return IP address as string or empty string if not connected
     */
    std::string getIPAddress() const;

private:
    static constexpr const char* TAG = "WiFiManager";
    
    // Namespace for storing WiFi credentials in NVS
    std::string nvs_namespace_;
    
    // Current WiFi state
    WiFiState state_;
    
    // Callback for connection state changes
    ConnectionCallback connection_callback_;
    void* user_data_;

    // WiFi event group for synchronization
    EventGroupHandle_t wifi_event_group_;
    
    // Flag if WiFi subsystem is initialized
    bool initialized_;
    
    // Provisioning mode
    bool provisioning_active_;
    
    // Current connection info
    std::string current_ssid_;
    std::string current_password_;
    
    // NVS keys for stored data
    static constexpr const char* NVS_KEY_SSID = "wifi_ssid";
    static constexpr const char* NVS_KEY_PASSWORD = "wifi_pass";

    // netif instances for STA and AP
    esp_netif_t* netif_sta_;
    esp_netif_t* netif_ap_;

    // Event handler for WiFi and IP events
    static void eventHandler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data);
    
    // Provisioning event handler
    static void provisioningEventHandler(void* arg, esp_event_base_t event_base,
                                        int32_t event_id, void* event_data);

    // Internal methods
    bool saveCredentials(const std::string& ssid, const std::string& password);
    bool loadCredentials(std::string& ssid, std::string& password);
    void updateState(WiFiState new_state);
    bool initNVS();
    bool initWiFi();
};

#endif // BI_WIFI_HPP