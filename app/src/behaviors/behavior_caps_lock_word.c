/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_caps_lock_word

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/keys.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct caps_lock_word_continue_item {
    uint16_t page;
    uint32_t id;
    uint8_t implicit_modifiers;
};

struct behavior_caps_lock_word_config {
    uint8_t continuations_count;
    struct caps_lock_word_continue_item continuations[];
};

struct behavior_caps_lock_word_data {
    bool active;
};

static void toggle_caps_lock(void) {
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = HID_USAGE_KEY,
        .keycode = HID_USAGE_KEY_KEYBOARD_CAPS_LOCK,
        .implicit_modifiers = 0,
        .explicit_modifiers = 0,
        .state = true,
        .timestamp = k_uptime_get(),
    });
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = HID_USAGE_KEY,
        .keycode = HID_USAGE_KEY_KEYBOARD_CAPS_LOCK,
        .implicit_modifiers = 0,
        .explicit_modifiers = 0,
        .state = false,
        .timestamp = k_uptime_get(),
    });
}

static void activate_caps_lock_word(const struct device *dev) {
    struct behavior_caps_lock_word_data *data = dev->data;

    if (data->active) {
        return;
    }

    data->active = true;
    LOG_DBG("Caps lock word activated");
    toggle_caps_lock();
}

static void deactivate_caps_lock_word(const struct device *dev) {
    struct behavior_caps_lock_word_data *data = dev->data;

    if (!data->active) {
        return;
    }

    data->active = false;
    LOG_DBG("Caps lock word deactivated");
    toggle_caps_lock();
}

static int on_caps_lock_word_binding_pressed(struct zmk_behavior_binding *binding,
                                             struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_caps_lock_word_data *data = dev->data;

    if (data->active) {
        deactivate_caps_lock_word(dev);
    } else {
        activate_caps_lock_word(dev);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_caps_lock_word_binding_released(struct zmk_behavior_binding *binding,
                                              struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_caps_lock_word_driver_api = {
    .binding_pressed = on_caps_lock_word_binding_pressed,
    .binding_released = on_caps_lock_word_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int caps_lock_word_keycode_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_caps_lock_word, caps_lock_word_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_caps_lock_word, zmk_keycode_state_changed);

#define GET_DEV(inst) DEVICE_DT_INST_GET(inst),
static const struct device *devs[] = {DT_INST_FOREACH_STATUS_OKAY(GET_DEV)};

static bool caps_lock_word_is_caps_includelist(const struct behavior_caps_lock_word_config *config,
                                               uint16_t usage_page, uint8_t usage_id,
                                               uint8_t implicit_modifiers) {
    for (int i = 0; i < config->continuations_count; i++) {
        const struct caps_lock_word_continue_item *continuation = &config->continuations[i];
        LOG_DBG("Comparing with 0x%02X - 0x%02X (with implicit mods: 0x%02X)", continuation->page,
                continuation->id, continuation->implicit_modifiers);

        if (continuation->page == usage_page && continuation->id == usage_id &&
            (continuation->implicit_modifiers &
             (implicit_modifiers | zmk_hid_get_explicit_mods())) ==
                continuation->implicit_modifiers) {
            LOG_DBG("Continuing caps_lock_word, found included usage: 0x%02X - 0x%02X", usage_page,
                    usage_id);
            return true;
        }
    }

    return false;
}

static bool caps_lock_word_is_alpha(uint8_t usage_id) {
    return (usage_id >= HID_USAGE_KEY_KEYBOARD_A && usage_id <= HID_USAGE_KEY_KEYBOARD_Z);
}

static bool caps_lock_word_is_numeric(uint8_t usage_id) {
    return (usage_id >= HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION &&
            usage_id <= HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS);
}

static int caps_lock_word_keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Ignore our own Caps Lock events to avoid re-entrant deactivation
    if (ev->usage_page == HID_USAGE_KEY &&
        ev->keycode == HID_USAGE_KEY_KEYBOARD_CAPS_LOCK) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (int i = 0; i < ARRAY_SIZE(devs); i++) {
        const struct device *dev = devs[i];

        struct behavior_caps_lock_word_data *data = dev->data;
        if (!data->active) {
            continue;
        }

        const struct behavior_caps_lock_word_config *config = dev->config;

        if (!caps_lock_word_is_alpha(ev->keycode) && !caps_lock_word_is_numeric(ev->keycode) &&
            !is_mod(ev->usage_page, ev->keycode) &&
            !caps_lock_word_is_caps_includelist(config, ev->usage_page, ev->keycode,
                                                ev->implicit_modifiers)) {
            LOG_DBG("Deactivating caps_lock_word for 0x%02X - 0x%02X", ev->usage_page,
                    ev->keycode);
            deactivate_caps_lock_word(dev);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#define CAPS_LOCK_WORD_LABEL(i, _n) DT_INST_LABEL(i)

#define PARSE_BREAK(i)                                                                             \
    {.page = ZMK_HID_USAGE_PAGE(i), .id = ZMK_HID_USAGE_ID(i), .implicit_modifiers = SELECT_MODS(i)}

#define BREAK_ITEM(i, n) PARSE_BREAK(DT_INST_PROP_BY_IDX(n, continue_list, i))

#define KP_INST(n)                                                                                 \
    static struct behavior_caps_lock_word_data behavior_caps_lock_word_data_##n = {                \
        .active = false};                                                                          \
    static const struct behavior_caps_lock_word_config behavior_caps_lock_word_config_##n = {      \
        .continuations = {LISTIFY(DT_INST_PROP_LEN(n, continue_list), BREAK_ITEM, (, ), n)},       \
        .continuations_count = DT_INST_PROP_LEN(n, continue_list),                                 \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &behavior_caps_lock_word_data_##n,                      \
                            &behavior_caps_lock_word_config_##n, POST_KERNEL,                      \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_caps_lock_word_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif
