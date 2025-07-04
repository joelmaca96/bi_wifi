/**
 * WiFiManager.cpp
 * Implementation of WiFi Manager for ESP32-C3 using ESP-IDF with SoftAP provisioning
 */

#include "bi_wifi.hpp"
#include <cstring>
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// Event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define PROVISIONING_DONE  BIT2

WiFiManager::WiFiManager(const std::string& nvs_namespace)
    : nvs_namespace_(nvs_namespace),
      state_(WiFiState::DISCONNECTED),
      connection_callback_(nullptr),
      user_data_(nullptr),
      wifi_event_group_(nullptr),
      initialized_(false),
      provisioning_active_(false),
      current_ssid_(""),
      current_password_(""),
      netif_sta_(nullptr),
      netif_ap_(nullptr) {
}

WiFiManager::~WiFiManager() {
    if (initialized_) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        
        if (wifi_event_group_) {
            vEventGroupDelete(wifi_event_group_);
            wifi_event_group_ = nullptr;
        }
    }
}

bool WiFiManager::init() {
    if (initialized_) {
        ESP_LOGD(TAG, "WiFi manager already initialized");
        return true;
    }
    
    // Initialize NVS
    if (!initNVS()) {
        ESP_LOGE(TAG, "Failed to initialize NVS");
        return false;
    }
    
    // Initialize WiFi
    if (!initWiFi()) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return false;
    }
    
    initialized_ = true;
    updateState(WiFiState::DISCONNECTED);
    
    ESP_LOGD(TAG, "WiFi manager initialized successfully");
    return true;
}

bool WiFiManager::initNVS() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return (ret == ESP_OK);
}

bool WiFiManager::initWiFi() {
    wifi_event_group_ = xEventGroupCreate();
    if (!wifi_event_group_) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default station netif instance
    netif_sta_ = esp_netif_create_default_wifi_sta();
    if (netif_sta_ == NULL) {
        ESP_LOGE(TAG, "Failed to create default STA netif");
        return false;
    }
    
    // Also create AP netif for provisioning
    netif_ap_ = esp_netif_create_default_wifi_ap();
    if (netif_ap_ == NULL) {
        ESP_LOGE(TAG, "Failed to create default AP netif");
        return false;
    }
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &WiFiManager::eventHandler,
                                                       this,
                                                       NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &WiFiManager::eventHandler,
                                                       this,
                                                       NULL));
    
    // Set WiFi mode to APSTA (supports both AP and STA mode)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    return true;
}

bool WiFiManager::connect() {
    if (!initialized_ && !init()) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager");
        return false;
    }
    
    std::string ssid, password;
    if (loadCredentials(ssid, password)) {
        ESP_LOGD(TAG, "Found stored credentials, connecting to %s", ssid.c_str());
        return connect(ssid, password, false);
    } else {
        ESP_LOGD(TAG, "No stored credentials found, starting provisioning");
        
        // Generate a unique AP name based on MAC address
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char ap_name[32];
        snprintf(ap_name, sizeof(ap_name), "zubIOT_%02X%02X%02X", mac[3], mac[4], mac[5]);
        
        return startProvisioning(ap_name);
    }
}

bool WiFiManager::connect(const std::string& ssid, const std::string& password, bool save) {
    if (!initialized_ && !init()) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager");
        return false;
    }
    
    if (state_ == WiFiState::CONNECTING || state_ == WiFiState::CONNECTED) {
        ESP_LOGD(TAG, "Already connecting or connected, disconnecting first");
        disconnect();
    }
    
    // Save credentials if requested
    if (save) {
        saveCredentials(ssid, password);
    }
    
    // Clear event bits
    xEventGroupClearBits(wifi_event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Set WiFi configuration
    wifi_config_t wifi_config = {};
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    updateState(WiFiState::CONNECTING);
    current_ssid_ = ssid;
    current_password_ = password;
    
    ESP_LOGD(TAG, "Connecting to %s...", ssid.c_str());
    return true;
}

bool WiFiManager::disconnect() {
    if (!initialized_) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return false;
    }
    
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect from WiFi: %s", esp_err_to_name(err));
        return false;
    }
    
    updateState(WiFiState::DISCONNECTED);
    ESP_LOGD(TAG, "Disconnected from WiFi");
    return true;
}

bool WiFiManager::startProvisioning(const std::string& ap_ssid, const std::string& ap_password, 
                                   uint8_t security, const std::string& pop) {
    if (!initialized_ && !init()) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager");
        return false;
    }
    
    // If already provisioning, do nothing
    if (provisioning_active_) {
        ESP_LOGD(TAG, "Provisioning already active");
        return true;
    }
    
    // Clear stored credentials before starting provisioning
    clearStoredCredentials();
    
    // Clear event bits
    xEventGroupClearBits(wifi_event_group_, PROVISIONING_DONE);
    
    // Initialize provisioning manager with SoftAP scheme
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = {0},  // Initialize app event handler to zero
        .wifi_prov_conn_cfg = {0}  // Initialize connection config to zero
    };
    
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    
    // Set provisioning callback events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, 
                                              ESP_EVENT_ANY_ID, 
                                              &WiFiManager::provisioningEventHandler, 
                                              this));
    
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
    
    if (!provisioned) {
        ESP_LOGD(TAG, "Starting provisioning with SoftAP");
               
        // Start provisioning
        wifi_prov_security_t security_mode = security == 0 ? 
                                          WIFI_PROV_SECURITY_0 : WIFI_PROV_SECURITY_1;
                                          
        const char* pop_str = security == 0 ? NULL : pop.c_str();
        
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security_mode, 
                                                        pop_str, 
                                                        ap_ssid.c_str(), 
                                                        NULL));
        
        updateState(WiFiState::PROVISIONING);
        provisioning_active_ = true;
        
        ESP_LOGD(TAG, "Provisioning started with SoftAP SSID: %s", ap_ssid.c_str());
        if (!ap_password.empty()) {
            ESP_LOGD(TAG, "SoftAP Password: %s", ap_password.c_str());
        } else {
            ESP_LOGD(TAG, "SoftAP is open (no password)");
        }
        
        if (security != 0) {
            ESP_LOGD(TAG, "Proof of Possession (PoP): %s", pop.c_str());
        }
    } else {
        ESP_LOGD(TAG, "Already provisioned, connecting to WiFi");
        wifi_prov_mgr_deinit();
        return connect();
    }
    
    return true;
}

bool WiFiManager::stopProvisioning() {
    if (!provisioning_active_) {
        ESP_LOGD(TAG, "Provisioning not active");
        return true;
    }
    
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    provisioning_active_ = false;
    updateState(WiFiState::DISCONNECTED);
    
    ESP_LOGD(TAG, "Provisioning stopped");
    return true;
}

void WiFiManager::setConnectionCallback(ConnectionCallback callback, void* user_data) {
    connection_callback_ = callback;
    user_data_ = user_data;
}

bool WiFiManager::hasStoredCredentials() {
    std::string ssid, password;
    return loadCredentials(ssid, password);
}

bool WiFiManager::clearStoredCredentials() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_erase_key(nvs_handle, NVS_KEY_SSID);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error erasing SSID from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_erase_key(nvs_handle, NVS_KEY_PASSWORD);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error erasing password from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGD(TAG, "WiFi credentials cleared from NVS");
    return true;
}

WiFiManager::WiFiState WiFiManager::getState() const {
    return state_;
}

std::string WiFiManager::getSSID() const {
    if (state_ != WiFiState::CONNECTED) {
        return "";
    }
    return current_ssid_;
}

std::string WiFiManager::getIPAddress() const {
    if (state_ != WiFiState::CONNECTED) {
        return "";
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif_sta_, &ip_info) != ESP_OK) {
        return "";
    }
    
    char ip_addr[16];
    snprintf(ip_addr, sizeof(ip_addr), IPSTR, IP2STR(&ip_info.ip));
    return std::string(ip_addr);
}

bool WiFiManager::saveCredentials(const std::string& ssid, const std::string& password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error setting SSID in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error setting password in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGD(TAG, "WiFi credentials saved to NVS");
    return true;
}

bool WiFiManager::loadCredentials(std::string& ssid, std::string& password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return false;
    }
    
    size_t ssid_len = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, NULL, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "SSID not found in NVS");
        nvs_close(nvs_handle);
        return false;
    }
    
    char* ssid_buf = new char[ssid_len];
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid_buf, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error getting SSID from NVS: %s", esp_err_to_name(err));
        delete[] ssid_buf;
        nvs_close(nvs_handle);
        return false;
    }
    
    size_t pass_len = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, NULL, &pass_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Password not found in NVS");
        delete[] ssid_buf;
        nvs_close(nvs_handle);
        return false;
    }
    
    char* pass_buf = new char[pass_len];
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, pass_buf, &pass_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error getting password from NVS: %s", esp_err_to_name(err));
        delete[] ssid_buf;
        delete[] pass_buf;
        nvs_close(nvs_handle);
        return false;
    }
    
    ssid = std::string(ssid_buf);
    password = std::string(pass_buf);
    
    delete[] ssid_buf;
    delete[] pass_buf;
    nvs_close(nvs_handle);
    
    return true;
}

void WiFiManager::updateState(WiFiState new_state) {
    if (state_ != new_state) {
        state_ = new_state;
        
        if (connection_callback_) {
            connection_callback_(state_, user_data_);
        }
    }
}

void WiFiManager::eventHandler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    WiFiManager* self = static_cast<WiFiManager*>(arg);
    
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGD(TAG, "WiFi station started");
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGD(TAG, "WiFi disconnected");
            
            // Retry connection if we were connected before
            if (self->state_ == WiFiState::CONNECTED || self->state_ == WiFiState::CONNECTING) {
                ESP_LOGD(TAG, "Trying to reconnect...");
                esp_wifi_connect();
                self->updateState(WiFiState::CONNECTING);
            } else {
                self->updateState(WiFiState::DISCONNECTED);
                xEventGroupSetBits(self->wifi_event_group_, WIFI_FAIL_BIT);
            }
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGD(TAG, "Station " MACSTR " joined, AID=%d", 
                    MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGD(TAG, "Station " MACSTR " left, AID=%d", 
                    MAC2STR(event->mac), event->aid);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGD(TAG, "WiFi connected with IP Address:" IPSTR, 
                IP2STR(&event->ip_info.ip));
        
        self->updateState(WiFiState::CONNECTED);
        xEventGroupSetBits(self->wifi_event_group_, WIFI_CONNECTED_BIT);
    }
}

void WiFiManager::provisioningEventHandler(void* arg, esp_event_base_t event_base,
                                          int32_t event_id, void* event_data) {
    WiFiManager* self = static_cast<WiFiManager*>(arg);
    
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGD(self->TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t* wifi_sta_cfg = (wifi_sta_config_t*)event_data;
                ESP_LOGD(self->TAG, "Received WiFi credentials:");
                ESP_LOGD(self->TAG, "SSID: %s", (const char*)wifi_sta_cfg->ssid);
                ESP_LOGD(self->TAG, "Password: %s", (const char*)wifi_sta_cfg->password);
                
                // Save credentials to NVS
                self->saveCredentials((const char*)wifi_sta_cfg->ssid, 
                                     (const char*)wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t* reason = (wifi_prov_sta_fail_reason_t*)event_data;
                ESP_LOGE(self->TAG, "Provisioning failed: %d", *reason);
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGD(self->TAG, "Provisioning successful");
                break;
            case WIFI_PROV_END:
                {
                ESP_LOGD(self->TAG, "Provisioning ended");
                
                // Deinitialize provisioning
                wifi_prov_mgr_deinit();
                self->provisioning_active_ = false;
                
                // Connect with the new credentials
                xEventGroupSetBits(self->wifi_event_group_, PROVISIONING_DONE);
                
                std::string ssid, password;
                if (self->loadCredentials(ssid, password)) {
                    self->connect(ssid, password, false);
                } else {
                    self->updateState(WiFiState::ERROR);
                    ESP_LOGE(self->TAG, "Failed to load credentials after provisioning");
                }
                break;
                }
            default:
                break;
        }
    }
}