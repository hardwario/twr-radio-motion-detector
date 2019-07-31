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
bc_led_t led;

// Button instance
bc_button_t button;

// Thermometer instance
bc_tmp112_t tmp112;
event_param_t temperature_event_param = { .next_pub = 0 };

// Accelerometer instance
bc_lis2dh12_t lis2dh12;

// Dice instance
bc_dice_t dice;

bc_module_pir_t pir;
uint16_t pir_event_count = 0;
bc_tick_t pir_next_pub = 0;

int pir_presence_count = 0;
bool presence_flag = false;

typedef struct Configuration
{
    uint8_t pir_sensitivity;
    bc_tick_t pir_pub_min_interval;

    struct
    {
        uint8_t threshold;
        bc_tick_t interval;
    } enter, leave;

    bc_tick_t temperature_measure_interval;
    bc_tick_t temperature_publish_interval;
    float temperature_publish_value_change;

    bc_tick_t accelerometer_measure_interval;

    bc_tick_t battery_publish_interval;

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
void lis2dh12_event_handler(bc_lis2dh12_t *self, bc_lis2dh12_event_t event, void *event_param)
{
    // Update event?
    if (event == BC_LIS2DH12_EVENT_UPDATE)
    {
        bc_lis2dh12_result_g_t result;

        // Successfully read accelerometer vectors?
        if (bc_lis2dh12_get_result_g(self, &result))
        {
            //bc_atci_printf("APP: Acceleration = [%.2f,%.2f,%.2f]", result.x_axis, result.y_axis, result.z_axis);

            // Update dice with new vectors
            bc_dice_feed_vectors(&dice, result.x_axis, result.y_axis, result.z_axis);

            // This variable holds last dice face
            static bc_dice_face_t last_face = BC_DICE_FACE_UNKNOWN;

            // Get current dice face
            bc_dice_face_t face = bc_dice_get_face(&dice);

            // Did dice face change from last time?
            if (last_face != face)
            {
                // Remember last dice face
                last_face = face;

                // Convert dice face to integer
                int orientation = face;

                //bc_atci_printf("APP: Publish orientation = %d", orientation);

                // Publish orientation message on radio
                // Be careful, this topic is only development state, can be change in future.
                bc_radio_pub_int("orientation", &orientation);
            }
        }
    }
    // Error event?
    else if (event == BC_LIS2DH12_EVENT_ERROR)
    {
        bc_atci_printf("APP: Accelerometer error");
    }
}


void button_event_handler(bc_button_t *self, bc_button_event_t event, void *event_param)
{
    (void) self;
    (void) event_param;

    if (event == BC_BUTTON_EVENT_CLICK)
    {
        bc_led_pulse(&led, 100);
    }

    if (event == BC_BUTTON_EVENT_HOLD)
    {
        config.pir_sensitivity++;

        if (config.pir_sensitivity > BC_MODULE_PIR_SENSITIVITY_VERY_HIGH)
        {
            config.pir_sensitivity = BC_MODULE_PIR_SENSITIVITY_LOW;
        }

        bc_config_save();

        bc_led_blink(&led, config.pir_sensitivity + 1);
    }
}

void battery_event_handler(bc_module_battery_event_t event, void *event_param)
{
    (void) event_param;

    float voltage;

    if (event == BC_MODULE_BATTERY_EVENT_UPDATE)
    {
        if (bc_module_battery_get_voltage(&voltage))
        {
            bc_radio_pub_battery(&voltage);
        }
    }
}

void tmp112_event_handler(bc_tmp112_t *self, bc_tmp112_event_t event, void *event_param)
{
    float value;
    event_param_t *param = (event_param_t *)event_param;

    if (event == BC_TMP112_EVENT_UPDATE)
    {
        if (bc_tmp112_get_temperature_celsius(self, &value))
        {
            if ((fabsf(value - param->value) >= config.temperature_publish_value_change) || (param->next_pub < bc_scheduler_get_spin_tick()))
            {
                bc_radio_pub_temperature(param->channel, &value);

                param->value = value;
                param->next_pub = bc_scheduler_get_spin_tick() + config.temperature_publish_interval;
            }
        }
    }
}

void pir_event_handler(bc_module_pir_t *self, bc_module_pir_event_t event, void *event_param)
{
    (void) self;
    (void) event_param;

    if (event == BC_MODULE_PIR_EVENT_MOTION)
    {
        pir_event_count++;
        pir_presence_count++;

        bc_atci_printf("PIR Event Count %d", pir_event_count);

        if (config.pir_pub_min_interval != 0 && pir_next_pub < bc_scheduler_get_spin_tick())
        {
            bc_radio_pub_event_count(BC_RADIO_PUB_EVENT_PIR_MOTION, &pir_event_count);

            pir_next_pub = bc_scheduler_get_spin_tick() + config.pir_pub_min_interval;

            bc_atci_printf("PIR Event Published %d", pir_event_count);
        }
    }
}



bool atci_config_set(bc_atci_param_t *param)
{

    char name[32+1];
    uint32_t value;

    if (!bc_atci_get_string(param, name, sizeof(name)))
    {
        return false;
    }

    if (!bc_atci_is_comma(param))
    {
        return false;
    }

    if (!bc_atci_get_uint(param, &value))
    {
        return false;
    }

    // PIR
    if (strncmp(name, "PIR Sensitivity", sizeof(name)) == 0)
    {
        config.pir_sensitivity = value;
        bc_module_pir_set_sensitivity(&pir, config.pir_sensitivity);
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
        bc_scheduler_plan_relative(0, config.enter.interval);
        return true;
    }

    // Temperature
    if (strncmp(name, "Temperature Measure Interval", sizeof(name)) == 0)
    {
        config.temperature_measure_interval = value * 1000;
        bc_tmp112_set_update_interval(&tmp112, config.temperature_measure_interval);
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
        bc_lis2dh12_set_update_interval(&lis2dh12, config.accelerometer_measure_interval);
        return true;
    }

    if (strncmp(name, "Battery Publish Interval", sizeof(name)) == 0)
    {
        config.battery_publish_interval = value * 1000;
        bc_module_battery_set_update_interval(config.battery_publish_interval);
        return true;
    }

    return false;
}

bool atci_config_action(void)
{

    bc_atci_printf("$CONFIG: \"PIR Sensitivity\",%d", config.pir_sensitivity);
    bc_atci_printf("$CONFIG: \"PIR Publish Min Interval\",%lld", config.pir_pub_min_interval / 1000);

    bc_atci_printf("$CONFIG: \"Presence Enter Threshold\",%d", config.enter.threshold);
    bc_atci_printf("$CONFIG: \"Presence Leave Threshold\",%d", config.leave.threshold);
    bc_atci_printf("$CONFIG: \"Presence Interval\",%lld", config.enter.interval / 1000);

    bc_atci_printf("$CONFIG: \"Temperature Measure Interval\",%lld", config.temperature_measure_interval / 1000);
    bc_atci_printf("$CONFIG: \"Temperature Publish Interval\",%lld", config.temperature_publish_interval / 1000);
    bc_atci_printf("$CONFIG: \"Temperature Publish Value Change\",%.1f", config.temperature_publish_value_change);

    bc_atci_printf("$CONFIG: \"Accelerometer Measure Interval\",%lld", config.accelerometer_measure_interval / 1000);

    bc_atci_printf("$CONFIG: \"Battery Publish Interval\",%lld", config.battery_publish_interval / 1000);

    return true;
}

bool atci_f_action(void)
{
    bc_config_reset();

    return true;
}

bool atci_w_action(void)
{
    bc_config_save();

    return true;
}

void application_init(void)
{
    bc_config_init(0x1234, &config, sizeof(config), &config_default);

    // Initialize LED
    bc_led_init(&led, BC_GPIO_LED, false, false);
    bc_led_set_mode(&led, BC_LED_MODE_OFF);

    bc_radio_init(BC_RADIO_MODE_NODE_SLEEPING);

    // Initialize button
    bc_button_init(&button, BC_GPIO_BUTTON, BC_GPIO_PULL_DOWN, false);
    bc_button_set_scan_interval(&button, 20);
    bc_button_set_hold_time(&button, 1000);
    bc_button_set_event_handler(&button, button_event_handler, NULL);

    // Initialize battery
    bc_module_battery_init();
    bc_module_battery_set_event_handler(battery_event_handler, NULL);
    bc_module_battery_set_update_interval(config.battery_publish_interval);

    // Initialize thermometer sensor on core module
    temperature_event_param.channel = BC_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_ALTERNATE;
    bc_tmp112_init(&tmp112, BC_I2C_I2C0, 0x49);
    bc_tmp112_set_event_handler(&tmp112, tmp112_event_handler, &temperature_event_param);
    bc_tmp112_set_update_interval(&tmp112, config.temperature_measure_interval);

    // Initialize pir module
    bc_module_pir_init(&pir);
    bc_module_pir_set_event_handler(&pir, pir_event_handler, NULL);
    bc_module_pir_set_sensitivity(&pir, BC_MODULE_PIR_SENSITIVITY_MEDIUM);

    // Initialize accelerometer
    bc_lis2dh12_init(&lis2dh12, BC_I2C_I2C0, 0x19);
    bc_lis2dh12_set_event_handler(&lis2dh12, lis2dh12_event_handler, NULL);
    bc_lis2dh12_set_update_interval(&lis2dh12, config.accelerometer_measure_interval);

    // Initialize dice
    bc_dice_init(&dice, BC_DICE_FACE_UNKNOWN);

    static const bc_atci_command_t commands[] = {
            { "&F", atci_f_action, NULL, NULL, NULL, "Restore configuration to factory defaults" },
            { "&W", atci_w_action, NULL, NULL, NULL, "Store configuration to non-volatile memory" },
            { "$CONFIG", atci_config_action, atci_config_set, NULL, NULL, "Get/set config parameters"},
            BC_ATCI_COMMAND_CLAC,
            BC_ATCI_COMMAND_HELP
    };
    bc_atci_init(commands, BC_ATCI_COMMANDS_LENGTH(commands));

    bc_radio_pairing_request("motion-detector", VERSION);

    // Replan application_task to the first interval
    bc_scheduler_plan_relative(0, config.enter.interval);

    bc_led_pulse(&led, 2000);
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

    bc_radio_pub_int("presence/-/state", &presence_publish);
    bc_radio_pub_int("presence/-/events", &pir_presence_count);

    bc_atci_printf("Presence: %d, Presence Event Count %d", presence_flag, pir_presence_count);

    pir_presence_count = 0;

    bc_scheduler_plan_current_relative(config.enter.interval);
}
