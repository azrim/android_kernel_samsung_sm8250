// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/asound.h>
#include <sound/pcm_params.h>
#include <sound/hwdep.h>
#include <sound/audio_effects.h>
#include <dsp/msm-dts-srs-tm-config.h>
#include <dsp/q6voice.h>
#include <dsp/q6adm-v2.h>
#include <dsp/q6asm-v2.h>
#include <dsp/q6afe-v2.h>
#include <dsp/q6lsm.h>
#include <dsp/q6core.h>
#include <dsp/q6common.h>
#include <dsp/audio_cal_utils.h>

#include "msm-pcm-routing-v2.h"
#include "msm-pcm-routing-devdep.h"
#include "msm-qti-pp-config.h"
#include "msm-dolby-dap-config.h"
#include "msm-ds2-dap-config.h"

#define DRV_NAME "msm-pcm-routing-v2"

#ifndef CONFIG_DOLBY_DAP
#undef DOLBY_ADM_COPP_TOPOLOGY_ID
#define DOLBY_ADM_COPP_TOPOLOGY_ID 0xFFFFFFFE
#endif

#ifndef CONFIG_DOLBY_DS2
#undef DS2_ADM_COPP_TOPOLOGY_ID
#define DS2_ADM_COPP_TOPOLOGY_ID 0xFFFFFFFF
#endif

/*
 * Concurrency & Lock Hierarchy:
 *
 * Strict locking hierarchy (top to bottom; never acquire in reverse order):
 *  1. Subsystem / Core Locks:
 *     - ALSA control rwsem (card->controls_rwsem) - held by ALSA core during kcontrol ops
 *     - ALSA PCM stream lock (substream->self_group.lock) - held during pcm_ops callbacks
 *     - Upper-layer driver session locks (e.g., pcm->lock in msm-pcm-loopback-v2)
 *
 *  2. Audio Routing Lock:
 *     - routing_lock (struct mutex)
 *       Protects: msm_bedais[], fe_dai_map[][], session_copp_map[][][],
 *       backend active flags, FE/BE session bitmasks, and ADM matrix routes.
 *
 *  3. Calibration Subsystem Lock:
 *     - cal_data[idx]->lock (struct mutex)
 *       Acquired temporarily within msm_routing_find_topology_on_index() while
 *       routing_lock may be held to look up calibration blocks. Never acquires
 *       any other lock while held.
 *
 *  4. DSP IPC / Subsystem Internal Locks:
 *     - Downstream APR/Q6 calls (q6adm, q6afe, q6voice) execute synchronous IPC.
 *       None of these downstream drivers acquire routing_lock.
 *
 * Concurrency Safety & Deadlock Prevention Rules:
 *  - Lock hierarchy must be strictly adhered to: never call back into outer
 *    subsystem locks (e.g. pcm->lock or controls_rwsem) while holding routing_lock.
 *  - routing_lock is non-recursive: no execution path may acquire routing_lock
 *    when already holding it.
 *  - Internal helpers called with routing_lock held must use lockdep_assert_held().
 *  - DAPM power updates: snd_soc_dapm_mixer_update_power() and
 *    snd_soc_dapm_mux_update_power() must be called outside routing_lock.
 */
static struct mutex routing_lock;

static struct cal_type_data *cal_data[MAX_ROUTING_CAL_TYPES];

static int fm_switch_enable;
static int hfp_switch_enable;
static int a2dp_switch_enable;
static int sco_switch_enable;
#ifndef CONFIG_MI2S_DISABLE
static int int0_mi2s_switch_enable;
static int int4_mi2s_switch_enable;
static int pri_mi2s_switch_enable;
static int sec_mi2s_switch_enable;
static int tert_mi2s_switch_enable;
static int quat_mi2s_switch_enable;
static int quin_mi2s_switch_enable;
static int sen_mi2s_switch_enable;
#endif
static int fm_pcmrx_switch_enable;
static int usb_switch_enable;
static int lsm_port_index[MAX_LSM_SESSIONS];
static int slim0_rx_aanc_fb_port;
static int msm_route_ec_ref_rx;
static int msm_ec_ref_ch = 4;
static int msm_ec_ref_ch_downmixed = 4;
static int msm_ec_ref_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int msm_ec_ref_sampling_rate = 48000;
static uint32_t voc_session_id = ALL_SESSION_VSID;
static int msm_route_ext_ec_ref;
static bool is_custom_stereo_on;
static bool is_ds2_on;
static bool ffecns_freeze_event;
static bool swap_ch;
static bool hifi_filter_enabled;
static int aanc_level;
static int num_app_cfg_types;
static int msm_ec_ref_port_id;
static int afe_loopback_tx_port_index;
static int afe_loopback_tx_port_id = -1;

#define WEIGHT_0_DB 0x4000
/* all the FEs which can support channel mixer */
static struct msm_pcm_channel_mixer channel_mixer[MSM_FRONTEND_DAI_MM_SIZE];

/* all the FES which can support channel mixer for bidirection */
static struct msm_pcm_channel_mixer
	channel_mixer_v2[MSM_FRONTEND_DAI_MM_SIZE][2];

/* input BE for each FE */
static int channel_input[MSM_FRONTEND_DAI_MM_SIZE][ADM_MAX_CHANNELS];

enum {
	MADNONE,
	MADAUDIO,
	MADBEACON,
	MADULTRASOUND,
	MADSWAUDIO,
};

#define ADM_LSM_PORT_INDEX 9

#define SLIMBUS_0_TX_TEXT "SLIMBUS_0_TX"
#define SLIMBUS_1_TX_TEXT "SLIMBUS_1_TX"
#define SLIMBUS_2_TX_TEXT "SLIMBUS_2_TX"
#define SLIMBUS_3_TX_TEXT "SLIMBUS_3_TX"
#define SLIMBUS_4_TX_TEXT "SLIMBUS_4_TX"
#define SLIMBUS_5_TX_TEXT "SLIMBUS_5_TX"
#define TERT_MI2S_TX_TEXT "TERT_MI2S_TX"
#define QUAT_MI2S_TX_TEXT "QUAT_MI2S_TX"
#define ADM_LSM_TX_TEXT "ADM_LSM_TX"
#define INT3_MI2S_TX_TEXT "INT3_MI2S_TX"
#define VA_CDC_DMA_TX_0_TEXT "VA_CDC_DMA_TX_0"
#define VA_CDC_DMA_TX_1_TEXT "VA_CDC_DMA_TX_1"
#define VA_CDC_DMA_TX_2_TEXT "VA_CDC_DMA_TX_2"
#define TX_CDC_DMA_TX_3_TEXT "TX_CDC_DMA_TX_3"
#define QUIN_TDM_TX_TEXT "QUIN_TDM_TX_0"
#define TERT_TDM_TX_TEXT "TERT_TDM_TX_0"

#define LSM_FUNCTION_TEXT "LSM Function"
static const char * const lsm_port_text[] = {
	"None",
	SLIMBUS_0_TX_TEXT, SLIMBUS_1_TX_TEXT, SLIMBUS_2_TX_TEXT,
	SLIMBUS_3_TX_TEXT, SLIMBUS_4_TX_TEXT, SLIMBUS_5_TX_TEXT,
	TERT_MI2S_TX_TEXT, QUAT_MI2S_TX_TEXT, ADM_LSM_TX_TEXT,
	INT3_MI2S_TX_TEXT, VA_CDC_DMA_TX_0_TEXT, VA_CDC_DMA_TX_1_TEXT,
	VA_CDC_DMA_TX_2_TEXT, TX_CDC_DMA_TX_3_TEXT, QUIN_TDM_TX_TEXT,
	TERT_TDM_TX_TEXT
};

struct msm_pcm_route_bdai_pp_params {
	u16 port_id; /* AFE port ID */
	unsigned long pp_params_config;
	bool mute_on;
	int latency;
};

static struct msm_pcm_route_bdai_pp_params
	msm_bedais_pp_params[MSM_BACKEND_DAI_PP_PARAMS_REQ_MAX] = {
	{HDMI_RX, 0, 0, 0},
	{HDMI_RX_MS, 0, 0, 0},
	{DISPLAY_PORT_RX, 0, 0, 0},
};

/*
 * The be_dai_name_table is passed to HAL so that it can specify the
 * BE ID for the BE it wants to enable based on the name. Thus there
 * is a matching table and structure in HAL that need to be updated
 * if any changes to these are made.
 */
struct msm_pcm_route_bdai_name {
	unsigned int be_id;
	char be_name[LPASS_BE_NAME_MAX_LENGTH];
};
static struct msm_pcm_route_bdai_name be_dai_name_table[MSM_BACKEND_DAI_MAX];

static int msm_routing_send_device_pp_params(int port_id,  int copp_idx,
					     int fe_id);

static void msm_routing_load_topology(size_t data_size, void *data);
static void msm_routing_unload_topology(uint32_t topology_id);

static int msm_routing_get_bit_width(unsigned int format)
{
	int bit_width;

	switch (format) {
	case SNDRV_PCM_FORMAT_S32_LE:
		bit_width = 32;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_3LE:
		bit_width = 24;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		bit_width = 16;
	}
	return bit_width;
}

static bool msm_is_resample_needed(int input_sr, int output_sr)
{
	bool rc = false;

	if (input_sr != output_sr)
		rc = true;

	pr_debug("perform resampling (%s) for copp rate (%d)afe rate (%d)",
		(rc ? "oh yes" : "not really"),
		input_sr, output_sr);

	return rc;
}

static void msm_pcm_routing_cfg_pp(int port_id, int copp_idx, int topology,
				   int channels)
{
	int rc = 0;

	switch (topology) {
	case SRS_TRUMEDIA_TOPOLOGY_ID:
		pr_debug("%s: SRS_TRUMEDIA_TOPOLOGY_ID\n", __func__);
		msm_dts_srs_tm_init(port_id, copp_idx);
		break;
	case DS2_ADM_COPP_TOPOLOGY_ID:
		pr_debug("%s: DS2_ADM_COPP_TOPOLOGY %d\n",
			 __func__, DS2_ADM_COPP_TOPOLOGY_ID);
		rc = msm_ds2_dap_init(port_id, copp_idx, channels,
				      is_custom_stereo_on);
		if (rc < 0)
			pr_err("%s: DS2 topo_id 0x%x, port %d, CS %d rc %d\n",
				__func__, topology, port_id,
				is_custom_stereo_on, rc);
		break;
	case DOLBY_ADM_COPP_TOPOLOGY_ID:
		if (is_ds2_on) {
			pr_debug("%s: DS2_ADM_COPP_TOPOLOGY\n", __func__);
			rc = msm_ds2_dap_init(port_id, copp_idx, channels,
				is_custom_stereo_on);
			if (rc < 0)
				pr_err("%s:DS2 topo_id 0x%x, port %d, rc %d\n",
					__func__, topology, port_id, rc);
		} else {
			pr_debug("%s: DOLBY_ADM_COPP_TOPOLOGY_ID\n", __func__);
			rc = msm_dolby_dap_init(port_id, copp_idx, channels,
						is_custom_stereo_on);
			if (rc < 0)
				pr_err("%s: DS1 topo_id 0x%x, port %d, rc %d\n",
					__func__, topology, port_id, rc);
		}
		break;
	case ADM_CMD_COPP_OPEN_TOPOLOGY_ID_AUDIOSPHERE:
		pr_debug("%s: TOPOLOGY_ID_AUDIOSPHERE\n", __func__);
		rc = msm_qti_pp_asphere_init(port_id, copp_idx);
		if (rc < 0)
			pr_err("%s: topo_id 0x%x, port %d, copp %d, rc %d\n",
				__func__, topology, port_id, copp_idx, rc);
		break;
	default:
		/* custom topology specific feature param handlers */
		break;
	}
}

static void msm_pcm_routing_deinit_pp(int port_id, int topology)
{
	switch (topology) {
	case SRS_TRUMEDIA_TOPOLOGY_ID:
		pr_debug("%s: SRS_TRUMEDIA_TOPOLOGY_ID\n", __func__);
		msm_dts_srs_tm_deinit(port_id);
		break;
	case DS2_ADM_COPP_TOPOLOGY_ID:
		pr_debug("%s: DS2_ADM_COPP_TOPOLOGY_ID %d\n",
			 __func__, DS2_ADM_COPP_TOPOLOGY_ID);
		msm_ds2_dap_deinit(port_id);
		break;
	case DOLBY_ADM_COPP_TOPOLOGY_ID:
		if (is_ds2_on) {
			pr_debug("%s: DS2_ADM_COPP_TOPOLOGY_ID\n", __func__);
			msm_ds2_dap_deinit(port_id);
		} else {
			pr_debug("%s: DOLBY_ADM_COPP_TOPOLOGY_ID\n", __func__);
			msm_dolby_dap_deinit(port_id);
		}
		break;
	case ADM_CMD_COPP_OPEN_TOPOLOGY_ID_AUDIOSPHERE:
		pr_debug("%s: TOPOLOGY_ID_AUDIOSPHERE\n", __func__);
		msm_qti_pp_asphere_deinit(port_id);
		break;
	default:
		/* custom topology specific feature deinit handlers */
		break;
	}
}

static void msm_pcm_routng_cfg_matrix_map_pp(struct route_payload payload,
					     int path_type, int perf_mode)
{
	int itr = 0, rc = 0;

	if ((path_type == ADM_PATH_PLAYBACK) &&
	    (perf_mode == LEGACY_PCM_MODE) &&
	    is_custom_stereo_on) {
		for (itr = 0; itr < payload.num_copps; itr++) {
			if ((payload.port_id[itr] != SLIMBUS_0_RX) &&
			    (payload.port_id[itr] != RT_PROXY_PORT_001_RX)) {
				continue;
			}

			rc = msm_qti_pp_send_stereo_to_custom_stereo_cmd(
				payload.port_id[itr],
				payload.copp_idx[itr],
				payload.session_id,
				Q14_GAIN_ZERO_POINT_FIVE,
				Q14_GAIN_ZERO_POINT_FIVE,
				Q14_GAIN_ZERO_POINT_FIVE,
				Q14_GAIN_ZERO_POINT_FIVE);
			if (rc < 0)
				pr_err("%s: err setting custom stereo\n",
					__func__);
		}
	}
}

#define SLIMBUS_EXTPROC_RX AFE_PORT_INVALID
struct msm_pcm_routing_bdai_data msm_bedais[MSM_BACKEND_DAI_MAX] = {
	{ PRIMARY_I2S_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_PRI_I2S_RX},
	{ PRIMARY_I2S_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_PRI_I2S_TX},
	{ SLIMBUS_0_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_0_RX},
	{ SLIMBUS_0_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_0_TX},
	{ HDMI_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_HDMI},
	{ INT_BT_SCO_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_INT_BT_SCO_RX},
	{ INT_BT_SCO_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_INT_BT_SCO_TX},
	{ INT_FM_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_INT_FM_RX},
	{ INT_FM_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_INT_FM_TX},
	{ RT_PROXY_PORT_001_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_AFE_PCM_RX},
	{ RT_PROXY_PORT_001_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_AFE_PCM_TX},
	{ AFE_PORT_ID_PRIMARY_PCM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_AUXPCM_RX},
	{ AFE_PORT_ID_PRIMARY_PCM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_AUXPCM_TX},
	{ VOICE_PLAYBACK_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_VOICE_PLAYBACK_TX},
	{ VOICE2_PLAYBACK_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_VOICE2_PLAYBACK_TX},
	{ VOICE_RECORD_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INCALL_RECORD_RX},
	{ VOICE_RECORD_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INCALL_RECORD_TX},
	{ MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_MI2S_RX},
	{ MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_MI2S_TX},
	{ SECONDARY_I2S_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SEC_I2S_RX},
	{ SLIMBUS_1_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_1_RX},
	{ SLIMBUS_1_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_1_TX},
	{ SLIMBUS_2_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_2_RX},
	{ SLIMBUS_2_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_2_TX},
	{ SLIMBUS_3_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_3_RX},
	{ SLIMBUS_3_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_3_TX},
	{ SLIMBUS_4_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_4_RX},
	{ SLIMBUS_4_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_4_TX},
	{ SLIMBUS_5_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_5_RX},
	{ SLIMBUS_5_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_5_TX},
	{ SLIMBUS_6_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_6_RX},
	{ SLIMBUS_6_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_6_TX},
	{ SLIMBUS_7_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_7_RX},
	{ SLIMBUS_7_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_7_TX},
	{ SLIMBUS_8_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_8_RX},
	{ SLIMBUS_8_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_8_TX},
	{ SLIMBUS_EXTPROC_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_STUB_RX},
	{ SLIMBUS_EXTPROC_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_STUB_TX},
	{ SLIMBUS_EXTPROC_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_STUB_1_TX},
	{ AFE_PORT_ID_QUATERNARY_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_MI2S_RX},
	{ AFE_PORT_ID_QUATERNARY_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_MI2S_TX},
	{ AFE_PORT_ID_SECONDARY_MI2S_RX,  0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_MI2S_RX},
	{ AFE_PORT_ID_SECONDARY_MI2S_TX,  0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_MI2S_TX},
	{ AFE_PORT_ID_PRIMARY_MI2S_RX,    0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_MI2S_RX},
	{ AFE_PORT_ID_PRIMARY_MI2S_TX,    0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_MI2S_TX},
	{ AFE_PORT_ID_TERTIARY_MI2S_RX,   0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_MI2S_RX},
	{ AFE_PORT_ID_TERTIARY_MI2S_TX,   0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_MI2S_TX},
	{ AUDIO_PORT_ID_I2S_RX,           0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_AUDIO_I2S_RX},
	{ AFE_PORT_ID_SECONDARY_PCM_RX,	  0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_AUXPCM_RX},
	{ AFE_PORT_ID_SECONDARY_PCM_TX,   0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_AUXPCM_TX},
	{ AFE_PORT_ID_PRIMARY_SPDIF_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_SPDIF_RX},
	{ AFE_PORT_ID_SECONDARY_MI2S_RX_SD1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_MI2S_RX_SD1},
	{ AFE_PORT_ID_QUINARY_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_MI2S_RX},
	{ AFE_PORT_ID_QUINARY_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_MI2S_TX},
	{ AFE_PORT_ID_SENARY_MI2S_TX,   0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SENARY_MI2S_TX},
	{ AFE_PORT_ID_PRIMARY_TDM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_0},
	{ AFE_PORT_ID_PRIMARY_TDM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_0},
	{ AFE_PORT_ID_PRIMARY_TDM_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_1},
	{ AFE_PORT_ID_PRIMARY_TDM_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_1},
	{ AFE_PORT_ID_PRIMARY_TDM_RX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_2},
	{ AFE_PORT_ID_PRIMARY_TDM_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_2},
	{ AFE_PORT_ID_PRIMARY_TDM_RX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_3},
	{ AFE_PORT_ID_PRIMARY_TDM_TX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_3},
	{ AFE_PORT_ID_PRIMARY_TDM_RX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_4},
	{ AFE_PORT_ID_PRIMARY_TDM_TX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_4},
	{ AFE_PORT_ID_PRIMARY_TDM_RX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_5},
	{ AFE_PORT_ID_PRIMARY_TDM_TX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_5},
	{ AFE_PORT_ID_PRIMARY_TDM_RX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_6},
	{ AFE_PORT_ID_PRIMARY_TDM_TX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_6},
	{ AFE_PORT_ID_PRIMARY_TDM_RX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_RX_7},
	{ AFE_PORT_ID_PRIMARY_TDM_TX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_TDM_TX_7},
	{ AFE_PORT_ID_SECONDARY_TDM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_0},
	{ AFE_PORT_ID_SECONDARY_TDM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_0},
	{ AFE_PORT_ID_SECONDARY_TDM_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_1},
	{ AFE_PORT_ID_SECONDARY_TDM_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_1},
	{ AFE_PORT_ID_SECONDARY_TDM_RX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_2},
	{ AFE_PORT_ID_SECONDARY_TDM_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_2},
	{ AFE_PORT_ID_SECONDARY_TDM_RX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_3},
	{ AFE_PORT_ID_SECONDARY_TDM_TX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_3},
	{ AFE_PORT_ID_SECONDARY_TDM_RX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_4},
	{ AFE_PORT_ID_SECONDARY_TDM_TX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_4},
	{ AFE_PORT_ID_SECONDARY_TDM_RX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_5},
	{ AFE_PORT_ID_SECONDARY_TDM_TX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_5},
	{ AFE_PORT_ID_SECONDARY_TDM_RX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_6},
	{ AFE_PORT_ID_SECONDARY_TDM_TX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_6},
	{ AFE_PORT_ID_SECONDARY_TDM_RX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_RX_7},
	{ AFE_PORT_ID_SECONDARY_TDM_TX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_TDM_TX_7},
	{ AFE_PORT_ID_TERTIARY_TDM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_0},
	{ AFE_PORT_ID_TERTIARY_TDM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_0},
	{ AFE_PORT_ID_TERTIARY_TDM_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_1},
	{ AFE_PORT_ID_TERTIARY_TDM_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_1},
	{ AFE_PORT_ID_TERTIARY_TDM_RX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_2},
	{ AFE_PORT_ID_TERTIARY_TDM_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_2},
	{ AFE_PORT_ID_TERTIARY_TDM_RX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_3},
	{ AFE_PORT_ID_TERTIARY_TDM_TX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_3},
	{ AFE_PORT_ID_TERTIARY_TDM_RX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_4},
	{ AFE_PORT_ID_TERTIARY_TDM_TX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_4},
	{ AFE_PORT_ID_TERTIARY_TDM_RX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_5},
	{ AFE_PORT_ID_TERTIARY_TDM_TX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_5},
	{ AFE_PORT_ID_TERTIARY_TDM_RX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_6},
	{ AFE_PORT_ID_TERTIARY_TDM_TX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_6},
	{ AFE_PORT_ID_TERTIARY_TDM_RX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_RX_7},
	{ AFE_PORT_ID_TERTIARY_TDM_TX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_TDM_TX_7},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_0},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_0},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_1},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_1},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_2},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_2},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_3},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_3},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_4},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_4},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_5},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_5},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_6},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_6},
	{ AFE_PORT_ID_QUATERNARY_TDM_RX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_RX_7},
	{ AFE_PORT_ID_QUATERNARY_TDM_TX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_TDM_TX_7},
	{ AFE_PORT_ID_QUINARY_TDM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_0},
	{ AFE_PORT_ID_QUINARY_TDM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_0},
	{ AFE_PORT_ID_QUINARY_TDM_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_1},
	{ AFE_PORT_ID_QUINARY_TDM_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_1},
	{ AFE_PORT_ID_QUINARY_TDM_RX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_2},
	{ AFE_PORT_ID_QUINARY_TDM_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_2},
	{ AFE_PORT_ID_QUINARY_TDM_RX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_3},
	{ AFE_PORT_ID_QUINARY_TDM_TX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_3},
	{ AFE_PORT_ID_QUINARY_TDM_RX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_4},
	{ AFE_PORT_ID_QUINARY_TDM_TX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_4},
	{ AFE_PORT_ID_QUINARY_TDM_RX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_5},
	{ AFE_PORT_ID_QUINARY_TDM_TX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_5},
	{ AFE_PORT_ID_QUINARY_TDM_RX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_6},
	{ AFE_PORT_ID_QUINARY_TDM_TX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_6},
	{ AFE_PORT_ID_QUINARY_TDM_RX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_RX_7},
	{ AFE_PORT_ID_QUINARY_TDM_TX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_TDM_TX_7},
	{ AFE_PORT_ID_SENARY_TDM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_0},
	{ AFE_PORT_ID_SENARY_TDM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_0},
	{ AFE_PORT_ID_SENARY_TDM_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_1},
	{ AFE_PORT_ID_SENARY_TDM_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_1},
	{ AFE_PORT_ID_SENARY_TDM_RX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_2},
	{ AFE_PORT_ID_SENARY_TDM_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_2},
	{ AFE_PORT_ID_SENARY_TDM_RX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_3},
	{ AFE_PORT_ID_SENARY_TDM_TX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_3},
	{ AFE_PORT_ID_SENARY_TDM_RX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_4},
	{ AFE_PORT_ID_SENARY_TDM_TX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_4},
	{ AFE_PORT_ID_SENARY_TDM_RX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_5},
	{ AFE_PORT_ID_SENARY_TDM_TX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_5},
	{ AFE_PORT_ID_SENARY_TDM_RX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_6},
	{ AFE_PORT_ID_SENARY_TDM_TX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_6},
	{ AFE_PORT_ID_SENARY_TDM_RX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_RX_7},
	{ AFE_PORT_ID_SENARY_TDM_TX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_TDM_TX_7},
	{ INT_BT_A2DP_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT_BT_A2DP_RX},
	{ AFE_PORT_ID_USB_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_USB_AUDIO_RX},
	{ AFE_PORT_ID_USB_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_USB_AUDIO_TX},
	{ DISPLAY_PORT_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_DISPLAY_PORT},
	{ DISPLAY_PORT_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_DISPLAY_PORT1},
	{ AFE_PORT_ID_TERTIARY_PCM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_AUXPCM_RX},
	{ AFE_PORT_ID_TERTIARY_PCM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TERT_AUXPCM_TX},
	{ AFE_PORT_ID_QUATERNARY_PCM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_AUXPCM_RX},
	{ AFE_PORT_ID_QUATERNARY_PCM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUAT_AUXPCM_TX},
	{ AFE_PORT_ID_QUINARY_PCM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_AUXPCM_RX},
	{ AFE_PORT_ID_QUINARY_PCM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_QUIN_AUXPCM_TX},
	{ AFE_PORT_ID_INT0_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT0_MI2S_RX},
	{ AFE_PORT_ID_INT0_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT0_MI2S_TX},
	{ AFE_PORT_ID_INT1_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT1_MI2S_RX},
	{ AFE_PORT_ID_INT1_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT1_MI2S_TX},
	{ AFE_PORT_ID_INT2_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT2_MI2S_RX},
	{ AFE_PORT_ID_INT2_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT2_MI2S_TX},
	{ AFE_PORT_ID_INT3_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT3_MI2S_RX},
	{ AFE_PORT_ID_INT3_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT3_MI2S_TX},
	{ AFE_PORT_ID_INT4_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT4_MI2S_RX},
	{ AFE_PORT_ID_INT4_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT4_MI2S_TX},
	{ AFE_PORT_ID_INT5_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT5_MI2S_RX},
	{ AFE_PORT_ID_INT5_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT5_MI2S_TX},
	{ AFE_PORT_ID_INT6_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT6_MI2S_RX},
	{ AFE_PORT_ID_INT6_MI2S_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_INT6_MI2S_TX},
	{ AFE_PORT_ID_SENARY_PCM_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_AUXPCM_RX},
	{ AFE_PORT_ID_SENARY_PCM_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEN_AUXPCM_TX},
	{ AFE_PORT_ID_SENARY_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SENARY_MI2S_RX},
	{ AFE_PORT_ID_WSA_CODEC_DMA_RX_0, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_WSA_CDC_DMA_RX_0},
	{ AFE_PORT_ID_WSA_CODEC_DMA_TX_0, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_WSA_CDC_DMA_TX_0},
	{ AFE_PORT_ID_WSA_CODEC_DMA_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_WSA_CDC_DMA_RX_1},
	{ AFE_PORT_ID_WSA_CODEC_DMA_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_WSA_CDC_DMA_TX_1},
	{ AFE_PORT_ID_WSA_CODEC_DMA_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_WSA_CDC_DMA_TX_2},
	{ AFE_PORT_ID_VA_CODEC_DMA_TX_0, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_VA_CDC_DMA_TX_0},
	{ AFE_PORT_ID_VA_CODEC_DMA_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_VA_CDC_DMA_TX_1},
	{ AFE_PORT_ID_VA_CODEC_DMA_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_VA_CDC_DMA_TX_2},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_0, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_0},
	{ AFE_PORT_ID_TX_CODEC_DMA_TX_0, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TX_CDC_DMA_TX_0},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_1},
	{ AFE_PORT_ID_TX_CODEC_DMA_TX_1, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TX_CDC_DMA_TX_1},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_2},
	{ AFE_PORT_ID_TX_CODEC_DMA_TX_2, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TX_CDC_DMA_TX_2},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_3},
	{ AFE_PORT_ID_TX_CODEC_DMA_TX_3, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TX_CDC_DMA_TX_3},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_4},
	{ AFE_PORT_ID_TX_CODEC_DMA_TX_4, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TX_CDC_DMA_TX_4},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_5},
	{ AFE_PORT_ID_TX_CODEC_DMA_TX_5, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_TX_CDC_DMA_TX_5},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_6, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_6},
	{ AFE_PORT_ID_RX_CODEC_DMA_RX_7, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_RX_CDC_DMA_RX_7},
	{ AFE_PORT_ID_PRIMARY_SPDIF_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_SPDIF_TX},
	{ AFE_PORT_ID_SECONDARY_SPDIF_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_SPDIF_RX},
	{ AFE_PORT_ID_SECONDARY_SPDIF_TX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_SPDIF_TX},
	{ SLIMBUS_9_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_9_RX},
	{ SLIMBUS_9_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_SLIMBUS_9_TX},
	{ AFE_LOOPBACK_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_AFE_LOOPBACK_TX},
	{ AFE_PORT_ID_PRIMARY_META_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_PRI_META_MI2S_RX},
	{ AFE_PORT_ID_SECONDARY_META_MI2S_RX, 0, {0}, {0}, 0, 0, 0, 0,
	  LPASS_BE_SEC_META_MI2S_RX},
	{ RT_PROXY_PORT_002_RX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_PROXY_RX},
	{ RT_PROXY_PORT_002_TX, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_PROXY_TX},
	{ HDMI_RX_MS, 0, {0}, {0}, 0, 0, 0, 0, LPASS_BE_HDMI_MS},
};

/* Track ASM playback & capture sessions of DAI
 * Track LSM listen sessions
 */
static struct msm_pcm_routing_fdai_data
	fe_dai_map[MSM_FRONTEND_DAI_MAX][2] = {
	/* MULTIMEDIA1 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA2 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA3 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA4 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA5 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA6 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA7*/
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA8 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA9 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA10 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA11 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA12 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA13 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA14 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA15 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA16 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA17 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA18 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA19 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA20 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA21 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA22 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA23 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA24 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA25 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA26 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA27 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA28 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA29 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* MULTIMEDIA30 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* VOIP */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* AFE_RX */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* AFE_TX */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* VOICE_STUB */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* DTMF_RX */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* QCHAT */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* VOLTE_STUB */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM1 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM2 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM3 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM4 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM5 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM6 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM7 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* LSM8 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* VOICE2_STUB */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* VOICEMMODE1 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
	/* VOICEMMODE2 */
	{{0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM},
	 {0, INVALID_SESSION, LEGACY_PCM_MODE, {NULL, NULL}, LEGACY_PCM} },
};

static unsigned long session_copp_map[MSM_FRONTEND_DAI_MAX][2]
				     [MSM_BACKEND_DAI_MAX];
static struct msm_pcm_routing_app_type_data app_type_cfg[MAX_APP_TYPES];
static struct msm_pcm_routing_app_type_data lsm_app_type_cfg[MAX_APP_TYPES];
static struct msm_pcm_stream_app_type_cfg
	fe_dai_app_type_cfg[MSM_FRONTEND_DAI_MAX][2][MSM_BACKEND_DAI_MAX];

static int last_be_id_configured[MSM_FRONTEND_DAI_MAX][MAX_SESSION_TYPES];

/* The caller of this should acquire routing lock */
void msm_pcm_routing_get_bedai_info(int be_idx,
				    struct msm_pcm_routing_bdai_data *be_dai)
{
	if (be_idx >= 0 && be_idx < MSM_BACKEND_DAI_MAX)
		memcpy(be_dai, &msm_bedais[be_idx],
		       sizeof(struct msm_pcm_routing_bdai_data));
}

/* The caller of this should acquire routing lock */
void msm_pcm_routing_get_fedai_info(int fe_idx, int sess_type,
				    struct msm_pcm_routing_fdai_data *fe_dai)
{
	if ((sess_type == SESSION_TYPE_TX) || (sess_type == SESSION_TYPE_RX))
		memcpy(fe_dai, &fe_dai_map[fe_idx][sess_type],
		       sizeof(struct msm_pcm_routing_fdai_data));
}

void msm_pcm_routing_acquire_lock(void)
{
	mutex_lock(&routing_lock);
}

void msm_pcm_routing_release_lock(void)
{
	mutex_unlock(&routing_lock);
}

static int msm_pcm_routing_get_app_type_idx(int app_type)
{
	int idx;

	pr_debug("%s: app_type: %d\n", __func__, app_type);
	for (idx = 0; idx < MAX_APP_TYPES; idx++) {
		if (app_type_cfg[idx].app_type == app_type)
			return idx;
	}
	pr_info("%s: App type not available, fallback to default\n", __func__);
	return 0;
}

static int msm_pcm_routing_get_lsm_app_type_idx(int app_type)
{
	int idx;

	pr_debug("%s: app_type: %d\n", __func__, app_type);
	for (idx = 0; idx < MAX_APP_TYPES; idx++) {
		if (lsm_app_type_cfg[idx].app_type == app_type)
			return idx;
	}
	pr_debug("%s: App type not available, fallback to default\n", __func__);
	return 0;
}

static int get_port_id(int port_id)
{
	int ret = port_id;

	if (port_id == AFE_LOOPBACK_TX) {
		/*
		 * Return afe_loopback_tx_port_id if set. Else return
		 * msm_ec_ref_port_id to maintain backward compatibility.
		 */
		if (afe_loopback_tx_port_id != -1)
			ret = afe_loopback_tx_port_id;
		else
			ret = msm_ec_ref_port_id;
	}

	return ret;
}

static bool is_mm_lsm_fe_id(int fe_id)
{
	bool rc = true;

	if (fe_id > MSM_FRONTEND_DAI_MM_MAX_ID &&
		((fe_id < MSM_FRONTEND_DAI_LSM1) ||
		 (fe_id > MSM_FRONTEND_DAI_LSM8))) {
		rc = false;
	}
	return rc;
}

/*
 * msm_pcm_routing_send_chmix_cfg:
 *	send the channel mixer command to mix the input channels
 *	into output channels.
 *
 * @fe_id: front end id
 * @ip_channel_cnt: input channel count
 * @op_channel_cnt: output channel count
 * @ch_wght_coeff: channel weight co-efficients for channel mixing
 * @session_type: indicates session is of type TX or RX
 * @stream_type: indicates either Audio or Listen stream type
 */
int msm_pcm_routing_send_chmix_cfg(int fe_id, int ip_channel_cnt,
				int op_channel_cnt, int *ch_wght_coeff,
				int session_type, int stream_type)
{

	int rc = 0, idx = 0;
	int be_index = 0, port_id;
	unsigned int session_id = 0;

	pr_debug("%s:fe_id[%d] ip_ch[%d] op_ch[%d] sess_type [%d], stream_type[%d]",
		 __func__, fe_id, ip_channel_cnt, op_channel_cnt, session_type,
		 stream_type);
	if (!is_mm_lsm_fe_id(fe_id)) {
		/* bad ID assigned in machine driver */
		pr_err("%s: bad MM ID %d\n", __func__, fe_id);
		return -EINVAL;
	}

	if (ch_wght_coeff == NULL) {
		pr_err("%s: Null channel weightage coefficients passed\n",
			__func__);
		return -EINVAL;
	}

	for (be_index = 0; be_index < MSM_BACKEND_DAI_MAX; be_index++) {
		port_id = msm_bedais[be_index].port_id;
		if (!msm_bedais[be_index].active ||
		    !test_bit(fe_id, &msm_bedais[be_index].fe_sessions[0]))
			continue;

		session_id = fe_dai_map[fe_id][session_type].strm_id;

		for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++) {
			unsigned long copp =
				session_copp_map[fe_id][session_type][be_index];
			if (!test_bit(idx, &copp))
				continue;
			msm_qti_pp_send_chmix_cfg_cmd(port_id, idx,
						session_id, ip_channel_cnt,
						op_channel_cnt, ch_wght_coeff,
						session_type, stream_type);
			if (rc < 0)
				pr_err("%s: err setting channel mix config\n",
					__func__);
		}
	}

	return 0;
}
EXPORT_SYMBOL(msm_pcm_routing_send_chmix_cfg);

/**
 * msm_pcm_routing_set_channel_mixer_cfg - cache channel mixer
 * setting before use case start.
 *
 * @fe_id: frontend idx
 * @type: stream direction type
 * @params: parameters of channel mixer setting
 *
 * Return 0 for success
 */
int msm_pcm_routing_set_channel_mixer_cfg(
	int fe_id, int type,
	struct msm_pcm_channel_mixer *params)
{
	int i, j = 0;

	channel_mixer_v2[fe_id][type].enable = params->enable;
	channel_mixer_v2[fe_id][type].rule = params->rule;
	channel_mixer_v2[fe_id][type].input_channel =
		params->input_channel;
	channel_mixer_v2[fe_id][type].output_channel =
		params->output_channel;
	channel_mixer_v2[fe_id][type].port_idx = params->port_idx;

	for (i = 0; i < ADM_MAX_CHANNELS; i++)
		channel_mixer_v2[fe_id][type].in_ch_map[i] =
			params->in_ch_map[i];
	for (i = 0; i < ADM_MAX_CHANNELS; i++)
		channel_mixer_v2[fe_id][type].out_ch_map[i] =
			params->out_ch_map[i];

	for (i = 0; i < ADM_MAX_CHANNELS; i++)
		for (j = 0; j < ADM_MAX_CHANNELS; j++)
			channel_mixer_v2[fe_id][type].channel_weight[i][j] =
				params->channel_weight[i][j];

	channel_mixer_v2[fe_id][type].override_in_ch_map =
			params->override_in_ch_map;
	channel_mixer_v2[fe_id][type].override_out_ch_map =
			params->override_out_ch_map;

	return 0;
}
EXPORT_SYMBOL(msm_pcm_routing_set_channel_mixer_cfg);

int msm_pcm_routing_reg_stream_app_type_cfg(
	int fedai_id, int session_type, int be_id,
	struct msm_pcm_stream_app_type_cfg *cfg_data)
{
	int ret = 0;

	if (cfg_data == NULL) {
		pr_err("%s: Received NULL pointer for cfg_data\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	pr_debug("%s: fedai_id %d, session_type %d, be_id %d, app_type %d, acdb_dev_id %d, sample_rate %d\n",
		__func__, fedai_id, session_type, be_id,
		cfg_data->app_type, cfg_data->acdb_dev_id,
		cfg_data->sample_rate);

	if (!is_mm_lsm_fe_id(fedai_id)) {
		pr_err("%s: Invalid machine driver ID %d\n",
			__func__, fedai_id);
		ret = -EINVAL;
		goto done;
	}
	if (session_type != SESSION_TYPE_RX &&
		session_type != SESSION_TYPE_TX) {
		pr_err("%s: Invalid session type %d\n",
			__func__, session_type);
		ret = -EINVAL;
		goto done;
	}
	if (be_id < 0 || be_id >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: Received out of bounds be_id %d\n",
			__func__, be_id);
		ret = -EINVAL;
		goto done;
	}

	fe_dai_app_type_cfg[fedai_id][session_type][be_id] = *cfg_data;

	/*
	 * Store the BE ID of the configuration information set as the latest so
	 * the get mixer control knows what to return.
	 */
	last_be_id_configured[fedai_id][session_type] = be_id;

done:
	return ret;
}
EXPORT_SYMBOL(msm_pcm_routing_reg_stream_app_type_cfg);

/**
 * msm_pcm_routing_get_stream_app_type_cfg
 *
 * Receives fedai_id, session_type, be_id, and populates app_type,
 * acdb_dev_id, & sample rate. Returns 0 on success. On failure returns
 * -EINVAL and does not alter passed values.
 *
 * fedai_id - Passed value, front end ID for which app type config is wanted
 * session_type - Passed value, session type for which app type config
 *                is wanted
 * be_id - Returned value, back end device id the app type config data is for
 * cfg_data - Returned value, configuration data used by app type config
 */
int msm_pcm_routing_get_stream_app_type_cfg(
	int fedai_id, int session_type, int *bedai_id,
	struct msm_pcm_stream_app_type_cfg *cfg_data)
{
	int be_id;
	int ret = 0;

	if (bedai_id == NULL) {
		pr_err("%s: Received NULL pointer for backend ID\n", __func__);
		ret = -EINVAL;
		goto done;
	} else if (cfg_data == NULL) {
		pr_err("%s: NULL pointer sent for cfg_data\n", __func__);
		ret = -EINVAL;
		goto done;
	} else if (!is_mm_lsm_fe_id(fedai_id)) {
		pr_err("%s: Invalid FE ID %d\n", __func__, fedai_id);
		ret = -EINVAL;
		goto done;
	} else if (session_type != SESSION_TYPE_RX &&
		   session_type != SESSION_TYPE_TX) {
		pr_err("%s: Invalid session type %d\n", __func__, session_type);
		ret = -EINVAL;
		goto done;
	}

	be_id = last_be_id_configured[fedai_id][session_type];
	if (be_id < 0 || be_id >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: Invalid BE ID %d\n", __func__, be_id);
		ret = -EINVAL;
		goto done;
	}

	*bedai_id = be_id;
	*cfg_data = fe_dai_app_type_cfg[fedai_id][session_type][be_id];
	pr_debug("%s: fedai_id %d, session_type %d, be_id %d, app_type %d, acdb_dev_id %d, sample_rate %d\n",
		__func__, fedai_id, session_type, *bedai_id,
		cfg_data->app_type, cfg_data->acdb_dev_id,
		cfg_data->sample_rate);
done:
	return ret;
}
EXPORT_SYMBOL(msm_pcm_routing_get_stream_app_type_cfg);

static struct cal_block_data *msm_routing_find_topology_by_path(int path,
								int cal_index)
{
	struct list_head		*ptr, *next;
	struct cal_block_data		*cal_block = NULL;
	pr_debug("%s\n", __func__);

	list_for_each_safe(ptr, next,
		&cal_data[cal_index]->cal_blocks) {

		cal_block = list_entry(ptr,
			struct cal_block_data, list);

		if (cal_utils_is_cal_stale(cal_block))
			continue;

		if (((struct audio_cal_info_adm_top *)cal_block
			->cal_info)->path == path) {
			return cal_block;
		}
	}
	pr_debug("%s: Can't find topology for path %d\n", __func__, path);
	return NULL;
}

static struct cal_block_data *msm_routing_find_topology(int path,
							int app_type,
							int acdb_id,
							int cal_index,
							bool exact)
{
	struct list_head *ptr, *next;
	struct cal_block_data *cal_block = NULL;
	struct audio_cal_info_adm_top *cal_info;

	pr_debug("%s\n", __func__);

	list_for_each_safe(ptr, next,
		&cal_data[cal_index]->cal_blocks) {

		cal_block = list_entry(ptr,
			struct cal_block_data, list);

		if (cal_utils_is_cal_stale(cal_block))
			continue;

		cal_info = (struct audio_cal_info_adm_top *)
			cal_block->cal_info;
		if ((cal_info->path == path)  &&
			(cal_info->app_type == app_type) &&
			(cal_info->acdb_id == acdb_id)) {
			return cal_block;
		}
	}
	pr_debug("%s: Can't find topology for path %d, app %d, "
		 "acdb_id %d %s\n",  __func__, path, app_type, acdb_id,
		 exact ? "fail" : "defaulting to search by path");
	return exact ? NULL : msm_routing_find_topology_by_path(path,
								cal_index);
}

static int msm_routing_find_topology_on_index(int session_type, int app_type,
					      int acdb_dev_id,  int idx,
					      bool exact)
{
	int topology = -EINVAL;
	struct cal_block_data *cal_block = NULL;

	mutex_lock(&cal_data[idx]->lock);
	cal_block = msm_routing_find_topology(session_type, app_type,
					      acdb_dev_id, idx, exact);
	if (cal_block != NULL) {
		topology = ((struct audio_cal_info_adm_top *)
			    cal_block->cal_info)->topology;
	}
	mutex_unlock(&cal_data[idx]->lock);
	return topology;
}

/*
 * Retrieving cal_block will mark cal_block as stale.
 * Hence it cannot be reused or resent unless the flag
 * is reset.
 */
static int msm_routing_get_adm_topology(int fedai_id, int session_type,
					int be_id)
{
	int topology = NULL_COPP_TOPOLOGY;
	int app_type = 0, acdb_dev_id = 0;

	pr_debug("%s: fedai_id %d, session_type %d, be_id %d\n",
	       __func__, fedai_id, session_type, be_id);

	if (cal_data == NULL)
		goto done;

	app_type = fe_dai_app_type_cfg[fedai_id][session_type][be_id].app_type;
	acdb_dev_id =
		fe_dai_app_type_cfg[fedai_id][session_type][be_id].acdb_dev_id;

	pr_debug("%s: Check for exact LSM topology\n", __func__);
	topology = msm_routing_find_topology_on_index(session_type,
					       app_type,
					       acdb_dev_id,
					       ADM_LSM_TOPOLOGY_CAL_TYPE_IDX,
					       true /*exact*/);
	if (topology < 0) {
		pr_debug("%s: Check for compatible topology\n", __func__);
		topology = msm_routing_find_topology_on_index(session_type,
						      app_type,
						      acdb_dev_id,
						      ADM_TOPOLOGY_CAL_TYPE_IDX,
						      false /*exact*/);
		if (topology < 0)
			topology = NULL_COPP_TOPOLOGY;
	}
done:
	pr_debug("%s: Using topology %d\n", __func__, topology);
	return topology;
}

static uint8_t is_be_dai_extproc(int be_dai)
{
	if (be_dai == MSM_BACKEND_DAI_EXTPROC_RX ||
	   be_dai == MSM_BACKEND_DAI_EXTPROC_TX ||
	   be_dai == MSM_BACKEND_DAI_EXTPROC_EC_TX)
		return 1;
	else
		return 0;
}

static void msm_pcm_routing_build_matrix(int fedai_id, int sess_type,
					 int path_type, int perf_mode,
					 uint32_t passthr_mode)
{
	int i, port_type, j, num_copps = 0;
	struct route_payload payload;

	lockdep_assert_held(&routing_lock);

	port_type = ((path_type == ADM_PATH_PLAYBACK ||
		      path_type == ADM_PATH_COMPRESSED_RX) ?
		MSM_AFE_PORT_TYPE_RX : MSM_AFE_PORT_TYPE_TX);

	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		if (!is_be_dai_extproc(i) &&
		   (afe_get_port_type(msm_bedais[i].port_id) == port_type) &&
		   (msm_bedais[i].active) &&
		   (test_bit(fedai_id, &msm_bedais[i].fe_sessions[0]))) {
			int port_id = get_port_id(msm_bedais[i].port_id);
			for (j = 0; j < MAX_COPPS_PER_PORT; j++) {
				unsigned long copp =
				      session_copp_map[fedai_id][sess_type][i];
				if (test_bit(j, &copp)) {
					if (num_copps >= MAX_COPPS_PER_PORT) {
						pr_err_ratelimited("%s: too many copps, truncating\n",
								   __func__);
						break;
					}
					payload.port_id[num_copps] = port_id;
					payload.copp_idx[num_copps] = j;
					payload.app_type[num_copps] =
						fe_dai_app_type_cfg
							[fedai_id][sess_type][i]
								.app_type;
					payload.acdb_dev_id[num_copps] =
						fe_dai_app_type_cfg
							[fedai_id][sess_type][i]
								.acdb_dev_id;
					payload.sample_rate[num_copps] =
						fe_dai_app_type_cfg
							[fedai_id][sess_type][i]
								.sample_rate;
					num_copps++;
				}
			}
		}
	}

	if (num_copps) {
		payload.num_copps = num_copps;
		payload.session_id = fe_dai_map[fedai_id][sess_type].strm_id;
		adm_matrix_map(path_type, payload, perf_mode, passthr_mode);
		msm_pcm_routng_cfg_matrix_map_pp(payload, path_type, perf_mode);
	}
}

void msm_pcm_routing_reg_psthr_stream(int fedai_id, int dspst_id,
				      int stream_type)
{
	int i, session_type, path_type, port_type;
	u32 mode = 0;

	if (fedai_id > MSM_FRONTEND_DAI_MM_MAX_ID) {
		/* bad ID assigned in machine driver */
		pr_err("%s: bad MM ID\n", __func__);
		return;
	}

	if (stream_type == SNDRV_PCM_STREAM_PLAYBACK) {
		session_type = SESSION_TYPE_RX;
		path_type = ADM_PATH_PLAYBACK;
		port_type = MSM_AFE_PORT_TYPE_RX;
	} else {
		session_type = SESSION_TYPE_TX;
		path_type = ADM_PATH_LIVE_REC;
		port_type = MSM_AFE_PORT_TYPE_TX;
	}

	mutex_lock(&routing_lock);

	fe_dai_map[fedai_id][session_type].strm_id = dspst_id;
	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		if (!is_be_dai_extproc(i) &&
		    (afe_get_port_type(msm_bedais[i].port_id) == port_type) &&
		    (msm_bedais[i].active) &&
		    (test_bit(fedai_id, &msm_bedais[i].fe_sessions[0]))) {
			mode = afe_get_port_type(msm_bedais[i].port_id);
			adm_connect_afe_port(mode, dspst_id,
					     msm_bedais[i].port_id);
			break;
		}
	}
	mutex_unlock(&routing_lock);
}

static bool route_check_fe_id_adm_support(int fe_id)
{
	bool rc = true;

	if ((fe_id >= MSM_FRONTEND_DAI_LSM1) &&
		 (fe_id <= MSM_FRONTEND_DAI_LSM8)) {
		/* fe id is listen while port is set to afe */
		if (lsm_port_index[fe_id - MSM_FRONTEND_DAI_LSM1] !=
				ADM_LSM_PORT_INDEX) {
			pr_debug("%s: fe_id %d, lsm mux slim port %d\n",
				__func__, fe_id,
				lsm_port_index[fe_id - MSM_FRONTEND_DAI_LSM1]);
			rc = false;
		}
	}

	return rc;
}

/*
 * msm_pcm_routing_get_pp_ch_cnt:
 *	Read the processed channel count
 *
 * @fe_id: Front end ID
 * @session_type: Inidicates RX or TX session type
 */
int msm_pcm_routing_get_pp_ch_cnt(int fe_id, int session_type)
{
	struct msm_pcm_stream_app_type_cfg cfg_data;
	int be_id = 0, app_type_idx = 0, app_type = 0;
	int ret = -EINVAL;

	memset(&cfg_data, 0, sizeof(cfg_data));

	if (!is_mm_lsm_fe_id(fe_id)) {
		pr_err("%s: bad MM ID\n", __func__);
		return -EINVAL;
	}

	ret = msm_pcm_routing_get_stream_app_type_cfg(fe_id, session_type,
						      &be_id, &cfg_data);
	if (ret) {
		pr_err("%s: cannot get stream app type cfg\n", __func__);
		return ret;
	}

	app_type = cfg_data.app_type;
	app_type_idx = msm_pcm_routing_get_lsm_app_type_idx(app_type);
	return lsm_app_type_cfg[app_type_idx].num_out_channels;
}
EXPORT_SYMBOL(msm_pcm_routing_get_pp_ch_cnt);

static inline int msm_pcm_routing_calc_path_type(int session_type,
						 uint32_t passthr_mode)
{
	if (session_type == SESSION_TYPE_RX) {
		if (passthr_mode != LEGACY_PCM)
			return ADM_PATH_COMPRESSED_RX;
		return ADM_PATH_PLAYBACK;
	}
	if ((passthr_mode != LEGACY_PCM) && (passthr_mode != LISTEN))
		return ADM_PATH_COMPRESSED_TX;
	return ADM_PATH_LIVE_REC;
}

/*
 * msm_pcm_routing_open_adm_copp - configure and open an ADM COPP for a FE/BE session
 *
 * Must be called with routing_lock held.
 */
static int msm_pcm_routing_open_adm_copp(int fe_id, int be_id, int session_type,
					 int path_type, int *out_topology,
					 u32 *out_channels)
{
	struct msm_pcm_routing_fdai_data *fdai = &fe_dai_map[fe_id][session_type];
	struct msm_pcm_routing_bdai_data *bedai = &msm_bedais[be_id];
	int port_id = get_port_id(bedai->port_id);
	bool is_lsm = (fe_id >= MSM_FRONTEND_DAI_LSM1) && (fe_id <= MSM_FRONTEND_DAI_LSM8);
	int app_type, app_type_idx, copp_idx, acdb_dev_id, topology;
	u32 channels, sample_rate;
	uint16_t bits_per_sample, be_bit_width;

	lockdep_assert_held(&routing_lock);

	if (session_type == SESSION_TYPE_TX && fdai->be_srate &&
	    (fdai->be_srate != bedai->sample_rate)) {
		pr_debug("%s: flush strm %d diff BE rates\n", __func__, fdai->strm_id);
		if (fdai->event_info.event_func)
			fdai->event_info.event_func(MSM_PCM_RT_EVT_BUF_RECFG,
						    fdai->event_info.priv_data);
		fdai->be_srate = 0;
	}

	bits_per_sample = msm_routing_get_bit_width(bedai->format);

	app_type = fe_dai_app_type_cfg[fe_id][session_type][be_id].app_type;
	if (app_type && is_lsm) {
		app_type_idx = msm_pcm_routing_get_lsm_app_type_idx(app_type);
		sample_rate = fe_dai_app_type_cfg[fe_id][session_type][be_id].sample_rate;
		bits_per_sample = lsm_app_type_cfg[app_type_idx].bit_width;
	} else if (app_type) {
		app_type_idx = msm_pcm_routing_get_app_type_idx(app_type);
		sample_rate = fe_dai_app_type_cfg[fe_id][session_type][be_id].sample_rate;
		bits_per_sample = app_type_cfg[app_type_idx].bit_width;
	} else {
		sample_rate = bedai->sample_rate;
	}

	if (!bedai->adm_override_ch)
		channels = bedai->channel;
	else
		channels = bedai->adm_override_ch;

	acdb_dev_id = fe_dai_app_type_cfg[fe_id][session_type][be_id].acdb_dev_id;
	topology = msm_routing_get_adm_topology(fe_id, session_type, be_id);

	if ((fdai->passthr_mode == COMPRESSED_PASSTHROUGH_DSD) ||
	    (fdai->passthr_mode == COMPRESSED_PASSTHROUGH_GEN) ||
	    (fdai->passthr_mode == COMPRESSED_PASSTHROUGH_IEC61937))
		topology = COMPRESSED_PASSTHROUGH_NONE_TOPOLOGY;

	be_bit_width = msm_routing_get_bit_width(bedai->format);
	if (hifi_filter_enabled &&
	    (bedai->sample_rate == 384000 || bedai->sample_rate == 352800) &&
	    be_bit_width == 32)
		bits_per_sample = msm_routing_get_bit_width(SNDRV_PCM_FORMAT_S32_LE);

	copp_idx = adm_open(port_id, path_type, sample_rate, channels, topology,
			    fdai->perf_mode, bits_per_sample, app_type, acdb_dev_id,
			    session_type, fdai->passthr_mode);
	if (copp_idx < 0 || copp_idx >= MAX_COPPS_PER_PORT) {
		pr_err("%s: adm open failed copp_idx:%d\n", __func__, copp_idx);
		return -EINVAL;
	}

	pr_debug("%s: setting idx bit of fe:%d, type: %d, be:%d\n",
		 __func__, fe_id, session_type, be_id);
	set_bit(copp_idx, &session_copp_map[fe_id][session_type][be_id]);

	if (msm_is_resample_needed(sample_rate, bedai->sample_rate))
		adm_copp_mfc_cfg(port_id, copp_idx, bedai->sample_rate);

	if (out_topology)
		*out_topology = topology;
	if (out_channels)
		*out_channels = channels;

	return copp_idx;
}

/*
 * msm_pcm_routing_close_adm_copp - teardown and close an ADM COPP for a FE/BE session
 *
 * Must be called with routing_lock held.
 */
static void msm_pcm_routing_close_adm_copp(int fe_id, int be_id, int session_type,
					   int copp_idx, int perf_mode,
					   uint32_t passthr_mode)
{
	int port_id = get_port_id(msm_bedais[be_id].port_id);
	int topology;

	lockdep_assert_held(&routing_lock);

	topology = adm_get_topology_for_port_copp_idx(port_id, copp_idx);
	msm_routing_unload_topology(topology);
	adm_close(port_id, perf_mode, copp_idx);

	pr_debug("%s: copp: %d, reset idx bit fe:%d, type: %d, be:%d topology=0x%x\n",
		 __func__, copp_idx, fe_id, session_type, be_id, topology);

	clear_bit(copp_idx, &session_copp_map[fe_id][session_type][be_id]);

	if ((perf_mode == LEGACY_PCM_MODE) && (passthr_mode == LEGACY_PCM))
		msm_pcm_routing_deinit_pp(port_id, topology);
}

int msm_pcm_routing_reg_phy_compr_stream(int fe_id, int perf_mode,
					  int dspst_id, int stream_type,
					  uint32_t passthr_mode)
{
	int i, j, session_type, path_type, port_type, topology;
	int num_copps = 0;
	struct route_payload payload;
	u32 channels;

	pr_debug("%s:fe_id[%d] perf_mode[%d] id[%d] stream_type[%d] passt[%d]",
		 __func__, fe_id, perf_mode, dspst_id,
		 stream_type, passthr_mode);
	if (!is_mm_lsm_fe_id(fe_id)) {
		/* bad ID assigned in machine driver */
		pr_err("%s: bad MM ID %d\n", __func__, fe_id);
		return -EINVAL;
	}

	if (stream_type == SNDRV_PCM_STREAM_PLAYBACK) {
		session_type = SESSION_TYPE_RX;
		port_type = MSM_AFE_PORT_TYPE_RX;
	} else if (stream_type == SNDRV_PCM_STREAM_CAPTURE) {
		session_type = SESSION_TYPE_TX;
		port_type = MSM_AFE_PORT_TYPE_TX;
	} else {
		pr_err("%s: invalid stream type %d\n", __func__, stream_type);
		return -EINVAL;
	}

	path_type = msm_pcm_routing_calc_path_type(session_type, passthr_mode);

	mutex_lock(&routing_lock);

	fe_dai_map[fe_id][session_type].strm_id = dspst_id;
	fe_dai_map[fe_id][session_type].perf_mode = perf_mode;
	fe_dai_map[fe_id][session_type].passthr_mode = passthr_mode;
	if (!route_check_fe_id_adm_support(fe_id)) {
		/* ignore adm open if not supported for fe_id */
		pr_debug("%s: No ADM support for fe id %d\n", __func__, fe_id);
		mutex_unlock(&routing_lock);
		return 0;
	}

	payload.num_copps = 0; /* only RX needs to use payload */
	/* re-enable EQ if active */
	msm_qti_pp_send_eq_values(fe_id);
	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		if (!is_be_dai_extproc(i) &&
		    (afe_get_port_type(msm_bedais[i].port_id) == port_type) &&
		    (msm_bedais[i].active) &&
		    (test_bit(fe_id, &msm_bedais[i].fe_sessions[0]))) {
			int port_id = get_port_id(msm_bedais[i].port_id);
			int copp_idx;

			copp_idx = msm_pcm_routing_open_adm_copp(fe_id, i,
					session_type, path_type, &topology,
					&channels);
			if (copp_idx < 0) {
				mutex_unlock(&routing_lock);
				return -EINVAL;
			}

			for (j = 0; j < MAX_COPPS_PER_PORT; j++) {
				unsigned long copp =
					session_copp_map[fe_id][session_type][i];
				if (test_bit(j, &copp)) {
					if (num_copps >= MAX_COPPS_PER_PORT) {
						pr_err_ratelimited("%s: too many copps, truncating\n",
								   __func__);
						break;
					}
					payload.port_id[num_copps] = port_id;
					payload.copp_idx[num_copps] = j;
					payload.app_type[num_copps] =
						fe_dai_app_type_cfg
							[fe_id][session_type][i]
								.app_type;
					payload.acdb_dev_id[num_copps] =
						fe_dai_app_type_cfg
							[fe_id][session_type][i]
								.acdb_dev_id;
					payload.sample_rate[num_copps] =
						fe_dai_app_type_cfg
							[fe_id][session_type][i]
								.sample_rate;
					num_copps++;
				}
			}
			if (passthr_mode != COMPRESSED_PASSTHROUGH_DSD &&
			    passthr_mode != COMPRESSED_PASSTHROUGH_GEN &&
			    passthr_mode != COMPRESSED_PASSTHROUGH_IEC61937)
				msm_routing_send_device_pp_params(port_id,
								  copp_idx,
								  fe_id);
		}
	}
	if (num_copps) {
		payload.num_copps = num_copps;
		payload.session_id = fe_dai_map[fe_id][session_type].strm_id;
		adm_matrix_map(path_type, payload, perf_mode, passthr_mode);
		msm_pcm_routng_cfg_matrix_map_pp(payload, path_type, perf_mode);
	}
	mutex_unlock(&routing_lock);
	return 0;
}

static u32 msm_pcm_routing_get_voc_sessionid(u16 val)
{
	u32 session_id;

	switch (val) {
	case MSM_FRONTEND_DAI_QCHAT:
		session_id = voc_get_session_id(QCHAT_SESSION_NAME);
		break;
	case MSM_FRONTEND_DAI_VOIP:
		session_id = voc_get_session_id(VOIP_SESSION_NAME);
		break;
	case MSM_FRONTEND_DAI_VOICEMMODE1:
		session_id = voc_get_session_id(VOICEMMODE1_NAME);
		break;
	case MSM_FRONTEND_DAI_VOICEMMODE2:
		session_id = voc_get_session_id(VOICEMMODE2_NAME);
		break;
	default:
		session_id = 0;
	}

	pr_debug("%s session_id 0x%x", __func__, session_id);
	return session_id;
}

static int msm_pcm_routing_channel_mixer_v2(int fe_id, bool perf_mode,
				int dspst_id, int stream_type)
{
	int copp_idx = 0;
	int sess_type = 0;
	int j = 0, be_id = 0;
	int ret = 0;

	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return 0;
	}

	if (stream_type == SNDRV_PCM_STREAM_PLAYBACK)
		sess_type = SESSION_TYPE_RX;
	else
		sess_type = SESSION_TYPE_TX;

	if (!(channel_mixer_v2[fe_id][sess_type].enable)) {
		pr_debug("%s: channel mixer not enabled for FE %d direction %d\n",
			__func__, fe_id, sess_type);
		return 0;
	}

	be_id = channel_mixer_v2[fe_id][sess_type].port_idx - 1;
	channel_mixer_v2[fe_id][sess_type].input_channels[0] =
		channel_mixer_v2[fe_id][sess_type].input_channel;

	pr_debug("%s sess type %d,fe_id %d,override in:%d out:%d,be active %d\n",
			__func__, sess_type, fe_id,
			channel_mixer_v2[fe_id][sess_type].override_in_ch_map,
			channel_mixer_v2[fe_id][sess_type].override_out_ch_map,
			msm_bedais[be_id].active);

	if ((msm_bedais[be_id].active) &&
		test_bit(fe_id, &msm_bedais[be_id].fe_sessions[0])) {
		unsigned long copp =
			session_copp_map[fe_id][sess_type][be_id];
		for (j = 0; j < MAX_COPPS_PER_PORT; j++) {
			if (test_bit(j, &copp)) {
				copp_idx = j;
				break;
			}
		}

		ret = adm_programable_channel_mixer(
			msm_bedais[be_id].port_id,
			copp_idx, dspst_id, sess_type,
			&channel_mixer_v2[fe_id][sess_type], 0);
	}

	return ret;
}

static int msm_pcm_routing_channel_mixer(int fe_id, bool perf_mode,
				int dspst_id, int stream_type)
{
	int copp_idx = 0;
	int sess_type = 0;
	int i = 0, j = 0, be_id;
	int ret = 0;

	ret = msm_pcm_routing_channel_mixer_v2(fe_id, perf_mode,
				dspst_id, stream_type);
	if (ret) {
		pr_err("%s channel mixer v2 cmd  set failure%d\n", __func__,
				fe_id);
		return ret;
	}

	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return 0;
	}

	if (!(channel_mixer[fe_id].enable)) {
		pr_debug("%s: channel mixer not enabled for FE %d\n",
			__func__, fe_id);
		return 0;
	}

	if (stream_type == SNDRV_PCM_STREAM_PLAYBACK)
		sess_type = SESSION_TYPE_RX;
	else
		sess_type = SESSION_TYPE_TX;

	for (i = 0; i < ADM_MAX_CHANNELS && channel_input[fe_id][i] > 0;
		++i) {
		be_id = channel_input[fe_id][i] - 1;
		channel_mixer[fe_id].input_channels[i] =
						msm_bedais[be_id].channel;

		if ((msm_bedais[be_id].active) &&
			test_bit(fe_id,
			&msm_bedais[be_id].fe_sessions[0])) {
			unsigned long copp =
				session_copp_map[fe_id][sess_type][be_id];
			for (j = 0; j < MAX_COPPS_PER_PORT; j++) {
				if (test_bit(j, &copp)) {
					copp_idx = j;
					break;
				}
			}

			pr_debug("%s: fe %d, be %d, channel %d, copp %d\n",
				__func__,
				fe_id, be_id, msm_bedais[be_id].channel,
				copp_idx);
			ret = adm_programable_channel_mixer(
					get_port_id(msm_bedais[be_id].port_id),
					copp_idx, dspst_id, sess_type,
					channel_mixer + fe_id, i);
		}
	}

	return ret;
}

/**
 * msm_pcm_routing_set_channel_mixer_runtime - apply channel mixer
 * setting during runtime.
 *
 * @be_id: backend index
 * @session_id: session index
 * @session_type: session type
 * @params: parameters for channel mixer
 *
 * Retuen: 0 for success, else error
 */
int msm_pcm_routing_set_channel_mixer_runtime(int be_id, int session_id,
			int session_type,
			struct msm_pcm_channel_mixer *params)
{
	int rc = 0;
	int port_id, copp_idx = 0;

	be_id--;
	if (be_id < 0 || be_id >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: invalid backend id %d\n", __func__,
				be_id);
		return -EINVAL;
	}

	port_id = msm_bedais[be_id].port_id;
	copp_idx = adm_get_default_copp_idx(port_id);
	pr_debug("%s: port_id - %d, copp_idx %d session id - %d\n",
		 __func__, port_id, copp_idx, session_id);

	if ((params->input_channel < 0) ||
		(params->input_channel > ADM_MAX_CHANNELS)) {
		pr_err("%s: invalid input channel %d\n", __func__,
				params->input_channel);
		return -EINVAL;
	}

	if ((params->output_channel < 0) ||
		(params->output_channel > ADM_MAX_CHANNELS)) {
		pr_err("%s: invalid output channel %d\n", __func__,
				params->output_channel);
		return -EINVAL;
	}

	params->input_channels[0] = params->input_channel;

	pr_debug("%s sess type %d,override in:%d out:%d,be active %d\n",
			__func__, session_type,
			params->override_in_ch_map,
			params->override_out_ch_map,
			msm_bedais[be_id].active);

	rc = adm_programable_channel_mixer(port_id,
					copp_idx,
					session_id,
					session_type,
					params,
					0);
	if (rc) {
		pr_err("%s: send params failed rc=%d\n", __func__, rc);
		rc = -EINVAL;
	}
	return rc;
}
EXPORT_SYMBOL(msm_pcm_routing_set_channel_mixer_runtime);

int msm_pcm_routing_reg_phy_stream(int fedai_id, int perf_mode,
				   int dspst_id, int stream_type)
{
	int i, j, session_type, path_type, port_type, topology;
	int num_copps = 0;
	struct route_payload payload;
	u32 channels;
	uint32_t passthr_mode = LEGACY_PCM;
	int ret = 0;

	if (fedai_id > MSM_FRONTEND_DAI_MM_MAX_ID) {
		/* bad ID assigned in machine driver */
		pr_err("%s: bad MM ID %d\n", __func__, fedai_id);
		return -EINVAL;
	}

	if (stream_type == SNDRV_PCM_STREAM_PLAYBACK) {
		session_type = SESSION_TYPE_RX;
		path_type = ADM_PATH_PLAYBACK;
		port_type = MSM_AFE_PORT_TYPE_RX;
	} else {
		session_type = SESSION_TYPE_TX;
		path_type = ADM_PATH_LIVE_REC;
		port_type = MSM_AFE_PORT_TYPE_TX;
	}

	mutex_lock(&routing_lock);

	payload.num_copps = 0; /* only RX needs to use payload */
	fe_dai_map[fedai_id][session_type].strm_id = dspst_id;
	fe_dai_map[fedai_id][session_type].perf_mode = perf_mode;
	fe_dai_map[fedai_id][session_type].passthr_mode = LEGACY_PCM;

	/* re-enable EQ if active */
	msm_qti_pp_send_eq_values(fedai_id);
	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		if (!is_be_dai_extproc(i) &&
		    (afe_get_port_type(msm_bedais[i].port_id) == port_type) &&
		    (msm_bedais[i].active) &&
		    (test_bit(fedai_id, &msm_bedais[i].fe_sessions[0]))) {
			int port_id = get_port_id(msm_bedais[i].port_id);
			int copp_idx;

			copp_idx = msm_pcm_routing_open_adm_copp(fedai_id, i,
					session_type, path_type, &topology,
					&channels);
			if (copp_idx < 0) {
				mutex_unlock(&routing_lock);
				return -EINVAL;
			}

			for (j = 0; j < MAX_COPPS_PER_PORT; j++) {
				unsigned long copp =
				    session_copp_map[fedai_id][session_type][i];
				if (test_bit(j, &copp)) {
					if (num_copps >= MAX_COPPS_PER_PORT) {
						pr_err_ratelimited("%s: too many copps, truncating\n",
								   __func__);
						break;
					}
					payload.port_id[num_copps] = port_id;
					payload.copp_idx[num_copps] = j;
					payload.app_type[num_copps] =
						fe_dai_app_type_cfg
							[fedai_id][session_type]
							[i].app_type;
					payload.acdb_dev_id[num_copps] =
						fe_dai_app_type_cfg
							[fedai_id][session_type]
							[i].acdb_dev_id;
					payload.sample_rate[num_copps] =
						fe_dai_app_type_cfg
							[fedai_id][session_type]
							[i].sample_rate;
					num_copps++;
				}
			}
			if (perf_mode == LEGACY_PCM_MODE)
				msm_pcm_routing_cfg_pp(port_id, copp_idx,
						       topology, channels);
		}
	}
	if (num_copps) {
		payload.num_copps = num_copps;
		payload.session_id =
			fe_dai_map[fedai_id][session_type].strm_id;
		adm_matrix_map(path_type, payload, perf_mode,
			       passthr_mode);
		msm_pcm_routng_cfg_matrix_map_pp(payload, path_type,
						 perf_mode);
	}

	ret = msm_pcm_routing_channel_mixer(fedai_id, perf_mode,
					    dspst_id, stream_type);
	mutex_unlock(&routing_lock);
	return ret;
}

int msm_pcm_routing_reg_phy_stream_v2(int fedai_id, int perf_mode,
				      int dspst_id, int stream_type,
				      struct msm_pcm_routing_evt event_info)
{
	if (msm_pcm_routing_reg_phy_stream(fedai_id, perf_mode, dspst_id,
				       stream_type)) {
		pr_err("%s: failed to reg phy stream\n", __func__);
		return -EINVAL;
	}

	if (stream_type == SNDRV_PCM_STREAM_PLAYBACK)
		fe_dai_map[fedai_id][SESSION_TYPE_RX].event_info = event_info;
	else
		fe_dai_map[fedai_id][SESSION_TYPE_TX].event_info = event_info;
	return 0;
}

void msm_pcm_routing_dereg_phy_stream(int fedai_id, int stream_type)
{
	int i, port_type, session_type;
	struct msm_pcm_routing_fdai_data *fdai;

	if (!is_mm_lsm_fe_id(fedai_id)) {
		pr_err("%s: bad MM ID\n", __func__);
		return;
	}

	if (stream_type == SNDRV_PCM_STREAM_PLAYBACK) {
		port_type = MSM_AFE_PORT_TYPE_RX;
		session_type = SESSION_TYPE_RX;
	} else {
		port_type = MSM_AFE_PORT_TYPE_TX;
		session_type = SESSION_TYPE_TX;
	}

	mutex_lock(&routing_lock);
	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		if (!is_be_dai_extproc(i) &&
		    (afe_get_port_type(msm_bedais[i].port_id) == port_type) &&
		    (msm_bedais[i].active) &&
		    (test_bit(fedai_id, &msm_bedais[i].fe_sessions[0]))) {
			int idx;
			unsigned long copp =
				session_copp_map[fedai_id][session_type][i];
			fdai = &fe_dai_map[fedai_id][session_type];

			for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++)
				if (test_bit(idx, &copp))
					break;

			if (idx >= MAX_COPPS_PER_PORT || idx < 0) {
				pr_debug("%s: copp idx is invalid, exiting\n",
					 __func__);
				continue;
			}
			msm_pcm_routing_close_adm_copp(fedai_id, i, session_type,
						       idx, fdai->perf_mode,
						       fdai->passthr_mode);
		}
	}

	fe_dai_map[fedai_id][session_type].strm_id = INVALID_SESSION;
	fe_dai_map[fedai_id][session_type].be_srate = 0;
	mutex_unlock(&routing_lock);
}

/* Check if FE/BE route is set */
static bool msm_pcm_routing_route_is_set(u16 be_id, u16 fe_id)
{
	bool rc = false;

	if (!is_mm_lsm_fe_id(fe_id)) {
		/* recheck FE ID in the mixer control defined in this file */
		pr_err("%s: bad MM ID\n", __func__);
		return rc;
	}

	if (test_bit(fe_id, &msm_bedais[be_id].fe_sessions[0]))
		rc = true;

	return rc;
}

static void msm_pcm_routing_process_audio(u16 reg, u16 val, int set)
{
	int session_type, path_type, topology;
	u32 channels;
	int copp_idx;
	struct msm_pcm_routing_fdai_data *fdai;
	uint32_t passthr_mode;

	pr_debug("%s: reg %x val %x set %x\n", __func__, reg, val, set);

	if (val == MSM_FRONTEND_DAI_DTMF_RX &&
	    afe_get_port_type(msm_bedais[reg].port_id) == MSM_AFE_PORT_TYPE_RX) {
		pr_debug("%s(): set=%d port id=0x%x for dtmf generation\n",
			 __func__, set, msm_bedais[reg].port_id);
		afe_set_dtmf_gen_rx_portid(msm_bedais[reg].port_id, set);
	} else if (!is_mm_lsm_fe_id(val)) {
		pr_err("%s: bad MM ID\n", __func__);
		return;
	}

	if (!route_check_fe_id_adm_support(val)) {
		pr_debug("%s: No ADM support for fe id %d\n", __func__, val);
		if (set)
			set_bit(val, &msm_bedais[reg].fe_sessions[0]);
		else
			clear_bit(val, &msm_bedais[reg].fe_sessions[0]);
		return;
	}

	session_type =
		(afe_get_port_type(msm_bedais[reg].port_id) == MSM_AFE_PORT_TYPE_RX) ?
		SESSION_TYPE_RX : SESSION_TYPE_TX;
	fdai = &fe_dai_map[val][session_type];
	passthr_mode = fdai->passthr_mode;
	path_type = msm_pcm_routing_calc_path_type(session_type, passthr_mode);

	mutex_lock(&routing_lock);
	if (set) {
		if (!test_bit(val, &msm_bedais[reg].fe_sessions[0]) &&
		    ((msm_bedais[reg].port_id == VOICE_PLAYBACK_TX) ||
		     (msm_bedais[reg].port_id == VOICE2_PLAYBACK_TX)))
			voc_start_playback(set, msm_bedais[reg].port_id);

		set_bit(val, &msm_bedais[reg].fe_sessions[0]);
		if (msm_bedais[reg].active && fdai->strm_id != INVALID_SESSION) {
			int port_id = get_port_id(msm_bedais[reg].port_id);

			copp_idx = msm_pcm_routing_open_adm_copp(val, reg,
					session_type, path_type, &topology,
					&channels);
			if (copp_idx < 0) {
				mutex_unlock(&routing_lock);
				return;
			}

			if (session_type == SESSION_TYPE_RX &&
			    fdai->event_info.event_func)
				fdai->event_info.event_func(
					MSM_PCM_RT_EVT_DEVSWITCH,
					fdai->event_info.priv_data);

			msm_pcm_routing_build_matrix(val, session_type,
						     path_type,
						     fdai->perf_mode,
						     passthr_mode);
			if ((fdai->perf_mode == LEGACY_PCM_MODE) &&
			    (passthr_mode == LEGACY_PCM))
				msm_pcm_routing_cfg_pp(port_id, copp_idx,
						       topology, channels);
		}
	} else {
		if (test_bit(val, &msm_bedais[reg].fe_sessions[0]) &&
		    ((msm_bedais[reg].port_id == VOICE_PLAYBACK_TX) ||
		     (msm_bedais[reg].port_id == VOICE2_PLAYBACK_TX)))
			voc_start_playback(set, msm_bedais[reg].port_id);
		clear_bit(val, &msm_bedais[reg].fe_sessions[0]);
		if (msm_bedais[reg].active && fdai->strm_id != INVALID_SESSION) {
			int idx;
			unsigned long copp =
				session_copp_map[val][session_type][reg];

			for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++)
				if (test_bit(idx, &copp))
					break;

			if (idx >= MAX_COPPS_PER_PORT) {
				pr_debug("%s: copp idx is invalid, exiting\n",
					 __func__);
				mutex_unlock(&routing_lock);
				return;
			}

			msm_pcm_routing_close_adm_copp(val, reg, session_type,
						       idx, fdai->perf_mode,
						       passthr_mode);
			msm_pcm_routing_build_matrix(val, session_type,
						     path_type,
						     fdai->perf_mode,
						     passthr_mode);
		}
	}
	if ((msm_bedais[reg].port_id == VOICE_RECORD_RX) ||
	    (msm_bedais[reg].port_id == VOICE_RECORD_TX))
		voc_start_record(msm_bedais[reg].port_id, set, voc_session_id);

	mutex_unlock(&routing_lock);
}

static inline int msm_routing_audio_mixer_get_helper(
	struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;

	ucontrol->value.integer.value[0] =
		test_bit(mc->rshift, &msm_bedais[mc->shift].fe_sessions[0]) ? 1 : 0;

	pr_debug("%s: shift %x rshift %x val %ld\n", __func__, mc->shift, mc->rshift,
		 ucontrol->value.integer.value[0]);

	return 0;
}

static inline int msm_routing_audio_mixer_put_helper(
	struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget =
		snd_soc_dapm_kcontrol_widget(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int set = !!ucontrol->value.integer.value[0];
	bool route_set = msm_pcm_routing_route_is_set(mc->shift, mc->rshift);

	pr_debug("%s: shift %x rshift %x val %ld\n", __func__, mc->shift, mc->rshift,
		 ucontrol->value.integer.value[0]);

	if (set && !route_set) {
		msm_pcm_routing_process_audio(mc->shift, mc->rshift, 1);
		snd_soc_dapm_mixer_update_power(widget->dapm, kcontrol, 1, NULL);
	} else if (!set && route_set) {
		msm_pcm_routing_process_audio(mc->shift, mc->rshift, 0);
		snd_soc_dapm_mixer_update_power(widget->dapm, kcontrol, 0, NULL);
	}

	return 1;
}

static int msm_routing_get_audio_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_audio_mixer_get_helper(kcontrol, ucontrol);
}

static int msm_routing_put_audio_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_audio_mixer_put_helper(kcontrol, ucontrol);
}

static int msm_routing_get_listen_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_audio_mixer_get_helper(kcontrol, ucontrol);
}

static int msm_routing_put_listen_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_audio_mixer_put_helper(kcontrol, ucontrol);
}

static void msm_pcm_routing_process_voice(u16 reg, u16 val, int set)
{
	u32 session_id = 0;
	u16 path_type;
	struct media_format_info voc_be_media_format;

	pr_debug("%s: reg %x val %x set %x\n", __func__, reg, val, set);

	session_id = msm_pcm_routing_get_voc_sessionid(val);

	if (!session_id) {
		pr_err("%s: Invalid session_id %x\n", __func__, session_id);
		return;
	}

	pr_debug("%s: FE DAI 0x%x session_id 0x%x\n",
		__func__, val, session_id);

	mutex_lock(&routing_lock);

	if (set)
		set_bit(val, &msm_bedais[reg].fe_sessions[0]);
	else
		clear_bit(val, &msm_bedais[reg].fe_sessions[0]);

	if (val == MSM_FRONTEND_DAI_DTMF_RX &&
	    afe_get_port_type(msm_bedais[reg].port_id) ==
						MSM_AFE_PORT_TYPE_RX) {
		pr_debug("%s(): set=%d port id=0x%x for dtmf generation\n",
			 __func__, set, msm_bedais[reg].port_id);
		afe_set_dtmf_gen_rx_portid(msm_bedais[reg].port_id, set);
	}

	if (afe_get_port_type(msm_bedais[reg].port_id) ==
						MSM_AFE_PORT_TYPE_RX)
		path_type = RX_PATH;
	else
		path_type = TX_PATH;

	if (set) {
		if (msm_bedais[reg].active) {
			voc_set_route_flag(session_id, path_type, 1);

			memset(&voc_be_media_format, 0,
			       sizeof(struct media_format_info));

			voc_be_media_format.port_id = msm_bedais[reg].port_id;
			voc_be_media_format.num_channels =
						msm_bedais[reg].channel;
			voc_be_media_format.sample_rate =
						msm_bedais[reg].sample_rate;
			voc_be_media_format.bits_per_sample =
						msm_bedais[reg].format;
			/* Defaulting this to 1 for voice call usecases */
			voc_be_media_format.channel_mapping[0] = 1;

			voc_set_device_config(session_id, path_type,
					      &voc_be_media_format);

			if (voc_get_route_flag(session_id, TX_PATH) &&
				voc_get_route_flag(session_id, RX_PATH))
				voc_enable_device(session_id);
		} else {
			pr_debug("%s BE is not active\n", __func__);
		}
	} else {
		voc_set_route_flag(session_id, path_type, 0);
		voc_disable_device(session_id);
	}

	mutex_unlock(&routing_lock);

}

static inline int msm_routing_voice_mixer_get_helper(
	struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;

	mutex_lock(&routing_lock);
	ucontrol->value.integer.value[0] =
		test_bit(mc->rshift, &msm_bedais[mc->shift].fe_sessions[0]) ? 1 : 0;
	mutex_unlock(&routing_lock);

	pr_debug("%s: shift %x rshift %x val %ld\n", __func__, mc->shift, mc->rshift,
		 ucontrol->value.integer.value[0]);

	return 0;
}

static int msm_routing_get_voice_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_voice_mixer_get_helper(kcontrol, ucontrol);
}

static int msm_routing_put_voice_mixer(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget =
		snd_soc_dapm_kcontrol_widget(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int set = !!ucontrol->value.integer.value[0];

	msm_pcm_routing_process_voice(mc->shift, mc->rshift, set);
	snd_soc_dapm_mixer_update_power(widget->dapm, kcontrol, set, NULL);
	return 1;
}

static int msm_routing_get_voice_stub_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_voice_mixer_get_helper(kcontrol, ucontrol);
}

static int msm_routing_put_voice_stub_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget =
		snd_soc_dapm_kcontrol_widget(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int set = !!ucontrol->value.integer.value[0];

	mutex_lock(&routing_lock);
	if (set)
		set_bit(mc->rshift, &msm_bedais[mc->shift].fe_sessions[0]);
	else
		clear_bit(mc->rshift, &msm_bedais[mc->shift].fe_sessions[0]);
	mutex_unlock(&routing_lock);

	snd_soc_dapm_mixer_update_power(widget->dapm, kcontrol, set, NULL);
	pr_debug("%s: shift %x rshift %x val %ld\n", __func__, mc->shift, mc->rshift,
		 ucontrol->value.integer.value[0]);
	return 1;
}

/*
 * Return the mapping between port ID and backend ID to enable the AFE callback
 * to determine the acdb_dev_id from the port id
 */
int msm_pcm_get_be_id_from_port_id(int port_id)
{
	int i;
	int be_id = -EINVAL;

	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		if (msm_bedais[i].port_id == port_id) {
			be_id = i;
			break;
		}
	}

	return be_id;
}

/*
 * Return the registered dev_acdb_id given a port ID to enable identifying the
 * correct AFE calibration information by comparing the header information.
 */
static int msm_pcm_get_dev_acdb_id_by_port_id(int port_id)
{
	int acdb_id = -EINVAL;
	int i = 0;
	int session;
	int port_type = afe_get_port_type(port_id);
	int be_id = msm_pcm_get_be_id_from_port_id(port_id);

	pr_debug("%s:port_id %d be_id %d, port_type 0x%x\n",
		  __func__, port_id, be_id, port_type);

	if (port_type == MSM_AFE_PORT_TYPE_TX) {
		session = SESSION_TYPE_TX;
	} else if (port_type == MSM_AFE_PORT_TYPE_RX) {
		session = SESSION_TYPE_RX;
	} else {
		pr_err("%s: Invalid port type %d\n", __func__, port_type);
		acdb_id = -EINVAL;
		goto exit;
	}

	if (be_id < 0) {
		pr_err("%s: Error getting backend id %d\n", __func__, be_id);
		goto exit;
	}

	mutex_lock(&routing_lock);
	i = find_first_bit(&msm_bedais[be_id].fe_sessions[0],
			   MSM_FRONTEND_DAI_MAX);
	if (i < MSM_FRONTEND_DAI_MAX)
		acdb_id = fe_dai_app_type_cfg[i][session][be_id].acdb_dev_id;

	pr_debug("%s: FE[%d] session[%d] BE[%d] acdb_id(%d)\n",
		 __func__, i, session, be_id, acdb_id);
	mutex_unlock(&routing_lock);
exit:
	return acdb_id;
}

static inline int msm_routing_switch_mixer_get_helper(
	struct snd_ctl_elem_value *ucontrol, int enable_val, const char *name)
{
	ucontrol->value.integer.value[0] = enable_val;
	pr_debug("%s: %s enable %ld\n", __func__, name,
		 ucontrol->value.integer.value[0]);
	return 0;
}

static inline int msm_routing_switch_mixer_put_helper(
	struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol,
	int *enable_var, const char *name)
{
	struct snd_soc_dapm_widget *widget =
		snd_soc_dapm_kcontrol_widget(kcontrol);

	pr_debug("%s: %s enable %ld\n", __func__, name,
		 ucontrol->value.integer.value[0]);
	*enable_var = ucontrol->value.integer.value[0];
	snd_soc_dapm_mixer_update_power(widget->dapm, kcontrol,
					*enable_var ? 1 : 0, NULL);
	return 1;
}

static int msm_routing_get_switch_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol, fm_switch_enable,
						   "FM Switch");
}

static int msm_routing_put_switch_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &fm_switch_enable,
						   "FM Switch");
}

static int msm_routing_get_hfp_switch_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol, hfp_switch_enable,
						   "HFP Switch");
}

static int msm_routing_put_hfp_switch_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &hfp_switch_enable,
						   "HFP Switch");
}

static int msm_routing_a2dp_switch_mixer_get(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol, a2dp_switch_enable,
						   "A2DP Switch");
}

static int msm_routing_a2dp_switch_mixer_put(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &a2dp_switch_enable,
						   "A2DP Switch");
}

static int msm_routing_sco_switch_mixer_get(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol, sco_switch_enable,
						   "SCO Switch");
}

static int msm_routing_sco_switch_mixer_put(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &sco_switch_enable,
						   "SCO Switch");
}

#ifndef CONFIG_MI2S_DISABLE
static int msm_routing_get_int0_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   int0_mi2s_switch_enable,
						   "INT0 MI2S Switch");
}

static int msm_routing_put_int0_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &int0_mi2s_switch_enable,
						   "INT0 MI2S Switch");
}

static int msm_routing_get_int4_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   int4_mi2s_switch_enable,
						   "INT4 MI2S Switch");
}

static int msm_routing_put_int4_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &int4_mi2s_switch_enable,
						   "INT4 MI2S Switch");
}

static int msm_routing_get_pri_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   pri_mi2s_switch_enable,
						   "PRI MI2S Switch");
}

static int msm_routing_put_pri_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &pri_mi2s_switch_enable,
						   "PRI MI2S Switch");
}

static int msm_routing_get_sec_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   sec_mi2s_switch_enable,
						   "SEC MI2S Switch");
}

static int msm_routing_put_sec_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &sec_mi2s_switch_enable,
						   "SEC MI2S Switch");
}

static int msm_routing_get_tert_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   tert_mi2s_switch_enable,
						   "TERT MI2S Switch");
}

static int msm_routing_put_tert_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &tert_mi2s_switch_enable,
						   "TERT MI2S Switch");
}

static int msm_routing_get_quat_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   quat_mi2s_switch_enable,
						   "QUAT MI2S Switch");
}

static int msm_routing_put_quat_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &quat_mi2s_switch_enable,
						   "QUAT MI2S Switch");
}

static int msm_routing_get_quin_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   quin_mi2s_switch_enable,
						   "QUIN MI2S Switch");
}

static int msm_routing_put_quin_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						  struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &quin_mi2s_switch_enable,
						   "QUIN MI2S Switch");
}

static int msm_routing_get_sen_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   sen_mi2s_switch_enable,
						   "SEN MI2S Switch");
}

static int msm_routing_put_sen_mi2s_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &sen_mi2s_switch_enable,
						   "SEN MI2S Switch");
}
#endif

static int msm_routing_get_usb_switch_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol, usb_switch_enable,
						   "USB Switch");
}

static int msm_routing_put_usb_switch_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &usb_switch_enable,
						   "USB Switch");
}

static int msm_routing_get_fm_pcmrx_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_get_helper(ucontrol,
						   fm_pcmrx_switch_enable,
						   "FM Switch");
}

static int msm_routing_put_fm_pcmrx_switch_mixer(struct snd_kcontrol *kcontrol,
						 struct snd_ctl_elem_value *ucontrol)
{
	return msm_routing_switch_mixer_put_helper(kcontrol, ucontrol,
						   &fm_pcmrx_switch_enable,
						   "FM Switch");
}

static void msm_routing_get_lsm_fe_idx(struct snd_kcontrol *kcontrol,
						u8 *fe_idx)
{
	int fe_id = MSM_FRONTEND_DAI_LSM1;

	if (strnstr(kcontrol->id.name, "LSM1", sizeof("LSM1"))) {
		fe_id = MSM_FRONTEND_DAI_LSM1;
	} else if (strnstr(kcontrol->id.name, "LSM2", sizeof("LSM2"))) {
		fe_id = MSM_FRONTEND_DAI_LSM2;
	} else if (strnstr(kcontrol->id.name, "LSM3", sizeof("LSM3"))) {
		fe_id = MSM_FRONTEND_DAI_LSM3;
	} else if (strnstr(kcontrol->id.name, "LSM4", sizeof("LSM4"))) {
		fe_id = MSM_FRONTEND_DAI_LSM4;
	} else if (strnstr(kcontrol->id.name, "LSM5", sizeof("LSM5"))) {
		fe_id = MSM_FRONTEND_DAI_LSM5;
	} else if (strnstr(kcontrol->id.name, "LSM6", sizeof("LSM6"))) {
		fe_id = MSM_FRONTEND_DAI_LSM6;
	} else if (strnstr(kcontrol->id.name, "LSM7", sizeof("LSM7"))) {
		fe_id = MSM_FRONTEND_DAI_LSM7;
	} else if (strnstr(kcontrol->id.name, "LSM8", sizeof("LSM8"))) {
		fe_id = MSM_FRONTEND_DAI_LSM8;
	} else {
		pr_err("%s: Invalid kcontrol name:%s\n", __func__,
				kcontrol->id.name);
		return;
	}

	*fe_idx = fe_id - MSM_FRONTEND_DAI_LSM1;
	pr_debug("%s: fe_id: %d, fe_idx:%d\n", __func__, fe_id, *fe_idx);
}

static int msm_routing_lsm_port_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	u8 fe_idx = 0;

	msm_routing_get_lsm_fe_idx(kcontrol, &fe_idx);
	ucontrol->value.integer.value[0] = lsm_port_index[fe_idx];
	return 0;
}

static int msm_routing_lsm_port_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int mux = ucontrol->value.enumerated.item[0];
	int lsm_port = AFE_PORT_ID_SLIMBUS_MULTI_CHAN_5_TX;
	int lsm_port_idx = 0;
	u8 fe_idx = 0;

	if (mux >= e->items) {
		pr_err("%s: Invalid mux value %d\n", __func__, mux);
		return -EINVAL;
	}

	pr_debug("%s: LSM enable %ld\n", __func__,
			ucontrol->value.integer.value[0]);
	lsm_port_idx = ucontrol->value.integer.value[0];
	switch (ucontrol->value.integer.value[0]) {
	case 1:
		lsm_port = AFE_PORT_ID_SLIMBUS_MULTI_CHAN_0_TX;
		break;
	case 2:
		lsm_port = AFE_PORT_ID_SLIMBUS_MULTI_CHAN_1_TX;
		break;
	case 3:
		lsm_port = AFE_PORT_ID_SLIMBUS_MULTI_CHAN_2_TX;
		break;
	case 4:
		lsm_port = AFE_PORT_ID_SLIMBUS_MULTI_CHAN_3_TX;
		break;
	case 5:
		lsm_port = AFE_PORT_ID_SLIMBUS_MULTI_CHAN_4_TX;
		break;
	case 6:
		lsm_port = AFE_PORT_ID_SLIMBUS_MULTI_CHAN_5_TX;
		break;
	case 7:
		lsm_port = AFE_PORT_ID_TERTIARY_MI2S_TX;
		break;
	case 8:
		lsm_port = AFE_PORT_ID_QUATERNARY_MI2S_TX;
		break;
	case 9:
		lsm_port = ADM_LSM_PORT_ID;
		break;
	case 10:
		lsm_port = AFE_PORT_ID_INT3_MI2S_TX;
		break;
	case 11:
		lsm_port = AFE_PORT_ID_VA_CODEC_DMA_TX_0;
		break;
	case 12:
		lsm_port = AFE_PORT_ID_VA_CODEC_DMA_TX_1;
		break;
	case 13:
		lsm_port = AFE_PORT_ID_VA_CODEC_DMA_TX_2;
		break;
	case 14:
		lsm_port = AFE_PORT_ID_TX_CODEC_DMA_TX_3;
		break;
	case 15:
		lsm_port = AFE_PORT_ID_QUINARY_TDM_TX;
		break;
	case 16:
		lsm_port = AFE_PORT_ID_TERTIARY_TDM_TX;
		break;
	default:
		pr_err("Default lsm port");
		break;
	}
	set_lsm_port(lsm_port);
	msm_routing_get_lsm_fe_idx(kcontrol, &fe_idx);
	lsm_port_index[fe_idx] = ucontrol->value.integer.value[0];
	if (lsm_port_idx < 1 || lsm_port_idx > 16)
		lsm_port = 0xffff;
	afe_set_lsm_afe_port_id(fe_idx, lsm_port);

	return 0;
}

static int msm_routing_lsm_func_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	int i;
	u16 port_id;
	enum afe_mad_type mad_type;

	pr_debug("%s: id name %s\n", __func__, kcontrol->id.name);
	for (i = 0; i < ARRAY_SIZE(lsm_port_text); i++)
		if (!strnstr(kcontrol->id.name, lsm_port_text[i],
			    strlen(lsm_port_text[i])))
			break;

	if (i-- == ARRAY_SIZE(lsm_port_text)) {
		pr_warn("%s: Invalid id name %s\n", __func__,
			kcontrol->id.name);
		return -EINVAL;
	}

	port_id = i * 2 + 1 + SLIMBUS_0_RX;

	/*Check for Tertiary/Quaternary/INT3 TX port*/
	if (strnstr(kcontrol->id.name, lsm_port_text[7],
			strlen(lsm_port_text[7])))
		port_id = AFE_PORT_ID_TERTIARY_MI2S_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[8],
			strlen(lsm_port_text[8])))
		port_id = AFE_PORT_ID_QUATERNARY_MI2S_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[10],
			strlen(lsm_port_text[10])))
		port_id = AFE_PORT_ID_INT3_MI2S_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[14],
			strlen(lsm_port_text[14])))
		port_id = AFE_PORT_ID_TX_CODEC_DMA_TX_3;

	if (strnstr(kcontrol->id.name, lsm_port_text[15],
			strlen(lsm_port_text[15])))
		port_id = AFE_PORT_ID_QUINARY_TDM_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[16],
			strlen(lsm_port_text[16])))
		port_id = AFE_PORT_ID_TERTIARY_TDM_TX;

	mad_type = afe_port_get_mad_type(port_id);
	pr_debug("%s: port_id 0x%x, mad_type %d\n", __func__, port_id,
		 mad_type);
	switch (mad_type) {
	case MAD_HW_NONE:
		ucontrol->value.integer.value[0] = MADNONE;
		break;
	case MAD_HW_AUDIO:
		ucontrol->value.integer.value[0] = MADAUDIO;
		break;
	case MAD_HW_BEACON:
		ucontrol->value.integer.value[0] = MADBEACON;
		break;
	case MAD_HW_ULTRASOUND:
		ucontrol->value.integer.value[0] = MADULTRASOUND;
		break;
	case MAD_SW_AUDIO:
		ucontrol->value.integer.value[0] = MADSWAUDIO;
	break;
	default:
		pr_warn("%s: Unknown\n", __func__);
		return -EINVAL;
	}
	return 0;
}

static int msm_routing_lsm_func_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	int i;
	u16 port_id;
	enum afe_mad_type mad_type;

	pr_debug("%s: id name %s\n", __func__, kcontrol->id.name);
	for (i = 0; i < ARRAY_SIZE(lsm_port_text); i++)
		if (strnstr(kcontrol->id.name, lsm_port_text[i],
			    strlen(lsm_port_text[i])))
			break;

	if (i-- == ARRAY_SIZE(lsm_port_text)) {
		pr_warn("%s: Invalid id name %s\n", __func__,
			kcontrol->id.name);
		return -EINVAL;
	}

	port_id = i * 2 + 1 + SLIMBUS_0_RX;
	switch (ucontrol->value.integer.value[0]) {
	case MADNONE:
		mad_type = MAD_HW_NONE;
		break;
	case MADAUDIO:
		mad_type = MAD_HW_AUDIO;
		break;
	case MADBEACON:
		mad_type = MAD_HW_BEACON;
		break;
	case MADULTRASOUND:
		mad_type = MAD_HW_ULTRASOUND;
		break;
	case MADSWAUDIO:
		mad_type = MAD_SW_AUDIO;
		break;
	default:
		pr_warn("%s: Unknown\n", __func__);
		return -EINVAL;
	}

	/*Check for Tertiary/Quaternary/INT3 TX port*/
	if (strnstr(kcontrol->id.name, lsm_port_text[7],
			strlen(lsm_port_text[7])))
		port_id = AFE_PORT_ID_TERTIARY_MI2S_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[8],
			strlen(lsm_port_text[8])))
		port_id = AFE_PORT_ID_QUATERNARY_MI2S_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[10],
			strlen(lsm_port_text[10])))
		port_id = AFE_PORT_ID_INT3_MI2S_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[14],
			strlen(lsm_port_text[14])))
		port_id = AFE_PORT_ID_TX_CODEC_DMA_TX_3;

	if (strnstr(kcontrol->id.name, lsm_port_text[15],
			strlen(lsm_port_text[15])))
		port_id = AFE_PORT_ID_QUINARY_TDM_TX;

	if (strnstr(kcontrol->id.name, lsm_port_text[16],
			strlen(lsm_port_text[16])))
		port_id = AFE_PORT_ID_TERTIARY_TDM_TX;

	pr_debug("%s: port_id 0x%x, mad_type %d\n", __func__, port_id,
		 mad_type);
	return afe_port_set_mad_type(port_id, mad_type);
}

static const char *const adm_override_chs_text[] = {"Zero", "One", "Two"};

static SOC_ENUM_SINGLE_EXT_DECL(adm_override_chs,
				adm_override_chs_text);

static SOC_ENUM_SINGLE_EXT_DECL(mi2s_rx_adm_override_chs,
				adm_override_chs_text);

static int msm_routing_adm_get_backend_idx(struct snd_kcontrol *kcontrol)
{
	int backend_id;

	if (strnstr(kcontrol->id.name, "SLIM7_RX", sizeof("SLIM7_RX"))) {
		backend_id = MSM_BACKEND_DAI_SLIMBUS_7_RX;
	} else if (strnstr(kcontrol->id.name, "SLIM9_TX", sizeof("SLIM9_TX"))) {
		backend_id = MSM_BACKEND_DAI_SLIMBUS_9_TX;
	} else if (strnstr(kcontrol->id.name, "QUAT_MI2S_RX", sizeof("QUAT_MI2S_RX"))) {
		backend_id = MSM_BACKEND_DAI_QUATERNARY_MI2S_RX;
	} else {
		pr_err("%s: unsupported backend id: %s",
			__func__, kcontrol->id.name);
		return -EINVAL;
	}

	return backend_id;
}

static int msm_routing_adm_channel_config_get(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int backend_id = msm_routing_adm_get_backend_idx(kcontrol);

	if (backend_id >= 0) {
		mutex_lock(&routing_lock);
		ucontrol->value.integer.value[0] =
			 msm_bedais[backend_id].adm_override_ch;
		pr_debug("%s: adm channel count %ld for BE:%d\n", __func__,
			 ucontrol->value.integer.value[0], backend_id);
		 mutex_unlock(&routing_lock);
	}

	return 0;
}

static int msm_routing_adm_channel_config_put(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int backend_id = msm_routing_adm_get_backend_idx(kcontrol);

	if (backend_id >= 0) {
		mutex_lock(&routing_lock);
		msm_bedais[backend_id].adm_override_ch =
				 ucontrol->value.integer.value[0];
		pr_debug("%s:updating BE :%d  adm channels: %d\n",
			  __func__, backend_id,
			  msm_bedais[backend_id].adm_override_ch);
		mutex_unlock(&routing_lock);
	}

	return 0;
}

static const struct snd_kcontrol_new adm_channel_config_controls[] = {
	SOC_ENUM_EXT("SLIM7_RX ADM Channels", adm_override_chs,
			msm_routing_adm_channel_config_get,
			msm_routing_adm_channel_config_put),
	SOC_ENUM_EXT("SLIM9_TX ADM Channels", adm_override_chs,
			msm_routing_adm_channel_config_get,
			msm_routing_adm_channel_config_put),
	SOC_ENUM_EXT("QUAT_MI2S_RX ADM Channels", mi2s_rx_adm_override_chs,
			msm_routing_adm_channel_config_get,
			msm_routing_adm_channel_config_put),
};

static int msm_routing_slim_0_rx_aanc_mux_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{

	mutex_lock(&routing_lock);
	ucontrol->value.integer.value[0] = slim0_rx_aanc_fb_port;
	mutex_unlock(&routing_lock);
	pr_debug("%s: AANC Mux Port %ld\n", __func__,
		ucontrol->value.integer.value[0]);
	return 0;
};

static int msm_routing_slim_0_rx_aanc_mux_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct aanc_data aanc_info;

	mutex_lock(&routing_lock);
	memset(&aanc_info, 0x00, sizeof(aanc_info));
	pr_debug("%s: AANC Mux Port %ld\n", __func__,
		ucontrol->value.integer.value[0]);
	slim0_rx_aanc_fb_port = ucontrol->value.integer.value[0];
	if (ucontrol->value.integer.value[0] == 0) {
		aanc_info.aanc_active = false;
		aanc_info.aanc_tx_port = 0;
		aanc_info.aanc_rx_port = 0;
	} else {
		aanc_info.aanc_active = true;
		aanc_info.aanc_rx_port = SLIMBUS_0_RX;
		aanc_info.aanc_tx_port =
			(SLIMBUS_0_RX - 1 + (slim0_rx_aanc_fb_port * 2));
	}
	afe_set_aanc_info(&aanc_info);
	mutex_unlock(&routing_lock);
	return 0;
};
static int msm_routing_get_port_mixer(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int idx = 0, shift = 0;
	struct soc_mixer_control *mc =
	(struct soc_mixer_control *)kcontrol->private_value;

	idx = mc->rshift/(sizeof(msm_bedais[mc->shift].port_sessions[0]) * 8);
	shift = mc->rshift%(sizeof(msm_bedais[mc->shift].port_sessions[0]) * 8);

	if (idx >= BE_DAI_PORT_SESSIONS_IDX_MAX) {
		pr_err("%s: Invalid idx = %d\n", __func__, idx);
		return -EINVAL;
	}

	if (test_bit(shift,
		(unsigned long *)&msm_bedais[mc->shift].port_sessions[idx]))
		ucontrol->value.integer.value[0] = 1;
	else
		ucontrol->value.integer.value[0] = 0;

	pr_debug("%s: shift %x rshift %x val %ld\n", __func__, mc->shift, mc->rshift,
	ucontrol->value.integer.value[0]);

	return 0;
}

static int msm_routing_put_port_mixer(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int idx = 0, shift = 0;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;

	idx = mc->rshift/(sizeof(msm_bedais[mc->shift].port_sessions[0]) * 8);
	shift = mc->rshift%(sizeof(msm_bedais[mc->shift].port_sessions[0]) * 8);

	if (idx >= BE_DAI_PORT_SESSIONS_IDX_MAX) {
		pr_err("%s: Invalid idx = %d\n", __func__, idx);
		return -EINVAL;
	}

	pr_debug("%s: shift 0x%x rshift 0x%x val %ld idx %d reminder shift %d\n",
		 __func__, mc->shift, mc->rshift,
		 ucontrol->value.integer.value[0], idx, shift);

	if (ucontrol->value.integer.value[0]) {
		afe_loopback(1, msm_bedais[mc->shift].port_id,
			    msm_bedais[mc->rshift].port_id);
		set_bit(shift,
		(unsigned long *)&msm_bedais[mc->shift].port_sessions[idx]);
	} else {
		afe_loopback(0, msm_bedais[mc->shift].port_id,
			    msm_bedais[mc->rshift].port_id);
		clear_bit(shift,
		(unsigned long *)&msm_bedais[mc->shift].port_sessions[idx]);
	}

	return 1;
}

static int msm_pcm_get_channel_rule_index(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;

	fe_id = ((struct soc_mixer_control *)
			kcontrol->private_value)->shift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	ucontrol->value.integer.value[0] = channel_mixer[fe_id].rule;

	return 0;
}

static int msm_pcm_put_channel_rule_index(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;

	fe_id = ((struct soc_mixer_control *)
			kcontrol->private_value)->shift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	channel_mixer[fe_id].rule = ucontrol->value.integer.value[0];

	return 1;
}

static int msm_pcm_get_out_chs(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;

	fe_id = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->shift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	ucontrol->value.integer.value[0] =
		channel_mixer[fe_id].output_channel;
	return 0;
}

static int msm_pcm_put_out_chs(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;

	fe_id = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->shift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	pr_debug("%s: fe_id is %d, output channels = %d\n", __func__,
			fe_id,
			(unsigned int)(ucontrol->value.integer.value[0]));
	channel_mixer[fe_id].output_channel =
			(unsigned int)(ucontrol->value.integer.value[0]);

	return 1;
}

static const char *const ch_mixer[] = {"Disable", "Enable"};

/* If new backend is added, need update this array */
static const char *const be_name[] = {
"ZERO", "PRI_I2S_RX", "PRI_I2S_TX", "SLIM_0_RX",
"SLIM_0_TX", "HDMI_RX", "INT_BT_SCO_RX", "INT_BT_SCO_TX",
"INT_FM_RX", "INT_FM_TX", "AFE_PCM_RX", "AFE_PCM_TX",
"AUXPCM_RX", "AUXPCM_TX", "VOICE_PLAYBACK_TX", "VOICE2_PLAYBACK_TX",
"INCALL_RECORD_RX", "INCALL_RECORD_TX", "MI2S_RX", "MI2S_TX",
"SEC_I2S_RX", "SLIM_1_RX", "SLIM_1_TX", "SLIM_2_RX",
"SLIM_2_TX", "SLIM_3_RX", "SLIM_3_TX", "SLIM_4_RX",
"SLIM_4_TX", "SLIM_5_RX", "SLIM_5_TX", "SLIM_6_RX",
"SLIM_6_TX", "SLIM_7_RX", "SLIM_7_TX", "SLIM_8_RX",
"SLIM_8_TX", "EXTPROC_RX", "EXTPROC_TX", "EXPROC_EC_TX",
"QUAT_MI2S_RX", "QUAT_MI2S_TX", "SECOND_MI2S_RX", "SECOND_MI2S_TX",
"PRI_MI2S_RX", "PRI_MI2S_TX", "TERT_MI2S_RX", "TERT_MI2S_TX",
"AUDIO_I2S_RX", "SEC_AUXPCM_RX", "SEC_AUXPCM_TX", "PRI_SPDIF_RX",
"SECOND_MI2S_RX_SD1", "QUIN_MI2S_RX", "QUIN_MI2S_TX", "SENARY_MI2S_TX",
"PRI_TDM_RX_0", "PRI_TDM_TX_0", "PRI_TDM_RX_1", "PRI_TDM_TX_1",
"PRI_TDM_RX_2", "PRI_TDM_TX_2", "PRI_TDM_RX_3", "PRI_TDM_TX_3",
"PRI_TDM_RX_4", "PRI_TDM_TX_4", "PRI_TDM_RX_5", "PRI_TDM_TX_5",
"PRI_TDM_RX_6", "PRI_TDM_TX_6", "PRI_TDM_RX_7", "PRI_TDM_TX_7",
"SEC_TDM_RX_0", "SEC_TDM_TX_0", "SEC_TDM_RX_1", "SEC_TDM_TX_1",
"SEC_TDM_RX_2", "SEC_TDM_TX_2", "SEC_TDM_RX_3", "SEC_TDM_TX_3",
"SEC_TDM_RX_4", "SEC_TDM_TX_4", "SEC_TDM_RX_5", "SEC_TDM_TX_5",
"SEC_TDM_RX_6", "SEC_TDM_TX_6", "SEC_TDM_RX_7", "SEC_TDM_TX_7",
"TERT_TDM_RX_0", "TERT_TDM_TX_0", "TERT_TDM_RX_1", "TERT_TDM_TX_1",
"TERT_TDM_RX_2", "TERT_TDM_TX_2", "TERT_TDM_RX_3", "TERT_TDM_TX_3",
"TERT_TDM_RX_4", "TERT_TDM_TX_4", "TERT_TDM_RX_5", "TERT_TDM_TX_5",
"TERT_TDM_RX_6", "TERT_TDM_TX_6", "TERT_TDM_RX_7", "TERT_TDM_TX_7",
"QUAT_TDM_RX_0", "QUAT_TDM_TX_0", "QUAT_TDM_RX_1", "QUAT_TDM_TX_1",
"QUAT_TDM_RX_2", "QUAT_TDM_TX_2", "QUAT_TDM_RX_3", "QUAT_TDM_TX_3",
"QUAT_TDM_RX_4", "QUAT_TDM_TX_4", "QUAT_TDM_RX_5", "QUAT_TDM_TX_5",
"QUAT_TDM_RX_6", "QUAT_TDM_TX_6", "QUAT_TDM_RX_7", "QUAT_TDM_TX_7",
"QUIN_TDM_RX_0", "QUIN_TDM_TX_0", "QUIN_TDM_RX_1", "QUIN_TDM_TX_1",
"QUIN_TDM_RX_2", "QUIN_TDM_TX_2", "QUIN_TDM_RX_3", "QUIN_TDM_TX_3",
"QUIN_TDM_RX_4", "QUIN_TDM_TX_4", "QUIN_TDM_RX_5", "QUIN_TDM_TX_5",
"QUIN_TDM_RX_6", "QUIN_TDM_TX_6", "QUIN_TDM_RX_7", "QUIN_TDM_TX_7",
"SEN_TDM_RX_0", "SEN_TDM_TX_0", "SEN_TDM_RX_1", "SEN_TDM_TX_1",
"SEN_TDM_RX_2", "SEN_TDM_TX_2", "SEN_TDM_RX_3", "SEN_TDM_TX_3",
"SEN_TDM_RX_4", "SEN_TDM_TX_4", "SEN_TDM_RX_5", "SEN_TDM_TX_5",
"SEN_TDM_RX_6", "SEN_TDM_TX_6", "SEN_TDM_RX_7", "SEN_TDM_TX_7",
"INT_BT_A2DP_RX", "USB_RX", "USB_TX", "DISPLAY_PORT_RX",
"DISPLAY_PORT_RX1", "TERT_AUXPCM_RX", "TERT_AUXPCM_TX", "QUAT_AUXPCM_RX",
"QUAT_AUXPCM_TX", "QUIN_AUXPCM_RX", "QUIN_AUXPCM_TX", "INT0_MI2S_RX",
"INT0_MI2S_TX", "INT1_MI2S_RX", "INT1_MI2S_TX", "INT2_MI2S_RX",
"INT2_MI2S_TX", "INT3_MI2S_RX", "INT3_MI2S_TX", "INT4_MI2S_RX",
"INT4_MI2S_TX", "INT5_MI2S_RX", "INT5_MI2S_TX", "INT6_MI2S_RX",
"INT6_MI2S_TX", "SEN_AUXPCM_RX", "SEN_AUXPCM_TX", "SENARY_MI2S_RX",
"WSA_CDC_DMA_RX_0", "WSA_CDC_DMA_TX_0", "WSA_CDC_DMA_RX_1","WSA_CDC_DMA_TX_1",
"WSA_CDC_DMA_TX_2", "VA_CDC_DMA_TX_0", "VA_CDC_DMA_TX_1", "VA_CDC_DMA_TX_2",
"RX_CDC_DMA_RX_0", "TX_CDC_DMA_TX_0", "RX_CDC_DMA_RX_1", "TX_CDC_DMA_TX_1",
"RX_CDC_DMA_RX_2", "TX_CDC_DMA_TX_2", "RX_CDC_DMA_RX_3", "TX_CDC_DMA_TX_3",
"RX_CDC_DMA_RX_4", "TX_CDC_DMA_TX_4", "RX_CDC_DMA_RX_5", "TX_CDC_DMA_TX_5",
"RX_CDC_DMA_RX_6", "RX_CDC_DMA_RX_7",
"PRI_SPDIF_TX", "SEC_SPDIF_RX", "SEC_SPDIF_TX",
"SLIM_9_RX", "SLIM_9_TX", "AFE_LOOPBACK_TX", "PRI_META_MI2S_RX",
"SEC_META_MI2S_RX", "PROXY_RX", "PROXY_TX", "HDMI_RX_MS"
};

static SOC_ENUM_SINGLE_DECL(mm1_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm2_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA2, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm3_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA3, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm4_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA4, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm18_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA18, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm19_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA19, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm28_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA28, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm29_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA29, ch_mixer);
static SOC_ENUM_SINGLE_DECL(mm30_channel_mux,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA30, ch_mixer);

static SOC_ENUM_DOUBLE_DECL(mm1_ch1_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 0, be_name);
static SOC_ENUM_DOUBLE_DECL(mm1_ch2_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 1, be_name);
static SOC_ENUM_DOUBLE_DECL(mm1_ch3_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 2, be_name);
static SOC_ENUM_DOUBLE_DECL(mm1_ch4_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 3, be_name);
static SOC_ENUM_DOUBLE_DECL(mm1_ch5_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 4, be_name);
static SOC_ENUM_DOUBLE_DECL(mm1_ch6_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 5, be_name);
static SOC_ENUM_DOUBLE_DECL(mm1_ch7_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 6, be_name);
static SOC_ENUM_DOUBLE_DECL(mm1_ch8_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA1, 7, be_name);
static SOC_ENUM_DOUBLE_DECL(mm18_ch1_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA18, 0, be_name);
static SOC_ENUM_DOUBLE_DECL(mm18_ch2_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA18, 1, be_name);
static SOC_ENUM_DOUBLE_DECL(mm19_ch1_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA19, 0, be_name);
static SOC_ENUM_DOUBLE_DECL(mm19_ch2_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA19, 1, be_name);
static SOC_ENUM_DOUBLE_DECL(mm28_ch1_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA28, 0, be_name);
static SOC_ENUM_DOUBLE_DECL(mm28_ch2_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA28, 1, be_name);
static SOC_ENUM_DOUBLE_DECL(mm29_ch1_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA29, 0, be_name);
static SOC_ENUM_DOUBLE_DECL(mm29_ch2_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA29, 1, be_name);
static SOC_ENUM_DOUBLE_DECL(mm30_ch1_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA30, 0, be_name);
static SOC_ENUM_DOUBLE_DECL(mm30_ch2_enum,
	SND_SOC_NOPM, MSM_FRONTEND_DAI_MULTIMEDIA30, 1, be_name);

static int msm_pcm_get_ctl_enum_info(struct snd_ctl_elem_info *uinfo,
		unsigned int channels,
		unsigned int items, const char *const names[])
{
	if (uinfo->value.enumerated.item >= items)
		uinfo->value.enumerated.item = items - 1;

	WARN(strlen(names[uinfo->value.enumerated.item]) >=
		sizeof(uinfo->value.enumerated.name),
		"ALSA: too long item name '%s'\n",
		names[uinfo->value.enumerated.item]);
	strlcpy(uinfo->value.enumerated.name,
		names[uinfo->value.enumerated.item],
		sizeof(uinfo->value.enumerated.name));
	return 0;
}

static int msm_pcm_channel_mixer_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;

	uinfo->value.enumerated.items = ARRAY_SIZE(ch_mixer);
	msm_pcm_get_ctl_enum_info(uinfo, 1, e->items, e->texts);

	return 0;
}
static int msm_pcm_channel_mixer_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;

	fe_id = ((struct soc_enum *)
			kcontrol->private_value)->shift_l;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	pr_debug("%s: FE %d %s\n", __func__,
		fe_id,
		channel_mixer[fe_id].enable ? "Enabled" : "Disabled");
	ucontrol->value.enumerated.item[0] = channel_mixer[fe_id].enable;
	return 0;
}

static int msm_pcm_channel_mixer_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;

	fe_id = ((struct soc_enum *)
			kcontrol->private_value)->shift_l;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	channel_mixer[fe_id].enable = ucontrol->value.enumerated.item[0];
	pr_debug("%s: %s FE %d\n", __func__,
		channel_mixer[fe_id].enable ? "Enable" : "Disable",
		fe_id);
	return 0;
}

static int msm_pcm_channel_input_be_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;

	uinfo->value.enumerated.items = ARRAY_SIZE(be_name);
	msm_pcm_get_ctl_enum_info(uinfo, 1, e->items, e->texts);

	return 0;
}

static int msm_pcm_channel_input_be_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	u16 fe_id = 0, in_ch = 0;

	fe_id = e->shift_l;
	in_ch = e->shift_r;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}
	if (in_ch >= ADM_MAX_CHANNELS) {
		pr_err("%s: invalid input channel %d\n", __func__, in_ch);
		return -EINVAL;
	}

	channel_input[fe_id][in_ch] = ucontrol->value.enumerated.item[0];
	return 1;
}

static int msm_pcm_channel_input_be_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	u16 fe_id = 0, in_ch = 0;

	fe_id = e->shift_l;
	in_ch = e->shift_r;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}
	if (in_ch >= ADM_MAX_CHANNELS) {
		pr_err("%s: invalid input channel %d\n", __func__, in_ch);
		return -EINVAL;
	}

	ucontrol->value.enumerated.item[0] = channel_input[fe_id][in_ch];
	return 1;
}


static int msm_pcm_channel_weight_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = ADM_MAX_CHANNELS;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = WEIGHT_0_DB;

	return 0;
}

static int msm_pcm_channel_weight_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0, out_ch = 0;
	int i, weight;

	fe_id = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->shift;
	out_ch = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->rshift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}
	if (out_ch >= ADM_MAX_CHANNELS) {
		pr_err("%s: invalid input channel %d\n", __func__, out_ch);
		return -EINVAL;
	}

	pr_debug("%s: FE_ID: %d, channel weight %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld\n",
		__func__, fe_id,
		ucontrol->value.integer.value[0],
		ucontrol->value.integer.value[1],
		ucontrol->value.integer.value[2],
		ucontrol->value.integer.value[3],
		ucontrol->value.integer.value[4],
		ucontrol->value.integer.value[5],
		ucontrol->value.integer.value[6],
		ucontrol->value.integer.value[7]);

	for (i = 0; i < ADM_MAX_CHANNELS; ++i) {
		weight = ucontrol->value.integer.value[i];
		channel_mixer[fe_id].channel_weight[out_ch][i] = weight;
		pr_debug("%s: FE_ID %d, output %d input %d weight %d\n",
			__func__, fe_id, out_ch, i,
			channel_mixer[fe_id].channel_weight[out_ch][i]);
	}

	return 0;
}

static int msm_pcm_channel_weight_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0, out_ch = 0;
	int i;

	fe_id = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->shift;
	out_ch = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->rshift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}
	if (out_ch >= ADM_MAX_CHANNELS) {
		pr_err("%s: invalid input channel %d\n", __func__, out_ch);
		return -EINVAL;
	}

	for (i = 0; i < ADM_MAX_CHANNELS; ++i)
		ucontrol->value.integer.value[i] =
			channel_mixer[fe_id].channel_weight[out_ch][i];

	pr_debug("%s: FE_ID: %d, weight  %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld",
		__func__, fe_id,
		ucontrol->value.integer.value[0],
		ucontrol->value.integer.value[1],
		ucontrol->value.integer.value[2],
		ucontrol->value.integer.value[3],
		ucontrol->value.integer.value[4],
		ucontrol->value.integer.value[5],
		ucontrol->value.integer.value[6],
		ucontrol->value.integer.value[7]);

	return 0;
}

static int msm_pcm_channel_output_map_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = PCM_FORMAT_MAX_NUM_CHANNEL_V8;
	/* Valid channel map value ranges from 1 to 64 */
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 64;

	return 0;
}

static int msm_pcm_channel_output_map_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;
	int i, ch_map;

	fe_id = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->shift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	for (i = 0; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; ++i) {
		ch_map = ucontrol->value.integer.value[i];
		channel_mixer[fe_id].out_ch_map[i] = ch_map;
		pr_debug("%s: FE_ID %d, channel %d channel map %d\n",
			__func__, fe_id, i, channel_mixer[fe_id].out_ch_map[i]);
	}

	/* Make override_out_ch_map false if ch map is reset */
	if (channel_mixer[fe_id].out_ch_map[0] == 0)
		channel_mixer[fe_id].override_out_ch_map = false;
	else
		channel_mixer[fe_id].override_out_ch_map = true;

	return 0;
}

static int msm_pcm_channel_output_map_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u16 fe_id = 0;
	int i;

	fe_id = ((struct soc_multi_mixer_control *)
			kcontrol->private_value)->shift;
	if (fe_id >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_err("%s: invalid FE %d\n", __func__, fe_id);
		return -EINVAL;
	}

	for (i = 0; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; ++i)
		ucontrol->value.integer.value[i] =
			channel_mixer[fe_id].out_ch_map[i];

	return 0;
}


static int msm_ec_ref_ch_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_ec_ref_ch;
	pr_debug("%s: msm_ec_ref_ch = %ld\n", __func__,
		ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_ec_ref_ch_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	msm_ec_ref_ch = ucontrol->value.integer.value[0];
	pr_debug("%s: msm_ec_ref_ch = %d\n", __func__, msm_ec_ref_ch);
	adm_num_ec_ref_rx_chans(msm_ec_ref_ch);
	return 0;
}

static int msm_ec_ref_ch_downmixed_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_ec_ref_ch_downmixed;
	pr_debug("%s: msm_ec_ref_downmixed_ch = %ld\n", __func__,
		ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_ec_ref_ch_downmixed_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	msm_ec_ref_ch_downmixed = ucontrol->value.integer.value[0];
	pr_debug("%s: msm_ec_ref_downmixed_ch = %d\n",
		__func__, msm_ec_ref_ch_downmixed);
	adm_num_ec_ref_rx_chans_downmixed(msm_ec_ref_ch_downmixed);
	return 0;
}

static int msm_ec_ref_chmixer_weights_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	int i = 0, ret = 0;
	uint16_t weights[PCM_FORMAT_MAX_NUM_CHANNEL_V8] = {0};
	int out_channel_idx = ((struct soc_multi_mixer_control *)
				kcontrol->private_value)->shift;

	for (; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; i++)
		weights[i] = ucontrol->value.integer.value[i];

	ret = adm_ec_ref_chmixer_weights(out_channel_idx,
					weights, PCM_FORMAT_MAX_NUM_CHANNEL_V8);
	pr_debug("%s: ch_index = %d, ret = %d\n", __func__, out_channel_idx, ret);
	return ret;
}

static const char *const ec_ref_ch_text[] = {"Zero", "One", "Two", "Three",
	"Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven",
	"Twelve", "Thirteen", "Fourteen", "Fifteen", "Sixteen"};

static int msm_ec_ref_bit_format_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	switch (msm_ec_ref_bit_format) {
	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 2;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		ucontrol->value.integer.value[0] = 1;
		break;
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}
	pr_debug("%s: msm_ec_ref_bit_format = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_ec_ref_bit_format_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	u16 bit_width = 0;

	switch (ucontrol->value.integer.value[0]) {
	case 2:
		msm_ec_ref_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 1:
		msm_ec_ref_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	default:
		msm_ec_ref_bit_format = 0;
		break;
	}

	if (msm_ec_ref_bit_format == SNDRV_PCM_FORMAT_S16_LE)
		bit_width = 16;
	else if (msm_ec_ref_bit_format == SNDRV_PCM_FORMAT_S24_LE)
		bit_width = 24;

	pr_debug("%s: msm_ec_ref_bit_format = %d\n",
		 __func__, msm_ec_ref_bit_format);
	adm_ec_ref_rx_bit_width(bit_width);
	return 0;
}

static char const *ec_ref_bit_format_text[] = {"0", "S16_LE", "S24_LE"};

static int msm_ec_ref_rate_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = msm_ec_ref_sampling_rate;
	pr_debug("%s: msm_ec_ref_sampling_rate = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_ec_ref_rate_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		msm_ec_ref_sampling_rate = 0;
		break;
	case 1:
		msm_ec_ref_sampling_rate = 8000;
		break;
	case 2:
		msm_ec_ref_sampling_rate = 16000;
		break;
	case 3:
		msm_ec_ref_sampling_rate = 32000;
		break;
	case 4:
		msm_ec_ref_sampling_rate = 44100;
		break;
	case 5:
		msm_ec_ref_sampling_rate = 48000;
		break;
	case 6:
		msm_ec_ref_sampling_rate = 96000;
		break;
	case 7:
		msm_ec_ref_sampling_rate = 192000;
		break;
	case 8:
		msm_ec_ref_sampling_rate = 384000;
		break;
	default:
		msm_ec_ref_sampling_rate = 48000;
		break;
	}
	pr_debug("%s: msm_ec_ref_sampling_rate = %d\n",
		 __func__, msm_ec_ref_sampling_rate);
	adm_ec_ref_rx_sampling_rate(msm_ec_ref_sampling_rate);
	return 0;
}

static const u16 ec_ref_port_id_map[] = {
	[0]  = AFE_PORT_INVALID,
	[1]  = SLIMBUS_0_RX,
	[2]  = AFE_PORT_ID_PRIMARY_MI2S_RX,
	[3]  = AFE_PORT_ID_PRIMARY_MI2S_TX,
	[4]  = AFE_PORT_ID_SECONDARY_MI2S_TX,
	[5]  = AFE_PORT_ID_TERTIARY_MI2S_TX,
	[6]  = AFE_PORT_ID_QUATERNARY_MI2S_TX,
	[7]  = AFE_PORT_ID_SECONDARY_MI2S_RX,
	[8]  = AFE_PORT_INVALID,
	[9]  = SLIMBUS_5_RX,
	[10] = SLIMBUS_1_TX,
	[11] = AFE_PORT_ID_QUATERNARY_TDM_TX_1,
	[12] = AFE_PORT_ID_QUATERNARY_TDM_RX,
	[13] = AFE_PORT_ID_QUATERNARY_TDM_RX_1,
	[14] = AFE_PORT_ID_QUATERNARY_TDM_RX_2,
	[15] = SLIMBUS_6_RX,
	[16] = AFE_PORT_ID_TERTIARY_MI2S_RX,
	[17] = AFE_PORT_ID_QUATERNARY_MI2S_RX,
	[18] = AFE_PORT_ID_TERTIARY_TDM_TX,
	[19] = AFE_PORT_ID_USB_RX,
	[20] = AFE_PORT_ID_INT0_MI2S_RX,
	[21] = AFE_PORT_ID_INT4_MI2S_RX,
	[22] = AFE_PORT_ID_INT3_MI2S_TX,
	[23] = AFE_PORT_ID_HDMI_OVER_DP_RX,
	[24] = AFE_PORT_ID_WSA_CODEC_DMA_RX_0,
	[25] = AFE_PORT_ID_WSA_CODEC_DMA_RX_1,
	[26] = AFE_PORT_ID_WSA_CODEC_DMA_TX_0,
	[27] = AFE_PORT_ID_WSA_CODEC_DMA_TX_1,
	[28] = AFE_PORT_ID_WSA_CODEC_DMA_TX_2,
	[29] = SLIMBUS_7_RX,
	[30] = AFE_PORT_ID_RX_CODEC_DMA_RX_0,
	[31] = AFE_PORT_ID_RX_CODEC_DMA_RX_1,
	[32] = AFE_PORT_ID_RX_CODEC_DMA_RX_2,
	[33] = AFE_PORT_ID_RX_CODEC_DMA_RX_3,
	[34] = AFE_PORT_ID_TX_CODEC_DMA_TX_0,
	[35] = AFE_PORT_ID_TERTIARY_TDM_RX_2,
	[36] = AFE_PORT_ID_SECONDARY_TDM_TX,
	[37] = AFE_PORT_ID_HDMI_OVER_DP_RX,
	[38] = AFE_PORT_ID_SENARY_MI2S_RX,
	[39] = AFE_PORT_ID_SENARY_MI2S_TX,
	[40] = AFE_PORT_ID_QUINARY_TDM_TX,
	[41] = AFE_PORT_ID_PRIMARY_TDM_RX,
	[42] = SLIMBUS_7_TX,
};

static int get_ec_ref_port_id(int value, int *index)
{
	if (value >= 0 && value < ARRAY_SIZE(ec_ref_port_id_map) &&
	    (value == 0 || ec_ref_port_id_map[value] != AFE_PORT_INVALID)) {
		*index = value;
		return ec_ref_port_id_map[value];
	}

	*index = 0; /* NONE */
	pr_err("%s: Invalid value %d\n", __func__, value);
	return AFE_PORT_INVALID;
}

static int msm_routing_afe_lb_tx_port_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: port index = %d", __func__, afe_loopback_tx_port_index);
	mutex_lock(&routing_lock);
	ucontrol->value.integer.value[0] = afe_loopback_tx_port_index;
	mutex_unlock(&routing_lock);

	return 0;
}

static int msm_routing_afe_lb_tx_port_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int value = ucontrol->value.integer.value[0];

	mutex_lock(&routing_lock);
	afe_loopback_tx_port_id = get_ec_ref_port_id(value,
			&afe_loopback_tx_port_index);
	pr_debug("%s: afe_loopback_tx_port_index = %d\n",
	    __func__, afe_loopback_tx_port_index);
	mutex_unlock(&routing_lock);

	return 0;
}

static int msm_routing_ec_ref_rx_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: ec_ref_rx  = %d", __func__, msm_route_ec_ref_rx);
	mutex_lock(&routing_lock);
	ucontrol->value.integer.value[0] = msm_route_ec_ref_rx;
	mutex_unlock(&routing_lock);

	return 0;
}

static int msm_routing_ec_ref_rx_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int value = ucontrol->value.integer.value[0];
	struct snd_soc_dapm_widget *widget =
		snd_soc_dapm_kcontrol_widget(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_dapm_update *update = NULL;

	mutex_lock(&routing_lock);
	msm_ec_ref_port_id = get_ec_ref_port_id(value, &msm_route_ec_ref_rx);
	adm_ec_ref_rx_id(msm_ec_ref_port_id);
	pr_debug("%s: msm_route_ec_ref_rx = %d\n",
	    __func__, msm_route_ec_ref_rx);
	mutex_unlock(&routing_lock);

	snd_soc_dapm_mux_update_power(widget->dapm, kcontrol,
					msm_route_ec_ref_rx, e, update);
	return 0;
}

static int msm_routing_ext_ec_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: ext_ec_ref_rx  = %x\n", __func__, msm_route_ext_ec_ref);

	mutex_lock(&routing_lock);
	ucontrol->value.integer.value[0] = msm_route_ext_ec_ref;
	mutex_unlock(&routing_lock);
	return 0;
}

static const u16 ext_ec_ref_port_id_map[] = {
	[EXT_EC_REF_PRI_MI2S_TX]  = AFE_PORT_ID_PRIMARY_MI2S_TX,
	[EXT_EC_REF_SEC_MI2S_TX]  = AFE_PORT_ID_SECONDARY_MI2S_TX,
	[EXT_EC_REF_TERT_MI2S_TX] = AFE_PORT_ID_TERTIARY_MI2S_TX,
	[EXT_EC_REF_QUAT_MI2S_TX] = AFE_PORT_ID_QUATERNARY_MI2S_TX,
	[EXT_EC_REF_QUIN_MI2S_TX] = AFE_PORT_ID_QUINARY_MI2S_TX,
	[EXT_EC_REF_SLIM_1_TX]    = SLIMBUS_1_TX,
	[EXT_EC_REF_PRI_TDM_TX]   = AFE_PORT_ID_PRIMARY_TDM_TX,
	[EXT_EC_REF_SEC_TDM_TX]   = AFE_PORT_ID_SECONDARY_TDM_TX,
};

static int msm_routing_ext_ec_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget =
		snd_soc_dapm_kcontrol_widget(kcontrol);
	int mux = ucontrol->value.enumerated.item[0];
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	int ret = 1;
	bool state = false;
	uint16_t ext_ec_ref_port_id = AFE_PORT_INVALID;
	struct snd_soc_dapm_update *update = NULL;

	if (mux >= e->items) {
		pr_err("%s: Invalid mux value %d\n", __func__, mux);
		return -EINVAL;
	}

	mutex_lock(&routing_lock);
	msm_route_ext_ec_ref = ucontrol->value.integer.value[0];

	if (msm_route_ext_ec_ref > 0 &&
	    msm_route_ext_ec_ref < ARRAY_SIZE(ext_ec_ref_port_id_map) &&
	    ext_ec_ref_port_id_map[msm_route_ext_ec_ref]) {
		ext_ec_ref_port_id = ext_ec_ref_port_id_map[msm_route_ext_ec_ref];
		state = true;
	}

	pr_debug("%s: val = %d ext_ec_ref_port_id = 0x%0x state = %d\n",
		 __func__, msm_route_ext_ec_ref, ext_ec_ref_port_id, state);

	if (!voc_set_ext_ec_ref_port_id(ext_ec_ref_port_id, state)) {
		mutex_unlock(&routing_lock);
		snd_soc_dapm_mux_update_power(widget->dapm, kcontrol, mux, e,
					      update);
	} else {
		ret = -EINVAL;
		mutex_unlock(&routing_lock);
	}
	return ret;
}

static const char * const aanc_slim_0_rx_text[] = {
	"ZERO", "SLIMBUS_0_TX", "SLIMBUS_1_TX", "SLIMBUS_2_TX", "SLIMBUS_3_TX",
	"SLIMBUS_4_TX", "SLIMBUS_5_TX", "SLIMBUS_6_TX"
};

static const struct soc_enum aanc_slim_0_rx_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(aanc_slim_0_rx_text),
				aanc_slim_0_rx_text);

static const struct snd_kcontrol_new aanc_slim_0_rx_mux[] = {
	SOC_ENUM_EXT("AANC_SLIM_0_RX MUX", aanc_slim_0_rx_enum,
		msm_routing_slim_0_rx_aanc_mux_get,
		msm_routing_slim_0_rx_aanc_mux_put)
};

static int msm_routing_aanc_noise_level_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = aanc_level;

	return 0;
}

static int msm_routing_aanc_noise_level_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;

	mutex_lock(&routing_lock);
	aanc_level = ucontrol->value.integer.value[0];
	pr_debug("%s: value: %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	ret = afe_set_aanc_noise_level(aanc_level);
	mutex_unlock(&routing_lock);

	return ret;
}

static const struct snd_kcontrol_new aanc_noise_level[] = {
	SOC_SINGLE_EXT("AANC Noise Level", SND_SOC_NOPM, 0, 255,
	0, msm_routing_aanc_noise_level_get, msm_routing_aanc_noise_level_put)
};

static int msm_routing_get_stereo_to_custom_stereo_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = is_custom_stereo_on;
	return 0;
}

static int msm_routing_put_stereo_to_custom_stereo_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int flag = 0, i = 0, rc = 0, idx = 0;
	int be_index = 0, port_id, topo_id;
	unsigned int session_id = 0;
	uint16_t op_FL_ip_FL_weight = 0;
	uint16_t op_FL_ip_FR_weight = 0;
	uint16_t op_FR_ip_FL_weight = 0;
	uint16_t op_FR_ip_FR_weight = 0;

	flag = ucontrol->value.integer.value[0];
	pr_debug("%s E flag %d\n", __func__, flag);

	if ((is_custom_stereo_on && flag) || (!is_custom_stereo_on && !flag)) {
		pr_err("%s: is_custom_stereo_on %d, flag %d\n",
			__func__, is_custom_stereo_on, flag);
		return 0;
	}
	is_custom_stereo_on = flag ? true : false;
	pr_debug("%s:is_custom_stereo_on %d\n", __func__, is_custom_stereo_on);
	for (be_index = 0; be_index < MSM_BACKEND_DAI_MAX; be_index++) {
		port_id = msm_bedais[be_index].port_id;
		if (!msm_bedais[be_index].active)
			continue;
		if ((port_id != SLIMBUS_0_RX) &&
		     (port_id != RT_PROXY_PORT_001_RX) &&
			(port_id != AFE_PORT_ID_PRIMARY_MI2S_RX) &&
			(port_id != AFE_PORT_ID_INT4_MI2S_RX))
			continue;

		for_each_set_bit(i, &msm_bedais[be_index].fe_sessions[0],
				MSM_FRONTEND_DAI_MM_SIZE) {
			if (fe_dai_map[i][SESSION_TYPE_RX].perf_mode !=
			    LEGACY_PCM_MODE)
				goto skip_send_custom_stereo;
			session_id =
				fe_dai_map[i][SESSION_TYPE_RX].strm_id;
			if (is_custom_stereo_on) {
				op_FL_ip_FL_weight =
					Q14_GAIN_ZERO_POINT_FIVE;
				op_FL_ip_FR_weight =
					Q14_GAIN_ZERO_POINT_FIVE;
				op_FR_ip_FL_weight =
					Q14_GAIN_ZERO_POINT_FIVE;
				op_FR_ip_FR_weight =
					Q14_GAIN_ZERO_POINT_FIVE;
			} else {
				op_FL_ip_FL_weight = Q14_GAIN_UNITY;
				op_FL_ip_FR_weight = 0;
				op_FR_ip_FL_weight = 0;
				op_FR_ip_FR_weight = Q14_GAIN_UNITY;
			}
			for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++) {
				unsigned long copp =
					session_copp_map[i]
					[SESSION_TYPE_RX][be_index];
				if (!test_bit(idx, &copp))
					goto skip_send_custom_stereo;
				topo_id = adm_get_topology_for_port_copp_idx(
					msm_bedais[be_index].port_id, idx);
				if (topo_id < 0)
					pr_debug("%s:Err:custom stereo topo %d",
						 __func__, topo_id);
					pr_debug("idx %d\n", idx);
				if (topo_id == DS2_ADM_COPP_TOPOLOGY_ID)
					rc = msm_ds2_dap_set_custom_stereo_onoff
						(msm_bedais[be_index].port_id,
						idx, is_custom_stereo_on);
				else if (topo_id == DOLBY_ADM_COPP_TOPOLOGY_ID)
					rc = dolby_dap_set_custom_stereo_onoff(
						msm_bedais[be_index].port_id,
						idx, is_custom_stereo_on);
				else
				rc = msm_qti_pp_send_stereo_to_custom_stereo_cmd
						(msm_bedais[be_index].port_id,
						idx, session_id,
						op_FL_ip_FL_weight,
						op_FL_ip_FR_weight,
						op_FR_ip_FL_weight,
						op_FR_ip_FR_weight);
				if (rc < 0)
skip_send_custom_stereo:
					pr_err("%s: err setting custom stereo\n",
						__func__);
			}

		}
	}
	return 0;
}

static const struct snd_kcontrol_new stereo_to_custom_stereo_controls[] = {
	SOC_SINGLE_EXT("Set Custom Stereo OnOff", SND_SOC_NOPM, 0,
	1, 0, msm_routing_get_stereo_to_custom_stereo_control,
	msm_routing_put_stereo_to_custom_stereo_control),
};

static int msm_routing_get_app_type_cfg_control(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int msm_routing_put_app_type_cfg_control(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	int i = 0, j;
	int num_app_types = ucontrol->value.integer.value[i++];

	pr_debug("%s\n", __func__);

	memset(app_type_cfg, 0, MAX_APP_TYPES*
				sizeof(struct msm_pcm_routing_app_type_data));
	if (num_app_types > MAX_APP_TYPES) {
		pr_err("%s: number of app types exceed the max supported\n",
			__func__);
		return -EINVAL;
	}
	for (j = 0; j < num_app_types; j++) {
		app_type_cfg[j].app_type =
				ucontrol->value.integer.value[i++];
		app_type_cfg[j].sample_rate =
				ucontrol->value.integer.value[i++];
		app_type_cfg[j].bit_width =
				ucontrol->value.integer.value[i++];
	}

	return 0;
}

static int msm_routing_put_app_type_gain_control(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	int j, fe_id, be_id, port_type;
	int ret = 0;
	unsigned long copp;
	struct msm_pcm_routing_bdai_data *bedai;
	int dir = ucontrol->value.integer.value[0] ? SESSION_TYPE_TX :
						     SESSION_TYPE_RX;
	int app_type = ucontrol->value.integer.value[1];
	int gain = (ucontrol->value.integer.value[2] +
		    ucontrol->value.integer.value[3])/2;

	port_type = (dir == SESSION_TYPE_RX) ? MSM_AFE_PORT_TYPE_RX :
					       MSM_AFE_PORT_TYPE_TX;

	mutex_lock(&routing_lock);
	for (be_id = 0; be_id < MSM_BACKEND_DAI_MAX; be_id++) {
		if (is_be_dai_extproc(be_id))
			continue;

		bedai = &msm_bedais[be_id];
		if (afe_get_port_type(bedai->port_id) != port_type)
			continue;

		if (!bedai->active)
			continue;

		for (fe_id = 0; fe_id < MSM_FRONTEND_DAI_MAX; fe_id++) {
			if (!test_bit(fe_id, &bedai->fe_sessions[0]))
				continue;

			if (app_type !=
			    fe_dai_app_type_cfg[fe_id][dir][be_id].app_type)
				continue;

			copp = session_copp_map[fe_id][dir][be_id];
			for (j = 0; j < MAX_COPPS_PER_PORT; j++) {
				if (!test_bit(j, &copp))
					continue;
				ret |= adm_set_volume(bedai->port_id, j, gain);
			}
		}
	}
	mutex_unlock(&routing_lock);
	return ret ? -EINVAL : 0;
}

static const struct snd_kcontrol_new app_type_cfg_controls[] = {
	SOC_SINGLE_MULTI_EXT("App Type Config", SND_SOC_NOPM, 0,
	0x7FFFFFFF, 0, 128, msm_routing_get_app_type_cfg_control,
	msm_routing_put_app_type_cfg_control),
	SOC_SINGLE_MULTI_EXT("App Type Gain", SND_SOC_NOPM, 0,
	0x2000, 0, 4, NULL, msm_routing_put_app_type_gain_control)
};

static int msm_routing_put_module_cfg_control(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	int copp_idx, fe_id, be_id, port_type;
	int ret = 0;
	unsigned long copp;
	struct msm_pcm_routing_bdai_data *bedai;
	u8 *packed_params = NULL;
	struct param_hdr_v3 param_hdr;
	u32 packed_param_size = (sizeof(struct param_hdr_v3) +
				 sizeof(uint32_t));

	int dir = ucontrol->value.integer.value[0] ? SESSION_TYPE_TX :
						     SESSION_TYPE_RX;
	int app_type = ucontrol->value.integer.value[1];
	int module_id = ucontrol->value.integer.value[2];
	int instance_id = ucontrol->value.integer.value[3];
	int param_id = ucontrol->value.integer.value[4];
	int param_value = ucontrol->value.integer.value[5];

	port_type = (dir == SESSION_TYPE_RX) ? MSM_AFE_PORT_TYPE_RX :
					       MSM_AFE_PORT_TYPE_TX;
	pr_debug("%s app_type:%d mod_id:%d instance_id:%d param_id:%d value:%d\n",
		  __func__, app_type, module_id,
		  instance_id, param_id, param_value);

	packed_params = kzalloc(packed_param_size, GFP_KERNEL);
	if (!packed_params)
		return -ENOMEM;

	memset(&param_hdr, 0, sizeof(param_hdr));
	param_hdr.module_id = module_id;
	param_hdr.instance_id = instance_id;
	param_hdr.param_id = param_id;
	param_hdr.param_size = sizeof(uint32_t);

	packed_param_size = 0;

	mutex_lock(&routing_lock);
	for (be_id = 0; be_id < MSM_BACKEND_DAI_MAX; be_id++) {
		if (is_be_dai_extproc(be_id))
			continue;

		bedai = &msm_bedais[be_id];
		if (afe_get_port_type(bedai->port_id) != port_type)
			continue;

		if (!bedai->active)
			continue;

		for (fe_id = 0; fe_id < MSM_FRONTEND_DAI_MAX; fe_id++) {
			if (!test_bit(fe_id, &bedai->fe_sessions[0]))
				continue;

			if (app_type !=
			    fe_dai_app_type_cfg[fe_id][dir][be_id].app_type)
				continue;

			copp = session_copp_map[fe_id][dir][be_id];
			for (copp_idx = 0; copp_idx < MAX_COPPS_PER_PORT;
			     copp_idx++) {
				if (!test_bit(copp_idx, &copp))
					continue;

				ret = q6common_pack_pp_params(packed_params,
							&param_hdr,
							(u8 *) &param_value,
							&packed_param_size);
				if (ret) {
					pr_err("%s: Failed to pack params, error %d\n",
					       __func__, ret);
					goto done;
				}

				ret = adm_set_pp_params(bedai->port_id,
							 copp_idx, NULL,
							 packed_params,
							 packed_param_size);
				if (ret) {
					pr_err("%s: Setting param failed with err=%d\n",
						__func__, ret);
					ret = -EINVAL;
					goto done;
				}
			}
		}
	}
done:
	mutex_unlock(&routing_lock);
	kfree(packed_params);
	return ret;
}

static const struct snd_kcontrol_new module_cfg_controls[] = {
	SOC_SINGLE_MULTI_EXT("Audio Effect", SND_SOC_NOPM, 0,
	0x2000, 0, 6, NULL, msm_routing_put_module_cfg_control)
};

static int msm_routing_get_lsm_app_type_cfg_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int shift = ((struct soc_multi_mixer_control *)
				kcontrol->private_value)->shift;
	int i = 0, j = 0;

	mutex_lock(&routing_lock);
	ucontrol->value.integer.value[i] = num_app_cfg_types;

	for (j = 0; j < num_app_cfg_types; ++j) {
		ucontrol->value.integer.value[++i] =
				lsm_app_type_cfg[j].app_type;
		ucontrol->value.integer.value[++i] =
				lsm_app_type_cfg[j].sample_rate;
		ucontrol->value.integer.value[++i] =
				lsm_app_type_cfg[j].bit_width;
		if (shift == 1)
			ucontrol->value.integer.value[++i] =
				lsm_app_type_cfg[j].num_out_channels;
	}
	mutex_unlock(&routing_lock);
	return 0;
}

static int msm_routing_put_lsm_app_type_cfg_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int shift = ((struct soc_multi_mixer_control *)
				kcontrol->private_value)->shift;
	int i = 0, j;

	mutex_lock(&routing_lock);
	if (ucontrol->value.integer.value[0] > MAX_APP_TYPES) {
		pr_err("%s: number of app types exceed the max supported\n",
			__func__);
		mutex_unlock(&routing_lock);
		return -EINVAL;
	}

	num_app_cfg_types = ucontrol->value.integer.value[i++];
	memset(lsm_app_type_cfg, 0, MAX_APP_TYPES*
	       sizeof(struct msm_pcm_routing_app_type_data));

	for (j = 0; j < num_app_cfg_types; j++) {
		lsm_app_type_cfg[j].app_type =
				ucontrol->value.integer.value[i++];
		lsm_app_type_cfg[j].sample_rate =
				ucontrol->value.integer.value[i++];
		lsm_app_type_cfg[j].bit_width =
				ucontrol->value.integer.value[i++];
		/* Shift of 1 indicates this is V2 mixer control */
		if (shift == 1)
			lsm_app_type_cfg[j].num_out_channels =
				ucontrol->value.integer.value[i++];
	}
	mutex_unlock(&routing_lock);
	return 0;
}

static const struct snd_kcontrol_new lsm_app_type_cfg_controls[] = {
	SOC_SINGLE_MULTI_EXT("Listen App Type Config", SND_SOC_NOPM, 0,
	0xFFFFFFFF, 0, 128, msm_routing_get_lsm_app_type_cfg_control,
	msm_routing_put_lsm_app_type_cfg_control),
	SOC_SINGLE_MULTI_EXT("Listen App Type Config V2", SND_SOC_NOPM, 1,
	0xFFFFFFFF, 0, 128, msm_routing_get_lsm_app_type_cfg_control,
	msm_routing_put_lsm_app_type_cfg_control),
};

static int msm_routing_get_use_ds1_or_ds2_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = is_ds2_on;
	return 0;
}

static int msm_routing_put_use_ds1_or_ds2_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	is_ds2_on = ucontrol->value.integer.value[0];
	return 0;
}

static const struct snd_kcontrol_new use_ds1_or_ds2_controls[] = {
	SOC_SINGLE_EXT("DS2 OnOff", SND_SOC_NOPM, 0,
	1, 0, msm_routing_get_use_ds1_or_ds2_control,
	msm_routing_put_use_ds1_or_ds2_control),
};

static int msm_routing_get_hifi_filter_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = hifi_filter_enabled;
	return 0;
}

static int msm_routing_put_hifi_filter_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	hifi_filter_enabled = ucontrol->value.integer.value[0];
	return 0;
}

static const struct snd_kcontrol_new hifi_filter_controls[] = {
	SOC_SINGLE_EXT("HiFi Filter", SND_SOC_NOPM, 0,
		1, 0, msm_routing_get_hifi_filter_control,
		msm_routing_put_hifi_filter_control),
};

static int msm_routing_get_ffecns_freeze_event_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = ffecns_freeze_event;
	return 0;
}

static int msm_routing_put_ffecns_freeze_event_control(
					struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int ret = -EINVAL;

	ffecns_freeze_event = ucontrol->value.integer.value[0];

	ret = adm_set_ffecns_freeze_event(ffecns_freeze_event);
	if (ret)
		pr_err("%s: failed to set ffecns imc event to%d\n",
			__func__, ffecns_freeze_event);

	return ret;
}

static const struct snd_kcontrol_new use_ffecns_freeze_event_controls[] = {
	SOC_SINGLE_EXT("FFECNS Freeze Event", SND_SOC_NOPM, 0,
	1, 0, msm_routing_get_ffecns_freeze_event_control,
	msm_routing_put_ffecns_freeze_event_control),
};

int msm_routing_get_rms_value_control(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol) {
	int rc = 0;
	int be_idx = 0;
	char *param_value;
	int *update_param_value;
	uint32_t param_size = (RMS_PAYLOAD_LEN + 1) * sizeof(uint32_t);
	struct param_hdr_v3 param_hdr;

	param_value = kzalloc(param_size, GFP_KERNEL);
	if (!param_value)
		return -ENOMEM;

	memset(&param_hdr, 0, sizeof(param_hdr));
	for (be_idx = 0; be_idx < MSM_BACKEND_DAI_MAX; be_idx++)
		if (msm_bedais[be_idx].port_id == SLIMBUS_0_TX)
			break;
	if ((be_idx < MSM_BACKEND_DAI_MAX) && msm_bedais[be_idx].active) {
		param_hdr.module_id = RMS_MODULEID_APPI_PASSTHRU;
		param_hdr.instance_id = INSTANCE_ID_0;
		param_hdr.param_id = RMS_PARAM_FIRST_SAMPLE;
		param_hdr.param_size = param_size;
		rc = adm_get_pp_params(SLIMBUS_0_TX, 0, ADM_CLIENT_ID_DEFAULT,
				       NULL, &param_hdr, (u8 *) param_value);
		if (rc) {
			pr_err("%s: get parameters failed:%d\n", __func__, rc);
			kfree(param_value);
			return -EINVAL;
		}
		update_param_value = (int *)param_value;
		ucontrol->value.integer.value[0] = update_param_value[0];

		pr_debug("%s: FROM DSP value[0] 0x%x\n",
			  __func__, update_param_value[0]);
	}
	kfree(param_value);
	return 0;
}

static int msm_voc_session_id_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	voc_session_id = ucontrol->value.integer.value[0];

	pr_debug("%s: voc_session_id=%u\n", __func__, voc_session_id);

	return 0;
}

static int msm_voc_session_id_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = voc_session_id;

	return 0;
}

static struct snd_kcontrol_new msm_voc_session_controls[] = {
	SOC_SINGLE_MULTI_EXT("Voc VSID", SND_SOC_NOPM, 0,
			     0xFFFFFFFF, 0, 1, msm_voc_session_id_get,
			     msm_voc_session_id_put),
};

static int msm_sound_focus_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct sound_focus_param);

	return 0;
}

static int msm_voice_sound_focus_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct sound_focus_param soundFocusData;

	memcpy((void *)&soundFocusData, ucontrol->value.bytes.data,
		sizeof(struct sound_focus_param));
	ret = voc_set_sound_focus(soundFocusData);
	if (ret) {
		pr_err("%s: Error setting Sound Focus Params, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
	}

	return ret;
}

static int msm_voice_sound_focus_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct sound_focus_param soundFocusData;

	memset(&soundFocusData, 0, sizeof(struct sound_focus_param));

	ret = voc_get_sound_focus(&soundFocusData);
	if (ret) {
		pr_debug("%s: Error getting Sound Focus Params, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}
	memcpy(ucontrol->value.bytes.data, (void *)&soundFocusData,
		sizeof(struct sound_focus_param));

done:
	return ret;
}

static int msm_source_tracking_info(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct source_tracking_param);

	return 0;
}

static int msm_voice_source_tracking_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct source_tracking_param sourceTrackingData;

	memset(&sourceTrackingData, 0, sizeof(struct source_tracking_param));

	ret = voc_get_source_tracking(&sourceTrackingData);
	if (ret) {
		pr_debug("%s: Error getting Source Tracking Params, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}
	memcpy(ucontrol->value.bytes.data, (void *)&sourceTrackingData,
		sizeof(struct source_tracking_param));

done:
	return ret;
}

static int msm_audio_get_copp_idx_from_port_id(int port_id, int session_type,
					 int *copp_idx)
{
	int i, idx, be_idx;
	int ret = 0;
	unsigned long copp;

	pr_debug("%s: Enter, port_id=%d\n", __func__, port_id);

	ret = q6audio_validate_port(port_id);
	if (ret < 0) {
		pr_err("%s: port validation failed id 0x%x ret %d\n",
			__func__, port_id, ret);

		ret = -EINVAL;
		goto done;
	}

	for (be_idx = 0; be_idx < MSM_BACKEND_DAI_MAX; be_idx++) {
		if (msm_bedais[be_idx].port_id == port_id)
			break;
	}
	if (be_idx >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: Invalid be id %d\n", __func__, be_idx);

		ret = -EINVAL;
		goto done;
	}

	for_each_set_bit(i, &msm_bedais[be_idx].fe_sessions[0],
			 MSM_FRONTEND_DAI_MAX) {
		if (!(is_mm_lsm_fe_id(i) &&
				route_check_fe_id_adm_support(i)))
			continue;

		for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++) {
			copp = session_copp_map[i]
				[session_type][be_idx];
			if (test_bit(idx, &copp))
				break;
		}
		if (idx >= MAX_COPPS_PER_PORT)
			continue;
		else
			break;
	}
	if (i >= MSM_FRONTEND_DAI_MAX) {
		pr_debug("%s: Invalid FE, exiting\n", __func__);

		ret = -EINVAL;
		goto done;
	}
	*copp_idx = idx;
	pr_debug("%s: copp_idx=%d\n", __func__, *copp_idx);

done:
	return ret;
}

#ifdef CONFIG_SEC_SND_ADAPTATION
static bool q6audio_determine_curr_copp_idx(int stream, enum sb_type func_type)
{
	int ret = false;

	pr_info("%s: stream(%d) sb_type(%d)\n",
		__func__,
		stream, func_type);

	switch (func_type) {
	case SB_DISABLE:
		if ((stream == MSM_FRONTEND_DAI_MULTIMEDIA4) ||
		    (stream == MSM_FRONTEND_DAI_MULTIMEDIA6) ||
		    (stream == MSM_FRONTEND_DAI_MULTIMEDIA11))
			ret = true;
		break;
	case SB_ENABLE:
		if ((stream == MSM_FRONTEND_DAI_MULTIMEDIA4) ||
		    (stream == MSM_FRONTEND_DAI_MULTIMEDIA6))
			ret = true;
		break;
	case SB_RINGTONE:
		if (stream == MSM_FRONTEND_DAI_MULTIMEDIA11)
			ret = true;
		break;
	case SB_REARLEFT:
	case SB_REARRIGHT:
	case SB_FRONTLEFT:
	case SB_FRONTRIGHT:
		if (stream == MSM_FRONTEND_DAI_MULTIMEDIA1)
			ret = true;
		break;
	case SB_ROTATION:
		if ((stream == MSM_FRONTEND_DAI_MULTIMEDIA1) ||
		    (stream == MSM_FRONTEND_DAI_MULTIMEDIA4) ||
		    (stream == MSM_FRONTEND_DAI_MULTIMEDIA6))
			ret = true;
		break;
	case SB_ROTATION_LL:
		if (stream == MSM_FRONTEND_DAI_MULTIMEDIA5)
			ret = true;
		break;
	case SB_ROTATION_RINGTONE:
		if (stream == MSM_FRONTEND_DAI_MULTIMEDIA11)
			ret = true;
		break;
	case SB_FLATMOTION:
		if ((stream == MSM_FRONTEND_DAI_MULTIMEDIA1) ||
		    (stream == MSM_FRONTEND_DAI_MULTIMEDIA11))
			ret = true;
		break;
	case SB_VOLUME:
		if (stream == MSM_FRONTEND_DAI_MULTIMEDIA1)
			ret = true;
		break;
	default:
		pr_info("%s: unknown function type\n", __func__);
		break;
	}

	return ret;
}

int q6audio_get_copp_idx_from_port_id(int port_id, enum sb_type func_type,
					int *copp_idx)
{
	int i, idx, be_idx;
	int ret = 0;
	unsigned long copp;

	pr_info("%s: port_id=0x%x, func_type=%d\n",
		__func__, port_id, func_type);

	ret = q6audio_validate_port(port_id);
	if (ret < 0) {
		pr_err("%s: port validation failed id 0x%x ret %d\n",
			__func__, port_id, ret);

		ret = -EINVAL;
		goto done;
	}

	for (be_idx = 0; be_idx < MSM_BACKEND_DAI_MAX; be_idx++) {
		if (msm_bedais[be_idx].port_id == port_id)
			break;
	}
	if (be_idx >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: Invalid be id %d\n", __func__, be_idx);

		ret = -EINVAL;
		goto done;
	}

	for_each_set_bit(i, &msm_bedais[be_idx].fe_sessions[0],
			 MSM_FRONTEND_DAI_MM_SIZE) {
		for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++) {
			copp = session_copp_map[i]
				[SESSION_TYPE_RX][be_idx];
			if (test_bit(idx, &copp)) {
				if (q6audio_determine_curr_copp_idx(i, func_type))
					break;
			}
		}
		if (idx >= MAX_COPPS_PER_PORT)
			continue;
		else
			break;
	}
	if (i >= MSM_FRONTEND_DAI_MM_SIZE) {
		pr_debug("%s: Invalid FE, exiting\n", __func__);

		ret = -EINVAL;
		goto done;
	}
	*copp_idx = idx;
	pr_debug("%s: copp_idx=%d\n", __func__, *copp_idx);

done:
	return ret;
}
EXPORT_SYMBOL(q6audio_get_copp_idx_from_port_id);
#endif /* CONFIG_SEC_SND_ADAPTATION */

static int msm_audio_sound_focus_derive_port_id(struct snd_kcontrol *kcontrol,
					    const char *prefix, int *port_id)
{
	int ret = 0;

	pr_debug("%s: Enter, prefix:%s\n", __func__, prefix);

	/*
	 * Mixer control name will be like "Sound Focus Audio Tx SLIMBUS_0"
	 * where the prefix is "Sound Focus Audio Tx ". Skip the prefix
	 * and compare the string with the backend name to derive the port id.
	 */
	if (!strcmp(kcontrol->id.name + strlen(prefix),
					"SLIMBUS_0")) {
		*port_id = SLIMBUS_0_TX;
	} else if (!strcmp(kcontrol->id.name + strlen(prefix),
					"TERT_MI2S")) {
		*port_id = AFE_PORT_ID_TERTIARY_MI2S_TX;
	} else if (!strcmp(kcontrol->id.name + strlen(prefix),
					"INT3_MI2S")) {
		*port_id = AFE_PORT_ID_INT3_MI2S_TX;
	} else if (!strcmp(kcontrol->id.name + strlen(prefix),
					"VA_CDC_DMA_TX_0")) {
		*port_id = AFE_PORT_ID_VA_CODEC_DMA_TX_0;
	} else if (!strcmp(kcontrol->id.name + strlen(prefix),
					"TX_CDC_DMA_TX_3")) {
		*port_id = AFE_PORT_ID_TX_CODEC_DMA_TX_3;
	} else if (!strcmp(kcontrol->id.name + strlen(prefix),
					"QUIN_TDM_TX_0")) {
		*port_id = AFE_PORT_ID_QUINARY_TDM_TX;
	} else if (!strcmp(kcontrol->id.name + strlen(prefix),
					"PRIMARY_TDM")) {
		*port_id = AFE_PORT_ID_PRIMARY_TDM_TX;
	} else {
		pr_err("%s: mixer ctl name=%s, could not derive valid port id\n",
			__func__, kcontrol->id.name);

		ret = -EINVAL;
		goto done;
	}
	pr_debug("%s: mixer ctl name=%s, derived port_id=%d\n",
		  __func__, kcontrol->id.name, *port_id);

done:
	return ret;
}

static int msm_audio_sound_focus_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct sound_focus_param soundFocusData;
	int port_id, copp_idx;

	ret = msm_audio_sound_focus_derive_port_id(kcontrol,
				"Sound Focus Audio Tx ", &port_id);
	if (ret != 0) {
		pr_err("%s: Error in deriving port id, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}

	ret = msm_audio_get_copp_idx_from_port_id(port_id, SESSION_TYPE_TX,
					    &copp_idx);
	if (ret) {
		pr_err("%s: Could not get copp idx for port_id=%d\n",
			__func__, port_id);

		ret = -EINVAL;
		goto done;
	}

	memcpy((void *)&soundFocusData, ucontrol->value.bytes.data,
		sizeof(struct sound_focus_param));

	ret = adm_set_sound_focus(port_id, copp_idx, soundFocusData);
	if (ret) {
		pr_err("%s: Error setting Sound Focus Params, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}

done:
	return ret;
}

static int msm_audio_sound_focus_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct sound_focus_param soundFocusData;
	int port_id, copp_idx;

	ret = msm_audio_sound_focus_derive_port_id(kcontrol,
				"Sound Focus Audio Tx ", &port_id);
	if (ret) {
		pr_err("%s: Error in deriving port id, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}

	ret = msm_audio_get_copp_idx_from_port_id(port_id, SESSION_TYPE_TX,
					    &copp_idx);
	if (ret) {
		pr_debug("%s: Could not get copp idx for port_id=%d\n",
			__func__, port_id);

		ret = -EINVAL;
		goto done;
	}

	ret = adm_get_sound_focus(port_id, copp_idx, &soundFocusData);
	if (ret) {
		pr_err("%s: Error getting Sound Focus Params, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}

	memcpy(ucontrol->value.bytes.data, (void *)&soundFocusData,
		sizeof(struct sound_focus_param));

done:
	return ret;
}

static int msm_audio_source_tracking_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct source_tracking_param sourceTrackingData;
	int port_id, copp_idx;

	ret = msm_audio_sound_focus_derive_port_id(kcontrol,
				"Source Tracking Audio Tx ", &port_id);
	if (ret) {
		pr_err("%s: Error in deriving port id, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}

	ret = msm_audio_get_copp_idx_from_port_id(port_id, SESSION_TYPE_TX,
					    &copp_idx);
	if (ret) {
		pr_debug("%s: Could not get copp idx for port_id=%d\n",
			__func__, port_id);

		ret = -EINVAL;
		goto done;
	}

	ret = adm_get_source_tracking(port_id, copp_idx, &sourceTrackingData);
	if (ret) {
		pr_err("%s: Error getting Source Tracking Params, err=%d\n",
			  __func__, ret);

		ret = -EINVAL;
		goto done;
	}

	memcpy(ucontrol->value.bytes.data, (void *)&sourceTrackingData,
		sizeof(struct source_tracking_param));

done:
	return ret;
}

static int msm_doa_tracking_mon_info(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(struct doa_tracking_mon_param);

	return 0;
}

static int msm_doa_tracking_mon_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	struct doa_tracking_mon_param doa_tracking_data;
	int port_id, copp_idx;

	memset(&doa_tracking_data, 0, sizeof(struct doa_tracking_mon_param));
	ret = msm_audio_sound_focus_derive_port_id(kcontrol,
				"Doa Tracking Monitor Listen ", &port_id);
	if (ret) {
		pr_err("%s: Error in deriving port id, err=%d\n",
			  __func__, ret);
		ret = -EINVAL;
		goto done;
	}

	/*
	 * If copp id exists for given port id, query adm to get doa data.
	 * Else query afe for doa tracking params.
	 * This is to support in cases where LSM directly connects to
	 * AFE for FFNS.
	 */
	ret = msm_audio_get_copp_idx_from_port_id(port_id, SESSION_TYPE_TX,
					    &copp_idx);
	if (!ret)
		ret = adm_get_doa_tracking_mon(port_id, copp_idx,
					&doa_tracking_data);
	else
		ret = afe_get_doa_tracking_mon(port_id, &doa_tracking_data);

	if (ret) {
		pr_err("%s: Error getting Doa Tracking Params, err=%d\n",
			  __func__, ret);
		ret = -EINVAL;
		goto done;
	}

	memcpy(ucontrol->value.bytes.data, (void *)&doa_tracking_data,
		sizeof(struct doa_tracking_mon_param));
done:
	return ret;
}

static const struct snd_kcontrol_new msm_source_tracking_controls[] = {
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Voice Tx SLIMBUS_0",
		.info	= msm_sound_focus_info,
		.get	= msm_voice_sound_focus_get,
		.put	= msm_voice_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Voice Tx SLIMBUS_0",
		.info	= msm_source_tracking_info,
		.get	= msm_voice_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Audio Tx SLIMBUS_0",
		.info	= msm_sound_focus_info,
		.get	= msm_audio_sound_focus_get,
		.put	= msm_audio_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Audio Tx SLIMBUS_0",
		.info	= msm_source_tracking_info,
		.get	= msm_audio_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Voice Tx TERT_MI2S",
		.info	= msm_sound_focus_info,
		.get	= msm_voice_sound_focus_get,
		.put	= msm_voice_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Voice Tx TERT_MI2S",
		.info	= msm_source_tracking_info,
		.get	= msm_voice_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Audio Tx TERT_MI2S",
		.info	= msm_sound_focus_info,
		.get	= msm_audio_sound_focus_get,
		.put	= msm_audio_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Audio Tx TERT_MI2S",
		.info	= msm_source_tracking_info,
		.get	= msm_audio_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Voice Tx INT3_MI2S",
		.info	= msm_sound_focus_info,
		.get	= msm_voice_sound_focus_get,
		.put	= msm_voice_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Voice Tx INT3_MI2S",
		.info	= msm_source_tracking_info,
		.get	= msm_voice_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Audio Tx INT3_MI2S",
		.info	= msm_sound_focus_info,
		.get	= msm_audio_sound_focus_get,
		.put	= msm_audio_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Audio Tx INT3_MI2S",
		.info	= msm_source_tracking_info,
		.get	= msm_audio_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Voice Tx VA_CDC_DMA_TX_0",
		.info	= msm_sound_focus_info,
		.get	= msm_voice_sound_focus_get,
		.put	= msm_voice_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Voice Tx VA_CDC_DMA_TX_0",
		.info	= msm_source_tracking_info,
		.get	= msm_voice_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Audio Tx VA_CDC_DMA_TX_0",
		.info	= msm_sound_focus_info,
		.get	= msm_audio_sound_focus_get,
		.put	= msm_audio_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Audio Tx VA_CDC_DMA_TX_0",
		.info	= msm_source_tracking_info,
		.get	= msm_audio_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Voice Tx TX_CDC_DMA_TX_3",
		.info	= msm_sound_focus_info,
		.get	= msm_voice_sound_focus_get,
		.put	= msm_voice_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Voice Tx TX_CDC_DMA_TX_3",
		.info	= msm_source_tracking_info,
		.get	= msm_voice_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Audio Tx TX_CDC_DMA_TX_3",
		.info	= msm_sound_focus_info,
		.get	= msm_audio_sound_focus_get,
		.put	= msm_audio_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Audio Tx TX_CDC_DMA_TX_3",
		.info	= msm_source_tracking_info,
		.get	= msm_audio_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Voice Tx QUIN_TDM_TX_0",
		.info	= msm_sound_focus_info,
		.get	= msm_voice_sound_focus_get,
		.put	= msm_voice_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Voice Tx QUIN_TDM_TX_0",
		.info	= msm_source_tracking_info,
		.get	= msm_voice_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Sound Focus Audio Tx QUIN_TDM_TX_0",
		.info	= msm_sound_focus_info,
		.get	= msm_audio_sound_focus_get,
		.put	= msm_audio_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Source Tracking Audio Tx QUIN_TDM_TX_0",
		.info	= msm_source_tracking_info,
		.get	= msm_audio_source_tracking_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name   = "Sound Focus Audio Tx PRIMARY_TDM",
		.info   = msm_sound_focus_info,
		.get    = msm_audio_sound_focus_get,
		.put    = msm_audio_sound_focus_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name   = "Source Tracking Audio Tx PRIMARY_TDM",
		.info   = msm_source_tracking_info,
		.get    = msm_audio_source_tracking_get,
	},
};

static const struct snd_kcontrol_new msm_source_doa_tracking_controls[] = {
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Doa Tracking Monitor Listen VA_CDC_DMA_TX_0",
		.info	= msm_doa_tracking_mon_info,
		.get	= msm_doa_tracking_mon_get,
	},
};

static int spkr_prot_put_vi_port_helper(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol,
					int ch)
{
	int ret = 0;
	int item;
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;

	pr_debug("%s ch %d item is %d\n", __func__, ch,
		 ucontrol->value.enumerated.item[0]);
	mutex_lock(&routing_lock);
	item = ucontrol->value.enumerated.item[0];
	if (item < e->items) {
		pr_debug("%s RX DAI ID %d TX DAI id %d\n",
			 __func__, e->shift_l, e->values[item]);
		if (e->shift_l < MSM_BACKEND_DAI_MAX &&
		    e->values[item] < MSM_BACKEND_DAI_MAX)
			/* Enable feedback TX path */
			ret = afe_spk_prot_feed_back_cfg(
				msm_bedais[e->values[item]].port_id,
				msm_bedais[e->shift_l].port_id, 1, ch, 1);
		else {
			pr_debug("%s values are out of range item %d\n",
				 __func__, e->values[item]);
			/* Disable feedback TX path */
			if (e->values[item] == MSM_BACKEND_DAI_MAX)
				ret = afe_spk_prot_feed_back_cfg(0, 0, 0, 0, 0);
			else
				ret = -EINVAL;
		}
	} else {
		pr_err("%s item value is out of range item\n", __func__);
		ret = -EINVAL;
	}
	mutex_unlock(&routing_lock);
	return ret;
}

static int spkr_prot_put_vi_lch_port(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	return spkr_prot_put_vi_port_helper(kcontrol, ucontrol, 0);
}

static int spkr_prot_put_vi_rch_port(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	return spkr_prot_put_vi_port_helper(kcontrol, ucontrol, 1);
}

static int spkr_prot_get_vi_lch_port(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int spkr_prot_get_vi_rch_port(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s\n", __func__);
	ucontrol->value.enumerated.item[0] = 0;
	return 0;
}

#include "msm-pcm-routing-v2-tables.h"


static int msm_pcm_routing_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int be_id = rtd->dai_link->id;

	if (be_id >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: unexpected BE id %d\n", __func__, be_id);
		return -EINVAL;
	}

	mutex_lock(&routing_lock);
	msm_bedais[be_id].sample_rate = params_rate(params);
	msm_bedais[be_id].channel = params_channels(params);
	msm_bedais[be_id].format = params_format(params);
	pr_debug("%s: BE Sample Rate (%d) format (%d) BE id %d\n",
		__func__, msm_bedais[be_id].sample_rate,
		msm_bedais[be_id].format, be_id);
	mutex_unlock(&routing_lock);
	return 0;
}

static int msm_pcm_routing_close(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int be_id = rtd->dai_link->id;
	int i, session_type;
	struct msm_pcm_routing_bdai_data *bedai;
	struct msm_pcm_routing_fdai_data *fdai;

	pr_debug("%s: substream->pcm->id:%s\n",
		 __func__, substream->pcm->id);

	if (be_id >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: unexpected BE id %d\n", __func__, be_id);
		return -EINVAL;
	}

	bedai = &msm_bedais[be_id];
	session_type = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		       SESSION_TYPE_RX : SESSION_TYPE_TX;

	mutex_lock(&routing_lock);
	for_each_set_bit(i, &bedai->fe_sessions[0], MSM_FRONTEND_DAI_MAX) {
		if (!is_mm_lsm_fe_id(i))
			continue;
		fdai = &fe_dai_map[i][session_type];
		if (fdai->strm_id != INVALID_SESSION) {
			int idx;
			unsigned long copp =
				session_copp_map[i][session_type][be_id];
			for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++)
				if (test_bit(idx, &copp))
					break;

			if (idx >= MAX_COPPS_PER_PORT) {
				pr_debug("%s: copp idx is invalid, exiting\n",
					 __func__);
				continue;
			}
			fdai->be_srate = bedai->sample_rate;
			msm_pcm_routing_close_adm_copp(i, be_id, session_type,
						       idx, fdai->perf_mode,
						       fdai->passthr_mode);
		}
	}

	bedai->active = 0;
	bedai->sample_rate = 0;
	bedai->channel = 0;
	mutex_unlock(&routing_lock);

	return 0;
}

/*
 * Undo a partially completed prepare(): close every ADM COPP opened for this
 * backend and clear the active flag, so a later prepare() can retry instead of
 * silently skipping configuration. Must be called with routing_lock held.
 */
static void msm_pcm_routing_rollback_prepare(
		struct msm_pcm_routing_bdai_data *bedai, int be_id, int stream)
{
	int i, session_type;

	lockdep_assert_held(&routing_lock);

	for_each_set_bit(i, &bedai->fe_sessions[0], MSM_FRONTEND_DAI_MAX) {
		struct msm_pcm_routing_fdai_data *fdai;
		int idx;

		if (!(is_mm_lsm_fe_id(i) && route_check_fe_id_adm_support(i)))
			continue;

		session_type = (stream == SNDRV_PCM_STREAM_PLAYBACK) ?
					SESSION_TYPE_RX : SESSION_TYPE_TX;
		fdai = &fe_dai_map[i][session_type];
		if (fdai->strm_id == INVALID_SESSION)
			continue;

		for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++) {
			unsigned long copp =
				session_copp_map[i][session_type][be_id];

			if (test_bit(idx, &copp)) {
				msm_pcm_routing_close_adm_copp(i, be_id,
						session_type, idx,
						fdai->perf_mode,
						fdai->passthr_mode);
				break;
			}
		}
	}

	bedai->active = 0;
}

static int msm_pcm_routing_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int be_id = rtd->dai_link->id;
	int i, path_type, topology;
	int session_type = INVALID_SESSION;
	struct msm_pcm_routing_bdai_data *bedai;
	u32 channels = 0;
	uint16_t voc_path_type;
	struct msm_pcm_routing_fdai_data *fdai;
	u32 session_id;
	struct media_format_info voc_be_media_format;

	pr_debug("%s: substream->pcm->id:%s\n",
		 __func__, substream->pcm->id);

	if (be_id >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: unexpected BE id %d\n", __func__, be_id);
		return -EINVAL;
	}

	bedai = &msm_bedais[be_id];

	mutex_lock(&routing_lock);
	if (bedai->active == 1)
		goto done; /* Ignore prepare if back-end already active */

	/* AFE port is not active at this point. However, still
	 * go ahead setting active flag under the notion that
	 * QDSP6 is able to handle ADM starting before AFE port
	 * is started.
	 */
	bedai->active = 1;

	for_each_set_bit(i, &bedai->fe_sessions[0], MSM_FRONTEND_DAI_MAX) {
		if (!(is_mm_lsm_fe_id(i) &&
				route_check_fe_id_adm_support(i)))
			continue;

		session_type = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
						SESSION_TYPE_RX : SESSION_TYPE_TX;
		fdai = &fe_dai_map[i][session_type];
		path_type = msm_pcm_routing_calc_path_type(session_type,
							   fdai->passthr_mode);

		if (fdai->strm_id != INVALID_SESSION) {
			int copp_idx;
			int port_id = get_port_id(bedai->port_id);

			copp_idx = msm_pcm_routing_open_adm_copp(i, be_id,
					session_type, path_type, &topology,
					&channels);
			if (copp_idx < 0) {
				msm_pcm_routing_rollback_prepare(bedai, be_id,
						substream->stream);
				mutex_unlock(&routing_lock);
				return -EINVAL;
			}

			msm_pcm_routing_build_matrix(i, session_type, path_type,
						     fdai->perf_mode,
						     fdai->passthr_mode);
			if ((fdai->perf_mode == LEGACY_PCM_MODE) &&
				(fdai->passthr_mode == LEGACY_PCM))
				msm_pcm_routing_cfg_pp(port_id, copp_idx,
						       topology, channels);
		}
	}

	for_each_set_bit(i, &bedai->fe_sessions[0], MSM_FRONTEND_DAI_MAX) {
		session_id = msm_pcm_routing_get_voc_sessionid(i);
		if (session_id) {
			pr_debug("%s voice session_id: 0x%x\n", __func__,
				 session_id);

			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				voc_path_type = RX_PATH;
			else
				voc_path_type = TX_PATH;

			voc_set_route_flag(session_id, voc_path_type, 1);

			memset(&voc_be_media_format, 0,
			       sizeof(struct media_format_info));

			voc_be_media_format.port_id = bedai->port_id;
			voc_be_media_format.num_channels = bedai->channel;
			voc_be_media_format.sample_rate = bedai->sample_rate;
			voc_be_media_format.bits_per_sample = bedai->format;
			/* Defaulting this to 1 for voice call usecases */
			voc_be_media_format.channel_mapping[0] = 1;

			voc_set_device_config(session_id, voc_path_type,
					      &voc_be_media_format);

			if (voc_get_route_flag(session_id, RX_PATH) &&
			    voc_get_route_flag(session_id, TX_PATH))
				voc_enable_device(session_id);
		}
	}

	/* Check if backend is an external ec ref port and set as needed */
	if (unlikely(bedai->port_id == voc_get_ext_ec_ref_port_id())) {
		memset(&voc_be_media_format, 0,
		       sizeof(struct media_format_info));

		/* Get format info for ec ref port from msm_bedais[] */
		voc_be_media_format.port_id = bedai->port_id;
		voc_be_media_format.num_channels = bedai->channel;
		voc_be_media_format.bits_per_sample = bedai->format;
		voc_be_media_format.sample_rate = bedai->sample_rate;
		/* Defaulting this to 1 for voice call usecases */
		voc_be_media_format.channel_mapping[0] = 1;
		voc_set_ext_ec_ref_media_fmt_info(&voc_be_media_format);
		pr_debug("%s: EC Ref media format info set to port_id=%d, num_channels=%d, bits_per_sample=%d, sample_rate=%d\n",
			 __func__, voc_be_media_format.port_id,
			 voc_be_media_format.num_channels,
			 voc_be_media_format.bits_per_sample,
			 voc_be_media_format.sample_rate);
	}

done:
	mutex_unlock(&routing_lock);
	return 0;
}

static int msm_routing_send_device_pp_params(int port_id, int copp_idx,
					     int fe_id)
{
	int index, topo_id, be_idx;
	unsigned long pp_config = 0;
	bool mute_on;
	int latency, session_type;
	bool compr_passthr_mode = true;

	pr_debug("%s: port_id %d, copp_idx %d\n", __func__, port_id, copp_idx);

	if (port_id != HDMI_RX && port_id != DISPLAY_PORT_RX
			&& port_id != HDMI_RX_MS) {
		pr_err("%s: Device pp params on invalid port %d, copp_idx %d, fe_id %d\n",
			__func__, port_id, copp_idx, fe_id);
		return  -EINVAL;
	}

	for (be_idx = 0; be_idx < MSM_BACKEND_DAI_MAX; be_idx++) {
		if (port_id == msm_bedais[be_idx].port_id)
			break;
	}

	if (be_idx >= MSM_BACKEND_DAI_MAX) {
		pr_debug("%s: Invalid be id %d\n", __func__, be_idx);
		return  -EINVAL;
	}

	for (index = 0; index < MSM_BACKEND_DAI_PP_PARAMS_REQ_MAX; index++) {
		if (msm_bedais_pp_params[index].port_id == port_id)
			break;
	}
	if (index >= MSM_BACKEND_DAI_PP_PARAMS_REQ_MAX) {
		pr_err("%s: Invalid backend pp params index %d\n",
			__func__, index);
		return -EINVAL;
	}

	topo_id = adm_get_topology_for_port_copp_idx(port_id, copp_idx);
	if (topo_id != COMPRESSED_PASSTHROUGH_DEFAULT_TOPOLOGY) {
		pr_err("%s: Invalid passthrough topology 0x%x\n",
			__func__, topo_id);
		return -EINVAL;
	}

	session_type =
		(afe_get_port_type(port_id) == MSM_AFE_PORT_TYPE_RX) ?
		SESSION_TYPE_RX : SESSION_TYPE_TX;

	if ((fe_dai_map[fe_id][session_type].passthr_mode == LEGACY_PCM) ||
		(fe_dai_map[fe_id][session_type].passthr_mode == LISTEN))
		compr_passthr_mode = false;

	pp_config = msm_bedais_pp_params[index].pp_params_config;
	if (test_bit(ADM_PP_PARAM_MUTE_BIT, &pp_config)) {
		pr_debug("%s: ADM_PP_PARAM_MUTE\n", __func__);
		clear_bit(ADM_PP_PARAM_MUTE_BIT, &pp_config);
		mute_on = msm_bedais_pp_params[index].mute_on;
		if ((msm_bedais[be_idx].active) && compr_passthr_mode)
			adm_send_compressed_device_mute(port_id,
								copp_idx,
								mute_on);
	}
	if (test_bit(ADM_PP_PARAM_LATENCY_BIT, &pp_config)) {
		pr_debug("%s: ADM_PP_PARAM_LATENCY\n", __func__);
		clear_bit(ADM_PP_PARAM_LATENCY_BIT,
			  &pp_config);
		latency = msm_bedais_pp_params[index].latency;
		if ((msm_bedais[be_idx].active) && compr_passthr_mode)
			adm_send_compressed_device_latency(port_id,
							   copp_idx,
							   latency);
	}
	return 0;
}

static uint32_t msm_routing_get_topology(size_t data_size, void *data)
{
	uint32_t topology = NULL_COPP_TOPOLOGY;
	void *cal_info = NULL;
	uint32_t size = 0;

	/* Retrieve cal_info size from cal data*/
	if (data_size < sizeof(struct audio_cal_type_basic) +
			sizeof(struct audio_cal_info_adm_top)) {
		pr_err("%s: Invalid data size: %zd\n", __func__, data_size);
		goto done;
	}
	size = data_size - sizeof(struct audio_cal_type_basic);
	cal_info = kzalloc(size, GFP_KERNEL);

	if (!cal_info)
		goto done;

	memcpy(cal_info,
		((uint8_t *)data + sizeof(struct audio_cal_type_basic)), size);

	topology = ((struct audio_cal_info_adm_top *)cal_info)->topology;
	kfree(cal_info);
	cal_info = NULL;

done:
	pr_debug("%s: Using topology %d\n", __func__, topology);

	return topology;
}

static void msm_routing_load_topology(size_t data_size, void *data)
{
	uint32_t topology_id;
	int ret = -EINVAL;

	topology_id = msm_routing_get_topology(data_size, data);
	if (topology_id != NULL_COPP_TOPOLOGY)
		ret = q6core_load_unload_topo_modules(topology_id,
			CORE_LOAD_TOPOLOGY);
	if (ret < 0)
		pr_debug("%s %d load topology failed\n",
				 __func__, topology_id);

}

static void msm_routing_unload_topology(uint32_t topology_id)
{
	int ret = -EINVAL;

	if (topology_id != NULL_COPP_TOPOLOGY)
		ret = q6core_load_unload_topo_modules(topology_id,
			CORE_UNLOAD_TOPOLOGY);
	if (ret < 0)
		pr_debug("%s %d unload topology failed\n",
				 __func__, topology_id);

}

static int msm_routing_put_device_pp_params_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int pp_id = ucontrol->value.integer.value[0];
	int port_id = 0;
	int index, be_idx, i, topo_id, idx;
	bool mute;
	int latency, session_type;
	bool compr_passthr_mode = true;

	pr_debug("%s: pp_id: 0x%x\n", __func__, pp_id);

	for (be_idx = 0; be_idx < MSM_BACKEND_DAI_MAX; be_idx++) {
		port_id = msm_bedais[be_idx].port_id;
		if (port_id == HDMI_RX || port_id == DISPLAY_PORT_RX
				|| port_id == HDMI_RX_MS)
			break;
	}

	if (be_idx >= MSM_BACKEND_DAI_MAX) {
		pr_debug("%s: Invalid be id %d\n", __func__, be_idx);
		return  -EINVAL;
	}

	for (index = 0; index < MSM_BACKEND_DAI_PP_PARAMS_REQ_MAX; index++) {
		if (msm_bedais_pp_params[index].port_id == port_id)
			break;
	}
	if (index >= MSM_BACKEND_DAI_PP_PARAMS_REQ_MAX) {
		pr_err("%s: Invalid pp params backend index %d\n",
			__func__, index);
		return -EINVAL;
	}

	session_type =
		(afe_get_port_type(port_id) == MSM_AFE_PORT_TYPE_RX) ?
		SESSION_TYPE_RX : SESSION_TYPE_TX;

	for_each_set_bit(i, &msm_bedais[be_idx].fe_sessions[0],
				MSM_FRONTEND_DAI_MM_SIZE) {
		if ((fe_dai_map[i][session_type].passthr_mode == LEGACY_PCM) ||
			(fe_dai_map[i][session_type].passthr_mode == LISTEN))
			compr_passthr_mode = false;

		for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++) {
			unsigned long copp =
				session_copp_map[i]
				[SESSION_TYPE_RX][be_idx];
			if (!test_bit(idx, &copp))
				continue;
			topo_id = adm_get_topology_for_port_copp_idx(port_id,
								     idx);
			if (topo_id != COMPRESSED_PASSTHROUGH_DEFAULT_TOPOLOGY)
				continue;
		pr_debug("%s: port: 0x%x, copp %ld, be active: %d, passt: %d\n",
			 __func__, port_id, copp, msm_bedais[be_idx].active,
			 fe_dai_map[i][session_type].passthr_mode);
		switch (pp_id) {
		case ADM_PP_PARAM_MUTE_ID:
			pr_debug("%s: ADM_PP_PARAM_MUTE\n", __func__);
			mute = ucontrol->value.integer.value[1] ? true : false;
			msm_bedais_pp_params[index].mute_on = mute;
			set_bit(ADM_PP_PARAM_MUTE_BIT,
				&msm_bedais_pp_params[index].pp_params_config);
			if ((msm_bedais[be_idx].active) && compr_passthr_mode)
				adm_send_compressed_device_mute(port_id,
					idx, mute);
			break;
		case ADM_PP_PARAM_LATENCY_ID:
			pr_debug("%s: ADM_PP_PARAM_LATENCY\n", __func__);
			msm_bedais_pp_params[index].latency =
				ucontrol->value.integer.value[1];
			set_bit(ADM_PP_PARAM_LATENCY_BIT,
				&msm_bedais_pp_params[index].pp_params_config);
			latency = msm_bedais_pp_params[index].latency =
				ucontrol->value.integer.value[1];
			if ((msm_bedais[be_idx].active) && compr_passthr_mode)
				adm_send_compressed_device_latency(port_id,
					idx, latency);
			break;
		default:
			pr_info("%s, device pp param %d not supported\n",
				__func__, pp_id);
			break;
		}
		}
	}
	return 0;
}

static int msm_routing_get_device_pp_params_mixer(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s:msm_routing_get_device_pp_params_mixer", __func__);
	return 0;
}

static const struct snd_kcontrol_new device_pp_params_mixer_controls[] = {
	SOC_SINGLE_MULTI_EXT("Device PP Params", SND_SOC_NOPM, 0, 0xFFFFFFFF,
	0, 3, msm_routing_get_device_pp_params_mixer,
	msm_routing_put_device_pp_params_mixer),
};

static int msm_aptx_dec_license_control_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] =
			core_get_license_status(ASM_MEDIA_FMT_APTX);
	pr_debug("%s: status %ld\n", __func__,
			ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_aptx_dec_license_control_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int32_t status = 0;

	status = core_set_license(ucontrol->value.integer.value[0],
				APTX_CLASSIC_DEC_LICENSE_ID);
	pr_debug("%s: status %d\n", __func__, status);
	return status;
}

static const struct snd_kcontrol_new aptx_dec_license_controls[] = {
	SOC_SINGLE_EXT("APTX Dec License", SND_SOC_NOPM, 0,
	0xFFFF, 0, msm_aptx_dec_license_control_get,
	msm_aptx_dec_license_control_put),
};

static int msm_routing_get_pll_clk_drift(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int msm_routing_put_pll_clk_drift(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	u16 port_id = 0;
	int32_t clk_drift = 0;
	uint32_t clk_reset = 0;
	int be_idx, ret = -EINVAL;

	be_idx = ucontrol->value.integer.value[0];
	clk_drift = ucontrol->value.integer.value[1];
	clk_reset = ucontrol->value.integer.value[2];

	if (be_idx < 0 || be_idx >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: Invalid be id %d\n", __func__, be_idx);
		return -EINVAL;
	}

	if (!msm_bedais[be_idx].active && !clk_reset) {
		pr_err("%s:BE is not active %d, cannot set clock drift\n",
			__func__, be_idx);
		return -EINVAL;
	}

	port_id = msm_bedais[be_idx].port_id;
	pr_debug("%s: clk drift %d be idx %d clk reset %d port id 0x%x\n",
		  __func__, clk_drift, be_idx, clk_reset, port_id);
	ret = afe_set_pll_clk_drift(port_id, clk_drift, clk_reset);
	if (ret < 0)
		pr_err("%s: failed to set pll clk drift\n", __func__);

	return ret;
}

static const struct snd_kcontrol_new pll_clk_drift_controls[] = {
	SOC_SINGLE_MULTI_EXT("PLL config data", SND_SOC_NOPM, 0, 0xFFFFFFFF,
	0, 128, msm_routing_get_pll_clk_drift, msm_routing_put_pll_clk_drift),
};

static int msm_routing_put_port_chmap_mixer(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	uint8_t channel_map[PCM_FORMAT_MAX_NUM_CHANNEL_V8];
	uint32_t be_idx = ucontrol->value.integer.value[0];
	int i;

	if (be_idx >= MSM_BACKEND_DAI_MAX) {
		pr_err("%s: Invalid Backend index %d\n",  __func__, be_idx);
		return -EINVAL;
	}

	for (i = 0; i < PCM_FORMAT_MAX_NUM_CHANNEL_V8; i++) {
		channel_map[i] = (char)(ucontrol->value.integer.value[i + 1]);
		if (channel_map[i] > PCM_MAX_CHMAP_ID) {
			pr_err("%s: Invalid channel map %d\n",
				__func__, channel_map[i]);
			return -EINVAL;
		}
	}
	adm_set_port_multi_ch_map(channel_map, msm_bedais[be_idx].port_id);

	return 0;
}

static const struct snd_kcontrol_new port_multi_channel_map_mixer_controls[] = {
	SOC_SINGLE_MULTI_EXT("Backend Device Channel Map", SND_SOC_NOPM, 0,
			MSM_BACKEND_DAI_MAX, 0,
			PCM_FORMAT_MAX_NUM_CHANNEL_V8 + 1, NULL,
			msm_routing_put_port_chmap_mixer),
};

static int msm_routing_be_dai_name_table_info(struct snd_kcontrol *kcontrol,
					      struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(be_dai_name_table);
	return 0;
}

static int msm_routing_be_dai_name_table_tlv_get(struct snd_kcontrol *kcontrol,
						 unsigned int __user *bytes,
						 unsigned int size)
{
	int i;
	int ret = 0;

	if (size < sizeof(be_dai_name_table)) {
		pr_err("%s: invalid size %d requested, returning\n",
			__func__, size);
		ret = -EINVAL;
		goto done;
	}

	/*
	 * Fill be_dai_name_table from msm_bedais table to reduce code changes
	 * needed when adding new backends
	 */
	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		be_dai_name_table[i].be_id = i;
		strlcpy(be_dai_name_table[i].be_name,
			msm_bedais[i].name,
			LPASS_BE_NAME_MAX_LENGTH);
	}

	ret = copy_to_user(bytes, &be_dai_name_table,
			   sizeof(be_dai_name_table));
	if (ret) {
		pr_err("%s: failed to copy be_dai_name_table\n", __func__);
		ret = -EFAULT;
	}

done:
	return ret;
}

static const struct snd_kcontrol_new
	msm_routing_be_dai_name_table_mixer_controls[] = {
	{
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			  SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.info = msm_routing_be_dai_name_table_info,
		.name = "Backend DAI Name Table",
		.tlv.c = snd_soc_bytes_tlv_callback,
		.private_value = (unsigned long) &(struct soc_bytes_ext) {
			.max = sizeof(be_dai_name_table),
			.get = msm_routing_be_dai_name_table_tlv_get,
		}
	},
};

static int msm_routing_stereo_channel_reverse_control_get(
			struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = swap_ch;
	pr_debug("%s: Swap channel value: %ld\n", __func__,
				ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_routing_stereo_channel_reverse_control_put(
			struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int i, idx, be_index, port_id;
	int ret = 0;
	unsigned long copp;

	pr_debug("%s Swap channel value:%ld\n", __func__,
				ucontrol->value.integer.value[0]);

	swap_ch = ucontrol->value.integer.value[0];

	mutex_lock(&routing_lock);
	for (be_index = 0; be_index < MSM_BACKEND_DAI_MAX; be_index++) {
		port_id = msm_bedais[be_index].port_id;
		if (!msm_bedais[be_index].active)
			continue;

		for_each_set_bit(i, &msm_bedais[be_index].fe_sessions[0],
				MSM_FRONTEND_DAI_MM_SIZE) {
			copp = session_copp_map[i][SESSION_TYPE_RX][be_index];
			for (idx = 0; idx < MAX_COPPS_PER_PORT; idx++) {
				if (!test_bit(idx, &copp))
					continue;

				pr_debug("%s: swap channel control of portid:%d, coppid:%d\n",
					 __func__, port_id, idx);
				ret = adm_swap_speaker_channels(
					port_id, idx,
					msm_bedais[be_index].sample_rate,
					swap_ch);
				if (ret) {
					pr_err("%s:Swap_channel failed, err=%d\n",
						 __func__, ret);
					goto done;
				}
			}
		}
	}
done:
	mutex_unlock(&routing_lock);
	return ret;
}

static const struct snd_kcontrol_new stereo_channel_reverse_control[] = {
	SOC_SINGLE_EXT("Swap channel", SND_SOC_NOPM, 0,
	1, 0, msm_routing_stereo_channel_reverse_control_get,
	msm_routing_stereo_channel_reverse_control_put),
};

static int msm_routing_instance_id_support_info(struct snd_kcontrol *kcontrol,
						struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	return 0;
}

static int msm_routing_instance_id_support_put(
	struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	bool supported = ucontrol->value.integer.value[0] ? true : false;

	q6common_update_instance_id_support(supported);
	return 0;
}

static int msm_routing_instance_id_support_get(
	struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	bool supported = false;

	supported = q6common_is_instance_id_supported();
	ucontrol->value.integer.value[0] = supported ? 1 : 0;

	return 0;
}

static const struct snd_kcontrol_new
	msm_routing_feature_support_mixer_controls[] = {
		{
			.access = SNDRV_CTL_ELEM_ACCESS_READ |
				SNDRV_CTL_ELEM_ACCESS_WRITE,
			.info = msm_routing_instance_id_support_info,
			.name = "Instance ID Support",
			.put = msm_routing_instance_id_support_put,
			.get = msm_routing_instance_id_support_get,
		},
};

static const struct snd_pcm_ops msm_routing_pcm_ops = {
	.hw_params	= msm_pcm_routing_hw_params,
	.close          = msm_pcm_routing_close,
	.prepare        = msm_pcm_routing_prepare,
};

#ifdef CONFIG_DOA_PARAMS_ENABLED
void msm_routing_add_doa_control(struct snd_soc_component *component)
{
	snd_soc_add_component_controls(component,
				msm_source_doa_tracking_controls,
				ARRAY_SIZE(msm_source_doa_tracking_controls));
}
#else
void msm_routing_add_doa_control(struct snd_soc_component *component)
{
	return;
}
#endif

#ifndef CONFIG_TDM_DISABLE
static void snd_soc_dapm_new_controls_tdm(struct snd_soc_component *component)
{
        snd_soc_dapm_new_controls(&component->dapm,
                                msm_qdsp6_widgets_tdm,
                                ARRAY_SIZE(msm_qdsp6_widgets_tdm));
}
static void snd_soc_dapm_add_routes_tdm(struct snd_soc_component *component)
{
        snd_soc_dapm_add_routes(&component->dapm, intercon_tdm,
                ARRAY_SIZE(intercon_tdm));
}
#else
static void snd_soc_dapm_new_controls_tdm(struct snd_soc_component *component)
{
        return;
}

static void snd_soc_dapm_add_routes_tdm(struct snd_soc_component *component)
{
        return;
}
#endif


#ifndef CONFIG_MI2S_DISABLE
static void snd_soc_dapm_new_controls_mi2s(struct snd_soc_component *component)
{
        snd_soc_dapm_new_controls(&component->dapm,
                                msm_qdsp6_widgets_mi2s,
                                ARRAY_SIZE(msm_qdsp6_widgets_mi2s));
}
static void snd_soc_dapm_add_routes_mi2s(struct snd_soc_component *component)
{
        snd_soc_dapm_add_routes(&component->dapm, intercon_mi2s,
                ARRAY_SIZE(intercon_mi2s));
}
#else
static void snd_soc_dapm_new_controls_mi2s(struct snd_soc_component *component)
{
        return;
}
static void snd_soc_dapm_add_routes_mi2s(struct snd_soc_component *component)
{
        return;
}
#endif

#ifndef CONFIG_AUXPCM_DISABLE
static void snd_soc_dapm_new_controls_aux_pcm(struct snd_soc_component *component)
{
        snd_soc_dapm_new_controls(&component->dapm,
                                msm_qdsp6_widgets_aux_pcm,
                                ARRAY_SIZE(msm_qdsp6_widgets_aux_pcm));
}
static void snd_soc_dapm_add_routes_aux_pcm(struct snd_soc_component *component)
{
        snd_soc_dapm_add_routes(&component->dapm, intercon_aux_pcm,
                ARRAY_SIZE(intercon_aux_pcm));
}
#else
static void snd_soc_dapm_new_controls_aux_pcm(struct snd_soc_component *component)
{
        return;
}
static void snd_soc_dapm_add_routes_aux_pcm(struct snd_soc_component *component)
{
        return;
}
#endif

/* Not used but frame seems to require it */
static int msm_routing_probe(struct snd_soc_component *component)
{
	snd_soc_dapm_new_controls(&component->dapm, msm_qdsp6_widgets,
			   ARRAY_SIZE(msm_qdsp6_widgets));

	snd_soc_dapm_new_controls_tdm(component);
	snd_soc_dapm_new_controls_mi2s(component);
	snd_soc_dapm_new_controls_aux_pcm(component);

	snd_soc_dapm_ignore_suspend(&component->dapm, "BE_OUT");
	snd_soc_dapm_ignore_suspend(&component->dapm, "BE_IN");
	snd_soc_dapm_add_routes(&component->dapm, intercon,
		ARRAY_SIZE(intercon));

	snd_soc_dapm_add_routes_tdm(component);
	snd_soc_dapm_add_routes_mi2s(component);
	snd_soc_dapm_add_routes_aux_pcm(component);

	snd_soc_dapm_new_widgets(component->dapm.card);

	snd_soc_add_component_controls(component, lsm_controls,
				      ARRAY_SIZE(lsm_controls));

	snd_soc_add_component_controls(component, aanc_slim_0_rx_mux,
				      ARRAY_SIZE(aanc_slim_0_rx_mux));

	snd_soc_add_component_controls(component, aanc_noise_level,
				      ARRAY_SIZE(aanc_noise_level));

	snd_soc_add_component_controls(component, msm_voc_session_controls,
				      ARRAY_SIZE(msm_voc_session_controls));

	snd_soc_add_component_controls(component, app_type_cfg_controls,
				      ARRAY_SIZE(app_type_cfg_controls));

	snd_soc_add_component_controls(component, lsm_app_type_cfg_controls,
				      ARRAY_SIZE(lsm_app_type_cfg_controls));

	snd_soc_add_component_controls(component, module_cfg_controls,
				      ARRAY_SIZE(module_cfg_controls));

	snd_soc_add_component_controls(component,
				stereo_to_custom_stereo_controls,
			ARRAY_SIZE(stereo_to_custom_stereo_controls));

	snd_soc_add_component_controls(component, ec_ref_param_controls,
				ARRAY_SIZE(ec_ref_param_controls));

	snd_soc_add_component_controls(component, channel_mixer_controls,
				ARRAY_SIZE(channel_mixer_controls));

	msm_qti_pp_add_controls(component);

	msm_dts_srs_tm_add_controls(component);

	msm_dolby_dap_add_controls(component);

	snd_soc_add_component_controls(component,
			use_ds1_or_ds2_controls,
			ARRAY_SIZE(use_ds1_or_ds2_controls));

	snd_soc_add_component_controls(component,
			hifi_filter_controls,
			ARRAY_SIZE(hifi_filter_controls));

	snd_soc_add_component_controls(component,
			use_ffecns_freeze_event_controls,
			ARRAY_SIZE(use_ffecns_freeze_event_controls));

	snd_soc_add_component_controls(component,
				device_pp_params_mixer_controls,
				ARRAY_SIZE(device_pp_params_mixer_controls));

	snd_soc_add_component_controls(component,
		msm_routing_be_dai_name_table_mixer_controls,
		ARRAY_SIZE(msm_routing_be_dai_name_table_mixer_controls));

	/* Add doa control based on config */
	msm_routing_add_doa_control(component);
	snd_soc_add_component_controls(component, msm_source_tracking_controls,
				ARRAY_SIZE(msm_source_tracking_controls));
	snd_soc_add_component_controls(component, adm_channel_config_controls,
				ARRAY_SIZE(adm_channel_config_controls));

	snd_soc_add_component_controls(component, aptx_dec_license_controls,
					ARRAY_SIZE(aptx_dec_license_controls));
	snd_soc_add_component_controls(component,
				stereo_channel_reverse_control,
				ARRAY_SIZE(stereo_channel_reverse_control));
	snd_soc_add_component_controls(
			component, msm_routing_feature_support_mixer_controls,
			ARRAY_SIZE(msm_routing_feature_support_mixer_controls));
	snd_soc_add_component_controls(component,
			port_multi_channel_map_mixer_controls,
			ARRAY_SIZE(port_multi_channel_map_mixer_controls));

	snd_soc_add_component_controls(component, pll_clk_drift_controls,
				      ARRAY_SIZE(pll_clk_drift_controls));

	return 0;
}

int msm_routing_pcm_new(struct snd_soc_pcm_runtime *runtime)
{
	return msm_pcm_routing_hwdep_new(runtime, msm_bedais);
}

void msm_routing_pcm_free(struct snd_pcm *pcm)
{
	msm_pcm_routing_hwdep_free(pcm);
}

static struct snd_soc_component_driver msm_soc_routing_component = {
	.name		= DRV_NAME,
	.ops		= &msm_routing_pcm_ops,
	.probe		= msm_routing_probe,
	.pcm_new	= msm_routing_pcm_new,
	.pcm_free	= msm_routing_pcm_free,
};

static int msm_routing_pcm_probe(struct platform_device *pdev)
{

	dev_dbg(&pdev->dev, "dev name %s\n", dev_name(&pdev->dev));
	return snd_soc_register_component(&pdev->dev,
				&msm_soc_routing_component,
				NULL, 0);
}

static int msm_routing_pcm_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static const struct of_device_id msm_pcm_routing_dt_match[] = {
	{.compatible = "qcom,msm-pcm-routing"},
	{}
};
MODULE_DEVICE_TABLE(of, msm_pcm_routing_dt_match);

static struct platform_driver msm_routing_pcm_driver = {
	.driver = {
		.name = "msm-pcm-routing",
		.owner = THIS_MODULE,
		.of_match_table = msm_pcm_routing_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_routing_pcm_probe,
	.remove = msm_routing_pcm_remove,
};

int msm_routing_check_backend_enabled(int fedai_id)
{
	int i;

	if (fedai_id > MSM_FRONTEND_DAI_MM_MAX_ID) {
		/* bad ID assigned in machine driver */
		pr_err("%s: bad MM ID\n", __func__);
		return 0;
	}
	for (i = 0; i < MSM_BACKEND_DAI_MAX; i++) {
		if (test_bit(fedai_id, &msm_bedais[i].fe_sessions[0]))
			return msm_bedais[i].active;
	}
	return 0;
}

static int get_cal_type_index(int32_t cal_type)
{
	int ret = -EINVAL;

	switch (cal_type) {
	case ADM_TOPOLOGY_CAL_TYPE:
		ret = ADM_TOPOLOGY_CAL_TYPE_IDX;
		break;
	case ADM_LSM_TOPOLOGY_CAL_TYPE:
		ret = ADM_LSM_TOPOLOGY_CAL_TYPE_IDX;
		break;
	default:
		pr_err("%s: Invalid cal type %d\n", __func__, cal_type);
	}
	return ret;
}

static int msm_routing_set_cal(int32_t cal_type,
					size_t data_size, void *data)
{
	int ret = 0;
	int cal_index;
	pr_debug("%s\n", __func__);

	cal_index = get_cal_type_index(cal_type);
	if (cal_index < 0) {
		pr_err("%s: Could not get cal index %d\n",
			__func__, cal_index);
		ret = -EINVAL;
		goto done;
	}

	ret = cal_utils_set_cal(data_size, data, cal_data[cal_index], 0, NULL);
	if (ret < 0) {
		pr_err("%s: cal_utils_set_cal failed, ret = %d, cal type = %d!\n",
			__func__, ret, cal_type);
		ret = -EINVAL;
		goto done;
	}
	/* Pre-load if it is ADM topology */
	if ((cal_index == ADM_TOPOLOGY_CAL_TYPE_IDX) ||
		(cal_index == ADM_LSM_TOPOLOGY_CAL_TYPE_IDX)) {
		msm_routing_load_topology(data_size, data);
	}
done:
	return ret;
}

static void msm_routing_delete_cal_data(void)
{
	pr_debug("%s\n", __func__);

	cal_utils_destroy_cal_types(MAX_ROUTING_CAL_TYPES, &cal_data[0]);
}

static int msm_routing_init_cal_data(void)
{
	int ret = 0;
	struct cal_type_info cal_type_info[] = {
		{{ADM_TOPOLOGY_CAL_TYPE,
		{NULL, NULL, NULL,
		msm_routing_set_cal, NULL, NULL} },
		{NULL, NULL, cal_utils_match_buf_num} },

		{{ADM_LSM_TOPOLOGY_CAL_TYPE,
		{NULL, NULL, NULL,
		msm_routing_set_cal, NULL, NULL} },
		{NULL, NULL, cal_utils_match_buf_num} },
	};
	pr_debug("%s\n", __func__);

	ret = cal_utils_create_cal_types(MAX_ROUTING_CAL_TYPES, &cal_data[0],
		&cal_type_info[0]);
	if (ret < 0) {
		pr_err("%s: could not create cal type!\n",
			__func__);
		ret = -EINVAL;
		goto err;
	}

	return ret;
err:
	msm_routing_delete_cal_data();
	return ret;
}

int __init msm_soc_routing_platform_init(void)
{
	mutex_init(&routing_lock);
	if (msm_routing_init_cal_data())
		pr_err("%s: could not init cal data!\n", __func__);

	afe_set_routing_callback(
		(routing_cb)msm_pcm_get_dev_acdb_id_by_port_id);

	memset(&be_dai_name_table, 0, sizeof(be_dai_name_table));
	memset(&last_be_id_configured, 0, sizeof(last_be_id_configured));

	return platform_driver_register(&msm_routing_pcm_driver);
}

void msm_soc_routing_platform_exit(void)
{
	msm_routing_delete_cal_data();
	memset(&be_dai_name_table, 0, sizeof(be_dai_name_table));
	mutex_destroy(&routing_lock);
	platform_driver_unregister(&msm_routing_pcm_driver);
}

MODULE_DESCRIPTION("MSM routing platform driver");
MODULE_LICENSE("GPL v2");
