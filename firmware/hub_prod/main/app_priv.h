/*
 * app_priv.h — RO Monitor production hub: pins, protocol, thresholds, types
 *
 * Every GPIO here is the as-built hub. docs/check_pinmap.py holds WIRING.md §1,
 * HARDWARE.md §3.1 and the phase-B spec §3.5 to the same table; if a pin moves,
 * it moves in all three plus this file, in one commit.
 *
 * The RS485 frame layout and CRC are docs/RS485_PROTOCOL.md, and
 * docs/check_frame.py fails the build if this file and the node firmware
 * disagree about either.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ identity */
#define FW_VERSION              "1.0.0"
#define NODE_TYPE               "Water Plant Monitor"
#define NODE_PROJECT            "RO Monitor"

/* ------------------------------------------------------------------ GPIO map */
/* RS485 master — XY-485 auto-flow, so no DE/RE line to drive. */
#define GPIO_RS485_RX           16
#define GPIO_RS485_TX           17
#define RS485_UART              UART_NUM_2
#define RS485_BAUD              9600

/* Local SHT30 (RO room). 4.7k pull-ups are on the GY-SHT30-D board. */
#define GPIO_I2C_SDA            21
#define GPIO_I2C_SCL            22
#define SHT30_ADDR              0x44

/* PC817 dry-contact channels. As-built channel order is a one-place rotation of
 * the order the docs carried until 2026-08-27 — see WIRING.md §5. Verified by
 * injecting 5V into each input and reading the label back. */
#define GPIO_IN_RL1_STAT        26   /* PC817 ch1 — Aster RL1 / multiport valve */
#define GPIO_IN_RL2_STAT        25   /* PC817 ch2 — Aster RL2                   */
#define GPIO_IN_ALARM           33   /* PC817 ch3 — Aster AUX OP, NO, closes on fault */
#define GPIO_IN_TWT_FLOT        32   /* PC817 ch4 — Aster TWT FLOTY loop        */

/* 240 V opto modules. Read as ANALOG, not digital — see app_sensors.c. Both are
 * ADC1, so they keep working with Wi-Fi up. The module's own 47k pull-up to VCC
 * idles them high; there is no external resistor and never was (WIRING.md §8). */
#define GPIO_IN_HPP_AC          34   /* ADC1_CH6 */
#define GPIO_IN_RWP_AC          35   /* ADC1_CH7 */

/* SCT-013-030 current clamps, on the hub's last two free ADC1 pins. */
#define GPIO_IN_HPP_CT          36   /* ADC1_CH0 */
#define GPIO_IN_RWP_CT          39   /* ADC1_CH3 */

/* Dosing tank AJ-SR04M, wired straight to the hub (WIRING.md §13). ECHO is 5V
 * logic and reaches GPIO 4 through a 1k/2k divider — ESP32 pads are not 5V
 * tolerant. Digital only: GPIO 4 is ADC2, and ADC2 is dead while Wi-Fi is up. */
#define GPIO_US_TRIG_DOS        5
#define GPIO_US_ECHO_DOS        4

/* 4-channel relay board, ACTIVE LOW. Float emulation is Phase C; these are
 * driven to the de-energised state at boot and then left alone. */
#define GPIO_RLY_TWT            27
#define GPIO_RLY_RWT            23
#define GPIO_RLY_DOS            18
#define GPIO_RLY_AUX            19
#define RELAY_ON                0
#define RELAY_OFF               1

#define GPIO_LED_STATUS         2
#define GPIO_BOOT_BUTTON        0
#define WIFI_RESET_HOLD_SEC     3
#define FACTORY_RESET_HOLD_SEC  10

/* ------------------------------------------------------------- RS485 protocol */
/* 0xAA 0x55 ADDR CMD LEN <payload> CRC_L CRC_H, CRC-16/Modbus over every
 * preceding byte, payload fields big-endian. docs/RS485_PROTOCOL.md §2. */
#define PREAMBLE_1              0xAA
#define PREAMBLE_2              0x55
#define RESPONSE_BIT            0x80
#define RS485_MAX_PAYLOAD       32

#define CMD_PING                0x01
#define CMD_READ_LEVEL          0x02
#define CMD_READ_CLIMATE        0x06
#define CMD_SET_FAN_RELAY       0x07

#define NODE_ADDR_RWT           0x02
#define NODE_ADDR_TWT           0x03
#define NODE_ADDR_BATTERY       0x04

#define RS485_REPLY_TIMEOUT_MS  120
#define RS485_POLL_ATTEMPTS     3
#define RS485_GUARD_MS          5

/* Payload lengths, per RS485_PROTOCOL.md §4. Anything else is a bad reply and is
 * treated as absence rather than as data. */
#define LEN_LEVEL_REPLY         10
#define LEN_CLIMATE_REPLY       6

/* ------------------------------------------------------------------- sensing */
#define BLIND_ZONE_MM           200
#define US_TRIG_WIDTH_US        10      /* some AJ-SR04M batches want 20 — WIRING.md §9.0 */
#define US_TIMEOUT_US           35000   /* ~6 m of flight time */

/* Mains present on a 240 V opto channel. The module rectifies, clamps and
 * smooths before the LED, so a live channel sits steady near 0 V rather than
 * pulsing — but the decision is taken on the MINIMUM across a full mains cycle,
 * which latches correctly either way. That is phase-B spec §5.4's prescribed fix,
 * obtained for free by measuring instead of sampling. */
#define AC_ACTIVE_BELOW_MV      500
#define AC_IDLE_ABOVE_MV        2500
#define PROBE_WINDOW_MS         30      /* > one 50 Hz cycle */

/* CT bias pedestal. 3V3 through a 10k/10k divider, shared by both channels. */
#define CT_PEDESTAL_MIN_MV      1550
#define CT_PEDESTAL_MAX_MV      1750
#define CT_RMS_SAMPLES          400     /* ~200 ms at 500 us — a full 10 cycles */
#define CT_RMS_INTERVAL_US      500

/* ------------------------------------------------------------------- cadence */
#define POLL_CYCLE_MS           2000
#define NODE_STALE_MS           10000   /* dashboard shows STALE past this */
#define NODE_OFFLINE_MS         30000   /* ...and OFFLINE past this        */

/* ------------------------------------------------------- battery room fan policy */
/* The hub owns the thresholds; the node keeps a hotter backstop for when this hub
 * goes quiet. Re-assert periodically even when unchanged — silence is not the
 * same as "leave it as is" (RS485_PROTOCOL.md §4.4). */
#define FAN_ON_DECI_C_DEFAULT   380
#define FAN_OFF_DECI_C_DEFAULT  360
#define FAN_LIMIT_LOW_DECI      250
#define FAN_LIMIT_HIGH_DECI     550
#define FAN_MIN_HYST_DECI       10
#define FAN_REFRESH_MS          60000

/* ------------------------------------------------------------------- alerting */
/* Thresholds behind the RainMaker push notifications in
 * DASHBOARD_AND_RAINMAKER.md §4. Each alert latches and needs the value to come
 * back past a margin before it can fire again — a threshold sitting exactly on
 * the line must not send a notification every cycle. */
#define ALERT_TANK_FULL_PCT     95
#define ALERT_DOSING_LOW_PCT    20
#define ALERT_HYST_PCT          5
#define ALERT_REARM_MS          300000  /* 5 min floor between repeats of one alert */

/* Over-current. Deliberately generous: this is a "something is badly wrong"
 * threshold, not a protection trip, and nothing here touches a motor circuit.
 * Per-channel because the two pumps are not the same size. Amps x10. */
#define OC_HPP_DECI_A_DEFAULT   120     /* 12.0 A */
#define OC_RWP_DECI_A_DEFAULT   90      /*  9.0 A */
#define OC_LIMIT_LOW_DECI       10
#define OC_LIMIT_HIGH_DECI      300
#define OC_CONFIRM_CYCLES       3       /* must persist — one bad RMS read is not a fault */

/* ------------------------------------------------------- RainMaker parameters */
/* These strings are the keys the cloud and phone app use. Changing one orphans
 * its history in the app, so they are append-only in practice. */
#define DEV_TANKS               "Water Tanks"
#define DEV_PUMPS               "Pumps & Motors"
#define DEV_CLIMATE             "Environment"
#define DEV_VENT                "Ventilation"
#define DEV_PLANT               "RO Plant"

#define PARAM_RWT_PCT           "Raw Water Level"
#define PARAM_TWT_PCT           "Treated Water Level"
#define PARAM_DOS_PCT           "Dosing Level"
#define PARAM_HPP_ON            "HPP Running"
#define PARAM_RWP_ON            "RWP Running"
#define PARAM_HPP_AMPS          "HPP Current"
#define PARAM_RWP_AMPS          "RWP Current"
#define PARAM_OVERCURRENT       "Over Current"
#define PARAM_RO_TEMP           "RO Room Temp"
#define PARAM_RO_HUM            "RO Room Humidity"
#define PARAM_BAT_TEMP          "Battery Room Temp"
#define PARAM_BAT_HUM           "Battery Room Humidity"
#define PARAM_FAN_ON            "Exhaust Fan"
#define PARAM_FAN_THRESHOLD     "Fan On Above"
#define PARAM_ALARM             "Controller Fault"
#define PARAM_STATUS            "Status"

/* ------------------------------------------------------------------ web server */
#define WEB_PORT                80
#define CAL_USER                "admin"
#define CAL_PASS_DEFAULT        "ro-calibrate"   /* changeable at /cal, stored in NVS */
#define AP_SSID                 "RO-HUB"
#define AP_PASS                 "ro-monitor"
#define MDNS_HOSTNAME           "ro-hub"

/* --------------------------------------------------------------------- types */

/* Sensor status as the node reports it — RS485_PROTOCOL.md §4.2. */
typedef enum {
    SENSOR_OK        = 0,
    SENSOR_BLIND     = 1,
    SENSOR_NO_ECHO   = 2,
    SENSOR_HW_FAULT  = 3,
} sensor_status_t;

typedef enum {
    LINK_ONLINE  = 0,
    LINK_STALE   = 1,
    LINK_OFFLINE = 2,
} link_state_t;

/* One tank, whether read over RS485 or straight off the hub. */
typedef struct {
    uint16_t        distance_mm;   /* transducer face to liquid surface */
    uint16_t        raw_mm;
    int16_t         pct;           /* -1 = not computable (uncalibrated or out of range) */
    uint8_t         quality;       /* 0..100, RS485 nodes only */
    sensor_status_t sensor;
    int64_t         last_ok_us;
} tank_state_t;

typedef struct {
    int16_t  temp_deci_c;
    uint16_t hum_deci_pct;
    bool     fault;                /* SHT30 unreadable */
    int64_t  last_ok_us;
} climate_state_t;

typedef struct {
    bool     running;              /* mains present on the contactor opto */
    int16_t  deci_amps;            /* -1 while no clamp is fitted */
    uint32_t mv_lo;                /* raw probe figures, surfaced for commissioning */
    uint32_t mv_hi;
} motor_state_t;

/* Everything the dashboard and RainMaker read. Written only by the poll task,
 * read by the web handlers — see the note on locking in app_main.c. */
typedef struct {
    tank_state_t    rwt, twt, dosing;
    climate_state_t ro_room, battery_room;
    motor_state_t   hpp, rwp;

    bool     fan_on;
    bool     twt_float_closed;
    bool     rl1_active;
    bool     rl2_active;
    bool     alarm_active;

    bool     rwt_online, twt_online, battery_online;
    uint32_t rs485_errors;
    uint32_t last_cycle_ms;
    bool     overcurrent;
} hub_state_t;

/*
 * Shared state accessors, implemented in app_main.c.
 *
 * The poll task is the only writer; the web handlers and the RainMaker reporter
 * are readers. A mutex rather than volatile reads: a telemetry response that
 * mixes half of one cycle with half of the next is a bug that only shows up as
 * an impossible reading months later, and holding a lock for the length of a
 * snprintf costs nothing here.
 */
void hub_state_lock(void);
void hub_state_unlock(void);
hub_state_t *hub_state(void);

#ifdef __cplusplus
}
#endif
