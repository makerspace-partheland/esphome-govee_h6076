import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import (
    binary_sensor,
    button,
    light,
    number,
    select,
    sensor,
    switch,
    text_sensor,
    time,
    uart,
)
from esphome.components.light.effects import register_rgb_effect
from esphome.components.light.types import LightEffect
from esphome.const import (
    CONF_BACKGROUND_COLOR,
    CONF_BLUE,
    CONF_BRIGHTNESS,
    CONF_COLOR,
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ESPHOME,
    CONF_GAMMA_CORRECT,
    CONF_GREEN,
    CONF_ID,
    CONF_INITIAL_STATE,
    CONF_INITIAL_VALUE,
    CONF_LIGHT,
    CONF_LIGHT_ID,
    CONF_MINUTES,
    CONF_MODE,
    CONF_NAME,
    CONF_OUTPUT_ID,
    CONF_PROJECT,
    CONF_RED,
    CONF_RESTORE_MODE,
    CONF_RESTORE_STATE,
    CONF_SEGMENTS,
    CONF_STATE,
    CONF_TIME_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    UNIT_MINUTE,
)
from esphome.core import CORE

from . import DOMAIN, VERSION
from .scene_catalogue import SCENE_CATALOGUE

CONF_BACKGROUND_BRIGHTNESS = "background_brightness"
CONF_CALM = "calm"
CONF_COLOR_INDEX = "color_index"
CONF_CUSTOM_ACTION_TYPE = "custom_action_type"
CONF_CUSTOM_BRIGHTNESS_MIN = "custom_brightness_min"
CONF_CUSTOM_BRIGHTNESS_MAX = "custom_brightness_max"
CONF_CUSTOM_BRIGHTNESS_SHAPE = "custom_brightness_shape"
CONF_CUSTOM_COLOR_CHANGE = "custom_color_change"
CONF_CUSTOM_COLOR_DISTRIBUTION = "custom_color_distribution"
CONF_CUSTOM_COLOR_ORDER = "custom_color_order"
CONF_CUSTOM_DECAY_SPEED = "custom_decay_speed"
CONF_CUSTOM_DIRECTION = "custom_direction"
CONF_CUSTOM_IDLE_BRIGHTNESS = "custom_idle_brightness"
CONF_CUSTOM_SPOT_LENGTH = "custom_spot_length"
CONF_EFFECT_ID = "effect_id"
CONF_FIXED_COLOR = "fixed_color"
CONF_GOVEE_H6076_ID = "govee_h6076_id"
CONF_GOVEE_H6076_MUSIC = "govee_h6076_music"
CONF_GOVEE_H6076_SCENE = "govee_h6076_scene"
CONF_GOVEE_H6076_SEGMENTS = "govee_h6076_segments"
CONF_GRADIENT = "gradient"
CONF_MUSIC_PALETTE = "music_palette"
CONF_MUSIC_COLORS = "music_colors"
DEFAULT_MUSIC_PALETTE = [0xFF0000, 0xFF7F00, 0xFFFF00, 0x00FF00, 0x0000FF, 0x00FFFF, 0x8B00FF]
CONF_MUSIC_SENSITIVITY = "music_sensitivity"
CONF_DREAMVIEW = "dreamview"
CONF_MUSIC_EFFECT = "music_effect"
CONF_MEMBERS = "members"
CONF_SYNCHRONIZATION = "synchronization"
CONF_CREATE_GROUP = "create_group"
CONF_REMOVE_GROUP = "remove_group"
CONF_STATUS = "status"
CONF_CONNECTION = "connection"
CONF_PROBLEM = "problem"
CONF_NATIVE_SLEEP = "native_sleep"
CONF_NATIVE_TIMER_COUNT = "native_timer_count"
CONF_NATIVE_TIMER_SUMMARY = "native_timer_summary"
CONF_NATIVE_WAKE_STATUS = "native_wake_status"
CONF_PIANO_BARS = "piano_bars"
CONF_SCENE = "scene"
CONF_SEGMENT = "segment"
CONF_SHAPE = "shape"
CONF_SLEEP_REMAINING = "sleep_remaining"
CONF_SLEEP_STATUS = "sleep_status"
CONF_SLEEP_TIMER = "sleep_timer"

DEPENDENCIES = ["esp32", "uart"]
ns = cg.esphome_ns.namespace(DOMAIN)
GoveeLight = ns.class_("GoveeLight", light.LightOutput, cg.Component, uart.UARTDevice)
GoveeMusicEffect = ns.class_("GoveeMusicEffect", LightEffect)
GoveeMusicSensitivityNumber = ns.class_("GoveeMusicSensitivityNumber", number.Number, cg.Component)
GoveeSleepNumber = ns.class_("GoveeSleepNumber", number.Number)

SLEEP_SENSORS = {
    CONF_SLEEP_REMAINING: sensor.sensor_schema(
        unit_of_measurement=UNIT_MINUTE,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_DURATION,
    ),
    CONF_NATIVE_TIMER_COUNT: sensor.sensor_schema(
        accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_NATIVE_WAKE_STATUS: sensor.sensor_schema(
        accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
}
SLEEP_TEXT_SENSORS = {
    CONF_SLEEP_STATUS: text_sensor.text_sensor_schema(),
    CONF_NATIVE_TIMER_SUMMARY: text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
}


CUSTOM_MUSIC_FIELDS = {
    CONF_CUSTOM_BRIGHTNESS_SHAPE: cv.enum(
        {
            "uniform_min": 0,
            "peak": 1,
            "valley": 2,
            "rising": 3,
            "falling": 4,
            "uniform_max": 5,
        },
        lower=True,
    ),
    CONF_CUSTOM_SPOT_LENGTH: cv.int_range(min=1, max=14),
    CONF_CUSTOM_ACTION_TYPE: cv.int_range(min=1, max=6),
    CONF_CUSTOM_DIRECTION: cv.enum({"forward": 5, "reverse": 4}, lower=True),
    CONF_CUSTOM_IDLE_BRIGHTNESS: cv.int_range(min=0, max=100),
    CONF_CUSTOM_DECAY_SPEED: cv.int_range(min=1, max=5),
    CONF_CUSTOM_COLOR_DISTRIBUTION: cv.enum({"uniform": 0, "blocks": 2, "gradient": 3}, lower=True),
    CONF_CUSTOM_COLOR_CHANGE: cv.enum({"hold": 0, "timed": 1}, lower=True),
    CONF_CUSTOM_COLOR_ORDER: cv.enum({"sequential": 0, "random": 1}, lower=True),
}


def validate_music_settings(config):
    for key in CUSTOM_MUSIC_FIELDS:
        legacy = key.replace("custom_", "ab_", 1)
        if legacy in config:
            if key in config and config[key] != config[legacy]:
                raise cv.Invalid(f"{key} and {legacy} must not conflict")
            config[key] = config.pop(legacy)
    mode = str(config.get(CONF_MODE, "energetic"))
    if mode == "experimental_ab":
        mode = "custom"
    for key in (CONF_CUSTOM_BRIGHTNESS_MIN, CONF_CUSTOM_BRIGHTNESS_MAX):
        if key in config and mode != "custom":
            raise cv.Invalid(f"{key} requires mode: custom")
    if config.get(CONF_CUSTOM_BRIGHTNESS_MIN, 20) > config.get(CONF_CUSTOM_BRIGHTNESS_MAX, 100):
        raise cv.Invalid("custom_brightness_min must be less than or equal to custom_brightness_max")
    for key in (CONF_CUSTOM_BRIGHTNESS_SHAPE, CONF_CUSTOM_SPOT_LENGTH, CONF_CUSTOM_ACTION_TYPE):
        if key in config and mode != "custom":
            raise cv.Invalid(f"{key} requires mode: custom")
    if (mode != "custom" or config.get(CONF_CUSTOM_ACTION_TYPE, 1) != 1) and any(
        key in config
        for key in (
            CONF_CUSTOM_DIRECTION,
            CONF_CUSTOM_IDLE_BRIGHTNESS,
            CONF_CUSTOM_DECAY_SPEED,
            CONF_CUSTOM_COLOR_ORDER,
            CONF_CUSTOM_COLOR_CHANGE,
            CONF_CUSTOM_COLOR_DISTRIBUTION,
        )
    ):
        raise cv.Invalid(
            "custom_direction, custom_idle_brightness, custom_decay_speed, custom_color_order, "
            "custom_color_change and custom_color_distribution require mode: custom and "
            "custom_action_type: 1"
        )
    if CONF_FIXED_COLOR in config and mode not in {"rhythm", "bounce", "hopping", "beat", "spray"}:
        raise cv.Invalid("fixed_color is only supported by rhythm, bounce, hopping, beat and spray")
    if CONF_CALM in config and mode != "rhythm":
        raise cv.Invalid("calm is only supported by rhythm")
    if CONF_PIANO_BARS in config and mode != "piano":
        raise cv.Invalid("piano_bars requires piano")
    if CONF_GRADIENT in config and mode not in {"separation", "piano", "day_and_night"}:
        raise cv.Invalid("gradient is only supported by separation, piano and day_and_night")
    if CONF_BACKGROUND_COLOR in config and mode not in {"starlight", "dynamism"}:
        raise cv.Invalid("background_color is only supported by starlight and dynamism")
    if CONF_BACKGROUND_BRIGHTNESS in config and mode != "dynamism":
        raise cv.Invalid("background_brightness is only supported by dynamism")
    return config


@register_rgb_effect(
    CONF_GOVEE_H6076_MUSIC,
    GoveeMusicEffect,
    "Music",
    {
        **{cv.Optional(key): validator for key, validator in CUSTOM_MUSIC_FIELDS.items()},
        **{cv.Optional(key.replace("custom_", "ab_", 1)): validator for key, validator in CUSTOM_MUSIC_FIELDS.items()},
        cv.Optional(CONF_CUSTOM_BRIGHTNESS_MIN): cv.int_range(min=0, max=100),
        cv.Optional(CONF_CUSTOM_BRIGHTNESS_MAX): cv.int_range(min=0, max=100),
        cv.Optional(CONF_PIANO_BARS): cv.int_range(min=1, max=14),
        cv.Optional(CONF_GRADIENT): cv.boolean,
        cv.Optional(CONF_BACKGROUND_COLOR): cv.int_range(min=0, max=0xFFFFFF),
        cv.Optional(CONF_BACKGROUND_BRIGHTNESS): cv.int_range(min=0, max=50),
        cv.Optional(CONF_FIXED_COLOR): cv.int_range(min=0, max=0xFFFFFF),
        cv.Optional(CONF_CALM): cv.boolean,
        cv.Required(CONF_GOVEE_H6076_ID): cv.use_id(GoveeLight),
        cv.Optional(CONF_MODE, default="energetic"): cv.enum(
            {
                "custom": 0xFF,
                "experimental_ab": 0xFF,
                "energy_legacy": 0x05,
                "spectrum": 0x90,
                "scroll": 0x91,
                "double_separate": 0x5D,
                "tiaodong": 0x4A,
                "piano": 0x34,
                "fountain": 0x35,
                "dynamism": 0x33,
                "flow": 0x59,
                "starlight": 0x98,
                "bloom": 0x30,
                "bright": 0x31,
                "day_and_night": 0x37,
                "rhythm": 0x04,
                "beat": 0x06,
                "energetic": 0x00,
                "bounce": 0x08,
                "hopping": 0x02,
                "spray": 0x07,
                "skittles": 0x4D,
                "torch": 0x50,
                "candy_crush": 0x4E,
                "fusion": 0x4F,
                "shiny": 0x51,
                "separation": 0x32,
                "starry_sky": 0x16,
                "music_blink": 0x8F,
                "across": 0x8E,
                "double_scroll": 0x5A,
                "new_scroll": 0x71,
            },
            lower=True,
        ),
    },
    validate_music_settings,
)
async def govee_music_to_code(config, effect_id):
    var = cg.new_Pvariable(effect_id, config[CONF_NAME])
    parent = await cg.get_variable(config[CONF_GOVEE_H6076_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_mode(config[CONF_MODE]))
    for key in (
        *CUSTOM_MUSIC_FIELDS,
        CONF_CUSTOM_BRIGHTNESS_MIN,
        CONF_CUSTOM_BRIGHTNESS_MAX,
        CONF_PIANO_BARS,
        CONF_GRADIENT,
        CONF_BACKGROUND_COLOR,
        CONF_BACKGROUND_BRIGHTNESS,
    ):
        if key in config:
            cg.add(getattr(var, f"set_{key}")(config[key]))
    if CONF_FIXED_COLOR in config:
        cg.add(var.set_color(config[CONF_FIXED_COLOR]))
    if CONF_CALM in config:
        cg.add(var.set_calm(config[CONF_CALM]))
    return var


GoveeSceneEffect = ns.class_("GoveeSceneEffect", LightEffect)
SCENES = {
    "night_light": 2,
    "fireworks": 41,
    "white_light": 99,
    "stacking": 36,
    "bouncing_ball": 38,
    "bubble": 40,
    "rainbow": 22,
    "meteor": 50,
    "ripple": 35,
}

SCENES.update({key: entry["code"] for key, entry in SCENE_CATALOGUE.items()})


@register_rgb_effect(CONF_GOVEE_H6076_SCENE, GoveeSceneEffect, "Scene", {
    cv.Required(CONF_GOVEE_H6076_ID): cv.use_id(GoveeLight),
    cv.Required(CONF_SCENE): cv.enum(SCENES, lower=True),
})
async def govee_scene_to_code(config, effect_id):
    var = cg.new_Pvariable(effect_id, config[CONF_NAME])
    parent = await cg.get_variable(config[CONF_GOVEE_H6076_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_scene(config[CONF_SCENE]))
    if config[CONF_SCENE] in SCENE_CATALOGUE:
        cg.add(var.set_body(SCENE_CATALOGUE[config[CONF_SCENE]]["body"]))
    return var


GoveeSegmentsEffect = ns.class_("GoveeSegmentsEffect", LightEffect)
GoveeSegmentLight = ns.class_("GoveeSegmentLight", light.LightOutput)

SEGMENT_LIGHT_SCHEMA = light.light_schema(GoveeSegmentLight, light.LightType.RGB).extend({
    cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.positive_float,
    cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds,
})

@register_rgb_effect(CONF_GOVEE_H6076_SEGMENTS, GoveeSegmentsEffect, "Segments", {
    cv.Optional(CONF_EFFECT_ID): cv.declare_id(GoveeSegmentsEffect),
    cv.Required(CONF_GOVEE_H6076_ID): cv.use_id(GoveeLight),
    cv.Required(CONF_SEGMENTS): cv.All(cv.ensure_list(cv.Schema({
        cv.Required(CONF_COLOR): cv.int_range(min=0, max=0xFFFFFF),
        cv.Optional(CONF_BRIGHTNESS, default=100): cv.int_range(min=0, max=100),
        cv.Optional(CONF_LIGHT): SEGMENT_LIGHT_SCHEMA,
    })), cv.Length(min=7, max=7)),
})
async def govee_segments_to_code(config, effect_id):
    var = cg.new_Pvariable(config.get(CONF_EFFECT_ID, effect_id), config[CONF_NAME])
    parent = await cg.get_variable(config[CONF_GOVEE_H6076_ID])
    cg.add(var.set_parent(parent))
    for index, segment in enumerate(config[CONF_SEGMENTS]):
        cg.add(var.set_segment(index, segment[CONF_COLOR], segment[CONF_BRIGHTNESS]))
        if CONF_LIGHT in segment:
            cg.add_define("USE_GOVEE_SEGMENT_LIGHTS")
            output = await light.new_light(segment[CONF_LIGHT])
            cg.add(output.set_parent(var))
            cg.add(output.set_segment_index(index))
            cg.add(var.set_segment_light(index, output))
    return var

GoveeMusicColorLight = ns.class_("GoveeMusicColorLight", light.LightOutput)


def validate_music_color(config):
    if CONF_RESTORE_MODE not in config and CONF_RESTORE_STATE not in config:
        config[CONF_RESTORE_STATE] = light.RESTORE_STATE_SCHEMA("all")
    return config


MUSIC_COLOR_SCHEMA = cv.All(
    light.light_schema(GoveeMusicColorLight, light.LightType.RGB,
                       entity_category=ENTITY_CATEGORY_CONFIG, icon="mdi:palette").extend({
        cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.positive_float,
        cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds,
    }),
    validate_music_color,
)

GoveeDreamViewSwitch = ns.class_("GoveeDreamViewSwitch", switch.Switch)
GoveeDreamViewButton = ns.class_("GoveeDreamViewButton", button.Button)
GoveeDreamViewSelect = ns.class_("GoveeDreamViewSelect", select.Select)


def validate_dreamview_members(value):
    members = cv.All(cv.ensure_list(cv.mac_address), cv.Length(min=0, max=5))(value)
    if len({str(member) for member in members}) != len(members):
        raise cv.Invalid("DreamView members must have distinct Bluetooth MAC addresses")
    if any(member.as_hex == 0 or member.as_hex == 0xFFFFFFFFFFFF for member in members):
        raise cv.Invalid("DreamView requires individual Bluetooth MAC addresses")
    return members


DREAMVIEW_SCHEMA = cv.Schema({
    cv.Required(CONF_MEMBERS): validate_dreamview_members,
    cv.Optional(CONF_MUSIC_EFFECT): cv.All(
        select.select_schema(GoveeDreamViewSelect), cv.requires_component("select"),
    ),
    cv.Required(CONF_SYNCHRONIZATION): cv.All(
        switch.switch_schema(GoveeDreamViewSwitch, default_restore_mode="DISABLED"),
        cv.requires_component("switch"),
    ),
    cv.Optional(CONF_CREATE_GROUP): cv.invalid(
        "DreamView groups are configured automatically; remove create_group"
    ),
    cv.Required(CONF_REMOVE_GROUP): cv.All(
        button.button_schema(GoveeDreamViewButton, entity_category=ENTITY_CATEGORY_CONFIG), cv.requires_component("button"),
    ),
    cv.Required(CONF_CONNECTION): cv.All(
        binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.requires_component("binary_sensor"),
    ),
    cv.Required(CONF_PROBLEM): cv.All(
        binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM, entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.requires_component("binary_sensor"),
    ),
    cv.Optional(CONF_STATUS): cv.All(
        text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC).extend({
            cv.Optional(CONF_DISABLED_BY_DEFAULT, default=True): cv.boolean,
        }),
        cv.requires_component("text_sensor"),
    ),
})


CONFIG_SCHEMA = (
    light.RGB_LIGHT_SCHEMA.extend(
        {
            cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.positive_float,
            cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_DREAMVIEW): DREAMVIEW_SCHEMA,
            cv.Optional(CONF_NATIVE_SLEEP, default=False): cv.boolean,
            cv.Optional(CONF_SLEEP_TIMER): cv.All(
                number.number_schema(
                    GoveeSleepNumber,
                    unit_of_measurement=UNIT_MINUTE,
                    device_class=DEVICE_CLASS_DURATION,
                    icon="mdi:timer-outline",
                ).extend({cv.Optional(CONF_MODE, default="SLIDER"): cv.enum(number.NUMBER_MODES, upper=True)}),
                cv.requires_component("number"),
            ),
            **{
                cv.Optional(key): cv.All(schema, cv.requires_component("sensor"))
                for key, schema in SLEEP_SENSORS.items()
            },
            **{
                cv.Optional(key): cv.All(schema, cv.requires_component("text_sensor"))
                for key, schema in SLEEP_TEXT_SENSORS.items()
            },
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_MUSIC_PALETTE): cv.All(
                cv.ensure_list(cv.int_range(min=0, max=0xFFFFFF)),
                cv.Length(min=7, max=7),
            ),
            cv.Optional(CONF_MUSIC_COLORS): cv.All(cv.ensure_list(MUSIC_COLOR_SCHEMA), cv.Length(min=7, max=7)),
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(GoveeLight),
            cv.Optional(CONF_MUSIC_SENSITIVITY): cv.All(
                number.number_schema(
                    GoveeMusicSensitivityNumber,
                    icon="mdi:microphone",
                    entity_category=ENTITY_CATEGORY_CONFIG,
                )
                .extend({cv.Optional(CONF_INITIAL_VALUE, default=99): cv.int_range(min=0, max=99)})
                .extend(cv.COMPONENT_SCHEMA),
                cv.requires_component("number"),
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    DOMAIN, baud_rate=115200, require_tx=True, require_rx=True,
    data_bits=8, parity="NONE", stop_bits=1,
)


def validate_native_sleep(config):
    if config[CONF_NATIVE_SLEEP] and CONF_TIME_ID not in config:
        raise cv.Invalid("native_sleep requires time_id for the FR clock")
    sleep_entities = (CONF_SLEEP_TIMER, *SLEEP_SENSORS, *SLEEP_TEXT_SENSORS)
    if any(key in config for key in sleep_entities) and not config[CONF_NATIVE_SLEEP]:
        raise cv.Invalid("sleep entities require native_sleep: true")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, validate_native_sleep)

async def to_code(config):
    cg.add_define("GOVEE_H6076_VERSION", VERSION)
    if CONF_PROJECT not in CORE.config[CONF_ESPHOME]:
        # HA derives the device manufacturer/model from native project information.
        cg.add_define("ESPHOME_PROJECT_NAME", "Govee.H6076")
        cg.add_define("ESPHOME_PROJECT_VERSION", VERSION)
        cg.add_define("ESPHOME_PROJECT_VERSION_30", VERSION[:29])
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_native_sleep(config[CONF_NATIVE_SLEEP]))
    if CONF_DREAMVIEW in config:
        cg.add_define("USE_GOVEE_DREAMVIEW")
        cg.add(var.set_dreamview_enabled(True))
        conf = config[CONF_DREAMVIEW]
        if CONF_MUSIC_EFFECT in conf:
            cg.add_define("USE_GOVEE_DREAMVIEW_MUSIC")
            entity = await select.new_select(conf[CONF_MUSIC_EFFECT], options=[str(i) for i in range(1, 13)])
            cg.add(entity.set_parent(var))
            cg.add(var.set_dreamview_music_effect(entity))
        for member in conf[CONF_MEMBERS]:
            cg.add(var.add_dreamview_member(member.as_hex))
        entity = await switch.new_switch(conf[CONF_SYNCHRONIZATION])
        cg.add(entity.set_parent(var))
        cg.add(var.set_dreamview_switch(entity))
        entity = await button.new_button(conf[CONF_REMOVE_GROUP])
        cg.add(entity.set_parent(var))
        entity = await binary_sensor.new_binary_sensor(conf[CONF_CONNECTION])
        cg.add(var.set_dreamview_connection(entity))
        entity = await binary_sensor.new_binary_sensor(conf[CONF_PROBLEM])
        cg.add(var.set_dreamview_problem(entity))
        if CONF_STATUS in conf:
            entity = await text_sensor.new_text_sensor(conf[CONF_STATUS])
            cg.add(var.set_dreamview_status(entity))
    if CONF_TIME_ID in config:
        source = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(source))
    for index, color in enumerate(config.get(CONF_MUSIC_PALETTE, [])):
        cg.add(var.set_music_color(index, color))
    if CONF_MUSIC_COLORS in config:
        cg.add_define("USE_GOVEE_MUSIC_COLORS")
        palette = config.get(CONF_MUSIC_PALETTE, DEFAULT_MUSIC_PALETTE)
        for index, color_config in enumerate(config[CONF_MUSIC_COLORS]):
            color = palette[index]
            conf = dict(color_config)
            conf[CONF_INITIAL_STATE] = light.LIGHT_STATE_SCHEMA({
                CONF_STATE: True,
                CONF_BRIGHTNESS: 1.0,
                CONF_RED: ((color >> 16) & 0xFF) / 255.0,
                CONF_GREEN: ((color >> 8) & 0xFF) / 255.0,
                CONF_BLUE: (color & 0xFF) / 255.0,
                **conf.get(CONF_INITIAL_STATE, {}),
            })
            output = await light.new_light(conf)
            cg.add(output.set_parent(var))
            cg.add(output.set_color_index(index))
            cg.add(var.set_music_color_light(index, output))
    if CONF_MUSIC_SENSITIVITY in config:
        conf = config[CONF_MUSIC_SENSITIVITY]
        sensitivity = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(sensitivity, conf)
        await number.register_number(sensitivity, conf, min_value=0, max_value=99, step=1)
        cg.add(sensitivity.set_parent(var))
        cg.add(sensitivity.set_initial_value(conf[CONF_INITIAL_VALUE]))

    if CONF_SLEEP_TIMER in config:
        entity = await number.new_number(config[CONF_SLEEP_TIMER], min_value=0, max_value=240, step=1)
        cg.add(entity.set_parent(var))
        cg.add(var.set_sleep_timer(entity))
    for key in SLEEP_SENSORS:
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(entity))
    for key in SLEEP_TEXT_SENSORS:
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_text_sensor")(entity))


RGB_ACTION_FIELDS = {
    CONF_RED: cv.int_range(min=0, max=255),
    CONF_GREEN: cv.int_range(min=0, max=255),
    CONF_BLUE: cv.int_range(min=0, max=255),
}
ACTION_FIELDS = {
    "start_sleep": ("StartSleepAction", {
        CONF_MINUTES: cv.int_range(min=10, max=240),
    }),
    "cancel_sleep": ("CancelSleepAction", {}),
    "set_music_color": ("SetMusicColorAction", {
        CONF_COLOR_INDEX: cv.int_range(min=1, max=7),
        **RGB_ACTION_FIELDS,
    }),
    "set_music_background": ("SetMusicBackgroundAction", {
        **RGB_ACTION_FIELDS,
        CONF_BRIGHTNESS: cv.int_range(min=0, max=50),
    }),
    "set_music_gradient": ("SetMusicGradientAction", {CONF_GRADIENT: cv.boolean}),
    "set_custom_brightness_shape": ("SetCustomBrightnessShapeAction", {CONF_SHAPE: cv.int_range(min=0, max=5)}),
    "set_ab_brightness_shape": ("SetCustomBrightnessShapeAction", {CONF_SHAPE: cv.int_range(min=0, max=5)}),
    "set_segment": ("SetSegmentAction", {
        CONF_SEGMENT: cv.int_range(min=1, max=7),
        **RGB_ACTION_FIELDS,
        CONF_BRIGHTNESS: cv.int_range(min=0, max=100),
    }),
}


def register_govee_action(name, class_name, fields):
    action_class = ns.class_(class_name, automation.Action)
    schema = {cv.Required(CONF_ID): cv.use_id(GoveeSegmentsEffect if name == "set_segment" else GoveeLight)}
    schema.update({cv.Required(key): cv.templatable(validator) for key, validator in fields.items()})
    if name == "set_segment":
        schema[cv.Required(CONF_LIGHT_ID)] = cv.use_id(light.LightState)

    @automation.register_action(f"{DOMAIN}.{name}", action_class, cv.Schema(schema), synchronous=True)
    async def action_to_code(config, action_id, template_arg, args):
        parent = await cg.get_variable(config[CONF_ID])
        if name == "set_segment":
            state = await cg.get_variable(config[CONF_LIGHT_ID])
            var = cg.new_Pvariable(action_id, template_arg, parent, state)
        else:
            var = cg.new_Pvariable(action_id, template_arg, parent)
        for key in fields:
            value = await cg.templatable(config[key], args, cg.bool_ if key == CONF_GRADIENT else cg.int_)
            cg.add(getattr(var, f"set_{key}")(value))
        return var


for _name, (_class_name, _fields) in ACTION_FIELDS.items():
    register_govee_action(_name, _class_name, _fields)
