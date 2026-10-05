/*
 *  sec_battery_psy.c
 *  Samsung Mobile Battery Driver - power_supply property surface
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"

#if defined(CONFIG_CCIC_MAX77705)
#include <linux/ccic/max77705_usbc.h>
#include <linux/ccic/ccic_core.h>
#endif

#if defined(CONFIG_SEC_ABC)
#include <linux/sti/abc_common.h>
#endif

int sec_bat_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);
	int current_cable_type = SEC_BATTERY_CABLE_NONE;
	int full_check_type = SEC_BATTERY_FULLCHARGED_NONE;
	union power_supply_propval value = {0, };
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property) psp;

	dev_dbg(battery->dev,
		"%s: (%d,%d)\n", __func__, psp, val->intval);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (battery->charging_mode == SEC_BATTERY_CHARGING_1ST)
			full_check_type = battery->pdata->full_check_type;
		else
			full_check_type = battery->pdata->full_check_type_2nd;
		if ((full_check_type == SEC_BATTERY_FULLCHARGED_CHGINT) &&
			(val->intval == POWER_SUPPLY_STATUS_FULL))
			sec_bat_do_fullcharged(battery, false);
		sec_bat_set_charging_status(battery, val->intval);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (battery->cable_type != SEC_BATTERY_CABLE_WIRELESS_FAKE) {
			__pm_stay_awake(battery->monitor_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					   &battery->monitor_work, msecs_to_jiffies(100));
		}
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		current_cable_type = val->intval;
#if !defined(CONFIG_CCIC_NOTIFIER)
		if ((battery->muic_cable_type != ATTACHED_DEV_SMARTDOCK_TA_MUIC)
		    && ((current_cable_type == SEC_BATTERY_CABLE_SMART_OTG) ||
			(current_cable_type == SEC_BATTERY_CABLE_SMART_NOTG)))
			break;
#endif

		if (current_cable_type < 0) {
			dev_info(battery->dev,
					"%s: ignore event(%d)\n",
					__func__, current_cable_type);
		} else if (current_cable_type == SEC_BATTERY_CABLE_OTG) {
			battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
			battery->is_recharging = false;
			sec_bat_set_charging_status(battery,
					POWER_SUPPLY_STATUS_DISCHARGING);
			battery->cable_type = current_cable_type;
			__pm_stay_awake(battery->monitor_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					   &battery->monitor_work, 0);
			break;
		} else {
			battery->wire_status = current_cable_type;
			if (is_nocharge_type(battery->wire_status) &&
				(battery->wc_status != SEC_WIRELESS_PAD_NONE) )
				current_cable_type = SEC_BATTERY_CABLE_WIRELESS;
		}
		dev_info(battery->dev,
				"%s: current_cable(%d), wc_status(%d), wire_status(%d)\n",
				__func__, current_cable_type, battery->wc_status,
				battery->wire_status);

		/* cable is attached or detached
		 * if current_cable_type is minus value,
		 * check cable by sec_bat_get_cable_type()
		 * although SEC_BATTERY_CABLE_SOURCE_EXTERNAL is set
		 * (0 is SEC_BATTERY_CABLE_UNKNOWN)
		 */
		if ((current_cable_type >= 0) &&
			(current_cable_type < SEC_BATTERY_CABLE_MAX) &&
			(battery->pdata->cable_source_type &
			SEC_BATTERY_CABLE_SOURCE_EXTERNAL)) {

			__pm_stay_awake(battery->cable_wake_lock);
				queue_delayed_work(battery->monitor_wqueue,
					&battery->cable_work,0);
		} else {
			if (sec_bat_get_cable_type(battery,
						battery->pdata->cable_source_type)) {
				__pm_stay_awake(battery->cable_wake_lock);
					queue_delayed_work(battery->monitor_wqueue,
						&battery->cable_work,0);
			}
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		battery->capacity = val->intval;
		power_supply_changed(battery->psy_bat);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* If JIG is attached, the voltage is set as 1079 */
		pr_info("%s : set to the battery history : (%d)\n", __func__, val->intval);
		if (val->intval == 1079)	{
			battery->voltage_now = 1079;
			battery->voltage_avg = 1079;
			power_supply_changed(battery->psy_bat);
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue, &battery->monitor_work, 0);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		battery->present = val->intval;

		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work(battery->monitor_wqueue,
				   &battery->monitor_work, 0);
		break;
#if defined(CONFIG_BATTERY_SWELLING)
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		break;
#endif
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
	case POWER_SUPPLY_PROP_CHARGE_COUNTER_SHADOW:
		break;
	case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
		value.intval = val->intval;
		pr_info("%s: CHGIN-OTG %s\n", __func__, value.intval > 0 ? "on" : "off");
		psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
		break;
	case POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL:
		value.intval = val->intval;
		pr_info("%s: WCIN-UNO %s\n", __func__, value.intval > 0 ? "on" : "off");
		psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL, value);
		break;
#if defined(CONFIG_UPDATE_BATTERY_DATA)
	case POWER_SUPPLY_PROP_POWER_DESIGN:
		sec_bat_parse_dt(battery->dev, battery);
		break;
#endif
#if defined(CONFIG_BATTERY_CISD)
	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		pr_info("%s: Valert was occured! run monitor work for updating cisd data!\n", __func__);
		battery->cisd.data[CISD_DATA_VALERT_COUNT]++;
		battery->cisd.data[CISD_DATA_VALERT_COUNT_PER_DAY]++;
		__pm_stay_awake(battery->monitor_wake_lock);
		queue_delayed_work_on(0, battery->monitor_wqueue,
			&battery->monitor_work, 0);
		break;
#endif
	case POWER_SUPPLY_PROP_MAX ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_AICL_CURRENT:
			battery->aicl_current = val->intval;
			battery->max_charge_power = battery->charge_power = (battery->input_voltage * val->intval) / 10;
			pr_info("%s: aicl : %dmA, %dmW)\n", __func__,
				battery->aicl_current, battery->charge_power);
				if (is_wired_type(battery->cable_type))
					sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_AICL,
						SEC_BAT_CURRENT_EVENT_AICL);

#if defined(CONFIG_BATTERY_CISD)
				battery->cisd.data[CISD_DATA_AICL_COUNT]++;
				battery->cisd.data[CISD_DATA_AICL_COUNT_PER_DAY]++;
#endif
			break;
		case POWER_SUPPLY_EXT_PROP_SYSOVLO:
			if (battery->status != POWER_SUPPLY_STATUS_DISCHARGING) {
				pr_info("%s: Vsys is ovlo !!\n", __func__);
				battery->is_sysovlo = true;
				battery->is_recharging = false;
				battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
				battery->health = POWER_SUPPLY_HEALTH_VSYS_OVP;
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_VSYS_OVP, SEC_BAT_CURRENT_EVENT_VSYS_OVP);
				sec_bat_set_charging_status(battery, POWER_SUPPLY_STATUS_NOT_CHARGING);
#if defined(CONFIG_BATTERY_CISD)
				battery->cisd.data[CISD_DATA_VSYS_OVP]++;
				battery->cisd.data[CISD_DATA_VSYS_OVP_PER_DAY]++;
#endif
#if defined(CONFIG_SEC_ABC)
				sec_abc_send_event("MODULE=battery@ERROR=vsys_ovp");
#endif
				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
				__pm_stay_awake(battery->monitor_wake_lock);
				queue_delayed_work(battery->monitor_wqueue,
						   &battery->monitor_work, 0);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_VBAT_OVP:
			if (battery->status != POWER_SUPPLY_STATUS_DISCHARGING) {
				pr_info("%s: Vbat is ovlo !!\n", __func__);
				battery->is_vbatovlo = true;
				battery->is_recharging = false;
				battery->charging_mode = SEC_BATTERY_CHARGING_NONE;
				battery->health = POWER_SUPPLY_HEALTH_VBAT_OVP;
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_VBAT_OVP, SEC_BAT_CURRENT_EVENT_VBAT_OVP);
				sec_bat_set_charging_status(battery, POWER_SUPPLY_STATUS_NOT_CHARGING);

				sec_bat_set_charge(battery, SEC_BAT_CHG_MODE_CHARGING_OFF);
				__pm_stay_awake(battery->monitor_wake_lock);
				queue_delayed_work(battery->monitor_wqueue,
						   &battery->monitor_work, 0);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_USB_CONFIGURE:
			pr_info("%s: usb configured %d\n", __func__, val->intval);

			if (val->intval != battery->prev_usb_conf) {
				int cable_work_delay = 0;

				if (val->intval == USB_CURRENT_UNCONFIGURED) {
					sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_USB_100MA, SEC_BAT_CURRENT_EVENT_USB_STATE);
				} else if ((val->intval == USB_CURRENT_HIGH_SPEED) || (val->intval == USB_CURRENT_SUPER_SPEED)) {
					battery->usb_slow_chg = false;
					if (val->intval == USB_CURRENT_HIGH_SPEED) {
						sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_USB_STATE);
						sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
								USB_CURRENT_HIGH_SPEED, USB_CURRENT_HIGH_SPEED);
					} else {
						sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_USB_SUPER, SEC_BAT_CURRENT_EVENT_USB_STATE);
						sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
								USB_CURRENT_SUPER_SPEED, USB_CURRENT_SUPER_SPEED);
					}

					if (battery->pdic_info.sink_status.rp_currentlvl == RP_CURRENT_LEVEL3) {
						if (battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE) {
							sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
									battery->pdata->default_input_current, battery->pdata->default_charging_current);
						} else {
							if(battery->store_mode || !battery->charging_enabled)
								sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
										battery->pdata->rp_current_rdu_rp3, battery->pdata->max_charging_current);
							else
								sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
										battery->pdata->rp_current_rp3, battery->pdata->max_charging_current);
						}
					} else if (battery->pdic_info.sink_status.rp_currentlvl == RP_CURRENT_LEVEL2) {
						sec_bat_change_default_current(battery, SEC_BATTERY_CABLE_USB,
								battery->pdata->rp_current_rp2, battery->pdata->rp_current_rp2);
					}
				} else if (val->intval == USB_CURRENT_SUSPENDED) {
					battery->usb_slow_chg = false;
					sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_USB_SUSPENDED, SEC_BAT_CURRENT_EVENT_USB_STATE);
					cable_work_delay = 500;
				}
				battery->prev_usb_conf = val->intval;

				cancel_delayed_work(&battery->cable_work);
				__pm_stay_awake(battery->cable_wake_lock);
				queue_delayed_work(battery->monitor_wqueue, &battery->cable_work, msecs_to_jiffies(cable_work_delay));
			}
			break;
		case POWER_SUPPLY_EXT_PROP_HV_DISABLE:
#if !defined(CONFIG_PD_CHARGER_HV_DISABLE)
			pr_info("HV wired charging mode is %s\n", (val->intval == CH_MODE_AFC_DISABLE_VAL ? "Disabled" : "Enabled"));
			if (val->intval == CH_MODE_AFC_DISABLE_VAL) {
				sec_bat_set_current_event(battery,
					SEC_BAT_CURRENT_EVENT_HV_DISABLE, SEC_BAT_CURRENT_EVENT_HV_DISABLE);

				if (is_pd_wire_type(battery->cable_type)) {
					battery->update_pd_list = true;
					pr_info("%s: update pd list\n", __func__);
					select_pdo(1);
				}
			} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE) {
				int target_pd_index = 0;

				sec_bat_set_current_event(battery,
					0, SEC_BAT_CURRENT_EVENT_HV_DISABLE);

				if (is_pd_wire_type(battery->cable_type)) {
					battery->update_pd_list = true;
					pr_info("%s: update pd list\n", __func__);
#if defined(CONFIG_PDIC_PD30)
					target_pd_index = battery->pd_list.num_fpdo - 1;
#else
					target_pd_index = battery->pd_list.max_pd_count - 1;
#endif
					if (target_pd_index >= 0 && target_pd_index < MAX_PDO_NUM)
						select_pdo(battery->pd_list.pd_info[target_pd_index].pdo_index);
				}
			}
#endif
			break;
		case POWER_SUPPLY_EXT_PROP_WC_CONTROL:
			pr_info("%s: Recover MFC IC (wc_enable: %d)\n",
				__func__, battery->wc_enable);

			mutex_lock(&battery->wclock);
			if (battery->wc_enable) {
#if defined(CONFIG_DISABLE_MFC_IC)
				sec_bat_set_mfc_off(battery, false);
#else
				if (battery->pdata->wpc_en) {
					gpio_direction_output(battery->pdata->wpc_en, 1);
					pr_info("%s: WC CONTROL: Disable\n", __func__);
				}
#endif
				msleep(500);

#if defined(CONFIG_DISABLE_MFC_IC)
				sec_bat_set_mfc_on(battery);
#else
				if (battery->pdata->wpc_en) {
					gpio_direction_output(battery->pdata->wpc_en, 0);
					pr_info("%s: WC CONTROL: Enable\n", __func__);
				}
#endif
			}
			mutex_unlock(&battery->wclock);
			break;
		case POWER_SUPPLY_EXT_PROP_WDT_STATUS:
			if (val->intval)
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_WDT_EXPIRED,
					SEC_BAT_CURRENT_EVENT_WDT_EXPIRED);
			break;
		case POWER_SUPPLY_EXT_PROP_CURRENT_EVENT:
			if (!(battery->current_event & val->intval)) {
				pr_info("%s: set new current_event %d\n", __func__, val->intval);
				if (val->intval == SEC_BAT_CURRENT_EVENT_DC_ERR)
					battery->cisd.event_data[EVENT_DC_ERR]++;
				sec_bat_set_current_event(battery, val->intval, val->intval);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_CURRENT_EVENT_CLEAR:
			pr_info("%s: new current_event clear %d\n", __func__, val->intval);
			sec_bat_set_current_event(battery, 0, val->intval);
			break;
#if defined(CONFIG_WIRELESS_TX_MODE)
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_AVG_CURR:
			break;
#endif
		case POWER_SUPPLY_EXT_PROP_SRCCAP:
			pr_info("%s: set init_src_cap(%d->%d)",
				__func__, battery->init_src_cap, val->intval);
			battery->init_src_cap = true;
			break;
#if defined(CONFIG_DIRECT_CHARGING)
		case POWER_SUPPLY_EXT_PROP_DIRECT_TA_ALERT:
			if (battery->ta_alert_wa) {
				pr_info("@TA_ALERT: %s: TA OCP DETECT\n", __func__);
				battery->cisd.event_data[EVENT_TA_OCP_DET]++;
				if (battery->ta_alert_mode == OCP_NONE)
					battery->cisd.event_data[EVENT_TA_OCP_ON]++;
				battery->ta_alert_mode = OCP_DETECT;
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_25W_OCP,
							SEC_BAT_CURRENT_EVENT_25W_OCP);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_DIRECT_SEND_UVDM:
			if (is_pd_apdo_wire_type(battery->cable_type)) {
				char direct_charging_source_status[2] = {0, };

				pr_info("@SEND_UVDM: Request Change Charging Source : %s\n",
					val->intval == 0 ? "Switch Charger" : "Direct Charger");

				direct_charging_source_status[0] = SEC_SEND_UVDM;
				direct_charging_source_status[1] = val->intval;

				sec_bat_set_current_event(battery, val->intval == 0 ?
					SEC_BAT_CURRENT_EVENT_SEND_UVDM : 0, SEC_BAT_CURRENT_EVENT_SEND_UVDM);
				value.strval = direct_charging_source_status;
				psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_EXT_PROP_CHANGE_CHARGING_SOURCE, value);
			}
			break;
#endif
#if defined(CONFIG_DUAL_BATTERY)
		case POWER_SUPPLY_EXT_PROP_FASTCHG_LIMIT_CURRENT:
			if (is_wireless_type(battery->cable_type))
				sec_bat_set_divide_charging_current(battery);
			break;
#endif
#if defined(CONFIG_DISABLE_MFC_IC)
		case POWER_SUPPLY_EXT_PROP_WPC_EN:
			sec_bat_set_current_event(battery,
				val->intval ? 0 : SEC_BAT_CURRENT_EVENT_WPC_EN, SEC_BAT_CURRENT_EVENT_WPC_EN);
			break;
#endif
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sec_bat_check_status(struct sec_battery_info *battery)
{
	if ((battery->health == POWER_SUPPLY_HEALTH_OVERVOLTAGE) ||
		(battery->health == POWER_SUPPLY_HEALTH_UNDERVOLTAGE))
		return POWER_SUPPLY_STATUS_DISCHARGING;

	if ((battery->pdata->cable_check_type &
		SEC_BATTERY_CABLE_CHECK_NOUSBCHARGE) &&
		!lpcharge) {
		switch (battery->cable_type) {
		case SEC_BATTERY_CABLE_USB:
		case SEC_BATTERY_CABLE_USB_CDP:
			return POWER_SUPPLY_STATUS_DISCHARGING;
		}
	}

#if defined(CONFIG_STORE_MODE)
	if (battery->store_mode && !lpcharge &&
		!is_nocharge_type(battery->cable_type) &&
		battery->status == POWER_SUPPLY_STATUS_DISCHARGING)
		return POWER_SUPPLY_STATUS_CHARGING;
#endif

	if (is_eu_eco_rechg(battery->fs) &&
		(battery->status == POWER_SUPPLY_STATUS_FULL)) {
		if (battery->is_recharging) {
#if defined(CONFIG_ENABLE_FULL_BY_SOC)
			if (battery->capacity < 100)
				return POWER_SUPPLY_STATUS_CHARGING;
#else
			return POWER_SUPPLY_STATUS_CHARGING;
#endif
		}

		if (check_eu_eco_rechg_ui_condition(battery))
			return POWER_SUPPLY_STATUS_CHARGING;
	}

	return battery->status;
}

int sec_bat_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);
	union power_supply_propval value = {0, };
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property) psp;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = sec_bat_check_status(battery);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
	{
		unsigned int input_current = battery->pdata->charging_current[battery->cable_type].input_current_limit;
		if (is_nocharge_type(battery->cable_type)) {
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		} else if (is_hv_wire_type(battery->cable_type) || is_pd_wire_type(battery->cable_type) || is_wireless_type(battery->cable_type)) {
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		} else if (input_current <= SLOW_CHARGING_CURRENT_STANDARD || battery->usb_slow_chg) {
			val->intval = POWER_SUPPLY_CHARGE_TYPE_SLOW;
			pr_info("%s: slow-charging mode\n", __func__);
		} else {
			psy_do_property(battery->pdata->charger_name, get,
				POWER_SUPPLY_PROP_CHARGE_TYPE, value);
			if (value.intval == SEC_BATTERY_CABLE_UNKNOWN)
				/* if error in CHARGE_TYPE of charger
				 * set CHARGE_TYPE as NONE
				 */
				val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
			else
				val->intval = value.intval;
		}
	}
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (lpcharge &&
			(battery->health == POWER_SUPPLY_HEALTH_OVERVOLTAGE ||
			battery->health == POWER_SUPPLY_HEALTH_UNDERVOLTAGE ||
			battery->health == POWER_SUPPLY_HEALTH_DC_ERR))
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		else if (battery->health >= POWER_SUPPLY_HEALTH_MAX)
			val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
		else
			val->intval = battery->health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = battery->present;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		if (is_hv_wireless_type(battery->cable_type) ||
			(battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV) ||
			(battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20)) {
			if (sec_bat_hv_wc_normal_mode_check(battery))
				val->intval = SEC_BATTERY_CABLE_WIRELESS;
			else
				val->intval = SEC_BATTERY_CABLE_HV_WIRELESS_ETX;
		} else if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_PACK ||
			battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_STAND ||
			battery->cable_type == SEC_BATTERY_CABLE_PMA_WIRELESS ||
			battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_VEHICLE ||
			battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_TX ||
			battery->cable_type == SEC_BATTERY_CABLE_HV_WIRELESS_20 ||
			battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_FAKE) {
			val->intval = SEC_BATTERY_CABLE_WIRELESS;
		} else {
			val->intval = battery->cable_type;
		}
		pr_debug("%s cable type = %d sleep_mode = %d\n", __func__, val->intval, sleep_mode);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = battery->pdata->technology;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
#ifdef CONFIG_SEC_FACTORY
		psy_do_property(battery->pdata->fuelgauge_name, get,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, value);
		battery->voltage_now = value.intval;
		dev_err(battery->dev,
			"%s: voltage now(%d)\n", __func__, battery->voltage_now);
#endif
		/* voltage value should be in uV */
		val->intval = battery->voltage_now * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
#ifdef CONFIG_SEC_FACTORY
		value.intval = SEC_BATTERY_VOLTAGE_AVERAGE;
		psy_do_property(battery->pdata->fuelgauge_name, get,
				POWER_SUPPLY_PROP_VOLTAGE_AVG, value);
		battery->voltage_avg = value.intval;
		dev_err(battery->dev,
			"%s: voltage avg(%d)\n", __func__, battery->voltage_avg);
#endif
		/* voltage value should be in uV */
		val->intval = battery->voltage_avg * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		value.intval = SEC_BATTERY_CURRENT_MA;
		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_CURRENT_NOW, value);
		val->intval = value.intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		value.intval = SEC_BATTERY_CURRENT_MA;
		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_CURRENT_AVG, value);
		val->intval = value.intval;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = battery->pdata->battery_full_capacity * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		value.intval = SEC_BATTERY_CAPACITY_AGEDCELL;
		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_PROP_ENERGY_NOW, value);
		val->intval = value.intval * 1000;
		break;
	/* charging mode (differ from power supply) */
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = battery->charging_mode;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (battery->pdata->fake_capacity) {
			val->intval = 90;
			pr_info("%s : capacity(%d)\n", __func__, val->intval);
		} else {
			val->intval = battery->capacity;
			if ((battery->status == POWER_SUPPLY_STATUS_FULL) &&
#if defined(CONFIG_ENG_BATTERY_CONCEPT)
				!battery->eng_not_full_status &&
#endif
				!is_eu_eco_rechg(battery->fs))
				val->intval = 100;
		}
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = battery->temperature;
		break;
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		val->intval = battery->temper_amb;
		break;
#if defined(CONFIG_FUELGAUGE_MAX77705)
	case POWER_SUPPLY_PROP_POWER_NOW:
		value.intval = SEC_BATTERY_ISYS_MA;
		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_EXT_PROP_MEASURE_SYS, value);
		val->intval = value.intval;
		break;
	case POWER_SUPPLY_PROP_POWER_AVG:
		value.intval = SEC_BATTERY_ISYS_AVG_MA;
		psy_do_property(battery->pdata->fuelgauge_name, get,
			POWER_SUPPLY_EXT_PROP_MEASURE_SYS, value);
		val->intval = value.intval;
		break;
#endif
#if defined(CONFIG_CALC_TIME_TO_FULL)
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		if (battery->capacity == 100) {
			val->intval = 0;
			break;
		}

		if (((battery->status == POWER_SUPPLY_STATUS_CHARGING) ||
			(battery->status == POWER_SUPPLY_STATUS_FULL && battery->capacity != 100)) &&
			(!battery->swelling_mode &&
			!(battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND)))
			val->intval = battery->timetofull;
		else
			val->intval = 0;
		break;
#endif
#if defined(CONFIG_BATTERY_SWELLING)
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		if (battery->swelling_mode)
			val->intval = 1;
		else
			val->intval = 0;
		break;
#endif
	case POWER_SUPPLY_PROP_CHARGE_COUNTER_SHADOW:
		val->intval = battery->wire_status;
		break;
	case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
	case POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL:
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = battery->charge_counter;
		break;
	case POWER_SUPPLY_PROP_MAX ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_SUB_PBA_TEMP_REC:
			val->intval = !battery->vbus_limit;
			break;
		case POWER_SUPPLY_EXT_PROP_CHARGE_POWER:
			val->intval = battery->charge_power;
			break;
		case POWER_SUPPLY_EXT_PROP_CURRENT_EVENT:
			val->intval = battery->current_event;
			break;
#if defined(CONFIG_WIRELESS_TX_MODE)
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_AVG_CURR:
			val->intval = battery->tx_avg_curr;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ENABLE:
			val->intval = battery->wc_tx_enable;
			break;
#endif
#if defined(CONFIG_DIRECT_CHARGING)
#if defined(CONFIG_PDIC_PD30)
		case POWER_SUPPLY_EXT_PROP_DIRECT_FIXED_PDO:
			val->intval = battery->pd_list.num_apdo > 0 ? battery->pd_list.num_fpdo : 1;
			break;
#endif
		case POWER_SUPPLY_EXT_PROP_DIRECT_CHARGER_MODE:
			val->intval = battery->pd_list.now_isApdo;
			break;
		case POWER_SUPPLY_EXT_PROP_DIRECT_HV_PDO:
			val->intval = battery->hv_pdo;
			break;
		case POWER_SUPPLY_EXT_PROP_DIRECT_HAS_APDO:
			val->intval = battery->pdic_info.sink_status.has_apdo;
			break;
#endif
		case POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL:
                  if (battery->wpc_vout_ctrl_lcd_on)
                    val->intval = battery->lcd_status;
                  else
                    val->intval = false;
			break;
#if defined(CONFIG_DIRECT_CHARGING)
		case POWER_SUPPLY_EXT_PROP_DIRECT_TA_ALERT:
			if (battery->ta_alert_wa) {
				val->intval = battery->ta_alert_mode;
			} else
				val->intval = OCP_NONE;
			break;
		case POWER_SUPPLY_EXT_PROP_DIRECT_SEND_UVDM:
			break;
#if defined(CONFIG_DUAL_BATTERY_CELL_SENSING)
		case POWER_SUPPLY_EXT_PROP_DIRECT_VBAT_CHECK:
			pr_info("%s : mc:%dmV, sc:%dmV\n", __func__, battery->voltage_cell_main, battery->voltage_cell_sub);
			if ((battery->voltage_cell_main - battery->pdata->main_cell_margin_cv >= battery->pdata->chg_float_voltage) ||
				(battery->voltage_cell_sub >= battery->pdata->chg_float_voltage))
				val->intval = 1;
			else
				val->intval = 0;
			break;
#endif
#endif
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int sec_usb_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);
#if defined(CONFIG_CCIC_MAX77705)
	struct max77705_usbc_platform_data *usbc_data;
	struct device *ccic_device = get_ccic_device();
#endif

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		/* Whatever -> uV */
		val->intval = battery->input_voltage / (SEC_INPUT_VOLTAGE_5V / 5) * 1000000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		/* mA -> uA */
		val->intval = battery->pdata->charging_current[battery->cable_type].input_current_limit * 1000;
		return 0;
#if defined(CONFIG_CCIC_MAX77705)
	case POWER_SUPPLY_PROP_MOISTURE_DETECTED: {
		if (!ccic_device) {
			pr_info("ccic_dev is null\n");
			return -ENODEV;
		}

		usbc_data = dev_get_drvdata(ccic_device);

		if (!usbc_data) {
			pr_info("usbc_data is null");
			return -ENODEV;
		}

		if (usbc_data->current_connstat)
			val->intval = 1;
		else
			val->intval = 0;

		return 0;
	}
#endif
	default:
		return -EINVAL;
	}

	if ((battery->health == POWER_SUPPLY_HEALTH_OVERVOLTAGE) ||
		(battery->health == POWER_SUPPLY_HEALTH_UNDERVOLTAGE)) {
		val->intval = 0;
		return 0;
	}
	/* Set enable=1 only if the USB charger is connected */
	switch (battery->wire_status) {
	case SEC_BATTERY_CABLE_USB:
	case SEC_BATTERY_CABLE_USB_CDP:
		val->intval = 1;
		break;
	case SEC_BATTERY_CABLE_PDIC:
	case SEC_BATTERY_CABLE_NONE:
	        val->intval = (battery->pd_usb_attached) ? 1:0;
	        break;
	default:
		val->intval = 0;
		break;
	}

	if (is_slate_mode(battery))
		val->intval = 0;
	return 0;
}

int sec_ac_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if ((battery->health == POWER_SUPPLY_HEALTH_OVERVOLTAGE) ||
				(battery->health == POWER_SUPPLY_HEALTH_UNDERVOLTAGE)) {
			val->intval = 0;
			return 0;
		}

		/* Set enable=1 only if the AC charger is connected */
		switch (battery->cable_type) {
		case SEC_BATTERY_CABLE_TA:
		case SEC_BATTERY_CABLE_UARTOFF:
		case SEC_BATTERY_CABLE_LAN_HUB:
		case SEC_BATTERY_CABLE_UNKNOWN:
		case SEC_BATTERY_CABLE_PREPARE_TA:
		case SEC_BATTERY_CABLE_9V_ERR:
		case SEC_BATTERY_CABLE_9V_UNKNOWN:
		case SEC_BATTERY_CABLE_9V_TA:
		case SEC_BATTERY_CABLE_12V_TA:
		case SEC_BATTERY_CABLE_HMT_CONNECTED:
		case SEC_BATTERY_CABLE_HMT_CHARGE:
		case SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT:
		case SEC_BATTERY_CABLE_QC20:
		case SEC_BATTERY_CABLE_QC30:
		case SEC_BATTERY_CABLE_TIMEOUT:
		case SEC_BATTERY_CABLE_SMART_OTG:
		case SEC_BATTERY_CABLE_SMART_NOTG:
		case SEC_BATTERY_CABLE_POGO:
#if defined(CONFIG_PDIC_PD30)
		case SEC_BATTERY_CABLE_PDIC_APDO:
#endif
			val->intval = 1;
			break;
		case SEC_BATTERY_CABLE_PDIC:
			val->intval = (battery->pd_usb_attached) ? 0:1;
			break;
		default:
			val->intval = 0;
			break;
		}
		if (lpcharge && (battery->misc_event & BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE))
			val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = battery->chg_temp;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		/* Whatever -> uV */
		val->intval = battery->input_voltage / (SEC_INPUT_VOLTAGE_5V / 5) * 1000000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		/* mA -> uA */
		val->intval = battery->pdata->charging_current[battery->cable_type].input_current_limit * 1000;
		return 0;
	default:
		return -EINVAL;
	}

	return 0;
}

int sec_wireless_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = is_wireless_fake_type(battery->cable_type) ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = (battery->pdata->wireless_charger_name) ?
			1 : 0;
		break;
	case POWER_SUPPLY_PROP_POWER_DESIGN:
		if (battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20)
			val->intval = battery->wc20_power_class;
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		/* Whatever -> uV */
		val->intval = battery->input_voltage / (SEC_INPUT_VOLTAGE_5V / 5) * 1000000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		/* mA -> uA */
		val->intval = battery->pdata->charging_current[battery->cable_type].input_current_limit * 1000;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void sec_wireless_test_print_sgf_data(const char *buf, int count)
{
	char temp_buf[1024] = {0, };
	int i, size = 0;

	snprintf(temp_buf, sizeof(temp_buf), "0x%x", buf[0]);
	size = sizeof(temp_buf) - strlen(temp_buf);

	for (i = 1; i < count; i++) {
		snprintf(temp_buf + strlen(temp_buf), size, " 0x%x", buf[i]);
		size = sizeof(temp_buf) - strlen(temp_buf);
	}

	pr_info("%s: %s\n", __func__, temp_buf);
}

static ssize_t sec_wireless_sgf_show_attr(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return -EINVAL;
}

static ssize_t sec_wireless_sgf_store_attr(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);
	union power_supply_propval value = {0, };

	if (count < 4) {
		pr_err("%s: invalid data\n", __func__);
		return -EINVAL;
	}

	psy_do_property(battery->pdata->wireless_charger_name, get,
		POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ID, value);
	pr_info("%s!!!(cable_type = %d, tx_id = %d, count = %ld)\n",
		__func__, battery->cable_type, value.intval, count);
	sec_wireless_test_print_sgf_data(buf, (int)count);

	if (is_wireless_type(battery->cable_type) && value.intval == 0xEF) {
		value.strval = buf;
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_SGF, value);
	}

	return count;
}
DEVICE_ATTR(sgf, 0664, sec_wireless_sgf_show_attr, sec_wireless_sgf_store_attr);

int sec_wireless_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property) psp;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
#if defined(CONFIG_BATTERY_CISD)
		if (val->intval != SEC_WIRELESS_PAD_NONE && battery->wc_status == SEC_WIRELESS_PAD_NONE) {
			battery->cisd.data[CISD_DATA_WIRELESS_COUNT]++;
			battery->cisd.data[CISD_DATA_WIRELESS_COUNT_PER_DAY]++;
		}
#endif
		pr_info("%s : wireless_type(0x%x)\n", __func__, val->intval);

		/* Clear the FOD , AUTH State */
		sec_bat_set_misc_event(battery, 0, BATT_MISC_EVENT_WIRELESS_FOD);

		battery->wc_status = val->intval;

		if ((battery->ext_event & BATT_EXT_EVENT_CALL) &&
			(battery->wc_status == SEC_WIRELESS_PAD_WPC_PACK ||
			battery->wc_status == SEC_WIRELESS_PAD_WPC_PACK_HV ||
			battery->wc_status == SEC_WIRELESS_PAD_TX)) {
				battery->wc_rx_phm_mode = true;
		}

		if (battery->wc_status == SEC_WIRELESS_PAD_NONE) {
			battery->wpc_vout_level = WIRELESS_VOUT_10V;
			battery->wpc_max_vout_level = WIRELESS_VOUT_12_5V;
			battery->auto_mode = false;

			if (delayed_work_pending(&battery->wc_headroom_work)) {
				__pm_relax(battery->wc_headroom_wake_lock);
				cancel_delayed_work(&battery->wc_headroom_work);
			}

			sec_bat_set_misc_event(battery, 0,
			(BATT_MISC_EVENT_WIRELESS_DET_LEVEL | /* clear wpc_det level status */
			BATT_MISC_EVENT_WIRELESS_AUTH_START |
			BATT_MISC_EVENT_WIRELESS_AUTH_RECVED |
			BATT_MISC_EVENT_WIRELESS_AUTH_FAIL |
			BATT_MISC_EVENT_WIRELESS_AUTH_PASS));
		} else if (battery->wc_status != SEC_WIRELESS_PAD_FAKE) {
			sec_bat_set_misc_event(battery, BATT_MISC_EVENT_WIRELESS_DET_LEVEL, /* set wpc_det level status */
			BATT_MISC_EVENT_WIRELESS_DET_LEVEL);
		
			if (battery->wc_status == SEC_WIRELESS_PAD_WPC_HV_20) {
				sec_bat_set_misc_event(battery, BATT_MISC_EVENT_WIRELESS_AUTH_PASS,
					BATT_MISC_EVENT_WIRELESS_AUTH_PASS);
#if defined(CONFIG_BATTERY_CISD)
				if (battery->wc_status == SEC_WIRELESS_PAD_WPC_HV_20)
					battery->cisd.cable_data[CISD_CABLE_HV_WC_20]++;
#endif
			}
		}

		__pm_stay_awake(battery->cable_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
				&battery->cable_work, 0);
		if (battery->wc_status == SEC_WIRELESS_PAD_NONE ||
			battery->wc_status == SEC_WIRELESS_PAD_WPC_PACK ||
			battery->wc_status == SEC_WIRELESS_PAD_WPC_PACK_HV ||
			battery->wc_status == SEC_WIRELESS_PAD_VEHICLE) {
			sec_bat_set_misc_event(battery,
				(battery->wc_status == SEC_WIRELESS_PAD_NONE ? 0 : BATT_MISC_EVENT_WIRELESS_BACKPACK_TYPE),
				BATT_MISC_EVENT_WIRELESS_BACKPACK_TYPE);
		}
		break;
	case POWER_SUPPLY_PROP_AUTHENTIC:
#if defined(CONFIG_BATTERY_CISD)
		pr_info("%s : tx_type(0x%x)\n", __func__, val->intval);
		count_cisd_pad_data(&battery->cisd, val->intval);
#endif
		break;
	case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
		sec_wireless_otg_control(battery, val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		if ((battery->aicl_current > 0) &&
			is_wireless_type(battery->cable_type)) {
			union power_supply_propval value;

			psy_do_property(battery->pdata->charger_name, get,
				POWER_SUPPLY_PROP_CURRENT_MAX, value);
			pr_info("%s: check input current(%d <--> %d) before reset AICL\n",
				__func__, battery->input_current, value.intval);

			if (battery->input_current != value.intval)
				battery->input_current = value.intval;
		}
		battery->aicl_current = 0; /* reset aicl current */
		sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_AICL);
		pr_info("%s: reset aicl\n", __func__);
		break;
	case POWER_SUPPLY_PROP_MAX ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ENABLE:
			sec_wireless_set_tx_enable(battery, val->intval);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_ERR:
			if (is_wireless_type(battery->cable_type))
				sec_bat_set_misc_event(battery, val->intval ? BATT_MISC_EVENT_WIRELESS_FOD : 0,
					BATT_MISC_EVENT_WIRELESS_FOD);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ERR:
			if (val->intval & BATT_TX_EVENT_WIRELESS_TX_MISALIGN) {
				sec_bat_handle_tx_misalign(battery, true);
			} else if (val->intval & BATT_TX_EVENT_WIRELESS_TX_OCP) {
				sec_bat_handle_tx_ocp(battery, true);
			} else {
				sec_bat_set_tx_event(battery, val->intval, val->intval);
				sec_wireless_set_tx_enable(battery, false);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_CONNECTED:
			sec_bat_set_tx_event(battery, val->intval ? BATT_TX_EVENT_WIRELESS_RX_CONNECT : 0,
				BATT_TX_EVENT_WIRELESS_RX_CONNECT);
			battery->wc_rx_connected = val->intval;
			if (!val->intval) {
				battery->wc_rx_type = NO_DEV;

				battery->tx_switch_mode = TX_SWITCH_MODE_OFF;
				battery->tx_switch_mode_change = false;	
				battery->tx_switch_start_soc = 0;

				if (battery->afc_disable) {
					battery->afc_disable = false;
					muic_hv_charger_disable(battery->afc_disable);
				}
				if (battery->wc_tx_enable) {
					pr_info("@Tx_Mode %s: Device detached.\n", __func__);

					if (is_hv_wire_type(battery->wire_status)) {
						pr_info("@Tx_Mode %s : charging voltage change(9V -> 5V).\n", __func__);
						muic_afc_set_voltage(SEC_INPUT_VOLTAGE_5V/10);
						break; /* do not set buck off/uno off untill vbus level get real 5V */			
					} else if (is_pd_wire_type(battery->wire_status) && battery->hv_pdo) {
						pr_info("@Tx_Mode %s: PD charnge pdo (9V -> 5V). Because Tx Start.\n", __func__);
						sec_bat_change_pdo(battery, SEC_INPUT_VOLTAGE_5V);
						break; /* do not set buck off/uno off untill vbus level get real 5V */		
					}

					if (!battery->buck_cntl_by_tx) {
						battery->buck_cntl_by_tx = true;
						sec_bat_set_charge(battery, battery->charger_mode);
					}

					sec_bat_wireless_iout_cntl(battery, battery->pdata->tx_uno_iout, battery->pdata->tx_mfc_iout_gear);
					sec_bat_wireless_vout_cntl(battery, WC_TX_VOUT_5_0V);
					sec_bat_wireless_minduty_cntl(battery, battery->pdata->tx_minduty_default);
				}
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_TYPE:
			battery->wc_rx_type = val->intval;
#if defined(CONFIG_BATTERY_CISD)
			if (battery->wc_rx_type) {
				if (battery->wc_rx_type == SS_BUDS) {
					battery->cisd.tx_data[SS_PHONE]--;
				}
				battery->cisd.tx_data[battery->wc_rx_type]++;
			}
#endif
			cancel_delayed_work(&battery->wpc_tx_work);
			__pm_stay_awake(battery->wpc_tx_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					&battery->wpc_tx_work, 0);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_STATUS:
			if (val->intval == WIRELESS_AUTH_START)
				sec_bat_set_misc_event(battery, BATT_MISC_EVENT_WIRELESS_AUTH_START, BATT_MISC_EVENT_WIRELESS_AUTH_START);
			else if (val->intval == WIRELESS_AUTH_RECEIVED)
				sec_bat_set_misc_event(battery, BATT_MISC_EVENT_WIRELESS_AUTH_RECVED, BATT_MISC_EVENT_WIRELESS_AUTH_RECVED);
			else if (val->intval == WIRELESS_AUTH_SENT) /* need to be clear this value when data is sent */
				sec_bat_set_misc_event(battery, 0, BATT_MISC_EVENT_WIRELESS_AUTH_START | BATT_MISC_EVENT_WIRELESS_AUTH_RECVED);
			else if (val->intval == WIRELESS_AUTH_FAIL)
				sec_bat_set_misc_event(battery, BATT_MISC_EVENT_WIRELESS_AUTH_FAIL, BATT_MISC_EVENT_WIRELESS_AUTH_FAIL);
			break;
		case POWER_SUPPLY_EXT_PROP_CALL_EVENT:
			if (val->intval == 1) {
				pr_info("%s : PHM enabled\n", __func__);
				battery->wc_rx_phm_mode = true;				
			}
			break;
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL)
		case POWER_SUPPLY_EXT_PROP_GEAR_PHM_EVENT:
			pr_info("%s : gear phm event(%d)\n", __func__, val->intval);
			if (battery->pdata->tx_gear_vout != WC_TX_VOUT_5_0V) {
				cancel_delayed_work(&battery->wpc_tx_work);
				__pm_stay_awake(battery->wpc_tx_wake_lock);
				queue_delayed_work(battery->monitor_wqueue,
							&battery->wpc_tx_work, 0);
			}
			break;
#endif
		case POWER_SUPPLY_PROP_WIRELESS_RX_POWER:
			pr_info("%s : rx power %d\n", __func__, val->intval);
			sec_bat_set_wireless20_current(battery, val->intval);
#if defined(CONFIG_CALC_TIME_TO_FULL)
			sec_bat_predict_wireless20_time_to_full_current(battery, val->intval);
#endif
			break;
		case POWER_SUPPLY_PROP_WIRELESS_MAX_VOUT:
			pr_info("%s : max vout %s\n", __func__, vout_control_mode_str[val->intval]);
			battery->wpc_max_vout_level = val->intval;
			break;
		case POWER_SUPPLY_PROP_WIRELESS_ABNORMAL_PAD:
			sec_bat_set_misc_event(battery, val->intval ? BATT_MISC_EVENT_ABNORMAL_PAD : 0,
					BATT_MISC_EVENT_ABNORMAL_PAD);
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

#if defined(CONFIG_USE_POGO)
int sec_pogo_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	val->intval = battery->pogo_status;
	pr_info("%s: POGO online : %d\n", __func__, val->intval);

	return 0;
}

int sec_pogo_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		battery->pogo_status = val->intval;

		__pm_stay_awake(battery->cable_wake_lock);
		queue_delayed_work(battery->monitor_wqueue,
			&battery->cable_work, 0);
		pr_info("%s: pogo_status : %d\n", __func__, battery->pogo_status);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
#endif

int sec_ps_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);
	union power_supply_propval value;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == 0 && battery->ps_enable == true) {
			battery->ps_enable = false;
			value.intval = val->intval;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
		} else if ((val->intval == 1) && (battery->ps_enable == false) &&
				(battery->ps_status == true)) {
			battery->ps_enable = true;
			value.intval = val->intval;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
		} else {
			dev_err(battery->dev,
				"%s: invalid setting (%d)\n", __func__, val->intval);
		}
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		if (val->intval == SEC_BATTERY_CABLE_POWER_SHARING) {
			battery->ps_status = true;
			battery->ps_enable = true;
			value.intval = battery->ps_enable;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
		} else {
			battery->ps_status = false;
			battery->ps_enable = false;
			value.intval = battery->ps_enable;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int sec_ps_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct sec_battery_info *battery = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = (battery->ps_enable) ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (battery->ps_status) ? 1 : 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
