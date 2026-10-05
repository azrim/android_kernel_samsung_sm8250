/*
 *  sec_battery.c
 *  Samsung Mobile Battery Driver
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"
#include "include/sec_battery_sysfs.h"
#include "include/sec_battery_dt.h"

#include <linux/sec_param.h>
#include <linux/sec_debug.h>

#if defined(CONFIG_CCIC_MAX77705)
#include <linux/ccic/max77705_usbc.h>
#include <linux/ccic/ccic_core.h>
#endif

#ifdef CONFIG_SAMSUNG_BATTERY_DISALLOW_DEEP_SLEEP
#include <linux/clk.h>
struct clk *xo_chr;
#endif

#if defined(CONFIG_SEC_ABC)
#include <linux/sti/abc_common.h>
#endif

#if defined(CONFIG_KUNIT)
#define __visible_for_testing
#else
#define __visible_for_testing static
#endif

static enum power_supply_property sec_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
#if defined(CONFIG_FUELGAUGE_MAX77705)
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
#endif
#if defined(CONFIG_CALC_TIME_TO_FULL)
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
#endif
	POWER_SUPPLY_PROP_CHARGE_COUNTER_SHADOW,
	POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL,
	POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static enum power_supply_property sec_power_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
#if defined(CONFIG_CCIC_MAX77705)
	POWER_SUPPLY_PROP_MOISTURE_DETECTED,
#endif
};

static enum power_supply_property sec_wireless_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static enum power_supply_property sec_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static enum power_supply_property sec_ps_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ONLINE,
};

static char *supply_list[] = {
	"battery",
};

char *sec_cable_type[SEC_BATTERY_CABLE_MAX] = {
	"UNKNOWN",                 /* 0 */
	"NONE",                    /* 1 */
	"PREAPARE_TA",             /* 2 */
	"TA",                      /* 3 */
	"USB",                     /* 4 */
	"USB_CDP",                 /* 5 */
	"9V_TA",                   /* 6 */
	"9V_ERR",                  /* 7 */
	"9V_UNKNOWN",              /* 8 */
	"12V_TA",                  /* 9 */
	"WC",                /* 10 */
	"HV_WC",			/* 11 */
	"PMA_WC",            /* 12 */
	"WC_PACK",           /* 13 */
	"WC_HV_PACK",        /* 14 */
	"WC_STAND",          /* 15 */
	"WC_HV_STAND",       /* 16 */
	"OC20",                    /* 17 */
	"QC30",                    /* 18 */
	"PDIC",                    /* 19 */
	"UARTOFF",                 /* 20 */
	"OTG",                     /* 21 */
	"LAN_HUB",                 /* 22 */
	"POWER_SHARGING",          /* 23 */
	"HMT_CONNECTED",           /* 24 */
	"HMT_CHARGE",              /* 25 */
	"HV_TA_CHG_LIMIT",          /* 26 */
	"WC_VEHICLE",          /* 27 */
	"WC_HV_VEHICLE",	   /* 28 */
	"WC_HV_PREPARE",	   /* 29 */
	"TIMEOUT",                 /* 30 */
	"SMART_OTG",               /* 31 */
	"SMART_NOTG",              /* 32 */
	"WC_TX",              /* 33 */
	"HV_WC_20",           /* 34 */
	"HV_WC_20_LIMIT",     /* 35 */
	"WC_FAKE",			/* 36 */
	"HV_WC_20_PREPARE",   /* 37 */
	"PDIC_APDO",                /* 38 */
	"POGO",                    /* 39 */
};

char *sec_bat_charging_mode_str[] = {
	"None",
	"Normal",
	"Additional",
	"Re-Charging",
	"ABS"
};

char *sec_bat_status_str[] = {
	"Unknown",
	"Charging",
	"Discharging",
	"Not-charging",
	"Full"
};

char *sec_bat_health_str[] = {
	"Unknown",
	"Good",
	"Overheat",
	"Dead",
	"OverVoltage",
	"UnspecFailure",
	"Cold",
	"WatchdogTimerExpire",
	"SafetyTimerExpire",
	"Warm",
	"Cool",
	"Hot",
	"UnderVoltage",
	"OverheatLimit",
	"VsysOVP",
	"VbatOVP",
#if defined(CONFIG_DIRECT_CHARGING)
	"DCErr",
#endif
};

char *sec_bat_charge_mode_str[] = {
	"Charging-On",
	"Charging-Off",
	"Buck-Off",
};

char *sec_bat_rx_type_str[] = {
	"No Dev",
	"Other DEV",
	"SS Gear",
	"SS Phone",
	"SS Buds",
};

char *vout_control_mode_str[] = {
	"Set VOUT Off",
	"Set VOUT NV",
	"Set Vout Rsv",
	"Set Vout HV",
	"Set Vout CV",
	"Set Vout Call",
	"Set Vout 5V",
	"Set Vout 9V",
	"Set Vout 10V",
	"Set Vout 11V",
	"Set Vout 12V",
	"Set Vout 12.5V",
	"Set Vout 5V Step",
	"Set Vout 5.5V Step",
	"Set Vout 9V Step",
	"Set Vout 10V Step",
};

char *swelling_mode_str[] = {
	"Swelling None",
	"Swelling Charging",
	"Swelling Full",
};

bool sleep_mode;
int fg_reset;
bool batt_boot_complete;
//int is_debug_level_low;
unsigned int lpcharge;
EXPORT_SYMBOL(lpcharge);
#if defined(CONFIG_SEC_FACTORY)
int factory_mode;
EXPORT_SYMBOL(factory_mode);
#endif
#if defined(CONFIG_PREVENT_USB_CONN_OVERHEAT)
extern int muic_set_hiccup_mode(int on_off);
extern void pdic_manual_ccopen_request(int is_on);
#endif
bool mfc_fw_update;
EXPORT_SYMBOL(mfc_fw_update);
bool boot_complete;
EXPORT_SYMBOL(boot_complete);
int charging_night_mode;
static int pd_hv_disable;
int temp_control_test;

extern int muic_afc_set_voltage(int vol);
extern int muic_hv_charger_disable(bool en);

static int sec_bat_is_lpm_check(char *str)
{
	lpcharge = (strncmp(str, "charger", 7) == 0);
	pr_info("%s: Low power charging mode: %d\n", __func__, lpcharge);

	return lpcharge;
}
__setup("androidboot.mode=", sec_bat_is_lpm_check);

static int __init charging_mode(char *str)
{
	int mode;

	/*
	 * charging_mode packs the night-mode flag in the low byte and the
	 * temperature-control test flag in the second byte.
	 */
	if (get_option(&str, &mode)) {
		charging_night_mode = mode & 0x000000FF;
		pr_err("charging_night_mode : 0x%x(%d)\n",
			charging_night_mode, charging_night_mode);

		temp_control_test = (mode & 0x00FF0000) >> 16;
		pr_err("temp_control_test : 0x%x(%d)\n",
			temp_control_test, temp_control_test);

		return 0;
	}

	pr_err("%s() : %d\n", __func__, -EINVAL);

	return -EINVAL;
}
early_param("charging_mode", charging_mode);

static int __init pd_disable(char *str)
{
	get_option(&str, &pd_hv_disable);
	pr_info("%s: pd_hv_disable is 0x%02x\n", __func__, pd_hv_disable);

	return 0;
}
early_param("pd_disable", pd_disable);

int get_pd_disable(void)
{
	/* 0x31 is the ASCII character '1' */
	return (pd_hv_disable == 0x31);
}

static int sec_bat_get_fg_reset(char *val)
{
	fg_reset = (strncmp(val, "1", 1) == 0);
	pr_info("%s, fg_reset:%d\n", __func__, fg_reset);
	return 1;
}
__setup("fg_reset=", sec_bat_get_fg_reset);

#if 0
static int sec_bat_get_debug_level(char *val)
{
	is_debug_level_low = strncmp(val, "0x4f4c", 6) ? 0 : 1;
	pr_info("%s, is_debug_level_low:%d(%s)\n", __func__, is_debug_level_low, val);
	return 1;
}
__setup("androidboot.debug_level=", sec_bat_get_debug_level);
#endif

#if defined(CONFIG_SEC_FACTORY)
static int sec_bat_get_factory_mode(char *val)
{
	factory_mode = (strncmp(val, "1", 1) == 0);
	pr_info("%s, factory_mode:%d\n", __func__, factory_mode);
	return 1;
}
__setup("factory_mode=", sec_bat_get_factory_mode);
#endif

#if defined(CONFIG_WIRELESS_IC_PARAM)
unsigned int wireless_fw_ver_param;
EXPORT_SYMBOL(wireless_fw_ver_param);

unsigned int wireless_chip_id_param;
EXPORT_SYMBOL(wireless_chip_id_param);

unsigned int wireless_fw_mode_param;
EXPORT_SYMBOL(wireless_fw_mode_param);
static int __init sec_bat_get_wireless_ic(char *str)
{
	int ic_info;

	/*
	 * wireless_ic packs the chip id, firmware version and firmware mode
	 * into a single cmdline value.
	 */
	if (get_option(&str, &ic_info)) {
		wireless_chip_id_param = (ic_info & 0xFF000000) >> 24;
		wireless_fw_ver_param = (ic_info & 0x00FFFF00) >> 8;
		wireless_fw_mode_param = (ic_info & 0x000000F0) >> 4;

		pr_err("wireless_ic() : ic_info(0x%08X), chip_id(0x%02X), "
			"fw_ver(0x%04X), fw_mode(0x%01X)\n", ic_info,
			wireless_chip_id_param, wireless_fw_ver_param, wireless_fw_mode_param);

		return 0;
	}

	pr_err("wireless_ic() : %d\n", -EINVAL);

	return -EINVAL;
}
early_param("wireless_ic", sec_bat_get_wireless_ic);
#endif









static bool sec_bat_check_by_psy(struct sec_battery_info *battery)
{
	char *psy_name = NULL;
	union power_supply_propval value = {0, };

	switch (battery->pdata->battery_check_type) {
	case SEC_BATTERY_CHECK_PMIC:
		psy_name = battery->pdata->pmic_name;
		break;
	case SEC_BATTERY_CHECK_FUELGAUGE:
		psy_name = battery->pdata->fuelgauge_name;
		break;
	case SEC_BATTERY_CHECK_CHARGER:
		psy_name = battery->pdata->charger_name;
		break;
	default:
		dev_err(battery->dev,
			"%s: Invalid Battery Check Type\n", __func__);
		return false;
	}

	psy_do_property(psy_name, get,
		POWER_SUPPLY_PROP_PRESENT, value);

	return (bool)value.intval;
}

#if defined(CONFIG_DUAL_BATTERY)
static bool sec_bat_check_by_gpio(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	bool ret = true;
	int main_det = -1, sub_det = -1;

	value.intval = SEC_DUAL_BATTERY_MAIN;
	psy_do_property(battery->pdata->dual_battery_name, get,
		POWER_SUPPLY_EXT_PROP_DUAL_BAT_DET, value);
	main_det = value.intval;

	value.intval = SEC_DUAL_BATTERY_SUB;
	psy_do_property(battery->pdata->dual_battery_name, get,
		POWER_SUPPLY_EXT_PROP_DUAL_BAT_DET, value);
	sub_det = value.intval;

	ret = (bool)(main_det & sub_det);
	if (!ret)
		pr_info("%s : main det = %d, sub det = %d \n", __func__, main_det, sub_det);

	return ret;
}
#endif

bool sec_bat_check(struct sec_battery_info *battery)
{
	bool ret = true;

	if (battery->factory_mode || battery->is_jig_on) {
		dev_dbg(battery->dev, "%s: No need to check in factory mode\n",
			__func__);
		return ret;
	}

	if (battery->health != POWER_SUPPLY_HEALTH_GOOD &&
		battery->health != POWER_SUPPLY_HEALTH_UNSPEC_FAILURE) {
		dev_dbg(battery->dev, "%s: No need to check\n", __func__);
		return ret;
	}

	switch (battery->pdata->battery_check_type) {
	case SEC_BATTERY_CHECK_ADC:
		if (is_nocharge_type(battery->cable_type))
			ret = battery->present;
		else
			ret = sec_bat_check_vf_adc(battery);
		break;
	case SEC_BATTERY_CHECK_INT:
	case SEC_BATTERY_CHECK_CALLBACK:
		if (is_nocharge_type(battery->cable_type)) {
			ret = battery->present;
		} else {
			if (battery->pdata->check_battery_callback)
				ret = battery->pdata->check_battery_callback();
		}
		break;
	case SEC_BATTERY_CHECK_PMIC:
	case SEC_BATTERY_CHECK_FUELGAUGE:
	case SEC_BATTERY_CHECK_CHARGER:
		ret = sec_bat_check_by_psy(battery);
		break;
#if defined(CONFIG_DUAL_BATTERY)
	case SEC_BATTERY_CHECK_DUAL_BAT_GPIO:
		ret = sec_bat_check_by_gpio(battery);
		break;
#endif
	case SEC_BATTERY_CHECK_NONE:
		dev_dbg(battery->dev, "%s: No Check\n", __func__);
		/* fall through */
	default:
		break;
	}

	return ret;
}


void sec_bat_set_charging_status(struct sec_battery_info *battery,
		int status)
{
	union power_supply_propval value = {0, };

	switch (status) {
	case POWER_SUPPLY_STATUS_CHARGING:
		if (battery->siop_level < 100 || battery->lcd_status || battery->wc_tx_enable)
			battery->stop_timer = true;
		break;
	case POWER_SUPPLY_STATUS_NOT_CHARGING:
	case POWER_SUPPLY_STATUS_DISCHARGING:
		/*
		 * Leaving the charging/full state: scale the fuel gauge up to
		 * 101% (so the reported SOC can reach 100%) and re-arm the
		 * safety timer.
		 */
		if ((battery->status == POWER_SUPPLY_STATUS_FULL ||
		     (battery->capacity == 100 && !is_slate_mode(battery))) &&
		    !battery->store_mode && battery->charging_enabled &&
		    !is_eu_eco_rechg(battery->fs)) {

			pr_info("%s : Update fg scale to 101%%\n", __func__);
			value.intval = 100;
			psy_do_property(battery->pdata->fuelgauge_name, set,
					POWER_SUPPLY_PROP_CHARGE_FULL, value);

			/* To get SOC value (NOT raw SOC), need to reset value */
			value.intval = 0;
			psy_do_property(battery->pdata->fuelgauge_name, get,
					POWER_SUPPLY_PROP_CAPACITY, value);
			battery->capacity = value.intval;
		}
		battery->expired_time = battery->pdata->expired_time;
		battery->prev_safety_time = 0;
		break;
	case POWER_SUPPLY_STATUS_FULL:
		sec_bat_send_cs100(battery);
		break;
	default:
		break;
	}
	battery->status = status;
}





#if defined(CONFIG_BATTERY_AGE_FORECAST)
__visible_for_testing bool sec_bat_set_aging_step(struct sec_battery_info *battery, int step)
{
	union power_supply_propval value = {0, };

	if (battery->pdata->num_age_step <= 0 || step < 0 || step >= battery->pdata->num_age_step) {
		pr_info("%s: [AGE] abnormal age step : %d/%d\n",
			__func__, step, battery->pdata->num_age_step-1);
		return false;
	}

	battery->pdata->age_step = step;

	/* float voltage */
	battery->pdata->chg_float_voltage =
		battery->pdata->age_data[battery->pdata->age_step].float_voltage;
	battery->pdata->swelling_normal_float_voltage =
		battery->pdata->chg_float_voltage;

	if (!battery->swelling_mode) {
		psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_VOLTAGE_MAX, value);
		if (value.intval > battery->pdata->chg_float_voltage) {
			value.intval = battery->pdata->chg_float_voltage;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_VOLTAGE_MAX, value);
		}
	}

	/* full/recharge condition */
	battery->pdata->recharge_condition_vcell =
		battery->pdata->age_data[battery->pdata->age_step].recharge_condition_vcell;
	battery->pdata->full_condition_soc =
		battery->pdata->age_data[battery->pdata->age_step].full_condition_soc;
	battery->pdata->full_condition_vcell =
		battery->pdata->age_data[battery->pdata->age_step].full_condition_vcell;
#if defined(CONFIG_DUAL_BATTERY)
	value.intval = battery->pdata->age_step;
	psy_do_property(battery->pdata->dual_battery_name, set,
		POWER_SUPPLY_EXT_PROP_FULL_CONDITION, value);
#endif
#if !defined(CONFIG_KUNIT)
	value.intval = battery->pdata->full_condition_soc;
	psy_do_property(battery->pdata->fuelgauge_name, set,
		POWER_SUPPLY_PROP_CAPACITY_LEVEL, value);
#endif

#if defined(CONFIG_STEP_CHARGING)
	sec_bat_set_aging_info_step_charging(battery);
#endif

	dev_info(battery->dev,
		 "%s: Step(%d/%d), Cycle(%d), float_v(%d), r_v(%d), f_s(%d), f_vl(%d)\n",
		 __func__,
		 battery->pdata->age_step, battery->pdata->num_age_step-1, battery->batt_cycle,
		 battery->pdata->chg_float_voltage,
		 battery->pdata->recharge_condition_vcell,
		 battery->pdata->full_condition_soc,
		 battery->pdata->full_condition_vcell);

	return true;
}

void sec_bat_aging_check(struct sec_battery_info *battery)
{
	int prev_step = battery->pdata->age_step;
	int calc_step = -1;
	bool ret = 0;

	if (battery->pdata->num_age_step <= 0 || battery->batt_cycle < 0)
		return;

	if (battery->temperature < 50) {
		pr_info("%s: [AGE] skip (temperature:%d)\n", __func__, battery->temperature);
		return;
	}

	for (calc_step = battery->pdata->num_age_step - 1; calc_step >= 0; calc_step--) {
		if (battery->pdata->age_data[calc_step].cycle <= battery->batt_cycle)
			break;
	}

	if (calc_step == prev_step)
		return;

	ret = sec_bat_set_aging_step(battery, calc_step);
	dev_info(battery->dev,
		 "%s: %s change step (%d->%d), Cycle(%d)\n",
		 __func__, ret ? "Succeed in" : "Fail to",
		 prev_step, battery->pdata->age_step, battery->batt_cycle);
}
#endif


#if defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
extern bool get_usb_enumeration_state(void);
/* To disaply slow charging when usb charging 100MA*/
static void sec_bat_check_slowcharging_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, slowcharging_work.work);

	if (battery->pdic_info.sink_status.rp_currentlvl == RP_CURRENT_LEVEL_DEFAULT &&
		battery->cable_type == SEC_BATTERY_CABLE_USB &&
		!get_usb_enumeration_state() &&
		(battery->current_event & SEC_BAT_CURRENT_EVENT_USB_100MA)) {
		battery->usb_slow_chg = true;
		battery->max_charge_power = (battery->input_voltage * battery->current_max) / 10;
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
	}
	dev_info(battery->dev, "%s:\n", __func__);
}
#endif


static void sec_bat_monitor_work(
				struct work_struct *work)
{
	struct sec_battery_info *battery =
		container_of(work, struct sec_battery_info,
		monitor_work.work);
	static struct timespec old_ts = {0, };
	struct timespec c_ts = {0, };
	union power_supply_propval val = {0, };
	union power_supply_propval value = {0, };

	dev_dbg(battery->dev, "%s: Start\n", __func__);
	c_ts = ktime_to_timespec(ktime_get_boottime());

	mutex_lock(&battery->wclock);
	if (!battery->wc_enable) {
		pr_debug("%s: wc_enable(%d), cnt(%d)\n",
			__func__, battery->wc_enable, battery->wc_enable_cnt);
		if (battery->wc_enable_cnt > battery->wc_enable_cnt_value) {
#if defined(CONFIG_DISABLE_MFC_IC)
			char wpc_en_status[2];

			battery->wc_enable = true;
			battery->wc_enable_cnt = 0;
			wpc_en_status[0] = WPC_EN_SYSFS;
			wpc_en_status[1] = true;
			value.strval = wpc_en_status;
			psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_EXT_PROP_WPC_EN, value);
#else
			if (battery->pdata->wpc_en)
				gpio_direction_output(battery->pdata->wpc_en, 0);
#endif
			pr_info("%s: WC CONTROL: Enable\n", __func__);
			pr_info("%s: wpc_en(%d)\n",
				__func__, gpio_get_value(battery->pdata->wpc_en));
		}
		battery->wc_enable_cnt++;
	}
	mutex_unlock(&battery->wclock);

	if (is_hv_wireless_type(battery->cable_type)
		&& !battery->wc_auth_retried && !lpcharge)
		sec_bat_check_wc_re_auth(battery);

	/* monitor once after wakeup */
	if (battery->polling_in_sleep) {
		battery->polling_in_sleep = false;
		if ((battery->status == POWER_SUPPLY_STATUS_DISCHARGING) &&
			((battery->ps_enable != true) && !battery->wc_tx_enable)) {
			if ((unsigned long)(c_ts.tv_sec - old_ts.tv_sec) < 10 * 60) {
					psy_do_property(battery->pdata->fuelgauge_name, get,
						POWER_SUPPLY_PROP_VOLTAGE_NOW, value);
					battery->voltage_now = value.intval;

					value.intval = 0;
					psy_do_property(battery->pdata->fuelgauge_name, get,
							POWER_SUPPLY_PROP_CAPACITY, value);
					battery->capacity = value.intval;

					sec_bat_get_temperature_info(battery);
#if defined(CONFIG_BATTERY_CISD)
					sec_bat_cisd_check(battery);
#endif
					power_supply_changed(battery->psy_bat);
					pr_info("Skip monitor work(%ld, Vnow:%d(mV), SoC:%d(%%), Tbat:%d(0.1'C))\n",
						c_ts.tv_sec - old_ts.tv_sec, battery->voltage_now, battery->capacity, battery->temperature);

				goto skip_monitor;
			}
		}
	}
	/* update last monitor time */
	old_ts = c_ts;

	sec_bat_get_battery_info(battery);
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_EXT_PROP_INFO, value);

#if defined(CONFIG_BATTERY_CISD)
	sec_bat_cisd_check(battery);
#endif

#if defined(CONFIG_STEP_CHARGING)
	sec_bat_check_step_charging(battery);
#endif
#if defined(CONFIG_CALC_TIME_TO_FULL)
	/* time to full check */
	sec_bat_calc_time_to_full(battery);
#endif
	sec_bat_check_full_capacity(battery);
#if defined(CONFIG_WIRELESS_TX_MODE)
	/* tx mode check */
	if (battery->wc_tx_enable) {
		pr_info("@Tx_Mode %s: tx_retry(0x%x), tx_switch(0x%x)",
			__func__, battery->tx_retry_case, battery->tx_switch_mode);
#if !defined(CONFIG_SEC_FACTORY)
		sec_bat_check_tx_battery_drain(battery);
		sec_bat_check_tx_temperature(battery);

		if ((battery->wc_rx_type == SS_PHONE) || (battery->wc_rx_type == OTHER_DEV) || (battery->wc_rx_type == SS_BUDS))
			sec_bat_check_tx_current(battery);
#endif
		sec_bat_txpower_calc(battery);
		sec_bat_handle_tx_misalign(battery, false);
		sec_bat_handle_tx_ocp(battery, false);

		if (battery->tx_switch_mode != TX_SWITCH_MODE_OFF && battery->tx_switch_start_soc != 0)
			sec_bat_check_tx_switch_mode(battery);

	} else if (battery->tx_retry_case != SEC_BAT_TX_RETRY_NONE) {
		pr_info("@Tx_Mode %s: tx_retry(0x%x)", __func__, battery->tx_retry_case);
#if !defined(CONFIG_SEC_FACTORY)
		sec_bat_check_tx_temperature(battery);
#endif
		sec_bat_handle_tx_misalign(battery, false);
		sec_bat_handle_tx_ocp(battery, false);
	}
#endif

	/* 0. test mode */
	if (battery->test_mode) {
		dev_err(battery->dev, "%s: Test Mode\n", __func__);
		sec_bat_do_test_function(battery);
		if (battery->test_mode != 0)
			goto continue_monitor;
	}

	/* 1. battery check */
	if (!sec_bat_battery_cable_check(battery))
		goto continue_monitor;

	/* 2. voltage check */
	if (!sec_bat_voltage_check(battery))
		goto continue_monitor;

	/* monitor short routine in initial monitor */
	if (battery->pdata->monitor_initial_count || sec_bat_is_short_polling(battery))
		goto skip_current_monitor;

	/* 3. time management */
	if (!sec_bat_time_management(battery))
		goto continue_monitor;

	/* 4. temperature check */
	if (!sec_bat_temperature_check(battery))
		goto continue_monitor;

#if defined(CONFIG_BATTERY_SWELLING)
	/* 5. swelling check */
	sec_bat_swelling_check(battery);

	/* 6. full charging check */
	if ((battery->swelling_mode == SWELLING_MODE_CHARGING || battery->swelling_mode == SWELLING_MODE_FULL) &&
		(!battery->charging_block))
		sec_bat_swelling_fullcharged_check(battery);
	else
		sec_bat_fullcharged_check(battery);
#endif

	/* 6-1. eu eco check */
	if (check_eu_eco_full_status(battery))
		sec_bat_do_fullcharged(battery, true);

	/* 7. additional check */
	if (battery->pdata->monitor_additional_check)
		battery->pdata->monitor_additional_check();

	if (is_nv_wireless_type(battery->cable_type) &&
		(!battery->wc_cv_mode) &&
		(battery->charging_passed_time > 10))
		sec_bat_wc_cv_mode_check(battery);

#if defined(CONFIG_STEP_CHARGING)
#if defined(CONFIG_DIRECT_CHARGING)
	if (is_pd_apdo_wire_type(battery->cable_type))
		sec_bat_check_dc_step_charging(battery);
#endif
#endif

continue_monitor:
	/* clear HEATING_CONTROL*/
	sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL);

	/* calculate safety time */
	if (!battery->charging_block)
		sec_bat_calculate_safety_time(battery);

	/* set charging current */
	sec_bat_set_charging_current(battery);

skip_current_monitor:
	psy_do_property(battery->pdata->charger_name, get,
		POWER_SUPPLY_EXT_PROP_MONITOR_WORK, val);

	if (battery->pdata->wireless_charger_name)
		psy_do_property(battery->pdata->wireless_charger_name, get,
			POWER_SUPPLY_EXT_PROP_MONITOR_WORK, val);

	dev_dbg(battery->dev,
		"%s: HLT(%d) HLR(%d) HT(%d), HR(%d), LT(%d), LR(%d), lpcharge(%d)\n",
		__func__, battery->temp_highlimit_threshold, battery->temp_highlimit_recovery,
		battery->temp_high_threshold, battery->temp_high_recovery,
		battery->temp_low_threshold, battery->temp_low_recovery, lpcharge);

	pr_debug("%s: Status(%s), mode(%s), Health(%s), Cable(%s, %s, %d, %d), rp(%d), level(%d%%), lcd(%d), slate_mode(%d), store_mode(%d), charging_enabled(%d)"
#if defined(CONFIG_AFC_CHARGER_MODE)
		", HV(%s, %d), sleep_mode(%d)"
#endif
#if defined(CONFIG_BATTERY_AGE_FORECAST)
		", Cycle(%d)"
#endif
		 "\n", __func__,
		 sec_bat_status_str[battery->status],
		 sec_bat_charging_mode_str[battery->charging_mode],
		 sec_bat_health_str[battery->health],
		 sec_cable_type[battery->cable_type],
		 sec_cable_type[battery->wire_status],
		 battery->muic_cable_type,
		 battery->pd_usb_attached,
		 battery->pdic_info.sink_status.rp_currentlvl,
		 battery->siop_level,
		 battery->lcd_status,
		 is_slate_mode(battery),
		 battery->store_mode,
		 battery->charging_enabled
#if defined(CONFIG_AFC_CHARGER_MODE)
		, battery->hv_chg_name, battery->vbus_chg_by_siop, sleep_mode
#endif
#if defined(CONFIG_BATTERY_AGE_FORECAST)
		, battery->batt_cycle
#endif
		 );

#if defined(CONFIG_WIRELESS_TX_MODE)
	if (battery->wc_tx_enable) {
		unsigned int vout;
		vout = battery->wc_tx_vout == 0 ? 5000 : (5000 + (battery->wc_tx_vout * 500));
		pr_info("@Tx_Mode %s: Rx(%s), WC_TX_VOUT(%dmV), UNO_IOUT(%d), MFC_IOUT(%d) AFC_DISABLE(%d)\n",
			__func__, sec_bat_rx_type_str[battery->wc_rx_type],
			vout, battery->tx_uno_iout, battery->tx_mfc_iout, battery->afc_disable);
	}
#endif

#if defined(CONFIG_ENG_BATTERY_CONCEPT)
	pr_info("%s: battery->stability_test(%d), battery->eng_not_full_status(%d)\n",
			__func__, battery->stability_test, battery->eng_not_full_status);
#endif

	if (!is_nocharge_type(battery->cable_type) && !battery->charging_enabled) {
		int chg_mode;

		pr_info("%s: @battery->capacity = (%d), battery->status= (%d), battery->charging_enabled=(%d)\n",
			 __func__, battery->capacity, battery->status, battery->charging_enabled);

		chg_mode = battery->misc_event &
			(BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE | BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE) ?
				SEC_BAT_CHG_MODE_BUCK_OFF : SEC_BAT_CHG_MODE_CHARGING_OFF;

		sec_bat_set_charging_status(battery,
					    POWER_SUPPLY_STATUS_DISCHARGING);
		sec_bat_set_charge(battery, chg_mode);

		/* Enable charging on capacity lower than 30%, in case something bad happened */
		if ((battery->capacity <= 30) && (battery->status == POWER_SUPPLY_STATUS_DISCHARGING)) {
			sec_bat_set_charging_status(battery,
						    POWER_SUPPLY_STATUS_CHARGING);
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
		}
	}

#if defined(CONFIG_SEC_FACTORY)
	if (!is_nocharge_type(battery->cable_type)) {
#else
	if (!is_nocharge_type(battery->cable_type) && battery->store_mode) {
#endif
		pr_info("%s: @battery->capacity = (%d), battery->status= (%d), battery->store_mode=(%d)\n",
			 __func__, battery->capacity, battery->status, battery->store_mode);

		if (battery->capacity >= battery->pdata->store_mode_charging_max) {
			int chg_mode = battery->misc_event &
				(BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE | BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE) ?
					SEC_BAT_CHG_MODE_BUCK_OFF : SEC_BAT_CHG_MODE_CHARGING_OFF;
			/* to discharge the battery, off buck */
			if (battery->capacity > battery->pdata->store_mode_charging_max
					|| battery->pdata->store_mode_buckoff)
				chg_mode = SEC_BAT_CHG_MODE_BUCK_OFF;

			sec_bat_set_charging_status(battery,
						    POWER_SUPPLY_STATUS_DISCHARGING);
			sec_bat_set_charge(battery, chg_mode);
		}

		if ((battery->capacity <= battery->pdata->store_mode_charging_min) && (battery->status == POWER_SUPPLY_STATUS_DISCHARGING)) {
			sec_bat_set_charging_status(battery,
						    POWER_SUPPLY_STATUS_CHARGING);
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
		}
	}

	if (!is_nocharge_type(battery->cable_type) && battery->charging_suspended && battery->charging_enabled && !battery->store_mode) {
		pr_info("%s: @battery->capacity = (%d), battery->status= (%d), battery->charging_enabled=(%d)\n",
			 __func__, battery->capacity, battery->status, battery->charging_enabled);

		sec_bat_set_charging_status(battery,
					    POWER_SUPPLY_STATUS_CHARGING);
		sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);

		battery->charging_suspended = false;
	}

	power_supply_changed(battery->psy_bat);

skip_monitor:
	sec_bat_set_polling(battery);

#if defined(CONFIG_WIRELESS_TX_MODE)
	if (battery->tx_switch_mode_change) {
		cancel_delayed_work(&battery->wpc_tx_work);
		__pm_stay_awake(battery->wpc_tx_wake_lock);
		queue_delayed_work(battery->monitor_wqueue,
				&battery->wpc_tx_work, 0);
	}
#endif

	if (battery->capacity <= 0 || battery->health_change)
		__pm_wakeup_event(battery->monitor_wake_lock, jiffies_to_msecs(HZ * 5));
	else
		__pm_relax(battery->monitor_wake_lock);

	dev_dbg(battery->dev, "%s: End\n", __func__);

	return;
}





#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER) || defined(CONFIG_MUIC_NOTIFIER)
static int sec_bat_cable_check(struct sec_battery_info *battery,
				muic_attached_dev_t attached_dev)
{
	int current_cable_type = -1;
	union power_supply_propval val = {0, };

	pr_info("[%s]ATTACHED(%d)\n", __func__, attached_dev);

	switch (attached_dev) {
	case ATTACHED_DEV_JIG_UART_OFF_MUIC:
	case ATTACHED_DEV_JIG_UART_ON_MUIC:
		battery->is_jig_on = true;
#if defined(CONFIG_BATTERY_CISD)
		battery->skip_cisd = true;
#endif
		current_cable_type = SEC_BATTERY_CABLE_NONE;
		break;
	case ATTACHED_DEV_SMARTDOCK_MUIC:
	case ATTACHED_DEV_DESKDOCK_MUIC:
	case ATTACHED_DEV_JIG_USB_ON_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_NONE;
		break;
	case ATTACHED_DEV_UNDEFINED_CHARGING_MUIC:
	case ATTACHED_DEV_UNDEFINED_RANGE_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_NONE;
		break;
	case ATTACHED_DEV_HICCUP_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_NONE;
		break;
	case ATTACHED_DEV_OTG_MUIC:
	case ATTACHED_DEV_JIG_UART_OFF_VB_OTG_MUIC:
	case ATTACHED_DEV_HMT_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_OTG;
		break;
	case ATTACHED_DEV_TIMEOUT_OPEN_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_TIMEOUT;
		break;
	case ATTACHED_DEV_USB_MUIC:
	case ATTACHED_DEV_JIG_USB_OFF_MUIC:
	case ATTACHED_DEV_SMARTDOCK_USB_MUIC:
	case ATTACHED_DEV_UNOFFICIAL_ID_USB_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_USB;
		break;
	case ATTACHED_DEV_JIG_UART_ON_VB_MUIC:
	case ATTACHED_DEV_JIG_UART_OFF_VB_MUIC:
	case ATTACHED_DEV_JIG_UART_OFF_VB_FG_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_UARTOFF;
		break;
	case ATTACHED_DEV_RDU_TA_MUIC:
		battery->store_mode = true;
		__pm_stay_awake(battery->parse_mode_dt_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->parse_mode_dt_work, 0);
		current_cable_type = SEC_BATTERY_CABLE_TA;
		break;
	case ATTACHED_DEV_TA_MUIC:
	case ATTACHED_DEV_CARDOCK_MUIC:
	case ATTACHED_DEV_DESKDOCK_VB_MUIC:
	case ATTACHED_DEV_SMARTDOCK_TA_MUIC:
	case ATTACHED_DEV_UNOFFICIAL_TA_MUIC:
	case ATTACHED_DEV_UNOFFICIAL_ID_TA_MUIC:
	case ATTACHED_DEV_UNOFFICIAL_ID_ANY_MUIC:
	case ATTACHED_DEV_UNSUPPORTED_ID_VB_MUIC:
	case ATTACHED_DEV_AFC_CHARGER_DISABLED_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_TA;
		break;
	case ATTACHED_DEV_AFC_CHARGER_5V_MUIC:
	case ATTACHED_DEV_QC_CHARGER_5V_MUIC:
	case ATTACHED_DEV_AFC_CHARGER_5V_DUPLI_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT;
		break;
	case ATTACHED_DEV_CDP_MUIC:
	case ATTACHED_DEV_UNOFFICIAL_ID_CDP_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_USB_CDP;
		break;
	case ATTACHED_DEV_USB_LANHUB_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_LAN_HUB;
		break;
	case ATTACHED_DEV_CHARGING_CABLE_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_POWER_SHARING;
		break;
	case ATTACHED_DEV_AFC_CHARGER_PREPARE_MUIC:
	case ATTACHED_DEV_QC_CHARGER_PREPARE_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_PREPARE_TA;
		break;
	case ATTACHED_DEV_QC_CHARGER_9V_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_9V_TA;
		if ((battery->cable_type == SEC_BATTERY_CABLE_TA) ||
		    (battery->cable_type == SEC_BATTERY_CABLE_NONE))
			battery->cisd.cable_data[CISD_CABLE_QC]++;
		break;
	case ATTACHED_DEV_AFC_CHARGER_9V_MUIC:
	case ATTACHED_DEV_AFC_CHARGER_9V_DUPLI_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_9V_TA;
		if ((battery->cable_type == SEC_BATTERY_CABLE_TA) ||
		    (battery->cable_type == SEC_BATTERY_CABLE_NONE))
			battery->cisd.cable_data[CISD_CABLE_AFC]++;
		break;
#if defined(CONFIG_MUIC_HV_12V)
	case ATTACHED_DEV_AFC_CHARGER_12V_MUIC:
	case ATTACHED_DEV_AFC_CHARGER_12V_DUPLI_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_12V_TA;
		break;
#endif
	case ATTACHED_DEV_AFC_CHARGER_ERR_V_MUIC:
	case ATTACHED_DEV_AFC_CHARGER_ERR_V_DUPLI_MUIC:
		battery->cisd.cable_data[CISD_CABLE_AFC_FAIL]++;
		break;
	case ATTACHED_DEV_QC_CHARGER_ERR_V_MUIC:
		battery->cisd.cable_data[CISD_CABLE_QC_FAIL]++;
		break;
	case ATTACHED_DEV_HV_ID_ERR_UNDEFINED_MUIC:
	case ATTACHED_DEV_HV_ID_ERR_UNSUPPORTED_MUIC:
	case ATTACHED_DEV_HV_ID_ERR_SUPPORTED_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_9V_UNKNOWN;
		break;
	case ATTACHED_DEV_VZW_INCOMPATIBLE_MUIC:
		current_cable_type = SEC_BATTERY_CABLE_UNKNOWN;
		break;
	default:
		pr_err("%s: invalid type for charger:%d\n",
			__func__, attached_dev);
		break;
	}

	if (battery->is_jig_on && !battery->pdata->support_fgsrc_change)
		psy_do_property(battery->pdata->fuelgauge_name, set,
			POWER_SUPPLY_PROP_ENERGY_NOW, val);

	return current_cable_type;
}
#endif

#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
#if defined(CONFIG_CCIC_NOTIFIER)
static int sec_bat_get_pd_list_index(PDIC_SINK_STATUS *sink_status, struct sec_bat_pdic_list *pd_list)
{
	int i;

	for (i = 0; i < pd_list->max_pd_count; i++) {
		if (pd_list->pd_info[i].pdo_index == sink_status->current_pdo_num)
			return i;
	}

	/*
	 * "not found" returns 0, not an error sentinel, so a miss is
	 * indistinguishable from list slot 0.  Kept as is because callers
	 * store the result as now_pd_index.
	 */
	return 0;
}

static void sec_bat_set_rp_current(struct sec_battery_info *battery, int cable_type)
{
	if (battery->pdic_info.sink_status.rp_currentlvl == RP_CURRENT_ABNORMAL) {
		sec_bat_change_default_current(battery, cable_type,
			battery->pdata->rp_current_abnormal_rp3, battery->pdata->rp_current_abnormal_rp3);
	} else if (battery->pdic_info.sink_status.rp_currentlvl == RP_CURRENT_LEVEL3) {
		if (battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE)
			sec_bat_change_default_current(battery, cable_type,
				battery->pdata->default_input_current, battery->pdata->default_charging_current);
		else {
			if (battery->store_mode || !battery->charging_enabled)
				sec_bat_change_default_current(battery, cable_type,
					battery->pdata->rp_current_rdu_rp3, battery->pdata->max_charging_current);
			else
				sec_bat_change_default_current(battery, cable_type,
					battery->pdata->rp_current_rp3, battery->pdata->max_charging_current);
		}
	} else if (battery->pdic_info.sink_status.rp_currentlvl == RP_CURRENT_LEVEL2) {
		sec_bat_change_default_current(battery, cable_type,
			battery->pdata->rp_current_rp2, battery->pdata->rp_current_rp2);
	} else if (battery->pdic_info.sink_status.rp_currentlvl == RP_CURRENT_LEVEL_DEFAULT) {
		if (cable_type == SEC_BATTERY_CABLE_USB) {
			if (battery->current_event & SEC_BAT_CURRENT_EVENT_USB_SUPER)
				sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
					USB_CURRENT_SUPER_SPEED, USB_CURRENT_SUPER_SPEED);
			else
				sec_bat_change_default_current(battery, cable_type,
					battery->pdata->default_usb_input_current,
					battery->pdata->default_usb_charging_current);
		} else if (cable_type == SEC_BATTERY_CABLE_TA) {
			sec_bat_change_default_current(battery, cable_type,
				battery->pdata->default_input_current,
				battery->pdata->default_charging_current);
		}
	}
	battery->aicl_current = 0;

	pr_info("%s:(%d)\n", __func__, battery->pdic_info.sink_status.rp_currentlvl);
	battery->max_charge_power = 0;
	if (battery->status != POWER_SUPPLY_STATUS_DISCHARGING)
		sec_bat_check_input_voltage(battery);
	/* prevent TA ocp */
	if (!is_hv_wireless_type(battery->cable_type) &&
		battery->cable_type != SEC_BATTERY_CABLE_PREPARE_WIRELESS_20)
		sec_bat_set_charging_current(battery);
}
#endif

__visible_for_testing int make_pd_list(struct sec_battery_info *battery)
{
	int i;
	int base_charge_power = 0, selected_pdo_voltage = 0, selected_pdo_power = 0, selected_pdo_num = 0;
	int pd_list_index = 0, temp_power = 0, num_pd_list = 0, pd_list_select = 0;
	int pd_charging_charge_power = battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE ?
		battery->pdata->nv_charge_power : battery->pdata->pd_charging_charge_power;
	union power_supply_propval value = {0, };
	POWER_LIST *pPower_list;

	/* If PD charger is attached first, current_pdo_num should be 1 supports 5V */
#if defined(CONFIG_PDIC_PD30)
	battery->pd_list.pd_info[0].max_voltage =
		battery->pdic_info.sink_status.power_list[1].max_voltage;
	battery->pd_list.pd_info[0].max_current =
		battery->pdic_info.sink_status.power_list[1].max_current;
	battery->pd_list.pd_info[0].comm_capable =
		battery->pdic_info.sink_status.power_list[1].comm_capable;
	battery->pd_list.pd_info[0].suspend =
		battery->pdic_info.sink_status.power_list[1].suspend;
#else
	battery->pd_list.pd_info[0].input_voltage =
		battery->pdic_info.sink_status.power_list[1].max_voltage;
	battery->pd_list.pd_info[0].input_current =
		battery->pdic_info.sink_status.power_list[1].max_current;
#endif
	battery->pd_list.pd_info[0].pdo_index = 1;
	pd_list_index++;

	base_charge_power =
		battery->pdic_info.sink_status.power_list[1].max_voltage * battery->pdic_info.sink_status.power_list[1].max_current;

	selected_pdo_voltage = SEC_INPUT_VOLTAGE_5V * 100;
	selected_pdo_power = 0;
	selected_pdo_num = 0;

	for (i = 1; i <= battery->pdic_info.sink_status.available_pdo_num; i++) {
		pPower_list = &battery->pdic_info.sink_status.power_list[i];
#if defined(CONFIG_PDIC_PD30)
		if (!pPower_list->accept || pPower_list->apdo) /* skip not accept of apdo list */
			continue;
#endif
		temp_power = pPower_list->max_voltage * pPower_list->max_current;

		if ((temp_power >= base_charge_power - 1000000) &&
		    (temp_power <= pd_charging_charge_power * 1000)) {
			if (temp_power >= selected_pdo_power &&
			    pPower_list->max_voltage > selected_pdo_voltage &&
			    pPower_list->max_voltage <= battery->pdata->max_input_voltage) {
				selected_pdo_voltage = pPower_list->max_voltage;
				selected_pdo_power = temp_power;
				selected_pdo_num = i;
			}
		}
	}
	if (selected_pdo_num) {
		POWER_LIST *pSelected_power_list =
			&battery->pdic_info.sink_status.power_list[selected_pdo_num];

		battery->pd_list.pd_info[pd_list_index].pdo_index = selected_pdo_num;
#if defined(CONFIG_PDIC_PD30)
		battery->pd_list.pd_info[pd_list_index].apdo = false;
		battery->pd_list.pd_info[pd_list_index].max_voltage = pSelected_power_list->max_voltage;
		battery->pd_list.pd_info[pd_list_index].max_current = pSelected_power_list->max_current;
		battery->pd_list.pd_info[pd_list_index].min_voltage = 0;
		battery->pd_list.pd_info[pd_list_index].comm_capable = pSelected_power_list->comm_capable;
		battery->pd_list.pd_info[pd_list_index].suspend = pSelected_power_list->suspend;
#else
		battery->pd_list.pd_info[pd_list_index].input_voltage = pSelected_power_list->max_voltage;
		battery->pd_list.pd_info[pd_list_index].input_current = pSelected_power_list->max_current;
#endif
		pd_list_index++;
	}

#if defined(CONFIG_PDIC_PD30)
	battery->pd_list.num_fpdo = pd_list_index;

	if (battery->pdic_info.sink_status.has_apdo) {
		/* unconditionally add APDO list */
		for (i = 1; i <= battery->pdic_info.sink_status.available_pdo_num; i++) {
			pPower_list = &battery->pdic_info.sink_status.power_list[i];

			if (pPower_list->apdo && pd_list_index >= 0 && pd_list_index < MAX_PDO_NUM) {
				battery->pd_list.pd_info[pd_list_index].pdo_index = i;
				battery->pd_list.pd_info[pd_list_index].apdo = true;
				battery->pd_list.pd_info[pd_list_index].max_voltage = pPower_list->max_voltage;
				battery->pd_list.pd_info[pd_list_index].min_voltage = pPower_list->min_voltage;
				battery->pd_list.pd_info[pd_list_index].max_current = pPower_list->max_current;

				pd_list_index++;
			}
		}
		battery->pd_list.num_apdo = pd_list_index - battery->pd_list.num_fpdo;
	} else {
		/* battery->pdic_info.sink_status has no apdo */
		battery->pd_list.num_apdo = 0;
	}
#endif

	num_pd_list = pd_list_index;

	if (num_pd_list <= 0 || num_pd_list > MAX_PDO_NUM) {
		pr_info("%s : PDO list is wrong: %d!!\n", __func__, num_pd_list);
		return 0;
	}
#if defined(CONFIG_PDIC_PD30)
	pr_info("%s: total num_pd_list: %d, num_fpdo: %d, num_apdo: %d\n",
		__func__, num_pd_list, battery->pd_list.num_fpdo, battery->pd_list.num_apdo);
#else
	pr_info("%s: total num_pd_list: %d\n", __func__, num_pd_list);
#endif

#if defined(CONFIG_PDIC_PD30)
	if (battery->pdic_info.sink_status.has_apdo) {
		for (i = 0; i < battery->pd_list.num_fpdo - 1; i++) {
			/* select pdo 1, if pd have apdo */
			if (battery->pd_list.pd_info[i].pdo_index == 1) {
				pd_list_select = i;
				break;
			}
		}
	} else {
		pd_list_select = num_pd_list - battery->pd_list.num_apdo - 1;
	}
#else
	pd_list_select = num_pd_list - 1;
#endif
	if (pd_list_select < 0 || pd_list_select >= MAX_PDO_NUM) {
		pr_info("%s: pd_list_select is wrong: %d\n", __func__, pd_list_select);
		return 0;
	}

	for (i = 0; i < num_pd_list; i++) {
#if defined(CONFIG_PDIC_PD30)
		pr_info("%s: Made pd_list[%d] %s[%d,%s] maxVol:%d, minVol:%d, maxCur:%d, comm:%d, suspend:%d\n",
			__func__, i, i == pd_list_select ? "**" : " ",
			battery->pd_list.pd_info[i].pdo_index,
			battery->pd_list.pd_info[i].apdo ? "APDO" : "FIXED",
			battery->pd_list.pd_info[i].max_voltage,
			battery->pd_list.pd_info[i].min_voltage,
			battery->pd_list.pd_info[i].max_current,
			battery->pd_list.pd_info[i].comm_capable,
			battery->pd_list.pd_info[i].suspend);
#else
		pr_info("%s: Made pd_list[%d] %s[%d] voltage : %d, current : %d\n",
			__func__, i, i == pd_list_select ? "**" : " ",
			battery->pd_list.pd_info[i].pdo_index,
			battery->pd_list.pd_info[i].input_voltage,
			battery->pd_list.pd_info[i].input_current);
#endif
	}

	battery->pd_list.max_pd_count = num_pd_list;

#if defined(CONFIG_PDIC_PD30)
	if (!battery->pdic_info.sink_status.has_apdo ||
		battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE) {
		battery->max_charge_power =
			battery->pdic_info.sink_status.power_list[
				battery->pd_list.pd_info[pd_list_select].pdo_index].max_voltage *
			battery->pdic_info.sink_status.power_list[
				battery->pd_list.pd_info[pd_list_select].pdo_index].max_current / 1000;
		battery->pd_max_charge_power = battery->max_charge_power;
	}
#else
	battery->max_charge_power =
		battery->pdic_info.sink_status.power_list[
			battery->pd_list.pd_info[pd_list_select].pdo_index].max_voltage *
		battery->pdic_info.sink_status.power_list[
			battery->pd_list.pd_info[pd_list_select].pdo_index].max_current / 1000;
	battery->pd_max_charge_power = battery->max_charge_power;
#endif

	if (battery->cable_type == SEC_BATTERY_CABLE_NONE) {
		if (battery->pd_max_charge_power > 12000)
			battery->cisd.cable_data[CISD_CABLE_PD_HIGH]++;
		else
			battery->cisd.cable_data[CISD_CABLE_PD]++;
	}

	if (battery->pdic_info.sink_status.selected_pdo_num == battery->pd_list.pd_info[pd_list_select].pdo_index) {
		battery->pdic_ps_rdy = true;
		dev_info(battery->dev, "%s: battery->pdic_ps_rdy(%d)\n", __func__, battery->pdic_ps_rdy);
	} else if (battery->wc_rx_type != SS_GEAR) {
		/* change input current before request new pdo if new pdo's input current is less than now */
#if defined(CONFIG_PDIC_PD30)
		if (battery->pd_list.pd_info[pd_list_select].max_current < battery->input_current) {
			int input_current = battery->pd_list.pd_info[pd_list_select].max_current;
#else
		if (battery->pd_list.pd_info[pd_list_select].input_current < battery->input_current) {
			int input_current = battery->pd_list.pd_info[pd_list_select].input_current;
#endif

			value.intval = input_current;
			battery->input_current = input_current;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CURRENT_MAX, value);
		}
		battery->pdic_ps_rdy = false;
		sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_SELECT_PDO,
			SEC_BAT_CURRENT_EVENT_SELECT_PDO);
		select_pdo(battery->pd_list.pd_info[pd_list_select].pdo_index);
	}

	battery->pd_list.now_pd_index = sec_bat_get_pd_list_index(&battery->pdic_info.sink_status,
		&battery->pd_list);
	pr_info("%s : now_pd_index : %d\n", __func__, battery->pd_list.now_pd_index);

#if defined(CONFIG_DIRECT_CHARGING) && defined(CONFIG_PDIC_PD30)
	value.intval = battery->pd_list.num_apdo > 0 ? battery->pd_list.num_fpdo : 1;
	psy_do_property(battery->pdata->charger_name, set,
		POWER_SUPPLY_EXT_PROP_DIRECT_FIXED_PDO, value);
#endif

	return battery->pd_list.max_pd_count;
}

static int usb_typec_handle_notification(struct notifier_block *nb,
		unsigned long action, void *data)
{
	const char *cmd = "NONE";
	struct sec_battery_info *battery =
			container_of(nb, struct sec_battery_info, usb_typec_nb);
	int cable_type = SEC_BATTERY_CABLE_NONE, i = 0, current_pdo = 0;
	int pd_charging_charge_power = battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE ?
		battery->pdata->nv_charge_power : battery->pdata->pd_charging_charge_power;
	CC_NOTI_ATTACH_TYPEDEF usb_typec_info = *(CC_NOTI_ATTACH_TYPEDEF *)data;
	bool bPdIndexChanged = false;
	int max_power = 0;
#if defined(CONFIG_PDIC_PD30)
	bool bPrintPDlog = true;
	int apdo_power = 0;
#if defined(CONFIG_DIRECT_CHARGING)
	union power_supply_propval val = {0, };
#endif
#endif

	dev_info(battery->dev, "%s: action (%ld) dump(0x%01x, 0x%01x, 0x%02x, 0x%04x, 0x%04x, 0x%04x)\n",
		__func__, action, usb_typec_info.src, usb_typec_info.dest, usb_typec_info.id,
		usb_typec_info.attach, usb_typec_info.rprd, usb_typec_info.cable_type);

	if (usb_typec_info.dest != CCIC_NOTIFY_DEV_BATTERY) {
		dev_info(battery->dev, "%s: skip handler dest(%d)\n",
			__func__, usb_typec_info.dest);
		return 0;
	}

	mutex_lock(&battery->typec_notylock);
	switch (usb_typec_info.id) {
	case CCIC_NOTIFY_ID_WATER:
	case CCIC_NOTIFY_ID_ATTACH:
		switch (usb_typec_info.attach) {
		case MUIC_NOTIFY_CMD_DETACH:
		case MUIC_NOTIFY_CMD_LOGICALLY_DETACH:
			cmd = "DETACH";
			battery->is_jig_on = false;
			battery->pd_usb_attached = false;
			cable_type = SEC_BATTERY_CABLE_NONE;
			battery->muic_cable_type = ATTACHED_DEV_NONE_MUIC;
			battery->pdic_info.sink_status.rp_currentlvl = RP_CURRENT_LEVEL_NONE;
			break;
		case MUIC_NOTIFY_CMD_ATTACH:
		case MUIC_NOTIFY_CMD_LOGICALLY_ATTACH:
			/* Skip notify from MUIC if PDIC is attached already */
			if (is_pd_wire_type(battery->wire_status) || battery->init_src_cap) {
				if (lpcharge) {
					mutex_unlock(&battery->typec_notylock);
					return 0;
				} else if (!battery->usb_temp_flag && !(battery->misc_event & BATT_MISC_EVENT_TEMP_HICCUP_TYPE)) {
					mutex_unlock(&battery->typec_notylock);
					return 0;
				}
			}
			cmd = "ATTACH";
			battery->muic_cable_type = usb_typec_info.cable_type;
			cable_type = sec_bat_cable_check(battery, battery->muic_cable_type);
			if (battery->cable_type != cable_type &&
				battery->pdic_info.sink_status.rp_currentlvl >= RP_CURRENT_LEVEL_DEFAULT &&
				(cable_type == SEC_BATTERY_CABLE_USB || cable_type == SEC_BATTERY_CABLE_TA)) {
				sec_bat_set_rp_current(battery, cable_type);
			} else if ((struct pdic_notifier_struct *)usb_typec_info.pd != NULL &&
				(*(struct pdic_notifier_struct *)usb_typec_info.pd).event == PDIC_NOTIFY_EVENT_CCIC_ATTACH &&
				(*(struct pdic_notifier_struct *)usb_typec_info.pd).sink_status.rp_currentlvl >= RP_CURRENT_LEVEL_DEFAULT &&
				(cable_type == SEC_BATTERY_CABLE_USB || cable_type == SEC_BATTERY_CABLE_TA)) {
				battery->pdic_info.sink_status.rp_currentlvl =
					(*(struct pdic_notifier_struct *)usb_typec_info.pd).sink_status.rp_currentlvl;
				sec_bat_set_rp_current(battery, cable_type);
			}
			break;
		default:
			cmd = "ERROR";
			cable_type = -1;
			battery->muic_cable_type = usb_typec_info.cable_type;
			break;
		}
		battery->pdic_attach = false;
		battery->pdic_ps_rdy = false;
		battery->init_src_cap = false;
#if defined(CONFIG_AFC_CHARGER_MODE)
		if (battery->muic_cable_type == ATTACHED_DEV_QC_CHARGER_9V_MUIC ||
			battery->muic_cable_type == ATTACHED_DEV_QC_CHARGER_ERR_V_MUIC)
			battery->hv_chg_name = "QC";
		else if (battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_9V_MUIC ||
			battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_9V_DUPLI_MUIC ||
			battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_ERR_V_MUIC ||
			battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_ERR_V_DUPLI_MUIC)
			battery->hv_chg_name = "AFC";
#if defined(CONFIG_MUIC_HV_12V)
		else if (battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_12V_MUIC ||
			battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_12V_DUPLI_MUIC)
			battery->hv_chg_name = "12V";
#endif
		else
			battery->hv_chg_name = "NONE";
#endif
		break;
	case CCIC_NOTIFY_ID_POWER_STATUS:
		/*
		 * pd is only guaranteed non-NULL for a real PD event, and it
		 * is dereferenced repeatedly below.  The attach path already
		 * NULL-checks it; guard this path the same way.
		 */
		if (usb_typec_info.pd == NULL) {
			dev_info(battery->dev, "%s: null pd, skip POWER_STATUS\n",
				__func__);
			mutex_unlock(&battery->typec_notylock);
			return 0;
		}
#ifdef CONFIG_SEC_FACTORY
		dev_info(battery->dev, "%s: pd_event(%d)\n", __func__,
			(*(struct pdic_notifier_struct *)usb_typec_info.pd).event);
#endif
		if ((*(struct pdic_notifier_struct *)usb_typec_info.pd).event == PDIC_NOTIFY_EVENT_DETACH) {
			dev_info(battery->dev, "%s: skip pd operation - attach(%d)\n", __func__, usb_typec_info.attach);
			battery->pdic_attach = false;
			battery->pdic_ps_rdy = false;
			battery->init_src_cap = false;
			battery->hv_pdo = false;
			battery->pd_list.now_pd_index = 0;
#if defined(CONFIG_PDIC_PD30)
			battery->pd_list.now_isApdo = false;
			battery->pd_list.num_apdo = 0;
			battery->pd_list.num_fpdo = 0;
#endif
			mutex_unlock(&battery->typec_notylock);
			return 0;
		} else if ((*(struct pdic_notifier_struct *)usb_typec_info.pd).event == PDIC_NOTIFY_EVENT_PD_PRSWAP_SNKTOSRC) {
			cmd = "PD_PRWAP";
			dev_info(battery->dev, "%s: PRSWAP_SNKTOSRC(%d)\n", __func__, usb_typec_info.attach);
			cable_type = SEC_BATTERY_CABLE_NONE;

			battery->pdic_attach = false;
			battery->pdic_ps_rdy = false;
			battery->init_src_cap = false;
			battery->hv_pdo = false;
			battery->pd_list.now_pd_index = 0;
			goto skip_cable_check;
		} else if (!lpcharge && (battery->usb_temp_flag || (battery->misc_event & BATT_MISC_EVENT_TEMP_HICCUP_TYPE))) {
			goto skip_cable_check;
		}

		cmd = "PD_ATTACH";
		if ((*(struct pdic_notifier_struct *)usb_typec_info.pd).event == PDIC_NOTIFY_EVENT_CCIC_ATTACH) {
			battery->pdic_info.sink_status.rp_currentlvl =
				(*(struct pdic_notifier_struct *)usb_typec_info.pd).sink_status.rp_currentlvl;
			dev_info(battery->dev, "%s: battery->rp_currentlvl(%d)\n", __func__, battery->pdic_info.sink_status.rp_currentlvl);
			if (battery->wire_status == SEC_BATTERY_CABLE_USB || battery->wire_status == SEC_BATTERY_CABLE_TA) {
				cable_type = battery->wire_status;
				battery->chg_limit = false;
				battery->lrp_limit = false;
				battery->lrp_step = LRP_NONE;
				sec_bat_set_rp_current(battery, cable_type);
				goto skip_cable_check;
			}
			mutex_unlock(&battery->typec_notylock);
			return 0;
		}
		battery->init_src_cap = false;
		if ((*(struct pdic_notifier_struct *)usb_typec_info.pd).event == PDIC_NOTIFY_EVENT_PD_SINK_CAP || battery->update_pd_list) {
			pr_info("%s : update_pd_list(%d)\n", __func__, battery->update_pd_list);
#if defined(CONFIG_DIRECT_CHARGING)
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_EXT_PROP_DIRECT_CLEAR_ERR, val);
#endif
			battery->pdic_attach = false;
			battery->update_pd_list = false;
		}
		if (!battery->pdic_attach) {
			battery->pdic_info = *(struct pdic_notifier_struct *)usb_typec_info.pd;
			battery->pd_list.now_pd_index = 0;
			bPdIndexChanged = true;
		} else {
			unsigned int prev_pd_index = battery->pd_list.now_pd_index;

			battery->pdic_info.sink_status.selected_pdo_num =
				(*(struct pdic_notifier_struct *)usb_typec_info.pd).sink_status.selected_pdo_num;
			battery->pdic_info.sink_status.current_pdo_num =
				(*(struct pdic_notifier_struct *)usb_typec_info.pd).sink_status.current_pdo_num;
			battery->pd_list.now_pd_index = sec_bat_get_pd_list_index(&battery->pdic_info.sink_status,
				&battery->pd_list);
			dev_info(battery->dev, "%s: battery->pd_list.now_pd_index(%d), prev_pd_index(%d)\n",
				__func__, battery->pd_list.now_pd_index, prev_pd_index);
			if (battery->pd_list.now_pd_index != prev_pd_index)
				bPdIndexChanged = true;

			battery->pdic_ps_rdy = true;
		}
		current_pdo = battery->pdic_info.sink_status.current_pdo_num;
		if ((battery->pdic_info.sink_status.power_list[current_pdo].max_voltage / 100) > SEC_INPUT_VOLTAGE_5V)
			battery->hv_pdo = true;
		else
			battery->hv_pdo = false;
		dev_info(battery->dev, "%s: battery->pdic_ps_rdy(%d), hv_pdo(%d)\n",
				__func__, battery->pdic_ps_rdy, battery->hv_pdo);

#if defined(CONFIG_PDIC_PD30)
		if (battery->pdic_info.sink_status.has_apdo) {
			cable_type = SEC_BATTERY_CABLE_PDIC_APDO;
			if (battery->pdic_info.sink_status.power_list[current_pdo].apdo) {
				battery->hv_chg_name = "PDIC_APDO";
				battery->pd_list.now_isApdo = true;
			} else {
				battery->hv_chg_name = "PDIC_FIXED";
				battery->pd_list.now_isApdo = false;
			}

			if (battery->pdic_attach)
				bPrintPDlog = false;
		} else {
			cable_type = SEC_BATTERY_CABLE_PDIC;
			battery->hv_chg_name = "PDIC";
			battery->pd_list.now_isApdo = false;
		}
#else
		cable_type = SEC_BATTERY_CABLE_PDIC;
#if defined(CONFIG_AFC_CHARGER_MODE)
		battery->hv_chg_name = "PDIC";
#endif
#endif //_CONFIG_PDIC_PD30
		battery->muic_cable_type = ATTACHED_DEV_NONE_MUIC;
		battery->input_voltage =
				battery->pdic_info.sink_status.power_list[current_pdo].max_voltage / 100;
		dev_info(battery->dev, "%s: available pdo : %d, current pdo : %d\n", __func__,
			battery->pdic_info.sink_status.available_pdo_num, current_pdo);

		for (i = 1; i <= battery->pdic_info.sink_status.available_pdo_num; i++) {
			bool isUpdated = false;
#if defined(CONFIG_PDIC_PD30)
			bool isApdo = battery->pdic_info.sink_status.power_list[i].apdo;
			bool isAccpet = battery->pdic_info.sink_status.power_list[i].accept;
#endif
			if (!battery->pdic_attach &&
				(battery->pdic_info.sink_status.power_list[i].max_voltage *
				battery->pdic_info.sink_status.power_list[i].max_current) > max_power) {
				max_power = battery->pdic_info.sink_status.power_list[i].max_voltage *
					battery->pdic_info.sink_status.power_list[i].max_current;
				pr_info("%s: max_power = %dmW\n", __func__, max_power);
			}
#if defined(CONFIG_PDIC_PD30)
			if (bPrintPDlog)
				pr_info("%s:%spower_list[%d,%s,%s], maxVol:%d, minVol:%d, maxCur:%d, power:%d\n",
					__func__, i == current_pdo ? "**" : "  ",
					i, isApdo ? "APDO" : "FIXED", isAccpet ? "O" : "X",
					battery->pdic_info.sink_status.power_list[i].max_voltage,
					isApdo ? battery->pdic_info.sink_status.power_list[i].min_voltage : 0,
					battery->pdic_info.sink_status.power_list[i].max_current,
					battery->pdic_info.sink_status.power_list[i].max_voltage *
					battery->pdic_info.sink_status.power_list[i].max_current);

			if (!battery->pdic_attach && isApdo) {
				int max_current = battery->pdic_info.sink_status.power_list[i].max_current;
				int max_volt = battery->pdic_info.sink_status.power_list[i].max_voltage;
				int power_temp;

				max_volt = (max_volt < battery->pdata->apdo_max_volt ?
					max_volt : battery->pdata->apdo_max_volt);
				power_temp = max_volt * max_current / 1000;
				apdo_power = (power_temp > apdo_power ? power_temp : apdo_power);
				pr_info("%s: apdo_power = %dmW\n", __func__, apdo_power);
			}

			/* no change apdo */
			if (!isAccpet || isApdo)
				continue;
#else
			pr_info("%s:%spower_list[%d], voltage : %d, current : %d, power : %d\n",
				__func__, i == current_pdo ? "**" : "  ", i,
				battery->pdic_info.sink_status.power_list[i].max_voltage,
				battery->pdic_info.sink_status.power_list[i].max_current,
				battery->pdic_info.sink_status.power_list[i].max_voltage *
				battery->pdic_info.sink_status.power_list[i].max_current);
#endif
			if ((battery->pdic_info.sink_status.power_list[i].max_voltage *
			     battery->pdic_info.sink_status.power_list[i].max_current) >
			    (pd_charging_charge_power * 1000)) {
				battery->pdic_info.sink_status.power_list[i].max_current =
					(pd_charging_charge_power * 1000) /
					battery->pdic_info.sink_status.power_list[i].max_voltage;
				isUpdated = true;
			}

			if (battery->pdic_info.sink_status.power_list[i].max_current >
			    battery->pdata->max_input_current) {
				isUpdated = true;
				battery->pdic_info.sink_status.power_list[i].max_current =
					battery->pdata->max_input_current;
			}

			if (isUpdated) {
#if defined(CONFIG_PDIC_PD30)
				if (bPrintPDlog)
					pr_info("%s: ->updated [%d,%s,%s], maxVol:%d, minVol:%d, maxCur:%d, power:%d\n",
						__func__, i, isApdo ? "APDO" : "FIXED", isAccpet ? "O" : "X",
						battery->pdic_info.sink_status.power_list[i].max_voltage,
						isApdo ? battery->pdic_info.sink_status.power_list[i].min_voltage : 0,
						battery->pdic_info.sink_status.power_list[i].max_current,
						battery->pdic_info.sink_status.power_list[i].max_voltage *
						battery->pdic_info.sink_status.power_list[i].max_current);
#else
				pr_info("%s: ->updated [%d], voltage : %d, current : %d, power : %d\n", __func__, i,
					battery->pdic_info.sink_status.power_list[i].max_voltage,
					battery->pdic_info.sink_status.power_list[i].max_current,
					battery->pdic_info.sink_status.power_list[i].max_voltage *
					battery->pdic_info.sink_status.power_list[i].max_current);
#endif
			}
		}

		if (!battery->pdic_attach) {
#if defined(CONFIG_PDIC_PD30)
			if (battery->pdic_info.sink_status.has_apdo &&
				!(battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE)) {
				apdo_power = apdo_power > battery->pdata->max_charging_charge_power ?
					battery->pdata->max_charging_charge_power : apdo_power;
				battery->max_charge_power = apdo_power;
				battery->pd_max_charge_power = battery->max_charge_power;
				pr_info("%s: pd_max_charge_power = %dmW\n", __func__, battery->pd_max_charge_power);
			}
#endif
			count_cisd_power_data(&battery->cisd, (max_power / 1000));

			if (make_pd_list(battery) <= 0)
				goto skip_cable_work;
		}
		battery->pdic_attach = true;
#if defined(CONFIG_PDIC_PD30)
		if (is_pd_apdo_wire_type(battery->wire_status) && !bPdIndexChanged &&
			battery->pdic_info.sink_status.power_list[current_pdo].apdo) {
			battery->wire_status = cable_type;
			goto skip_cable_work;
		}
#endif
		break;
	case CCIC_NOTIFY_ID_USB:
		if (usb_typec_info.cable_type == PD_USB_TYPE)
			battery->pd_usb_attached = true;
		dev_info(battery->dev, "%s: CCIC_NOTIFY_ID_USB: %d\n", __func__, battery->pd_usb_attached);
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		mutex_unlock(&battery->typec_notylock);
		return 0;
	default:
		cmd = "ERROR";
		cable_type = -1;
		battery->muic_cable_type = ATTACHED_DEV_NONE_MUIC;
#if defined(CONFIG_AFC_CHARGER_MODE)
		battery->hv_chg_name = "NONE";
#endif
		break;
	}

skip_cable_check:

#if defined(CONFIG_PD_CHARGER_HV_DISABLE) && !defined(CONFIG_SEC_FACTORY)
		if (battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_DISABLED_MUIC) {
			pr_info("%s set SEC_BAT_CURRENT_EVENT_AFC_DISABLE\n", __func__);
			sec_bat_set_current_event(battery,
				SEC_BAT_CURRENT_EVENT_AFC_DISABLE, SEC_BAT_CURRENT_EVENT_AFC_DISABLE);
			__pm_stay_awake(battery->monitor_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					   &battery->monitor_work, 0);
		} else {
			sec_bat_set_current_event(battery,
				0, SEC_BAT_CURRENT_EVENT_AFC_DISABLE);
		}
#endif
	sec_bat_set_misc_event(battery,
		(battery->muic_cable_type == ATTACHED_DEV_UNDEFINED_CHARGING_MUIC ? BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE : 0) |
		(battery->muic_cable_type == ATTACHED_DEV_UNDEFINED_RANGE_MUIC ? BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE : 0),
		BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE);

	if (battery->muic_cable_type == ATTACHED_DEV_HICCUP_MUIC) {
		if (battery->usb_temp_flag || (battery->misc_event & BATT_MISC_EVENT_TEMP_HICCUP_TYPE)) {
			pr_info("%s: Hiccup Set because of USB Temp\n", __func__);
			sec_bat_set_misc_event(battery, BATT_MISC_EVENT_TEMP_HICCUP_TYPE, BATT_MISC_EVENT_TEMP_HICCUP_TYPE);
			battery->usb_temp_flag = false;
		} else {
			pr_info("%s: Hiccup Set because of Water detect\n", __func__);
			sec_bat_set_misc_event(battery, BATT_MISC_EVENT_HICCUP_TYPE, BATT_MISC_EVENT_HICCUP_TYPE);
		}
		battery->hiccup_status = 1;
	} else {
		battery->hiccup_status = 0;
		if (battery->hiccup_clear) {
			sec_bat_set_misc_event(battery,
									0, (BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE));
			battery->hiccup_clear = false;
			pr_info("%s : Hiccup event clear! hiccup clear bit set (%d)\n", __func__, battery->hiccup_clear);
		} else if (battery->misc_event & (BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE)) {
			__pm_stay_awake(battery->monitor_wake_lock);
			queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		}
	}

	/*
	 * Show the charging icon and notification (no sound, vi, haptic)
	 * only if slow insertion is detected by MUIC.
	 */
	sec_bat_set_misc_event(battery,
		(battery->muic_cable_type == ATTACHED_DEV_TIMEOUT_OPEN_MUIC ? BATT_MISC_EVENT_TIMEOUT_OPEN_TYPE : 0),
		 BATT_MISC_EVENT_TIMEOUT_OPEN_TYPE);

	if (cable_type < 0 || cable_type > SEC_BATTERY_CABLE_MAX) {
		dev_info(battery->dev, "%s: ignore event(%d)\n",
			__func__, battery->muic_cable_type);
		goto skip_cable_work;
	} else if ((cable_type == SEC_BATTERY_CABLE_UNKNOWN) &&
		   (battery->status != POWER_SUPPLY_STATUS_DISCHARGING)) {
		battery->cable_type = cable_type;
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		dev_info(battery->dev, "%s: UNKNOWN cable plugin\n", __func__);
		goto skip_cable_work;
	}
	battery->wire_status = cable_type;

	if (battery->wc_tx_enable) {
		int work_delay = 0;

		if (battery->wire_status == SEC_BATTERY_CABLE_NONE) {
			battery->buck_cntl_by_tx = true;
			battery->tx_switch_mode = TX_SWITCH_MODE_OFF;
			battery->tx_switch_mode_change = false;
			battery->tx_switch_start_soc = 0;
		}

		cancel_delayed_work(&battery->wpc_tx_work);
		__pm_stay_awake(battery->wpc_tx_wake_lock);
		if ((is_hv_wire_type(battery->wire_status)) ||
			(is_pd_wire_type(battery->wire_status) && battery->hv_pdo))
			work_delay = battery->pdata->tx_gear_vout_delay;

#if defined(CONFIG_TX_5V_DISABLE)
		if (battery->wire_status == SEC_BATTERY_CABLE_TA)
			work_delay = battery->pdata->pre_afc_work_delay + 500;//add delay more afc check
#endif
		queue_delayed_work(battery->monitor_wqueue,
				&battery->wpc_tx_work, msecs_to_jiffies(work_delay));
	}

	cancel_delayed_work(&battery->cable_work);
	__pm_relax(battery->cable_wake_lock);

	if (cable_type == SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT) {
		/* set current event */
		cancel_delayed_work(&battery->afc_work);
		__pm_relax(battery->afc_wake_lock);
		sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_CHG_LIMIT,
					  (SEC_BAT_CURRENT_EVENT_CHG_LIMIT | SEC_BAT_CURRENT_EVENT_AFC));
		__pm_stay_awake(battery->monitor_wake_lock);
		battery->polling_count = 1;	/* initial value = 1 */
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
	} else if ((battery->wire_status == battery->cable_type) &&
		(((battery->wire_status == SEC_BATTERY_CABLE_USB || battery->wire_status == SEC_BATTERY_CABLE_TA) &&
		battery->pdic_info.sink_status.rp_currentlvl > RP_CURRENT_LEVEL_DEFAULT &&
		!(battery->current_event & SEC_BAT_CURRENT_EVENT_AFC)) ||
		is_hv_wire_type(battery->wire_status))) {
		cancel_delayed_work(&battery->afc_work);
		__pm_relax(battery->afc_wake_lock);
		sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_AFC);

		__pm_stay_awake(battery->monitor_wake_lock);
		battery->polling_count = 1;	/* initial value = 1 */
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
	} else if (cable_type == SEC_BATTERY_CABLE_PREPARE_TA) {
		sec_bat_set_current_event(battery,
			SEC_BAT_CURRENT_EVENT_AFC, SEC_BAT_CURRENT_EVENT_AFC);
		sec_bat_set_charging_current(battery);
		cancel_delayed_work(&battery->afc_work);
		__pm_relax(battery->afc_wake_lock);
	} else {
		__pm_stay_awake(battery->cable_wake_lock);
		if (battery->ta_alert_wa && battery->ta_alert_mode != OCP_NONE) {
			if (!strcmp(cmd, "DETACH")) {
				queue_delayed_work(battery->monitor_wqueue,
					&battery->cable_work, msecs_to_jiffies(3000));
			} else {
				queue_delayed_work(battery->monitor_wqueue,
					&battery->cable_work, 0);
			}
		} else {
			queue_delayed_work(battery->monitor_wqueue,
				&battery->cable_work, 0);
		}
	}

skip_cable_work:
	dev_info(battery->dev, "%s: CMD[%s], CABLE_TYPE[%d]\n", __func__, cmd, cable_type);
	mutex_unlock(&battery->typec_notylock);
	return 0;
}
#else
#if defined(CONFIG_CCIC_NOTIFIER)
static int batt_pdic_handle_notification(struct notifier_block *nb,
		unsigned long action, void *data)
{
	const char *cmd;
	int i, selected_pdo;
	struct sec_battery_info *battery =
		container_of(nb, struct sec_battery_info,
				pdic_nb);
	battery->pdic_info = *(struct pdic_notifier_struct *)data;

	mutex_lock(&battery->batt_handlelock);
	pr_info("%s: pdic_event: %d\n", __func__, battery->pdic_info.event);

	switch (battery->pdic_info.event) {
	case PDIC_NOTIFY_EVENT_DETACH:
		cmd = "DETACH";
		battery->pdic_attach = false;
		battery->init_src_cap = false;
		if (battery->wire_status == SEC_BATTERY_CABLE_PDIC) {
			battery->wire_status = SEC_BATTERY_CABLE_NONE;
			__pm_stay_awake(battery->cable_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					&battery->cable_work, 0);
		}
		break;
	case PDIC_NOTIFY_EVENT_CCIC_ATTACH:
		cmd = "ATTACH";
		break;
	case PDIC_NOTIFY_EVENT_PD_SINK:
		selected_pdo = battery->pdic_info.sink_status.selected_pdo_num;
		cmd = "ATTACH";
		battery->wire_status = SEC_BATTERY_CABLE_PDIC;
		battery->pdic_attach = true;
		battery->input_voltage =
			battery->pdic_info.sink_status.power_list[selected_pdo].max_voltage / 100;

		pr_info("%s: total pdo : %d, selected pdo : %d\n", __func__,
				battery->pdic_info.sink_status.available_pdo_num, selected_pdo);
		for (i = 1; i <= battery->pdic_info.sink_status.available_pdo_num; i++) {
			pr_info("%s: power_list[%d], voltage : %d, current : %d, power : %d\n", __func__, i,
					battery->pdic_info.sink_status.power_list[i].max_voltage,
					battery->pdic_info.sink_status.power_list[i].max_current,
					battery->pdic_info.sink_status.power_list[i].max_voltage *
					battery->pdic_info.sink_status.power_list[i].max_current);
		}
		__pm_stay_awake(battery->cable_wake_lock);
		queue_delayed_work(battery->monitor_wqueue,
				&battery->cable_work, 0);
		break;
	case PDIC_NOTIFY_EVENT_PD_SOURCE:
		cmd = "ATTACH";
		break;
	default:
		cmd = "ERROR";
		break;
	}
	pr_info("%s: CMD=%s, cable_type : %d\n", __func__, cmd, battery->cable_type);
	mutex_unlock(&battery->batt_handlelock);
	return 0;
}
#endif

#if defined(CONFIG_MUIC_NOTIFIER)
static int batt_handle_notification(struct notifier_block *nb,
		unsigned long action, void *data)
{
	const char *cmd;
	int cable_type = SEC_BATTERY_CABLE_NONE;
	struct sec_battery_info *battery =
		container_of(nb, struct sec_battery_info,
			     batt_nb);
	union power_supply_propval value = {0, };

#if defined(CONFIG_CCIC_NOTIFIER)
	CC_NOTI_ATTACH_TYPEDEF *p_noti = (CC_NOTI_ATTACH_TYPEDEF *)data;
	muic_attached_dev_t attached_dev = p_noti->cable_type;
#else
	muic_attached_dev_t attached_dev = *(muic_attached_dev_t *)data;
#endif

	mutex_lock(&battery->batt_handlelock);
	switch (action) {
	case MUIC_NOTIFY_CMD_DETACH:
	case MUIC_NOTIFY_CMD_LOGICALLY_DETACH:
		cmd = "DETACH";
		battery->is_jig_on = false;
		cable_type = SEC_BATTERY_CABLE_NONE;
		battery->muic_cable_type = ATTACHED_DEV_NONE_MUIC;
		break;
	case MUIC_NOTIFY_CMD_ATTACH:
	case MUIC_NOTIFY_CMD_LOGICALLY_ATTACH:
		cmd = "ATTACH";
		cable_type = sec_bat_cable_check(battery, attached_dev);
		battery->muic_cable_type = attached_dev;
		break;
	default:
		cmd = "ERROR";
		cable_type = -1;
		battery->muic_cable_type = ATTACHED_DEV_NONE_MUIC;
		break;
	}

	sec_bat_set_misc_event(battery,
#if !defined(CONFIG_ENG_BATTERY_CONCEPT) && !defined(CONFIG_SEC_FACTORY)
		(battery->muic_cable_type == ATTACHED_DEV_JIG_UART_ON_MUIC ? BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE : 0) |
		(battery->muic_cable_type == ATTACHED_DEV_JIG_USB_ON_MUIC ? BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE : 0) |
#endif
		(battery->muic_cable_type == ATTACHED_DEV_UNDEFINED_RANGE_MUIC ? BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE : 0),
		 BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE);

	if (battery->muic_cable_type == ATTACHED_DEV_HICCUP_MUIC) {
		if (battery->usb_temp_flag || (battery->misc_event & BATT_MISC_EVENT_TEMP_HICCUP_TYPE)) {
			pr_info("%s: Hiccup Set because of USB Temp\n", __func__);
			sec_bat_set_misc_event(battery, BATT_MISC_EVENT_TEMP_HICCUP_TYPE, BATT_MISC_EVENT_TEMP_HICCUP_TYPE);
			battery->usb_temp_flag = false;
		} else {
			pr_info("%s: Hiccup Set because of Water detect\n", __func__);
			sec_bat_set_misc_event(battery, BATT_MISC_EVENT_HICCUP_TYPE, BATT_MISC_EVENT_HICCUP_TYPE);
		}
		battery->hiccup_status = 1;
	} else {
		battery->hiccup_status = 0;
		if (battery->misc_event & (BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE)) {
			__pm_stay_awake(battery->monitor_wake_lock);
			queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		}
	}

#if defined(CONFIG_CCIC_NOTIFIER)
	/* If PD cable is already attached, return this function */
	if (battery->pdic_attach) {
		dev_info(battery->dev, "%s: ignore event pdic attached(%d)\n",
			__func__, battery->pdic_attach);
		mutex_unlock(&battery->batt_handlelock);
		return 0;
	}
#endif

	if (attached_dev == ATTACHED_DEV_MHL_MUIC) {
		mutex_unlock(&battery->batt_handlelock);
		return 0;
	}

	if (cable_type < 0) {
		dev_info(battery->dev, "%s: ignore event(%d)\n",
			__func__, cable_type);
	} else if (cable_type == SEC_BATTERY_CABLE_POWER_SHARING) {
		battery->ps_status = true;
		battery->ps_enable = true;
		battery->wire_status = cable_type;
		dev_info(battery->dev, "%s: power sharing cable plugin\n", __func__);
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC;
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_PACK) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC_PACK;
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_PACK) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC_PACK_HV;
	} else if (cable_type == SEC_BATTERY_CABLE_HV_WIRELESS) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC_HV;
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_STAND) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC_STAND;
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_STAND) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC_STAND_HV;
	} else if (cable_type == SEC_BATTERY_CABLE_PMA_WIRELESS) {
		battery->wc_status = SEC_WIRELESS_PAD_PMA;
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_VEHICLE) {
		battery->wc_status = SEC_WIRELESS_PAD_VEHICLE;
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_VEHICLE) {
		battery->wc_status = SEC_WIRELESS_PAD_VEHICLE_HV;
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_TX) {
		battery->wc_status = SEC_WIRELESS_PAD_TX;
	} else if (cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC_PREPARE_HV_20;
	} else if (cable_type == SEC_BATTERY_CABLE_HV_WIRELESS_20) {
		battery->wc_status = SEC_WIRELESS_PAD_WPC_HV_20;
	} else if ((cable_type == SEC_BATTERY_CABLE_UNKNOWN) &&
		   (battery->status != POWER_SUPPLY_STATUS_DISCHARGING)) {
		battery->cable_type = cable_type;
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		dev_info(battery->dev,
			"%s: UNKNOWN cable plugin\n", __func__);
		mutex_unlock(&battery->batt_handlelock);
		return 0;
	} else {
		battery->wire_status = cable_type;
		if (is_nocharge_type(battery->wire_status) &&
			(battery->wc_status) && (!battery->ps_status))
			cable_type = SEC_BATTERY_CABLE_WIRELESS;
	}
	dev_info(battery->dev,
			"%s: current_cable(%d), wc_status(%d), wire_status(%d)\n",
			__func__, cable_type, battery->wc_status,
			battery->wire_status);

	mutex_unlock(&battery->batt_handlelock);
	if (attached_dev == ATTACHED_DEV_USB_LANHUB_MUIC) {
		if (!strcmp(cmd, "ATTACH")) {
			value.intval = true;
			psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CHARGE_POWERED_OTG_CONTROL,
					value);
			dev_info(battery->dev,
				"%s: Powered OTG cable attached\n", __func__);
		} else {
			value.intval = false;
			psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CHARGE_POWERED_OTG_CONTROL,
					value);
			dev_info(battery->dev,
				"%s: Powered OTG cable detached\n", __func__);
		}
	}

#if defined(CONFIG_AFC_CHARGER_MODE)
	if (!strcmp(cmd, "ATTACH")) {
		if ((battery->muic_cable_type >= ATTACHED_DEV_QC_CHARGER_PREPARE_MUIC) &&
		    (battery->muic_cable_type <= ATTACHED_DEV_QC_CHARGER_9V_MUIC)) {
			battery->hv_chg_name = "QC";
		} else if ((battery->muic_cable_type >= ATTACHED_DEV_AFC_CHARGER_PREPARE_MUIC) &&
			 (battery->muic_cable_type <= ATTACHED_DEV_AFC_CHARGER_ERR_V_DUPLI_MUIC)) {
			battery->hv_chg_name = "AFC";
#if defined(CONFIG_MUIC_HV_12V)
		} else if (battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_12V_MUIC ||
			battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_12V_DUPLI_MUIC) {
			battery->hv_chg_name = "12V";
#endif
		} else
			battery->hv_chg_name = "NONE";
	} else {
		battery->hv_chg_name = "NONE";
	}

	pr_info("%s : HV_CHARGER_NAME(%s)\n",
		__func__, battery->hv_chg_name);
#endif

	if ((cable_type >= 0) &&
	    cable_type <= SEC_BATTERY_CABLE_MAX) {
		if (cable_type == SEC_BATTERY_CABLE_POWER_SHARING) {
			value.intval = battery->ps_enable;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
			__pm_stay_awake(battery->monitor_wake_lock);
			queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		} else if ((cable_type == SEC_BATTERY_CABLE_NONE) && (battery->ps_status)) {
			if (battery->ps_enable) {
				battery->ps_enable = false;
				value.intval = battery->ps_enable;
				psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
			}
			battery->ps_status = false;
			__pm_stay_awake(battery->monitor_wake_lock);
			queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		} else if (cable_type != battery->cable_type) {
			__pm_stay_awake(battery->cable_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					   &battery->cable_work, 0);
		} else {
			dev_info(battery->dev,
				"%s: Cable is Not Changed(%d)\n",
				__func__, battery->cable_type);
		}
	}

	pr_info("%s: CMD=%s, attached_dev=%d\n", __func__, cmd, attached_dev);

	return 0;
}
#endif /* CONFIG_MUIC_NOTIFIER */
#endif

#if defined(CONFIG_VBUS_NOTIFIER)
static int vbus_handle_notification(struct notifier_block *nb,
		unsigned long action, void *data)
{
	vbus_status_t vbus_status = *(vbus_status_t *)data;
	struct sec_battery_info *battery =
		container_of(nb, struct sec_battery_info,
			     vbus_nb);
	union power_supply_propval value = {0, };

	mutex_lock(&battery->batt_handlelock);
	if (battery->muic_cable_type == ATTACHED_DEV_HMT_MUIC &&
		battery->muic_vbus_status != vbus_status &&
		battery->muic_vbus_status == STATUS_VBUS_HIGH &&
		vbus_status == STATUS_VBUS_LOW) {
		msleep(500);
		value.intval = true;
		psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL,
				value);
		dev_info(battery->dev,
			"%s: changed to OTG cable attached\n", __func__);

		battery->wire_status = SEC_BATTERY_CABLE_OTG;
		__pm_stay_awake(battery->cable_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->cable_work, 0);
	}
	pr_info("%s: action=%d, vbus_status=%d\n", __func__, (int)action, vbus_status);
	mutex_unlock(&battery->batt_handlelock);
	battery->muic_vbus_status = vbus_status;

	return 0;
}
#endif

#if !defined(CONFIG_MUIC_NOTIFIER)
void cable_initial_check(struct sec_battery_info *battery)
{
	union power_supply_propval value;

	pr_info("%s : current_cable_type : (%d)\n", __func__, battery->cable_type);

	if (battery->cable_type != SEC_BATTERY_CABLE_NONE) {
		if (battery->cable_type == SEC_BATTERY_CABLE_POWER_SHARING) {
			value.intval =  battery->cable_type;
			psy_do_property("ps", set,
					POWER_SUPPLY_PROP_ONLINE, value);
		} else {
			value.intval =  battery->cable_type;
			psy_do_property("battery", set,
					POWER_SUPPLY_PROP_ONLINE, value);
		}
	} else {
		psy_do_property(battery->pdata->charger_name, get,
				POWER_SUPPLY_PROP_ONLINE, value);
		if (value.intval == SEC_BATTERY_CABLE_WIRELESS) {
			value.intval = 1;
			psy_do_property("wireless", set,
				POWER_SUPPLY_PROP_ONLINE, value);
		}
	}
}
#endif

static void sec_bat_init_chg_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, init_chg_work.work);

	if (battery->cable_type == SEC_BATTERY_CABLE_NONE &&
		!(battery->misc_event & (BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE |
			BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE))) {
		pr_info("%s: disable charging\n", __func__);
		sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
	}
}

static const struct power_supply_desc battery_power_supply_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sec_battery_props,
	.num_properties = ARRAY_SIZE(sec_battery_props),
	.get_property = sec_bat_get_property,
	.set_property = sec_bat_set_property,
};

static const struct power_supply_desc usb_power_supply_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sec_power_props,
	.num_properties = ARRAY_SIZE(sec_power_props),
	.get_property = sec_usb_get_property,
};

static const struct power_supply_desc ac_power_supply_desc = {
	.name = "ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = sec_ac_props,
	.num_properties = ARRAY_SIZE(sec_ac_props),
	.get_property = sec_ac_get_property,
};

static const struct power_supply_desc wireless_power_supply_desc = {
	.name = "wireless",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sec_wireless_props,
	.num_properties = ARRAY_SIZE(sec_wireless_props),
	.get_property = sec_wireless_get_property,
	.set_property = sec_wireless_set_property,
};

static const struct power_supply_desc ps_power_supply_desc = {
	.name = "ps",
	.type = POWER_SUPPLY_TYPE_POWER_SHARING,
	.properties = sec_ps_props,
	.num_properties = ARRAY_SIZE(sec_ps_props),
	.get_property = sec_ps_get_property,
	.set_property = sec_ps_set_property,
};

#if defined(CONFIG_USE_POGO)
static const struct power_supply_desc pogo_power_supply_desc = {
	.name = "pogo",
	.type = POWER_SUPPLY_TYPE_POGO,
	.properties = sec_power_props,
	.num_properties = ARRAY_SIZE(sec_power_props),
	.get_property = sec_pogo_get_property,
	.set_property = sec_pogo_set_property,
};
#endif

#if !defined(CONFIG_SEC_FACTORY)
#define SALE_CODE_STR_LEN		3
static char sales_code_from_cmdline[SALE_CODE_STR_LEN + 1];

static int __init sales_code_setup(char *str)
{
	strlcpy(sales_code_from_cmdline, str,
			ARRAY_SIZE(sales_code_from_cmdline));

	return 1;
}
__setup("androidboot.sales_code=", sales_code_setup);

bool sales_code_is(char *str)
{
	return !strncmp(sales_code_from_cmdline, str,
						SALE_CODE_STR_LEN + 1);
}
#endif

static int sec_battery_probe(struct platform_device *pdev)
{
	sec_battery_platform_data_t *pdata = NULL;
	struct sec_battery_info *battery;
	struct power_supply_config battery_cfg = {};

	int ret = 0;
#ifndef CONFIG_OF
	int i = 0;
#endif
#if defined(CONFIG_STORE_MODE) && !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_DIRECT_CHARGING)
	char direct_charging_source_status[2] = {0, };
#endif
	union power_supply_propval value = {0, };

	dev_info(&pdev->dev,
		"%s: SEC Battery Driver Loading\n", __func__);

	battery = kzalloc(sizeof(*battery), GFP_KERNEL);
	if (!battery)
		return -ENOMEM;

	if (pdev->dev.of_node) {
		pdata = devm_kzalloc(&pdev->dev,
				sizeof(sec_battery_platform_data_t),
				GFP_KERNEL);
		if (!pdata) {
			dev_err(&pdev->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err_bat_free;
		}

		battery->pdata = pdata;

		if (sec_bat_parse_dt(&pdev->dev, battery)) {
			dev_err(&pdev->dev,
				"%s: Failed to get battery dt\n", __func__);
			ret = -EINVAL;
			goto err_bat_free;
		}
	} else {
		pdata = dev_get_platdata(&pdev->dev);
		battery->pdata = pdata;
	}

	platform_set_drvdata(pdev, battery);

	battery->dev = &pdev->dev;

	mutex_init(&battery->adclock);
	mutex_init(&battery->iolock);
	mutex_init(&battery->misclock);
	mutex_init(&battery->txeventlock);
	mutex_init(&battery->batt_handlelock);
	mutex_init(&battery->current_eventlock);
	mutex_init(&battery->typec_notylock);
	mutex_init(&battery->wclock);
	mutex_init(&battery->voutlock);
	mutex_init(&battery->init_soc_updatelock);

	dev_dbg(battery->dev, "%s: ADC init\n", __func__);

#ifdef CONFIG_OF
	adc_init(pdev, battery);
#else
	for (i = 0; i < SEC_BAT_ADC_CHANNEL_NUM; i++)
		adc_init(pdev, pdata, i);
#endif
	battery->monitor_wake_lock = wakeup_source_register(battery->dev, "sec-battery-monitor");
	battery->cable_wake_lock = wakeup_source_register(battery->dev, "sec-battery-cable");
	battery->vbus_wake_lock = wakeup_source_register(battery->dev, "sec-battery-vbus");
	battery->afc_wake_lock = wakeup_source_register(battery->dev, "sec-battery-afc");
	battery->siop_level_wake_lock = wakeup_source_register(battery->dev, "sec-battery-siop_level");
	battery->ext_event_wake_lock = wakeup_source_register(battery->dev, "sec-battery-ext_event");
	battery->wc_headroom_wake_lock = wakeup_source_register(battery->dev, "sec-battery-wc_headroom");
	battery->wpc_tx_wake_lock = wakeup_source_register(battery->dev, "sec-battery-wcp-tx");
	battery->wpc_tx_en_wake_lock = wakeup_source_register(&pdev->dev, "sec-battery-wpc_tx_en");
#if defined(CONFIG_UPDATE_BATTERY_DATA)
	battery->batt_data_wake_lock = wakeup_source_register(battery->dev, "sec-battery-update-data");
#endif
	battery->misc_event_wake_lock = wakeup_source_register(battery->dev, "sec-battery-misc-event");
	battery->tx_event_wake_lock = wakeup_source_register(battery->dev, "sec-battery-tx-event");
#ifdef CONFIG_OF
	battery->parse_mode_dt_wake_lock = wakeup_source_register(battery->dev, "sec-battery-parse_mode_dt");
#endif
	/* initialization of battery info */
	sec_bat_set_charging_status(battery,
			POWER_SUPPLY_STATUS_DISCHARGING);
	battery->health = POWER_SUPPLY_HEALTH_GOOD;
	battery->ta_alert_mode = OCP_NONE;
	battery->present = true;
	battery->is_jig_on = false;
	battery->wdt_kick_disable = 0;

	battery->polling_count = 1;	/* initial value = 1 */
	battery->polling_time = pdata->polling_time[
		SEC_BATTERY_POLLING_TIME_DISCHARGING];
	battery->polling_in_sleep = false;
	battery->polling_short = false;

	battery->check_count = 0;
	battery->check_adc_count = 0;
	battery->check_adc_value = 0;

	battery->input_current = 0;
	battery->charging_current = 0;
#if defined(CONFIG_DUAL_BATTERY)
	battery->main_charging_current = 0;
	battery->sub_charging_current = 0;
#endif
	battery->topoff_current = 0;
	battery->wpc_vout_level = WIRELESS_VOUT_10V;
	battery->wpc_max_vout_level = WIRELESS_VOUT_12_5V;
	battery->charging_start_time = 0;
	battery->charging_passed_time = 0;
	battery->wc_heating_start_time = 0;
	battery->wc_heating_passed_time = 0;
	battery->charging_next_time = 0;
	battery->charging_fullcharged_time = 0;
	battery->siop_level = 100;
	battery->wc_enable = 1;
	battery->wc_enable_cnt = 0;
	battery->wc_enable_cnt_value = 3;
#if defined(CONFIG_ENG_BATTERY_CONCEPT)
	battery->stability_test = 0;
	battery->eng_not_full_status = 0;
	battery->temperature_test_battery = 0x7FFF;
	battery->temperature_test_usb = 0x7FFF;
	battery->temperature_test_wpc = 0x7FFF;
	battery->temperature_test_chg = 0x7FFF;
#if defined(CONFIG_DUAL_BATTERY)
	battery->temperature_test_sub = 0x7FFF;
#endif
	battery->temperature_test_dchg = 0x7FFF;
	battery->temperature_test_blkt = 0x7FFF;
#if defined(CONFIG_STEP_CHARGING)
	battery->test_step_condition = 0x7FFF;
	battery->step_charging_status = -1;
#if defined(CONFIG_DIRECT_CHARGING)
	battery->dc_float_voltage_set = false;
#endif
#endif
	battery->test_max_current = false;
	battery->test_charge_current = false;
#endif
	battery->ps_enable = false;
	battery->wc_status = SEC_WIRELESS_PAD_NONE;
	battery->wc_cv_mode = false;
	battery->wire_status = SEC_BATTERY_CABLE_NONE;

	battery->wc_rx_phm_mode = false;
	battery->wc_tx_enable = false;
	battery->uno_en = false;
	battery->afc_disable = false;
	battery->pd_disable = false;
	battery->buck_cntl_by_tx = false;
	battery->wc_tx_vout = WC_TX_VOUT_5_0V;
	battery->wc_rx_type = NO_DEV;
	battery->tx_mfc_iout = 0;
	battery->tx_uno_iout = 0;
	battery->wc_need_ldo_on = false;

	battery->tx_minduty = battery->pdata->tx_minduty_default;

#if defined(CONFIG_WIRELESS_TX_MODE)
	battery->tx_clear = true;
	battery->tx_clear_cisd = true;
#endif
#if defined(CONFIG_BATTERY_SWELLING)
	battery->swelling_mode = SWELLING_MODE_NONE;
#endif
	battery->charging_block = false;
	battery->chg_limit = false;
	battery->lrp_limit = false;
	battery->lrp_step = LRP_NONE;
	battery->mix_limit = false;
	battery->vbus_limit = false;
	battery->vbus_chg_by_siop = SEC_INPUT_VOLTAGE_0V;
	battery->vbus_chg_by_full = false;
	battery->usb_temp = 0;
#if defined(CONFIG_DIRECT_CHARGING)
	battery->dchg_temp = 0;
#endif
	battery->blkt_temp = 0;
#if defined(CONFIG_ENG_BATTERY_CONCEPT) || defined(CONFIG_SEC_FACTORY)
	battery->cooldown_mode = true;
#endif
	battery->lrp = 0;
	battery->lrp_test = 0;
	battery->skip_swelling = false;
	battery->led_cover = 0;
	battery->hiccup_status = 0;
	battery->hiccup_clear = false;
	battery->ext_event = BATT_EXT_EVENT_NONE;
	battery->tx_retry_case = SEC_BAT_TX_RETRY_NONE;
	battery->tx_misalign_cnt = 0;
	battery->tx_ocp_cnt = 0;
	battery->auto_mode = false;
	battery->update_pd_list = false;

#if defined(CONFIG_DISABLE_MFC_IC)
	psy_do_property(battery->pdata->wireless_charger_name, get,
		POWER_SUPPLY_EXT_PROP_WPC_EN, value);
	sec_bat_set_current_event(battery,
		value.intval ? SEC_BAT_CURRENT_EVENT_WPC_EN : 0, SEC_BAT_CURRENT_EVENT_WPC_EN);
#endif
	sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_USB_100MA, SEC_BAT_CURRENT_EVENT_USB_100MA);

	if (lpcharge) {
		battery->temp_highlimit_threshold =
			battery->pdata->temp_highlimit_threshold_lpm;
		battery->temp_highlimit_recovery =
			battery->pdata->temp_highlimit_recovery_lpm;
		battery->temp_high_threshold =
			battery->pdata->temp_high_threshold_lpm;
		battery->temp_high_recovery =
			battery->pdata->temp_high_recovery_lpm;
		battery->temp_low_recovery =
			battery->pdata->temp_low_recovery_lpm;
		battery->temp_low_threshold =
			battery->pdata->temp_low_threshold_lpm;
	} else {
		battery->temp_highlimit_threshold =
			battery->pdata->temp_highlimit_threshold_normal;
		battery->temp_highlimit_recovery =
			battery->pdata->temp_highlimit_recovery_normal;
		battery->temp_high_threshold =
			battery->pdata->temp_high_threshold_normal;
		battery->temp_high_recovery =
			battery->pdata->temp_high_recovery_normal;
		battery->temp_low_recovery =
			battery->pdata->temp_low_recovery_normal;
		battery->temp_low_threshold =
			battery->pdata->temp_low_threshold_normal;
	}

	battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
	battery->is_recharging = false;
	battery->cable_type = SEC_BATTERY_CABLE_NONE;
	battery->test_mode = 0;
	battery->factory_mode = false;
	battery->store_mode = false;
	battery->charging_enabled = true;
	battery->prev_usb_conf = USB_CURRENT_NONE;
	battery->is_hc_usb = false;
	battery->is_sysovlo = false;
	battery->is_vbatovlo = false;
	battery->is_abnormal_temp = false;
	battery->hv_pdo = false;
	battery->charge_power = 100;

	battery->safety_timer_set = true;
	battery->stop_timer = false;
	battery->prev_safety_time = 0;
	battery->lcd_status = false;
	battery->wc_auth_retried = false;

	battery->wc20_power_class = 0;
#if defined(CONFIG_CALC_TIME_TO_FULL)
	battery->ttf_predict_wc20_charge_current = 0;
#endif
	battery->wc20_vout = 0;

#if defined(CONFIG_BATTERY_CISD)
	battery->usb_overheat_check = false;
	battery->skip_cisd = false;
#endif

#if defined(CONFIG_BATTERY_AGE_FORECAST)
	battery->batt_cycle = -1;
	battery->pdata->age_step = 0;
#endif

	battery->batt_asoc = 100;
	battery->health_change = false;
	battery->usb_temp_flag = false;

	battery->batt_full_capacity = 0;
	battery->usb_slow_chg = false;

	/* '1' means disable usb temp check & high temp/highlimit temp */
	if (temp_control_test == '1')
		sec_bat_set_temp_control_test(battery, true);
	else
		sec_bat_set_temp_control_test(battery, false);

	/* Check High Voltage charging option for wireless charging */
	/* '1' means disabling High Voltage charging */
	if (charging_night_mode == '1')
		sleep_mode = true;
	else
		sleep_mode = false;

	/* Check High Voltage charging option for wired charging */
#if defined(CONFIG_PD_CHARGER_HV_DISABLE)
	/* PD High Voltage charging option for wired charging */
	if (get_pd_disable()) {
		battery->pd_disable = true;
		pr_info("PD wired charging mode is disabled\n");
		sec_bat_set_current_event(battery,
			SEC_BAT_CURRENT_EVENT_HV_DISABLE, SEC_BAT_CURRENT_EVENT_HV_DISABLE);
	}
#else
	if (get_afc_mode() == CH_MODE_AFC_DISABLE_VAL) {
		pr_info("HV wired charging mode is disabled\n");
		sec_bat_set_current_event(battery,
			SEC_BAT_CURRENT_EVENT_HV_DISABLE, SEC_BAT_CURRENT_EVENT_HV_DISABLE);
	}
#endif
	if (fg_reset)
		sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_FG_RESET,
			SEC_BAT_CURRENT_EVENT_FG_RESET);

	battery->pdata->store_mode_charging_max = STORE_MODE_CHARGING_MAX;
	battery->pdata->store_mode_charging_min = STORE_MODE_CHARGING_MIN;

#if !defined(CONFIG_SEC_FACTORY)
	if (sales_code_is("VZW")) {
		dev_err(battery->dev, "%s: Sales is VZW\n", __func__);
		battery->pdata->store_mode_charging_max = STORE_MODE_CHARGING_MAX_VZW;
		battery->pdata->store_mode_charging_min = STORE_MODE_CHARGING_MIN_VZW;
	}
#endif

#if defined(CONFIG_CALC_TIME_TO_FULL)
	battery->timetofull = -1;
#endif

	if (battery->pdata->charger_name == NULL)
		battery->pdata->charger_name = "sec-charger";
	if (battery->pdata->fuelgauge_name == NULL)
		battery->pdata->fuelgauge_name = "sec-fuelgauge";

	/* create work queue */
	battery->monitor_wqueue =
		create_singlethread_workqueue(dev_name(&pdev->dev));
	if (!battery->monitor_wqueue) {
		dev_err(battery->dev,
			"%s: Fail to Create Workqueue\n", __func__);
		goto err_irq;
	}

	sec_bat_get_battery_info(battery);
	/* updates temperatures on boot */
	battery->temperature = battery->raw_bat_temp;

	INIT_DELAYED_WORK(&battery->monitor_work, sec_bat_monitor_work);
	INIT_DELAYED_WORK(&battery->cable_work, sec_bat_cable_work);
	INIT_DELAYED_WORK(&battery->wpc_tx_work, sec_bat_wpc_tx_work);
	INIT_DELAYED_WORK(&battery->wpc_tx_en_work, sec_bat_wpc_tx_en_work);
#if defined(CONFIG_CALC_TIME_TO_FULL)
	INIT_DELAYED_WORK(&battery->timetofull_work, sec_bat_time_to_full_work);
#endif
#if defined(CONFIG_WIRELESS_TX_MODE)
	INIT_DELAYED_WORK(&battery->wpc_txpower_calc_work, sec_bat_txpower_calc_work);
#endif
#if defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
	INIT_DELAYED_WORK(&battery->slowcharging_work, sec_bat_check_slowcharging_work);
#endif
	INIT_DELAYED_WORK(&battery->afc_work, sec_bat_afc_work);
	INIT_DELAYED_WORK(&battery->ext_event_work, sec_bat_ext_event_work);
	INIT_DELAYED_WORK(&battery->siop_level_work, sec_bat_siop_level_work);
	INIT_DELAYED_WORK(&battery->wc_headroom_work, sec_bat_wc_headroom_work);
#if defined(CONFIG_WIRELESS_FIRMWARE_UPDATE)
	INIT_DELAYED_WORK(&battery->fw_init_work, sec_bat_fw_init_work);
#endif
#if defined(CONFIG_UPDATE_BATTERY_DATA)
	INIT_DELAYED_WORK(&battery->batt_data_work, sec_bat_update_data_work);
#endif
	INIT_DELAYED_WORK(&battery->misc_event_work, sec_bat_misc_event_work);
#ifdef CONFIG_OF
	INIT_DELAYED_WORK(&battery->parse_mode_dt_work, sec_bat_parse_mode_dt_work);
#endif
	INIT_DELAYED_WORK(&battery->init_chg_work, sec_bat_init_chg_work);

	switch (pdata->polling_type) {
	case SEC_BATTERY_MONITOR_WORKQUEUE:
		INIT_DELAYED_WORK(&battery->polling_work,
			sec_bat_polling_work);
		break;
	case SEC_BATTERY_MONITOR_ALARM:
		battery->last_poll_time = ktime_get_boottime();
		alarm_init(&battery->polling_alarm, ALARM_BOOTTIME,
			sec_bat_alarm);
		break;
	default:
		break;
	}

#if defined(CONFIG_BATTERY_CISD)
	sec_battery_cisd_init(battery);
#endif
	battery_cfg.drv_data = battery;

	/* init power supplier framework */
	battery->psy_ps = power_supply_register(&pdev->dev, &ps_power_supply_desc, &battery_cfg);
	if (IS_ERR(battery->psy_ps)) {
		ret = PTR_ERR(battery->psy_ps);
		dev_err(battery->dev,
			"%s: Failed to Register psy_ps(%d)\n", __func__, ret);
		goto err_workqueue;
	}
	battery->psy_ps->supplied_to = supply_list;
	battery->psy_ps->num_supplicants = ARRAY_SIZE(supply_list);

	battery->psy_usb = power_supply_register(&pdev->dev, &usb_power_supply_desc, &battery_cfg);
	if (IS_ERR(battery->psy_usb)) {
		ret = PTR_ERR(battery->psy_usb);
		dev_err(battery->dev,
			"%s: Failed to Register psy_usb(%d)\n", __func__, ret);
		goto err_supply_unreg_ps;
	}
	battery->psy_usb->supplied_to = supply_list;
	battery->psy_usb->num_supplicants = ARRAY_SIZE(supply_list);

	battery->psy_ac = power_supply_register(&pdev->dev, &ac_power_supply_desc, &battery_cfg);
	if (IS_ERR(battery->psy_ac)) {
		ret = PTR_ERR(battery->psy_ac);
		dev_err(battery->dev,
			"%s: Failed to Register psy_ac(%d)\n", __func__, ret);
		goto err_supply_unreg_usb;
	}
	battery->psy_ac->supplied_to = supply_list;
	battery->psy_ac->num_supplicants = ARRAY_SIZE(supply_list);

	battery->psy_bat = power_supply_register(&pdev->dev, &battery_power_supply_desc, &battery_cfg);
	if (IS_ERR(battery->psy_bat)) {
		ret = PTR_ERR(battery->psy_bat);
		dev_err(battery->dev,
			"%s: Failed to Register psy_bat(%d)\n", __func__, ret);
		goto err_supply_unreg_ac;
	}

#if defined(CONFIG_USE_POGO)
	battery->psy_pogo = power_supply_register(&pdev->dev, &pogo_power_supply_desc, &battery_cfg);
	if (IS_ERR(battery->psy_pogo)) {
		ret = PTR_ERR(battery->psy_pogo);
		dev_err(battery->dev,
			"%s: Failed to Register psy_pogo(%d)\n", __func__, ret);
		/* pogo itself failed: unwind only the supplies below it */
		goto err_supply_unreg_bat;
	}
#endif

	battery->psy_wireless = power_supply_register(&pdev->dev, &wireless_power_supply_desc, &battery_cfg);
	if (IS_ERR(battery->psy_wireless)) {
		ret = PTR_ERR(battery->psy_wireless);
		dev_err(battery->dev,
			"%s: Failed to Register psy_wireless(%d)\n", __func__, ret);
#if defined(CONFIG_USE_POGO)
		goto err_supply_unreg_pogo;
#else
		goto err_supply_unreg_bat;
#endif
	}
	battery->psy_wireless->supplied_to = supply_list;
	battery->psy_wireless->num_supplicants = ARRAY_SIZE(supply_list);
	if (device_create_file(&battery->psy_wireless->dev, &dev_attr_sgf))
		dev_err(battery->dev,
			"%s: failed to create sgf attr\n", __func__);

	ret = sec_bat_create_attrs(&battery->psy_bat->dev);
	if (ret) {
		dev_err(battery->dev,
			"%s : Failed to create_attrs\n", __func__);
		goto err_req_irq;
	}

	/* initialize battery level */
	value.intval = 0;
	psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_CAPACITY, value);
	battery->capacity = value.intval;

#if defined(CONFIG_WIRELESS_FIRMWARE_UPDATE)
	queue_delayed_work(battery->monitor_wqueue, &battery->fw_init_work, msecs_to_jiffies(2000));
#endif

	/*
	 * Notify the wireless charger driver when sec_battery probe is done.
	 * If wireless charging is possible, POWER_SUPPLY_PROP_ONLINE of the
	 * wireless property will be called.
	 */
	value.intval = 0;
	psy_do_property(battery->pdata->wireless_charger_name, set,
					POWER_SUPPLY_PROP_CHARGE_TYPE, value);

#if defined(CONFIG_STORE_MODE) && !defined(CONFIG_SEC_FACTORY)
	battery->store_mode = true;
	sec_bat_parse_mode_dt(battery);
#if defined(CONFIG_DIRECT_CHARGING)
	direct_charging_source_status[0] = SEC_STORE_MODE;
	direct_charging_source_status[1] = SEC_DIRECT_CHG_CHARGING_SOURCE_SWITCHING;
	value.strval = direct_charging_source_status;
	psy_do_property(battery->pdata->charger_name, set,
		POWER_SUPPLY_EXT_PROP_CHANGE_CHARGING_SOURCE, value);
#endif
#endif

#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	battery->pdic_info.sink_status.rp_currentlvl = RP_CURRENT_LEVEL_NONE;
	manager_notifier_register(&battery->usb_typec_nb,
		usb_typec_handle_notification, MANAGER_NOTIFY_CCIC_BATTERY);
#else
#if defined(CONFIG_MUIC_NOTIFIER)
	muic_notifier_register(&battery->batt_nb,
		batt_handle_notification, MUIC_NOTIFY_DEV_CHARGER);
#else
	cable_initial_check(battery);
#endif
#if defined(CONFIG_CCIC_NOTIFIER)
	pr_info("%s: Registering PDIC_NOTIFY.\n", __func__);
	pdic_notifier_register(&battery->pdic_nb,
		batt_pdic_handle_notification, PDIC_NOTIFY_DEV_BATTERY);
#endif
#endif
#if defined(CONFIG_VBUS_NOTIFIER)
	vbus_notifier_register(&battery->vbus_nb,
		vbus_handle_notification, VBUS_NOTIFY_DEV_CHARGER);
#endif

#if defined(CONFIG_WIRELESS_AUTH)
	sec_bat_misc_init(battery);
#endif

	value.intval = true;
	psy_do_property(battery->pdata->charger_name, set,
		POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX, value);

	/* make fg_reset true again for actual normal booting after recovery kernel is done */
	if (fg_reset && is_boot_recovery()) {
		pr_info("%s: fg_reset(%d) boot_recov(%d)\n",
			__func__, fg_reset, is_boot_recovery());
		psy_do_property(battery->pdata->fuelgauge_name, set,
			POWER_SUPPLY_PROP_ENERGY_NOW, value);
		pr_info("%s: make fg_reset true again for actual normal booting\n", __func__);
	}

	if ((battery->cable_type == SEC_BATTERY_CABLE_NONE) ||
		(battery->cable_type == SEC_BATTERY_CABLE_PREPARE_TA)) {
		queue_delayed_work(battery->monitor_wqueue, &battery->init_chg_work, 0);

		dev_info(&pdev->dev,
				"%s: SEC Battery Driver Monitorwork\n", __func__);
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
	}

	if (battery->pdata->check_battery_callback)
		battery->present = battery->pdata->check_battery_callback();

	ret = sb_full_soc_init(battery);
	dev_info(battery->dev, "%s: sb_full_soc (%s)\n", __func__, (ret) ? "fail" : "success");

	dev_info(battery->dev,
		"%s: SEC Battery Driver Loaded\n", __func__);
	return 0;

err_req_irq:
	device_remove_file(&battery->psy_wireless->dev, &dev_attr_sgf);
	power_supply_unregister(battery->psy_wireless);
#if defined(CONFIG_USE_POGO)
err_supply_unreg_pogo:
	power_supply_unregister(battery->psy_pogo);
#endif
err_supply_unreg_bat:
	power_supply_unregister(battery->psy_bat);
err_supply_unreg_ac:
	power_supply_unregister(battery->psy_ac);
err_supply_unreg_usb:
	power_supply_unregister(battery->psy_usb);
err_supply_unreg_ps:
	power_supply_unregister(battery->psy_ps);
err_workqueue:
	destroy_workqueue(battery->monitor_wqueue);
err_irq:
	wakeup_source_unregister(battery->monitor_wake_lock);
	wakeup_source_unregister(battery->cable_wake_lock);
	wakeup_source_unregister(battery->vbus_wake_lock);
	wakeup_source_unregister(battery->afc_wake_lock);
	wakeup_source_unregister(battery->siop_level_wake_lock);
	wakeup_source_unregister(battery->ext_event_wake_lock);
	wakeup_source_unregister(battery->wc_headroom_wake_lock);
	wakeup_source_unregister(battery->wpc_tx_wake_lock);
	wakeup_source_unregister(battery->wpc_tx_en_wake_lock);
#if defined(CONFIG_UPDATE_BATTERY_DATA)
	wakeup_source_unregister(battery->batt_data_wake_lock);
#endif
	wakeup_source_unregister(battery->misc_event_wake_lock);
	wakeup_source_unregister(battery->tx_event_wake_lock);
#ifdef CONFIG_OF
	wakeup_source_unregister(battery->parse_mode_dt_wake_lock);
#endif

	mutex_destroy(&battery->adclock);
	mutex_destroy(&battery->iolock);
	mutex_destroy(&battery->misclock);
	mutex_destroy(&battery->txeventlock);
	mutex_destroy(&battery->batt_handlelock);
	mutex_destroy(&battery->current_eventlock);
	mutex_destroy(&battery->typec_notylock);
	mutex_destroy(&battery->wclock);
	mutex_destroy(&battery->voutlock);
	mutex_destroy(&battery->init_soc_updatelock);
err_bat_free:
	kfree(battery);

	return ret;
}

static int sec_battery_remove(struct platform_device *pdev)
{
	struct sec_battery_info *battery = platform_get_drvdata(pdev);
#ifndef CONFIG_OF
	int i;
#endif

	pr_info("%s: ++\n", __func__);

	/*
	 * Stop the charger/USB notifiers first: their callbacks resolve back
	 * to this struct via container_of(), so they must not be able to fire
	 * after battery is freed below.
	 */
#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	manager_notifier_unregister(&battery->usb_typec_nb);
#else
#if defined(CONFIG_MUIC_NOTIFIER)
	muic_notifier_unregister(&battery->batt_nb);
#endif
#if defined(CONFIG_CCIC_NOTIFIER)
	pdic_notifier_unregister(&battery->pdic_nb);
#endif
#endif
#if defined(CONFIG_VBUS_NOTIFIER)
	vbus_notifier_unregister(&battery->vbus_nb);
#endif

	/*
	 * Drain monitor_wqueue before cancelling polling_work.  A monitor_work
	 * already executing on monitor_wqueue calls sec_bat_set_polling(),
	 * which re-arms polling_work on system_wq; if polling_work were
	 * cancelled first, that re-arm would survive and later queue
	 * monitor_work onto the destroyed monitor_wqueue.
	 */
	flush_workqueue(battery->monitor_wqueue);

	switch (battery->pdata->polling_type) {
	case SEC_BATTERY_MONITOR_WORKQUEUE:
		cancel_delayed_work_sync(&battery->polling_work);
		break;
	case SEC_BATTERY_MONITOR_ALARM:
		alarm_cancel(&battery->polling_alarm);
		break;
	default:
		break;
	}

	destroy_workqueue(battery->monitor_wqueue);
	wakeup_source_unregister(battery->monitor_wake_lock);
	wakeup_source_unregister(battery->cable_wake_lock);
	wakeup_source_unregister(battery->vbus_wake_lock);
	wakeup_source_unregister(battery->afc_wake_lock);
	wakeup_source_unregister(battery->siop_level_wake_lock);
	wakeup_source_unregister(battery->ext_event_wake_lock);
	wakeup_source_unregister(battery->misc_event_wake_lock);
	wakeup_source_unregister(battery->tx_event_wake_lock);
	wakeup_source_unregister(battery->wc_headroom_wake_lock);
	wakeup_source_unregister(battery->wpc_tx_wake_lock);
	wakeup_source_unregister(battery->wpc_tx_en_wake_lock);
#if defined(CONFIG_UPDATE_BATTERY_DATA)
	wakeup_source_unregister(battery->batt_data_wake_lock);
#endif
#ifdef CONFIG_OF
	wakeup_source_unregister(battery->parse_mode_dt_wake_lock);
#endif

	mutex_destroy(&battery->adclock);
	mutex_destroy(&battery->iolock);
	mutex_destroy(&battery->misclock);
	mutex_destroy(&battery->txeventlock);
	mutex_destroy(&battery->batt_handlelock);
	mutex_destroy(&battery->current_eventlock);
	mutex_destroy(&battery->typec_notylock);
	mutex_destroy(&battery->wclock);
	mutex_destroy(&battery->voutlock);
	mutex_destroy(&battery->init_soc_updatelock);
#ifdef CONFIG_OF
	adc_exit(battery);
#else
	for (i = 0; i < SEC_BAT_ADC_CHANNEL_NUM; i++)
		adc_exit(battery->pdata, i);
#endif
	sb_full_soc_exit(battery);
#if defined(CONFIG_WIRELESS_AUTH)
	sec_bat_misc_exit();
#endif
	power_supply_unregister(battery->psy_ps);
	device_remove_file(&battery->psy_wireless->dev, &dev_attr_sgf);
	power_supply_unregister(battery->psy_wireless);
	power_supply_unregister(battery->psy_ac);
	power_supply_unregister(battery->psy_usb);
	power_supply_unregister(battery->psy_bat);
#if defined(CONFIG_USE_POGO)
	power_supply_unregister(battery->psy_pogo);
#endif

	kfree(battery);

	pr_info("%s: --\n", __func__);

	return 0;
}

static int sec_battery_prepare(struct device *dev)
{
	struct sec_battery_info *battery = dev_get_drvdata(dev);

	dev_info(battery->dev, "%s: Start\n", __func__);

	switch (battery->pdata->polling_type) {
	case SEC_BATTERY_MONITOR_WORKQUEUE:
		cancel_delayed_work(&battery->polling_work);
		break;
	case SEC_BATTERY_MONITOR_ALARM:
		alarm_cancel(&battery->polling_alarm);
		break;
	default:
		break;
	}

	/* monitor_wake_lock must be released before cancelling monitor_work */
	__pm_relax(battery->monitor_wake_lock);
	cancel_delayed_work_sync(&battery->monitor_work);

	battery->polling_in_sleep = true;

	sec_bat_set_polling(battery);

	/*
	 * Cancel the polling work that sec_bat_set_polling() may have
	 * armed: there is no polling while suspended.
	 */
	if (battery->pdata->polling_type ==
		SEC_BATTERY_MONITOR_WORKQUEUE)
		cancel_delayed_work(&battery->polling_work);

	dev_info(battery->dev, "%s: End\n", __func__);

	return 0;
}

static int sec_battery_suspend(struct device *dev)
{
	return 0;
}

static int sec_battery_resume(struct device *dev)
{
	return 0;
}

static void sec_battery_complete(struct device *dev)
{
	struct sec_battery_info *battery = dev_get_drvdata(dev);

	dev_info(battery->dev, "%s: Start\n", __func__);

	/* cancel current alarm and reset after monitor work */
	if (battery->pdata->polling_type == SEC_BATTERY_MONITOR_ALARM)
		alarm_cancel(&battery->polling_alarm);

	__pm_stay_awake(battery->monitor_wake_lock);
	queue_delayed_work(battery->monitor_wqueue,
		&battery->monitor_work, 0);

	dev_info(battery->dev, "%s: End\n", __func__);
}

static void sec_battery_shutdown(struct platform_device *pdev)
{
	struct sec_battery_info *battery = platform_get_drvdata(pdev);

	pr_info("%s: ++\n", __func__);

	switch (battery->pdata->polling_type) {
	case SEC_BATTERY_MONITOR_WORKQUEUE:
		cancel_delayed_work(&battery->polling_work);
		break;
	case SEC_BATTERY_MONITOR_ALARM:
		alarm_cancel(&battery->polling_alarm);
		break;
	default:
		break;
	}

	pr_info("%s: --\n", __func__);
}

#ifdef CONFIG_OF
static const struct of_device_id sec_battery_dt_ids[] = {
	{ .compatible = "samsung,sec-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, sec_battery_dt_ids);
#endif /* CONFIG_OF */

static const struct dev_pm_ops sec_battery_pm_ops = {
	.prepare = sec_battery_prepare,
	.suspend = sec_battery_suspend,
	.resume = sec_battery_resume,
	.complete = sec_battery_complete,
};

static struct platform_driver sec_battery_driver = {
	.driver = {
		   .name = "sec-battery",
		   .owner = THIS_MODULE,
		   .pm = &sec_battery_pm_ops,
#ifdef CONFIG_OF
		.of_match_table = sec_battery_dt_ids,
#endif
	},
	.probe = sec_battery_probe,
	.remove = sec_battery_remove,
	.shutdown = sec_battery_shutdown,
};

static int sec_battery_init(void)
{
	return platform_driver_register(&sec_battery_driver);
}

static void sec_battery_exit(void)
{
	platform_driver_unregister(&sec_battery_driver);
}

late_initcall(sec_battery_init);
module_exit(sec_battery_exit);

MODULE_DESCRIPTION("Samsung Battery Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
