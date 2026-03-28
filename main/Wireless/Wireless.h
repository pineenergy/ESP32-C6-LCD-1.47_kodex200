#pragma once

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h> // For memcpy
#include "esp_system.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"

extern uint16_t BLE_NUM;
extern uint16_t WIFI_NUM;
extern bool Scan_finish;
extern volatile int32_t KODEX200_Price;
extern volatile int32_t KODEXSMR_Price;
extern volatile int32_t KODEX200_Change;
extern volatile int32_t KODEXSMR_Change;
extern volatile int32_t KODEX200_ChangeBp;
extern volatile int32_t KODEXSMR_ChangeBp;
extern volatile uint32_t ETF_Update_Sequence;
extern bool ETF_Price_Valid;
extern char ETF_Status[96];

void Wireless_Init(void);
void WIFI_Init(void *arg);
uint16_t WIFI_Scan(void);
void BLE_Init(void *arg);
uint16_t BLE_Scan(void);