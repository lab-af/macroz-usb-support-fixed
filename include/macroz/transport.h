#pragma once

#include <zephyr/types.h>
#include <macroz/protocol.h>

/* Applies a single control command (the same BEGIN/CHUNK/COMMIT/RESET/
 * SELECT_READ_CHUNK format already used over BLE) regardless of which
 * transport delivered it. Returns 0 on success, or a negative value if the
 * command was rejected; in both cases the shared `status` state (readable
 * via macroz_get_status) is updated the same way the BLE control
 * characteristic already updates it today. */
int macroz_apply_command(const uint8_t *data, uint16_t len);

/* Copies up to max_len bytes of the active config starting at `offset`
 * (which must have been set via a prior SELECT_READ_CHUNK command) into
 * `out`. Returns the number of bytes actually copied. */
size_t macroz_read_config_chunk(uint16_t offset, uint8_t *out, size_t max_len);

/* Copies the current status snapshot into `out`. */
void macroz_get_status(struct macroz_status *out);
