#include <application.h>

#define BATTERY_PUBLISH_INTERVAL (60 * 60 * 1000)
#define TEMPERATURE_PUBLISH_INTEVAL (15 * 60 * 1000)
#define TEMPERATURE_PUBLISH_VALUE_CHANGE 1.0f
#define TEMPERATURE_MEASURE_INTERVAL (5 * 1000)

#define PIR_PUB_MIN_INTERVAL  (1 * 60 * 1000)
#define PIR_SENSITIVITY 1

#define ACCELEROMETER_UPDATE_NORMAL_INTERVAL (5 * 1000)

#define PRESENCE_ENTER_THRESHOLD 4
#define PRESENCE_LEAVE_THRESHOLD 2
#define PRESENCE_INTERVAL (2 * 60 * 1000)

// LED instance
twr_led_t led;

// Button instance
twr_button_t button;

// Thermometer instance
twr_tmp112_t tmp112;
event_param_t temperature_event_param = { .next_pub = 0 };

// Accelerometer instance
twr_lis2dh12_t lis2dh12;

// Dice instance
twr_dice_t dice;

twr_module_pir_t pir;
uint16_t pir_event_count = 0;
twr_tick_t pir_next_pub = 0;

int pir_presence_count = 0;
bool presence_flag = false;

typedef struct Configuration
{
    uint8_t pir_sensitivity;
    twr_tick_t pir_pub_min_interval;

    struct
    {
        uint8_t threshold;
        twr_tick_t interval;
    } enter, leave;

    twr_tick_t temperature_measure_interval;
    twr_tick_t temperature_publish_interval;
    float temperature_publish_value_change;

    twr_tick_t accelerometer_measure_interval;

    twr_tick_t battery_publish_interval;

} Configuration;

Configuration config;

Configuration config_default = {
    .pir_sensitivity = PIR_SENSITIVITY,
    .pir_pub_min_interval = PIR_PUB_MIN_INTERVAL,
    .enter.threshold = PRESENCE_ENTER_THRESHOLD,
    .leave.threshold = PRESENCE_LEAVE_THRESHOLD,
    .enter.interval = PRESENCE_INTERVAL,

    .temperature_measure_interval = TEMPERATURE_MEASURE_INTERVAL,
    .temperature_publish_interval = TEMPERATURE_PUBLISH_INTEVAL,
    .temperature_publish_value_change = TEMPERATURE_PUBLISH_VALUE_CHANGE,

    .accelerometer_measure_interval = ACCELEROMETER_UPDATE_NORMAL_INTERVAL,

    .battery_publish_interval = BATTERY_PUBLISH_INTERVAL
};

// This function dispatches accelerometer events
void lis2dh12_event_handler(twr_lis2dh12_t *self, twr_lis2dh12_event_t event, void *event_param)
{
    // Update event?
    if (event == TWR_LIS2DH12_EVENT_UPDATE)
    {
        twr_lis2dh12_result_g_t result;

        // Successfully read accelerometer vectors?
        if (twr_lis2dh12_get_result_g(self, &result))
        {
            //twr_atci_printf("APP: Acceleration = [%.2f,%.2f,%.2f]", result.x_axis, result.y_axis, result.z_axis);

            // Update dice with new vectors
            twr_dice_feed_vectors(&dice, result.x_axis, result.y_axis, result.z_axis);

            // This variable holds last dice face
            static twr_dice_face_t last_face = TWR_DICE_FACE_UNKNOWN;

            // Get current dice face
            twr_dice_face_t face = twr_dice_get_face(&dice);

            // Did dice face change from last time?
            if (last_face != face)
            {
                // Remember last dice face
                last_face = face;

                // Convert dice face to integer
                int orientation = face;

                //twr_atci_printf("APP: Publish orientation = %d", orientation);

                // Publish orientation message on radio
                // Be careful, this topic is only development state, can be change in future.
                twr_radio_pub_int("orientation", &orientation);
            }
        }
    }
    // Error event?
    else if (event == TWR_LIS2DH12_EVENT_ERROR)
    {
        twr_atci_printf("APP: Accelerometer error");
    }
}


void button_event_handler(twr_button_t *self, twr_button_event_t event, void *event_param)
{
    (void) self;
    (void) event_param;

    if (event == TWR_BUTTON_EVENT_CLICK)
    {
        twr_led_pulse(&led, 100);
    }

    if (event == TWR_BUTTON_EVENT_HOLD)
    {
        config.pir_sensitivity++;

        if (config.pir_sensitivity > TWR_MODULE_PIR_SENSITIVITY_VERY_HIGH)
        {
            config.pir_sensitivity = TWR_MODULE_PIR_SENSITIVITY_LOW;
        }

        twr_config_save();

        twr_led_blink(&led, config.pir_sensitivity + 1);
    }
}

void battery_event_handler(twr_module_battery_event_t event, void *event_param)
{
    (void) event_param;

    float voltage;

    if (event == TWR_MODULE_BATTERY_EVENT_UPDATE)
    {
        if (twr_module_battery_get_voltage(&voltage))
        {
            twr_radio_pub_battery(&voltage);
        }
    }
}

void tmp112_event_handler(twr_tmp112_t *self, twr_tmp112_event_t event, void *event_param)
{
    float value;
    event_param_t *param = (event_param_t *)event_param;

    if (event == TWR_TMP112_EVENT_UPDATE)
    {
        if (twr_tmp112_get_temperature_celsius(self, &value))
        {
            if ((fabsf(value - param->value) >= config.temperature_publish_value_change) || (param->next_pub < twr_scheduler_get_spin_tick()))
            {
                twr_radio_pub_temperature(param->channel, &value);

                param->value = value;
                param->next_pub = twr_scheduler_get_spin_tick() + config.temperature_publish_interval;
            }
        }
    }
}

void pir_event_handler(twr_module_pir_t *self, twr_module_pir_event_t event, void *event_param)
{
    (void) self;
    (void) event_param;

    if (event == TWR_MODULE_PIR_EVENT_MOTION)
    {
        pir_event_count++;
        pir_presence_count++;

        twr_atci_printf("PIR Event Count %d", pir_event_count);

        if (config.pir_pub_min_interval != 0 && pir_next_pub < twr_scheduler_get_spin_tick())
        {
            twr_radio_pub_event_count(TWR_RADIO_PUB_EVENT_PIR_MOTION, &pir_event_count);

            pir_next_pub = twr_scheduler_get_spin_tick() + config.pir_pub_min_interval;

            twr_atci_printf("PIR Event Published %d", pir_event_count);
        }
    }
}

bool atci_config_set(twr_atci_param_t *param)
{

    char name[32+1];
    uint32_t value;

    if (!twr_atci_get_string(param, name, sizeof(name)))
    {
        return false;
    }

    if (!twr_atci_is_comma(param))
    {
        return false;
    }

    if (!twr_atci_get_uint(param, &value))
    {
        return false;
    }

    // PIR
    if (strncmp(name, "PIR Sensitivity", sizeof(name)) == 0)
    {
        config.pir_sensitivity = value;
        twr_module_pir_set_sensitivity(&pir, config.pir_sensitivity);
        return true;
    }

    if (strncmp(name, "PIR Publish Min Interval", sizeof(name)) == 0)
    {
        config.pir_pub_min_interval = value * 1000;
        return true;
    }

    // Presence
    if (strncmp(name, "Presence Enter Threshold", sizeof(name)) == 0)
    {
        config.enter.threshold = value;
        return true;
    }

    if (strncmp(name, "Presence Leave Threshold", sizeof(name)) == 0)
    {
        config.leave.threshold = value;
        return true;
    }

    if (strncmp(name, "Presence Interval", sizeof(name)) == 0)
    {
        config.enter.interval = value * 1000;
        twr_scheduler_plan_relative(0, config.enter.interval);
        return true;
    }

    // Temperature
    if (strncmp(name, "Temperature Measure Interval", sizeof(name)) == 0)
    {
        config.temperature_measure_interval = value * 1000;
        twr_tmp112_set_update_interval(&tmp112, config.temperature_measure_interval);
        return true;
    }

    if (strncmp(name, "Temperature Publish Interval", sizeof(name)) == 0)
    {
        config.temperature_publish_interval = value * 1000;
        return true;
    }

    if (strncmp(name, "Temperature Publish Value Change", sizeof(name)) == 0)
    {
        config.temperature_publish_value_change = value / 10.0;
        return true;
    }

    if (strncmp(name, "Accelerometer Measure Interval", sizeof(name)) == 0)
    {
        config.accelerometer_measure_interval = value * 1000;
        twr_lis2dh12_set_update_interval(&lis2dh12, config.accelerometer_measure_interval);
        return true;
    }

    if (strncmp(name, "Battery Publish Interval", sizeof(name)) == 0)
    {
        config.battery_publish_interval = value * 1000;
        twr_module_battery_set_update_interval(config.battery_publish_interval);
        return true;
    }

    return false;
}

bool atci_config_action(void)
{

    twr_atci_printf("$CONFIG: \"PIR Sensitivity\",%d", config.pir_sensitivity);
    twr_atci_printf("$CONFIG: \"PIR Publish Min Interval\",%lld", config.pir_pub_min_interval / 1000);

    twr_atci_printf("$CONFIG: \"Presence Enter Threshold\",%d", config.enter.threshold);
    twr_atci_printf("$CONFIG: \"Presence Leave Threshold\",%d", config.leave.threshold);
    twr_atci_printf("$CONFIG: \"Presence Interval\",%lld", config.enter.interval / 1000);

    twr_atci_printf("$CONFIG: \"Temperature Measure Interval\",%lld", config.temperature_measure_interval / 1000);
    twr_atci_printf("$CONFIG: \"Temperature Publish Interval\",%lld", config.temperature_publish_interval / 1000);
    twr_atci_printf("$CONFIG: \"Temperature Publish Value Change\",%.1f", config.temperature_publish_value_change);

    twr_atci_printf("$CONFIG: \"Accelerometer Measure Interval\",%lld", config.accelerometer_measure_interval / 1000);

    twr_atci_printf("$CONFIG: \"Battery Publish Interval\",%lld", config.battery_publish_interval / 1000);

    return true;
}

bool atci_f_action(void)
{
    twr_config_reset();

    return true;
}

bool atci_w_action(void)
{
    twr_config_save();

    return true;
}

void application_init(void)
{
    twr_config_init(0x1234, &config, sizeof(config), &config_default);

    // Initialize LED
    twr_led_init(&led, TWR_GPIO_LED, false, false);
    twr_led_set_mode(&led, TWR_LED_MODE_OFF);

    twr_radio_init(TWR_RADIO_MODE_NODE_SLEEPING);

    // Initialize button
    twr_button_init(&button, TWR_GPIO_BUTTON, TWR_GPIO_PULL_DOWN, false);
    twr_button_set_scan_interval(&button, 20);
    twr_button_set_hold_time(&button, 1000);
    twr_button_set_event_handler(&button, button_event_handler, NULL);

    // Initialize battery
    twr_module_battery_init();
    twr_module_battery_set_event_handler(battery_event_handler, NULL);
    twr_module_battery_set_update_interval(config.battery_publish_interval);

    // Initialize thermometer sensor on core module
    temperature_event_param.channel = TWR_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_ALTERNATE;
    twr_tmp112_init(&tmp112, TWR_I2C_I2C0, 0x49);
    twr_tmp112_set_event_handler(&tmp112, tmp112_event_handler, &temperature_event_param);
    twr_tmp112_set_update_interval(&tmp112, config.temperature_measure_interval);

    // Initialize pir module
    twr_module_pir_init(&pir);
    twr_module_pir_set_event_handler(&pir, pir_event_handler, NULL);
    twr_module_pir_set_sensitivity(&pir, TWR_MODULE_PIR_SENSITIVITY_MEDIUM);

    // Initialize accelerometer
    twr_lis2dh12_init(&lis2dh12, TWR_I2C_I2C0, 0x19);
    twr_lis2dh12_set_event_handler(&lis2dh12, lis2dh12_event_handler, NULL);
    twr_lis2dh12_set_update_interval(&lis2dh12, config.accelerometer_measure_interval);

    // Initialize dice
    twr_dice_init(&dice, TWR_DICE_FACE_UNKNOWN);

    static const twr_atci_command_t commands[] = {
            { "&F", atci_f_action, NULL, NULL, NULL, "Restore configuration to factory defaults" },
            { "&W", atci_w_action, NULL, NULL, NULL, "Store configuration to non-volatile memory" },
            { "$CONFIG", atci_config_action, atci_config_set, NULL, NULL, "Get/set config parameters"},
            TWR_ATCI_COMMAND_CLAC,
            TWR_ATCI_COMMAND_HELP
    };
    twr_atci_init(commands, TWR_ATCI_COMMANDS_LENGTH(commands));

    twr_radio_pairing_request("motion-detector", FW_VERSION);

    // Replan application_task to the first interval
    twr_scheduler_plan_relative(0, config.enter.interval);

    twr_led_pulse(&led, 2000);
}

void application_task()
{
    if (config.enter.interval == 0)
    {
        return;
    }

    if (presence_flag == false && pir_presence_count >= config.enter.threshold)
    {
        presence_flag = true;
    }

    if (presence_flag == true && pir_presence_count <= config.leave.threshold)
    {
        presence_flag = false;
    }

    int presence_publish = presence_flag ? 1 : 0;

    twr_radio_pub_int("presence/-/state", &presence_publish);
    twr_radio_pub_int("presence/-/events", &pir_presence_count);

    twr_atci_printf("Presence: %d, Presence Event Count %d", presence_flag, pir_presence_count);

    pir_presence_count = 0;

    twr_scheduler_plan_current_relative(config.enter.interval);
}
