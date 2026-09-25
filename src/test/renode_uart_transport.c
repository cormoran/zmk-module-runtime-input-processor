/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Renode's nRF52840 USB model cannot bring up a usable USB endpoint. This is
 * the Studio RPC UART transport, registered as ZMK_TRANSPORT_NONE so it is
 * selected by an emulated board with USB and BLE disabled.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#define UART_DEVICE_NODE DT_CHOSEN(zmk_studio_rpc_uart)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static void tx_notify(struct ring_buf *tx_ring_buf, size_t written, bool msg_done,
                      void *user_data) {
    if (msg_done || ring_buf_size_get(tx_ring_buf) > ring_buf_capacity_get(tx_ring_buf) / 2) {
        uart_irq_tx_enable(uart_dev);
    }
}

static int start_rx(void) {
    uart_irq_rx_enable(uart_dev);
    return 0;
}

static int stop_rx(void) {
    uart_irq_rx_disable(uart_dev);
    return 0;
}

ZMK_RPC_TRANSPORT(renode_uart, ZMK_TRANSPORT_NONE, start_rx, stop_rx, NULL, tx_notify);

static void serial_cb(const struct device *dev, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    if (!uart_irq_update(uart_dev)) {
        return;
    }

    if (uart_irq_rx_ready(uart_dev)) {
        uint32_t last_read = 0;
        uint32_t len = 0;
        struct ring_buf *buf = zmk_rpc_get_rx_buf();

        do {
            uint8_t *buffer;
            len = ring_buf_put_claim(buf, &buffer, buf->size);
            if (len > 0) {
                last_read = uart_fifo_read(uart_dev, buffer, len);
                ring_buf_put_finish(buf, last_read);
            } else {
                LOG_ERR("Dropping incoming RPC byte, insufficient room in the RX buffer");
                uint8_t dummy;
                last_read = uart_fifo_read(uart_dev, &dummy, 1);
            }
        } while (last_read && last_read == len);

        zmk_rpc_rx_notify();
    }

    if (uart_irq_tx_ready(uart_dev)) {
        struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
        uint32_t len;

        while ((len = ring_buf_size_get(tx_buf)) > 0) {
            uint8_t *buf;
            uint32_t claim_len = ring_buf_get_claim(tx_buf, &buf, tx_buf->size);

            if (claim_len == 0) {
                continue;
            }

            int sent = uart_fifo_fill(uart_dev, buf, claim_len);
            ring_buf_get_finish(tx_buf, MAX(sent, 0));
        }

        /* Renode keeps TX-ready asserted when idle; avoid an IRQ storm. */
        if (ring_buf_size_get(tx_buf) == 0) {
            uart_irq_tx_disable(uart_dev);
        }
    }
}

static int renode_uart_rpc_interface_init(void) {
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not found");
        return -ENODEV;
    }

    int ret = uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
    if (ret < 0) {
        LOG_ERR("Unable to set UART callback: %d", ret);
    }

    return ret;
}

SYS_INIT(renode_uart_rpc_interface_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
