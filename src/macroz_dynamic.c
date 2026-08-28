#define DT_DRV_COMPAT macroz_behavior_dynamic

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <macroz/protocol.h>
#include <macroz/transport.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_REGISTER(macroz_dynamic, CONFIG_MACROZ_LOG_LEVEL);

#define MACROZ_LEGACY_SETTINGS_KEY "macroz/config"
#define MACROZ_CORE_SETTINGS_KEY "macroz/core"
#define MACROZ_SAVE_DELAY K_MSEC(750)
#define MACROZ_TAP_DURATION K_MSEC(20)
#define MACROZ_QUEUE_SIZE 8
#define MACROZ_CONFIG_CORE_SIZE offsetof(struct macroz_config, macros)

#define MACROZ_UUID(value) BT_UUID_128_ENCODE(value, 0x7b6b, 0x4d9d, 0xa862, 0x8e6e4f4d5a10)
#define MACROZ_SERVICE_UUID MACROZ_UUID(0x6d7f2f00)
#define MACROZ_CONFIG_UUID MACROZ_UUID(0x6d7f2f01)
#define MACROZ_CONTROL_UUID MACROZ_UUID(0x6d7f2f02)
#define MACROZ_STATUS_UUID MACROZ_UUID(0x6d7f2f03)

BUILD_ASSERT(sizeof(struct macroz_binding) == 8, "Unexpected binding wire size");
BUILD_ASSERT(sizeof(struct macroz_macro_step) == 6, "Unexpected macro step wire size");
BUILD_ASSERT(sizeof(struct macroz_macro) == 1532, "Unexpected macro wire size");
BUILD_ASSERT(sizeof(struct macroz_config) == 9272, "Unexpected config wire size");
BUILD_ASSERT(sizeof(struct macroz_config) <= UINT16_MAX, "Config offsets exceed the protocol");

struct macroz_legacy_macro {
    uint8_t length;
    uint8_t reserved;
    struct macroz_macro_step steps[8];
} __packed;

struct macroz_legacy_config {
    uint16_t magic;
    uint8_t version;
    uint8_t key_count;
    uint8_t macro_count;
    uint8_t macro_step_count;
    uint8_t reserved[2];
    struct macroz_binding bindings[MACROZ_KEY_COUNT];
    struct macroz_legacy_macro macros[MACROZ_MACRO_COUNT];
} __packed;

BUILD_ASSERT(sizeof(struct macroz_legacy_config) == 380, "Unexpected legacy config size");

static const char *const macro_settings_keys[MACROZ_MACRO_COUNT] = {
    "macroz/m0", "macroz/m1", "macroz/m2", "macroz/m3", "macroz/m4", "macroz/m5",
};

static K_MUTEX_DEFINE(state_lock);
static struct macroz_config active_config;
static struct macroz_config staging_config;
static struct macroz_config save_snapshot;
static struct macroz_legacy_config legacy_config;
static uint16_t staging_received;
static uint16_t read_chunk_offset;
static bool loaded_v2_core;
static uint8_t loaded_v2_macros;
static struct macroz_status status = {
    .code = MACROZ_STATUS_READY,
    .protocol_version = MACROZ_PROTOCOL_VERSION,
};

struct held_key {
    bool active;
    uint32_t encoded;
};

static struct held_key held_keys[MACROZ_KEY_COUNT];
static uint8_t macro_queue[MACROZ_QUEUE_SIZE];
static uint8_t queue_head;
static uint8_t queue_count;
static bool runner_active;
static bool runner_releasing;
static uint8_t runner_step;
static struct macroz_macro runner_macro;

static bool valid_action(uint8_t usage_page, uint16_t usage) {
    if (usage == 0) {
        return false;
    }

    if (usage_page == MACROZ_USAGE_PAGE_KEYBOARD) {
        return usage <= UINT8_MAX;
    }

    return usage_page == MACROZ_USAGE_PAGE_CONSUMER && usage <= 0x0FFF;
}

static bool valid_binding(const struct macroz_binding *binding) {
    if (binding->kind == MACROZ_BINDING_KEY) {
        return valid_action(binding->usage_page, binding->usage);
    }

    return binding->kind == MACROZ_BINDING_MACRO && binding->macro_index < MACROZ_MACRO_COUNT;
}

static bool valid_macro(uint8_t length, const struct macroz_macro_step *steps) {
    for (size_t step = 0; step < length; step++) {
        if (!valid_action(steps[step].usage_page, steps[step].usage)) {
            return false;
        }
    }

    return true;
}

static bool valid_config(const struct macroz_config *config) {
    if (config->magic != MACROZ_CONFIG_MAGIC || config->version != MACROZ_PROTOCOL_VERSION ||
        config->key_count != MACROZ_KEY_COUNT || config->macro_count != MACROZ_MACRO_COUNT ||
        config->macro_step_count != MACROZ_MACRO_STEP_COUNT) {
        return false;
    }

    for (size_t i = 0; i < MACROZ_KEY_COUNT; i++) {
        if (!valid_binding(&config->bindings[i])) {
            return false;
        }
    }

    for (size_t i = 0; i < MACROZ_MACRO_COUNT; i++) {
        const struct macroz_macro *macro = &config->macros[i];
        if (!valid_macro(macro->length, macro->steps)) {
            return false;
        }
    }

    return true;
}

static bool valid_legacy_config(const struct macroz_legacy_config *config) {
    if (config->magic != MACROZ_CONFIG_MAGIC || config->version != 1 ||
        config->key_count != MACROZ_KEY_COUNT || config->macro_count != MACROZ_MACRO_COUNT ||
        config->macro_step_count != 8) {
        return false;
    }

    for (size_t i = 0; i < MACROZ_KEY_COUNT; i++) {
        if (!valid_binding(&config->bindings[i])) {
            return false;
        }
    }

    for (size_t i = 0; i < MACROZ_MACRO_COUNT; i++) {
        if (config->macros[i].length > 8 ||
            !valid_macro(config->macros[i].length, config->macros[i].steps)) {
            return false;
        }
    }

    return true;
}

static void load_defaults(struct macroz_config *config) {
    static const uint8_t default_usages[MACROZ_KEY_COUNT] = {
        0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70,
    };

    memset(config, 0, sizeof(*config));
    config->magic = MACROZ_CONFIG_MAGIC;
    config->version = MACROZ_PROTOCOL_VERSION;
    config->key_count = MACROZ_KEY_COUNT;
    config->macro_count = MACROZ_MACRO_COUNT;
    config->macro_step_count = MACROZ_MACRO_STEP_COUNT;

    for (size_t i = 0; i < MACROZ_KEY_COUNT; i++) {
        config->bindings[i].kind = MACROZ_BINDING_KEY;
        config->bindings[i].usage_page = MACROZ_USAGE_PAGE_KEYBOARD;
        config->bindings[i].usage = default_usages[i];
    }
}

static uint32_t encoded_action(uint8_t usage_page, uint16_t usage, uint8_t modifiers) {
    return ((uint32_t)modifiers << 24) | ((uint32_t)usage_page << 16) | usage;
}

static void save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&state_lock, K_FOREVER);
    memcpy(&save_snapshot, &active_config, sizeof(save_snapshot));
    k_mutex_unlock(&state_lock);

    int first_err = settings_save_one(MACROZ_CORE_SETTINGS_KEY, &save_snapshot,
                                      MACROZ_CONFIG_CORE_SIZE);
    for (size_t i = 0; i < MACROZ_MACRO_COUNT; i++) {
        int err = settings_save_one(macro_settings_keys[i], &save_snapshot.macros[i],
                                    sizeof(save_snapshot.macros[i]));
        if (!first_err && err) {
            first_err = err;
        }
    }

    if (!first_err) {
        settings_delete(MACROZ_LEGACY_SETTINGS_KEY);
    } else {
        LOG_ERR("Unable to save configuration (%d)", first_err);
    }
}

K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

static void macro_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(macro_work, macro_work_handler);

static void macro_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    struct macroz_macro_step action;
    bool releasing;

    k_mutex_lock(&state_lock, K_FOREVER);
    if (!runner_active) {
        while (queue_count > 0 && !runner_active) {
            uint8_t macro_index = macro_queue[queue_head];
            queue_head = (queue_head + 1) % MACROZ_QUEUE_SIZE;
            queue_count--;
            memcpy(&runner_macro, &active_config.macros[macro_index], sizeof(runner_macro));
            runner_step = 0;
            runner_releasing = false;
            runner_active = runner_macro.length > 0;
        }
    }

    if (!runner_active) {
        k_mutex_unlock(&state_lock);
        return;
    }

    action = runner_macro.steps[runner_step];
    releasing = runner_releasing;
    runner_releasing = !runner_releasing;
    if (releasing) {
        runner_step++;
        if (runner_step >= runner_macro.length) {
            runner_active = false;
        }
    }
    k_mutex_unlock(&state_lock);

    uint32_t encoded = encoded_action(action.usage_page, action.usage, action.modifiers);
    raise_zmk_keycode_state_changed_from_encoded(encoded, !releasing, k_uptime_get());

    if (!releasing) {
        k_work_reschedule(&macro_work, MACROZ_TAP_DURATION);
    } else {
        k_work_reschedule(&macro_work, K_MSEC(action.delay_ms));
    }
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "config") == 0) {
        if (len != sizeof(legacy_config)) {
            return -EINVAL;
        }

        ssize_t read = read_cb(cb_arg, &legacy_config, sizeof(legacy_config));
        if (read != sizeof(legacy_config) || !valid_legacy_config(&legacy_config)) {
            LOG_WRN("Ignoring invalid legacy configuration");
            return read < 0 ? (int)read : -EINVAL;
        }

        bool migrated = false;
        if (!loaded_v2_core) {
            memcpy(active_config.bindings, legacy_config.bindings,
                   sizeof(active_config.bindings));
            migrated = true;
        }
        for (size_t i = 0; i < MACROZ_MACRO_COUNT; i++) {
            if (loaded_v2_macros & BIT(i)) {
                continue;
            }
            active_config.macros[i].length = legacy_config.macros[i].length;
            memcpy(active_config.macros[i].steps, legacy_config.macros[i].steps,
                   sizeof(legacy_config.macros[i].steps));
            migrated = true;
        }
        if (migrated) {
            k_work_reschedule(&save_work, MACROZ_SAVE_DELAY);
        }
        return 0;
    }

    if (strcmp(name, "core") == 0) {
        if (len != MACROZ_CONFIG_CORE_SIZE) {
            return -EINVAL;
        }

        memset(&staging_config, 0, sizeof(staging_config));
        ssize_t read = read_cb(cb_arg, &staging_config, MACROZ_CONFIG_CORE_SIZE);
        if (read != MACROZ_CONFIG_CORE_SIZE || !valid_config(&staging_config)) {
            LOG_WRN("Ignoring invalid stored configuration core");
            return read < 0 ? (int)read : -EINVAL;
        }

        memcpy(&active_config, &staging_config, MACROZ_CONFIG_CORE_SIZE);
        loaded_v2_core = true;
        return 0;
    }

    if (name[0] == 'm' && name[1] >= '0' && name[1] < '0' + MACROZ_MACRO_COUNT &&
        name[2] == '\0') {
        uint8_t index = name[1] - '0';
        if (len != sizeof(staging_config.macros[index])) {
            return -EINVAL;
        }

        ssize_t read = read_cb(cb_arg, &staging_config.macros[index],
                               sizeof(staging_config.macros[index]));
        if (read != sizeof(staging_config.macros[index]) ||
            !valid_macro(staging_config.macros[index].length,
                         staging_config.macros[index].steps)) {
            LOG_WRN("Ignoring invalid stored macro %u", index);
            return read < 0 ? (int)read : -EINVAL;
        }

        memcpy(&active_config.macros[index], &staging_config.macros[index],
               sizeof(active_config.macros[index]));
        loaded_v2_macros |= BIT(index);
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(macroz, "macroz", NULL, settings_set, NULL, NULL);

/* --- Shared command/read logic, callable from any transport (BLE, USB) --- */

int macroz_apply_command(const uint8_t *data, uint16_t len) {
    if (len < 1) {
        status.code = MACROZ_STATUS_SEQUENCE_ERROR;
        return -EINVAL;
    }

    switch (data[0]) {
    case MACROZ_COMMAND_BEGIN:
        if (len != 1) {
            status.code = MACROZ_STATUS_SEQUENCE_ERROR;
            return -EINVAL;
        }
        staging_received = 0;
        memset(&staging_config, 0, sizeof(staging_config));
        status.code = MACROZ_STATUS_RECEIVING;
        status.received = 0;
        return 0;

    case MACROZ_COMMAND_CHUNK: {
        if (len < 4 || status.code != MACROZ_STATUS_RECEIVING) {
            status.code = MACROZ_STATUS_SEQUENCE_ERROR;
            return -EINVAL;
        }
        uint16_t chunk_offset = sys_get_le16(&data[1]);
        size_t chunk_len = len - 3;
        if (chunk_offset != staging_received || chunk_len > sizeof(staging_config) - staging_received) {
            status.code = MACROZ_STATUS_SEQUENCE_ERROR;
            return -EINVAL;
        }
        memcpy((uint8_t *)&staging_config + staging_received, &data[3], chunk_len);
        staging_received += chunk_len;
        status.received = staging_received;
        return 0;
    }

    case MACROZ_COMMAND_COMMIT:
        if (len != 1 || status.code != MACROZ_STATUS_RECEIVING ||
            staging_received != sizeof(staging_config)) {
            status.code = MACROZ_STATUS_SEQUENCE_ERROR;
            return -EINVAL;
        }
        if (!valid_config(&staging_config)) {
            status.code = MACROZ_STATUS_INVALID;
            return -EINVAL;
        }
        k_mutex_lock(&state_lock, K_FOREVER);
        memcpy(&active_config, &staging_config, sizeof(active_config));
        k_mutex_unlock(&state_lock);
        status.code = MACROZ_STATUS_SAVED;
        k_work_reschedule(&save_work, MACROZ_SAVE_DELAY);
        return 0;

    case MACROZ_COMMAND_RESET:
        if (len != 1) {
            status.code = MACROZ_STATUS_SEQUENCE_ERROR;
            return -EINVAL;
        }
        k_mutex_lock(&state_lock, K_FOREVER);
        load_defaults(&active_config);
        k_mutex_unlock(&state_lock);
        status.code = MACROZ_STATUS_SAVED;
        status.received = 0;
        k_work_reschedule(&save_work, MACROZ_SAVE_DELAY);
        return 0;

    case MACROZ_COMMAND_SELECT_READ_CHUNK: {
        if (len != 3) {
            status.code = MACROZ_STATUS_SEQUENCE_ERROR;
            return -EINVAL;
        }
        uint16_t requested_offset = sys_get_le16(&data[1]);
        if (requested_offset >= sizeof(active_config)) {
            status.code = MACROZ_STATUS_SEQUENCE_ERROR;
            return -EINVAL;
        }
        k_mutex_lock(&state_lock, K_FOREVER);
        read_chunk_offset = requested_offset;
        k_mutex_unlock(&state_lock);
        return 0;
    }

    default:
        status.code = MACROZ_STATUS_SEQUENCE_ERROR;
        return -ENOTSUP;
    }
}

size_t macroz_read_config_chunk(uint16_t offset, uint8_t *out, size_t max_len) {
    k_mutex_lock(&state_lock, K_FOREVER);
    if (offset >= sizeof(active_config)) {
        k_mutex_unlock(&state_lock);
        return 0;
    }
    size_t remaining = sizeof(active_config) - offset;
    size_t chunk_size = MIN(remaining, max_len);
    memcpy(out, (const uint8_t *)&active_config + offset, chunk_size);
    k_mutex_unlock(&state_lock);
    return chunk_size;
}

void macroz_get_status(struct macroz_status *out) {
    *out = status;
}

/* --- BLE-specific wrappers: thin adapters over the shared logic above --- */

static ssize_t read_config(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    uint8_t chunk[MACROZ_READ_CHUNK_SIZE];
    size_t chunk_size = macroz_read_config_chunk(read_chunk_offset, chunk, sizeof(chunk));
    return bt_gatt_attr_read(conn, attr, buf, len, offset, chunk, chunk_size);
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &status, sizeof(status));
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) {
        status.code = MACROZ_STATUS_SEQUENCE_ERROR;
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    int result = macroz_apply_command(buf, len);
    if (result != 0) {
        uint8_t att_error = (result == -ENOTSUP) ? BT_ATT_ERR_NOT_SUPPORTED
                                                  : BT_ATT_ERR_VALUE_NOT_ALLOWED;
        return BT_GATT_ERR(att_error);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(
    macroz_service, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(MACROZ_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(MACROZ_CONFIG_UUID), BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, read_config, NULL, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(MACROZ_CONTROL_UUID), BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_control, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(MACROZ_STATUS_UUID), BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, read_status, NULL, NULL));

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    uint8_t index = binding->param1;
    if (index >= MACROZ_KEY_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    struct macroz_binding assignment = active_config.bindings[index];
    if (assignment.kind == MACROZ_BINDING_MACRO) {
        if (queue_count < MACROZ_QUEUE_SIZE) {
            macro_queue[(queue_head + queue_count) % MACROZ_QUEUE_SIZE] = assignment.macro_index;
            queue_count++;
            k_work_schedule(&macro_work, K_NO_WAIT);
        } else {
            LOG_WRN("Macro queue full");
        }
        k_mutex_unlock(&state_lock);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    uint32_t encoded = encoded_action(assignment.usage_page, assignment.usage,
                                      assignment.modifiers);
    held_keys[index] = (struct held_key){.active = true, .encoded = encoded};
    k_mutex_unlock(&state_lock);
    return raise_zmk_keycode_state_changed_from_encoded(encoded, true, event.timestamp);
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    uint8_t index = binding->param1;
    if (index >= MACROZ_KEY_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    struct held_key held = held_keys[index];
    held_keys[index].active = false;
    k_mutex_unlock(&state_lock);

    if (!held.active) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    return raise_zmk_keycode_state_changed_from_encoded(held.encoded, false, event.timestamp);
}

static const struct behavior_driver_api driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

static int macroz_init(const struct device *device) {
    ARG_UNUSED(device);
    load_defaults(&active_config);
    return 0;
}

#define MACROZ_INST(n)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, macroz_init, NULL, NULL, NULL, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &driver_api);

DT_INST_FOREACH_STATUS_OKAY(MACROZ_INST)
