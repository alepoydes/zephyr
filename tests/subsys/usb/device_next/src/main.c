/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbh.h>

#include "usbh_ch9.h"
#include "usbh_device.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_test, LOG_LEVEL_INF);

#define TEST_DEFAULT_INTERFACE		0
#define TEST_DEFAULT_ALTERNATE		1

USBD_CONFIGURATION_DEFINE(test_fs_config,
			  USB_SCD_SELF_POWERED | USB_SCD_REMOTE_WAKEUP,
			  200, NULL);

USBD_CONFIGURATION_DEFINE(test_hs_config,
			  USB_SCD_SELF_POWERED | USB_SCD_REMOTE_WAKEUP,
			  200, NULL);


USBD_DESC_LANG_DEFINE(test_lang);
USBD_DESC_STRING_DEFINE(test_mfg, "ZEPHYR", 1);
USBD_DESC_STRING_DEFINE(test_product, "Zephyr USB Test", 2);
USBD_DESC_STRING_DEFINE(test_sn, "0123456789ABCDEF", 3);

USBD_DEVICE_DEFINE(test_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x2fe3, 0xffff);

USBH_CONTROLLER_DEFINE(uhs_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));

K_THREAD_STACK_DEFINE(disable_stack, 2048);
static struct k_thread disable_thread;
K_SEM_DEFINE(callback_done, 0, 1);
static int disable_result;
static atomic_t callback_active;
static atomic_t worker_started_in_callback;
static atomic_t disable_finished_in_callback;

static void disable_device(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (atomic_get(&callback_active)) {
		atomic_set(&worker_started_in_callback, 1);
	}
	disable_result = usbd_disable(&test_usbd);
	if (atomic_get(&callback_active)) {
		atomic_set(&disable_finished_in_callback, 1);
	}
}

static int vendor_disable_request(const struct usbd_context *const ctx,
				  const struct usb_setup_packet *const setup,
				  const struct net_buf *const buf)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(setup);
	ARG_UNUSED(buf);

	atomic_set(&callback_active, 1);
	k_thread_create(&disable_thread, disable_stack,
			K_THREAD_STACK_SIZEOF(disable_stack), disable_device,
			NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	atomic_clear(&callback_active);
	k_sem_give(&callback_done);

	return 0;
}

USBD_VREQUEST_DEFINE(test_disable_vreq, 0x7a, NULL, vendor_disable_request);

static int test_cmp_string_desc(struct net_buf *const buf, const int idx)
{
	static struct usbd_desc_node *desc_nd;
	size_t len;

	if (idx == test_mfg.str.idx) {
		desc_nd = &test_mfg;
	} else if (idx == test_product.str.idx) {
		desc_nd = &test_product;
	} else if (idx == test_sn.str.idx) {
		desc_nd = &test_sn;
	} else {
		return -ENOTSUP;
	}

	if (net_buf_pull_u8(buf) != desc_nd->bLength) {
		return -EINVAL;
	}

	if (net_buf_pull_u8(buf) != USB_DESC_STRING) {
		return -EINVAL;
	}

	LOG_HEXDUMP_DBG(buf->data, buf->len, "");
	len = MIN(buf->len / 2, desc_nd->bLength / 2);
	for (size_t i = 0; i < len; i++) {
		uint16_t a = net_buf_pull_le16(buf);
		uint16_t b = ((uint8_t *)(desc_nd->ptr))[i];

		if (a != b) {
			LOG_INF("%c != %c", a, b);
			return -EINVAL;
		}
	}

	return 0;
}

ZTEST(device_next, test_get_desc_string)
{
	const uint8_t type = USB_DESC_STRING;
	const uint16_t id = 0x0409;
	static struct usb_device *udev;
	struct net_buf *buf;
	int err;

	udev = usbh_device_get_any(&uhs_ctx);
	zassert_not_null(udev, "No USB device available");

	buf = usbh_xfer_buf_alloc(udev, UINT8_MAX);
	zassert_not_null(udev, "Failed to allocate buffer");

	err = k_mutex_lock(&udev->mutex, K_MSEC(200));
	zassert_equal(err, 0, "Failed to lock device");

	err = usbh_req_desc(udev, type, 1, id, UINT8_MAX, buf);
	zassert_equal(err, 0, "Transfer status is an error");
	err = test_cmp_string_desc(buf, 1);
	zassert_equal(err, 0, "Descriptor comparison failed");

	net_buf_reset(buf);
	err = usbh_req_desc(udev, type, 2, id, UINT8_MAX, buf);
	zassert_equal(err, 0, "Transfer status is an error");
	err = test_cmp_string_desc(buf, 2);
	zassert_equal(err, 0, "Descriptor comparison failed");

	net_buf_reset(buf);
	err = usbh_req_desc(udev, type, 3, id, UINT8_MAX, buf);
	zassert_equal(err, 0, "Transfer status is an error");
	err = test_cmp_string_desc(buf, 3);
	zassert_equal(err, 0, "Descriptor comparison failed");

	k_mutex_unlock(&udev->mutex);
	usbh_xfer_buf_free(udev, buf);
}

ZTEST(device_next, test_vendor_control_in)
{
	const uint8_t bmRequestType = (USB_REQTYPE_DIR_TO_HOST << 7) |
				      (USB_REQTYPE_TYPE_VENDOR << 5);
	static struct usb_device *udev;
	const uint8_t bRequest = 0x5c;
	const uint16_t wLength = 64;
	struct net_buf *buf;
	int err;

	if (!IS_ENABLED(CONFIG_UHC_VIRTUAL)) {
		LOG_WRN("The test was skipped, controller is not supported.");
		return;
	}

	udev = usbh_device_get_any(&uhs_ctx);
	zassert_not_null(udev, "No USB device available");

	buf = usbh_xfer_buf_alloc(udev, wLength);
	zassert_not_null(udev, "Failed to allocate buffer");

	err = k_mutex_lock(&udev->mutex, K_MSEC(200));
	zassert_equal(err, 0, "Failed to lock device");

	/* Perform regular vendor IN transfer */
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT; i++) {
		net_buf_reset(buf);
		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, buf);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	/* Perform vendor IN transfer but omit status stage*/
	usbh_req_omit_status(true);
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT * 2; i++) {
		net_buf_reset(buf);
		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, buf);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	/* Perform vendor IN requests but omit data and status stage*/
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT * 2; i++) {
		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, NULL);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	usbh_req_omit_status(false);

	/* Perform regular vendor IN transfer again */
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT; i++) {
		net_buf_reset(buf);
		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, buf);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	k_mutex_unlock(&udev->mutex);
	usbh_xfer_buf_free(udev, buf);
}

ZTEST(device_next, test_vendor_control_out)
{
	const uint8_t bmRequestType = (USB_REQTYPE_DIR_TO_DEVICE << 7) |
				      (USB_REQTYPE_TYPE_VENDOR << 5);
	const uint8_t bRequest = 0x5b;
	static struct usb_device *udev;
	const uint16_t wLength = 64;
	struct net_buf *buf;
	int err;

	if (!IS_ENABLED(CONFIG_UHC_VIRTUAL)) {
		LOG_WRN("The test was skipped, controller is not supported.");
		return;
	}

	udev = usbh_device_get_any(&uhs_ctx);
	zassert_not_null(udev, "No USB device available");

	buf = usbh_xfer_buf_alloc(udev, wLength);
	zassert_not_null(udev, "Failed to allocate buffer");

	err = k_mutex_lock(&udev->mutex, K_MSEC(200));
	zassert_equal(err, 0, "Failed to lock device");

	/* Perform regular vendor OUT transfer */
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT; i++) {
		net_buf_reset(buf);
		for (uint32_t n = 0; n < wLength; n++) {
			net_buf_add_u8(buf, n);
		}

		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, buf);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	/* Perform vendor OUT transfer but omit status stage*/
	usbh_req_omit_status(true);
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT * 2; i++) {
		net_buf_reset(buf);
		for (uint32_t n = 0; n < wLength; n++) {
			net_buf_add_u8(buf, n);
		}

		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, buf);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	/* Perform vendor OUT requests but omit data and status stage*/
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT * 2; i++) {
		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, NULL);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	usbh_req_omit_status(false);

	/* Perform regular vendor OUT transfer again */
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT; i++) {
		net_buf_reset(buf);
		for (uint32_t n = 0; n < wLength; n++) {
			net_buf_add_u8(buf, n);
		}

		err = usbh_req_setup(udev, bmRequestType, bRequest, 0, 0, wLength, buf);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	k_mutex_unlock(&udev->mutex);
	usbh_xfer_buf_free(udev, buf);
}

ZTEST(device_next, test_control_nodata)
{
	const uint8_t bmRequestType = USB_REQTYPE_RECIPIENT_ENDPOINT;
	const uint8_t bRequest = USB_SREQ_CLEAR_FEATURE;
	const uint16_t wValue = USB_SFS_ENDPOINT_HALT;
	const uint16_t wIndex = USB_CONTROL_EP_OUT;
	static struct usb_device *udev;
	int err;

	if (!IS_ENABLED(CONFIG_UHC_VIRTUAL)) {
		LOG_WRN("The test was skipped, controller is not supported.");
		return;
	}

	udev = usbh_device_get_any(&uhs_ctx);
	zassert_not_null(udev, "No USB device available");

	err = k_mutex_lock(&udev->mutex, K_MSEC(200));
	zassert_equal(err, 0, "Failed to lock device");

	/* Perform regular control transfer */
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT; i++) {
		err = usbh_req_setup(udev,
				     bmRequestType, bRequest, wValue, wIndex, 0,
				     NULL);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	/* Perform transfer but omit status stage*/
	usbh_req_omit_status(true);
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT * 2; i++) {
		err = usbh_req_setup(udev,
				     bmRequestType, bRequest, wValue, wIndex, 0,
				     NULL);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	usbh_req_omit_status(false);

	/* Perform regular control transfer again */
	for (uint32_t i = 0; i < CONFIG_UDC_BUF_COUNT; i++) {
		err = usbh_req_setup(udev,
				     bmRequestType, bRequest, wValue, wIndex, 0,
				     NULL);
		zassert_equal(err, 0, "Transfer status is an error");
	}

	k_mutex_unlock(&udev->mutex);
}

/* Get Configuration request test */
ZTEST(device_next, test_get_configuration)
{
	struct usb_device *udev;
	uint8_t cfg = 0;
	int err;

	udev = usbh_device_get_any(&uhs_ctx);
	zassert_not_null(udev, "No USB device available");

	err = k_mutex_lock(&udev->mutex, K_MSEC(200));
	zassert_equal(err, 0, "Failed to lock device");

	err = usbh_req_get_cfg(udev, &cfg);
	k_mutex_unlock(&udev->mutex);

	switch (udev->state) {
	case USB_STATE_DEFAULT:
		/* Not specified, expect protocol error */
		zassert_equal(err, -EPIPE, "Transfer status is not a protocol error");
		break;
	case USB_STATE_ADDRESSED:
		/* TODO: Expect zero value */
		zassert_equal(err, 0, "Transfer status is an error");
		zassert_equal(cfg, 0, "Device not in address state");
		break;
	case USB_STATE_CONFIGURED:
		/* TODO: Expect non-zero valid configuration value */
		zassert_equal(err, 0, "Transfer status is an error");
		zassert_not_equal(cfg, 0, "Device not in configured state");
		break;
	default:
		break;
	}
}

/* Set Interface request test */
ZTEST(device_next, test_set_interface)
{
	struct usb_device *udev;
	int err;

	udev = usbh_device_get_any(&uhs_ctx);
	zassert_not_null(udev, "No USB device available");

	err = k_mutex_lock(&udev->mutex, K_MSEC(200));
	zassert_equal(err, 0, "Failed to lock device");

	err = usbh_req_set_alt(udev, TEST_DEFAULT_INTERFACE,
			       TEST_DEFAULT_ALTERNATE);
	k_mutex_unlock(&udev->mutex);

	switch (udev->state) {
	case USB_STATE_DEFAULT:
		/* Not specified, expect protocol error */
	case USB_STATE_ADDRESSED:
		/* Expect protocol error */
		zassert_equal(err, -EPIPE, "Transfer status is not a protocol error");
		break;
	case USB_STATE_CONFIGURED:
		/* TODO */
	default:
		break;
	}
}

ZTEST(device_next, test_z_vendor_request_serializes_disable)
{
	const uint8_t request_type = (USB_REQTYPE_TYPE_VENDOR << 5) |
				     USB_REQTYPE_RECIPIENT_DEVICE;
	struct usb_device *udev = usbh_device_get_any(&uhs_ctx);
	int err;

	zassert_not_null(udev, "No USB device available");
	k_sem_reset(&callback_done);
	atomic_clear(&callback_active);
	atomic_clear(&worker_started_in_callback);
	atomic_clear(&disable_finished_in_callback);
	disable_result = -EAGAIN;

	err = k_mutex_lock(&udev->mutex, K_MSEC(200));
	zassert_equal(err, 0, "Failed to lock host device");
	/* The device is deliberately detached before the control status stage. */
	usbh_req_omit_status(true);
	(void)usbh_req_setup(udev, request_type, 0x7a, 0, 0, 0, NULL);
	usbh_req_omit_status(false);
	k_mutex_unlock(&udev->mutex);

	err = k_sem_take(&callback_done, K_SECONDS(1));
	zassert_equal(err, 0, "Vendor request was not dispatched");
	err = k_thread_join(&disable_thread, K_SECONDS(1));
	zassert_equal(err, 0, "Disable worker did not finish");

	err = usbd_enable(&test_usbd);
	zassert_equal(err, 0, "Failed to re-enable device");
	/* As at suite startup, let the virtual host finish re-enumeration. */
	k_msleep(200);
	zassert_equal(disable_result, 0, "Failed to disable device");
	if (IS_ENABLED(CONFIG_USBD_THREAD_PREEMPTIBLE)) {
		zassert_equal(atomic_get(&worker_started_in_callback), 1,
			      "Disable worker did not preempt vendor callback");
	}
	zassert_equal(atomic_get(&disable_finished_in_callback), 0,
		      "Disable completed while vendor callback was active");
}

static void *usb_test_enable(void)
{
	int err;

	err = usbh_init(&uhs_ctx);
	zassert_equal(err, 0, "Failed to initialize USB host");

	err = usbh_enable(&uhs_ctx);
	zassert_equal(err, 0, "Failed to enable USB host");

	err = uhc_bus_reset(uhs_ctx.dev);
	zassert_equal(err, 0, "Failed to signal bus reset");

	err = uhc_bus_resume(uhs_ctx.dev);
	zassert_equal(err, 0, "Failed to signal bus resume");

	err = uhc_sof_enable(uhs_ctx.dev);
	zassert_equal(err, 0, "Failed to enable SoF generator");

	LOG_INF("Host controller enabled");

	err = usbd_add_descriptor(&test_usbd, &test_lang);
	zassert_equal(err, 0, "Failed to initialize descriptor (%d)", err);

	err = usbd_add_descriptor(&test_usbd, &test_mfg);
	zassert_equal(err, 0, "Failed to initialize descriptor (%d)", err);

	err = usbd_add_descriptor(&test_usbd, &test_product);
	zassert_equal(err, 0, "Failed to initialize descriptor (%d)", err);

	err = usbd_add_descriptor(&test_usbd, &test_sn);
	zassert_equal(err, 0, "Failed to initialize descriptor (%d)", err);

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&test_usbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&test_usbd, USBD_SPEED_HS, &test_hs_config);
		zassert_equal(err, 0, "Failed to add configuration (%d)", err);
	}

	err = usbd_add_configuration(&test_usbd, USBD_SPEED_FS, &test_fs_config);
	zassert_equal(err, 0, "Failed to add configuration (%d)", err);

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&test_usbd) == USBD_SPEED_HS) {
		err = usbd_register_all_classes(&test_usbd, USBD_SPEED_HS, 1, NULL);
		zassert_equal(err, 0, "Failed to unregister all instances(%d)", err);

		err = usbd_unregister_all_classes(&test_usbd, USBD_SPEED_HS, 1);
		zassert_equal(err, 0, "Failed to unregister all instances(%d)", err);

		err = usbd_register_class(&test_usbd, "loopback_0", USBD_SPEED_HS, 1);
		zassert_equal(err, 0, "Failed to register loopback_0 class (%d)", err);
	}

	err = usbd_register_all_classes(&test_usbd, USBD_SPEED_FS, 1, NULL);
	zassert_equal(err, 0, "Failed to unregister all instances(%d)", err);

	err = usbd_unregister_all_classes(&test_usbd, USBD_SPEED_FS, 1);
	zassert_equal(err, 0, "Failed to unregister all instances(%d)", err);

	err = usbd_register_class(&test_usbd, "loopback_0", USBD_SPEED_FS, 1);
	zassert_equal(err, 0, "Failed to register loopback_0 class (%d)", err);

	err = usbd_device_register_vreq(&test_usbd, &test_disable_vreq);
	zassert_equal(err, 0, "Failed to register vendor request (%d)", err);

	err = usbd_init(&test_usbd);
	zassert_equal(err, 0, "Failed to initialize device support");

	err = usbd_enable(&test_usbd);
	zassert_equal(err, 0, "Failed to enable device support");

	LOG_INF("Device support enabled");

	/* Allow the host time to reset the device. */
	k_msleep(200);

	return NULL;
}

static void usb_test_shutdown(void *f)
{
	int err;

	err = usbd_disable(&test_usbd);
	zassert_equal(err, 0, "Failed to enable device support");

	err = usbd_shutdown(&test_usbd);
	zassert_equal(err, 0, "Failed to shutdown device support");

	LOG_INF("Device support disabled");

	err = usbh_disable(&uhs_ctx);
	zassert_equal(err, 0, "Failed to disable USB host");

	LOG_INF("Host controller disabled");
}

ZTEST_SUITE(device_next, NULL, usb_test_enable, NULL, NULL, usb_test_shutdown);
