#include "Wireless.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include <stdlib.h>
#include <time.h>

uint16_t BLE_NUM = 0;
uint16_t WIFI_NUM = 0;
bool Scan_finish = 0;
volatile int32_t KODEX200_Price = -1;
volatile int32_t KODEXSMR_Price = -1;
volatile int32_t KODEX200_Change = 0;
volatile int32_t KODEXSMR_Change = 0;
volatile int32_t KODEX200_ChangeBp = 0;
volatile int32_t KODEXSMR_ChangeBp = 0;
volatile uint32_t ETF_Update_Sequence = 0;
bool ETF_Price_Valid = false;
char ETF_Status[96] = "Wi-Fi connecting...";

static EventGroupHandle_t s_wifi_event_group;
static bool s_netif_initialized = false;
static bool s_scan_only_mode = true;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_WIFI_RETRY 1
static int s_wifi_retry_num = 0;

typedef struct
{
    const char *ssid;
    const char *password;
} wifi_credential_t;

static const wifi_credential_t s_wifi_candidates[] = {
    {CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PASSWORD},
    {CONFIG_APP_WIFI_SSID_2, CONFIG_APP_WIFI_PASSWORD_2},
};

static size_t s_wifi_candidate_count = 0;
static size_t s_active_wifi_index = 0;

static size_t get_wifi_candidate_count(void)
{
    size_t count = 0;
    size_t total = sizeof(s_wifi_candidates) / sizeof(s_wifi_candidates[0]);
    for (size_t i = 0; i < total; i++)
    {
        if (s_wifi_candidates[i].ssid != NULL && s_wifi_candidates[i].ssid[0] != '\0')
        {
            count++;
        }
    }
    return count;
}

static const wifi_credential_t *get_active_wifi_credential(void)
{
    if (s_active_wifi_index >= s_wifi_candidate_count)
    {
        return NULL;
    }
    return &s_wifi_candidates[s_active_wifi_index];
}

static esp_err_t apply_active_wifi_credential(void)
{
    const wifi_credential_t *cred = get_active_wifi_credential();
    if (cred == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strncpy((char *)wifi_config.sta.ssid, cred->ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (cred->password != NULL)
    {
        strncpy((char *)wifi_config.sta.password, cred->password, sizeof(wifi_config.sta.password) - 1);
    }

    ESP_LOGI("WIFI", "Configured SSID[%u/%u]: %s",
             (unsigned)(s_active_wifi_index + 1),
             (unsigned)s_wifi_candidate_count,
             cred->ssid);
    ESP_LOGI("WIFI", "Password length: %d chars", (int)strlen(cred->password));
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static bool switch_to_next_wifi_credential(void)
{
    if (s_active_wifi_index + 1 >= s_wifi_candidate_count)
    {
        return false;
    }

    s_active_wifi_index++;
    s_wifi_retry_num = 0;

    esp_err_t err = apply_active_wifi_credential();
    if (err != ESP_OK)
    {
        ESP_LOGE("WIFI", "Failed to apply fallback Wi-Fi config: %s", esp_err_to_name(err));
        return false;
    }

    const wifi_credential_t *cred = get_active_wifi_credential();
    snprintf(ETF_Status, sizeof(ETF_Status), "Switching to %s", cred->ssid);
    ESP_LOGW("WIFI", "Switching to fallback SSID: %s", cred->ssid);
    return true;
}

static esp_err_t ensure_time_synced(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year >= (2023 - 1900))
    {
        ESP_LOGI("WIFI", "System time already valid");
        return ESP_OK;
    }

    ESP_LOGI("WIFI", "Starting SNTP time sync");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();

    for (int i = 0; i < 15; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2023 - 1900))
        {
            ESP_LOGI("WIFI", "Time sync complete: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900,
                     timeinfo.tm_mon + 1,
                     timeinfo.tm_mday,
                     timeinfo.tm_hour,
                     timeinfo.tm_min,
                     timeinfo.tm_sec);
            return ESP_OK;
        }
    }

    ESP_LOGW("WIFI", "SNTP sync timeout, HTTPS may fail due to certificate validation");
    return ESP_ERR_TIMEOUT;
}

static const char *wifi_disconnect_reason_to_str(int reason)
{
    switch (reason)
    {
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

static const char *wifi_authmode_to_str(wifi_auth_mode_t authmode)
{
    switch (authmode)
    {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "UNKNOWN";
    }
}

static bool log_visible_wifi_networks(const char *target_ssid)
{
    uint16_t ap_count = 0;
    bool target_found = false;
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
    };

    ESP_LOGI("WIFI", "[SCAN] Starting AP scan");
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    if (ap_count == 0)
    {
        ESP_LOGW("WIFI", "[SCAN] No access points found");
        snprintf(ETF_Status, sizeof(ETF_Status), "No Wi-Fi AP found");
        return false;
    }

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_list == NULL)
    {
        ESP_LOGE("WIFI", "[SCAN] Failed to allocate AP list for %u records", ap_count);
        snprintf(ETF_Status, sizeof(ETF_Status), "Wi-Fi scan alloc fail");
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    ESP_LOGI("WIFI", "[SCAN] %u access points found", ap_count);
    for (uint16_t i = 0; i < ap_count; i++)
    {
        const char *ssid = (const char *)ap_list[i].ssid;
        if (target_ssid != NULL && ssid[0] != '\0' && strcmp(ssid, target_ssid) == 0)
        {
            target_found = true;
        }
        ESP_LOGI("WIFI",
                 "[SCAN][%u] SSID='%s' RSSI=%d CH=%u AUTH=%s",
                 i + 1,
                 ssid[0] ? ssid : "<hidden>",
                 ap_list[i].rssi,
                 ap_list[i].primary,
                 wifi_authmode_to_str(ap_list[i].authmode));
    }

    free(ap_list);
    snprintf(ETF_Status, sizeof(ETF_Status), target_found ? "Target Wi-Fi found" : "Target Wi-Fi not found");
    return target_found;
}

static esp_err_t parse_symbol_quote(const char *json,
                                    const char *symbol,
                                    int32_t *out_price,
                                    int32_t *out_change,
                                    int32_t *out_change_bp)
{
    char needle[64] = {0};
    snprintf(needle, sizeof(needle), "\"symbol\":\"%s\"", symbol);
    const char *sym_pos = strstr(json, needle);
    if (!sym_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }

    const char *price_key = "\"regularMarketPrice\":";
    const char *price_pos = strstr(sym_pos, price_key);
    if (!price_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }

    price_pos += strlen(price_key);
    double parsed_price = strtod(price_pos, NULL);
    *out_price = (int32_t)(parsed_price + 0.5);

    const char *change_key = "\"regularMarketChange\":";
    const char *change_pos = strstr(sym_pos, change_key);
    if (!change_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }

    change_pos += strlen(change_key);
    double parsed_change = strtod(change_pos, NULL);
    if (parsed_change >= 0)
    {
        *out_change = (int32_t)(parsed_change + 0.5);
    }
    else
    {
        *out_change = (int32_t)(parsed_change - 0.5);
    }

    const char *change_pct_key = "\"regularMarketChangePercent\":";
    const char *change_pct_pos = strstr(sym_pos, change_pct_key);
    if (!change_pct_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }

    change_pct_pos += strlen(change_pct_key);
    double parsed_change_pct = strtod(change_pct_pos, NULL);
    double scaled = parsed_change_pct * 100.0;
    if (scaled >= 0)
    {
        *out_change_bp = (int32_t)(scaled + 0.5);
    }
    else
    {
        *out_change_bp = (int32_t)(scaled - 0.5);
    }

    return ESP_OK;
}

static void symbol_to_item_code(const char *symbol, char *out_code, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; symbol[i] != '\0' && j + 1 < out_size; i++)
    {
        if ((symbol[i] >= '0' && symbol[i] <= '9') ||
            (symbol[i] >= 'A' && symbol[i] <= 'Z') ||
            (symbol[i] >= 'a' && symbol[i] <= 'z'))
        {
            out_code[j++] = symbol[i];
        }
        else if (symbol[i] == '.')
        {
            break;
        }
    }
    out_code[j] = '\0';
}

static esp_err_t parse_itemcode_quote(const char *json,
                                      const char *item_code,
                                      int32_t *out_price,
                                      int32_t *out_change,
                                      int32_t *out_change_bp)
{
    char needle_itemcode[64] = {0};
    char needle_cd[64] = {0};
    snprintf(needle_itemcode, sizeof(needle_itemcode), "\"itemCode\":\"%s\"", item_code);
    snprintf(needle_cd, sizeof(needle_cd), "\"cd\":\"%s\"", item_code);

    const char *item_pos = strstr(json, needle_itemcode);
    if (!item_pos)
    {
        item_pos = strstr(json, needle_cd);
    }
    if (!item_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }

    const char *price_key = "\"nv\":";
    const char *price_pos = strstr(item_pos, price_key);
    if (!price_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }

    price_pos += strlen(price_key);
    while (*price_pos == ' ' || *price_pos == '\t' || *price_pos == '"')
    {
        price_pos++;
    }

    double parsed = strtod(price_pos, NULL);
    if (parsed <= 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_price = (int32_t)(parsed + 0.5);

    const char *change_key = "\"cv\":";
    const char *change_pos = strstr(item_pos, change_key);
    if (!change_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }
    change_pos += strlen(change_key);
    while (*change_pos == ' ' || *change_pos == '\t' || *change_pos == '"')
    {
        change_pos++;
    }

    double parsed_change = strtod(change_pos, NULL);
    int32_t day_change = (int32_t)(parsed_change + 0.5);

    const char *change_rate_key = "\"cr\":";
    const char *change_rate_pos = strstr(item_pos, change_rate_key);
    if (!change_rate_pos)
    {
        return ESP_ERR_NOT_FOUND;
    }
    change_rate_pos += strlen(change_rate_key);
    while (*change_rate_pos == ' ' || *change_rate_pos == '\t' || *change_rate_pos == '"')
    {
        change_rate_pos++;
    }

    double parsed_change_rate = strtod(change_rate_pos, NULL);
    int32_t day_change_bp = (int32_t)(parsed_change_rate * 100.0 + 0.5);

    const char *rf_key = "\"rf\":\"";
    const char *rf_pos = strstr(item_pos, rf_key);
    if (rf_pos)
    {
        rf_pos += strlen(rf_key);
        char rf = *rf_pos;
        // Naver market flag: 1/2 up, 3 flat, 4/5 down
        if (rf == '4' || rf == '5')
        {
            day_change = -day_change;
            day_change_bp = -day_change_bp;
        }
    }

    *out_change = day_change;
    *out_change_bp = day_change_bp;
    return ESP_OK;
}

static esp_err_t http_get_to_buffer(const char *url, int timeout_ms, char **out_response, int *out_len)
{
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .user_agent = "ESP32-C6-ETF/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE("ETF", "HTTP open failed: %s (%d)", esp_err_to_name(err), err);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (content_length < 0 || status_code != 200)
    {
        ESP_LOGE("ETF", "HTTP status=%d (len=%d) for URL: %s", status_code, content_length, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int alloc_len = content_length > 0 ? content_length + 1 : 8192;
    char *response = (char *)calloc(alloc_len, sizeof(char));
    if (!response)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (1)
    {
        int read_len = esp_http_client_read(client, response + total, alloc_len - total - 1);
        if (read_len <= 0)
        {
            break;
        }
        total += read_len;
        if (total >= alloc_len - 1)
        {
            break;
        }
    }
    response[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_response = response;
    *out_len = total;
    return ESP_OK;
}

static esp_err_t fetch_naver_price_by_code(const char *item_code,
                                           int32_t *out_price,
                                           int32_t *out_change,
                                           int32_t *out_change_bp)
{
    char *response = NULL;
    int total = 0;
    char naver_url[256] = {0};
    snprintf(naver_url,
             sizeof(naver_url),
             "https://polling.finance.naver.com/api/realtime?query=SERVICE_ITEM:%s",
             item_code);

    esp_err_t err = http_get_to_buffer(naver_url, 8000, &response, &total);
    if (err != ESP_OK)
    {
        if (response != NULL)
        {
            free(response);
        }
        return err;
    }

    err = parse_itemcode_quote(response, item_code, out_price, out_change, out_change_bp);
    if (err != ESP_OK)
    {
        ESP_LOGE("ETF", "Naver parse failed for code %s, response snippet: %.160s", item_code, response);
    }

    free(response);
    return err;
}

static esp_err_t fetch_etf_prices(int32_t *kodex200,
                                  int32_t *smr,
                                  int32_t *kodex200_change,
                                  int32_t *smr_change,
                                  int32_t *kodex200_change_bp,
                                  int32_t *smr_change_bp)
{
    esp_err_t err;
    char *response = NULL;
    int total = 0;
    char yahoo_url[256] = {0};
    snprintf(yahoo_url,
             sizeof(yahoo_url),
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s,%s",
             CONFIG_APP_STOCK_SYMBOL_KODEX200,
             CONFIG_APP_STOCK_SYMBOL_SMR);

    err = http_get_to_buffer(yahoo_url, 8000, &response, &total);

    int32_t p1 = -1;
    int32_t p2 = -1;
    int32_t c1 = 0;
    int32_t c2 = 0;
    int32_t r1_bp = 0;
    int32_t r2_bp = 0;
    if (err == ESP_OK)
    {
        err = parse_symbol_quote(response, CONFIG_APP_STOCK_SYMBOL_KODEX200, &p1, &c1, &r1_bp);
        if (err == ESP_OK)
        {
            err = parse_symbol_quote(response, CONFIG_APP_STOCK_SYMBOL_SMR, &p2, &c2, &r2_bp);
        }
    }

    if (err == ESP_OK)
    {
        free(response);
        *kodex200 = p1;
        *smr = p2;
        *kodex200_change = c1;
        *smr_change = c2;
        *kodex200_change_bp = r1_bp;
        *smr_change_bp = r2_bp;
        return ESP_OK;
    }

    ESP_LOGW("ETF", "Yahoo fetch/parse failed, trying Naver fallback: %s (%d)", esp_err_to_name(err), err);
    if (response != NULL)
    {
        ESP_LOGW("ETF", "Yahoo response snippet: %.120s", response);
        free(response);
        response = NULL;
    }

    char code1[16] = {0};
    char code2[16] = {0};
    symbol_to_item_code(CONFIG_APP_STOCK_SYMBOL_KODEX200, code1, sizeof(code1));
    symbol_to_item_code(CONFIG_APP_STOCK_SYMBOL_SMR, code2, sizeof(code2));

    err = fetch_naver_price_by_code(code1, &p1, &c1, &r1_bp);
    if (err == ESP_OK)
    {
        err = fetch_naver_price_by_code(code2, &p2, &c2, &r2_bp);
    }
    if (err != ESP_OK)
    {
        return err;
    }

    *kodex200 = p1;
    *smr = p2;
    *kodex200_change = c1;
    *smr_change = c2;
    *kodex200_change_bp = r1_bp;
    *smr_change_bp = r2_bp;
    ESP_LOGI("ETF", "Using Naver fallback data source");
    return ESP_OK;
}

static void etf_price_task(void *arg)
{
    while (1)
    {
        int32_t p_kodex200 = -1;
        int32_t p_smr = -1;
        int32_t c_kodex200 = 0;
        int32_t c_smr = 0;
        int32_t r_kodex200_bp = 0;
        int32_t r_smr_bp = 0;
        esp_err_t err = fetch_etf_prices(&p_kodex200,
                                         &p_smr,
                                         &c_kodex200,
                                         &c_smr,
                                         &r_kodex200_bp,
                                         &r_smr_bp);
        if (err == ESP_OK)
        {
            KODEX200_Price = p_kodex200;
            KODEXSMR_Price = p_smr;
            KODEX200_Change = c_kodex200;
            KODEXSMR_Change = c_smr;
            KODEX200_ChangeBp = r_kodex200_bp;
            KODEXSMR_ChangeBp = r_smr_bp;
            ETF_Price_Valid = true;
            snprintf(ETF_Status, sizeof(ETF_Status), "Updated");
            ESP_LOGI("ETF", "Updated KODEX200=%ld (%+ld, %+ld.%02ld%%), SMR=%ld (%+ld, %+ld.%02ld%%)",
                     (long)KODEX200_Price,
                     (long)KODEX200_Change,
                     (long)(KODEX200_ChangeBp / 100),
                     (long)abs(KODEX200_ChangeBp % 100),
                     (long)KODEXSMR_Price,
                     (long)KODEXSMR_Change,
                     (long)(KODEXSMR_ChangeBp / 100),
                     (long)abs(KODEXSMR_ChangeBp % 100));
        }
        else
        {
            ETF_Price_Valid = false;
            snprintf(ETF_Status, sizeof(ETF_Status), "Fetch fail (%d)", err);
            ESP_LOGE("ETF", "Fetch failed: %s (%d)", esp_err_to_name(err), err);
        }
        ETF_Update_Sequence++;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_STOCK_UPDATE_PERIOD_SEC * 1000));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        if (s_scan_only_mode)
        {
            ESP_LOGI("WIFI", "[START] Scan-only mode enabled");
            return;
        }
        ESP_LOGI("WIFI", "[START] Attempting to connect...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_scan_only_mode)
        {
            ESP_LOGI("WIFI", "[DISCONNECT] Ignored in scan-only mode");
            return;
        }
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI("WIFI", "[DISCONNECT] Reason: %d (%s)",
                 disconnected->reason,
                 wifi_disconnect_reason_to_str(disconnected->reason));

        if (s_wifi_retry_num < MAX_WIFI_RETRY)
        {
            ESP_LOGI("WIFI", "[DISCONNECT] Retry %d/%d", s_wifi_retry_num + 1, MAX_WIFI_RETRY);
            esp_wifi_connect();
            s_wifi_retry_num++;
            snprintf(ETF_Status, sizeof(ETF_Status), "Wi-Fi reconnecting... (%d)", s_wifi_retry_num);
        }
        else if (switch_to_next_wifi_credential())
        {
            esp_wifi_connect();
        }
        else
        {
            ESP_LOGI("WIFI", "[FAILED] Max retries exceeded (Reason: %d, %s)",
                     disconnected->reason,
                     wifi_disconnect_reason_to_str(disconnected->reason));
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            snprintf(ETF_Status, sizeof(ETF_Status), "Wi-Fi connect failed");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI", "[SUCCESS] Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        snprintf(ETF_Status, sizeof(ETF_Status), "Wi-Fi connected");
    }
}

bool WiFi_Scan_Finish = 0;
bool BLE_Scan_Finish = 0;
void Wireless_Init(void)
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // WiFi
    xTaskCreatePinnedToCore(
        WIFI_Init,
        "WIFI task",
        8192,
        NULL,
        1,
        NULL,
        0);
    // BLE - Disabled: Not needed for ETF price monitoring
    /*
    xTaskCreatePinnedToCore(
        BLE_Init,
        "BLE task",
        4096,
        NULL,
        2,
        NULL,
        0);
    */
}

void WIFI_Init(void *arg)
{
    if (!s_netif_initialized)
    {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        s_netif_initialized = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_wifi_candidate_count = get_wifi_candidate_count();
    s_active_wifi_index = 0;
    if (s_wifi_candidate_count == 0)
    {
        ESP_LOGE("WIFI", "No configured SSID. Set APP_WIFI_SSID (and optional APP_WIFI_SSID_2)");
        snprintf(ETF_Status, sizeof(ETF_Status), "Wi-Fi config missing");
        vTaskDelete(NULL);
    }

    ESP_ERROR_CHECK(apply_active_wifi_credential());

    ESP_LOGI("WIFI", "Auth threshold: WPA or stronger");
    ESP_LOGI("WIFI", "Mode: scan then connect");

    ESP_ERROR_CHECK(esp_wifi_start());

    log_visible_wifi_networks(NULL);

    s_scan_only_mode = false;
    s_wifi_retry_num = 0;
    const wifi_credential_t *cred = get_active_wifi_credential();
    snprintf(ETF_Status, sizeof(ETF_Status), "Connecting to %s", cred->ssid);
    ESP_LOGI("WIFI", "Starting connection to %s", cred->ssid);
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT)
    {
        ensure_time_synced();
        xTaskCreatePinnedToCore(etf_price_task,
                                "ETF Price Task",
                                8192,
                                NULL,
                                3,
                                NULL,
                                0);
    }

    vTaskDelete(NULL);
}
uint16_t WIFI_Scan(void)
{
    uint16_t ap_count = 0;
    esp_wifi_scan_start(NULL, true);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    esp_wifi_scan_stop();
    WiFi_Scan_Finish = 1;
    if (BLE_Scan_Finish == 1)
        Scan_finish = 1;
    return ap_count;
}

#define GATTC_TAG "GATTC_TAG"
#define SCAN_DURATION 5
#define MAX_DISCOVERED_DEVICES 100

typedef struct
{
    uint8_t address[6];
    bool is_valid;
} discovered_device_t;

static discovered_device_t discovered_devices[MAX_DISCOVERED_DEVICES];
static size_t num_discovered_devices = 0;
static size_t num_devices_with_name = 0;

static bool is_device_discovered(const uint8_t *addr)
{
    for (size_t i = 0; i < num_discovered_devices; i++)
    {
        if (memcmp(discovered_devices[i].address, addr, 6) == 0)
        {
            return true;
        }
    }
    return false;
}

static void add_device_to_list(const uint8_t *addr)
{
    if (num_discovered_devices < MAX_DISCOVERED_DEVICES)
    {
        memcpy(discovered_devices[num_discovered_devices].address, addr, 6);
        discovered_devices[num_discovered_devices].is_valid = true;
        num_discovered_devices++;
    }
}

static bool extract_device_name(const uint8_t *adv_data, uint8_t adv_data_len, char *device_name, size_t max_name_len)
{
    size_t offset = 0;
    while (offset < adv_data_len)
    {
        if (adv_data[offset] == 0)
            break;

        uint8_t length = adv_data[offset];
        if (length == 0 || offset + length > adv_data_len)
            break;

        uint8_t type = adv_data[offset + 1];
        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || type == ESP_BLE_AD_TYPE_NAME_SHORT)
        {
            if (length > 1 && length - 1 < max_name_len)
            {
                memcpy(device_name, &adv_data[offset + 2], length - 1);
                device_name[length - 1] = '\0';
                return true;
            }
            else
            {
                return false;
            }
        }
        offset += length + 1;
    }
    return false;
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    // BLE_Init disabled - Not needed for ETF price monitoring
    /*
    static char device_name[100];

    switch (event)
    {
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
        {
            if (!is_device_discovered(param->scan_rst.bda))
            {
                add_device_to_list(param->scan_rst.bda);
                BLE_NUM++;

                if (extract_device_name(param->scan_rst.ble_adv, param->scan_rst.adv_data_len, device_name, sizeof(device_name)))
                {
                    num_devices_with_name++;
                    printf("Found device: %02X:%02X:%02X:%02X:%02X:%02X\n        Name: %s\n        RSSI: %d\r\n",
                           param->scan_rst.bda[0], param->scan_rst.bda[1],
                           param->scan_rst.bda[2], param->scan_rst.bda[3],
                           param->scan_rst.bda[4], param->scan_rst.bda[5],
                           device_name, param->scan_rst.rssi);
                    printf("\r\n");
                }
                else
                {
                    printf("Found device: %02X:%02X:%02X:%02X:%02X:%02X\n        Name: Unknown\n        RSSI: %d\r\n",
                           param->scan_rst.bda[0], param->scan_rst.bda[1],
                           param->scan_rst.bda[2], param->scan_rst.bda[3],
                           param->scan_rst.bda[4], param->scan_rst.bda[5],
                           param->scan_rst.rssi);
                    printf("\r\n");
                }
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(GATTC_TAG, "Scan complete. Total devices found: %d (with names: %d)", BLE_NUM, num_devices_with_name);
        break;
    default:
        break;
    }
    */
}

// BLE_Init disabled - Not needed for ETF price monitoring
/*
void BLE_Init(void *arg)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret)
    {
        printf("%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret)
    {
        printf("%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_init();
    if (ret)
    {
        printf("%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_enable();
    if (ret)
    {
        printf("%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    // register the  callback function to the gap module
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret)
    {
        printf("%s gap register error, error code = %x\n", __func__, ret);
        return;
    }
    BLE_Scan();
    // while(1)
    // {
    //     vTaskDelay(pdMS_TO_TICKS(150));
    // }

    vTaskDelete(NULL);
}
uint16_t BLE_Scan(void)
{
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));

    printf("Starting BLE scan...\n");
    ESP_ERROR_CHECK(esp_ble_gap_start_scanning(SCAN_DURATION));

    // Set scanning duration
    vTaskDelay(SCAN_DURATION * 1000 / portTICK_PERIOD_MS);

    printf("Stopping BLE scan...\n");
    ESP_ERROR_CHECK(esp_ble_gap_stop_scanning());
    BLE_Scan_Finish = 1;
    if (WiFi_Scan_Finish == 1)
        Scan_finish = 1;
    return BLE_NUM;
}
*/