// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/ztest.h>
#include "usbh_device.h"
#include "usbh_desc.h"

USBH_CONTROLLER_DEFINE(host, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));
static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(test_uart));
static struct usb_device *device;
K_SEM_DEFINE(probed, 0, 1);
K_SEM_DEFINE(transferred, 0, 1);
K_SEM_DEFINE(received, 0, 2);
K_SEM_DEFINE(rx_armed, 0, 2);
K_SEM_DEFINE(rx_failed, 0, 1);
static uint8_t payload[64];
static size_t pending;
static int complete;
static uint8_t last_received;
static atomic_t fail_rx_alloc;
static atomic_t fail_rx_enqueue;
static atomic_t rx_accepted;

struct net_buf *__real_net_buf_alloc_fixed(struct net_buf_pool *pool, k_timeout_t timeout);
int __real_usbd_ep_enqueue(const struct usbd_class_data *c_data, struct net_buf *buf);

struct net_buf *__wrap_net_buf_alloc_fixed(struct net_buf_pool *pool, k_timeout_t timeout)
{
	/* The CDC endpoint pool is the only two-buffer pool of UDC requests here. */
	if (pool->buf_count == 2 && pool->user_data_size == sizeof(struct udc_buf_info) &&
	    atomic_set(&fail_rx_alloc, 0)) {
		k_sem_give(&rx_failed);
		return NULL;
	}

	return __real_net_buf_alloc_fixed(pool, timeout);
}

int __wrap_usbd_ep_enqueue(const struct usbd_class_data *c_data, struct net_buf *buf)
{
	const uint8_t ep = udc_get_buf_info(buf)->ep;

	if ((ep & USB_EP_DIR_IN) == 0 && atomic_set(&fail_rx_enqueue, 0)) {
		k_sem_give(&rx_failed);
		return -EIO;
	}

	int ret = __real_usbd_ep_enqueue(c_data, buf);

	if (ret == 0 && (ep & USB_EP_DIR_IN) == 0) {
		atomic_inc(&rx_accepted);
		k_sem_give(&rx_armed);
	}

	return ret;
}

static void uart_callback(const struct device *dev, void *context)
{
	uart_irq_update(dev);
	if (uart_irq_rx_ready(dev)) {
		uint8_t value;

		if (uart_fifo_read(dev, &value, 1) == 1) {
			last_received = value;
			k_sem_give(&received);
		}
	}
	if (uart_irq_tx_ready(dev)) {
		if (pending) {
			int written = uart_fifo_fill(dev, payload, pending);

			zassert_equal(written, pending);
			pending = 0;
		}
		complete = uart_irq_tx_complete(dev);
		uart_irq_tx_disable(dev);
		k_sem_give(&probed);
	}
}

static int probe(size_t length)
{
	pending = length;
	k_sem_reset(&probed);
	uart_irq_tx_enable(uart);
	zassert_ok(k_sem_take(&probed, K_SECONDS(1)));
	return complete;
}

static int transfer_done(struct usb_device *udev, struct uhc_transfer *xfer)
{
	k_sem_give(&transferred);
	return 0;
}

static void *setup(void)
{
	memset(payload, 0xa5, sizeof(payload));
	zassert_ok(usbh_init(&host));
	zassert_ok(usbh_enable(&host));
	zassert_ok(uhc_bus_reset(host.dev));
	zassert_ok(uhc_bus_resume(host.dev));
	zassert_ok(uhc_sof_enable(host.dev));
	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		zassert_ok(usbd_enable(ctx));
	}
	int64_t deadline = k_uptime_get() + 1000;

	do {
		device = usbh_device_get_any(&host);
		if (device && device->state == USB_STATE_CONFIGURED) {
			break;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);
	zassert_not_null(device);
	zassert_equal(device->state, USB_STATE_CONFIGURED);
	zassert_ok(uart_irq_callback_user_data_set(uart, uart_callback, NULL));
	uart_irq_rx_enable(uart);
	zassert_ok(k_sem_take(&rx_armed, K_SECONDS(1)));
	return NULL;
}

static void write_byte(uint8_t endpoint, uint8_t value)
{
	struct uhc_transfer *xfer = usbh_xfer_alloc(device, endpoint, transfer_done, NULL);
	struct net_buf *buf = usbh_xfer_buf_alloc(device, 1);

	zassert_not_null(xfer);
	zassert_not_null(buf);
	net_buf_add_u8(buf, value);
	zassert_ok(usbh_xfer_buf_add(device, xfer, buf));
	k_sem_reset(&transferred);
	zassert_ok(usbh_xfer_enqueue(device, xfer));
	zassert_ok(k_sem_take(&transferred, K_MSEC(250)), "Host OUT remained unarmed");
	zassert_ok(xfer->err);
	usbh_xfer_free(device, xfer);
	usbh_xfer_buf_free(device, buf);
	zassert_ok(k_sem_take(&received, K_MSEC(250)), "CDC UART did not receive host OUT");
	zassert_equal(last_received, value);
}

ZTEST(cdc, test_rx_rearms_after_allocation_and_enqueue_failure)
{
	const struct usb_ep_descriptor *endpoint = NULL;

	for (uint8_t ep = 1; ep < 0x10; ep++) {
		const struct usb_ep_descriptor *candidate = usbh_desc_get_endpoint(device, ep);

		if (candidate &&
		    (candidate->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) ==
		    USB_EP_TYPE_BULK) {
			endpoint = candidate;
			break;
		}
	}
	zassert_not_null(endpoint);

	for (int failure = 0; failure < 2; failure++) {
		/* The current OUT request completes; its replacement fails once. */
		k_sem_reset(&rx_failed);
		atomic_set(failure ? &fail_rx_enqueue : &fail_rx_alloc, 1);
		write_byte(endpoint->bEndpointAddress, (uint8_t)(0x41 + failure * 2));
		zassert_ok(k_sem_take(&rx_failed, K_MSEC(250)), "RX failure was not injected");
		zassert_equal(atomic_get(&fail_rx_alloc), 0);
		zassert_equal(atomic_get(&fail_rx_enqueue), 0);

		/* A public UART trigger must be able to replace the failed request. */
		int prior = atomic_get(&rx_accepted);

		uart_irq_rx_enable(uart);
		zassert_ok(k_sem_take(&rx_armed, K_MSEC(250)), "RX request was not rearmed");
		zassert_equal(atomic_get(&rx_accepted), prior + 1);
		write_byte(endpoint->bEndpointAddress, (uint8_t)(0x42 + failure * 2));
		zassert_ok(k_sem_take(&rx_armed, K_MSEC(250)), "RX did not remain armed");
	}
}

static void read_packet(uint8_t endpoint, size_t capacity, size_t expected)
{
	struct uhc_transfer *xfer = usbh_xfer_alloc(device, endpoint,
						 transfer_done, NULL);
	struct net_buf *buf = usbh_xfer_buf_alloc(device, capacity);

	zassert_not_null(xfer);
	zassert_not_null(buf);
	zassert_ok(usbh_xfer_buf_add(device, xfer, buf));
	zassert_ok(usbh_xfer_enqueue(device, xfer));
	zassert_ok(k_sem_take(&transferred, K_SECONDS(1)));
	zassert_ok(xfer->err);
	zassert_equal(buf->len, expected);
	if (expected) {
		zassert_mem_equal(buf->data, payload, expected);
	}
	usbh_xfer_free(device, xfer);
	usbh_xfer_buf_free(device, buf);
}

ZTEST(cdc, test_fifo_and_inflight_completion)
{
	const struct usb_ep_descriptor *endpoint = NULL;

	for (uint8_t ep = 0x81; ep < 0x90; ep++) {
		const struct usb_ep_descriptor *candidate =
			usbh_desc_get_endpoint(device, ep);

		if (candidate &&
		    (candidate->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) ==
		    USB_EP_TYPE_BULK) {
			endpoint = candidate;
			break;
		}
	}
	zassert_not_null(endpoint);
	zassert_equal(probe(0), 1);
	/* Short packet versus a full packet requiring a terminating ZLP. */
	for (size_t length = 13; length <= sizeof(payload); length += 51) {
		zassert_equal(probe(length), 0);
		/* Without a host read, draining the FIFO is not completion. */
		k_msleep(2);
		zassert_equal(probe(0), 0);
		read_packet(endpoint->bEndpointAddress, length, length);
		if (length == sizeof(payload)) {
			/* One full packet leaves the terminating ZLP unread. */
			zassert_equal(probe(0), 0);
			read_packet(endpoint->bEndpointAddress, 1, 0);
		}
		int64_t deadline = k_uptime_get() + 1000;

		while (!probe(0) && k_uptime_get() < deadline) {
			k_msleep(1);
		}
		zassert_equal(complete, 1);
	}
}

ZTEST_SUITE(cdc, NULL, setup, NULL, NULL, NULL);
