/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/ringbuf.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

/* log tags */
#define BT_AV_TAG             "BT_AV"
#define BT_RC_CT_TAG          "RC_CT"

/* device name */
#define LOCAL_DEVICE_NAME     "ESP_A2DP_SRC"

/* PCM input from the other ESP32: 16-bit little-endian, 44.1 kHz, mono.
 * It is expanded to stereo in the A2DP callback. */
#define PCM_UART              UART_NUM_2
#define PCM_UART_RX_PIN       GPIO_NUM_16
#define PCM_UART_TX_PIN       GPIO_NUM_17
#define PCM_UART_BAUD_RATE    1000000
#define PCM_RINGBUF_SIZE      (64 * 1024)
#define PCM_INPUT_FRAME_BYTES 2       /* 16-bit little-endian mono */
#define PCM_OUTPUT_FRAME_BYTES 4      /* 16-bit little-endian stereo */
#define PCM_PREBUFFER_BYTES   (16 * 1024) /* about 186 ms at 44.1 kHz mono */
#define PCM_PREBUFFER_MAX     (48 * 1024) /* about 557 ms; leave ring headroom */
#define PCM_PREBUFFER_STEP    (8 * 1024)
#define PCM_LOW_WATER_BYTES   (8 * 1024)
#define PCM_HIGH_WATER_BYTES  (32 * 1024)
#define PCM_FADE_FRAMES       128
#define PCM_UART_HEADER_SIZE  12
#define PCM_UART_MAX_PAYLOAD  4096

/* Diagnostic: 1 = bypass the UART ring buffer and feed a 440 Hz sine wave,
 * to check whether the Bluetooth link itself is clean. Set back to 0 for
 * normal operation. While enabled, "ring buffer full" warnings are expected
 * (the UART keeps receiving but the data is not consumed). */
#define PCM_FEED_TEST_TONE    0

/* Greeting clip playback gain in 1/256 units: 256 = original level,
 * 128 = half amplitude (-6 dB), 64 = -12 dB. TTS output sits near full
 * scale and sounds much louder than the UART stream, so start at half. */
#define CLIP_GAIN_256         128

/* Status output GPIO: driven high while the A2DP link with the headset is
 * up, low otherwise. Note: GPIO5 is a strapping pin and outputs PWM pulses
 * during boot, so expect a brief glitch at power-on. */
#define STATUS_GPIO           GPIO_NUM_5

#include "bt_config.h"
static const esp_bd_addr_t target_peer_bda = APP_TARGET_PEER_BDA;

/* AVRCP used transaction label */
#define APP_RC_CT_TL_GET_CAPS            (0)
#define APP_RC_CT_TL_RN_VOLUME_CHANGE    (1)
#define APP_RC_CT_TL_SET_VOLUME          (2)
#define DEFAULT_HEADSET_VOLUME           (40)  /* AVRCP range: 0..127 */

/* Master PCM gain in 1/256 units. 64 = 25% amplitude, approximately -12 dB.
 * This works even when the headset ignores AVRCP absolute-volume commands. */
#define MASTER_GAIN_256                  (128)

/* Speech-oriented noise conditioning. The low-pass removes high-frequency
 * UART/source hiss; the soft gate mutes only very quiet passages. */
#define PCM_NOISE_GATE_CLOSE             (96)
#define PCM_NOISE_GATE_OPEN              (384)

enum {
    BT_APP_STACK_UP_EVT   = 0x0000,    /* event for stack up */
    BT_APP_HEART_BEAT_EVT = 0xff00,    /* event for heart beat */
};

/* A2DP global states */
enum {
    APP_AV_STATE_IDLE,
    APP_AV_STATE_DISCOVERING,
    APP_AV_STATE_DISCOVERED,
    APP_AV_STATE_UNCONNECTED,
    APP_AV_STATE_CONNECTING,
    APP_AV_STATE_CONNECTED,
    APP_AV_STATE_DISCONNECTING,
};

/* sub states of APP_AV_STATE_CONNECTED */
enum {
    APP_AV_MEDIA_STATE_IDLE,
    APP_AV_MEDIA_STATE_STARTING,
    APP_AV_MEDIA_STATE_STARTED,
    APP_AV_MEDIA_STATE_STOPPING,
};

/*********************************
 * STATIC FUNCTION DECLARATIONS
 ********************************/

/* handler for bluetooth stack enabled events */
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/* avrc controller event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);

/* GAP callback function */
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

/* callback function for A2DP source */
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/* callback function for A2DP source audio data stream */
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);

/* callback function for AVRCP controller */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

/* handler for heart beat timer */
static void bt_app_a2d_heart_beat(TimerHandle_t arg);

/* A2DP application state machine */
static void bt_app_av_sm_hdlr(uint16_t event, void *param);

/* utils for transfer BLuetooth Deveice Address into string form */
static char *bda2str(esp_bd_addr_t bda, char *str, size_t size);

/* A2DP application state machine handler for each state */
static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param);

/* greeting clip player (PCM embedded in flash) */
static void clip_play(void);
static size_t clip_read(uint8_t *data, size_t wanted);

/*********************************
 * STATIC VARIABLE DEFINITIONS
 ********************************/

static esp_bd_addr_t s_peer_bda = {0};                        /* Bluetooth Device Address of peer device*/
static uint8_t s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];  /* Bluetooth Device Name of peer device*/
static int s_a2d_state = APP_AV_STATE_IDLE;                   /* A2DP global state */
static int s_media_state = APP_AV_MEDIA_STATE_IDLE;           /* sub states of APP_AV_STATE_CONNECTED */
static int s_connecting_intv = 0;                             /* count of heart beat intervals for connecting */
static uint32_t s_pkt_cnt = 0;                                /* count of packets */
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;         /* AVRC target notification event capability bit mask */
static TimerHandle_t s_tmr;                                   /* handle of heart beat timer */
static RingbufHandle_t s_pcm_ringbuf;                          /* PCM received from UART */
static uint32_t s_pcm_cb_count = 0;                            /* A2DP data callback invocations */
static uint32_t s_pcm_demand_bytes = 0;                        /* total bytes requested by A2DP */
static uint32_t s_pcm_fed_bytes = 0;                           /* total PCM bytes fed from UART */
static uint32_t s_pcm_underrun_bytes = 0;                      /* bytes zero-filled due to underrun */
static uint32_t s_pcm_overrun_bytes = 0;                       /* aligned UART bytes dropped */
static uint32_t s_pcm_trimmed_frames = 0;                      /* frames dropped to correct fast input clock */
static uint32_t s_pcm_stretched_frames = 0;                    /* frames repeated to correct slow input clock */
static uint32_t s_pcm_crc_errors = 0;                          /* UART frames rejected by CRC/header */
static uint32_t s_pcm_seq_errors = 0;                          /* missing/out-of-order UART frames */
static volatile bool s_pcm_stream_enabled = false;
static bool s_pcm_prebuffering = true;
static size_t s_pcm_prebuffer_target = PCM_PREBUFFER_BYTES;
static uint32_t s_pcm_stable_callbacks = 0;
static size_t s_pcm_fade_in_frames = PCM_FADE_FRAMES;
static int16_t s_pcm_last_left = 0;
static int16_t s_pcm_last_right = 0;
static int32_t s_pcm_lowpass_state = 0;
static int32_t s_pcm_noise_envelope = 0;
static int32_t s_pcm_noise_gain = 0;       /* 0..256 */

/* Greeting clip: 16-bit little-endian mono 44.1 kHz PCM, embedded in flash
 * via EMBED_FILES in main/CMakeLists.txt. `const` keeps it in flash (rodata)
 * instead of consuming RAM; it is memory-mapped, so memcpy from it is cheap. */
extern const uint8_t connected_pcm_start[] asm("_binary_connected_pcm_start");
extern const uint8_t connected_pcm_end[]   asm("_binary_connected_pcm_end");
static const uint8_t *s_clip_pos;    /* next clip byte to output, NULL = idle */
static const uint8_t *s_clip_stop;   /* one past the last clip byte */

static const char remote_device_name[] = CONFIG_EXAMPLE_PEER_DEVICE_NAME;

static uint16_t pcm_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void pcm_condition_mono(uint8_t *data, size_t len)
{
    size_t sample_count = len / sizeof(int16_t);

    for (size_t i = 0; i < sample_count; i++) {
        int16_t raw = (int16_t)((uint16_t)data[i * 2] |
                                ((uint16_t)data[i * 2 + 1] << 8));

        /* One-pole low-pass, approximately 7 kHz at 44.1 kHz. */
        s_pcm_lowpass_state += (((int32_t)raw - s_pcm_lowpass_state) * 5) / 8;
        int32_t sample = s_pcm_lowpass_state;
        int32_t level = sample < 0 ? -sample : sample;

        /* Peak envelope with a gentle release so the gate does not chatter
         * between individual speech samples. */
        if (level > s_pcm_noise_envelope) {
            s_pcm_noise_envelope = level;
        } else if (s_pcm_noise_envelope > 0) {
            int32_t decay = s_pcm_noise_envelope >> 10;
            s_pcm_noise_envelope -= decay > 0 ? decay : 1;
        }

        int32_t target_gain;
        if (s_pcm_noise_envelope <= PCM_NOISE_GATE_CLOSE) {
            target_gain = 0;
        } else if (s_pcm_noise_envelope >= PCM_NOISE_GATE_OPEN) {
            target_gain = 256;
        } else {
            target_gain = (s_pcm_noise_envelope - PCM_NOISE_GATE_CLOSE) * 256 /
                          (PCM_NOISE_GATE_OPEN - PCM_NOISE_GATE_CLOSE);
        }

        if (s_pcm_noise_gain < target_gain) {
            s_pcm_noise_gain += (target_gain - s_pcm_noise_gain + 15) / 16;
        } else if (s_pcm_noise_gain > target_gain) {
            s_pcm_noise_gain -= (s_pcm_noise_gain - target_gain + 63) / 64;
        }

        sample = (int32_t)(((int64_t)sample * s_pcm_noise_gain *
                            MASTER_GAIN_256) >> 16);
        data[i * 2] = (uint8_t)sample;
        data[i * 2 + 1] = (uint8_t)(sample >> 8);
    }
}

static void pcm_uart_task(void *arg)
{
    static const uint8_t magic[4] = {0xa5, 0x5a, 0xc3, 0x3c};
    static uint8_t uart_buffer[512];
    static uint8_t header[PCM_UART_HEADER_SIZE];
    static uint8_t payload[PCM_UART_MAX_PAYLOAD];
    enum { WAIT_MAGIC, READ_HEADER, READ_PAYLOAD } state = WAIT_MAGIC;
    size_t magic_pos = 0;
    size_t header_pos = 0;
    size_t payload_pos = 0;
    size_t payload_len = 0;
    uint16_t payload_crc = 0;
    uint16_t frame_seq = 0;
    uint16_t expected_seq = 0;
    bool have_sequence = false;

    while (true) {
        int received = uart_read_bytes(PCM_UART, uart_buffer, sizeof(uart_buffer),
                                       pdMS_TO_TICKS(20));
        if (received <= 0) {
            continue;
        }

        for (int i = 0; i < received; i++) {
            uint8_t byte = uart_buffer[i];

            if (state == WAIT_MAGIC) {
                if (byte == magic[magic_pos]) {
                    magic_pos++;
                    if (magic_pos == sizeof(magic)) {
                        memcpy(header, magic, sizeof(magic));
                        header_pos = sizeof(magic);
                        magic_pos = 0;
                        state = READ_HEADER;
                    }
                } else {
                    magic_pos = (byte == magic[0]) ? 1 : 0;
                }
                continue;
            }

            if (state == READ_HEADER) {
                header[header_pos++] = byte;
                if (header_pos < PCM_UART_HEADER_SIZE) {
                    continue;
                }

                payload_len = (size_t)header[4] | ((size_t)header[5] << 8);
                frame_seq = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
                payload_crc = (uint16_t)header[8] | ((uint16_t)header[9] << 8);
                uint16_t header_crc = (uint16_t)header[10] |
                                      ((uint16_t)header[11] << 8);
                if (header_crc != pcm_crc16(header, 10) || payload_len == 0 ||
                    payload_len > PCM_UART_MAX_PAYLOAD ||
                    (payload_len & (PCM_INPUT_FRAME_BYTES - 1)) != 0) {
                    s_pcm_crc_errors++;
                    state = WAIT_MAGIC;
                    header_pos = 0;
                    continue;
                }
                payload_pos = 0;
                state = READ_PAYLOAD;
                continue;
            }

            payload[payload_pos++] = byte;
            if (payload_pos < payload_len) {
                continue;
            }

            if (pcm_crc16(payload, payload_len) != payload_crc) {
                s_pcm_crc_errors++;
            } else {
                if (have_sequence && frame_seq != expected_seq) {
                    s_pcm_seq_errors++;
                }
                expected_seq = (uint16_t)(frame_seq + 1);
                have_sequence = true;

                if (s_pcm_stream_enabled &&
                    xRingbufferSend(s_pcm_ringbuf, payload, payload_len,
                                    pdMS_TO_TICKS(20)) != pdTRUE) {
                    s_pcm_overrun_bytes += payload_len;
                }
            }
            state = WAIT_MAGIC;
            payload_pos = 0;
        }
    }
}

static size_t pcm_ringbuf_used(void)
{
    return PCM_RINGBUF_SIZE - xRingbufferGetCurFreeSize(s_pcm_ringbuf);
}

static size_t pcm_ringbuf_read(uint8_t *data, size_t wanted)
{
    size_t filled = 0;

    wanted &= ~(size_t)(PCM_INPUT_FRAME_BYTES - 1);
    while (filled < wanted) {
        size_t received = 0;
        uint8_t *pcm = xRingbufferReceiveUpTo(s_pcm_ringbuf, &received, 0,
                                              wanted - filled);
        if (pcm == NULL) {
            break;
        }
        memcpy(data + filled, pcm, received);
        vRingbufferReturnItem(s_pcm_ringbuf, pcm);
        filled += received;
    }
    return filled;
}

static void pcm_ringbuf_flush(void)
{
    size_t received;
    uint8_t *pcm;

    while ((pcm = xRingbufferReceiveUpTo(s_pcm_ringbuf, &received, 0,
                                          PCM_RINGBUF_SIZE)) != NULL) {
        vRingbufferReturnItem(s_pcm_ringbuf, pcm);
    }
}

/* Start accepting PCM only when A2DP is about to start. This prevents several
 * seconds of pre-connection UART audio from filling the ring buffer. */
static void pcm_stream_prepare(void)
{
    s_pcm_stream_enabled = false;
    /* Do not flush the UART driver here. It is a raw byte stream with no frame
     * marker, so dropping an arbitrary number of bytes can invert the 16-bit
     * sample phase. pcm_uart_task has already been draining old samples while
     * preserving that phase. */
    pcm_ringbuf_flush();
    s_pcm_last_left = 0;
    s_pcm_last_right = 0;
    s_pcm_lowpass_state = 0;
    s_pcm_noise_envelope = 0;
    s_pcm_noise_gain = 0;
    s_pcm_fade_in_frames = PCM_FADE_FRAMES;
    s_pcm_prebuffering = true;
    s_pcm_stream_enabled = true;
}

static void pcm_stream_stop(void)
{
    s_pcm_stream_enabled = false;
    s_pcm_prebuffering = true;
    s_clip_pos = NULL;   /* abort a greeting clip in progress */
}

/* Start the greeting clip from the beginning. Called when A2DP streaming
 * starts so the headset hears it right after the connection is up. */
static void clip_play(void)
{
    s_clip_pos = connected_pcm_start;
    s_clip_stop = connected_pcm_end;
    /* Begin immediately (skip the UART prebuffer wait) and ramp up from
     * silence so the first clip sample does not pop. */
    s_pcm_prebuffering = false;
    s_pcm_fade_in_frames = PCM_FADE_FRAMES;
    ESP_LOGI(BT_AV_TAG, "Playing greeting clip (%u bytes of PCM)",
             (unsigned)(s_clip_stop - s_clip_pos));
}

/* Copy up to `wanted` bytes of clip PCM into `data`; returns bytes written. */
static size_t clip_read(uint8_t *data, size_t wanted)
{
    if (s_clip_pos == NULL) {
        return 0;
    }

    size_t remaining = (size_t)(s_clip_stop - s_clip_pos);
    if (wanted > remaining) {
        wanted = remaining;
    }
    memcpy(data, s_clip_pos, wanted);

    /* Apply the clip gain: scale the 16-bit samples in place. This only
     * touches the bytes just copied from the clip; bytes later filled from
     * the UART ring buffer in the same callback are not affected. */
    if (CLIP_GAIN_256 != 256) {
        int16_t *samples = (int16_t *)data;
        for (size_t i = 0; i < wanted / 2; i++) {
            samples[i] = (int16_t)(((int32_t)samples[i] * CLIP_GAIN_256) >> 8);
        }
    }

    s_clip_pos += wanted;
    if (s_clip_pos == s_clip_stop) {
        ESP_LOGI(BT_AV_TAG, "Greeting clip finished");
        s_clip_pos = NULL;
    }
    return wanted;
}

static void pcm_uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = PCM_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* RX buffer: 8192 bytes ~= 40 ms of 44.1 kHz/16-bit/stereo PCM, so the
     * driver survives task scheduling hiccups of that order without
     * losing bytes (no hardware flow control is used). */
    ESP_ERROR_CHECK(uart_driver_install(PCM_UART, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PCM_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(PCM_UART, PCM_UART_TX_PIN, PCM_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_pcm_ringbuf = xRingbufferCreate(PCM_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    configASSERT(s_pcm_ringbuf != NULL);
    /* The BT stack is pinned to core 0, so pin the PCM drain task to core 1
     * at a high priority: it must empty the UART driver buffer promptly, but
     * must never preempt the BT task and vice versa. */
    xTaskCreatePinnedToCore(pcm_uart_task, "pcm_uart_rx", 4096, NULL, 12, NULL, 1);
    ESP_LOGI(BT_AV_TAG, "PCM UART ready: UART%d RX GPIO%d, %d baud",
             PCM_UART, PCM_UART_RX_PIN, PCM_UART_BAUD_RATE);
}

/*********************************
 * STATIC FUNCTION DEFINITIONS
 ********************************/

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    /* get complete or short local name from eir data */
    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint32_t cod = 0;     /* class of device */
    int32_t rssi = -129;  /* invalid value */
    uint8_t *eir = NULL;
    esp_bt_gap_dev_prop_t *p;

    /* handle the discovery results */
    ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%"PRIx32, cod);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--RSSI: %"PRId32, rssi);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = (uint8_t *)(p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
        default:
            break;
        }
    }

    /* search for device with MAJOR service class as "rendering" in COD */
    if (!esp_bt_gap_is_valid_cod(cod) ||
            !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
        return;
    }

    /* search for target device in its Extended Inqury Response */
    if (eir) {
        get_name_from_eir(eir, s_peer_bdname, NULL);
        if (strcmp((char *)s_peer_bdname, remote_device_name) == 0) {
            ESP_LOGI(BT_AV_TAG, "Found a target device, address %s, name %s", bda_str, s_peer_bdname);
            s_a2d_state = APP_AV_STATE_DISCOVERED;
            memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
            ESP_LOGI(BT_AV_TAG, "Cancel device discovery ...");
            esp_bt_gap_cancel_discovery();
        }
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    /* when device discovered a result, this event comes */
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (s_a2d_state == APP_AV_STATE_DISCOVERING) {
            filter_inquiry_scan_result(param);
        }
        break;
    }
    /* when discovery state changed, this event comes */
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_a2d_state == APP_AV_STATE_DISCOVERED) {
                s_a2d_state = APP_AV_STATE_CONNECTING;
                ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
                ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %s", s_peer_bdname);
                /* connect source to peer device specified by Bluetooth Device Address */
                esp_a2d_source_connect(s_peer_bda);
            } else {
                /* not discovered, continue to discover */
                ESP_LOGI(BT_AV_TAG, "Device discovery failed, continue to discover...");
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(BT_AV_TAG, "Discovery started.");
        }
        break;
    }
    /* when authentication completed, this event comes */
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOG_BUFFER_HEX(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(BT_AV_TAG, "authentication failed, status: %d", param->auth_cmpl.stat);
        }
        break;
    }
    /* when Legacy Pairing pin code requested, this event comes */
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit: %d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(BT_AV_TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(BT_AV_TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* when Security Simple Pairing user confirmation requested, this event comes */
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    /* when Security Simple Pairing passkey notified, this event comes */
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %06"PRIu32, param->key_notif.passkey);
        break;
    /* when Security Simple Pairing passkey requested, this event comes */
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    /* when GAP mode changed, this event comes */
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode: %d", param->mode_chg.mode);
        break;
    case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
        if (param->get_dev_name_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT device name: %s", param->get_dev_name_cmpl.name);
        } else {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT failed, state: %d", param->get_dev_name_cmpl.status);
        }
        break;
    /* other */
    default: {
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    }

    return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s event: %d", __func__, event);

    switch (event) {
    /* when stack up worked, this event comes */
    case BT_APP_STACK_UP_EVT: {
        char *dev_name = LOCAL_DEVICE_NAME;
        esp_bt_gap_set_device_name(dev_name);
        esp_bt_gap_register_callback(bt_app_gap_cb);

        esp_avrc_ct_init();
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

        esp_avrc_rn_evt_cap_mask_t evt_set = {0};
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
        ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));

        esp_a2d_source_init();
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);

        /* Avoid the state error of s_a2d_state caused by the connection initiated by the peer device. */
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        esp_bt_gap_get_device_name();

        static const uint8_t zero_bda[ESP_BD_ADDR_LEN] = {0};
        if (memcmp(target_peer_bda, zero_bda, ESP_BD_ADDR_LEN) == 0) {
            // No private address configured: use the existing name discovery flow.
            s_a2d_state = APP_AV_STATE_DISCOVERING;
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        } else {
            memcpy(s_peer_bda, target_peer_bda, ESP_BD_ADDR_LEN);
            ESP_LOGI(BT_AV_TAG, "Connecting to configured headset");
            s_a2d_state = APP_AV_STATE_CONNECTING;
            esp_a2d_source_connect(s_peer_bda);
        }

        /* create and start heart beat timer */
        do {
            int tmr_id = 0;
            s_tmr = xTimerCreate("connTmr", (10000 / portTICK_PERIOD_MS),
                                 pdTRUE, (void *) &tmr_id, bt_app_a2d_heart_beat);
            xTimerStart(s_tmr, portMAX_DELAY);
        } while (0);
        break;
    }
    /* other */
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL, NULL);
}

/* Supply UART PCM to the A2DP source. Missing data is sent as silence.
 * This runs in the Bluetooth task context, so it must never block. */
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
    if (data == NULL || len < 0) {
        return 0;
    }

#if PCM_FEED_TEST_TONE
    /* Local 440 Hz sine, 16-bit stereo. LUT-based: the BT task cannot afford
     * floating-point sin() per sample. */
    static int16_t s_tone_table[256];
    static uint32_t s_tone_phase;
    static bool s_tone_ready;

    if (!s_tone_ready) {
        for (int i = 0; i < 256; i++) {
            s_tone_table[i] = (int16_t)(sin(2.0 * M_PI * i / 256.0) * 32767.0);
        }
        s_tone_ready = true;
    }

    const uint32_t inc = 42852281u;  /* 440 Hz * 2^32 / 44100 Hz */
    for (int32_t i = 0; i + 4 <= len; i += 4) {
        int16_t s = s_tone_table[s_tone_phase >> 24];
        s_tone_phase += inc;
        data[i]     = (uint8_t)s;
        data[i + 1] = (uint8_t)(s >> 8);
        data[i + 2] = (uint8_t)s;
        data[i + 3] = (uint8_t)(s >> 8);
    }
    return len;
#endif

    size_t output_frames = (size_t)len / PCM_OUTPUT_FRAME_BYTES;
    size_t input_needed = output_frames * PCM_INPUT_FRAME_BYTES;

    s_pcm_cb_count++;
    s_pcm_demand_bytes += input_needed;

    if (!s_pcm_stream_enabled) {
        memset(data, 0, len);
        return len;
    }

    /* While a greeting clip is playing it takes over the mono feed; UART PCM
     * keeps buffering in the ring buffer and resumes once the clip ends. */
    bool clip_active = (s_clip_pos != NULL);

    /* Wait for about 186 ms of PCM before releasing audio. This absorbs normal
     * UART/task scheduling jitter instead of alternating audio and silence.
     * Bypassed while the clip plays so the greeting starts immediately. */
    if (s_pcm_prebuffering && !clip_active) {
        if (pcm_ringbuf_used() < s_pcm_prebuffer_target) {
            memset(data, 0, len);
            return len;
        }
        s_pcm_prebuffering = false;
    }

    size_t input_filled = 0;

    if (clip_active) {
        input_filled = clip_read(data, input_needed);
    }

    if (input_filled < input_needed) {
        size_t used = pcm_ringbuf_used();

        /* The two ESP32s have independent clocks. Correct slow drift one frame
         * at a time: discard above the high-water mark, repeat the last frame
         * below the low-water mark. The correction is tiny and avoids periodic
         * bulk overflow/underrun clicks. Skipped while the clip is active. */
        if (!clip_active) {
            if (used > PCM_HIGH_WATER_BYTES) {
                uint8_t discarded[PCM_INPUT_FRAME_BYTES];
                if (pcm_ringbuf_read(discarded, sizeof(discarded)) == sizeof(discarded)) {
                    s_pcm_trimmed_frames++;
                }
            } else if (used < PCM_LOW_WATER_BYTES &&
                       input_needed >= PCM_INPUT_FRAME_BYTES &&
                       used >= input_needed - PCM_INPUT_FRAME_BYTES) {
                /* Only stretch when the ring buffer can still satisfy this
                 * callback. Repeating a stale non-zero sample while the buffer
                 * is actually empty creates a new click at the start of every
                 * A2DP callback. */
                data[0] = (uint8_t)s_pcm_last_left;
                data[1] = (uint8_t)(s_pcm_last_left >> 8);
                input_filled = PCM_INPUT_FRAME_BYTES;
                s_pcm_stretched_frames++;
            }
        }

        input_filled += pcm_ringbuf_read(data + input_filled,
                                         input_needed - input_filled);
    }

    /* Expand mono to stereo in place. Walk backwards so the expanded samples
     * do not overwrite mono samples that have not been read yet. */
    size_t valid_frames = input_filled / PCM_INPUT_FRAME_BYTES;
    pcm_condition_mono(data, input_filled);
    for (size_t i = valid_frames; i > 0; i--) {
        size_t src = (i - 1) * PCM_INPUT_FRAME_BYTES;
        size_t dst = (i - 1) * PCM_OUTPUT_FRAME_BYTES;
        uint8_t lo = data[src];
        uint8_t hi = data[src + 1];
        data[dst] = lo;
        data[dst + 1] = hi;
        data[dst + 2] = lo;
        data[dst + 3] = hi;
    }
    size_t valid_output_bytes = valid_frames * PCM_OUTPUT_FRAME_BYTES;

    /* After a gap, ramp the first samples up from silence to suppress the
     * zero-to-waveform edge which is heard as a pop. */
    if (s_pcm_fade_in_frames > 0 && valid_frames > 0) {
        size_t frames = valid_frames;
        if (frames > s_pcm_fade_in_frames) {
            frames = s_pcm_fade_in_frames;
        }
        int16_t *samples = (int16_t *)data;
        for (size_t i = 0; i < frames; i++) {
            size_t progress = PCM_FADE_FRAMES - s_pcm_fade_in_frames + i;
            samples[i * 2] = (int16_t)((int32_t)samples[i * 2] * (int32_t)progress /
                                       (int32_t)PCM_FADE_FRAMES);
            samples[i * 2 + 1] = (int16_t)((int32_t)samples[i * 2 + 1] * (int32_t)progress /
                                           (int32_t)PCM_FADE_FRAMES);
        }
        s_pcm_fade_in_frames -= frames;
    }

    if (valid_output_bytes < (size_t)len) {
        size_t missing_output = (size_t)len - valid_output_bytes;
        if (valid_frames > 0) {
            s_pcm_last_left = (int16_t)((uint16_t)data[valid_output_bytes - 4] |
                                        ((uint16_t)data[valid_output_bytes - 3] << 8));
            s_pcm_last_right = s_pcm_last_left;
        }
        /* Fade the last valid frame to zero instead of making a hard edge. */
        size_t fade_frames = missing_output / PCM_OUTPUT_FRAME_BYTES;
        if (fade_frames > PCM_FADE_FRAMES) {
            fade_frames = PCM_FADE_FRAMES;
        }
        int16_t *fade = (int16_t *)(data + valid_output_bytes);
        for (size_t i = 0; i < fade_frames; i++) {
            int32_t gain = (int32_t)(fade_frames - i - 1);
            fade[i * 2] = (int16_t)((int32_t)s_pcm_last_left * gain /
                                    (int32_t)fade_frames);
            fade[i * 2 + 1] = (int16_t)((int32_t)s_pcm_last_right * gain /
                                        (int32_t)fade_frames);
        }
        size_t faded_bytes = fade_frames * PCM_OUTPUT_FRAME_BYTES;
        memset(data + valid_output_bytes + faded_bytes, 0,
               missing_output - faded_bytes);
        s_pcm_underrun_bytes += input_needed - input_filled;
        s_pcm_fade_in_frames = PCM_FADE_FRAMES;
        /* Stay at zero and collect a fresh jitter buffer before resuming.
         * This is especially important for separate TTS clips with gaps. */
        s_pcm_last_left = 0;
        s_pcm_last_right = 0;
        s_pcm_lowpass_state = 0;
        s_pcm_noise_envelope = 0;
        s_pcm_noise_gain = 0;
        s_pcm_stable_callbacks = 0;
        if (s_pcm_prebuffer_target < PCM_PREBUFFER_MAX) {
            s_pcm_prebuffer_target += PCM_PREBUFFER_STEP;
            if (s_pcm_prebuffer_target > PCM_PREBUFFER_MAX) {
                s_pcm_prebuffer_target = PCM_PREBUFFER_MAX;
            }
        }
        s_pcm_prebuffering = true;
    } else if (valid_frames > 0) {
        s_pcm_last_left = (int16_t)((uint16_t)data[valid_output_bytes - 4] |
                                    ((uint16_t)data[valid_output_bytes - 3] << 8));
        s_pcm_last_right = s_pcm_last_left;
        /* A long stable period means latency can be reduced gradually. */
        if (++s_pcm_stable_callbacks >= 500) {
            s_pcm_stable_callbacks = 0;
            if (s_pcm_prebuffer_target > PCM_PREBUFFER_BYTES) {
                s_pcm_prebuffer_target -= PCM_PREBUFFER_STEP;
            }
        }
    }
    s_pcm_fed_bytes += input_filled;

    return len;
}

/* Print streaming statistics once per heart beat (10 s). */
static void pcm_stream_stats_print(void)
{
    static uint32_t last_cb = 0, last_demand = 0, last_fed = 0, last_underrun = 0;
    static uint32_t last_overrun = 0, last_trimmed = 0, last_stretched = 0;
    static uint32_t last_crc_errors = 0, last_seq_errors = 0;

    uint32_t d_cb = s_pcm_cb_count - last_cb;
    uint32_t d_demand = s_pcm_demand_bytes - last_demand;
    uint32_t d_fed = s_pcm_fed_bytes - last_fed;
    uint32_t d_underrun = s_pcm_underrun_bytes - last_underrun;
    uint32_t d_overrun = s_pcm_overrun_bytes - last_overrun;
    uint32_t d_trimmed = s_pcm_trimmed_frames - last_trimmed;
    uint32_t d_stretched = s_pcm_stretched_frames - last_stretched;
    uint32_t d_crc_errors = s_pcm_crc_errors - last_crc_errors;
    uint32_t d_seq_errors = s_pcm_seq_errors - last_seq_errors;

    last_cb = s_pcm_cb_count;
    last_demand = s_pcm_demand_bytes;
    last_fed = s_pcm_fed_bytes;
    last_underrun = s_pcm_underrun_bytes;
    last_overrun = s_pcm_overrun_bytes;
    last_trimmed = s_pcm_trimmed_frames;
    last_stretched = s_pcm_stretched_frames;
    last_crc_errors = s_pcm_crc_errors;
    last_seq_errors = s_pcm_seq_errors;

    if (d_cb == 0) {
        return;
    }

    uint32_t underrun_pct = d_demand ? (d_underrun * 100 / d_demand) : 0;
    ESP_LOGI(BT_AV_TAG, "PCM stream: fed %" PRIu32 " B/s, underrun %" PRIu32
             " B/s (%" PRIu32 "%%), overrun %" PRIu32
             " B/s, trim/stretch %" PRIu32 "/%" PRIu32
             " frames, UART crc/seq %" PRIu32 "/%" PRIu32
             ", buffered %u B%s",
             d_fed / 10, d_underrun / 10, underrun_pct, d_overrun / 10,
             d_trimmed, d_stretched, d_crc_errors, d_seq_errors,
             (unsigned)pcm_ringbuf_used(),
             s_pcm_prebuffering ? " (prebuffering)" : "");
}

static void bt_app_a2d_heart_beat(TimerHandle_t arg)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL, NULL);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
    ESP_LOGI(BT_AV_TAG, "%s state: %d, event: 0x%x", __func__, s_a2d_state, event);

    /* select handler according to different states */
    switch (s_a2d_state) {
    case APP_AV_STATE_DISCOVERING:
    case APP_AV_STATE_DISCOVERED:
        break;
    case APP_AV_STATE_UNCONNECTED:
        bt_app_av_state_unconnected_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTING:
        bt_app_av_state_connecting_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTED:
        bt_app_av_state_connected_hdlr(event, param);
        break;
    case APP_AV_STATE_DISCONNECTING:
        bt_app_av_state_disconnecting_hdlr(event, param);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s invalid state: %d", __func__, s_a2d_state);
        break;
    }
}

static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    /* handle the events of interest in unconnected state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT: {
        uint8_t *bda = s_peer_bda;
        ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        esp_a2d_source_connect(s_peer_bda);
        s_a2d_state = APP_AV_STATE_CONNECTING;
        s_connecting_intv = 0;
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in connecting state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp connected");
            gpio_set_level(STATUS_GPIO, 1);
            s_a2d_state =  APP_AV_STATE_CONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT:
        /**
         * Switch state to APP_AV_STATE_UNCONNECTED
         * when connecting lasts more than 2 heart beat intervals.
         */
        if (++s_connecting_intv >= 2) {
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            s_connecting_intv = 0;
        }
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_app_av_media_proc(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    switch (s_media_state) {
    case APP_AV_MEDIA_STATE_IDLE: {
        if (event == BT_APP_HEART_BEAT_EVT) {
            ESP_LOGI(BT_AV_TAG, "a2dp media ready checking ...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media ready, starting ...");
                pcm_stream_prepare();
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                s_media_state = APP_AV_MEDIA_STATE_STARTING;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTING: {
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media start successfully.");
                s_media_state = APP_AV_MEDIA_STATE_STARTED;
            } else {
                /* not started successfully, transfer to idle state */
                ESP_LOGI(BT_AV_TAG, "a2dp media start failed.");
                pcm_stream_stop();
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTED: {
        if (event == BT_APP_HEART_BEAT_EVT) {
            /* Keep streaming indefinitely; report PCM supply health once
             * per heart beat (10 s). */
            pcm_stream_stats_print();
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STOPPING: {
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media suspend successfully, disconnecting...");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
                esp_a2d_source_disconnect(s_peer_bda);
                s_a2d_state = APP_AV_STATE_DISCONNECTING;
            } else {
                ESP_LOGI(BT_AV_TAG, "a2dp media suspending...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            }
        }
        break;
    }
    default: {
        break;
    }
    }
}

static void bt_app_av_state_connected_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in connected state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            gpio_set_level(STATUS_GPIO, 0);
            pcm_stream_stop();
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            s_pkt_cnt = 0;
            ESP_LOGI(BT_AV_TAG, "A2DP audio started; streaming UART PCM");
            /* Play the greeting once per streaming session; the UART feed
             * resumes automatically when the clip ends. */
            clip_play();
        } else {
            pcm_stream_stop();
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        // not supposed to occur for A2DP source
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT: {
        bt_app_av_media_proc(event, param);
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in disconnecing state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            gpio_set_level(STATUS_GPIO, 0);
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT:
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: 0x%u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

/* callback function for AVRCP controller */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
    case ESP_AVRC_CT_PROF_STATE_EVT: {
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL, NULL);
        break;
    }
    default: {
        ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
        break;
    }
    }
}

static void bt_av_volume_changed(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_VOLUME_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE, ESP_AVRC_RN_VOLUME_CHANGE, 0);
    }
}

void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id) {
    /* when volume changed locally on target, this event comes */
    case ESP_AVRC_RN_VOLUME_CHANGE: {
        ESP_LOGI(BT_RC_CT_TAG, "Volume changed: %d", event_parameter->volume);
        /* Respect volume changes made on the headset. The old code sent
         * volume + 5 here, which fought the volume-down button and could
         * ratchet the headset louder on every notification. */
        bt_av_volume_changed();
        break;
    }
    /* other */
    default:
        break;
    }
}

/* AVRC controller event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_RC_CT_TAG, "%s evt %d", __func__, event);
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);

    switch (event) {
    /* when connection state changed, this event comes */
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_RC_CT_TAG, "AVRC conn_state event: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
        } else {
            s_avrc_peer_rn_cap.bits = 0;
        }
        break;
    }
    /* when passthrough responded, this event comes */
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough response: key_code 0x%x, key_state %d, rsp_code %d", rc->psth_rsp.key_code,
                    rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
        break;
    }
    /* when notification changed, this event comes */
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
        bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    /* when indicate feature of remote device, this event comes */
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %"PRIx32", TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
        break;
    }
    /* when get supported notification events capability of peer device, this event comes */
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "remote rn_cap: count %d, bitmask 0x%x", rc->get_rn_caps_rsp.cap_count,
                 rc->get_rn_caps_rsp.evt_set.bits);
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;

        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                                               &s_avrc_peer_rn_cap,
                                               ESP_AVRC_RN_VOLUME_CHANGE)) {
            ESP_LOGI(BT_RC_CT_TAG, "Set initial headset volume: %d/127",
                     DEFAULT_HEADSET_VOLUME);
            esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_SET_VOLUME,
                                                     DEFAULT_HEADSET_VOLUME);
        }
        bt_av_volume_changed();
        break;
    }
    /* when set absolute volume responded, this event comes */
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume response: volume %d", rc->set_volume_rsp.volume);
        break;
    }
    /* other */
    default: {
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

/*********************************
 * MAIN ENTRY POINT
 ********************************/

void app_main(void)
{
    char bda_str[18] = {0};
    pcm_uart_init();
    ESP_LOGI(BT_AV_TAG, "Master PCM gain: %d/256 (software attenuation)",
             MASTER_GAIN_256);

    /* Status pin: high while the headset is connected, low otherwise. */
    gpio_reset_pin(STATUS_GPIO);
    gpio_set_direction(STATUS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(STATUS_GPIO, 0);
    /* initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /*
     * This example only uses the functions of Classical Bluetooth.
     * So release the controller memory for Bluetooth Low Energy.
     */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize controller failed", __func__);
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed", __func__);
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed", __func__);
        return;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(BT_AV_TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    bt_app_task_start_up();
    /* Bluetooth device name, connection mode and profile set up */
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_STACK_UP_EVT, NULL, 0, NULL, NULL);
}
