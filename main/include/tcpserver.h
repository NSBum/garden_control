/**
 * @file tcpserver.h
 *
 * TCP server utilities for the garden controller firmware
 *
 * Copyright (c) 2018 Alan K. duncan
 *
 * MIT license
 */
 
 #ifndef __TCPSERVER_H__
 #define __TCPSERVER_H__
 
 #include <i2cdev.h>
 
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the WiFi functionality for the device.
 */
void initialise_wifi(void);

/**
 * @brief Print the WiFi IP address to the monitor.
 * @param[in] pvParam Task parameters
 */
void printWiFiIP(void *pvParam);

#ifdef __cplusplus
}
#endif

#endif  // DS18B20_H

/**
 * @brief Convert, wait and read current temperature from device.
 * @param[in] ds18b20_info Pointer to device info instance. Must be initialised first.
 * @param[out] value Pointer to the measurement value returned by the device, in degrees Celsius.
 * @return DS18B20_OK if read is successful, otherwise error.
 */
DS18B20_ERROR ds18b20_convert_and_read_temp(const DS18B20_Info * ds18b20_info, float * value);