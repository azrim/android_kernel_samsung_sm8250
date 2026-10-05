/*
 *  sec_battery_time.c
 *  Samsung Mobile Battery Driver - polling, alarm and time-to-full
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"

void sec_bat_polling_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(
		work, struct sec_battery_info, polling_work.work);

	__pm_stay_awake(battery->monitor_wake_lock);
	queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
	dev_dbg(battery->dev, "%s: Activated\n", __func__);
}

static void sec_bat_program_alarm(
				struct sec_battery_info *battery, int seconds)
{
	alarm_start(&battery->polling_alarm,
		    ktime_add(battery->last_poll_time, ktime_set(seconds, 0)));
}
unsigned int sec_bat_get_polling_time(
	struct sec_battery_info *battery)
{
	/*
	 * The polling table has no entry for POWER_SUPPLY_STATUS_FULL, so a
	 * full battery is polled at the charging interval unless one of the
	 * cases below overrides it.
	 */
	if (battery->status == POWER_SUPPLY_STATUS_FULL)
		battery->polling_time =
			battery->pdata->polling_time[POWER_SUPPLY_STATUS_CHARGING];
	else
		battery->polling_time =
			battery->pdata->polling_time[battery->status];

	battery->polling_short = true;

	switch (battery->status) {
	case POWER_SUPPLY_STATUS_CHARGING:
		if (battery->polling_in_sleep)
			battery->polling_short = false;
		break;
	case POWER_SUPPLY_STATUS_DISCHARGING:
		if (battery->polling_in_sleep && (battery->ps_enable != true)) {
			battery->polling_time = battery->pdata->polling_time[
					SEC_BATTERY_POLLING_TIME_SLEEP];
		} else {
			battery->polling_time = battery->pdata->polling_time[
					battery->status];
		}
		if (!battery->wc_enable) {
			battery->polling_time = battery->pdata->polling_time[
					SEC_BATTERY_POLLING_TIME_CHARGING];
			pr_debug("%s: wc_enable is false, polling time is 30sec\n", __func__);
		}

		battery->polling_short = false;
		break;
	case POWER_SUPPLY_STATUS_FULL:
		if (battery->polling_in_sleep) {
			if (!(battery->pdata->full_condition_type &
					SEC_BATTERY_FULL_CONDITION_NOSLEEPINFULL) &&
					battery->charging_mode ==
					SEC_BATTERY_CHARGING_NONE) {
				battery->polling_time = battery->pdata->polling_time[
						SEC_BATTERY_POLLING_TIME_SLEEP];
			}
			battery->polling_short = false;
		} else {
			if (battery->charging_mode ==
					SEC_BATTERY_CHARGING_NONE)
				battery->polling_short = false;
		}
		break;
	case POWER_SUPPLY_STATUS_NOT_CHARGING:
		if ((battery->health == POWER_SUPPLY_HEALTH_OVERVOLTAGE ||
			(battery->health == POWER_SUPPLY_HEALTH_UNDERVOLTAGE)) &&
			(battery->health_check_count > 0)) {
			battery->health_check_count--;
			battery->polling_time = 1;
			battery->polling_short = false;
		}
		break;
	}

#if defined(CONFIG_WIRELESS_TX_MODE)
	if (battery->wc_tx_enable) {
		battery->polling_time = 10;
		battery->polling_short = false;
		pr_info("%s: Tx mode enable polling time is 10sec\n", __func__);
	}
#endif

#if defined(CONFIG_PDIC_PD30)
	if (is_pd_apdo_wire_type(battery->cable_type) &&
		(battery->pd_list.now_isApdo || battery->ta_alert_mode != OCP_NONE)) {
		battery->polling_time = 10;
		battery->polling_short = false;
		pr_info("%s: DC mode enable polling time is 10sec\n", __func__);
	}
#endif

	if (battery->polling_short)
		return battery->pdata->polling_time[
			SEC_BATTERY_POLLING_TIME_BASIC];
	/* set polling time to 46s to reduce current noise on wc */
	else if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS &&
			battery->status == POWER_SUPPLY_STATUS_CHARGING)
		battery->polling_time = 46;

	return battery->polling_time;
}
bool sec_bat_is_short_polling(
	struct sec_battery_info *battery)
{
	/* Change the full and short monitoring sequence
	 * Originally, full monitoring was the last time of polling_count
	 * But change full monitoring to first time
	 * because temperature check is too late
	 */
	return battery->polling_short && battery->polling_count != 1;
}

static void sec_bat_update_polling_count(
	struct sec_battery_info *battery)
{
	/* do NOT change polling count in sleep
	 * even though it is short polling
	 * to keep polling count along sleep/wakeup
	 */
	if (battery->polling_short && battery->polling_in_sleep)
		return;

	if (battery->polling_short &&
		(battery->polling_time /
		 battery->pdata->polling_time[SEC_BATTERY_POLLING_TIME_BASIC] >
		 battery->polling_count))
		battery->polling_count++;
	else
		battery->polling_count = 1;	/* initial value = 1 */
}

void sec_bat_set_polling(
	struct sec_battery_info *battery)
{
	unsigned int polling_time_temp = 0;

	dev_dbg(battery->dev, "%s: Start\n", __func__);

	polling_time_temp = sec_bat_get_polling_time(battery);

	dev_dbg(battery->dev,
		"%s: Status:%s, Sleep:%s, Charging:%s, Short Poll:%s\n",
		__func__, sec_bat_status_str[battery->status],
		battery->polling_in_sleep ? "Yes" : "No",
		(battery->charging_mode ==
		SEC_BATTERY_CHARGING_NONE) ? "No" : "Yes",
		battery->polling_short ? "Yes" : "No");
	dev_dbg(battery->dev,
		"%s: Polling time %d/%d sec.\n", __func__,
		battery->polling_short ?
		(polling_time_temp * battery->polling_count) :
		polling_time_temp, battery->polling_time);

	/* To sync with log above,
	 * change polling count after log is displayed
	 * Do NOT update polling count in initial monitor
	 */
	if (!battery->pdata->monitor_initial_count)
		sec_bat_update_polling_count(battery);
	else
		dev_dbg(battery->dev,
			"%s: Initial monitor %d times left.\n", __func__,
			battery->pdata->monitor_initial_count);

	switch (battery->pdata->polling_type) {
	case SEC_BATTERY_MONITOR_WORKQUEUE:
		if (battery->pdata->monitor_initial_count) {
			battery->pdata->monitor_initial_count--;
			schedule_delayed_work(&battery->polling_work, HZ);
		} else
			schedule_delayed_work(&battery->polling_work,
				polling_time_temp * HZ);
		break;
	case SEC_BATTERY_MONITOR_ALARM:
		/* the alarm fires relative to the last poll, so stamp it now */
		battery->last_poll_time = ktime_get_boottime();

		if (battery->pdata->monitor_initial_count) {
			battery->pdata->monitor_initial_count--;
			sec_bat_program_alarm(battery, 1);
		} else
			sec_bat_program_alarm(battery, polling_time_temp);
		break;
	case SEC_BATTERY_MONITOR_TIMER:
		break;
	default:
		break;
	}
	dev_dbg(battery->dev, "%s: End\n", __func__);
}

#if defined(CONFIG_CALC_TIME_TO_FULL)
void sec_bat_calc_time_to_full(struct sec_battery_info *battery)
{
	if (delayed_work_pending(&battery->timetofull_work)) {
		pr_info("%s: keep time_to_full(%5d sec)\n", __func__, battery->timetofull);
	} else if ((battery->status == POWER_SUPPLY_STATUS_CHARGING ||
		(battery->status == POWER_SUPPLY_STATUS_FULL && battery->capacity != 100)) && !battery->wc_tx_enable) {
		union power_supply_propval value = {0, };
		int charge = 0;

		if (is_hv_wire_12v_type(battery->cable_type)) {
			charge = battery->pdata->ttf_hv_12v_charge_current;
		} else if (is_hv_wireless_type(battery->cable_type) ||
				battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV ||
				battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20) {
			if (sec_bat_hv_wc_normal_mode_check(battery))
				charge = battery->pdata->ttf_wireless_charge_current;
			else if ((battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20 && !lpcharge) ||
				battery->cable_type == SEC_BATTERY_CABLE_HV_WIRELESS_20)
				charge = battery->ttf_predict_wc20_charge_current;
			else
				charge = battery->pdata->ttf_hv_wireless_charge_current;
		} else if (is_hv_wire_type(battery->cable_type)) {
			charge = battery->pdata->ttf_hv_charge_current;
		} else if (is_nv_wireless_type(battery->cable_type)) {
			charge = battery->pdata->ttf_wireless_charge_current;
		} else if (is_pd_apdo_wire_type(battery->cable_type) ||
			(is_pd_fpdo_wire_type(battery->cable_type) && battery->hv_pdo)) {
			if (battery->pd_max_charge_power > HV_CHARGER_STATUS_STANDARD4) {
				charge = battery->pdata->ttf_dc45_charge_current;
			} else if (battery->pd_max_charge_power > HV_CHARGER_STATUS_STANDARD3) {
				charge = battery->pdata->ttf_dc25_charge_current;
			} else if (battery->pd_max_charge_power <= battery->pdata->pd_charging_charge_power &&
				battery->pdata->charging_current[battery->cable_type].fast_charging_current >=
				battery->pdata->max_charging_current) { /* same PD power with AFC */
				charge = battery->pdata->ttf_hv_charge_current;
			} else {
				/*
				 * other PD charging: the lower of PD power / 5 and
				 * the fast charge current
				 */
				unsigned int pd_power = battery->pd_max_charge_power / 5;
				unsigned int fast_current =
					battery->pdata->charging_current[battery->cable_type].fast_charging_current;

				charge = pd_power > fast_current ? fast_current : pd_power;
			}
		} else {
			/* lower of input power / 5 and the fast charge current */
			unsigned int input_power = battery->max_charge_power / 5;
			unsigned int fast_current =
				battery->pdata->charging_current[battery->cable_type].fast_charging_current;

			charge = input_power > fast_current ? fast_current : input_power;
		}
		value.intval = charge;
		psy_do_property(battery->pdata->fuelgauge_name, get,
				POWER_SUPPLY_PROP_TIME_TO_FULL_NOW, value);
		dev_info(battery->dev, "%s: T: %5d sec, passed time: %5ld, current: %d\n",
				__func__, value.intval, battery->charging_passed_time, charge);
		battery->timetofull = value.intval;

		if (is_full_capacity(battery->fs)) {
			value.intval = charge;
			psy_do_property(battery->pdata->fuelgauge_name, get,
					POWER_SUPPLY_EXT_PROP_TTF_FULL_CAPACITY, value);
			battery->timetofull = battery->timetofull - value.intval;
			dev_info(battery->dev, "%s: time to 85 - T: %5d sec, passed time: %5ld, current: %d\n",
					__func__, battery->timetofull, battery->charging_passed_time, charge);
		}
	} else {
		battery->timetofull = -1;
	}
}

void sec_bat_time_to_full_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, timetofull_work.work);
	union power_supply_propval value = {0, };

	psy_do_property(battery->pdata->charger_name, get,
		POWER_SUPPLY_PROP_CURRENT_MAX, value);
	battery->current_max = value.intval;

	value.intval = SEC_BATTERY_CURRENT_MA;
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_CURRENT_NOW, value);
	battery->current_now = value.intval;

	value.intval = SEC_BATTERY_CURRENT_MA;
	psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_CURRENT_AVG, value);
	battery->current_avg = value.intval;

	sec_bat_calc_time_to_full(battery);
	dev_info(battery->dev, "%s:\n", __func__);
	/* nudge voltage_now so the psy change is seen even if nothing else moved */
	if (battery->voltage_now > 0)
		battery->voltage_now--;

	power_supply_changed(battery->psy_bat);
}
#endif
enum alarmtimer_restart sec_bat_alarm(
	struct alarm *alarm, ktime_t now)
{
	struct sec_battery_info *battery = container_of(alarm,
				struct sec_battery_info, polling_alarm);

	dev_dbg(battery->dev,
			"%s\n", __func__);

	/* In wake up, monitor work will be queued in complete function
	 * To avoid duplicated queuing of monitor work,
	 * do NOT queue monitor work in wake up by polling alarm
	 */
	if (!battery->polling_in_sleep) {
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		dev_dbg(battery->dev, "%s: Activated\n", __func__);
	}

	return ALARMTIMER_NORESTART;
}
