// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <zephyr/drivers/uart.h>
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
static uint8_t payload[64];
static size_t pending;
static int complete;

static void uart_callback(const struct device *dev, void *context)
{
	uart_irq_update(dev);
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
	return NULL;
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
