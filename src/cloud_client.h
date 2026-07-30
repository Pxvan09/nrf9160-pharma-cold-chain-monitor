/*
 * cloud_client.h - AWS IoT Core MQTT/TLS connection manager
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CLOUD_CLIENT_H__
#define CLOUD_CLIENT_H__

#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialise the cloud client. Call once after LTE is registered.
 * @return 0 on success, negative errno on failure.
 */
int cloud_client_init(void);

/**
 * @brief Establish the MQTT/TLS connection to AWS IoT Core.
 *
 * Blocks until connected or until an internal timeout.
 *
 * @return 0 on success, negative errno on failure.
 */
int cloud_client_connect(void);

/**
 * @brief Publish a payload to the telemetry topic.
 *
 * Delivers with QoS 1 (at-least-once). Waits for PUBACK.
 * After PUBACK, sets SO_RAI so the modem releases RRC immediately.
 *
 * @param payload  JSON string to publish.
 * @param len      Length of payload (bytes).
 * @return 0 on PUBACK, negative errno on failure.
 */
int cloud_client_publish(const char *payload, size_t len);

/**
 * @brief Publish an AWS Device Shadow update.
 *
 * @param payload  JSON shadow document.
 * @param len      Length of payload (bytes).
 * @return 0 on success, negative errno on failure.
 */
int cloud_client_publish_shadow(const char *payload, size_t len);

/**
 * @brief Return true if the MQTT connection is currently established.
 */
bool cloud_client_is_connected(void);

/**
 * @brief Gracefully disconnect from AWS IoT Core.
 */
void cloud_client_disconnect(void);

#endif /* CLOUD_CLIENT_H__ */
