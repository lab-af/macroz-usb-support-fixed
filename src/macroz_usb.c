/*
 * USB serial (CDC ACM) transport for MacroZ.
 *
 * Frames a request/response protocol over the raw byte stream so we can
 * reuse the exact same command handling that the BLE GATT service already
 * uses (see macroz_apply_command / macroz_read_config_chunk / macroz_get_status
 * in macroz_dynamic.c, declared in include/macroz/transport.h).
 *
 * Frame format (both directions), all little-endian:
 *   byte 0-1: sync bytes 0x5A 0x4D ("MZ")
 *   byte 2:   type
 *   byte 3-4: payload length (u16)
 *   byte ...: payload
 *   byte N:   checksum = XOR of every byte before it in the frame
 *
 * Host -> device types 1-5 reuse the existing macroz_command values
 * (BEGIN/CHUNK/COMMIT/RESET/SELECT_READ_CHUNK) unchanged.
 * Host -> device type 0x10 requests the config chunk at the offset most
 * recently selected via SELECT_READ_CHUNK.
 * Host -> device type 0x11 requests a status snapshot.
 *
 * Device -> host type 0x81 carries a struct macroz_status payload.
 * Device -> host type 0x82 carries up to MACROZ_READ_CHUNK_SIZE bytes of
 * config data.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/usb/usb_dc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include <macroz/protocol.h>
#include <macroz/transport.h>

LOG_MODULE_REGISTER(macroz_usb, CONFIG_MACROZ_LOG_LEVEL);

#define MZ_SYNC0 0x5A
#define MZ_SYNC1 0x4D
#define MZ_TYPE_STATUS 0x81
#define MZ_TYPE_CONFIG_CHUNK 0x82
#define MZ_TYPE_REQUEST_CHUNK 0x10
#define MZ_TYPE_REQUEST_STATUS 0x11

#define MZ_RX_BUF_SIZE 512
#define MZ_TX_BUF_SIZE (5 + MACROZ_READ_CHUNK_SIZE + 1)
#define MZ_FRAME_MAX_PAYLOAD MACROZ_READ_CHUNK_SIZE

static const struct device *const cdc_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

/* Simple byte accumulator for incoming frames; parsed in the UART ISR's
 * bottom-half work item so we never do heavy work in interrupt context. */
static uint8_t rx_buf[MZ_RX_BUF_SIZE];
static size_t rx_len;
static struct k_work rx_work;

static uint8_t frame_checksum(const uint8_t *frame, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum ^= frame[i];
    }
    return sum;
}

static bool usb_dtr_is_set(void)
{
    uint32_t dtr = 0;
    int ret = uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);
    if (ret != 0) {
        /* Some CDC implementations do not expose DTR. In that case the
         * serial endpoint can still be used, so don't block the transport. */
        return true;
    }
    return dtr != 0;
}

static void send_frame(uint8_t type, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t out[MZ_TX_BUF_SIZE];
    if (5 + payload_len + 1 > sizeof(out)) {
        LOG_WRN("Dropping oversized outgoing frame (type %u, len %u)", type, payload_len);
        return;
    }

    out[0] = MZ_SYNC0;
    out[1] = MZ_SYNC1;
    out[2] = type;
    out[3] = (uint8_t)(payload_len & 0xFF);
    out[4] = (uint8_t)(payload_len >> 8);
    if (payload_len > 0) {
        memcpy(&out[5], payload, payload_len);
    }

    size_t total = 5 + payload_len;
    out[total] = frame_checksum(out, total);
    total++;

    /* uart_fifo_fill() is an interrupt-context API. The old implementation
     * called it from rx_work_handler(), which is a thread context, so CDC ACM
     * could silently fail to transmit the response. Polling UART output is
     * explicitly supported by CDC ACM from thread context. */
    for (size_t i = 0; i < total; i++) {
        uart_poll_out(cdc_dev, out[i]);
    }
}

static void send_status_frame(void) {
    struct macroz_status snapshot;
    macroz_get_status(&snapshot);
    send_frame(MZ_TYPE_STATUS, (const uint8_t *)&snapshot, sizeof(snapshot));
}

static void handle_frame(uint8_t type, const uint8_t *payload, uint16_t len) {
    switch (type) {
    case MACROZ_COMMAND_BEGIN:
    case MACROZ_COMMAND_CHUNK:
    case MACROZ_COMMAND_COMMIT:
    case MACROZ_COMMAND_RESET:
    case MACROZ_COMMAND_SELECT_READ_CHUNK: {
        /* These commands expect the command byte itself as the first byte
         * of the payload passed to macroz_apply_command, matching the BLE
         * control characteristic's wire format exactly. */
        uint8_t command_buf[1 + MZ_FRAME_MAX_PAYLOAD];
        if (len + 1 > sizeof(command_buf)) {
            LOG_WRN("Command payload too large (%u bytes)", len);
            return;
        }
        command_buf[0] = type;
        if (len > 0) {
            memcpy(&command_buf[1], payload, len);
        }
        macroz_apply_command(command_buf, len + 1);
        send_status_frame();
        break;
    }

    case MZ_TYPE_REQUEST_CHUNK: {
        if (len != 2) {
            LOG_WRN("Malformed chunk request (len %u)", len);
            return;
        }
        uint16_t offset = (uint16_t)(payload[0] | (payload[1] << 8));
        uint8_t chunk[MACROZ_READ_CHUNK_SIZE];
        size_t chunk_size = macroz_read_config_chunk(offset, chunk, sizeof(chunk));
        send_frame(MZ_TYPE_CONFIG_CHUNK, chunk, (uint16_t)chunk_size);
        break;
    }

    case MZ_TYPE_REQUEST_STATUS:
        send_status_frame();
        break;

    default:
        LOG_WRN("Unknown MacroZ USB frame type 0x%02x", type);
        break;
    }
}

/* Scans rx_buf for a complete, checksum-valid frame starting at offset 0.
 * Returns the number of bytes consumed (0 if no complete frame is present
 * yet), resyncing past a single stray byte on checksum failure so a single
 * corrupted byte can't wedge the parser permanently. */
static size_t try_parse_one_frame(void) {
    if (rx_len < 5) {
        return 0;
    }
    if (rx_buf[0] != MZ_SYNC0 || rx_buf[1] != MZ_SYNC1) {
        return 1; /* resync: drop one byte and try again */
    }
    uint16_t payload_len = (uint16_t)(rx_buf[3] | (rx_buf[4] << 8));
    if (payload_len > MZ_FRAME_MAX_PAYLOAD) {
        return 1; /* clearly bogus length, resync */
    }
    size_t total = 5 + payload_len + 1;
    if (rx_len < total) {
        return 0; /* frame not fully received yet */
    }
    uint8_t expected = frame_checksum(rx_buf, total - 1);
    if (expected != rx_buf[total - 1]) {
        LOG_WRN("MacroZ USB checksum mismatch, resyncing");
        return 1;
    }
    handle_frame(rx_buf[2], payload_len > 0 ? &rx_buf[5] : NULL, payload_len);
    return total;
}

static void rx_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    for (;;) {
        size_t consumed = try_parse_one_frame();
        if (consumed == 0) {
            break;
        }
        memmove(rx_buf, rx_buf + consumed, rx_len - consumed);
        rx_len -= consumed;
    }
}

static void uart_cb(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
        return;
    }
    uint8_t chunk[64];
    int n;
    while ((n = uart_fifo_read(dev, chunk, sizeof(chunk))) > 0) {
        size_t space = sizeof(rx_buf) - rx_len;
        size_t take = MIN((size_t)n, space);
        if (take > 0) {
            memcpy(rx_buf + rx_len, chunk, take);
            rx_len += take;
        }
        if (take < (size_t)n) {
            LOG_WRN("MacroZ USB RX buffer full, dropping bytes");
        }
    }
    k_work_submit(&rx_work);
}

static int macroz_usb_init(void)
{
    if (!device_is_ready(cdc_dev)) {
        LOG_ERR("CDC ACM device not ready");
        return -ENODEV;
    }

    k_work_init(&rx_work, rx_work_handler);

    int ret = uart_irq_callback_user_data_set(cdc_dev, uart_cb, NULL);
    if (ret != 0) {
        LOG_ERR("Failed to install CDC ACM RX callback: %d", ret);
        return ret;
    }

    uart_irq_rx_enable(cdc_dev);

    LOG_INF("MacroZ USB transport ready (DTR=%s)",
            usb_dtr_is_set() ? "set" : "not set");
    return 0;
}

SYS_INIT(macroz_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
