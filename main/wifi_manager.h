#pragma once
#include <stdbool.h>

void wifi_manager_init(void);
bool wifi_is_connected(void);
bool wifi_manager_is_ap_mode(void);
