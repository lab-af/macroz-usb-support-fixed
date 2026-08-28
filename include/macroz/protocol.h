#pragma once

#include <zephyr/toolchain.h>
#include <zephyr/types.h>

#define MACROZ_PROTOCOL_VERSION 2
#define MACROZ_CONFIG_MAGIC 0x5A4D
#define MACROZ_KEY_COUNT 9
#define MACROZ_MACRO_COUNT 6
#define MACROZ_MACRO_STEP_COUNT UINT8_MAX
#define MACROZ_READ_CHUNK_SIZE 256

#define MACROZ_USAGE_PAGE_KEYBOARD 0x07
#define MACROZ_USAGE_PAGE_CONSUMER 0x0C

enum macroz_binding_kind {
    MACROZ_BINDING_KEY = 0,
    MACROZ_BINDING_MACRO = 1,
};

enum macroz_command {
    MACROZ_COMMAND_BEGIN = 1,
    MACROZ_COMMAND_CHUNK = 2,
    MACROZ_COMMAND_COMMIT = 3,
    MACROZ_COMMAND_RESET = 4,
    MACROZ_COMMAND_SELECT_READ_CHUNK = 5,
};

enum macroz_status_code {
    MACROZ_STATUS_READY = 0,
    MACROZ_STATUS_RECEIVING = 1,
    MACROZ_STATUS_SAVED = 2,
    MACROZ_STATUS_INVALID = 3,
    MACROZ_STATUS_SEQUENCE_ERROR = 4,
};

struct macroz_binding {
    uint8_t kind;
    uint8_t usage_page;
    uint16_t usage;
    uint8_t modifiers;
    uint8_t macro_index;
    uint8_t reserved[2];
} __packed;

struct macroz_macro_step {
    uint8_t usage_page;
    uint8_t modifiers;
    uint16_t usage;
    uint16_t delay_ms;
} __packed;

struct macroz_macro {
    uint8_t length;
    uint8_t reserved;
    struct macroz_macro_step steps[MACROZ_MACRO_STEP_COUNT];
} __packed;

struct macroz_config {
    uint16_t magic;
    uint8_t version;
    uint8_t key_count;
    uint8_t macro_count;
    uint8_t macro_step_count;
    uint8_t reserved[2];
    struct macroz_binding bindings[MACROZ_KEY_COUNT];
    struct macroz_macro macros[MACROZ_MACRO_COUNT];
} __packed;

struct macroz_status {
    uint8_t code;
    uint8_t protocol_version;
    uint16_t received;
} __packed;
