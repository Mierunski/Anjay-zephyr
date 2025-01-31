/*
 * Copyright 2020-2023 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if !(defined(CONFIG_BOARD_NRF9160DK_NRF9160_NS) \
      || defined(CONFIG_BOARD_THINGY91_NRF9160_NS))
#    error "This GPS implementation is not supported by selected board"
#endif // !(defined(CONFIG_BOARD_NRF9160DK_NRF9160_NS) ||
       // defined(CONFIG_BOARD_THINGY91_NRF9160_NS))

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/timeutil.h>

#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>

#include "../config.h"
#include "../gps.h"
#include "../network/network_internal.h"
#include "../utils.h"

#ifdef CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS
#    include "../objects/objects.h"
#endif // CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS

LOG_MODULE_REGISTER(anjay_zephyr_gps_nrf);

#define INTERRUPTED_FIXES_WARN_THRESHOLD 10

// Suggested GPS tuning parameters come from Nordic's SDK example
// https://github.com/nrfconnect/sdk-nrf/blob/master/samples/nrf9160/gps/src/main.c
static const char *const init_at_commands[] = {
// AT%XMAGPIO controls antenna tuner
#ifdef CONFIG_BOARD_THINGY91_NRF9160_NS
    "AT%XMAGPIO=1,1,1,7,1,746,803,2,698,748,2,1710,2200,3,824,894,4,880,960,5,"
    "791,849,7,1565,1586",
#endif // CONFIG_BOARD_THINGY91_NRF9160_NS
#ifdef CONFIG_BOARD_NRF9160DK_NRF9160_NS
    "AT%XMAGPIO=1,0,0,1,1,1574,1577",
#endif // CONFIG_BOARD_NRF9160DK_NRF9160_NS

// AT%XCOEX0 controls external Low-Noise Amplifier
#ifdef CONFIG_ANJAY_ZEPHYR_GPS_NRF_EXTERNAL_ANTENNA
    "AT%XCOEX0",
#else  // CONFIG_ANJAY_ZEPHYR_GPS_NRF_EXTERNAL_ANTENNA
    "AT%XCOEX0=1,1,1565,1586",
#endif // CONFIG_ANJAY_ZEPHYR_GPS_NRF_EXTERNAL_ANTENNA

    // https://infocenter.nordicsemi.com/topic/ref_at_commands/REF/at_commands/mob_termination_ctrl_status/cfun_set.html
    // 31 - Activates GNSS without changing LTE.
    "AT+CFUN=31",
};

K_MUTEX_DEFINE(anjay_zephyr_gps_read_last_mtx);
struct anjay_zephyr_gps_data anjay_zephyr_gps_read_last = {
    .valid = false
};

#ifdef CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS
static uint32_t modem_agps_request_mask;
#endif // CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS

static int64_t prio_mode_cooldown_end_time;
static size_t interrupted_fixes_in_row;
K_MSGQ_DEFINE(anjay_zephyr_incoming_pvt_msgq,
              sizeof(struct nrf_modem_gnss_pvt_data_frame),
              1,
              1);

static void incoming_pvt_work_handler(struct k_work *work);
static void prio_mode_disable_dwork_handler(struct k_work *work);

static K_WORK_DEFINE(incoming_pvt_work, incoming_pvt_work_handler);
static K_WORK_DELAYABLE_DEFINE(prio_mode_disable_dwork,
                               prio_mode_disable_dwork_handler);

volatile atomic_bool anjay_zephyr_gps_prio_mode;

static int64_t
gnss_datetime_to_timestamp(const struct nrf_modem_gnss_datetime *datetime) {
    struct tm broken_down = {
        .tm_year = datetime->year - 1900,
        .tm_mon = datetime->month - 1,
        .tm_mday = datetime->day,
        .tm_hour = datetime->hour,
        .tm_min = datetime->minute,
        .tm_sec = datetime->seconds
    };

    return timeutil_timegm64(&broken_down);
}

static void prio_mode_disable(void) {
    LOG_INF("Disabling gnss_prio_mode");

    atomic_store(&anjay_zephyr_gps_prio_mode, false);
    _anjay_zephyr_network_internal_connection_state_changed();

    if (nrf_modem_gnss_prio_mode_disable()) {
        LOG_ERR("Couldn't disable gnss_prio_mode");
        return;
    }

    prio_mode_cooldown_end_time =
            k_uptime_get()
            + MSEC_PER_SEC
                      * anjay_zephyr_config_get_gps_nrf_prio_mode_cooldown();
}

static void incoming_pvt_work_handler(struct k_work *work) {
    struct nrf_modem_gnss_pvt_data_frame pvt;

    if (k_msgq_get(&anjay_zephyr_incoming_pvt_msgq, &pvt, K_NO_WAIT)) {
        return;
    }

    LOG_DBG("Incoming PVT flags: 0x%02hhx", pvt.flags);

    if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
        interrupted_fixes_in_row = 0;

        // It's not possible to call k_work_flush_delayable(dwork, ...) from WQ
        // running dwork, but if we're already executing inside this WQ, the
        // "async" calls below become safe
        if (k_work_delayable_busy_get(&prio_mode_disable_dwork)) {
            k_work_cancel_delayable(&prio_mode_disable_dwork);
            prio_mode_disable();
        }

        SYNCHRONIZED(anjay_zephyr_gps_read_last_mtx) {
            if (!anjay_zephyr_gps_read_last.valid) {
                LOG_INF("First valid GPS fix produced");
            }
            anjay_zephyr_gps_read_last.valid = true;
            anjay_zephyr_gps_read_last.timestamp =
                    gnss_datetime_to_timestamp(&pvt.datetime);
            anjay_zephyr_gps_read_last.latitude = pvt.latitude;
            anjay_zephyr_gps_read_last.longitude = pvt.longitude;
            anjay_zephyr_gps_read_last.altitude = pvt.altitude;
            anjay_zephyr_gps_read_last.radius = pvt.accuracy;
            anjay_zephyr_gps_read_last.speed = pvt.speed;
        }
    } else if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
        if (k_uptime_get() > prio_mode_cooldown_end_time
                && ++interrupted_fixes_in_row
                               == INTERRUPTED_FIXES_WARN_THRESHOLD) {
            interrupted_fixes_in_row = 0;

            if (!anjay_zephyr_config_is_gps_nrf_prio_mode_permitted()) {
                return;
            }

            LOG_WRN("GPS was interrupted multiple times by the LTE modem when "
                    "producing a fix");

            if (nrf_modem_gnss_prio_mode_enable()) {
                LOG_ERR("Couldn't enable gnss_prio_mode");
                return;
            }

            atomic_store(&anjay_zephyr_gps_prio_mode, true);
            _anjay_zephyr_network_internal_connection_state_changed();

            // NOTE: As written here
            // https://developer.nordicsemi.com/nRF_Connect_SDK/doc/2.2.0/nrfxlib/nrf_modem/doc/gnss_interface.html#enabling-gnss-priority-mode
            // priority mode is disabled automatically after the first fix or
            // after 40 seconds
            k_work_schedule(&prio_mode_disable_dwork, K_SECONDS(41));
        }
    }
}

#ifdef CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS
static void
handle_modem_agps_request_evt(struct nrf_modem_gnss_agps_data_frame req) {
    uint32_t request_mask = 0;

    if (req.data_flags & NRF_MODEM_GNSS_AGPS_GPS_UTC_REQUEST) {
        request_mask |= LOC_SERVICES_A_GPS_MASK_UTC;
    }
    if (req.data_flags & NRF_MODEM_GNSS_AGPS_KLOBUCHAR_REQUEST) {
        request_mask |= LOC_SERVICES_A_GPS_MASK_KLOBUCHAR;
    }
    if (req.data_flags & NRF_MODEM_GNSS_AGPS_NEQUICK_REQUEST) {
        request_mask |= LOC_SERVICES_A_GPS_MASK_NEQUICK;
    }
    if (req.data_flags & NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST) {
        request_mask |=
                LOC_SERVICES_A_GPS_MASK_TOW | LOC_SERVICES_A_GPS_MASK_CLOCK;
    }
    if (req.data_flags & NRF_MODEM_GNSS_AGPS_POSITION_REQUEST) {
        request_mask |= LOC_SERVICES_A_GPS_MASK_LOCATION;
    }
    if (req.data_flags & NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST) {
        request_mask |= LOC_SERVICES_A_GPS_MASK_INTEGRITY;
    }
    if (req.sv_mask_ephe) {
        request_mask |= LOC_SERVICES_A_GPS_MASK_EPHEMERIS;
    }
    if (req.sv_mask_alm) {
        request_mask |= LOC_SERVICES_A_GPS_MASK_ALMANAC;
    }

    SYNCHRONIZED(anjay_zephyr_gps_read_last_mtx) {
        // We're reassigning the mask instead of ORing it with previous state,
        // as the modem might not require some kind of assistance data anymore
        modem_agps_request_mask = request_mask;
    }
}
#endif // CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS

static void prio_mode_disable_dwork_handler(struct k_work *work) {
    prio_mode_disable();
}

static void gnss_event_handler(int event) {
    if (event == NRF_MODEM_GNSS_EVT_PVT) {
        struct nrf_modem_gnss_pvt_data_frame pvt;

        if (nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT)) {
            LOG_ERR("Failed to retrieve a PVT event");
            return;
        }

        if (k_msgq_put(&anjay_zephyr_incoming_pvt_msgq, &pvt, K_NO_WAIT)) {
            return;
        }
        k_work_submit(&incoming_pvt_work);
    }
#ifdef CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS
    if (event == NRF_MODEM_GNSS_EVT_AGPS_REQ) {
        struct nrf_modem_gnss_agps_data_frame req;

        if (nrf_modem_gnss_read(
                    &req, sizeof(req), NRF_MODEM_GNSS_DATA_AGPS_REQ)) {
            LOG_ERR("Failed to retrieve a A-GPS REQ event");
            return;
        }

        handle_modem_agps_request_evt(req);
    }
#endif // CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS
}

static int config_at(void) {
    for (size_t i = 0; i < ARRAY_SIZE(init_at_commands); i++) {
        if (nrf_modem_at_printf("%s", init_at_commands[i])) {
            LOG_ERR("Failed to write initial AT command: %s",
                    init_at_commands[i]);
            return -1;
        }
    }

    return 0;
}

int _anjay_zephyr_initialize_gps(void) {
    if (config_at() || nrf_modem_gnss_event_handler_set(gnss_event_handler)
            || nrf_modem_gnss_fix_retry_set(0)
            || nrf_modem_gnss_fix_interval_set(1) || nrf_modem_gnss_start()) {
        LOG_ERR("Failed to initialize GPS interface");
        return -1;
    }
    return 0;
}

#ifdef CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS
uint32_t _anjay_zephyr_gps_fetch_modem_agps_request_mask(void) {
    uint32_t result = 0;

    SYNCHRONIZED(anjay_zephyr_gps_read_last_mtx) {
        if (modem_agps_request_mask) {
            result = modem_agps_request_mask;
        }
    }
    return result;
}

void _anjay_zephyr_gps_clear_modem_agps_request_mask(void) {
    SYNCHRONIZED(anjay_zephyr_gps_read_last_mtx) {
        modem_agps_request_mask = 0;
    }
}
#endif // CONFIG_ANJAY_ZEPHYR_GPS_NRF_A_GPS
