/*
 * Copyright Runtime.io 2018. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief Bluetooth transport for the mcumgr SMP protocol.
 * @ingroup mcumgr_transport_bt
 */

#ifndef ZEPHYR_INCLUDE_MGMT_SMP_BT_H_
#define ZEPHYR_INCLUDE_MGMT_SMP_BT_H_

/**
 * @brief This allows to use the MCUmgr SMP protocol over Bluetooth.
 * @defgroup mcumgr_transport_bt Bluetooth transport
 * @ingroup mcumgr_transport
 * @{
 */

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/types.h>
struct bt_conn;

#ifdef __cplusplus
extern "C" {
#endif

/** SMP service UUID value. */
#define SMP_BT_SVC_UUID_VAL \
	BT_UUID_128_ENCODE(0x8d53dc1d, 0x1db7, 0x4cd3, 0x868b, 0x8a527460aa84)

/** SMP service UUID. */
#define SMP_BT_SVC_UUID \
	BT_UUID_DECLARE_128(SMP_BT_SVC_UUID_VAL)

/** SMP characteristic UUID value. */
#define SMP_BT_CHR_UUID_VAL \
	BT_UUID_128_ENCODE(0xda2e7828, 0xfbce, 0x4e01, 0xae9e, 0x261174997c48)

/** SMP characteristic UUID
 *  Used for both requests and responses.
 */
#define SMP_BT_CHR_UUID \
	BT_UUID_DECLARE_128(SMP_BT_CHR_UUID_VAL)

/**
 * @brief Application admission and completion for Bluetooth SMP packets.
 *
 * Both callbacks run on the SMP processing thread. begin may acquire a shared
 * management/session resource and return false to discard an obsolete packet.
 * A successful begin has exactly one matching end, after all handlers, output
 * and packet cleanup, even if the peer disconnects. The connection reference
 * remains valid through end. These callbacks must not block the Bluetooth RX
 * thread waiting for packet completion; defer session release when necessary.
 */
struct smp_bt_request_callbacks {
	bool (*begin)(struct bt_conn *conn);
	void (*end)(struct bt_conn *conn, int result);
};

/**
 * @brief Install packet callbacks before enabling Bluetooth.
 *
 * Register once; the structure and its callbacks must remain valid for the
 * transport lifetime. Both callbacks are required. There is no unregister
 * operation while packets may be in flight.
 *
 * @return 0 on success, -EINVAL for an invalid pair, -EALREADY if registered.
 */
int smp_bt_register_request_callbacks(
	const struct smp_bt_request_callbacks *callbacks);

/**
 * @brief	Registers the SMP Bluetooth service. Should only be called if the Bluetooth
 *		transport has been unregistered by calling smp_bt_unregister().
 *
 * @return	0 on success; negative error code on failure.
 */
int smp_bt_register(void);

/**
 * @brief	Unregisters the SMP Bluetooth service.
 *
 * @return	0 on success; negative error code on failure.
 */
int smp_bt_unregister(void);

/**
 * @brief	Transmits an SMP command/response over the specified Bluetooth connection as a
 *		notification.
 *
 * @param conn	Connection object.
 * @param data	Pointer to SMP message.
 * @param len	data length.
 *
 * @return	0 in case of success or negative value in case of error.
 */
int smp_bt_notify(struct bt_conn *conn, const void *data, uint16_t len);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif
