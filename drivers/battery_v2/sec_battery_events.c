/*
 *  sec_battery_events.c
 *  Samsung Mobile Battery Driver - event producers and debug/statistics
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"
#include <linux/sec_debug.h>

void sec_bat_set_misc_event(struct sec_battery_info *battery,
	unsigned int misc_event_val, unsigned int misc_event_mask)
{
	unsigned int prev;

	mutex_lock(&battery->misclock);

	prev = battery->misc_event;
	battery->misc_event &= ~misc_event_mask;
	battery->misc_event |= misc_event_val;

	pr_info("%s: misc event before(0x%x), after(0x%x)\n",
		__func__, prev, battery->misc_event);

	/*
	 * Compare with the previous event and queue the work while still
	 * holding misclock: sec_bat_misc_event_work() updates prev_misc_event
	 * under the same lock, so this read is now race-free.
	 */
	if (battery->prev_misc_event != battery->misc_event) {
		cancel_delayed_work(&battery->misc_event_work);
		__pm_stay_awake(battery->misc_event_wake_lock);
		queue_delayed_work(battery->monitor_wqueue,
			&battery->misc_event_work, 0);
	}

	mutex_unlock(&battery->misclock);
}
void sec_bat_set_tx_event(struct sec_battery_info *battery,
	unsigned int tx_event_val, unsigned int tx_event_mask)
{
	unsigned int temp;

	mutex_lock(&battery->txeventlock);

	temp = battery->tx_event;
	battery->tx_event &= ~tx_event_mask;
	battery->tx_event |= tx_event_val;

	pr_info("@Tx_Mode %s: val(0x%x), mask(0x%x), tx event before(0x%x), after(0x%x)\n",
		__func__, tx_event_val, tx_event_mask, temp, battery->tx_event);

	pr_info("@Tx_Mode %s: tx event before(0x%x), after(0x%x)\n",
		__func__, temp, battery->tx_event);

	if (temp != battery->tx_event) {
		/* Assure receiving tx_event to App for sleep case */
		__pm_wakeup_event(battery->tx_event_wake_lock, jiffies_to_msecs(HZ * 2));
		power_supply_changed(battery->psy_bat);
	}

	mutex_unlock(&battery->txeventlock);
}
void sec_bat_set_current_event(struct sec_battery_info *battery,
			      unsigned int current_event_val, unsigned int current_event_mask)
{
	unsigned int temp;

	mutex_lock(&battery->current_eventlock);

	temp = battery->current_event;
	battery->current_event &= ~current_event_mask;
	battery->current_event |= current_event_val;

	pr_debug("%s: current event before(0x%x), after(0x%x)\n",
		__func__, temp, battery->current_event);

	mutex_unlock(&battery->current_eventlock);
}
void sec_bat_set_temp_control_test(struct sec_battery_info *battery,
			      bool temp_enable)
{
	if (temp_enable) {
		pr_info("%s : BATT_TEMP_CONTROL_TEST ENABLE\n", __func__);
		sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_TEMP_CTRL_TEST,
			SEC_BAT_CURRENT_EVENT_TEMP_CTRL_TEST);
		battery->pdata->usb_temp_check_type_backup = battery->pdata->usb_temp_check_type;
		battery->pdata->usb_temp_check_type = 0;

		battery->pdata->temp_highlimit_threshold_normal_backup =
			battery->pdata->temp_highlimit_threshold_normal;
		battery->pdata->temp_highlimit_threshold_normal = 990;
	} else {
		pr_info("%s : BATT_TEMP_CONTROL_TEST END\n", __func__);
		sec_bat_set_current_event(battery, 0,
			SEC_BAT_CURRENT_EVENT_TEMP_CTRL_TEST);
		if (!battery->pdata->usb_temp_check_type)
			battery->pdata->usb_temp_check_type = battery->pdata->usb_temp_check_type_backup;

		if (battery->pdata->temp_highlimit_threshold_normal == 990)
			battery->pdata->temp_highlimit_threshold_normal =
				battery->pdata->temp_highlimit_threshold_normal_backup;
	}
}
void sec_bat_check_battery_health(struct sec_battery_info *battery)
{
	static battery_health_condition default_table[3] = {
		{.cycle = 900, .asoc = 75}, {.cycle = 1200, .asoc = 65}, {.cycle = 1500, .asoc = 55}
	};

	battery_health_condition *ptable = default_table;
	battery_health_condition state;
	int i, battery_health, size = BATTERY_HEALTH_MAX;

	if (battery->pdata->health_condition == NULL) {
		/*
		 * If a new type is added to misc_battery_health, default table
		 * cannot verify the actual state except "bad". If you want to
		 * return the correct values for all states, add a table that
		 * matches the state added to the dt file.
		 */
		pr_info("%s: does not set health_condition_table, use default table\n", __func__);
		size = 3;
	} else {
		ptable = battery->pdata->health_condition;
	}

	/* Checking Cycle and ASoC */
	state.cycle = state.asoc = BATTERY_HEALTH_BAD;
	for (i = size - 1; i >= 0; i--) {
#if defined(CONFIG_BATTERY_AGE_FORECAST)
		if (ptable[i].cycle >= (battery->batt_cycle % 10000))
			state.cycle = i + BATTERY_HEALTH_GOOD;
#endif
		if (ptable[i].asoc <= battery->batt_asoc)
			state.asoc = i + BATTERY_HEALTH_GOOD;
	}
	battery_health = max(state.cycle, state.asoc);
	pr_info("%s: update battery_health(%d), (%d - %d)\n",
		__func__, battery_health, state.cycle, state.asoc);
	/* Update battery health */
	sec_bat_set_misc_event(battery,
		(battery_health << BATTERY_HEALTH_SHIFT), BATT_MISC_EVENT_BATTERY_HEALTH);
}
void sec_bat_get_battery_info(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	char str[1024] = {0, };

	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_VOLTAGE_NOW, value);
	battery->voltage_now = value.intval;

	value.intval = SEC_BATTERY_VOLTAGE_AVERAGE;
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_VOLTAGE_AVG, value);
	battery->voltage_avg = value.intval;

	/* Do not call it to reduce time after cable_work; this function calls FG full log */
	if (!(battery->current_event & SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL)) {
		value.intval = SEC_BATTERY_VOLTAGE_OCV;
		psy_do_property(battery->pdata->fuelgauge_name, get,
				POWER_SUPPLY_PROP_VOLTAGE_AVG, value);
		battery->voltage_ocv = value.intval;
	}

#if defined(CONFIG_DUAL_BATTERY)
	/* get main pack voltage */
	value.intval = SEC_DUAL_BATTERY_MAIN;
	psy_do_property(battery->pdata->dual_battery_name, get,
		POWER_SUPPLY_PROP_VOLTAGE_AVG, value);
	battery->voltage_avg_main = value.intval;

	/* get sub pack voltage */
	value.intval = SEC_DUAL_BATTERY_SUB;
	psy_do_property(battery->pdata->dual_battery_name, get,
		POWER_SUPPLY_PROP_VOLTAGE_AVG, value);
	battery->voltage_avg_sub = value.intval;

	/* get main current */
	value.intval = SEC_DUAL_BATTERY_MAIN;
	psy_do_property(battery->pdata->dual_battery_name, get,
		POWER_SUPPLY_PROP_CURRENT_AVG, value);
	battery->current_now_main = value.intval;

	/* get sub current */
	value.intval = SEC_DUAL_BATTERY_SUB;
	psy_do_property(battery->pdata->dual_battery_name, get,
		POWER_SUPPLY_PROP_CURRENT_AVG, value);
	battery->current_now_sub = value.intval;

#if defined(CONFIG_DUAL_BATTERY_CELL_SENSING)
	/* get main cell voltage */
	battery->voltage_cell_main = battery->voltage_now;

	/* get sub cell voltage */
	psy_do_property(battery->pdata->charger_name, get,
		POWER_SUPPLY_PROP_VOLTAGE_NOW, value);

	pr_info("%s : main delta = %dmV, sub delta = %dmV\n", __func__,
		battery->voltage_avg_main - battery->voltage_cell_main,
		battery->voltage_avg_sub - value.intval); // debug

	if (value.intval != 0)
		battery->voltage_cell_sub = value.intval;
	else {
		battery->voltage_cell_sub = battery->voltage_avg_sub;
		pr_info("%s : sub cell voltage is 0mV\n", __func__);
	}
#endif
#endif

	value.intval = SEC_BATTERY_CURRENT_MA;
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_CURRENT_NOW, value);
	battery->current_now = value.intval;

	value.intval = SEC_BATTERY_CURRENT_MA;
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_CURRENT_AVG, value);
	battery->current_avg = value.intval;

#if !defined(CONFIG_FUELGAUGE_SM5705)
	value.intval = SEC_BATTERY_ISYS_AVG_MA;
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_EXT_PROP_MEASURE_SYS, value);
	battery->current_sys_avg = value.intval;

	value.intval = SEC_BATTERY_ISYS_MA;
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_EXT_PROP_MEASURE_SYS, value);
	battery->current_sys = value.intval;
#endif

	/* input current limit in charger */
	psy_do_property(battery->pdata->charger_name, get,
		POWER_SUPPLY_PROP_CURRENT_MAX, value);
	battery->current_max = value.intval;

	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_CHARGE_COUNTER, value);
	battery->charge_counter = value.intval;

	/* check abnormal status for wireless charging */
	if (!(battery->current_event & SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL) &&
		(is_wireless_type(battery->cable_type) || battery->wc_tx_enable)) {
		value.intval = (battery->status == POWER_SUPPLY_STATUS_FULL) ?
			100 : battery->capacity;
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_ENERGY_NOW, value);
	}
#if defined(CONFIG_WIRELESS_CHARGER_MFC)
	value.intval = (battery->status == POWER_SUPPLY_STATUS_FULL) ?
		100 : battery->capacity;
	psy_do_property(battery->pdata->wireless_charger_name, set,
		POWER_SUPPLY_PROP_CAPACITY, value);
#endif

	sec_bat_get_temperature_info(battery);

	mutex_lock(&battery->init_soc_updatelock);
	/* To get SOC value (NOT raw SOC), need to reset value */
	value.intval = 0;
	psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_CAPACITY, value);
	/*
	 * If the battery status was full, and SOC wasn't 100% yet,
	 * then ignore FG SOC, and report (previous SOC + 1)%.
	 */
	battery->capacity = value.intval;
	mutex_unlock(&battery->init_soc_updatelock);

	/* voltage information */
#if defined(CONFIG_DUAL_BATTERY_CELL_SENSING)
	snprintf(str, sizeof(str), "%s:Vnow(%dmV),Vavg(%dmV),Vmc(%dmV),Vmp(%dmV),Vsc(%dmV),Vsp(%dmV),", __func__,
		battery->voltage_now, battery->voltage_avg,
		battery->voltage_cell_main, battery->voltage_avg_main,
		battery->voltage_cell_sub, battery->voltage_avg_sub
	);
#elif defined(CONFIG_DUAL_BATTERY)
	snprintf(str, sizeof(str), "%s:Vnow(%dmV),Vavg(%dmV),Vmp(%dmV),Vsp(%dmV),", __func__,
		battery->voltage_now, battery->voltage_avg,
		battery->voltage_avg_main, battery->voltage_avg_sub
	);

#else
	snprintf(str, sizeof(str), "%s:Vnow(%dmV),Vavg(%dmV),", __func__,
		battery->voltage_now, battery->voltage_avg
	);
#endif

	/* current information */
#if defined(CONFIG_DUAL_BATTERY)
	snprintf(str + strlen(str), sizeof(str) - strlen(str), "Inow(%dmA),Iavg(%dmA),Isysavg(%dmA),Inow_m(%dmA),Inow_s(%dmA),Imax(%dmA),Ichg(%dmA),Ichg_m(%dmA),Ichg_s(%dmA),SOC(%d%%),",
		battery->current_now, battery->current_avg,
		battery->current_sys_avg, battery->current_now_main,
		battery->current_now_sub, battery->current_max,
		battery->charging_current, battery->main_charging_current,
		battery->sub_charging_current, battery->capacity
	);
#else
	snprintf(str + strlen(str), sizeof(str) - strlen(str), "Inow(%dmA),Iavg(%dmA),Isysavg(%dmA),Imax(%dmA),Ichg(%dmA),SOC(%d%%),",
		battery->current_now, battery->current_avg,
		battery->current_sys_avg, battery->current_max,
		battery->charging_current, battery->capacity
	);
#endif

	/* temperature information */
#if defined(CONFIG_DIRECT_CHARGING)
	snprintf(str + strlen(str), sizeof(str) - strlen(str), "Tdchg(%d),",
		battery->dchg_temp
	);
#endif
#if defined(CONFIG_DUAL_BATTERY)
	snprintf(str + strlen(str), sizeof(str) - strlen(str), "Tsub(%d),",
		battery->sub_bat_temp
	);
#endif
	snprintf(str + strlen(str), sizeof(str) - strlen(str), "Tbat(%d),Tusb(%d),Tchg(%d),Twpc(%d),Tblkt(%d), lrp(%d)\n",
		battery->temperature, battery->usb_temp,
		battery->chg_temp, battery->wpc_temp,
		battery->blkt_temp, battery->lrp
	);

	pr_info("%s", str);
	battery_last_dcvs(battery->capacity, battery->voltage_avg, battery->temperature, battery->current_avg);
}
void sec_bat_ext_event_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
			struct sec_battery_info, ext_event_work.work);

	union power_supply_propval value = {0, };

	if (battery->wc_tx_enable) {
		/* TX ON state */
		if (battery->ext_event & BATT_EXT_EVENT_CAMERA) {
			pr_info("@Tx_Mode %s: Camera ON, TX OFF\n", __func__);
			sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_CAMERA_ON, BATT_TX_EVENT_WIRELESS_TX_CAMERA_ON);
			sec_wireless_set_tx_enable(battery, false);
		} else if (battery->ext_event & BATT_EXT_EVENT_DEX) {
			pr_info("@Tx_Mode %s: Dex ON, TX OFF\n", __func__);
			sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_OTG_ON, BATT_TX_EVENT_WIRELESS_TX_OTG_ON);
			sec_wireless_set_tx_enable(battery, false);
		} else if (battery->ext_event & BATT_EXT_EVENT_CALL) {
			pr_info("@Tx_Mode %s: Call ON, TX OFF\n", __func__);
			battery->tx_retry_case |= SEC_BAT_TX_RETRY_CALL;
			sec_wireless_set_tx_enable(battery, false);
			/* clear tx all event */
			sec_bat_set_tx_event(battery, 0, BATT_TX_EVENT_WIRELESS_ALL_MASK);
		}
	} else {
		/* TX OFF state, only the call scenario matters */
		if (battery->ext_event & BATT_EXT_EVENT_CALL) {
			pr_info("@Tx_Mode %s: Call ON\n", __func__);

			value.intval = BATT_EXT_EVENT_CALL;
			psy_do_property(battery->pdata->wireless_charger_name, set,
							POWER_SUPPLY_EXT_PROP_CALL_EVENT, value);

			if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_PACK ||
				battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_PACK ||
				battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_TX) {
				pr_info("%s : Call is on during Wireless Pack or TX\n", __func__);
				battery->wc_rx_phm_mode = true;
			}
			if (battery->tx_retry_case != SEC_BAT_TX_RETRY_NONE) {
				pr_info("@Tx_Mode %s: TX OFF because of other reason(retry:0x%x), save call retry case\n",
					__func__, battery->tx_retry_case);
				battery->tx_retry_case |= SEC_BAT_TX_RETRY_CALL;
			}
		} else {
			pr_info("@Tx_Mode %s: Call OFF\n", __func__);

			value.intval = BATT_EXT_EVENT_NONE;
			psy_do_property(battery->pdata->wireless_charger_name, set,
							POWER_SUPPLY_EXT_PROP_CALL_EVENT, value);

			/* check the diff between current and previous ext_event state */
			if (battery->tx_retry_case & SEC_BAT_TX_RETRY_CALL) {
				battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_CALL;
				if (!battery->tx_retry_case) {
					pr_info("@Tx_Mode %s: Call OFF, TX Retry\n", __func__);
					sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_RETRY, BATT_TX_EVENT_WIRELESS_TX_RETRY);
				}
			} else if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_PACK ||
				battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_PACK ||
				battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_TX) {
				pr_info("%s : Call is off during Wireless Pack or TX\n", __func__);
			}

			/* process escape phm */
			if (battery->wc_rx_phm_mode) {
#if defined(CONFIG_DISABLE_MFC_IC)
				pr_info("%s: ESCAPE PHM STEP 1\n", __func__);
				sec_bat_set_mfc_on(battery);
#else
				pr_info("%s: ESCAPE PHM STEP 1 - WC CONTROL: Enable\n", __func__);
				gpio_direction_output(battery->pdata->wpc_en, 0);
#endif
				msleep(100);

#if defined(CONFIG_DISABLE_MFC_IC)
				pr_info("%s: ESCAPE PHM STEP 2\n", __func__);
				sec_bat_set_mfc_off(battery, false);
#else
				pr_info("%s: ESCAPE PHM STEP 2 - WC CONTROL: Disable\n", __func__);
				gpio_direction_output(battery->pdata->wpc_en, 1);
#endif
				msleep(510);

#if defined(CONFIG_DISABLE_MFC_IC)
				pr_info("%s: ESCAPE PHM STEP 3\n", __func__);
				sec_bat_set_mfc_on(battery);
#else
				pr_info("%s: ESCAPE PHM STEP 3 - WC CONTROL: Enable\n", __func__);
				gpio_direction_output(battery->pdata->wpc_en, 0);
#endif
			}
			battery->wc_rx_phm_mode = false;
		}
	}

	__pm_relax(battery->ext_event_wake_lock);
}
void sec_bat_misc_event_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, misc_event_work.work);
	unsigned int prev_misc_event, misc_event;
	int xor_misc_event;

	mutex_lock(&battery->misclock);
	prev_misc_event = battery->prev_misc_event;
	misc_event = battery->misc_event;
	mutex_unlock(&battery->misclock);

	xor_misc_event = prev_misc_event ^ misc_event;

	if ((xor_misc_event & (BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE |
		BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE)) &&
		is_nocharge_type(battery->cable_type)) {
		if (misc_event & (BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE |
			BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE))
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
		else if (prev_misc_event & (BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE |
			BATT_MISC_EVENT_HICCUP_TYPE | BATT_MISC_EVENT_TEMP_HICCUP_TYPE))
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
	}

	pr_info("%s: change misc event(0x%x --> 0x%x)\n",
		__func__, prev_misc_event, misc_event);

	mutex_lock(&battery->misclock);
	battery->prev_misc_event = battery->misc_event;
	mutex_unlock(&battery->misclock);

	__pm_relax(battery->misc_event_wake_lock);

	__pm_stay_awake(battery->monitor_wake_lock);
	queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
}
void sec_bat_calculate_safety_time(struct sec_battery_info *battery)
{
	unsigned long long expired_time = battery->expired_time;
	struct timespec ts = {0, };
	int curr = 0;
	int input_power = 0;
	int float_voltage = battery->pdata->chg_float_voltage /
		battery->pdata->chg_float_voltage_conv;
	int charging_power = battery->charging_current * float_voltage;
	static int discharging_cnt;
#if defined(CONFIG_DIRECT_CHARGING)
	int direct_chg_done = 0;
	union power_supply_propval value = {0,};
#endif

#if defined(CONFIG_DIRECT_CHARGING)
	psy_do_property(battery->pdata->charger_name, get,
					POWER_SUPPLY_EXT_PROP_DIRECT_DONE, value);
	direct_chg_done = value.intval;

	if (is_pd_apdo_wire_type(battery->cable_type) && (battery->pd_list.now_isApdo) && (!battery->chg_limit) && !battery->lrp_limit)
		input_power = battery->pd_max_charge_power * 1000;
	else if (is_pd_apdo_wire_type(battery->cable_type) && direct_chg_done && (!battery->chg_limit) && !battery->lrp_limit)
		input_power = battery->pd_max_charge_power * 1000;
	else
		input_power = battery->current_max * battery->input_voltage * 100;

	pr_debug("%s : current_max(%d),charging_current(%d),input_voltage(%d),pd_max_charge_power(%d),chg_limit(%d),mix_limit(%d),direct_chg_done(%d) lrp_limit(%d)\n",
		__func__, battery->current_max, battery->charging_current, battery->input_voltage,
		battery->pd_max_charge_power, battery->chg_limit, battery->mix_limit, direct_chg_done, battery->lrp_limit);
#else
	input_power = battery->current_max * battery->input_voltage * 100;
#endif

	if (battery->current_avg < 0)
		discharging_cnt++;
	else
		discharging_cnt = 0;

	if (discharging_cnt >= 5) {
		battery->expired_time = battery->pdata->expired_time;
		battery->prev_safety_time = 0;
		pr_info("%s : SAFETY TIME RESET! DISCHARGING CNT(%d)\n",
			__func__, discharging_cnt);
		discharging_cnt = 0;
		return;
	} else if ((battery->lcd_status || battery->wc_tx_enable) && battery->stop_timer) {
		battery->prev_safety_time = 0;
		return;
	}

	get_monotonic_boottime(&ts);

	if (battery->prev_safety_time == 0)
		battery->prev_safety_time = ts.tv_sec;

	if (input_power > charging_power) {
		curr = battery->charging_current;
	} else {
		curr = input_power / float_voltage;
		curr = (curr * 9) / 10;
	}

	if ((battery->lcd_status || battery->wc_tx_enable) && !battery->stop_timer)
		battery->stop_timer = true;
	else if (!(battery->lcd_status || battery->wc_tx_enable) && battery->stop_timer)
		battery->stop_timer = false;

	pr_info("%s : EXPIRED_TIME(%llu), IP(%d), CP(%d), CURR(%d), STANDARD(%d)\n",
		__func__, expired_time, input_power, charging_power, curr, battery->pdata->standard_curr);

	if (curr == 0)
		return;
	else if (curr > battery->pdata->standard_curr)
		curr = battery->pdata->standard_curr;

	expired_time = (expired_time * battery->pdata->standard_curr) / curr;

	pr_info("%s : CAL_EXPIRED_TIME(%llu) TIME NOW(%ld) TIME PREV(%ld)\n", __func__, expired_time, ts.tv_sec, battery->prev_safety_time);

	if (expired_time <= ((ts.tv_sec - battery->prev_safety_time) * 1000))
		expired_time = 0;
	else
		expired_time -= ((ts.tv_sec - battery->prev_safety_time) * 1000);

	battery->cal_safety_time = expired_time;
	expired_time = (expired_time * curr) / battery->pdata->standard_curr;

	battery->expired_time = expired_time;
	battery->prev_safety_time = ts.tv_sec;
	pr_info("%s : REMAIN_TIME(%ld) CAL_REMAIN_TIME(%ld)\n", __func__, battery->expired_time, battery->cal_safety_time);
}
