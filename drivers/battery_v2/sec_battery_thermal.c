/*
 *  sec_battery_thermal.c
 *  Samsung Mobile Battery Driver - thermal, cable, temperature and
 *  full/recharge handling
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"

#if defined(CONFIG_SEC_ABC)
#include <linux/sti/abc_common.h>
#endif

#if defined(CONFIG_DUAL_BATTERY)
static int sec_bat_get_high_priority_temp(struct sec_battery_info *battery)
{
	const int standard_temp = 250;
	int priority_temp = battery->temperature;

	if (battery->pdata->sub_bat_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return battery->temperature;

	if (battery->temperature > standard_temp &&
		battery->sub_bat_temp > standard_temp) {
		if (battery->temperature < battery->sub_bat_temp)
			priority_temp = battery->sub_bat_temp;
	} else {
		if (battery->temperature > battery->sub_bat_temp)
			priority_temp = battery->sub_bat_temp;
	}

	pr_info("%s priority_temp = %d \n", __func__, priority_temp);
	return priority_temp;
}
#endif
#if !defined(CONFIG_SEC_FACTORY)
int sec_bat_get_temp_by_temp_control_source(struct sec_battery_info *battery,
	enum sec_battery_temp_control_source tcs)
{
	switch (tcs) {
	case TEMP_CONTROL_SOURCE_CHG_THM:
		return battery->chg_temp;
	case TEMP_CONTROL_SOURCE_USB_THM:
		return battery->usb_temp;
	case TEMP_CONTROL_SOURCE_WPC_THM:
		return battery->wpc_temp;
	case TEMP_CONTROL_SOURCE_NONE:
	case TEMP_CONTROL_SOURCE_BAT_THM:
	default:
		return battery->temperature;
	}
}

int sec_bat_check_mix_temp(struct sec_battery_info *battery, int input_current)
{
	int temperature = battery->pdata->blkt_temp_check_type ? battery->blkt_temp : battery->temperature;
	int chg_temp;

	if (battery->pdata->temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE ||
		battery->pdata->chg_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return input_current;

#if defined(CONFIG_DIRECT_CHARGING)
	if (is_pd_apdo_wire_type(battery->wire_status) && battery->pd_list.now_isApdo)
		chg_temp = battery->dchg_temp;
	else
		chg_temp = battery->chg_temp;
#else
	chg_temp = battery->chg_temp;
#endif

	if (battery->siop_level >= 100 && !battery->lcd_status &&
		is_not_wireless_type(battery->cable_type)) {
		if ((!battery->mix_limit &&
				(temperature >= battery->pdata->mix_high_temp) &&
				(chg_temp >= battery->pdata->mix_high_chg_temp)) ||
			(battery->mix_limit &&
				(temperature > battery->pdata->mix_high_temp_recovery))) {
			int max_input_current =
				battery->pdata->full_check_current_1st + 50;

			/* inpu current = float voltage * (topoff_current_1st + 50mA(margin)) / (vbus_level * 0.9) */
			if (battery->input_voltage) {
				input_current = ((battery->pdata->chg_float_voltage / battery->pdata->chg_float_voltage_conv) * max_input_current) /
					(battery->input_voltage * 9) / 10;
			} else {
				/* arm64: div-by-zero yields 0, so the original stored 0 */
				input_current = 0;
			}
			if (input_current > max_input_current)
				input_current = max_input_current;

			battery->mix_limit = true;
			/* skip other heating control */
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL,
						  SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL);

#if defined(CONFIG_WIRELESS_TX_MODE)
			if (battery->wc_tx_enable) {
				pr_info("%s: @Tx_Mode enter mix_temp_limit, TX mode should turn off\n", __func__);
				sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_HIGH_TEMP, BATT_TX_EVENT_WIRELESS_TX_HIGH_TEMP);
				battery->tx_retry_case |= SEC_BAT_TX_RETRY_MIX_TEMP;
				sec_wireless_set_tx_enable(battery, false);
			}
#endif
		} else if (battery->mix_limit) {
			battery->mix_limit = false;
			if (battery->tx_retry_case & SEC_BAT_TX_RETRY_MIX_TEMP) {
				pr_info("%s: @Tx_Mode recovery mix_temp_limit, TX mode should be retried\n", __func__);
				if ((battery->tx_retry_case & ~SEC_BAT_TX_RETRY_MIX_TEMP) == 0)
					sec_bat_set_tx_event(battery, BATT_TX_EVENT_WIRELESS_TX_RETRY, BATT_TX_EVENT_WIRELESS_TX_RETRY);
				battery->tx_retry_case &= ~SEC_BAT_TX_RETRY_MIX_TEMP;
			}
		}

		pr_info("%s: mix_limit(%d), temp(%d), chg_temp(%d), input_current(%d)\n",
			__func__, battery->mix_limit, temperature, chg_temp, input_current);
	} else {
		battery->mix_limit = false;
	}
	return input_current;
}

void sec_bat_check_wpc_temp(struct sec_battery_info *battery, int *input_current, int *charging_current)
{
	int wpc_high_temp = 0, wpc_high_temp_recovery = 0;

	if (battery->pdata->wpc_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return;

	if (is_wireless_type(battery->cable_type)) {
		union power_supply_propval value = {0, };
		int wpc_vout_level = 0;
		bool flicker_wa = false;

		if (battery->cable_type == SEC_BATTERY_CABLE_HV_WIRELESS_20) {
			wpc_vout_level = battery->wpc_max_vout_level;
			wpc_high_temp = battery->pdata->wpc_high_temp;
			wpc_high_temp_recovery = battery->pdata->wpc_high_temp_recovery;
		} else {
			wpc_vout_level = WIRELESS_VOUT_10V;
			wpc_high_temp = battery->pdata->non_wc20_wpc_high_temp;
			wpc_high_temp_recovery = battery->pdata->non_wc20_wpc_high_temp_recovery;
		}
		mutex_lock(&battery->voutlock);

		/* get vout level */
		psy_do_property(battery->pdata->wireless_charger_name, get,
			POWER_SUPPLY_EXT_PROP_WIRELESS_RX_VOUT, value);

		if (is_hv_wireless_type(battery->cable_type) &&
			value.intval == WIRELESS_VOUT_5_5V_STEP &&
			battery->wpc_vout_level != WIRELESS_VOUT_5_5V_STEP) {
			pr_info("%s: real vout was not 10V \n", __func__);
			battery->wpc_vout_level = WIRELESS_VOUT_5_5V_STEP;
		}

		if (battery->siop_level >= 100 && !battery->lcd_status) {
			int temp_val = sec_bat_get_temp_by_temp_control_source(battery,
				battery->pdata->wpc_temp_control_source);

			if ((!battery->chg_limit && temp_val >= wpc_high_temp) ||
				(battery->chg_limit && temp_val > wpc_high_temp_recovery)) {
				battery->chg_limit = true;
				if (*input_current > battery->pdata->wpc_input_limit_current) {
					if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_TX &&
						battery->pdata->wpc_input_limit_by_tx_check)
						*input_current = battery->pdata->wpc_input_limit_current_by_tx;
					else
						*input_current = battery->pdata->wpc_input_limit_current;
				}
				if (*charging_current > battery->pdata->wpc_charging_limit_current)
					*charging_current = battery->pdata->wpc_charging_limit_current;
				wpc_vout_level = WIRELESS_VOUT_5_5V_STEP;
			} else if (battery->chg_limit) {
				battery->chg_limit = false;
			}
		} else {
			if ((is_hv_wireless_type(battery->cable_type) &&
				battery->cable_type != SEC_BATTERY_CABLE_WIRELESS_HV_VEHICLE) ||
				battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV ||
				battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20) {
				int temp_val = sec_bat_get_temp_by_temp_control_source(battery,
					battery->pdata->wpc_temp_lcd_on_control_source);

				if ((!battery->chg_limit &&
						temp_val >= battery->pdata->wpc_lcd_on_high_temp) ||
					(battery->chg_limit &&
						temp_val > battery->pdata->wpc_lcd_on_high_temp_rec)) {
					if (*input_current > battery->pdata->wpc_lcd_on_input_limit_current)
						*input_current = battery->pdata->wpc_lcd_on_input_limit_current;
					if (*charging_current > battery->pdata->wpc_charging_limit_current)
						*charging_current = battery->pdata->wpc_charging_limit_current;
					battery->chg_limit = true;
					wpc_vout_level = WIRELESS_VOUT_5_5V_STEP;
				} else if (battery->chg_limit) {
					battery->chg_limit = false;
				}
			} else if (battery->chg_limit) {
				battery->chg_limit = false;
			}
		}

		if (is_hv_wireless_type(battery->cable_type)) {
			bool skip_wa = false;

			if (battery->wpc_vout_ctrl_lcd_on) {
				psy_do_property(battery->pdata->wireless_charger_name, get,
					POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ID, value);
				if (value.intval == WC_PAD_ID_UNKNOWN ||
					value.intval == WC_PAD_ID_SNGL_DREAM ||
					value.intval == WC_PAD_ID_STAND_DREAM) {
					pr_info("%s: do not use flicker w/a\n", __func__);
					skip_wa = true;
				}
			}
#if defined(CONFIG_ISDB_CHARGING_CONTROL)
			if ((battery->current_event & SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING) ||
				(battery->wpc_vout_ctrl_lcd_on && battery->lcd_status && !skip_wa) ||
				(battery->current_event & SEC_BAT_CURRENT_EVENT_ISDB) || sleep_mode) {
				pr_info("%s: vout 5.5V set. WPC_VOUT_CTRL_LCD_ON(%d), LCD(%d)\n",
					__func__, battery->wpc_vout_ctrl_lcd_on, battery->lcd_status);
#else
			if ((battery->current_event & SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING) ||
				(battery->wpc_vout_ctrl_lcd_on && battery->lcd_status && !skip_wa) || sleep_mode) {
				pr_info("%s: vout 5.5V set. WPC_VOUT_CTRL_LCD_ON(%d), LCD(%d)\n",
					__func__, battery->wpc_vout_ctrl_lcd_on, battery->lcd_status);
#endif
				wpc_vout_level = WIRELESS_VOUT_5_5V_STEP;
			}

			if (wpc_vout_level != battery->wpc_vout_level) {
				battery->wpc_vout_level = wpc_vout_level;
				if (battery->current_event & SEC_BAT_CURRENT_EVENT_WPC_VOUT_LOCK) {
					pr_info("%s: block to set wpc vout level(%s) because otg on\n",
						__func__, vout_control_mode_str[wpc_vout_level]);
				} else {
					value.intval = wpc_vout_level;
					psy_do_property(battery->pdata->wireless_charger_name, set,
						POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
					pr_info("%s: change vout level(%s)",
						__func__, vout_control_mode_str[battery->wpc_vout_level]);
					battery->aicl_current = 0; /* reset aicl current */
					flicker_wa = true;
				}
			} else if (battery->chg_limit && battery->wpc_vout_ctrl_lcd_on && !skip_wa) {
				value.intval = battery->lcd_status;
				psy_do_property(battery->pdata->wireless_charger_name, set,
						POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL, value);
				flicker_wa = true;
			} else if ((battery->wpc_vout_level == WIRELESS_VOUT_10V ||
				    battery->wpc_vout_level == battery->wpc_max_vout_level) &&
				   !battery->chg_limit) {
				/* reset aicl current to recover current for unexpected aicl
				 * during before vout boosting completion
				 */
				battery->aicl_current = 0;
			}

			value.intval = 0;
			psy_do_property(battery->pdata->wireless_charger_name, get,
					POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL, value);

			if (battery->wpc_vout_ctrl_lcd_on && !flicker_wa && !skip_wa && (value.intval == battery->lcd_status)) {
				pr_info("%s : Different pad voltage and lcd on/off\n. Change Pad Voltage.\n", __func__);
				value.intval = battery->lcd_status;
				psy_do_property(battery->pdata->wireless_charger_name, set,
						POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL, value);
			}
		}

		mutex_unlock(&battery->voutlock);
		pr_info("%s: change input_current(%d), change charge_current(%d), vout_level(%s), chg_limit(%d)\n",
			__func__, *input_current, *charging_current, vout_control_mode_str[battery->wpc_vout_level], battery->chg_limit);
	}
}

#endif
bool sec_bat_get_cable_type(
			struct sec_battery_info *battery,
			int cable_source_type)
{
	bool ret = false;
	int cable_type = battery->cable_type;

	if (cable_source_type & SEC_BATTERY_CABLE_SOURCE_CALLBACK) {
		if (battery->pdata->check_cable_callback)
			cable_type =
				battery->pdata->check_cable_callback();
	}

	if (cable_source_type & SEC_BATTERY_CABLE_SOURCE_ADC) {
		if (gpio_get_value_cansleep(
			battery->pdata->bat_gpio_ta_nconnected) ^
			battery->pdata->bat_polarity_ta_nconnected)
			cable_type = SEC_BATTERY_CABLE_NONE;
		else
			cable_type =
				sec_bat_get_charger_type_adc(battery);
	}

	if (battery->cable_type == cable_type) {
		dev_dbg(battery->dev,
			"%s: No need to change cable status\n", __func__);
	} else {
		if (cable_type < SEC_BATTERY_CABLE_NONE ||
			cable_type >= SEC_BATTERY_CABLE_MAX) {
			dev_err(battery->dev,
				"%s: Invalid cable type\n", __func__);
		} else {
			battery->cable_type = cable_type;
			if (battery->pdata->check_cable_result_callback)
				battery->pdata->check_cable_result_callback(
						battery->cable_type);

			ret = true;

			dev_dbg(battery->dev, "%s: Cable Changed (%d)\n",
				__func__, battery->cable_type);
		}
	}

	return ret;
}
bool sec_bat_battery_cable_check(struct sec_battery_info *battery)
{
	if (!sec_bat_check(battery)) {
		if (battery->check_count < battery->pdata->check_count)
			battery->check_count++;
		else {
			dev_err(battery->dev,
				"%s: Battery Disconnected\n", __func__);
			battery->present = false;
			battery->health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;

			if (battery->status !=
				POWER_SUPPLY_STATUS_DISCHARGING) {
				sec_bat_set_charging_status(battery,
						POWER_SUPPLY_STATUS_NOT_CHARGING);
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
			}

			if (battery->pdata->check_battery_result_callback)
				battery->pdata->check_battery_result_callback();
			return false;
		}
	} else
		battery->check_count = 0;

	battery->present = true;

	if (battery->health == POWER_SUPPLY_HEALTH_UNSPEC_FAILURE) {
		battery->health = POWER_SUPPLY_HEALTH_GOOD;

		if (battery->status == POWER_SUPPLY_STATUS_NOT_CHARGING) {
			sec_bat_set_charging_status(battery,
					POWER_SUPPLY_STATUS_CHARGING);
#if defined(CONFIG_BATTERY_SWELLING)
			if (!battery->swelling_mode)
#endif
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
		}
	}

	dev_dbg(battery->dev, "%s: Battery Connected\n", __func__);

	if (battery->pdata->cable_check_type &
		SEC_BATTERY_CABLE_CHECK_POLLING) {
		if (sec_bat_get_cable_type(battery,
			battery->pdata->cable_source_type)) {
			__pm_stay_awake(battery->cable_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					   &battery->cable_work, 0);
		}
	}
	return true;
}
static int sec_bat_ovp_uvlo_by_psy(struct sec_battery_info *battery)
{
	char *psy_name = NULL;
	union power_supply_propval value = {0, };

	value.intval = POWER_SUPPLY_HEALTH_GOOD;

	switch (battery->pdata->ovp_uvlo_check_type) {
	case SEC_BATTERY_OVP_UVLO_PMICPOLLING:
		psy_name = battery->pdata->pmic_name;
		break;
	case SEC_BATTERY_OVP_UVLO_CHGPOLLING:
		psy_name = battery->pdata->charger_name;
		break;
	default:
		dev_err(battery->dev,
			"%s: Invalid OVP/UVLO Check Type\n", __func__);
		goto ovp_uvlo_check_error;
	}

	psy_do_property(psy_name, get,
		POWER_SUPPLY_PROP_HEALTH, value);

ovp_uvlo_check_error:
	return value.intval;
}

static bool sec_bat_ovp_uvlo_result(
		struct sec_battery_info *battery, int health)
{
#if defined(CONFIG_DIRECT_CHARGING)
	union power_supply_propval val = {0, };

	if (health == POWER_SUPPLY_HEALTH_DC_ERR) {
		dev_info(battery->dev,
			"%s: DC err (%d)\n",
			__func__, health);
		battery->is_recharging = false;
		battery->health_check_count = DEFAULT_HEALTH_CHECK_COUNT;
		__pm_wakeup_event(battery->vbus_wake_lock, jiffies_to_msecs(HZ * 10));
		/* Enable charging anyway to check actual DC's health */
		val.intval = SEC_BAT_CHG_MODE_CHARGING_OFF;
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, val);

		sec_bat_set_charge(battery, battery->charger_mode);
	}
#endif

	if (battery->health != health) {
		battery->health = health;
		switch (health) {
		case POWER_SUPPLY_HEALTH_GOOD:
			dev_info(battery->dev, "%s: Safe voltage\n", __func__);
			dev_info(battery->dev, "%s: is_recharging : %d\n", __func__, battery->is_recharging);
			sec_bat_set_charging_status(battery,
					POWER_SUPPLY_STATUS_CHARGING);
			battery->charging_mode = SEC_BATTERY_CHARGING_1ST;
#if defined(CONFIG_BATTERY_SWELLING)
			if (!battery->swelling_mode)
#endif
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
			battery->health_check_count = 0;
			break;
		case POWER_SUPPLY_HEALTH_OVERVOLTAGE:
		case POWER_SUPPLY_HEALTH_UNDERVOLTAGE:
			dev_info(battery->dev,
				"%s: Unsafe voltage (%d)\n",
				__func__, health);
			sec_bat_set_charging_status(battery,
					POWER_SUPPLY_STATUS_NOT_CHARGING);
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
			battery->is_recharging = false;
			battery->health_check_count = DEFAULT_HEALTH_CHECK_COUNT;
#if defined(CONFIG_BATTERY_CISD)
			battery->cisd.data[CISD_DATA_UNSAFETY_VOLTAGE]++;
			battery->cisd.data[CISD_DATA_UNSAFE_VOLTAGE_PER_DAY]++;
#endif
			/*
			 * Take the wakelock during 10 seconds
			 * when over-voltage status is detected
			 */
			__pm_wakeup_event(battery->vbus_wake_lock, jiffies_to_msecs(HZ * 10));
			break;
		}
		power_supply_changed(battery->psy_bat);
		return true;
	}

	return false;
}

static bool sec_bat_ovp_uvlo(struct sec_battery_info *battery)
{
	int health = POWER_SUPPLY_HEALTH_GOOD;

	if (battery->wdt_kick_disable) {
		dev_dbg(battery->dev,
			"%s: No need to check in wdt test\n",
			__func__);
		return false;
	} else if ((battery->status == POWER_SUPPLY_STATUS_FULL) &&
		   (battery->charging_mode == SEC_BATTERY_CHARGING_NONE)) {
		dev_dbg(battery->dev, "%s: No need to check in Full status", __func__);
		return false;
	}

#if defined(CONFIG_DIRECT_CHARGING)
	if (battery->health != POWER_SUPPLY_HEALTH_GOOD &&
		battery->health != POWER_SUPPLY_HEALTH_OVERVOLTAGE &&
		battery->health != POWER_SUPPLY_HEALTH_UNDERVOLTAGE &&
		battery->health != POWER_SUPPLY_HEALTH_DC_ERR) {
		dev_dbg(battery->dev, "%s: No need to check\n", __func__);
		return false;
	}
#else
	if (battery->health != POWER_SUPPLY_HEALTH_GOOD &&
		battery->health != POWER_SUPPLY_HEALTH_OVERVOLTAGE &&
		battery->health != POWER_SUPPLY_HEALTH_UNDERVOLTAGE) {
		dev_dbg(battery->dev, "%s: No need to check\n", __func__);
		return false;
	}
#endif

	health = battery->health;

	switch (battery->pdata->ovp_uvlo_check_type) {
	case SEC_BATTERY_OVP_UVLO_CALLBACK:
		if (battery->pdata->ovp_uvlo_callback)
			health = battery->pdata->ovp_uvlo_callback();
		break;
	case SEC_BATTERY_OVP_UVLO_PMICPOLLING:
	case SEC_BATTERY_OVP_UVLO_CHGPOLLING:
		health = sec_bat_ovp_uvlo_by_psy(battery);
		break;
	case SEC_BATTERY_OVP_UVLO_PMICINT:
	case SEC_BATTERY_OVP_UVLO_CHGINT:
		/* nothing for interrupt check */
	default:
		break;
	}

	/*
	 * Move the location for calling the get_health
	 * in case of attaching the jig
	 */
	if (battery->factory_mode || battery->is_jig_on) {
		dev_dbg(battery->dev,
			"%s: No need to check in factory mode\n",
			__func__);
		return false;
	}

	return sec_bat_ovp_uvlo_result(battery, health);
}
static bool sec_bat_check_recharge(struct sec_battery_info *battery)
{
#if defined(CONFIG_DUAL_BATTERY)
	int voltage = 0;
#endif

#if defined(CONFIG_BATTERY_SWELLING)
	if (battery->swelling_mode == SWELLING_MODE_CHARGING ||
		battery->swelling_mode == SWELLING_MODE_FULL) {
		pr_info("%s: Skip normal recharge check routine for swelling mode\n",
			__func__);
		return false;
	}
#endif
	if ((battery->status == POWER_SUPPLY_STATUS_CHARGING) &&
			(battery->pdata->full_condition_type &
			 SEC_BATTERY_FULL_CONDITION_NOTIMEFULL) &&
			(battery->charging_mode == SEC_BATTERY_CHARGING_NONE)) {
		dev_info(battery->dev,
				"%s: Re-charging by NOTIMEFULL (%d)\n",
				__func__, battery->capacity);
		goto check_recharge_check_count;
	}

	if (battery->status == POWER_SUPPLY_STATUS_FULL &&
			battery->charging_mode == SEC_BATTERY_CHARGING_NONE) {
		int recharging_voltage = battery->pdata->recharge_condition_vcell;

		if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_MODE) {
			/* float voltage - 150mV */
			recharging_voltage =
				(battery->pdata->chg_float_voltage /
				 battery->pdata->chg_float_voltage_conv) -
				battery->pdata->swelling_low_rechg_thr;
			dev_info(battery->dev, "%s: recharging voltage changed by low temp(%d)\n",
					__func__, recharging_voltage);
		}
		dev_info(battery->dev, "%s: recharging voltage (%d)\n",
				__func__, recharging_voltage);

		if ((battery->pdata->recharge_condition_type &
					SEC_BATTERY_RECHARGE_CONDITION_SOC) &&
				(battery->capacity <=
				 battery->pdata->recharge_condition_soc)) {
			battery->expired_time = battery->pdata->recharging_expired_time;
			battery->prev_safety_time = 0;
			dev_info(battery->dev,
					"%s: Re-charging by SOC (%d)\n",
					__func__, battery->capacity);
			goto check_recharge_check_count;
		}

		if ((battery->pdata->recharge_condition_type &
		     SEC_BATTERY_RECHARGE_CONDITION_AVGVCELL) &&
		    (battery->voltage_avg <= recharging_voltage)) {
			battery->expired_time = battery->pdata->recharging_expired_time;
			battery->prev_safety_time = 0;
			dev_info(battery->dev,
					"%s: Re-charging by average VCELL (%d)\n",
					__func__, battery->voltage_avg);
			goto check_recharge_check_count;
		}

		if ((battery->pdata->recharge_condition_type &
		     SEC_BATTERY_RECHARGE_CONDITION_VCELL) &&
		    (battery->voltage_now <= recharging_voltage)) {
			battery->expired_time = battery->pdata->recharging_expired_time;
			battery->prev_safety_time = 0;
			dev_info(battery->dev,
					"%s: Re-charging by VCELL (%d)\n",
					__func__, battery->voltage_now);
			goto check_recharge_check_count;
		}

#if defined(CONFIG_DUAL_BATTERY)
		if (battery->pdata->recharge_condition_type &
					SEC_BATTERY_RECHARGE_CONDITION_LIMITER) {
			if (battery->voltage_avg_main > battery->voltage_avg_sub)
				voltage = battery->voltage_avg_main;
			else
				voltage = battery->voltage_avg_sub;

			if (voltage <= recharging_voltage) {
				battery->expired_time = battery->pdata->recharging_expired_time;
				battery->prev_safety_time = 0;
				dev_info(battery->dev,
						"%s: Re-charging by VPACK (%d)mV\n",
						__func__, voltage);
				goto check_recharge_check_count;
			} else if (abs(battery->voltage_avg_main - battery->voltage_avg_sub) >
						battery->pdata->force_recharge_margin) {
				battery->expired_time = battery->pdata->recharging_expired_time;
				battery->prev_safety_time = 0;
				dev_info(battery->dev,
						"%s: Force Re-charging by Vavg_m(%d)mV - Vavg_s(%d)mV,\n",
						__func__, battery->voltage_avg_main, battery->voltage_avg_sub);
				goto check_recharge_check_count;
			}
		}
#endif
	}

	battery->recharge_check_cnt = 0;
	return false;

check_recharge_check_count:
	if (battery->recharge_check_cnt <
		battery->pdata->recharge_check_count)
		battery->recharge_check_cnt++;
	dev_dbg(battery->dev,
		"%s: recharge count = %d\n",
		__func__, battery->recharge_check_cnt);

	if (battery->recharge_check_cnt >=
		battery->pdata->recharge_check_count)
		return true;
	else
		return false;
}

bool sec_bat_voltage_check(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };

	if (battery->status == POWER_SUPPLY_STATUS_DISCHARGING ||
		is_nocharge_type(battery->cable_type) ||
		battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_FAKE) {
		dev_dbg(battery->dev,
			"%s: Charging Disabled\n", __func__);
		return true;
	}

	/* OVP/UVLO check */
	if (sec_bat_ovp_uvlo(battery)) {
		if (battery->pdata->ovp_uvlo_result_callback)
			battery->pdata->ovp_uvlo_result_callback(battery->health);
		return false;
	}

	if ((battery->status == POWER_SUPPLY_STATUS_FULL) &&
		(battery->charging_mode != SEC_BATTERY_CHARGING_NONE || battery->swelling_mode)) {
		int voltage_now = battery->voltage_now;
		int voltage_ref = battery->pdata->recharge_condition_vcell - 50;
#if defined(CONFIG_ENABLE_FULL_BY_SOC)
		int soc_ref = 98;
#else
		int soc_ref = battery->pdata->full_condition_soc;
#endif

		pr_info("%s: chg mode (%d), swelling_mode(%d) \n",
			__func__, battery->charging_mode, battery->swelling_mode);

		if (is_eu_eco_rechg(battery->fs))
			soc_ref = (soc_ref > battery->pdata->recharge_condition_soc) ?
				battery->pdata->recharge_condition_soc : soc_ref;

		value.intval = 0;
		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_CAPACITY, value);

#if defined(CONFIG_DUAL_BATTERY)
		if (battery->pdata->recharge_condition_type &
					SEC_BATTERY_RECHARGE_CONDITION_LIMITER) {
			if (battery->voltage_avg_main > battery->voltage_avg_sub)
				voltage_now = battery->voltage_avg_main;
			else
				voltage_now = battery->voltage_avg_sub;
		}
#endif
		if (value.intval < soc_ref &&
			voltage_now < voltage_ref) {
			battery->is_recharging = false;
			battery->charging_mode = SEC_BATTERY_CHARGING_1ST;
			sec_bat_set_charging_status(battery,
					POWER_SUPPLY_STATUS_CHARGING);
			pr_info("%s: battery status full -> charging, RepSOC(%d)\n", __func__, value.intval);
			return false;
		}
	}

	/* Re-Charging check */
	if (sec_bat_check_recharge(battery)) {
		if (battery->pdata->full_check_type !=
			SEC_BATTERY_FULLCHARGED_NONE)
			battery->charging_mode = SEC_BATTERY_CHARGING_1ST;
		else
			battery->charging_mode = SEC_BATTERY_CHARGING_2ND;
		battery->is_recharging = true;
#if defined(CONFIG_BATTERY_CISD)
		battery->cisd.data[CISD_DATA_RECHARGING_COUNT]++;
		battery->cisd.data[CISD_DATA_RECHARGING_COUNT_PER_DAY]++;
#endif
#if defined(CONFIG_BATTERY_SWELLING)
		if (!battery->swelling_mode)
#endif
		sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
		return false;
	}

	return true;
}
#if defined(CONFIG_BATTERY_SWELLING)
void sec_bat_swelling_check(struct sec_battery_info *battery)
{
	union power_supply_propval val = {0, };
	int swelling_rechg_voltage = battery->pdata->swelling_high_rechg_voltage;
	bool en_swelling = false, en_rechg = false;
	int swelling_high_recovery = battery->pdata->swelling_high_temp_recov;
	int swelling_high_block = battery->pdata->swelling_high_temp_block;
	int temperature = battery->temperature;

#if defined(CONFIG_DUAL_BATTERY)
	temperature = sec_bat_get_high_priority_temp(battery);
#endif

	if (battery->pdata->temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return;

	if (is_wireless_type(battery->cable_type)) {
		swelling_high_recovery = battery->pdata->swelling_wc_high_temp_recov;
		swelling_high_block = battery->pdata->swelling_wc_high_temp_block;
	}
	pr_info_ratelimited("%s: swelling highblock(%d), highrecov(%d)\n",
		__func__, swelling_high_block, swelling_high_recovery);

	psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_VOLTAGE_MAX, val);

	pr_info_ratelimited("%s: status(%s), swell_mode(%s), chg_block(%d), low_temp_event(0x%x), vfloat(%d)mV, temp(%d)'C\n",
		__func__,
		sec_bat_status_str[battery->status],
		swelling_mode_str[battery->swelling_mode],
		battery->charging_block,
		(battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_MODE),
		val.intval,
		temperature);

	/*
	 * swelling_mode: under voltage, over voltage, battery missing
	 */
	if (battery->status == POWER_SUPPLY_STATUS_DISCHARGING ||
	    battery->status == POWER_SUPPLY_STATUS_NOT_CHARGING ||
	    battery->skip_swelling) {
		pr_debug("%s: DISCHARGING or NOT-CHARGING or 15 test mode. stop swelling mode\n", __func__);
		battery->swelling_mode = SWELLING_MODE_NONE;
		sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
		goto skip_swelling_check;
	}

	if (!battery->swelling_mode) {
		if (temperature >= swelling_high_block) {
			if (battery->wc_tx_enable &&
				(is_hv_wire_type(battery->cable_type) || is_pd_wire_type(battery->cable_type))) {
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			} else {
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
			}
			pr_info("%s: swelling mode start. stop charging\n", __func__);
			battery->swelling_mode = SWELLING_MODE_CHARGING;
			battery->swelling_full_check_cnt = 0;

#if defined(CONFIG_BATTERY_CISD)
			if (is_wireless_fake_type(battery->cable_type)) {
				battery->cisd.data[CISD_DATA_WC_HIGH_TEMP_SWELLING]++;
				battery->cisd.data[CISD_DATA_WC_HIGH_TEMP_SWELLING_PER_DAY]++;
			} else {
				battery->cisd.data[CISD_DATA_HIGH_TEMP_SWELLING]++;
				battery->cisd.data[CISD_DATA_HIGH_TEMP_SWELLING_PER_DAY]++;
			}
#endif
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING,
				SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
			en_swelling = true;
		} else if ((temperature <= battery->pdata->swelling_low_temp_block_2nd) &&
			!(battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND)) {
			pr_info("%s: 2nd low temperature swelling step!!  reduce current\n", __func__);
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND,
				SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
			if (battery->pdata->swelling_drop_float_voltage_lowtemp) {
				pr_info("%s: swelling_drop_float_voltage_lowtemp. set float volt (%d)\n",
					__func__, battery->pdata->swelling_drop_float_voltage);
				battery->swelling_mode = SWELLING_MODE_CHARGING;
				battery->swelling_full_check_cnt = 0;
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
				en_swelling = true;
			}
		} else if ((temperature <= battery->pdata->swelling_low_temp_block_1st) &&
			!(battery->current_event & (SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING | SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND))) {
			pr_info("%s: low temperature reduce current\n", __func__);
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING,
						  SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
		} else if (battery->swelling_low_temp_3rd_ctrl && (temperature <= battery->pdata->swelling_low_temp_block_3rd) &&
			!(battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_MODE)) {
			pr_info("%s: 3rd low temperature swelling step!!  reduce current\n", __func__);
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_3RD,
				SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
		} else if (battery->swelling_low_temp_3rd_ctrl && (temperature >= battery->pdata->swelling_low_temp_recov_3rd) &&
			(battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_MODE)) {
			pr_info("%s: normal temperature temperature recover current\n", __func__);
			sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_LOW_TEMP_MODE);
		} else if ((temperature >= battery->pdata->swelling_low_temp_recov_1st) &&
			(battery->current_event & (SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING | SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND))) {
			if (battery->swelling_low_temp_3rd_ctrl) {
				pr_info("%s: upto 1st low temperature swelling recovery temp! 3rd low temp swelling current set\n", __func__);
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_3RD,
							  SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
			} else {
				pr_info("%s: normal temperature temperature recover current\n", __func__);
				sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_LOW_TEMP_MODE);
			}
		} else if ((temperature >= battery->pdata->swelling_low_temp_recov_2nd) &&
			(battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND)) {
			pr_info("%s: upto 2nd low temperature swelling recovery temp! 1st low temp swelling current set\n", __func__);
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING,
						  SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
		}
	}

	if (!battery->voltage_now)
		return;

	if (battery->swelling_mode) {
		if ((temperature <= swelling_high_recovery && !battery->pdata->swelling_drop_float_voltage_lowtemp) ||
			(battery->pdata->swelling_drop_float_voltage_lowtemp &&
			temperature >= battery->pdata->swelling_low_temp_recov_2nd && temperature <= swelling_high_recovery)) {
			pr_info("%s: swelling mode end. restart charging\n", __func__);
			battery->swelling_mode = SWELLING_MODE_NONE;
			battery->charging_mode = SEC_BATTERY_CHARGING_1ST;
			sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
			/* restore 4.4V float voltage */
			val.intval = battery->pdata->swelling_normal_float_voltage;
			psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_VOLTAGE_MAX, val);
#if defined(CONFIG_BATTERY_CISD)
			battery->cisd.data[CISD_DATA_SWELLING_RECOVERY_CNT]++;
			battery->cisd.data[CISD_DATA_SWELLING_RECOVERY_CNT_PER_DAY]++;
#endif
		} else if (battery->voltage_now < swelling_rechg_voltage &&
			   battery->charging_block) {
			pr_info("%s: swelling mode recharging start. Vbatt(%d)\n",
				__func__, battery->voltage_now);
			battery->charging_mode = SEC_BATTERY_CHARGING_1ST;
			en_rechg = true;
			if (temperature > swelling_high_recovery) {
				pr_info("%s: swelling mode reduce charging current(HIGH-temp:%d)\n",
					__func__, temperature);
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING,
							  SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
			}
			/* change drop float voltage */
			val.intval = battery->pdata->swelling_drop_float_voltage;
			psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_VOLTAGE_MAX, val);
			/* set charging enable */
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
		}
	}

	if (en_swelling && !en_rechg) {
		pr_info("%s : SAFETY TIME RESET (SWELLING MODE CHARING STOP!)\n", __func__);
		battery->expired_time = battery->pdata->expired_time;
		battery->prev_safety_time = 0;
	}

skip_swelling_check:
	dev_dbg(battery->dev, "%s end\n", __func__);
}
#endif
static bool sec_bat_temperature(
				struct sec_battery_info *battery)
{
	if (is_wireless_fake_type(battery->cable_type)) {
		battery->temp_highlimit_threshold =
			battery->pdata->temp_highlimit_threshold_normal;
		battery->temp_highlimit_recovery =
			battery->pdata->temp_highlimit_recovery_normal;
		battery->temp_high_threshold =
			battery->pdata->wpc_high_threshold_normal;
		battery->temp_high_recovery =
			battery->pdata->wpc_high_recovery_normal;
		battery->temp_low_recovery =
			battery->pdata->wpc_low_recovery_normal;
		battery->temp_low_threshold =
			battery->pdata->wpc_low_threshold_normal;
	} else {
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
	}

	return true;
}

bool sec_bat_temperature_check(
				struct sec_battery_info *battery)
{
	int pre_health = POWER_SUPPLY_HEALTH_GOOD;
	int temperature = battery->temperature;

#if defined(CONFIG_DUAL_BATTERY)
	temperature = sec_bat_get_high_priority_temp(battery);
#endif

	if (battery->status == POWER_SUPPLY_STATUS_DISCHARGING) {
		battery->health_change = false;
		dev_dbg(battery->dev,
			"%s: Charging Disabled\n", __func__);
		return true;
	}

	if (battery->health != POWER_SUPPLY_HEALTH_GOOD &&
		battery->health != POWER_SUPPLY_HEALTH_OVERHEAT &&
		battery->health != POWER_SUPPLY_HEALTH_COLD &&
		battery->health != POWER_SUPPLY_HEALTH_OVERHEATLIMIT) {
		dev_dbg(battery->dev, "%s: No need to check\n", __func__);
		return false;
	}

#if defined(CONFIG_ENG_BATTERY_CONCEPT) || defined(CONFIG_SEC_FACTORY)
	if (!battery->cooldown_mode) {
		dev_err(battery->dev, "%s: Forced temp check block\n", __func__);
		return true;
	}
#endif

	sec_bat_temperature(battery);

	if (battery->pdata->temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE) {
		dev_err(battery->dev,
			"%s: Invalid Temp Check Type\n", __func__);
		return true;
	}
	pre_health = battery->health;

	if (battery->pdata->usb_temp_check_type && (battery->usb_temp >= battery->temp_highlimit_threshold)) {
		if (battery->health != POWER_SUPPLY_HEALTH_OVERHEATLIMIT) {
			if (battery->temp_highlimit_cnt <
			    battery->pdata->temp_check_count) {
				battery->temp_highlimit_cnt++;
				battery->temp_high_cnt = 0;
				battery->temp_low_cnt = 0;
				battery->temp_recover_cnt = 0;
			}
			dev_err(battery->dev,
				"%s: usb therm highlimit count = %d\n",
				__func__, battery->temp_highlimit_cnt);
		}
	} else if (battery->pdata->usb_temp_check_type && (battery->usb_temp > battery->temp_highlimit_recovery)
		&& (battery->health == POWER_SUPPLY_HEALTH_OVERHEATLIMIT)) {
		dev_err(battery->dev,
			"%s: usb therm highlimit\n", __func__);
	} else if (temperature >= battery->temp_highlimit_threshold && !battery->pdata->usb_temp_check_type) {
		if (battery->health != POWER_SUPPLY_HEALTH_OVERHEATLIMIT) {
			if (battery->temp_highlimit_cnt <
			    battery->pdata->temp_check_count) {
				battery->temp_highlimit_cnt++;
				battery->temp_high_cnt = 0;
				battery->temp_low_cnt = 0;
				battery->temp_recover_cnt = 0;
			}
			dev_err(battery->dev,
				"%s: highlimit count = %d\n",
				__func__, battery->temp_highlimit_cnt);
		}
	} else if (temperature >= battery->temp_high_threshold) {
		if (battery->health == POWER_SUPPLY_HEALTH_OVERHEATLIMIT && !battery->pdata->usb_temp_check_type) {
			if (temperature <= battery->temp_highlimit_recovery) {
				if (battery->temp_recover_cnt <
				    battery->pdata->temp_check_count) {
					battery->temp_recover_cnt++;
					battery->temp_highlimit_cnt = 0;
					battery->temp_high_cnt = 0;
					battery->temp_low_cnt = 0;
				}
				dev_err(battery->dev,
					"%s: recovery count = %d\n",
					__func__, battery->temp_recover_cnt);
			}
		} else if (battery->health != POWER_SUPPLY_HEALTH_OVERHEAT) {
			if (battery->temp_high_cnt <
			    battery->pdata->temp_check_count) {
				battery->temp_high_cnt++;
				battery->temp_highlimit_cnt = 0;
				battery->temp_low_cnt = 0;
				battery->temp_recover_cnt = 0;
			}
			dev_err(battery->dev,
				"%s: high count = %d\n",
				__func__, battery->temp_high_cnt);
		}
	} else if ((temperature <= battery->temp_high_recovery) &&
				(temperature >= battery->temp_low_recovery)) {
		if (battery->health == POWER_SUPPLY_HEALTH_OVERHEAT ||
			battery->health == POWER_SUPPLY_HEALTH_OVERHEATLIMIT ||
		    battery->health == POWER_SUPPLY_HEALTH_COLD) {
			if (battery->temp_recover_cnt <
			    battery->pdata->temp_check_count) {
				battery->temp_recover_cnt++;
				battery->temp_highlimit_cnt = 0;
				battery->temp_high_cnt = 0;
				battery->temp_low_cnt = 0;
			}
			dev_err(battery->dev,
				"%s: recovery count = %d\n",
				__func__, battery->temp_recover_cnt);
		}
	} else if (temperature <= battery->temp_low_threshold) {
		if (battery->health != POWER_SUPPLY_HEALTH_COLD) {
			if (battery->temp_low_cnt <
			    battery->pdata->temp_check_count) {
				battery->temp_low_cnt++;
				battery->temp_highlimit_cnt = 0;
				battery->temp_high_cnt = 0;
				battery->temp_recover_cnt = 0;
			}
			dev_err(battery->dev,
				"%s: low count = %d\n",
				__func__, battery->temp_low_cnt);
		}
	} else {
		battery->temp_highlimit_cnt = 0;
		battery->temp_high_cnt = 0;
		battery->temp_low_cnt = 0;
		battery->temp_recover_cnt = 0;
	}

#if defined(CONFIG_PREVENT_USB_CONN_OVERHEAT)
	if (battery->pdata->usb_thermal_source && !battery->usb_temp_flag) {
		int gap = 0;

		if (battery->usb_temp > battery->temperature)
			gap = battery->usb_temp - battery->temperature;

		if (battery->usb_temp >= battery->temp_highlimit_threshold) {
			pr_info("%s: Usb Temp over %d(%d)\n", __func__, battery->temp_highlimit_threshold, battery->usb_temp);
			battery->cisd.data[CISD_DATA_USB_OVERHEAT_CHARGING]++;
			battery->cisd.data[CISD_DATA_USB_OVERHEAT_CHARGING_PER_DAY]++;
			battery->usb_temp_flag = true;
		} else if ((battery->usb_temp >= battery->usb_protection_temp) && (gap >= battery->temp_gap_bat_usb)) {
			pr_info("%s: Temp gap between Usb temp and Bat temp : %d\n", __func__, gap);
			battery->usb_temp_flag = true;
			battery->cisd.data[CISD_DATA_USB_OVERHEAT_RAPID_CHANGE]++;
			battery->cisd.data[CISD_DATA_USB_OVERHEAT_RAPID_CHANGE_PER_DAY]++;

			if (gap > battery->cisd.data[CISD_DATA_USB_OVERHEAT_ALONE_PER_DAY])
				battery->cisd.data[CISD_DATA_USB_OVERHEAT_ALONE_PER_DAY] = gap;
		}

		if (battery->usb_temp_flag) {
			pr_info("%s: Usb temp flag %d\n", __func__, battery->usb_temp_flag);
			if (lpcharge && (battery->health != POWER_SUPPLY_HEALTH_OVERHEATLIMIT)) {
				battery->temp_highlimit_cnt = battery->pdata->temp_check_count;
			} else if (is_pd_wire_type(battery->cable_type)) {
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
				select_pdo(1);
				muic_set_hiccup_mode(1);
				pdic_manual_ccopen_request(1);
				return false;
			} else {
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
				muic_set_hiccup_mode(1);
				return false;
			}
		}
	}
#endif

	if (battery->temp_highlimit_cnt >=
	    battery->pdata->temp_check_count) {
#if defined(CONFIG_PREVENT_USB_CONN_OVERHEAT)
		if (lpcharge) {
			battery->health = POWER_SUPPLY_HEALTH_OVERHEATLIMIT;
			battery->temp_highlimit_cnt = 0;
		}
#else
		battery->health = POWER_SUPPLY_HEALTH_OVERHEATLIMIT;
		battery->temp_highlimit_cnt = 0;
#endif
	} else {
#if defined(CONFIG_PREVENT_USB_CONN_OVERHEAT)
		if (lpcharge && battery->usb_temp_flag)
			return true;
#endif
		if (battery->temp_high_cnt >=
			battery->pdata->temp_check_count) {
			battery->health = POWER_SUPPLY_HEALTH_OVERHEAT;
			battery->temp_high_cnt = 0;
		} else if (battery->temp_low_cnt >=
			battery->pdata->temp_check_count) {
			battery->health = POWER_SUPPLY_HEALTH_COLD;
			battery->temp_low_cnt = 0;
		} else if (battery->temp_recover_cnt >=
			 battery->pdata->temp_check_count) {
			if (battery->health == POWER_SUPPLY_HEALTH_OVERHEATLIMIT &&
				temperature > battery->temp_high_recovery) {
				battery->health = POWER_SUPPLY_HEALTH_OVERHEAT;
			} else {
				battery->health = POWER_SUPPLY_HEALTH_GOOD;
			}
			battery->temp_recover_cnt = 0;
		}
	}
	if (pre_health != battery->health) {
		battery->health_change = true;
		dev_info(battery->dev, "%s, health_change true\n", __func__);
	} else {
		battery->health_change = false;
	}

	if ((battery->health == POWER_SUPPLY_HEALTH_OVERHEAT) ||
		(battery->health == POWER_SUPPLY_HEALTH_COLD) ||
		(battery->health == POWER_SUPPLY_HEALTH_OVERHEATLIMIT)) {
		if (battery->health_change) {
			union power_supply_propval val = {0, };

			battery->is_abnormal_temp = true;
			if (is_wireless_fake_type(battery->cable_type)) {
				val.intval = battery->health;
				psy_do_property(battery->pdata->wireless_charger_name, set,
						POWER_SUPPLY_PROP_HEALTH, val);
			}
			dev_info(battery->dev,
				"%s: Unsafe Temperature\n", __func__);
			sec_bat_set_charging_status(battery,
					POWER_SUPPLY_STATUS_NOT_CHARGING);
#if defined(CONFIG_BATTERY_CISD)
			battery->cisd.data[CISD_DATA_UNSAFETY_TEMPERATURE]++;
			battery->cisd.data[CISD_DATA_UNSAFE_TEMPERATURE_PER_DAY]++;
#endif

			if (battery->health == POWER_SUPPLY_HEALTH_OVERHEATLIMIT) {
				/* change charging current to battery (default 0mA) */
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
				if (is_hv_afc_wire_type(battery->cable_type) && !battery->vbus_limit) {
#if defined(CONFIG_MUIC_HV) || defined(CONFIG_SUPPORT_HV_CTRL)
					battery->vbus_chg_by_siop = SEC_INPUT_VOLTAGE_0V;
					muic_afc_set_voltage(SEC_INPUT_VOLTAGE_0V);
#endif
					battery->vbus_limit = true;
					pr_info("%s: Set AFC TA to 0V\n", __func__);
				} else if (is_pd_wire_type(battery->cable_type)) {
					select_pdo(1);
					pr_info("%s: Set PD TA to PDO 0\n", __func__);
				}
			} else if (battery->health == POWER_SUPPLY_HEALTH_OVERHEAT) {
				/* to discharge battery */
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
			} else {
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			}

			return false;
		}
		/* dose not need buck control at low temperature */
		if (battery->health == POWER_SUPPLY_HEALTH_OVERHEAT) {
			if ((battery->charger_mode == SEC_BAT_CHG_MODE_BUCK_OFF) &&
				(battery->voltage_now < (battery->pdata->swelling_drop_float_voltage / battery->pdata->chg_float_voltage_conv))) {
				pr_info("%s: Vnow(%dmV) < %dmV has dropped enough to get buck on mode \n", __func__,
					battery->voltage_now,
					(battery->pdata->swelling_drop_float_voltage / battery->pdata->chg_float_voltage_conv));
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			}
		}
	} else {
		/* if recovered from not charging */
		if ((battery->health == POWER_SUPPLY_HEALTH_GOOD) &&
			(battery->status == POWER_SUPPLY_STATUS_NOT_CHARGING) &&
			!(battery->misc_event & BATT_MISC_EVENT_FULL_CAPACITY)) {
			battery->is_abnormal_temp = false;
			dev_info(battery->dev,
					"%s: Safe Temperature\n", __func__);
			if (battery->capacity >= 100)
				sec_bat_set_charging_status(battery,
						POWER_SUPPLY_STATUS_FULL);
			else	/* Normal Charging */
				sec_bat_set_charging_status(battery,
						POWER_SUPPLY_STATUS_CHARGING);
#if defined(CONFIG_BATTERY_SWELLING)
			if (temperature > battery->pdata->swelling_high_temp_recov) {
				pr_info("%s: swelling mode start. stop charging\n", __func__);
				battery->swelling_mode = SWELLING_MODE_CHARGING;
				battery->swelling_full_check_cnt = 0;
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
				if (temperature > battery->pdata->swelling_high_temp_recov) {
					sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING,
							SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING);
				}
			} else {
				union power_supply_propval val = {0, };

				if (pre_health == POWER_SUPPLY_HEALTH_COLD) {
					if (temperature <= battery->pdata->swelling_low_temp_recov_2nd) {
						sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND,
									SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
						if (battery->pdata->swelling_drop_float_voltage_lowtemp) {
							pr_info("%s: low swelling mode start. stop charging\n", __func__);
							battery->swelling_mode = SWELLING_MODE_CHARGING;
							battery->swelling_full_check_cnt = 0;
							val.intval = battery->pdata->swelling_drop_float_voltage;
							psy_do_property(battery->pdata->charger_name, set,
								POWER_SUPPLY_PROP_VOLTAGE_MAX, val);
							sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
						}
					} else if (temperature <= battery->pdata->swelling_low_temp_block_1st) {
						sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING,
									SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
					} else if (battery->swelling_low_temp_3rd_ctrl && temperature <= battery->pdata->swelling_low_temp_block_3rd) {
						sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_3RD,
									SEC_BAT_CURRENT_EVENT_SWELLING_MODE);
					}
				}
				/* restore 4.4V float voltage */
				val.intval = battery->pdata->swelling_normal_float_voltage;
				psy_do_property(battery->pdata->charger_name, set,
						POWER_SUPPLY_PROP_VOLTAGE_MAX, val);
				/* turn on charger by cable type */
				if ((battery->status == POWER_SUPPLY_STATUS_FULL) &&
					(battery->charging_mode == SEC_BATTERY_CHARGING_NONE)) {
					sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
				} else {
					sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
				}
			}
#else
			/* turn on charger by cable type */
			if ((battery->status == POWER_SUPPLY_STATUS_FULL) &&
				(battery->charging_mode == SEC_BATTERY_CHARGING_NONE)) {
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			} else {
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
			}
#endif
			return false;
		}
	}
	return true;
}

static bool sec_bat_check_fullcharged_condition(
					struct sec_battery_info *battery)
{
	int full_check_type = SEC_BATTERY_FULLCHARGED_NONE;

	if (battery->charging_mode == SEC_BATTERY_CHARGING_1ST)
		full_check_type = battery->pdata->full_check_type;
	else
		full_check_type = battery->pdata->full_check_type_2nd;

	switch (full_check_type) {
	case SEC_BATTERY_FULLCHARGED_ADC:
	case SEC_BATTERY_FULLCHARGED_FG_CURRENT:
	case SEC_BATTERY_FULLCHARGED_SOC:
	case SEC_BATTERY_FULLCHARGED_CHGGPIO:
	case SEC_BATTERY_FULLCHARGED_CHGPSY:
#if defined(CONFIG_DUAL_BATTERY)
	case SEC_BATTERY_FULLCHARGED_LIMITER:
#endif
		break;

	/* If these is NOT full check type or NONE full check type,
	 * it is full-charged
	 */
	case SEC_BATTERY_FULLCHARGED_CHGINT:
	case SEC_BATTERY_FULLCHARGED_TIME:
	case SEC_BATTERY_FULLCHARGED_NONE:
	default:
		return true;
	}

#if defined(CONFIG_ENABLE_FULL_BY_SOC)
	if (battery->capacity >= 100 &&
		!battery->is_recharging) {
		dev_info(battery->dev,
			"%s: enough SOC (%d%%), skip other full_condition_type\n",
			__func__, battery->capacity);
		return true;
	}
#endif

	if (battery->pdata->full_condition_type &
		SEC_BATTERY_FULL_CONDITION_SOC) {
		if (battery->capacity <
			battery->pdata->full_condition_soc) {
			dev_dbg(battery->dev,
				"%s: Not enough SOC (%d%%)\n",
				__func__, battery->capacity);
			return false;
		}
	}

	if (battery->pdata->full_condition_type &
		SEC_BATTERY_FULL_CONDITION_VCELL) {
		if (battery->voltage_now <
			battery->pdata->full_condition_vcell) {
			dev_dbg(battery->dev,
				"%s: Not enough VCELL (%dmV)\n",
				__func__, battery->voltage_now);
			return false;
		}
	}

	if (battery->pdata->full_condition_type &
		SEC_BATTERY_FULL_CONDITION_AVGVCELL) {
		if (battery->voltage_avg <
			battery->pdata->full_condition_avgvcell) {
			dev_dbg(battery->dev,
				"%s: Not enough AVGVCELL (%dmV)\n",
				__func__, battery->voltage_avg);
			return false;
		}
	}

	if (battery->pdata->full_condition_type &
		SEC_BATTERY_FULL_CONDITION_OCV) {
		if (battery->voltage_ocv <
			battery->pdata->full_condition_ocv) {
			dev_dbg(battery->dev,
				"%s: Not enough OCV (%dmV)\n",
				__func__, battery->voltage_ocv);
			return false;
		}
	}

	return true;
}

void sec_bat_do_test_function(
		struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };

	switch (battery->test_mode) {
	case 1:
		if (battery->status == POWER_SUPPLY_STATUS_CHARGING) {
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			sec_bat_set_charging_status(battery,
					POWER_SUPPLY_STATUS_DISCHARGING);
		}
		break;
	case 2:
		if (battery->status == POWER_SUPPLY_STATUS_DISCHARGING) {
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
			psy_do_property(battery->pdata->charger_name, get,
					POWER_SUPPLY_PROP_STATUS, value);
			sec_bat_set_charging_status(battery, value.intval);
		}
		battery->test_mode = 0;
		break;
	case 3: /* clear temp block */
		battery->health = POWER_SUPPLY_HEALTH_GOOD;
		sec_bat_set_charging_status(battery,
				POWER_SUPPLY_STATUS_DISCHARGING);
		break;
	case 4:
		if (battery->status == POWER_SUPPLY_STATUS_DISCHARGING) {
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
			psy_do_property(battery->pdata->charger_name, get,
					POWER_SUPPLY_PROP_STATUS, value);
			sec_bat_set_charging_status(battery, value.intval);
		}
		break;
	default:
		pr_info("%s: error test: unknown state\n", __func__);
		break;
	}
}

bool sec_bat_time_management(
				struct sec_battery_info *battery)
{
	struct timespec ts = {0, };
	unsigned long charging_time;

	if (battery->charging_start_time == 0 || !battery->safety_timer_set) {
		dev_dbg(battery->dev,
			"%s: Charging Disabled\n", __func__);
		return true;
	}

	get_monotonic_boottime(&ts);

	if (ts.tv_sec >= battery->charging_start_time) {
		charging_time = ts.tv_sec - battery->charging_start_time;
	} else {
		charging_time = 0xFFFFFFFF - battery->charging_start_time
			+ ts.tv_sec;
	}

	battery->charging_passed_time = charging_time;

	switch (battery->status) {
	case POWER_SUPPLY_STATUS_FULL:
		if (battery->expired_time == 0) {
			dev_info(battery->dev,
				"%s: Recharging Timer Expired\n", __func__);
			battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
			battery->health = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
			sec_bat_set_charging_status(battery, POWER_SUPPLY_STATUS_NOT_CHARGING);
			battery->is_recharging = false;
			if (sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF)) {
				dev_err(battery->dev,
					"%s: Fail to Set Charger\n", __func__);
				return true;
			}

			return false;
		}
		break;
	case POWER_SUPPLY_STATUS_CHARGING:
		if ((battery->pdata->full_condition_type &
		     SEC_BATTERY_FULL_CONDITION_NOTIMEFULL) &&
		    (battery->is_recharging && (battery->expired_time == 0))) {
			dev_info(battery->dev,
			"%s: Recharging Timer Expired\n", __func__);
			battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
			battery->health = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
			sec_bat_set_charging_status(battery, POWER_SUPPLY_STATUS_NOT_CHARGING);
			battery->is_recharging = false;
			if (sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF)) {
				dev_err(battery->dev,
					"%s: Fail to Set Charger\n", __func__);
				return true;
			}
			return false;
		} else if (!battery->is_recharging &&
			   (battery->expired_time == 0)) {
			dev_info(battery->dev,
				"%s: Charging Timer Expired\n", __func__);
			battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
			battery->health = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
			sec_bat_set_charging_status(battery, POWER_SUPPLY_STATUS_NOT_CHARGING);
#if defined(CONFIG_BATTERY_CISD)
			battery->cisd.data[CISD_DATA_SAFETY_TIMER]++;
			battery->cisd.data[CISD_DATA_SAFETY_TIMER_PER_DAY]++;
#endif
#if defined(CONFIG_SEC_ABC)
			sec_abc_send_event("MODULE=battery@ERROR=safety_timer");
#endif
			if (sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF)) {
				dev_err(battery->dev,
					"%s: Fail to Set Charger\n", __func__);
				return true;
			}
			return false;
		}
		break;
	default:
		dev_err(battery->dev,
			"%s: Undefine Battery Status\n", __func__);
		return true;
	}

	return true;
}

static bool sec_bat_check_fullcharged(
				struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	int current_adc = 0;
	int full_check_type = SEC_BATTERY_FULLCHARGED_NONE;
	bool ret = false;
	int err = 0;

	if (!sec_bat_check_fullcharged_condition(battery))
		goto not_full_charged;

	if (battery->charging_mode == SEC_BATTERY_CHARGING_1ST)
		full_check_type = battery->pdata->full_check_type;
	else
		full_check_type = battery->pdata->full_check_type_2nd;

	switch (full_check_type) {
	case SEC_BATTERY_FULLCHARGED_ADC:
		current_adc =
			sec_bat_get_adc_data(battery,
			SEC_BAT_ADC_CHANNEL_FULL_CHECK,
			battery->pdata->adc_check_count);

		dev_dbg(battery->dev,
			"%s: Current ADC (%d)\n",
			__func__, current_adc);

		if (current_adc < 0)
			break;
		battery->current_adc = current_adc;

		if (battery->current_adc <
			(battery->charging_mode ==
			SEC_BATTERY_CHARGING_1ST ?
			battery->pdata->full_check_current_1st :
			battery->pdata->full_check_current_2nd)) {
			battery->full_check_cnt++;
			dev_dbg(battery->dev,
				"%s: Full Check ADC (%d)\n",
				__func__,
				battery->full_check_cnt);
		} else
			battery->full_check_cnt = 0;
		break;

	case SEC_BATTERY_FULLCHARGED_FG_CURRENT:
		if ((battery->current_now > 0 && battery->current_now <
			battery->pdata->full_check_current_1st) &&
			(battery->current_avg > 0 && battery->current_avg <
			(battery->charging_mode ==
			SEC_BATTERY_CHARGING_1ST ?
			battery->pdata->full_check_current_1st :
			battery->pdata->full_check_current_2nd))) {
			battery->full_check_cnt++;
			dev_dbg(battery->dev,
				"%s: Full Check Current (%d)\n",
				__func__,
				battery->full_check_cnt);
		} else
			battery->full_check_cnt = 0;
		break;

	case SEC_BATTERY_FULLCHARGED_TIME:
		if ((battery->charging_mode ==
			SEC_BATTERY_CHARGING_2ND ?
			(battery->charging_passed_time -
			battery->charging_fullcharged_time) :
			battery->charging_passed_time) >
			(battery->charging_mode ==
			SEC_BATTERY_CHARGING_1ST ?
			battery->pdata->full_check_current_1st :
			battery->pdata->full_check_current_2nd)) {
			battery->full_check_cnt++;
			dev_dbg(battery->dev,
				"%s: Full Check Time (%d)\n",
				__func__,
				battery->full_check_cnt);
		} else
			battery->full_check_cnt = 0;
		break;

	case SEC_BATTERY_FULLCHARGED_SOC:
		if (battery->capacity <=
			(battery->charging_mode ==
			SEC_BATTERY_CHARGING_1ST ?
			battery->pdata->full_check_current_1st :
			battery->pdata->full_check_current_2nd)) {
			battery->full_check_cnt++;
			dev_dbg(battery->dev,
				"%s: Full Check SOC (%d)\n",
				__func__,
				battery->full_check_cnt);
		} else
			battery->full_check_cnt = 0;
		break;

	case SEC_BATTERY_FULLCHARGED_CHGGPIO:
		err = gpio_request(
			battery->pdata->chg_gpio_full_check,
			"GPIO_CHG_FULL");
		if (err) {
			dev_err(battery->dev,
				"%s: Error in Request of GPIO\n", __func__);
			break;
		}
		if (!(gpio_get_value_cansleep(
			battery->pdata->chg_gpio_full_check) ^
			!battery->pdata->chg_polarity_full_check)) {
			battery->full_check_cnt++;
			dev_dbg(battery->dev,
				"%s: Full Check GPIO (%d)\n",
				__func__, battery->full_check_cnt);
		} else
			battery->full_check_cnt = 0;
		gpio_free(battery->pdata->chg_gpio_full_check);
		break;

	case SEC_BATTERY_FULLCHARGED_CHGINT:
	case SEC_BATTERY_FULLCHARGED_CHGPSY:
		psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_STATUS, value);

		if (value.intval == POWER_SUPPLY_STATUS_FULL) {
			battery->full_check_cnt++;
			dev_info(battery->dev,
				"%s: Full Check Charger (%d)\n",
				__func__, battery->full_check_cnt);
		} else
			battery->full_check_cnt = 0;
		break;

	/* If these is NOT full check type or NONE full check type,
	 * it is full-charged
	 */
	case SEC_BATTERY_FULLCHARGED_NONE:
		battery->full_check_cnt = 0;
		ret = true;
		break;
#if defined(CONFIG_DUAL_BATTERY)
	case SEC_BATTERY_FULLCHARGED_LIMITER:
		value.intval = 1;
		psy_do_property(battery->pdata->dual_battery_name, get,
			POWER_SUPPLY_PROP_STATUS, value);
		if (value.intval == POWER_SUPPLY_STATUS_FULL) {
			battery->full_check_cnt++;
			dev_info(battery->dev,
				"%s: Full Check Limiter (%d)\n",
				__func__, battery->full_check_cnt);
		} else
			battery->full_check_cnt = 0;
		break;
#endif
	default:
		dev_err(battery->dev,
			"%s: Invalid Full Check\n", __func__);
		break;
	}

#if defined(CONFIG_ENABLE_FULL_BY_SOC)
	if (battery->capacity >= 100 &&
		battery->charging_mode == SEC_BATTERY_CHARGING_1ST &&
		!battery->is_recharging) {
		battery->full_check_cnt++;
		dev_info(battery->dev,
			"%s: enough SOC to make FULL(%d%%)\n",
			__func__, battery->capacity);
	}
#endif

	if (battery->full_check_cnt >=
		battery->pdata->full_check_count) {
		battery->full_check_cnt = 0;
		ret = true;
	}

not_full_charged:
	return ret;
}

void sec_bat_do_fullcharged(
				struct sec_battery_info *battery, bool force_fullcharged)
{
	union power_supply_propval value = {0, };

	/* To let charger/fuel gauge know the full status,
	 * set status before calling sec_bat_set_charge()
	 */
#if defined(CONFIG_BATTERY_CISD)
	if (battery->status != POWER_SUPPLY_STATUS_FULL) {
		battery->cisd.data[CISD_DATA_FULL_COUNT]++;
		battery->cisd.data[CISD_DATA_FULL_COUNT_PER_DAY]++;
	}
#endif
	sec_bat_set_charging_status(battery,
			POWER_SUPPLY_STATUS_FULL);

	if (battery->charging_mode == SEC_BATTERY_CHARGING_1ST &&
		battery->pdata->full_check_type_2nd != SEC_BATTERY_FULLCHARGED_NONE && !force_fullcharged) {
		battery->charging_mode = SEC_BATTERY_CHARGING_2ND;
		battery->charging_fullcharged_time = battery->charging_passed_time;
		value.intval = SEC_BAT_CHG_MODE_CHARGING_OFF;
		psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGING_ENABLED, value);
		sec_bat_set_charging_current(battery);
		sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
		pr_info("%s: 1st charging is done\n", __func__);
	} else {
		battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
		battery->is_recharging = false;

		if (!battery->wdt_kick_disable) {
			//pr_info("%s: wdt kick enable -> Charger Off, %d\n",
			//		__func__, battery->wdt_kick_disable);
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			pr_info("%s: 2nd charging is done\n", __func__);
		} else {
			pr_info("%s: wdt kick disabled -> skip charger off, %d\n",
					__func__, battery->wdt_kick_disable);
		}

#if defined(CONFIG_BATTERY_AGE_FORECAST)
		sec_bat_aging_check(battery);
#endif

		/* this concept is only for power-off charging mode*/
		if (is_hv_wire_type(battery->cable_type) && is_hv_wire_type(battery->wire_status) &&
			!battery->store_mode && (battery->cable_type != SEC_BATTERY_CABLE_QC30) &&
			lpcharge && !battery->vbus_chg_by_full) {
			/* vbus level : 9V --> 5V */
			battery->vbus_chg_by_full = true;
			battery->vbus_chg_by_siop = SEC_INPUT_VOLTAGE_5V;
			muic_afc_set_voltage(SEC_INPUT_VOLTAGE_5V/10);
			pr_info("%s: vbus is set 5V by 2nd full\n", __func__);
		}

		value.intval = POWER_SUPPLY_STATUS_FULL;
		psy_do_property(battery->pdata->fuelgauge_name, set,
			POWER_SUPPLY_PROP_STATUS, value);
	}

	/* platform can NOT get information of battery
	 * because wakeup time is too short to check uevent
	 * To make sure that target is wakeup if full-charged,
	 * activated wake lock in a few seconds
	 */
	if (battery->pdata->polling_type == SEC_BATTERY_MONITOR_ALARM)
		__pm_wakeup_event(battery->vbus_wake_lock, jiffies_to_msecs(HZ * 10));
}

bool sec_bat_fullcharged_check(
				struct sec_battery_info *battery)
{
	if ((battery->charging_mode == SEC_BATTERY_CHARGING_NONE) ||
		(battery->status == POWER_SUPPLY_STATUS_NOT_CHARGING)) {
		dev_dbg(battery->dev,
			"%s: No Need to Check Full-Charged\n", __func__);
		return true;
	}

	if (sec_bat_check_fullcharged(battery)) {
		union power_supply_propval value = {0, };

		if (battery->capacity < 100) {
			/* update capacity max */
			value.intval = battery->capacity;
			psy_do_property(battery->pdata->fuelgauge_name, set,
					POWER_SUPPLY_PROP_CHARGE_FULL, value);
			pr_info("%s : forced full-charged sequence for the capacity(%d)\n",
					__func__, battery->capacity);
			battery->full_check_cnt = battery->pdata->full_check_count;
		} else {
			sec_bat_do_fullcharged(battery, false);
		}
	}

	dev_info(battery->dev,
		"%s: Charging Mode : %s\n", __func__,
		battery->is_recharging ?
		sec_bat_charging_mode_str[SEC_BATTERY_CHARGING_RECHARGING] :
		sec_bat_charging_mode_str[battery->charging_mode]);

	return true;
}

static int sec_bat_adjust_temperature(struct sec_battery_info *battery,
	int read_temp, int prev_temp)
{
	int ret = read_temp;

	if (battery->pdata->batt_temp_adj_gap_inc) {
		if ((read_temp - prev_temp) > battery->pdata->batt_temp_adj_gap_inc)
			ret = prev_temp + battery->pdata->batt_temp_adj_gap_inc;
	}

	if (battery->pdata->batt_temp_adj_gap_dec) {
		if ((prev_temp - read_temp) > battery->pdata->batt_temp_adj_gap_dec)
			ret = prev_temp - battery->pdata->batt_temp_adj_gap_dec;
	}

	pr_info("%s: read: %d, prev: %d, now: %d\n",
			__func__, read_temp, prev_temp, ret);
	return ret;
}

#if !defined(CONFIG_SEC_FACTORY)
static void sec_bat_calc_unknown_wpc_temp(
	struct sec_battery_info *battery, int *batt_temp, int wpc_temp, int usb_temp)
{
	if (battery->support_unknown_wpcthm && battery->pdata->wpc_thermal_source &&
		!is_wireless_fake_type(battery->cable_type)) {
		if (wpc_temp <= (-200)) {
			if (usb_temp >= 270)
				*batt_temp = (usb_temp + 60) > 900 ? 900 : (usb_temp + 60);
			else if (usb_temp <= 210)
				*batt_temp = (usb_temp - 50) < (-200) ? (-200) : (usb_temp - 50);
			else
				*batt_temp = (170 * usb_temp - 26100) / 60;
		}
	}
}
#endif

void sec_bat_get_temperature_info(
				struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	static bool shipmode_en;
	int batt_temp = battery->temperature;
	int usb_temp = battery->usb_temp;
	int chg_temp = battery->chg_temp;
#if defined(CONFIG_DIRECT_CHARGING)
	int dchg_temp = battery->dchg_temp;
#endif
	int wpc_temp = battery->wpc_temp;
	int sub_bat_temp = battery->sub_bat_temp;
	int slave_temp = battery->slave_chg_temp;
	int blkt_temp = battery->blkt_temp;

	/* get battery thm info */
	switch (battery->pdata->thermal_source) {
	case SEC_BATTERY_THERMAL_SOURCE_FG:
		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_TEMP, value);
		battery->raw_bat_temp = value.intval;

		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_TEMP_AMBIENT, value);
		battery->temper_amb = value.intval;
		break;
	case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		if (battery->pdata->get_temperature_callback) {
			battery->pdata->get_temperature_callback(
				POWER_SUPPLY_PROP_TEMP, &value);
			battery->raw_bat_temp = value.intval;
			psy_do_property(battery->pdata->fuelgauge_name, set,
				POWER_SUPPLY_PROP_TEMP, value);

			battery->pdata->get_temperature_callback(
				POWER_SUPPLY_PROP_TEMP_AMBIENT, &value);
			battery->temper_amb = value.intval;
			psy_do_property(battery->pdata->fuelgauge_name, set,
				POWER_SUPPLY_PROP_TEMP_AMBIENT, value);
		}
		break;
	case SEC_BATTERY_THERMAL_SOURCE_ADC:
		if (sec_bat_get_value_by_adc(battery,
				SEC_BAT_ADC_CHANNEL_TEMP, &value, battery->pdata->temp_check_type)) {
			battery->raw_bat_temp = value.intval;
			battery->temper_amb = value.intval;
		} else {
			batt_temp = 0;
			battery->temper_amb = 0;
		}
		break;
	default:
		break;
	}

	if (battery->pdata->batt_temp_adj_gap_inc || battery->pdata->batt_temp_adj_gap_dec)
		batt_temp = sec_bat_adjust_temperature(battery, battery->raw_bat_temp, batt_temp);
	else
		batt_temp = battery->raw_bat_temp;

	/* get usb thm info */
	switch (battery->pdata->usb_thermal_source) {
	case SEC_BATTERY_THERMAL_SOURCE_FG:
	case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		break;
	case SEC_BATTERY_THERMAL_SOURCE_ADC:
		if (sec_bat_get_value_by_adc(battery,
			SEC_BAT_ADC_CHANNEL_USB_TEMP, &value, battery->pdata->usb_temp_check_type)) {
			usb_temp = value.intval;

			/* this shoud be moved */
			if (battery->vbus_limit && usb_temp <= battery->temp_highlimit_recovery)
				battery->vbus_limit = false;
		} else
			usb_temp = 0;
		break;
	default:
		break;
	}

	/* get chg thm info */
	switch (battery->pdata->chg_thermal_source) {
	case SEC_BATTERY_THERMAL_SOURCE_FG:
	case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		break;
	case SEC_BATTERY_THERMAL_SOURCE_ADC:
		if (sec_bat_get_value_by_adc(battery,
			SEC_BAT_ADC_CHANNEL_CHG_TEMP, &value, battery->pdata->chg_temp_check_type)) {
			chg_temp = value.intval;
		} else
			chg_temp = 0;
		break;
	default:
		break;
	}

#if defined(CONFIG_DIRECT_CHARGING)
	if (is_pd_apdo_wire_type(battery->wire_status)) {
		switch (battery->pdata->dchg_thermal_source) {
		case SEC_BATTERY_THERMAL_SOURCE_CHG_ADC:
			psy_do_property(battery->pdata->charger_name, get,
				POWER_SUPPLY_PROP_TEMP, value);

			dchg_temp = sec_bat_get_direct_chg_temp_adc(battery,
								value.intval, battery->pdata->adc_check_count);
			break;
		case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		case SEC_BATTERY_THERMAL_SOURCE_ADC:
			break;
		default:
			break;
		}
	}
#endif

	/* get wpc thm info */
	switch (battery->pdata->wpc_thermal_source) {
	case SEC_BATTERY_THERMAL_SOURCE_FG:
	case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		break;
	case SEC_BATTERY_THERMAL_SOURCE_ADC:
		if (sec_bat_get_value_by_adc(battery,
			SEC_BAT_ADC_CHANNEL_WPC_TEMP, &value, battery->pdata->wpc_temp_check_type)) {
			wpc_temp = value.intval;
		} else
			wpc_temp = 0;
		break;
	default:
		break;
	}

	/* get sub bat thm info */
	switch (battery->pdata->sub_bat_thermal_source) {
	case SEC_BATTERY_THERMAL_SOURCE_FG:
	case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		break;
	case SEC_BATTERY_THERMAL_SOURCE_ADC:
		if (sec_bat_get_value_by_adc(battery,
			SEC_BAT_ADC_CHANNEL_SUB_BAT_TEMP, &value, battery->pdata->sub_bat_temp_check_type)) {
			sub_bat_temp = value.intval;
		} else
			sub_bat_temp = 0;
		break;
	default:
		break;
	}

	/* get slave thm info */
	switch (battery->pdata->slave_thermal_source) {
	case SEC_BATTERY_THERMAL_SOURCE_FG:
	case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		break;
	case SEC_BATTERY_THERMAL_SOURCE_ADC:
		if (sec_bat_get_value_by_adc(battery,
			SEC_BAT_ADC_CHANNEL_SLAVE_CHG_TEMP, &value, battery->pdata->slave_chg_temp_check_type)) {
			slave_temp = value.intval;

			/* set temperature */
			value.intval = ((slave_temp) << 16) | (chg_temp);
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_TEMP, value);
		} else
			slave_temp = 0;
		break;
	default:
		break;
	}

	/* get blkt thm info */
	switch (battery->pdata->blkt_thermal_source) {
	case SEC_BATTERY_THERMAL_SOURCE_FG:
	case SEC_BATTERY_THERMAL_SOURCE_CALLBACK:
		break;
	case SEC_BATTERY_THERMAL_SOURCE_ADC:
		if (sec_bat_get_value_by_adc(battery,
			SEC_BAT_ADC_CHANNEL_BLKT_TEMP, &value, battery->pdata->blkt_temp_check_type)) {
			blkt_temp = value.intval;
		} else
			blkt_temp = 0;
		break;
	default:
		break;
	}

#if defined(CONFIG_ENG_BATTERY_CONCEPT)
	if (battery->temperature_test_battery > -300 && battery->temperature_test_battery < 3000) {
		pr_info("%s : battery temperature test %d\n", __func__, battery->temperature_test_battery);
		batt_temp = battery->temperature_test_battery;
	}
	if (battery->temperature_test_usb > -300 && battery->temperature_test_usb < 3000) {
		pr_info("%s : usb temperature test %d\n", __func__, battery->temperature_test_usb);
		usb_temp = battery->temperature_test_usb;
	}
	if (battery->temperature_test_wpc > -300 && battery->temperature_test_wpc < 3000) {
		pr_info("%s : wpc temperature test %d\n", __func__, battery->temperature_test_wpc);
		wpc_temp = battery->temperature_test_wpc;
	}
	if (battery->temperature_test_chg > -300 && battery->temperature_test_chg < 3000) {
		pr_info("%s : chg temperature test %d\n", __func__, battery->temperature_test_chg);
		chg_temp = battery->temperature_test_chg;
	}
	if (battery->temperature_test_blkt > -300 && battery->temperature_test_blkt < 3000) {
		pr_info("%s : blkt temperature test %d\n", __func__, battery->temperature_test_blkt);
		blkt_temp = battery->temperature_test_blkt;
	}
#if defined(CONFIG_DUAL_BATTERY)
	if (battery->temperature_test_sub > -300 && battery->temperature_test_sub < 3000) {
		pr_info("%s : sub temperature test %d\n", __func__, battery->temperature_test_sub);
		sub_bat_temp = battery->temperature_test_sub;
	}
#endif
#if defined(CONFIG_DIRECT_CHARGING)
	if (battery->temperature_test_dchg > -300 && battery->temperature_test_dchg < 3000) {
		pr_info("%s : direct chg temperature test %d\n", __func__, battery->temperature_test_dchg);
		dchg_temp = battery->temperature_test_dchg;
	}
#endif
#endif

#if !defined(CONFIG_SEC_FACTORY)
	sec_bat_calc_unknown_wpc_temp(battery, &batt_temp, wpc_temp, usb_temp);
#endif

	battery->temperature = batt_temp;
	battery->usb_temp = usb_temp;
	battery->chg_temp = chg_temp;
#if defined(CONFIG_DIRECT_CHARGING)
	battery->dchg_temp = dchg_temp;
#endif
	battery->wpc_temp = wpc_temp;
	battery->sub_bat_temp = sub_bat_temp;
	battery->slave_chg_temp = slave_temp;
	battery->blkt_temp = blkt_temp;

#if defined(CONFIG_SEC_FACTORY)
	if (battery->pdata->usb_temp_check_type) {
		if (battery->temperature <= (-200))
			value.intval = (battery->usb_temp <= (-200) ? battery->chg_temp : battery->usb_temp);
		else
			value.intval = battery->temperature;
	}
#else
	value.intval = battery->temperature;
#endif
	psy_do_property(battery->pdata->fuelgauge_name, set,
		POWER_SUPPLY_PROP_TEMP, value);

	psy_do_property(battery->pdata->fuelgauge_name, set,
		POWER_SUPPLY_PROP_TEMP_AMBIENT, value);

	if (!battery->pdata->dis_auto_shipmode_temp_ctrl) {
		if (battery->temperature < 0 && !shipmode_en) {
			value.intval = 0;
			psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_EXT_PROP_AUTO_SHIPMODE_CONTROL, value);
			shipmode_en = true;
		} else if (battery->temperature >= 50 && shipmode_en) {
			value.intval = 1;
			psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_EXT_PROP_AUTO_SHIPMODE_CONTROL, value);
			shipmode_en = false;
		}
	}
}

#if defined(CONFIG_BATTERY_SWELLING)
void sec_bat_swelling_fullcharged_check(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	int full_check_type = SEC_BATTERY_FULLCHARGED_NONE;
	int topoff_current = 0;

	if (battery->charging_mode == SEC_BATTERY_CHARGING_1ST) {
		full_check_type = battery->pdata->full_check_type;
		topoff_current = battery->pdata->full_check_current_1st;
	} else {
		full_check_type = battery->pdata->full_check_type_2nd;
		topoff_current = battery->pdata->full_check_current_2nd;
	}

	switch (full_check_type) {
	case SEC_BATTERY_FULLCHARGED_CHGINT:
	case SEC_BATTERY_FULLCHARGED_CHGPSY:
		psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_STATUS, value);
		if (value.intval == POWER_SUPPLY_STATUS_FULL) {
			battery->swelling_full_check_cnt++;
			pr_info("%s: Swelling mode full-charged check (%d)\n",
				__func__, battery->swelling_full_check_cnt);
		} else
			battery->swelling_full_check_cnt = 0;
		break;

	case SEC_BATTERY_FULLCHARGED_FG_CURRENT:
		if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_MODE)
			topoff_current = battery->pdata->swelling_low_temp_topoff;
		else if (battery->current_event & SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING)
			topoff_current = battery->pdata->swelling_high_temp_topoff;

		if ((battery->current_now > 0 && battery->current_now <
			battery->pdata->full_check_current_1st) &&
			(battery->current_avg > 0 && battery->current_avg < topoff_current)) {
			battery->swelling_full_check_cnt++;
			pr_info("%s: Swelling mode full-charged check (%d)\n",
				__func__, battery->swelling_full_check_cnt);
		} else
			battery->swelling_full_check_cnt = 0;
		break;
	default:
		pr_info("%s: Invalid Full Check\n", __func__);
		break;
	}
	if (battery->swelling_full_check_cnt >=
		battery->pdata->full_check_count) {
		battery->swelling_full_check_cnt = 0;
		battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
		battery->is_recharging = false;
		battery->swelling_mode = SWELLING_MODE_FULL;
		sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
		battery->expired_time = battery->pdata->expired_time;
		battery->prev_safety_time = 0;
#if defined(CONFIG_BATTERY_CISD)
		battery->cisd.data[CISD_DATA_SWELLING_FULL_CNT]++;
		battery->cisd.data[CISD_DATA_SWELLING_FULL_CNT_PER_DAY]++;
#endif
#if defined(CONFIG_DUAL_BATTERY)
		/* enable supplement mode in order to prevent balacing each battery */
		value.intval = 1;
		psy_do_property(battery->pdata->dual_battery_name, set,
		POWER_SUPPLY_PROP_CHARGING_ENABLED, value);
#endif
	}
}
#endif
void sec_bat_cable_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, cable_work.work);
	union power_supply_propval val = {0, };
	int current_cable_type = SEC_BATTERY_CABLE_NONE;
	int current_wire_status = READ_ONCE(battery->wire_status);
	int prev_cable_type = READ_ONCE(battery->cable_type);
	int cable_type;
	int wc_status;
	int monitor_work_delay = 0;
	int wire_current = 0;

	dev_info(battery->dev, "%s: Start\n", __func__);
	sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL,
				  SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL);
#if defined(CONFIG_CCIC_NOTIFIER)
	if (is_pd_wire_type(current_wire_status)) {
		if (battery->pdic_info.sink_status.selected_pdo_num ==
			battery->pdic_info.sink_status.current_pdo_num)
			sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_SELECT_PDO);
		sec_bat_get_input_current_in_power_list(battery);
		sec_bat_get_charging_current_in_power_list(battery);
#if defined(CONFIG_STEP_CHARGING) && defined(CONFIG_DIRECT_CHARGING)
		if (is_pd_apdo_wire_type(READ_ONCE(battery->cable_type)) && (battery->ta_alert_mode != OCP_NONE)) {
			battery->ta_alert_mode = OCP_WA_ACTIVE;
			sec_bat_reset_step_charging(battery);
		}
#endif
#if defined(CONFIG_PDIC_PD30)
		if (!battery->pd_list.pd_info[battery->pd_list.now_pd_index].comm_capable
			|| !battery->pd_list.pd_info[battery->pd_list.now_pd_index].suspend) {
			pr_info("%s : clear suspend event now_pd_index:%d, comm:%d, suspend:%d\n", __func__,
				battery->pd_list.now_pd_index,
				battery->pd_list.pd_info[battery->pd_list.now_pd_index].comm_capable,
				battery->pd_list.pd_info[battery->pd_list.now_pd_index].suspend);
			sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_USB_SUSPENDED);
		}
#endif
	}
#endif

	if (is_wired_type(current_wire_status)) {
		wire_current = (current_wire_status == SEC_BATTERY_CABLE_PREPARE_TA ?
			battery->pdata->charging_current[SEC_BATTERY_CABLE_TA].input_current_limit :
			battery->pdata->charging_current[current_wire_status].input_current_limit);

		wire_current = wire_current * (is_hv_wire_type(current_wire_status) ?
			(current_wire_status == SEC_BATTERY_CABLE_12V_TA ? SEC_INPUT_VOLTAGE_12V : SEC_INPUT_VOLTAGE_9V) / 10
			: SEC_INPUT_VOLTAGE_5V / 10);

		pr_info("%s: wr_cur(%d), wire_cable_type(%d)\n",
			__func__, wire_current, current_wire_status);
	}

	wc_status = READ_ONCE(battery->wc_status);

	if (wc_status && battery->wc_enable) {
		int wireless_current;
		int temp_current_type;

		if (wc_status == SEC_WIRELESS_PAD_WPC)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS;
		else if (wc_status == SEC_WIRELESS_PAD_WPC_HV)
			current_cable_type = SEC_BATTERY_CABLE_HV_WIRELESS;
		else if (wc_status == SEC_WIRELESS_PAD_WPC_PACK)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_PACK;
		else if (wc_status == SEC_WIRELESS_PAD_WPC_PACK_HV)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_HV_PACK;
		else if (wc_status == SEC_WIRELESS_PAD_WPC_STAND)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_STAND;
		else if (wc_status == SEC_WIRELESS_PAD_WPC_STAND_HV)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_HV_STAND;
		else if (wc_status == SEC_WIRELESS_PAD_VEHICLE)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_VEHICLE;
		else if (wc_status == SEC_WIRELESS_PAD_VEHICLE_HV)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_HV_VEHICLE;
		else if (wc_status == SEC_WIRELESS_PAD_PREPARE_HV)
			current_cable_type = SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV;
		else if (wc_status == SEC_WIRELESS_PAD_TX)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_TX;
		else if (wc_status == SEC_WIRELESS_PAD_WPC_PREPARE_HV_20)
			current_cable_type = SEC_BATTERY_CABLE_PREPARE_WIRELESS_20;
		else if (wc_status == SEC_WIRELESS_PAD_WPC_HV_20)
			current_cable_type = SEC_BATTERY_CABLE_HV_WIRELESS_20;
		else if (wc_status == SEC_WIRELESS_PAD_FAKE)
			current_cable_type = SEC_BATTERY_CABLE_WIRELESS_FAKE;
		else
			current_cable_type = SEC_BATTERY_CABLE_PMA_WIRELESS;

		if (current_cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV)
			temp_current_type = SEC_BATTERY_CABLE_HV_WIRELESS;
		else if (current_cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20)
			temp_current_type = SEC_BATTERY_CABLE_HV_WIRELESS_20;
		else
			temp_current_type = current_cable_type;

		if (!is_nocharge_type(current_wire_status)) {
			wireless_current = battery->pdata->charging_current[temp_current_type].input_current_limit;
			if (is_nv_wireless_type(temp_current_type))
				wireless_current = (wireless_current * SEC_INPUT_VOLTAGE_5_5V) / 10;
			else if (temp_current_type == SEC_BATTERY_CABLE_HV_WIRELESS_20)
				wireless_current = (wireless_current * battery->wc20_vout) / 10;
			else
				wireless_current = (wireless_current * SEC_INPUT_VOLTAGE_10V) / 10;

			if (is_pd_wire_type(current_wire_status)) {
				pr_info("%s: wl_cur(%d), pd_max_power(%d), wc_cable_type(%d), wire_cable_type(%d)\n",
					__func__, wireless_current, battery->pd_max_charge_power, current_cable_type, current_wire_status);
			} else {
				pr_info("%s: wl_cur(%d), wr_cur(%d), wc_cable_type(%d), wire_cable_type(%d)\n",
					__func__, wireless_current, wire_current, current_cable_type, current_wire_status);
			}

			if ((is_pd_wire_type(current_wire_status) && wireless_current < battery->pd_max_charge_power) ||
				(!is_pd_wire_type(current_wire_status) && wireless_current <= wire_current)) {
				current_cable_type = current_wire_status;
				pr_info("%s : switch charging path to cable\n", __func__);

				/* set limited charging current before switching cable charging from wireless charging,
				 * this step for wireless 2.0 -> HV cable charging
				 */
				if ((READ_ONCE(battery->cable_type) == SEC_BATTERY_CABLE_HV_WIRELESS_20) &&
					(temp_current_type == SEC_BATTERY_CABLE_HV_WIRELESS_20)) {
					val.intval = battery->pdata->wpc_charging_limit_current;
					pr_info("%s : set TA charging current %dmA for a moment in case of TA OCP\n", __func__, val.intval);
					mutex_lock(&battery->iolock);
					psy_do_property(battery->pdata->charger_name, set,
							POWER_SUPPLY_PROP_CURRENT_AVG, val);
#if defined(CONFIG_DUAL_BATTERY)
					sec_bat_divide_charging_current(battery, val.intval);
#endif
					battery->charging_current = val.intval;
					mutex_unlock(&battery->iolock);
					msleep(100);
				}

				battery->wc_need_ldo_on = true;
				val.intval = MFC_LDO_OFF;
				psy_do_property(battery->pdata->wireless_charger_name, set,
					POWER_SUPPLY_PROP_CHARGE_EMPTY, val);
				/* Turn off TX to charge by cable charging having more power */
				if (READ_ONCE(battery->wc_status) == SEC_WIRELESS_PAD_TX) {
					pr_info("@Tx_Mode %s : It is RX device with TA, notify TX device of this info\n", __func__);
					val.intval = true;
					psy_do_property(battery->pdata->wireless_charger_name, set,
						POWER_SUPPLY_EXT_PROP_WIRELESS_SWITCH, val);
				}
			} else {
				pr_info("%s : switch charging path to wireless\n", __func__);
				battery->wc_need_ldo_on = false;
				val.intval = MFC_LDO_ON;
				psy_do_property(battery->pdata->wireless_charger_name, set,
					POWER_SUPPLY_PROP_CHARGE_EMPTY, val);
			}
		} else {
			/*
			 * turn on ldo when ldo was off because of TA, ldo is supposed to
			 * turn on automatically except force off by sw. do not turn on
			 * ldo every wireless connection just in case ldo re-toggle by ic
			 */
			if (battery->wc_need_ldo_on) {
				battery->wc_need_ldo_on = false;
				val.intval = MFC_LDO_ON;
				psy_do_property(battery->pdata->wireless_charger_name, set,
					POWER_SUPPLY_PROP_CHARGE_EMPTY, val);
			}
		}
	}
#if defined(CONFIG_USE_POGO)
	else if (battery->pogo_status) {
		int pogo_current;

		current_cable_type = SEC_BATTERY_CABLE_POGO;

		if (current_wire_status != SEC_BATTERY_CABLE_NONE) {
			pogo_current = battery->pdata->charging_current[current_cable_type].input_current_limit;
			pogo_current = pogo_current * SEC_INPUT_VOLTAGE_5V;

			if (current_wire_status == SEC_BATTERY_CABLE_PDIC) {
				if (pogo_current < battery->pd_max_charge_power)
					current_cable_type = current_wire_status;
			} else {
				pr_info("%s: pogo_cur(%d), wr_cur(%d), pogo_cable_type(%d), wire_cable_type(%d)\n",
						__func__, pogo_current, wire_current, current_cable_type, current_wire_status);

				if (pogo_current < wire_current)
					current_cable_type = current_wire_status;
			}
		}
	}
#endif
	else {
		sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_HV_WIRELESS_20,
			battery->pdata->default_wc20_input_current,
			battery->pdata->default_wc20_charging_current);

		current_cable_type = current_wire_status;
	}

#if defined(CONFIG_BATTERY_SWELLING)
	cable_type = READ_ONCE(battery->cable_type);

	if (is_nocharge_type(current_cable_type) ||
		(is_nocharge_type(cable_type) && READ_ONCE(battery->swelling_mode) == SWELLING_MODE_NONE)) {
		WRITE_ONCE(battery->swelling_mode, SWELLING_MODE_NONE);
		/* restore 4.4V float voltage */
		val.intval = battery->pdata->swelling_normal_float_voltage;
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_VOLTAGE_MAX, val);
		pr_info("%s: float voltage = %d\n", __func__, val.intval);
	} else {
#if defined(CONFIG_STEP_CHARGING)
		sec_bat_reset_step_charging(battery);
#endif
		pr_info("%s: skip  float_voltage setting, swelling_mode(%d)\n",
			__func__, READ_ONCE(battery->swelling_mode));
	}
#endif

	cable_type = READ_ONCE(battery->cable_type);

	if ((current_cable_type == cable_type)
			&& !is_slate_mode(battery)
			&& !(battery->current_event & SEC_BAT_CURRENT_EVENT_USB_SUSPENDED)) {
		if (is_pd_wire_type(current_cable_type) && is_pd_wire_type(cable_type)) {
			cancel_delayed_work(&battery->afc_work);
			__pm_relax(battery->afc_wake_lock);
			sec_bat_set_current_event(battery, 0,
				SEC_BAT_CURRENT_EVENT_AFC | SEC_BAT_CURRENT_EVENT_AICL);
			battery->aicl_current = 0;
			sec_bat_set_charging_current(battery);
			power_supply_changed(battery->psy_bat);
		} else if (battery->prev_usb_conf != USB_CURRENT_NONE) {
			dev_info(battery->dev, "%s: set usb charging current to %d mA\n",
				__func__, battery->prev_usb_conf);
			sec_bat_set_charging_current(battery);
			battery->prev_usb_conf = USB_CURRENT_NONE;
		}
		dev_info(battery->dev, "%s: Cable is NOT Changed(%d)\n",
			__func__, cable_type);
		/* Do NOT activate cable work for NOT changed */
		goto end_of_cable_work;
	}

	/* to clear this value when cable type switched without dettach */

	cable_type = READ_ONCE(battery->cable_type);

	if ((is_wired_type(cable_type) && is_wireless_type(current_cable_type))
		|| (is_wireless_type(cable_type) && is_wired_type(current_cable_type))
		|| (battery->muic_cable_type == ATTACHED_DEV_AFC_CHARGER_DISABLED_MUIC))
		WRITE_ONCE(battery->max_charge_power, 0);

	if (current_cable_type == SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT)
		current_cable_type = SEC_BATTERY_CABLE_9V_TA;

	WRITE_ONCE(battery->cable_type, current_cable_type);
	cable_type = READ_ONCE(battery->cable_type);
	if (is_wireless_type(cable_type)) {
		power_supply_changed(battery->psy_bat);
		/* After 10sec wireless charging, Vrect headroom has to be reduced */
		__pm_stay_awake(battery->wc_headroom_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->wc_headroom_work,
			msecs_to_jiffies(10000));
	} else if (cable_type == SEC_BATTERY_CABLE_WIRELESS_FAKE) {
		power_supply_changed(battery->psy_bat);
	}

	if (battery->pdata->check_cable_result_callback)
		battery->pdata->check_cable_result_callback(cable_type);
	/* platform can NOT get information of cable connection
	 * because wakeup time is too short to check uevent
	 * To make sure that target is wakeup
	 * if cable is connected and disconnected,
	 * activated wake lock in a few seconds
	 */
	__pm_wakeup_event(battery->vbus_wake_lock, jiffies_to_msecs(HZ * 10));

	if (is_nocharge_type(current_wire_status)) {
		battery->prev_usb_conf = USB_CURRENT_NONE;
		/* usb default current is 100mA before configured*/
		sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_USB_100MA,
					  SEC_BAT_CURRENT_EVENT_USB_STATE);
	}

	cable_type = READ_ONCE(battery->cable_type);
	if (is_nocharge_type(cable_type) ||
		((battery->pdata->cable_check_type &
		SEC_BATTERY_CABLE_CHECK_NOINCOMPATIBLECHARGE) &&
		cable_type == SEC_BATTERY_CABLE_UNKNOWN)) {
		pr_info("%s: prev_cable_type(%d)\n", __func__, prev_cable_type);

		/* initialize all status */
		WRITE_ONCE(battery->charging_mode, SEC_BATTERY_CHARGING_NONE);
		battery->vbus_chg_by_siop = SEC_INPUT_VOLTAGE_NONE;
		battery->vbus_chg_by_full = false;
		battery->is_recharging = false;
#if defined(CONFIG_BATTERY_CISD)
		battery->cisd.ab_vbat_check_count = 0;
		battery->cisd.state &= ~CISD_STATE_OVER_VOLTAGE;
#endif
		battery->wc20_power_class = 0;
#if defined(CONFIG_CALC_TIME_TO_FULL)
		battery->ttf_predict_wc20_charge_current = 0;
#endif
		battery->wc20_vout = 0;
		battery->input_voltage = 0;
		battery->charge_power = 0;
		WRITE_ONCE(battery->max_charge_power, 0);
		battery->pd_max_charge_power = 0;
		sec_bat_set_charging_status(battery,
				POWER_SUPPLY_STATUS_DISCHARGING);
		battery->chg_limit = false;
		battery->lrp_limit = false;
		battery->lrp_step = LRP_NONE;
		battery->mix_limit = false;
		battery->chg_limit_recovery_cable = SEC_BATTERY_CABLE_NONE;
		battery->wc_heating_start_time = 0;
		battery->health = POWER_SUPPLY_HEALTH_GOOD;
		battery->prev_usb_conf = USB_CURRENT_NONE;
		battery->ta_alert_mode = OCP_NONE;
		cancel_delayed_work(&battery->afc_work);
		__pm_relax(battery->afc_wake_lock);
		sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
			battery->pdata->default_usb_input_current,
			battery->pdata->default_usb_charging_current);
		sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_TA,
			battery->pdata->default_input_current,
			battery->pdata->default_charging_current);
		sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_HV_WIRELESS_20,
			battery->pdata->default_wc20_input_current,
			battery->pdata->default_wc20_charging_current);
		/* usb default current is 100mA before configured*/
		sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_USB_100MA,
					  (SEC_BAT_CURRENT_EVENT_CHARGE_DISABLE |
					   SEC_BAT_CURRENT_EVENT_AFC |
					   SEC_BAT_CURRENT_EVENT_VBAT_OVP |
					   SEC_BAT_CURRENT_EVENT_VSYS_OVP |
					   SEC_BAT_CURRENT_EVENT_CHG_LIMIT |
					   SEC_BAT_CURRENT_EVENT_AICL |
					   SEC_BAT_CURRENT_EVENT_SELECT_PDO |
					   SEC_BAT_CURRENT_EVENT_WDT_EXPIRED |
					   SEC_BAT_CURRENT_EVENT_SAFETY_TMR |
					   SEC_BAT_CURRENT_EVENT_25W_OCP |
					   SEC_BAT_CURRENT_EVENT_DC_ERR |
					   SEC_BAT_CURRENT_EVENT_USB_STATE |
					   SEC_BAT_CURRENT_EVENT_SEND_UVDM));

		/* slate_mode needs to be clear manually since smart switch does not disable slate_mode sometimes */
		if (is_slate_mode(battery)) {
			pr_info("%s: slate_mode (%d)\n", __func__, battery->slate_mode);
			if (battery->slate_mode == SB_SLATE_SMART) {
				battery->slate_mode = SB_SLATE_NONE;
				sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_SLATE);
				dev_info(battery->dev,
						"%s: disable slate mode(smart switch) manually\n", __func__);
			}
		}
#if defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
		cancel_delayed_work(&battery->slowcharging_work);
#endif
		battery->wc_cv_mode = false;
		battery->is_sysovlo = false;
		battery->is_vbatovlo = false;
		battery->is_abnormal_temp = false;
		battery->auto_mode = false;
#if defined(CONFIG_PREVENT_USB_CONN_OVERHEAT)
		if (lpcharge)
			battery->usb_temp_flag = false;
#endif
		sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
		battery->usb_slow_chg = false;
	} else if (is_slate_mode(battery) || (battery->current_event & SEC_BAT_CURRENT_EVENT_USB_SUSPENDED)) {
		dev_info(battery->dev,
			"%s:slate mode on or set usb suspend\n", __func__);
		battery->is_recharging = false;
		WRITE_ONCE(battery->cable_type, SEC_BATTERY_CABLE_NONE);
		WRITE_ONCE(battery->charging_mode, SEC_BATTERY_CHARGING_NONE);
		battery->health = POWER_SUPPLY_HEALTH_GOOD;
		battery->is_sysovlo = false;
		battery->is_vbatovlo = false;
		battery->is_abnormal_temp = false;
		WRITE_ONCE(battery->swelling_mode, SWELLING_MODE_NONE);
		sec_bat_set_charging_status(battery,
			POWER_SUPPLY_STATUS_DISCHARGING);
		sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);

		if (battery->current_event & SEC_BAT_CURRENT_EVENT_USB_SUSPENDED) {
			battery->prev_usb_conf = USB_CURRENT_NONE;
			monitor_work_delay = 3000;
			goto run_monitor_work;
		}
	} else {
#if defined(CONFIG_EN_OOPS)
		val.intval = READ_ONCE(battery->cable_type);
		psy_do_property(battery->pdata->fuelgauge_name, set,
				POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, val);
#endif
		/* Do NOT display the charging icon when OTG or HMT_CONNECTED is enabled */
		if (READ_ONCE(battery->cable_type) == SEC_BATTERY_CABLE_OTG ||
			READ_ONCE(battery->cable_type) == SEC_BATTERY_CABLE_POWER_SHARING) {
			WRITE_ONCE(battery->charging_mode, SEC_BATTERY_CHARGING_NONE);
			battery->status = POWER_SUPPLY_STATUS_DISCHARGING;
		} else if (battery->misc_event & BATT_MISC_EVENT_FULL_CAPACITY) {
			battery->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		} else if (!battery->is_sysovlo && !battery->is_vbatovlo && !battery->is_abnormal_temp &&
				(!battery->charging_block || !READ_ONCE(battery->swelling_mode))) {
			if (battery->pdata->full_check_type !=
				SEC_BATTERY_FULLCHARGED_NONE)
				WRITE_ONCE(battery->charging_mode,
					SEC_BATTERY_CHARGING_1ST);
			else
				WRITE_ONCE(battery->charging_mode,
					SEC_BATTERY_CHARGING_2ND);

			if (battery->status == POWER_SUPPLY_STATUS_FULL)
				sec_bat_set_charging_status(battery,
						POWER_SUPPLY_STATUS_FULL);
			else
				sec_bat_set_charging_status(battery,
						POWER_SUPPLY_STATUS_CHARGING);
		}

		if (!battery->is_sysovlo && !battery->is_vbatovlo && !battery->is_abnormal_temp)
			battery->health = POWER_SUPPLY_HEALTH_GOOD;

		cable_type = READ_ONCE(battery->cable_type);
		wc_status = READ_ONCE(battery->wc_status);

		if (cable_type == SEC_BATTERY_CABLE_TA ||
			cable_type == SEC_BATTERY_CABLE_WIRELESS ||
			cable_type == SEC_BATTERY_CABLE_PMA_WIRELESS ||
			(is_hv_wire_type(cable_type) &&
			(wc_status == SEC_WIRELESS_PAD_WPC_PREPARE_HV_20 ||
			wc_status == SEC_WIRELESS_PAD_WPC_HV_20))) {
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_AFC, SEC_BAT_CURRENT_EVENT_AFC);
		} else {
			cancel_delayed_work(&battery->afc_work);
			__pm_relax(battery->afc_wake_lock);
			sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_AFC);
		}

		if (READ_ONCE(battery->cable_type) == SEC_BATTERY_CABLE_OTG ||
			READ_ONCE(battery->cable_type) == SEC_BATTERY_CABLE_POWER_SHARING) {
			sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
			goto end_of_cable_work;
		} else if (!battery->is_sysovlo && !battery->is_vbatovlo && !battery->is_abnormal_temp &&
				(!battery->charging_block || !READ_ONCE(battery->swelling_mode))) {
#if defined(CONFIG_ENABLE_FULL_BY_SOC)
			if (battery->capacity >= 100) {
				sec_bat_do_fullcharged(battery, true);
				dev_info(battery->dev,
					"%s: charging start at full, do not turn on charging\n", __func__);
			} else
#endif
				if (is_pd_wire_type(current_wire_status) && (battery->charge_power == 0)) {
					pr_info("%s: usb suspend mode of pd max power 0\n", __func__);
					sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_BUCK_OFF);
				} else {
					sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING);
				}
		}

#if defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
		if (READ_ONCE(battery->cable_type) == SEC_BATTERY_CABLE_USB && !lpcharge)
			queue_delayed_work(battery->monitor_wqueue, &battery->slowcharging_work,
						msecs_to_jiffies(3000));
#endif

#if defined(CONFIG_CALC_TIME_TO_FULL)
		if (lpcharge) {
			cancel_delayed_work(&battery->timetofull_work);
			if (battery->current_event & SEC_BAT_CURRENT_EVENT_AFC) {
				int work_delay = 0;

				cable_type = READ_ONCE(battery->cable_type);
				if (!is_wireless_type(cable_type))
					work_delay = battery->pdata->pre_afc_work_delay;
				else
					work_delay = battery->pdata->pre_wc_afc_work_delay;

				queue_delayed_work(battery->monitor_wqueue,
					&battery->timetofull_work, msecs_to_jiffies(work_delay));
			}
		}
#endif
	}

	if (READ_ONCE(battery->cable_type) != SEC_BATTERY_CABLE_WIRELESS_FAKE) {
		/* set online(cable type) */
		val.intval = READ_ONCE(battery->cable_type);
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_ONLINE, val);
		psy_do_property(battery->pdata->fuelgauge_name, set,
			POWER_SUPPLY_PROP_ONLINE, val);
#if defined(CONFIG_DUAL_BATTERY)
		psy_do_property(battery->pdata->dual_battery_name, set,
			POWER_SUPPLY_PROP_ONLINE, val);
#endif
		/* set charging current */
		psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_CURRENT_AVG, val);
		battery->aicl_current = 0;
		sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_AICL);
		battery->input_current = val.intval;
		/* to init battery type current when wireless charging -> battery case */
		cable_type = READ_ONCE(battery->cable_type);

		if (is_nocharge_type(cable_type))
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CURRENT_MAX, val);
		if (battery->status != POWER_SUPPLY_STATUS_DISCHARGING)
			sec_bat_check_input_voltage(battery);
		sec_bat_set_charging_current(battery);
	}

	/* polling time should be reset when cable is changed
	 * polling_in_sleep should be reset also
	 * before polling time is re-calculated
	 * to prevent from counting 1 for events
	 * right after cable is connected
	 */
	battery->polling_in_sleep = false;
	sec_bat_get_polling_time(battery);

	dev_info(battery->dev,
		"%s: Status:%s, Sleep:%s, Charging:%s, Short Poll:%s\n",
		__func__, sec_bat_status_str[battery->status],
		battery->polling_in_sleep ? "Yes" : "No",
		(READ_ONCE(battery->charging_mode) ==
		SEC_BATTERY_CHARGING_NONE) ? "No" : "Yes",
		battery->polling_short ? "Yes" : "No");
	dev_info(battery->dev,
		"%s: Polling time is reset to %d sec.\n", __func__,
		battery->polling_time);

	battery->polling_count = 1;	/* initial value = 1 */

run_monitor_work:
	cancel_delayed_work(&battery->monitor_work);
	__pm_stay_awake(battery->monitor_wake_lock);
	queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, msecs_to_jiffies(monitor_work_delay));
end_of_cable_work:
	__pm_relax(battery->cable_wake_lock);
	dev_info(battery->dev, "%s: End\n", __func__);
}
