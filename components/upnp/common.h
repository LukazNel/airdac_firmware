#ifndef AIRDAC_FIRMWARE_UPNP_COMMON_H
#define AIRDAC_FIRMWARE_UPNP_COMMON_H

#define SERVER_PORT 80

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define ESP_VER_STR STR(ESP_IDF_VERSION_MAJOR) "." STR(ESP_IDF_VERSION_MINOR) "." STR(ESP_IDF_VERSION_PATCH)

#define SERVER_STR "esp-idf/" ESP_VER_STR " UPnP/1.0 AirDAC/1.0"

#endif //AIRDAC_FIRMWARE_UPNP_COMMON_H