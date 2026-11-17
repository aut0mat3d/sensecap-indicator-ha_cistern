#include "cmd.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"
#include "esp_console.h"

#include "home_assistant_config.h"
#include "ha.h"
#include "storage_nvs.h"
#include "indicator_util.h"

#include "rp2040.h"
#include "view_data.h"
#include "chart_query_scheduler.h"
#include "alarm_model.h"
#include "alarm_view.h"
#include "bsp_lcd.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "CMD_RESP";

#define PROMPT_STR "Indicator"

ESP_EVENT_DEFINE_BASE(CMD_CFG_EVENT_BASE);
esp_event_loop_handle_t cmd_cfg_event_handle;

static ha_cfg_interface ha_cfg;

/* ── HomeAssistant & MQTT Commands ─────────────────────────────────────── */

static void print_mqtt_usage(void) {
    printf("\nMQTT configuration\n");
    printf("  Show current config:\n");
    printf("    haconfig\n\n");
    printf("  Set broker, client id and credentials:\n");
    printf("    setmqtt -a 192.168.1.10 -c indicator-01 -u mqtt_user -p mqtt_password\n");
    printf("    setmqtt --addr mqtt://192.168.1.10:1883\n");
    printf("    setmqtt --addr mqtt://broker.emqx.io\n\n");
    printf("  Notes:\n");
    printf("    - The screen MQTT page only asks for the broker IP. It builds mqtt://<ip>:1883.\n");
    printf("    - Restart is automatic after setmqtt succeeds.\n\n");
    printf("MQTT topics and payloads\n");
    printf("  Sensor data from device:\n");
    printf("    topic: %s\n", CONFIG_TOPIC_SENSOR_DATA);
    printf("    data : {\"temp\":\"23.5\"}, {\"humidity\":\"55\"}, {\"co2\":\"600\"}, {\"tvoc\":\"12\"}\n\n");
    printf("  Control device from Home Assistant/MQTT client:\n");
    printf("    topic: %s\n", CONFIG_TOPIC_SWITCH_SET);
    printf("    data : {\"switch1\":1}, {\"switch1\":0}, {\"switch5\":24}, {\"switch8\":50}\n\n");
    printf("  Device publishes control state:\n");
    printf("    topic: %s\n", CONFIG_TOPIC_SWITCH_STATE);
    printf("    data : {\"switch1\":1}, {\"switch5\":24}, {\"switch8\":50}\n\n");
}

static bool normalize_broker_url(const char *input, char *output, size_t output_size) {
    if (!input || input[0] == '\0' || !output || output_size == 0) return false;

    int written = 0;
    if (strncmp(input, "mqtt://", 7) == 0 || strncmp(input, "mqtts://", 8) == 0)
        written = snprintf(output, output_size, "%s", input);
    else
        written = snprintf(output, output_size, "mqtt://%s", input);

    return written > 0 && (size_t)written < output_size;
}

static int read_ha_config(int argc, char **argv) {
    ha_cfg_get(&ha_cfg);
    ESP_LOGI(TAG, "| Broker Address               | %-40s |", ha_cfg.broker_url);
    ESP_LOGI(TAG, "| Client ID                    | %-40s |", ha_cfg.client_id);
    ESP_LOGI(TAG, "| MQTT username                | %-40s |", ha_cfg.username);
    ESP_LOGI(TAG, "| MQTT password                | %-40s |", ha_cfg.password);
    ESP_LOGI(TAG, "Run 'mqtthelp' for setmqtt examples and MQTT topic/payload examples.");
    return 0;
}

static int mqtt_help(int argc, char **argv) {
    print_mqtt_usage();
    return 0;
}

static void register_read_config(void) {
    const esp_console_cmd_t cmd = {
        .command = "haconfig",
        .help    = "Show current MQTT broker/client configuration",
        .hint    = NULL,
        .func    = &read_ha_config,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void register_mqtt_help(void) {
    const esp_console_cmd_t cmd = {
        .command = "mqtthelp",
        .help    = "Show MQTT setup examples, topics and payloads",
        .hint    = NULL,
        .func    = &mqtt_help,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

struct {
    struct arg_str *username;
    struct arg_str *password;
    struct arg_str *broker_url;
    struct arg_str *client_id;
    struct arg_end *end;
} mqtt_args;

static int mqtt_config_set(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&mqtt_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, mqtt_args.end, argv[0]);
        print_mqtt_usage();
        return 1;
    }

    if (!mqtt_args.username->count && !mqtt_args.password->count &&
        !mqtt_args.broker_url->count && !mqtt_args.client_id->count) {
        print_mqtt_usage();
        return 0;
    }

    ha_cfg_get(&ha_cfg);

    if (mqtt_args.username->count > 0) {
        memset(ha_cfg.username, 0, sizeof(ha_cfg.username));
        strncpy(ha_cfg.username, mqtt_args.username->sval[0], sizeof(ha_cfg.username) - 1);
        ESP_LOGI(TAG, "Set MQTT username: %s", ha_cfg.username);
    }
    if (mqtt_args.password->count > 0) {
        memset(ha_cfg.password, 0, sizeof(ha_cfg.password));
        strncpy(ha_cfg.password, mqtt_args.password->sval[0], sizeof(ha_cfg.password) - 1);
        ESP_LOGI(TAG, "Set MQTT password: %s", ha_cfg.password);
    }
    if (mqtt_args.broker_url->count > 0) {
        char broker_url[sizeof(ha_cfg.broker_url)];
        if (!normalize_broker_url(mqtt_args.broker_url->sval[0], broker_url, sizeof(broker_url))) {
            ESP_LOGE(TAG, "Invalid or too long broker URL: %s", mqtt_args.broker_url->sval[0]);
            return 1;
        }
        memset(ha_cfg.broker_url, 0, sizeof(ha_cfg.broker_url));
        strncpy(ha_cfg.broker_url, broker_url, sizeof(ha_cfg.broker_url) - 1);
        ESP_LOGI(TAG, "Set MQTT broker URL: %s", ha_cfg.broker_url);
    }
    if (mqtt_args.client_id->count > 0) {
        memset(ha_cfg.client_id, 0, sizeof(ha_cfg.client_id));
        strncpy(ha_cfg.client_id, mqtt_args.client_id->sval[0], sizeof(ha_cfg.client_id) - 1);
        ESP_LOGI(TAG, "Set MQTT client ID: %s", ha_cfg.client_id);
    }

    if (ha_cfg_set(&ha_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MQTT configuration");
        return 1;
    }
    esp_event_post_to(ha_cfg_event_handle, HA_CFG_EVENT_BASE, HA_CFG_SET, NULL, 0, portMAX_DELAY);
    ESP_LOGI(TAG, "MQTT configuration saved. Reconnecting MQTT client.");
    return 0;
}

static void register_mqtt_config(void) {
    mqtt_args.username   = arg_str0("u", "usr", "<username>", "MQTT username");
    mqtt_args.password   = arg_str0("p", "psw", "<password>", "MQTT password");
    mqtt_args.broker_url = arg_str0("a", "addr", "<broker_url>",
                                    "MQTT broker URL, e.g. 192.168.1.10 or mqtt://host:1883");
    mqtt_args.client_id  = arg_str0("c", "id", "<client_id>", "MQTT client ID");
    mqtt_args.end        = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command  = "setmqtt",
        .help     = "Set MQTT config. Example: setmqtt -a 192.168.1.10 -c indicator-01 -u user -p pass",
        .hint     = NULL,
        .func     = &mqtt_config_set,
        .argtable = &mqtt_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── Telemetry & Historical Debugging Commands ─────────────────────────── */

static int cmd_telem_inject(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: telem_inject <dev_hex> <key> <val> [unit]\n");
        return 1;
    }

    uint32_t dev_addr = (uint32_t)strtoul(argv[1], NULL, 16);
    const char *key = argv[2];
    float val = strtof(argv[3], NULL);
    const char *unit = (argc >= 5) ? argv[4] : "";

    struct view_data_lora_telemetry telem = {0};
    telem.origin = DATA_ORIGIN_LORA;
    telem.dev_addr = dev_addr;
    telem.fcnt = 0;
    telem.val_type = LORA_VAL_TYPE_FLOAT;
    telem.val.f_val = val;
    telem.context = 0; /* Measurement */
    strlcpy(telem.key, key, sizeof(telem.key));
    strlcpy(telem.unit, unit, sizeof(telem.unit));

    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                      VIEW_EVENT_LORA_TELEMETRY, &telem,
                      sizeof(telem), portMAX_DELAY);

    printf("Injected: DevAddr=0x%08lX, Key='%s', Val=%.2f %s\n",
           (unsigned long)dev_addr, key, val, unit);
    return 0;
}

static void register_telem_inject(void) {
    const esp_console_cmd_t cmd = {
        .command = "telem_inject",
        .help    = "Inject telemetry value directly into event loop and UI",
        .hint    = "<dev_hex> <key> <val> [unit]",
        .func    = &cmd_telem_inject,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static int cmd_chart_poll(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: chart_poll <dev_hex> <key> <zone_0_to_2>\n");
        return 1;
    }

    uint32_t dev_addr = (uint32_t)strtoul(argv[1], NULL, 16);
    const char *key = argv[2];
    uint8_t zone_idx = (uint8_t)atoi(argv[3]);

    if (zone_idx > 2) {
        printf("Error: zone must be 0 (7d), 1 (24h), or 2 (55m)\n");
        return 1;
    }

    bool ok = chart_query_scheduler_trigger_poll(dev_addr, key, zone_idx);
    printf("Chart poll for 0x%08lX '%s' Zone %u: %s\n",
           (unsigned long)dev_addr, key, zone_idx, ok ? "ENQUEUED" : "FAILED (Not found)");
    return ok ? 0 : 1;
}

static void register_chart_poll(void) {
    const esp_console_cmd_t cmd = {
        .command = "chart_poll",
        .help    = "Trigger historical query via FIFO scheduler",
        .hint    = "<dev_hex> <key> <zone_0_to_2>",
        .func    = &cmd_chart_poll,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static int cmd_alarm_mute(int argc, char **argv) {
    uint32_t sec = 7200; /* Default: 2 hours */
    if (argc >= 2) {
        sec = (uint32_t)strtoul(argv[1], NULL, 10);
    }

    alarm_mute(sec);
    alarm_buzzer_stop();

    if (sec == 0) {
        printf("Alarms muted permanently.\n");
    } else {
        printf("Alarms muted for %lu seconds (%lu minutes).\n", 
               (unsigned long)sec, (unsigned long)(sec / 60));
    }
    return 0;
}

static void register_alarm_mute(void) {
    const esp_console_cmd_t cmd = {
        .command = "alarm_mute",
        .help    = "Mute alarm buzzer (default: 7200s, 0=indefinite)",
        .hint    = "[seconds]",
        .func    = &cmd_alarm_mute,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static int cmd_alarm_ack(int argc, char **argv) {
    alarm_view_acknowledge_all();
    printf("All active alarms acknowledged.\n");
    return 0;
}

static void register_alarm_ack(void) {
    const esp_console_cmd_t cmd = {
        .command = "alarm_ack",
        .help    = "Acknowledge all active alarms and stop buzzer",
        .hint    = NULL,
        .func    = &cmd_alarm_ack,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── Display diagnostics: lcd_restart / lcd_rgb_reinit ────────────────── */
/*
 * Used while the ~15 px horizontal display shift is visible, to locate where it lives:
 *  - lcd_restart: resynchronizes only the ESP32 side (GDMA + LCD FIFO restart at the next VSYNC),
 *                 the panel is not touched. Fixes the image -> shift is in the ESP32 data stream.
 *  - (panel reset + re-init was tested on 2026-11-17 and did NOT fix the shift; it is not offered
 *    as a command because the panel's 3-wire SPI shares GPIO41/48 with the SX1262 SPI bus and
 *    re-running lcd_panel_st7701s_init() at runtime takes these pins away from the radio)
 *  - lcd_rgb_reinit: full stop/reset/start of the ESP32 LCD engine. Fixes the image while
 *                 lcd_restart does not -> the engine/FIFO lost alignment in the driver's
 *                 restart path (ESP32-S3 restart workaround) or during an underrun.
 */
static int cmd_lcd_restart(int argc, char **argv) {
    esp_lcd_panel_handle_t panel = bsp_lcd_get_panel_handle();
    if (!panel) {
        printf("No LCD panel handle\n");
        return 1;
    }
    esp_err_t err = esp_lcd_rgb_panel_restart(panel);
    printf("RGB stream restart requested (applied at next VSYNC): %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_lcd_rgb_reinit(int argc, char **argv) {
    esp_lcd_panel_handle_t panel = bsp_lcd_get_panel_handle();
    if (!panel) {
        printf("No LCD panel handle\n");
        return 1;
    }
    /* Full restart of the ESP32 LCD engine (stop, reset, FIFO reset, bounce prefill, DMA start,
     * engine start) - unlike lcd_restart, which only restarts GDMA/FIFO while the engine keeps
     * running. Rendering is paused meanwhile so LVGL does not swap frame buffers in between. */
    lvgl_port_lock(0);
    esp_err_t err = esp_lcd_panel_init(panel);
    lvgl_port_unlock();
    printf("RGB engine fully restarted: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static void register_lcd_diag(void) {
    const esp_console_cmd_t rgb_reinit_cmd = {
        .command = "lcd_rgb_reinit",
        .help    = "Diagnostics: full restart of the ESP32 RGB LCD engine (stop/reset/start), panel untouched",
        .hint    = NULL,
        .func    = &cmd_lcd_rgb_reinit,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&rgb_reinit_cmd));

    const esp_console_cmd_t restart_cmd = {
        .command = "lcd_restart",
        .help    = "Diagnostics: restart the ESP32 RGB stream (GDMA/FIFO) at the next VSYNC, panel untouched",
        .hint    = NULL,
        .func    = &cmd_lcd_restart,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&restart_cmd));

}

/* ── Command: lora_inject ──────────────────────────────────────────────── */

static int cmd_lora_inject(int argc, char **argv) {
    if (argc < 3) {
        printf("\nUsage:\n");
        printf("  lora_inject <dev_hex> [fport] <hex_payload>\n\n");
        printf("Examples:\n");
        printf("  lora_inject 05975DC9 13 5503770100   (Explicit FPort 13)\n");
        printf("  lora_inject 05975DC9 5503770100      (Default FPort 13)\n");
        printf("  lora_inject 05975DC9 13 5503770A00   (10 cm raw + 95 cm offset = 105 cm)\n\n");
        return 1;
    }

    uint32_t dev_addr = (uint32_t)strtoul(argv[1], NULL, 16);
    uint8_t fport = 13;
    const char *hex_str = NULL;

    if (argc >= 4) {
        fport = (uint8_t)strtoul(argv[2], NULL, 0);
        hex_str = argv[3];
    } else {
        hex_str = argv[2];
    }

    if (strncmp(hex_str, "0x", 2) == 0 || strncmp(hex_str, "0X", 2) == 0) {
        hex_str += 2;
    }

    size_t hex_len = strlen(hex_str);
    if (hex_len == 0 || hex_len % 2 != 0) {
        printf("Error: hex_payload must have an even number of hex characters.\n");
        return 1;
    }

    size_t frm_len = hex_len / 2;
    if (frm_len > 180) {
        printf("Error: Payload length (%u bytes) exceeds maximum limit (180 bytes).\n", (unsigned)frm_len);
        return 1;
    }

    /* Construct standard LoRaWAN PHY frame (13 overhead bytes + decrypted payload) */
    uint8_t phy_payload[200] = {0};
    static uint16_t s_inject_fcnt = 1000;

    phy_payload[0] = 0x40; /* MHDR: Unconfirmed Data Up */
    phy_payload[1] = (uint8_t)(dev_addr & 0xFF);
    phy_payload[2] = (uint8_t)((dev_addr >> 8) & 0xFF);
    phy_payload[3] = (uint8_t)((dev_addr >> 16) & 0xFF);
    phy_payload[4] = (uint8_t)((dev_addr >> 24) & 0xFF);
    phy_payload[5] = 0x00; /* FCtrl */

    s_inject_fcnt++;
    phy_payload[6] = (uint8_t)(s_inject_fcnt & 0xFF);
    phy_payload[7] = (uint8_t)((s_inject_fcnt >> 8) & 0xFF);
    phy_payload[8] = fport;

    for (size_t i = 0; i < frm_len; i++) {
        char byte_buf[3] = { hex_str[i * 2], hex_str[i * 2 + 1], '\0' };
        phy_payload[9 + i] = (uint8_t)strtoul(byte_buf, NULL, 16);
    }

    /* 4 dummy MIC bytes */
    phy_payload[9 + frm_len + 0] = 0xAA;
    phy_payload[9 + frm_len + 1] = 0xBB;
    phy_payload[9 + frm_len + 2] = 0xCC;
    phy_payload[9 + frm_len + 3] = 0xDD;

    uint8_t phy_len = (uint8_t)(9 + frm_len + 4);

    int ret = rp2040_send_lora_raw_frame(dev_addr, -45, 9, phy_payload, phy_len);
    if (ret > 0) {
        printf("LoRa frame injected to RP2040: DevAddr=0x%08lX, FPort=%u, Payload=%u bytes, FCnt=%u\n",
               (unsigned long)dev_addr, fport, (unsigned)frm_len, s_inject_fcnt);
        return 0;
    } else {
        printf("Error: Failed to transmit UART frame to RP2040 co-processor.\n");
        return 1;
    }
}

static void register_lora_inject(void) {
    const esp_console_cmd_t cmd = {
        .command = "lora_inject",
        .help    = "Inject decrypted LoRa payload into RP2040 decoder cascade",
        .hint    = "<dev_hex> [fport] <hex_payload>",
        .func    = &cmd_lora_inject,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── Console REPL Lifecycle ────────────────────────────────────────────── */

int indicator_cmd_init(void) {
    esp_console_repl_t       *repl        = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt                    = PROMPT_STR "> ";
    repl_config.max_cmdline_length        = 1024;

    esp_event_loop_args_t cmd_event_task_args = {
        .queue_size      = 2,
        .task_name       = "cmd_event_task",
        .task_priority   = uxTaskPriorityGet(NULL),
        .task_stack_size = 4096,
        .task_core_id    = tskNO_AFFINITY,
    };
    ESP_ERROR_CHECK(esp_event_loop_create(&cmd_event_task_args, &cmd_cfg_event_handle));

    /* 1. Register Built-in Console Help */
    ESP_ERROR_CHECK(esp_console_register_help_command());

    /* 2. HomeAssistant & MQTT Commands */
    ha_cfg_get(&ha_cfg);
    register_read_config();
    register_mqtt_help();
    register_mqtt_config();

    /* 3. Telemetry, Chart & Alarm Debug Commands */
    register_telem_inject();
    register_lora_inject();
    register_chart_poll();
    register_alarm_mute();
    register_alarm_ack();
    register_lcd_diag();

    /* 4. Start UART REPL Driver */
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    return ESP_OK;
}