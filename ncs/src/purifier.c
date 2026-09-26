#include "purifier.h"
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(purifier, LOG_LEVEL_INF);
#define STALE_MS 35000
#define REPLY_MS 15000
static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
RING_BUF_DECLARE(rx, 256);
K_MUTEX_DEFINE(lock);
K_MSGQ_DEFINE(tx, AOIA_TX_SIZE, 1, 1);
static atomic_t rx_overflow;
static struct aoia_parser parser;
static struct purifier_snapshot current;
static int64_t last_rx, sent_at;
static bool sent;

static void receive_irq(const struct device *dev, void *unused)
{
    ARG_UNUSED(unused);
    uint8_t bytes[16];
    if (!uart_irq_update(dev)) { return; }
    while (uart_irq_rx_ready(dev)) {
        int n = uart_fifo_read(dev, bytes, sizeof(bytes));
        if (n <= 0) { break; }
        /* Always drain the FIFO, even on overflow, to avoid an interrupt storm. */
        if (ring_buf_put(&rx, bytes, n) != n) { atomic_set(&rx_overflow, 1); }
    }
}

int purifier_init(void)
{
    if (!device_is_ready(uart)) { return -ENODEV; }
    const struct uart_config config = {
        .baudrate = 9600, .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1, .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int err = uart_configure(uart, &config);
    /* UART0's nRF UARTE driver may return ENOTSUP for runtime reconfiguration.
     * The board DTS already applies this exact 9600 8N1/no-flow-control setup
     * before main; do not prevent Zigbee commissioning in that case. */
    if (err && err != -ENOTSUP) { return err; }
    if (err == -ENOTSUP) { LOG_WRN("UART runtime config unsupported; using DTS 9600 8N1"); }
    err = uart_irq_callback_user_data_set(uart, receive_irq, NULL);
    if (err) { return err; }
    uart_irq_rx_enable(uart);
    LOG_INF("UART0 P0.08 TX/P0.06 RX, 9600 8N1; awaiting telemetry");
    return 0;
}

void purifier_snapshot(struct purifier_snapshot *out)
{
    k_mutex_lock(&lock, K_FOREVER);
    *out = current;
    /* Check age here too; never accept a command in the tick before expiry. */
    out->known &= k_uptime_get() - last_rx <= STALE_MS;
    k_mutex_unlock(&lock);
}

int purifier_request(enum aoia_control control, uint8_t value)
{
    uint8_t frame[AOIA_TX_SIZE];
    if (k_mutex_lock(&lock, K_NO_WAIT)) { return -EBUSY; }
    int err = 0;
    if (!current.known || k_uptime_get() - last_rx > STALE_MS) { err = -ENODATA; }
    else if (current.pending) { err = -EBUSY; }
    else if (!aoia_make_command(&current.state, control, value, frame)) { err = -EINVAL; }
    else {
        err = k_msgq_put(&tx, frame, K_NO_WAIT);
        if (!err) { current.pending = true; }
    }
    k_mutex_unlock(&lock);
    return err;
}

void purifier_process(void)
{
    uint8_t byte, frame[AOIA_TX_SIZE];
    if (atomic_cas(&rx_overflow, 1, 0)) {
        /* Drop the partial stream under IRQ exclusion, never stitch over a gap. */
        unsigned key = irq_lock();
        ring_buf_reset(&rx);
        irq_unlock(key);
        parser.used = 0;
        LOG_WRN("RX overflow; resynchronizing");
    }
    while (ring_buf_get(&rx, &byte, 1) == 1) {
        struct aoia_state state;
        if (!aoia_parse_byte(&parser, byte, &state)) { continue; }
        k_mutex_lock(&lock, K_FOREVER);
        current.state = state;
        current.known = true;
        ++current.revision;
        last_rx = k_uptime_get();
        if (current.pending && sent) {
            current.pending = false;
            sent = false;
            ++current.completed;
        }
        k_mutex_unlock(&lock);
        LOG_INF("RX flags=%02x speed=%02x PM2.5=%u filter=%u%%",
                state.flags, state.speed, state.pm25, state.filter_percent);
    }
    if (k_msgq_get(&tx, frame, K_NO_WAIT) == 0) {
        /* UART TX is deliberately outside the ZBOSS thread (12.5 ms at 9600).
         * Drain pre-command RX above before considering a reply confirmation. */
        for (size_t i = 0; i < sizeof(frame); ++i) { uart_poll_out(uart, frame[i]); }
        sent_at = k_uptime_get();
        sent = true;
        LOG_INF("TX flags=%02x speed=%02x", frame[2], frame[3]);
    }
    k_mutex_lock(&lock, K_FOREVER);
    if (current.pending && sent && k_uptime_get() - sent_at > REPLY_MS) {
        current.pending = false;
        sent = false;
        ++current.completed;
        LOG_WRN("No command reply; retaining last confirmed state, no retry");
    }
    if (current.known && k_uptime_get() - last_rx > STALE_MS) {
        current.known = false;
        ++current.revision;
        parser.used = 0;
        LOG_WRN("Purifier telemetry stale; controls inhibited");
    }
    k_mutex_unlock(&lock);
}
