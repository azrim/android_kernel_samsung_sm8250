/*
 *  sec_battery_tx.c
 *  Samsung Mobile Battery Driver - wireless TX coordination
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"

void sec_bat_send_cs100(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	bool send_cs100_cmd = true;

	if (is_wireless_fake_type(battery->cable_type)) {
#ifdef CONFIG_CS100_JPNCONCEPT
		psy_do_property(battery->pdata->wireless_charger_name, get,
			POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ID, value);

		/* In case of the JPN PAD, this pad blocks the charge after give the cs100 command. */
		send_cs100_cmd = (battery->charging_mode == SEC_BATTERY_CHARGING_2ND ||	value.intval);
#endif
		if (send_cs100_cmd) {
			value.intval = POWER_SUPPLY_STATUS_FULL;
			psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_PROP_STATUS, value);
		}
	}
}
/* OTG during HV wireless charging or sleep mode have 4.5W normal wireless charging UI */
bool sec_bat_hv_wc_normal_mode_check(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };

	psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
	if (value.intval || sleep_mode) {
		pr_info("%s: otg(%d), sleep_mode(%d)\n", __func__, value.intval, sleep_mode);
		return true;
	}
	return false;
}
void sec_bat_handle_tx_misalign(struct sec_battery_info *battery, bool trigger_misalign)
{
	struct timespec ts = {0, };

	if (trigger_misalign) {
		if (battery->tx_misalign_start_time == 0) {
			ts = ktime_to_timespec(ktime_get_boottime());
			battery->tx_misalign_start_time = ts.tv_sec;
		}
		pr_info("@Tx_Mode %s: misalign is triggered!!(%d)\n", __func__, ++battery->tx_misalign_cnt);
		/* Attention!! in this case, 0x00(TX_OFF)  is sent first,
				and then 0x8000(RETRY) is sent */
		if (battery->tx_misalign_cnt < 3) {
			battery->tx_retry_case |= SEC_BAT_TX_RETRY_MISALIGN;
			sec_wireless_set_tx_enable(battery, false);
			/* clear tx all event */
			sec_bat_set_tx_event(battery, 0, BATT_TX_EVENT_WIRELESS_ALL_MASK);
			sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_RETRY, BATT_TX_EVENT_WIRELESS_TX_RETRY);
		} else {
			battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_MISALIGN;
			battery->tx_misalign_start_time = 0;
			battery->tx_misalign_cnt = 0;
			pr_info("@Tx_Mode %s: Misalign over 3 times, TX OFF (cancel misalign)\n", __func__);
			sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_MISALIGN, BATT_TX_EVENT_WIRELESS_TX_MISALIGN);
			sec_wireless_set_tx_enable(battery, false);
		}
	} else if (battery->tx_retry_case & SEC_BAT_TX_RETRY_MISALIGN) {
		get_monotonic_boottime(&ts);
		if (ts.tv_sec >= battery->tx_misalign_start_time) {
			battery->tx_misalign_passed_time = ts.tv_sec - battery->tx_misalign_start_time;
		} else {
			battery->tx_misalign_passed_time = 0xFFFFFFFF - battery->tx_misalign_start_time
				+ ts.tv_sec;
		}
		pr_info("@Tx_Mode %s: already misaligned, passed time(%ld)\n", __func__, battery->tx_misalign_passed_time);

		if (battery->tx_misalign_passed_time >= 60) {
			pr_info("@Tx_Mode %s: after 1min\n", __func__);
			if (battery->wc_tx_enable) {
				if (battery->wc_rx_connected) {
					pr_info("@Tx_Mode %s: RX Dev, Keep TX ON status (cancel misalign)\n", __func__);
				} else {
					pr_info("@Tx_Mode %s: NO RX Dev, TX OFF (cancel misalign)\n", __func__);
					sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_MISALIGN, BATT_TX_EVENT_WIRELESS_TX_MISALIGN);
					sec_wireless_set_tx_enable(battery, false);
				}
			} else {
				pr_info("@Tx_Mode %s: Keep TX OFF status (cancel misalign)\n", __func__);
				//sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_ETC, BATT_TX_EVENT_WIRELESS_TX_ETC);
			}
			battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_MISALIGN;
			battery->tx_misalign_start_time = 0;
			battery->tx_misalign_cnt = 0;
		}
	}
}
void sec_bat_handle_tx_ocp(struct sec_battery_info *battery, bool trigger_ocp)
{
	struct timespec ts = {0, };

	if (trigger_ocp) {
		if (battery->tx_ocp_start_time == 0) {
			ts = ktime_to_timespec(ktime_get_boottime());
			battery->tx_ocp_start_time = ts.tv_sec;
		}
		pr_info("@Tx_Mode %s: ocp is triggered!!(%d)\n", __func__, ++battery->tx_ocp_cnt);
		/* Attention!! in this case, 0x00(TX_OFF)  is sent first */
		/* and then 0x8000(RETRY) is sent */
		if (battery->tx_ocp_cnt < 3) {
			battery->tx_retry_case |= SEC_BAT_TX_RETRY_OCP;
			sec_wireless_set_tx_enable(battery, false);
			/* clear tx all event */
			sec_bat_set_tx_event(battery, 0, BATT_TX_EVENT_WIRELESS_ALL_MASK);
			sec_bat_set_tx_event(battery,
					BATT_TX_EVENT_WIRELESS_TX_RETRY, BATT_TX_EVENT_WIRELESS_TX_RETRY);
		} else {
			battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_OCP;
			battery->tx_ocp_start_time = 0;
			battery->tx_ocp_cnt = 0;
			pr_info("@Tx_Mode %s: ocp over 3 times, TX OFF (cancel ocp)\n", __func__);
			sec_bat_set_tx_event(battery,
				BATT_TX_EVENT_WIRELESS_TX_OCP, BATT_TX_EVENT_WIRELESS_TX_OCP);
			sec_wireless_set_tx_enable(battery, false);
		}
	} else if (battery->tx_retry_case & SEC_BAT_TX_RETRY_OCP) {
		ts = ktime_to_timespec(ktime_get_boottime());
		if (ts.tv_sec >= battery->tx_ocp_start_time) {
			battery->tx_ocp_passed_time = ts.tv_sec - battery->tx_ocp_start_time;
		} else {
			battery->tx_ocp_passed_time = 0xFFFFFFFF - battery->tx_ocp_start_time
				+ ts.tv_sec;
		}
		pr_info("@Tx_Mode %s: already ocp, passed time(%ld)\n",
				__func__, battery->tx_ocp_passed_time);

		if (battery->tx_ocp_passed_time >= 60) {
			pr_info("@Tx_Mode %s: after 1min\n", __func__);
			if (battery->wc_tx_enable) {
				if (battery->wc_rx_connected) {
					pr_info("@Tx_Mode %s: RX Dev, Keep TX ON status (cancel ocp)\n", __func__);
				} else {
					pr_info("@Tx_Mode %s: NO RX Dev, TX OFF (cancel ocp)\n", __func__);
					sec_bat_set_tx_event(battery,
							BATT_TX_EVENT_WIRELESS_TX_OCP, BATT_TX_EVENT_WIRELESS_TX_OCP);
					sec_wireless_set_tx_enable(battery, false);
				}
			} else {
				pr_info("@Tx_Mode %s: Keep TX OFF status (cancel ocp)\n", __func__);
			}
			battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_OCP;
			battery->tx_ocp_start_time = 0;
			battery->tx_ocp_cnt = 0;
		}
	}
}
void sec_bat_check_wc_re_auth(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };

	psy_do_property(battery->pdata->wireless_charger_name, get,
		POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ID, value);

	pr_info("%s: tx_id(0x%x), cable(%d), soc(%d)\n", __func__,
		value.intval, battery->cable_type, battery->capacity);

	if ((value.intval >= WC_PAD_ID_AUTH_PAD) && (value.intval <= WC_PAD_ID_AUTH_PAD_END)
		&& (battery->cable_type == SEC_BATTERY_CABLE_HV_WIRELESS)
		&& (battery->capacity >= 5)) {
		pr_info("%s: EPT Unknown for re-auth\n", __func__);

		value.intval = 1;
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_EXT_PROP_WC_EPT_UNKNOWN, value);

		battery->wc_auth_retried = true;
	} else if ((value.intval >= WC_PAD_ID_AUTH_PAD) && (value.intval <= WC_PAD_ID_AUTH_PAD_END)
		&& (battery->cable_type == SEC_BATTERY_CABLE_HV_WIRELESS_20)) {
		pr_info("%s: auth success\n", __func__);
		battery->wc_auth_retried = true;
	} else if ((value.intval < WC_PAD_ID_AUTH_PAD) || (value.intval > WC_PAD_ID_AUTH_PAD_END)) {
		pr_info("%s: re-auth is unnecessary\n", __func__);
		battery->wc_auth_retried = true;
	}
}
void sec_bat_wireless_minduty_cntl(struct sec_battery_info *battery, unsigned int duty_val)
{
	union power_supply_propval value = {0, };

	if (duty_val != battery->tx_minduty) {
		value.intval = duty_val;
		psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_MIN_DUTY, value);

		pr_info("@Tx_Mode %s : Min duty chagned (%d -> %d)\n", __func__, battery->tx_minduty, duty_val);
		battery->tx_minduty = duty_val;
	}
}
static void sec_bat_wireless_uno_cntl(struct sec_battery_info *battery, bool en)
{
	union power_supply_propval value = {0, };

	battery->uno_en = value.intval = en;
	pr_info("@Tx_Mode %s : Uno control %d\n", __func__, battery->uno_en);

	if (value.intval) {
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ENABLE, value);
	} else {
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_RX_CONNECTED, value);
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL, value);
	}
}
void sec_bat_wireless_iout_cntl(struct sec_battery_info *battery, int uno_iout, int mfc_iout) {
	union power_supply_propval value = {0, };

	if (battery->tx_uno_iout != uno_iout) {
		pr_info("@Tx_Mode %s : set uno iout(%d) -> (%d)\n", __func__, battery->tx_uno_iout, uno_iout);
		value.intval = battery->tx_uno_iout = uno_iout;
		psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_TX_IOUT, value);
	} else {
		pr_info("@Tx_Mode %s : Already set Uno Iout(%d == %d)\n", __func__, battery->tx_uno_iout, uno_iout);
	}

#if !defined(CONFIG_SEC_FACTORY)
	if (battery->lcd_status && (mfc_iout == battery->pdata->tx_mfc_iout_phone)) {
		pr_info("@Tx_Mode %s Reduce Tx MFC Iout. LCD ON\n", __func__);
		mfc_iout = battery->pdata->tx_mfc_iout_lcd_on;
	}
#endif

	if (battery->tx_mfc_iout != mfc_iout) {
		pr_info("@Tx_Mode %s : set mfc iout(%d) -> (%d)\n", __func__, battery->tx_mfc_iout, mfc_iout);
		value.intval = battery->tx_mfc_iout = mfc_iout;
		psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_TX_IOUT, value);
	} else {
		pr_info("@Tx_Mode %s : Already set MFC Iout(%d == %d)\n", __func__, battery->tx_mfc_iout, mfc_iout);
	}
}
void sec_bat_wireless_vout_cntl(struct sec_battery_info *battery, int vout_now)
{
	union power_supply_propval value = {0, };
	int vout_mv, vout_now_mv;

	vout_mv = battery->wc_tx_vout == 0 ? 5000 : (5000 + (battery->wc_tx_vout * 500));
	vout_now_mv = vout_now == 0 ? 5000 : (5000 + (vout_now * 500));

	pr_info("@Tx_Mode %s : set uno & mfc vout (%dmV -> %dmV)\n", __func__, vout_mv, vout_now_mv);

	if (battery->wc_tx_vout >= vout_now) {
		battery->wc_tx_vout = value.intval = vout_now;
		psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_TX_VOUT, value);
		psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_TX_VOUT, value);
	} else if (vout_now > battery->wc_tx_vout) {
		battery->wc_tx_vout = value.intval = vout_now;
		psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_TX_VOUT, value);
		psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_TX_VOUT, value);
	}

}
#if defined(CONFIG_WIRELESS_TX_MODE)
#if !defined(CONFIG_SEC_FACTORY)
void sec_bat_check_tx_battery_drain(struct sec_battery_info *battery)
{
	if (battery->capacity <= battery->pdata->tx_stop_capacity &&
		is_nocharge_type(battery->cable_type)) {
		pr_info("%s: @Tx_Mode battery level is drained, TX mode should turn off\n", __func__);
		/* set tx event */
		sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_SOC_DRAIN, BATT_TX_EVENT_WIRELESS_TX_SOC_DRAIN);
		sec_wireless_set_tx_enable(battery, false);
	}
}

void sec_bat_check_tx_current(struct sec_battery_info *battery)
{
	if (battery->lcd_status && (battery->tx_mfc_iout > battery->pdata->tx_mfc_iout_lcd_on)) {
		sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_lcd_on);
		pr_info("@Tx_Mode %s Reduce Tx MFC Iout. LCD ON\n", __func__);
	} else if (!battery->lcd_status && (battery->tx_mfc_iout == battery->pdata->tx_mfc_iout_lcd_on)) {
		union power_supply_propval value = {0, };
		sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
		pr_info("@Tx_Mode %s  Recovery Tx MFC Iout. LCD OFF\n", __func__);

		value.intval = true;
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_SEND_FSK, value);
	}
}

void sec_bat_check_tx_temperature(struct sec_battery_info *battery)
{
	if (battery->wc_tx_enable) {
		if (battery->temperature >= battery->pdata->tx_high_threshold) {
			pr_info("@Tx_Mode : %s: Battery temperature is too high. Tx mode should turn off\n", __func__);
			/* set tx event */
			sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_HIGH_TEMP, BATT_TX_EVENT_WIRELESS_TX_HIGH_TEMP);
			battery->tx_retry_case |= SEC_BAT_TX_RETRY_HIGH_TEMP;
			sec_wireless_set_tx_enable(battery, false);	
		} else if (battery->temperature <= battery->pdata->tx_low_threshold) {
			pr_info("@Tx_Mode : %s: Battery temperature is too low. Tx mode should turn off\n", __func__);
			/* set tx event */
			sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_LOW_TEMP, BATT_TX_EVENT_WIRELESS_TX_LOW_TEMP);
			battery->tx_retry_case |= SEC_BAT_TX_RETRY_LOW_TEMP;
			sec_wireless_set_tx_enable(battery, false);
		}
	} else if (battery->tx_retry_case & SEC_BAT_TX_RETRY_HIGH_TEMP) {
		if (battery->temperature <= battery->pdata->tx_high_recovery) {
			pr_info("@Tx_Mode : %s: Battery temperature goes to normal(High). Retry TX mode\n", __func__);
			battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_HIGH_TEMP;
			if (!battery->tx_retry_case)
				sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_RETRY, BATT_TX_EVENT_WIRELESS_TX_RETRY);
		}
	} else if (battery->tx_retry_case & SEC_BAT_TX_RETRY_LOW_TEMP) {
		if (battery->temperature >= battery->pdata->tx_low_recovery) {
			pr_info("@Tx_Mode : %s: Battery temperature goes to normal(Low). Retry TX mode\n", __func__);
			battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_LOW_TEMP;
			if (!battery->tx_retry_case)
				sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_RETRY, BATT_TX_EVENT_WIRELESS_TX_RETRY);
		}
	}
}
#endif

void sec_bat_check_tx_switch_mode(struct sec_battery_info *battery) {
	union power_supply_propval value = {0, };

	if (battery->current_event & SEC_BAT_CURRENT_EVENT_AFC)	{
		pr_info("@Tx_mode %s Do not switch switch mode! AFC Event set\n", __func__); 
		return;
	}

	value.intval = SEC_FUELGAUGE_CAPACITY_TYPE_CAPACITY_POINT;
	psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_CAPACITY, value);

	if ((battery->tx_switch_mode == TX_SWITCH_UNO_ONLY) && (!battery->buck_cntl_by_tx)) {
		battery->buck_cntl_by_tx = true;
		sec_bat_set_charge(battery, battery->charger_mode);

		sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
		sec_bat_wireless_vout_cntl(battery, battery->pdata->tx_uno_vout);
		sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);
	} else if ((battery->tx_switch_mode == TX_SWITCH_CHG_ONLY) && (battery->buck_cntl_by_tx)) {
		sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone_5v);
		sec_bat_wireless_vout_cntl(battery, WC_TX_VOUT_5_0V);
		sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_5V);

		battery->buck_cntl_by_tx = false;
		sec_bat_set_charge(battery, battery->charger_mode);
	}

	if (battery->status == POWER_SUPPLY_STATUS_FULL) {
		if (battery->charging_mode == SEC_BATTERY_CHARGING_NONE) {
			if (battery->tx_switch_mode == TX_SWITCH_CHG_ONLY)
				battery->tx_switch_mode_change = true;
		} else {
			if (battery->tx_switch_mode == TX_SWITCH_UNO_ONLY) {
				if (battery->tx_switch_start_soc >= 100) {
					if ((battery->capacity < 99) ||	((battery->capacity == 99) && (value.intval <= 1)))
						battery->tx_switch_mode_change = true;
				} else {
					if (((battery->capacity == battery->tx_switch_start_soc) && (value.intval <= 1)) ||
					(battery->capacity < battery->tx_switch_start_soc))
						battery->tx_switch_mode_change = true;
				}
			} else if (battery->tx_switch_mode == TX_SWITCH_CHG_ONLY) {
				if (battery->capacity >= 100)
					battery->tx_switch_mode_change = true;
			}
		}
	} else {
		if (battery->tx_switch_mode == TX_SWITCH_UNO_ONLY) {
			if (((battery->capacity == battery->tx_switch_start_soc) && (value.intval <= 1)) ||
				(battery->capacity < battery->tx_switch_start_soc))
				battery->tx_switch_mode_change = true;

		} else if (battery->tx_switch_mode == TX_SWITCH_CHG_ONLY) {
			if (((battery->capacity == (battery->tx_switch_start_soc + 1)) && (value.intval >= 8)) ||
				(battery->capacity > (battery->tx_switch_start_soc + 1)))
				battery->tx_switch_mode_change = true;
		}
	}
	pr_info("@Tx_mode Tx mode(%d) tx_switch_mode_chage(%d) start soc(%d) now soc(%d.%d)\n",
		battery->tx_switch_mode, battery->tx_switch_mode_change,
		battery->tx_switch_start_soc, battery->capacity, value.intval);
}
#endif
#if defined(CONFIG_WIRELESS_TX_MODE)
void sec_bat_txpower_calc(struct sec_battery_info * battery)
{
	if (delayed_work_pending(&battery->wpc_txpower_calc_work)) {
		pr_info("%s: keep average tx power(%5d mA)\n", __func__, battery->tx_avg_curr);
	} else if (battery->wc_tx_enable) {
		int tx_vout=0, tx_iout=0, vbatt=0;
		union power_supply_propval value = {0, };

		if (battery->tx_clear) {
			battery->tx_time_cnt = 0;
			battery->tx_avg_curr = 0;
			battery->tx_total_power = 0;
			battery->tx_clear = false;
		}

		if (battery->tx_clear_cisd) {
			battery->tx_total_power_cisd = 0;
			battery->tx_clear_cisd = false;
		}
		
		psy_do_property(battery->pdata->wireless_charger_name, get,
		POWER_SUPPLY_EXT_PROP_WIRELESS_TX_UNO_VIN, value);
		tx_vout = value.intval;

		psy_do_property(battery->pdata->wireless_charger_name, get,
		POWER_SUPPLY_EXT_PROP_WIRELESS_TX_UNO_IIN, value);
		tx_iout = value.intval;

		psy_do_property(battery->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_VOLTAGE_NOW, value);
		vbatt = value.intval;

		battery->tx_time_cnt++;

		/* AVG curr will be calculated only when the battery is discharged */ 
		if (battery->current_avg <= 0 && vbatt > 0)
			tx_iout = (tx_vout / vbatt) * tx_iout;
		else
			tx_iout = 0;

		/* monitor work will be scheduled every 10s when wc_tx_enable is true */
		battery->tx_avg_curr = ((battery->tx_avg_curr * battery->tx_time_cnt) + tx_iout) / (battery->tx_time_cnt + 1);
		battery->tx_total_power = (battery->tx_avg_curr * battery->tx_time_cnt) / (60*60/10);

		/* tx_total_power_cisd : daily accumulated power consumption by Tx, will be cleared when cisd data is sent */
		battery->tx_total_power_cisd = battery->tx_total_power_cisd + battery->tx_total_power;

		dev_info(battery->dev,
		"%s:tx_time_cnt(%ds), UNO_Vin(%dV), UNU_Iin(%dmA), tx_avg_curr(%dmA), tx_total_power(%dmAh), tx_total_power_cisd(%dmAh))\n", __func__,
		battery->tx_time_cnt*10, tx_vout, tx_iout, battery->tx_avg_curr, battery->tx_total_power, battery->tx_total_power_cisd);
	}
}

void sec_bat_txpower_calc_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, wpc_txpower_calc_work.work);

	sec_bat_txpower_calc(battery);
}
#endif
void sec_bat_wc_headroom_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
			struct sec_battery_info, wc_headroom_work.work);
	union power_supply_propval value = {0, };

	/* The default headroom is high, because initial wireless charging state is unstable.
		After 10sec wireless charging, however, recover headroom level to avoid chipset damage */
	if (battery->wc_status != SEC_WIRELESS_PAD_NONE) {
		/* When the capacity is higher than 99, and the device is in 5V wireless charging state,
			then Vrect headroom has to be headroom_2.
			Refer to the sec_bat_siop_work function. */
		if (battery->capacity < 99 && battery->status != POWER_SUPPLY_STATUS_FULL) {
			if (is_nv_wireless_type(battery->cable_type)) {
				if (battery->capacity < battery->pdata->wireless_cc_cv)
					value.intval = WIRELESS_VRECT_ADJ_ROOM_4; /* WPC 4.5W, Vrect Room 30mV */
				else
					value.intval = WIRELESS_VRECT_ADJ_ROOM_5; /* WPC 4.5W, Vrect Room 80mV */
			} else if (is_hv_wireless_type(battery->cable_type)) {
				value.intval = WIRELESS_VRECT_ADJ_ROOM_5;
			} else {
				value.intval = WIRELESS_VRECT_ADJ_OFF;
			}
			psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
			pr_info("%s: Changed Vrect adjustment from Rx activation(10seconds)", __func__);
		}
		if (is_nv_wireless_type(battery->cable_type))
			sec_bat_wc_cv_mode_check(battery);
	}
	__pm_relax(battery->wc_headroom_wake_lock);
}
#if defined(CONFIG_CALC_TIME_TO_FULL)
void sec_bat_predict_wireless20_time_to_full_current(struct sec_battery_info *battery, int rx_power)
{
	battery->ttf_predict_wc20_charge_current = battery->pdata->wireless_power_info[rx_power].ttf_charge_current;

	pr_info("%s: %dmA \n", __func__, battery->ttf_predict_wc20_charge_current);
}
#endif
void sec_bat_set_wireless20_current(struct sec_battery_info *battery, int rx_power)
{
	battery->wc20_vout = battery->pdata->wireless_power_info[rx_power].vout / 100;

	pr_info("%s: vout= %d \n", __func__, battery->wc20_vout);
	if (battery->wc_status == SEC_WIRELESS_PAD_WPC_HV_20) {
		sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_HV_WIRELESS_20,
				battery->pdata->wireless_power_info[rx_power].input_current_limit,
				battery->pdata->wireless_power_info[rx_power].fast_charging_current);

		if (battery->pdata->wireless_power_info[rx_power].rx_power <= 4500)
			battery->wc20_power_class = 0;
		else if (battery->pdata->wireless_power_info[rx_power].rx_power <= 7500)
			battery->wc20_power_class = SEC_WIRELESS_RX_POWER_CLASS_1;
		else if (battery->pdata->wireless_power_info[rx_power].rx_power <= 12000)
			battery->wc20_power_class = SEC_WIRELESS_RX_POWER_CLASS_2;
		else if (battery->pdata->wireless_power_info[rx_power].rx_power <= 20000)
			battery->wc20_power_class = SEC_WIRELESS_RX_POWER_CLASS_3;
		else
			battery->wc20_power_class = SEC_WIRELESS_RX_POWER_CLASS_4;

		if (is_wired_type(battery->cable_type)) {
			int wl_power = battery->pdata->wireless_power_info[rx_power].rx_power ;

			pr_info("%s: check power(%d <--> %d)\n",
				__func__, battery->max_charge_power, wl_power);
			if (battery->max_charge_power < wl_power) {
				__pm_stay_awake(battery->cable_wake_lock);
				queue_delayed_work(battery->monitor_wqueue,
					&battery->cable_work, 0);
			}
		} else {
			sec_bat_set_charging_current(battery);
		}
	}
}
u8 sec_bat_get_wireless20_power_class(struct sec_battery_info *battery)
{
	u8 power_class = 0;

	if (battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20) {
		power_class = battery->wc20_power_class;
	} else
		power_class = SEC_WIRELESS_RX_POWER_CLASS_1;

	pr_info("%s: power class = %d \n", __func__, power_class);

	return power_class;
}
#if defined(CONFIG_TX_5V_DISABLE)
static int is_5v_charger(struct sec_battery_info *battery)
{
	if ((is_pd_wire_type(battery->wire_status) && battery->pd_list.max_pd_count > 1)
			|| is_hv_wire_12v_type(battery->wire_status)
			|| is_hv_wire_type(battery->wire_status)
			|| (battery->wire_status == SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT)
			|| (battery->wire_status == SEC_BATTERY_CABLE_PREPARE_TA)) {
		return false;
	} else if (is_wired_type(battery->wire_status)) {
		return true;
	}
	return false;
}
#endif
void sec_bat_wpc_tx_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, wpc_tx_work.work);
	union power_supply_propval value = {0, };

	dev_info(battery->dev, "@Tx_Mode %s: Start\n", __func__);
	if (!battery->wc_tx_enable) {
		pr_info("@Tx_Mode %s : exit wpc_tx_work. Because Tx is already off\n", __func__);
		goto end_of_tx_work;
	}
#if defined(CONFIG_TX_5V_DISABLE)
	if (is_5v_charger(battery)) {
		pr_info("@Tx_Mode %s : 5V charger(%d) connected, disable TX\n", __func__, battery->cable_type);
		sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_5V_TA, BATT_TX_EVENT_WIRELESS_TX_5V_TA);
		sec_wireless_set_tx_enable(battery, false);
		goto end_of_tx_work;
	}
#endif
	switch (battery->wc_rx_type) {
	case NO_DEV:
		if (is_hv_wire_type(battery->wire_status)) {
			pr_info("@Tx_Mode %s : charging voltage change(9V -> 5V).\n", __func__);
			muic_afc_set_voltage(SEC_INPUT_VOLTAGE_5V/10);
			break;
		}

#if defined(CONFIG_DIRECT_CHARGING)
		if (is_pd_apdo_wire_type(battery->wire_status) && battery->pd_list.now_isApdo) {
			pr_info("@Tx_Mode %s: PD30 source charnge (APDO -> Fixed). Because Tx Start.\n", __func__);
			sec_bat_set_charge(battery, battery->charger_mode);
			break;
		} else if (is_pd_wire_type(battery->wire_status) && battery->hv_pdo) {
#else
		if (is_pd_wire_type(battery->wire_status) && battery->hv_pdo) {
#endif
			pr_info("@Tx_Mode %s: PD charnge pdo (9V -> 5V). Because Tx Start.\n", __func__);
			sec_bat_change_pdo(battery, SEC_INPUT_VOLTAGE_5V);
			break;
		}

		if (battery->afc_disable) {
			battery->afc_disable = false;
			muic_hv_charger_disable(battery->afc_disable);
		}

		if (!battery->buck_cntl_by_tx) {
			battery->buck_cntl_by_tx = true;
			sec_bat_set_charge(battery, battery->charger_mode);
		}

		if (!battery->uno_en) {
			battery->buck_cntl_by_tx = true;
			sec_bat_wireless_uno_cntl(battery, true);
		}

		sec_bat_wireless_vout_cntl(battery, WC_TX_VOUT_5_0V);
		sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_gear);

		break;
	case SS_GEAR:
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL)
		psy_do_property(battery->pdata->wireless_charger_name, get,
				POWER_SUPPLY_EXT_PROP_GEAR_PHM_EVENT, value);
#endif

		if ((battery->pdata->tx_gear_vout > WC_TX_VOUT_5_0V) && !value.intval) {
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL)
			if (battery->afc_disable) {
				battery->afc_disable = false;
				muic_hv_charger_disable(battery->afc_disable);
			}
#endif
			if (battery->wire_status == SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT) {
				pr_info("@Tx_Mode %s : charging voltage change(5V -> 9V)\n", __func__);
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL)
				/* prevent ocp */
				if (!battery->buck_cntl_by_tx) {
					sec_bat_wireless_vout_cntl(battery, WC_TX_VOUT_5_0V);
					sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, 1000);
					battery->buck_cntl_by_tx = true;
					sec_bat_set_charge(battery, battery->charger_mode);
					queue_delayed_work(battery->monitor_wqueue,
							&battery->wpc_tx_work, msecs_to_jiffies(500));
					return;
				} else if (battery->wc_tx_vout < WC_TX_VOUT_8_5V) {
					sec_bat_wireless_vout_cntl(battery, battery->wc_tx_vout+1);
					queue_delayed_work(battery->monitor_wqueue,
							&battery->wpc_tx_work, msecs_to_jiffies(500));
					return;
				}
#endif
				muic_afc_set_voltage(SEC_INPUT_VOLTAGE_9V/10);
				break;
			} else if (is_pd_wire_type(battery->wire_status) && !battery->hv_pdo) {
				pr_info("@Tx_Mode %s: PD change pdo (5V -> 9V). Because Tx Start.\n", __func__);
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL)
				/* prevent ocp */
				if (!battery->buck_cntl_by_tx) {
					sec_bat_wireless_vout_cntl(battery, WC_TX_VOUT_5_0V);
					sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, 1000);
					battery->buck_cntl_by_tx = true;
					sec_bat_set_charge(battery, battery->charger_mode);
					queue_delayed_work(battery->monitor_wqueue,
							&battery->wpc_tx_work, msecs_to_jiffies(500));
					return;
				} else if (battery->wc_tx_vout < WC_TX_VOUT_8_5V) {
					sec_bat_wireless_vout_cntl(battery, battery->wc_tx_vout+1);
					queue_delayed_work(battery->monitor_wqueue,
							&battery->wpc_tx_work, msecs_to_jiffies(500));
					return;
				}
#endif
				sec_bat_change_pdo(battery, SEC_INPUT_VOLTAGE_9V);
				break;
			}
		} else {
			if (!battery->afc_disable) {
				battery->afc_disable = true;
				muic_hv_charger_disable(battery->afc_disable);
			}

			if (is_hv_wire_type(battery->wire_status)) {
				pr_info("@Tx_Mode %s : charging voltage change(9V -> 5V).\n", __func__);
				muic_afc_set_voltage(SEC_INPUT_VOLTAGE_5V/10);
				break;
			} else if (is_pd_wire_type(battery->wire_status) && battery->hv_pdo) {
				pr_info("@Tx_Mode %s: PD change pdo (9V -> 5V). Because Tx Start.\n", __func__);
				sec_bat_change_pdo(battery, SEC_INPUT_VOLTAGE_5V);
				break;
			}
		}
		if (is_wired_type(battery->wire_status) && battery->buck_cntl_by_tx) {
			battery->buck_cntl_by_tx = false;
			sec_bat_set_charge(battery, battery->charger_mode);
		} else if ((battery->wire_status == SEC_BATTERY_CABLE_NONE) && (!battery->buck_cntl_by_tx)) {
			battery->buck_cntl_by_tx = true;
			sec_bat_set_charge(battery, battery->charger_mode);
		}
		sec_bat_wireless_vout_cntl(battery, battery->pdata->tx_gear_vout);
		sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_gear);

		break;
	default: /* SS_BUDS, SS_PHONE, OTHER_DEV */
		 if (battery->wire_status == SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT) {
			pr_info("@Tx_Mode %s : charging voltage change(5V -> 9V)\n", __func__);
			muic_afc_set_voltage(SEC_INPUT_VOLTAGE_9V/10);
			break;
		} else if (is_pd_wire_type(battery->wire_status) && !battery->hv_pdo) {
			pr_info("@Tx_Mode %s: PD charnge pdo (5V -> 9V). Because Tx Start.\n", __func__);
			sec_bat_change_pdo(battery, SEC_INPUT_VOLTAGE_9V);
			break;
		}

		 if (battery->wire_status == SEC_BATTERY_CABLE_NONE) {

			battery->tx_switch_mode = TX_SWITCH_MODE_OFF;
			battery->tx_switch_start_soc = 0;
			battery->tx_switch_mode_change = false;

			if (!battery->buck_cntl_by_tx) {
				battery->buck_cntl_by_tx = true;
				sec_bat_set_charge(battery, battery->charger_mode);
			}

			sec_bat_wireless_vout_cntl(battery, battery->pdata->tx_uno_vout);
			sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
			sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);

		} else if (is_hv_wire_type(battery->wire_status) || (is_pd_wire_type(battery->wire_status) && battery->hv_pdo)) {

			battery->tx_switch_mode = TX_SWITCH_MODE_OFF;
			battery->tx_switch_start_soc = 0;
			battery->tx_switch_mode_change = false;

			if (battery->buck_cntl_by_tx) {
				battery->buck_cntl_by_tx = false;
				sec_bat_set_charge(battery, battery->charger_mode);
			}

			sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
			sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);
		} else if (is_pd_wire_type(battery->wire_status) && battery->hv_pdo) {

			pr_info("@Tx_Mode %s: PD cable attached. HV PDO(%d)\n", __func__, battery->hv_pdo);

			battery->tx_switch_mode = TX_SWITCH_MODE_OFF;
			battery->tx_switch_start_soc = 0;
			battery->tx_switch_mode_change = false;

			if (battery->buck_cntl_by_tx) {
				battery->buck_cntl_by_tx = false;
				sec_bat_set_charge(battery, battery->charger_mode);
			}

			sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
			sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);
		} else if (is_wired_type(battery->wire_status) && !is_hv_wire_type(battery->wire_status) && (battery->wire_status != SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT)) {
			if (battery->current_event & SEC_BAT_CURRENT_EVENT_AFC)	{
				if (!battery->buck_cntl_by_tx) {
					battery->buck_cntl_by_tx = true;
					sec_bat_set_charge(battery, battery->charger_mode);
				}

				battery->tx_switch_mode = TX_SWITCH_MODE_OFF;
				battery->tx_switch_start_soc = 0;
				battery->tx_switch_mode_change = false;

				sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
				sec_bat_wireless_vout_cntl(battery, battery->pdata->tx_uno_vout);
				sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);

			} else if (battery->tx_switch_mode == TX_SWITCH_MODE_OFF) {
				battery->tx_switch_mode = TX_SWITCH_UNO_ONLY;
				battery->tx_switch_start_soc = battery->capacity;
				if (!battery->buck_cntl_by_tx) {
					battery->buck_cntl_by_tx = true;
					sec_bat_set_charge(battery, battery->charger_mode);
				}

				sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
				sec_bat_wireless_vout_cntl(battery, battery->pdata->tx_uno_vout);
				sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);

			} else if (battery->tx_switch_mode_change == true) {
				battery->tx_switch_start_soc = battery->capacity;

				pr_info("@Tx_mode: Switch Mode Change(%d -> %d)\n",
					battery->tx_switch_mode,
					battery->tx_switch_mode == TX_SWITCH_UNO_ONLY ?
					TX_SWITCH_CHG_ONLY : TX_SWITCH_UNO_ONLY);

				if (battery->tx_switch_mode == TX_SWITCH_UNO_ONLY) {
					battery->tx_switch_mode = TX_SWITCH_CHG_ONLY;

					sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone_5v);
					sec_bat_wireless_vout_cntl(battery, WC_TX_VOUT_5_0V);
					sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_5V);

					if (battery->buck_cntl_by_tx) {
						battery->buck_cntl_by_tx = false;
						sec_bat_set_charge(battery, battery->charger_mode);
					}

				} else if (battery->tx_switch_mode == TX_SWITCH_CHG_ONLY) {
					battery->tx_switch_mode = TX_SWITCH_UNO_ONLY;

					if (!battery->buck_cntl_by_tx) {
						battery->buck_cntl_by_tx = true;
						sec_bat_set_charge(battery, battery->charger_mode);
					}

					sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_phone);
					sec_bat_wireless_vout_cntl(battery, battery->pdata->tx_uno_vout);
					sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);

					value.intval = true;
					psy_do_property(battery->pdata->wireless_charger_name, set,
						POWER_SUPPLY_EXT_PROP_WIRELESS_SEND_FSK, value);

				}
				battery->tx_switch_mode_change = false;
			}

		}
		break;
	}
end_of_tx_work:
	__pm_relax(battery->wpc_tx_wake_lock);
	dev_info(battery->dev, "@Tx_Mode %s End\n", __func__);
}
void sec_bat_wpc_tx_en_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, wpc_tx_en_work.work);

	union power_supply_propval value = {0, };
	char wpc_en_status[2];

	pr_info("@Tx_Mode %s: tx %s\n", __func__,
		battery->wc_tx_enable ? "on" : "off");

	battery->tx_minduty = battery->pdata->tx_minduty_default;
	battery->tx_switch_mode = TX_SWITCH_MODE_OFF;
	battery->tx_switch_start_soc = 0;
	battery->tx_switch_mode_change = false;
	wpc_en_status[0] = WPC_EN_TX;

	if (battery->wc_tx_enable) {
		/* set tx event */
		sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_STATUS,
			(BATT_TX_EVENT_WIRELESS_TX_STATUS | BATT_TX_EVENT_WIRELESS_TX_RETRY));

#if defined(CONFIG_DIRECT_CHARGING)
		if (is_pd_apdo_wire_type(battery->wire_status) && battery->pd_list.now_isApdo) {
			pr_info("@Tx_Mode %s: PD30 source charnge (APDO -> Fixed). Because Tx Start.\n", __func__);
			sec_bat_set_charge(battery, battery->charger_mode);
		} else if (is_hv_wire_type(battery->wire_status)) {
#else
		if (is_hv_wire_type(battery->wire_status)) {
#endif
			muic_afc_set_voltage(SEC_INPUT_VOLTAGE_5V/10);
		} else if (is_pd_wire_type(battery->wire_status) && battery->hv_pdo) {
			pr_info("@Tx_Mode %s: PD charnge pdo (9V -> 5V). Because Tx Start.\n", __func__);
			sec_bat_change_pdo(battery, SEC_INPUT_VOLTAGE_5V);
		} else {
			battery->buck_cntl_by_tx = true;
			sec_bat_wireless_uno_cntl(battery, true);

			sec_bat_wireless_vout_cntl(battery, WC_TX_VOUT_5_0V);
			sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_gear);
		}

#if defined(CONFIG_WIRELESS_TX_MODE)
		pr_info("@Tx_Mode %s: TX Power Calculation start.\n", __func__);
		queue_delayed_work(battery->monitor_wqueue,
				&battery->wpc_txpower_calc_work, 0);
#endif
	} else {
		battery->uno_en = false;
		sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);
		value.intval = false;
		battery->wc_rx_type = NO_DEV;
		battery->wc_rx_connected = false;

		battery->tx_uno_iout = 0;
		battery->tx_mfc_iout = 0;

		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ENABLE, value);

		if (battery->afc_disable) {
			battery->afc_disable = false;
			muic_hv_charger_disable(battery->afc_disable);
		}

#if defined(CONFIG_DIRECT_CHARGING)
		if (is_pd_apdo_wire_type(battery->cable_type) || battery->buck_cntl_by_tx) {
#else
		if (battery->buck_cntl_by_tx) {
#endif
			battery->buck_cntl_by_tx = false;
			sec_bat_set_charge(battery, battery->charger_mode);
		}

		battery->wc_tx_vout = WC_TX_VOUT_5_0V;

		if (is_hv_wire_type(battery->cable_type)) {
			muic_afc_set_voltage(SEC_INPUT_VOLTAGE_9V/10);
		/* for 1) not supporting DC and charging bia PD20/DC on Tx
		       2) supporting DC and charging bia PD20 on Tx */
		} else if (is_pd_fpdo_wire_type(battery->cable_type) && !battery->hv_pdo) {
			sec_bat_change_pdo(battery, SEC_INPUT_VOLTAGE_9V);
		}

		cancel_delayed_work(&battery->wpc_tx_work);
#if defined(CONFIG_WIRELESS_TX_MODE)
		cancel_delayed_work(&battery->wpc_txpower_calc_work);
#endif
		__pm_relax(battery->wpc_tx_wake_lock);
	}

	pr_info("@Tx_Mode %s Done\n", __func__);
	__pm_relax(battery->wpc_tx_en_wake_lock);
}
void sec_wireless_set_tx_enable(struct sec_battery_info *battery, bool wc_tx_enable)
{
	pr_info("@Tx_Mode %s: TX Power enable ? (%d)\n", __func__, wc_tx_enable);

#if defined(CONFIG_TX_5V_DISABLE)
	if (wc_tx_enable && is_5v_charger(battery)) {
		pr_info("@Tx_Mode %s : 5V charger(%d) connected, do not turn on TX\n", __func__, battery->cable_type);
		sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_5V_TA, BATT_TX_EVENT_WIRELESS_TX_5V_TA);
		return;
	}
#endif

	battery->wc_tx_enable = wc_tx_enable;

	cancel_delayed_work(&battery->wpc_tx_en_work);
	__pm_stay_awake(battery->wpc_tx_en_wake_lock);
	queue_delayed_work(battery->monitor_wqueue,
		&battery->wpc_tx_en_work, 0);
}
void sec_wireless_otg_control(struct sec_battery_info *battery, int enable)
{
	union power_supply_propval value = {0, };

	if (enable) {
		sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_WPC_VOUT_LOCK,
			SEC_BAT_CURRENT_EVENT_WPC_VOUT_LOCK);
	} else {
		sec_bat_set_current_event(battery, 0,
			SEC_BAT_CURRENT_EVENT_WPC_VOUT_LOCK);
	}

	value.intval = enable;
	psy_do_property(battery->pdata->wireless_charger_name, set,
		POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);

	if (is_hv_wireless_type(battery->cable_type)) {
		int cnt;

		mutex_lock(&battery->voutlock);
		value.intval = (enable) ? WIRELESS_VOUT_5V :
			battery->wpc_vout_level;
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
		battery->aicl_current = 0; /* reset aicl current */
		mutex_unlock(&battery->voutlock);

		for (cnt = 0; cnt < 5; cnt++) {
			msleep(100);
			psy_do_property(battery->pdata->wireless_charger_name, get,
				POWER_SUPPLY_PROP_ENERGY_NOW, value);
			if (value.intval <= 6000) {
				pr_info("%s: wireless vout goes to 5V Vout(%d).\n",
					__func__, value.intval);
				break;
			}
		}
	} else if (is_nv_wireless_type(battery->cable_type)) {
		union power_supply_propval value = {0, };
		if (enable) {
			pr_info("%s: wireless 5V with OTG\n", __func__);
			value.intval = WIRELESS_VOUT_5V;
			psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
		} else {
			pr_info("%s: wireless 5.5V without OTG\n", __func__);
			value.intval = WIRELESS_VOUT_CC_CV_VOUT;
			psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
		}
	} else if (battery->wc_tx_enable && enable) {
		/* TX power should turn off during otg on */
		pr_info("@Tx_Mode %s: OTG is going to work, TX power should off\n", __func__);
		/* set tx event */
		sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_OTG_ON, BATT_TX_EVENT_WIRELESS_TX_OTG_ON);
		sec_wireless_set_tx_enable(battery, false);
	}
}
