/*
 * IoT Pharma Monitoring Device - AWS IoT Core client
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cloud_client.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <net/aws_iot.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(cloud, CONFIG_PHARMA_LOG_LEVEL);

static K_SEM_DEFINE(connected_sem, 0, 1);
static K_SEM_DEFINE(puback_sem, 0, 1);

static char topic_telemetry[APP_TOPIC_MAX_LEN];
static char topic_alarm[APP_TOPIC_MAX_LEN];
static char topic_cmd[APP_TOPIC_MAX_LEN];

static struct mqtt_topic sub_topics[1];

static cloud_cmd_cb_t   user_cmd_cb;
static cloud_state_cb_t user_state_cb;

static volatile bool     connected;
static volatile uint16_t awaited_msg_id;
static volatile bool     puback_ok;
static uint16_t          next_msg_id = 1U;

/* ------------------------------------------------------------------ */

static uint16_t alloc_msg_id(void)
{
	/* MQTT message id 0 is reserved. */
	if (next_msg_id == 0U) {
		next_msg_id = 1U;
	}
	return next_msg_id++;
}

static void aws_evt_handler(const struct aws_iot_evt *const evt)
{
	switch (evt->type) {
	case AWS_IOT_EVT_CONNECTING:
		LOG_INF("connecting to AWS IoT");
		break;

	case AWS_IOT_EVT_CONNECTED:
		LOG_INF("connected to AWS IoT");
		connected = true;
		k_sem_give(&connected_sem);
		if (user_state_cb) {
			user_state_cb(true);
		}
		break;

	case AWS_IOT_EVT_DISCONNECTED:
		LOG_WRN("disconnected from AWS IoT");
		connected = false;
		/* Release any publisher blocked on a PUBACK that will now
		 * never arrive, so the caller can retry from flash.
		 */
		puback_ok = false;
		k_sem_give(&puback_sem);
		if (user_state_cb) {
			user_state_cb(false);
		}
		break;

	case AWS_IOT_EVT_DATA_RECEIVED:
		LOG_INF("downlink %u bytes", (unsigned int)evt->data.msg.len);
		if (user_cmd_cb) {
			user_cmd_cb(evt->data.msg.topic.str,
				    evt->data.msg.topic.len,
				    evt->data.msg.ptr,
				    evt->data.msg.len);
		}
		break;

	case AWS_IOT_EVT_PUBACK:
		LOG_DBG("PUBACK id %u", evt->data.message_id);
		if (evt->data.message_id == awaited_msg_id) {
			puback_ok = true;
			k_sem_give(&puback_sem);
		}
		break;

	case AWS_IOT_EVT_ERROR:
		LOG_ERR("AWS IoT error %d", evt->data.err);
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */

int cloud_client_init(cloud_cmd_cb_t cmd_cb, cloud_state_cb_t state_cb)
{
	const char *id = CONFIG_AWS_IOT_CLIENT_ID_STATIC;
	int err;

	user_cmd_cb   = cmd_cb;
	user_state_cb = state_cb;

	if (snprintf(topic_telemetry, sizeof(topic_telemetry),
		     APP_TOPIC_TELEMETRY_FMT, id) >= (int)sizeof(topic_telemetry) ||
	    snprintf(topic_alarm, sizeof(topic_alarm),
		     APP_TOPIC_ALARM_FMT, id) >= (int)sizeof(topic_alarm) ||
	    snprintf(topic_cmd, sizeof(topic_cmd),
		     APP_TOPIC_CMD_FMT, id) >= (int)sizeof(topic_cmd)) {
		LOG_ERR("topic string truncated - increase APP_TOPIC_MAX_LEN");
		return -ENAMETOOLONG;
	}

	LOG_INF("thing '%s'", id);
	LOG_INF("  telemetry -> %s", topic_telemetry);
	LOG_INF("  alarm     -> %s", topic_alarm);
	LOG_INF("  command   <- %s", topic_cmd);

	/* NCS >= 2.7: the event handler is the only argument. */
	err = aws_iot_init(aws_evt_handler);
	if (err) {
		LOG_ERR("aws_iot_init failed (%d)", err);
		return err;
	}

	sub_topics[0].topic.utf8 = (const uint8_t *)topic_cmd;
	sub_topics[0].topic.size = strlen(topic_cmd);
	sub_topics[0].qos        = MQTT_QOS_1_AT_LEAST_ONCE;

	err = aws_iot_application_topics_set(sub_topics, ARRAY_SIZE(sub_topics));
	if (err) {
		LOG_ERR("aws_iot_application_topics_set failed (%d)", err);
		return err;
	}

	return 0;
}

int cloud_client_connect(uint32_t timeout_s)
{
	struct aws_iot_config cfg = { 0 };
	int err;

	k_sem_reset(&connected_sem);

	err = aws_iot_connect(&cfg);
	if (err) {
		LOG_ERR("aws_iot_connect failed (%d)", err);
		return err;
	}

	if (k_sem_take(&connected_sem, K_SECONDS(timeout_s)) != 0) {
		LOG_ERR("timed out waiting for AWS IoT connection");
		return -ETIMEDOUT;
	}

	return 0;
}

int cloud_client_disconnect(void)
{
	connected = false;
	return aws_iot_disconnect();
}

bool cloud_client_is_connected(void)
{
	return connected;
}

/* ------------------------------------------------------------------ *
 * Confirmed publish.
 *
 * Sends at QoS 1 and blocks until the broker's PUBACK for this exact
 * message id arrives. Only then may the caller release the records from
 * the flash audit buffer.
 * ------------------------------------------------------------------ */
static int publish_confirmed(const char *topic, const void *data, size_t len,
			     uint32_t timeout_s)
{
	struct aws_iot_data msg = { 0 };
	uint16_t id;
	int err;

	if (data == NULL || len == 0U) {
		return -EINVAL;
	}
	if (!connected) {
		return -ENOTCONN;
	}

	id = alloc_msg_id();

	msg.ptr        = data;
	msg.len        = len;
	msg.qos        = MQTT_QOS_1_AT_LEAST_ONCE;
	msg.message_id = id;
	msg.topic.str  = topic;
	msg.topic.len  = strlen(topic);

	awaited_msg_id = id;
	puback_ok      = false;
	k_sem_reset(&puback_sem);

	err = aws_iot_send(&msg);
	if (err) {
		LOG_ERR("aws_iot_send failed (%d)", err);
		awaited_msg_id = 0U;
		return err;
	}

	if (k_sem_take(&puback_sem, K_SECONDS(timeout_s)) != 0) {
		LOG_WRN("no PUBACK for id %u within %u s", id, timeout_s);
		awaited_msg_id = 0U;
		return -ETIMEDOUT;
	}

	awaited_msg_id = 0U;

	if (!puback_ok) {
		LOG_WRN("publish id %u aborted (link dropped)", id);
		return -ECONNRESET;
	}

	LOG_INF("published %u bytes to %s (confirmed)",
		(unsigned int)len, topic);
	return 0;
}

int cloud_client_publish_telemetry(const void *data, size_t len,
				   uint32_t timeout_s)
{
	return publish_confirmed(topic_telemetry, data, len, timeout_s);
}

int cloud_client_publish_alarm(const void *data, size_t len, uint32_t timeout_s)
{
	return publish_confirmed(topic_alarm, data, len, timeout_s);
}

int cloud_client_publish_shadow(const void *data, size_t len)
{
	struct aws_iot_data msg = { 0 };

	if (!connected) {
		return -ENOTCONN;
	}

	msg.ptr        = data;
	msg.len        = len;
	msg.qos        = MQTT_QOS_0_AT_MOST_ONCE;
	msg.topic.type = AWS_IOT_SHADOW_TOPIC_UPDATE;

	return aws_iot_send(&msg);
}

const char *cloud_client_id(void)
{
	return CONFIG_AWS_IOT_CLIENT_ID_STATIC;
}
