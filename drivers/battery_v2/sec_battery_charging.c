/*
 *  sec_battery_charging.c
 *  Samsung Mobile Battery Driver - charging current/voltage decision
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"

/*
 * pdata->charging_current[] holds exactly SEC_BATTERY_CABLE_MAX entries
 * (sec_battery_dt.c) but battery->cable_type is fed from notifier payloads
 * and is not otherwise range checked, so it can point past the array. Map an
 * out-of-range cable type to the SEC_BATTERY_CABLE_UNKNOWN entry; an in-range
 * cable type is used unchanged.
 */
static int sec_bat_charging_current_index(int cable_type)
{
	if (cable_type < 0 || cable_type >= SEC_BATTERY_CABLE_MAX)
		return SEC_BATTERY_CABLE_UNKNOWN;

	return cable_type;
}

static struct sec_charging_current *
sec_bat_charging_current(struct sec_battery_info *battery)
{
	return &battery->pdata->charging_current
			[sec_bat_charging_current_index(battery->cable_type)];
}

/* Input current advertised by the PD PDO at index. */
static unsigned int sec_bat_pd_pdo_current(struct sec_battery_info *battery, int index)
{
#if defined(CONFIG_PDIC_PD30)
	return battery->pd_list.pd_info[index].max_current;
#else
	return battery->pd_list.pd_info[index].input_current;
#endif
}

void sec_bat_change_default_current(struct sec_battery_info *battery,
					int cable_type, int input, int output)
{
	int index = sec_bat_charging_current_index(cable_type);

#if defined(CONFIG_ENG_BATTERY_CONCEPT)
	if (!battery->test_max_current)
#endif
		battery->pdata->charging_current[index].input_current_limit = input;
#if defined(CONFIG_ENG_BATTERY_CONCEPT)
	if (!battery->test_charge_current)
#endif
		battery->pdata->charging_current[index].fast_charging_current = output;
	pr_info("%s: cable_type: %d input: %d output: %d\n",
		__func__,
		cable_type,
		battery->pdata->charging_current[index].input_current_limit,
		battery->pdata->charging_current[index].fast_charging_current);
}

int sec_bat_get_wireless_current(struct sec_battery_info *battery, int incurr)
{
	union power_supply_propval value = {0, };
	int is_otg_on = 0;

	psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
	is_otg_on = value.intval;

	if (is_otg_on) {
		pr_info("%s: both wireless chg and otg recognized.\n", __func__);
		incurr = battery->pdata->wireless_otg_input_current;
	}

#if defined(CONFIG_ISDB_CHARGING_CONTROL)
	if (is_hv_wireless_type(battery->cable_type) && (battery->current_event & SEC_BAT_CURRENT_EVENT_ISDB)) {
		incurr = battery->pdata->charging_current[SEC_BATTERY_CABLE_WIRELESS].input_current_limit;
		pr_info("%s ISDB in_curr = %d \n", __func__, incurr);
	}
#endif

	/* 2. WPC_SLEEP_MODE */
	if (is_hv_wireless_type(battery->cable_type) && sleep_mode) {
		if (incurr > battery->pdata->sleep_mode_limit_current)
			incurr = battery->pdata->sleep_mode_limit_current;
		pr_info("%s sleep_mode =%d, chg_limit =%d, in_curr = %d \n", __func__,
			sleep_mode, battery->chg_limit, incurr);

		if (!battery->auto_mode) {
			/* send cmd once */
			battery->auto_mode = true;
			value.intval = WIRELESS_SLEEP_MODE_ENABLE;
			psy_do_property(battery->pdata->wireless_charger_name, set,
							POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
		}
	}

	/* 3. WPC_TEMP_MODE */
	if (is_wireless_type(battery->cable_type) && battery->chg_limit) {
		if ((battery->siop_level >= 100 && !battery->lcd_status) &&
				(incurr > battery->pdata->wpc_input_limit_current)) {
			if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_TX &&
				battery->pdata->wpc_input_limit_by_tx_check)
				incurr = battery->pdata->wpc_input_limit_current_by_tx;
			else
				incurr = battery->pdata->wpc_input_limit_current;
		} else if ((battery->siop_level < 100 || battery->lcd_status) &&
				(incurr > battery->pdata->wpc_lcd_on_input_limit_current))
			incurr = battery->pdata->wpc_lcd_on_input_limit_current;
	}

	/* 5. Full-Additional state */
	if (battery->status == POWER_SUPPLY_STATUS_FULL && battery->charging_mode == SEC_BATTERY_CHARGING_2ND) {
		if (incurr > battery->pdata->siop_hv_wireless_input_limit_current)
			incurr = battery->pdata->siop_hv_wireless_input_limit_current;
	}

	/* 6. Hero Stand Pad CV */
	if (battery->capacity >= battery->pdata->wc_hero_stand_cc_cv) {
		if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_STAND) {
			if (incurr > battery->pdata->wc_hero_stand_cv_current)
				incurr = battery->pdata->wc_hero_stand_cv_current;
		} else if (battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_STAND) {
			if (battery->chg_limit &&
					incurr > battery->pdata->wc_hero_stand_cv_current) {
				incurr = battery->pdata->wc_hero_stand_cv_current;
			} else if (!battery->chg_limit &&
					incurr > battery->pdata->wc_hero_stand_hv_cv_current) {
				incurr = battery->pdata->wc_hero_stand_hv_cv_current;
			}
		}
	}

	/* 7. Full-None state && SIOP_LEVEL 100 */
	if ((battery->siop_level >= 100 && !battery->lcd_status) &&
		battery->status == POWER_SUPPLY_STATUS_FULL && battery->charging_mode == SEC_BATTERY_CHARGING_NONE) {
		incurr = battery->pdata->wc_full_input_limit_current;
	}

	return incurr;
}

void sec_bat_get_charging_current_by_siop(struct sec_battery_info *battery,
		int *input_current, int *charging_current)
{

	if (battery->siop_level < 100  &&
		!((battery->siop_level == 80) && is_wired_type(battery->cable_type))) {
		int max_charging_current;

		if (is_wireless_type(battery->cable_type)) {
			max_charging_current = 1000; /* 1 step(70) */
			if (battery->siop_level == 0) { /* 3 step(0) */
				max_charging_current = 0;
			} else if (battery->siop_level <= 10) { /* 2 step(10) */
				max_charging_current = 500;
			}
		}
#if defined(CONFIG_DIRECT_CHARGING)
		else if (is_pd_apdo_wire_type(battery->cable_type))
			max_charging_current = battery->pdata->siop_apdo_charging_limit_current;
#endif
		else
			max_charging_current = 1800; /* 1 step(70) */

		/* do forced set charging current */
		if (*charging_current > max_charging_current)
			*charging_current = max_charging_current;

		if (is_nv_wireless_type(battery->cable_type)) {
			if (*input_current > battery->pdata->siop_wireless_input_limit_current)
				*input_current = battery->pdata->siop_wireless_input_limit_current;
			if (*charging_current > battery->pdata->siop_wireless_charging_limit_current)
				*charging_current = battery->pdata->siop_wireless_charging_limit_current;
		} else if (is_hv_wireless_type(battery->cable_type)) {
			if (*input_current > battery->pdata->siop_hv_wireless_input_limit_current)
				*input_current = battery->pdata->siop_hv_wireless_input_limit_current;
			if (*charging_current > battery->pdata->siop_hv_wireless_charging_limit_current)
				*charging_current = battery->pdata->siop_hv_wireless_charging_limit_current;
		} else if (is_hv_wire_type(battery->cable_type) && is_hv_wire_type(battery->wire_status)) {
			if (is_hv_wire_12v_type(battery->cable_type)) {
				if (*input_current > battery->pdata->siop_hv_12v_input_limit_current)
					*input_current = battery->pdata->siop_hv_12v_input_limit_current;
			} else {
				if (*input_current > battery->pdata->siop_hv_input_limit_current)
					*input_current = battery->pdata->siop_hv_input_limit_current;
				/* 2 step(0) for hv_wire_type */
				if (battery->siop_level == 0) {
					if (battery->store_mode && (*input_current > battery->pdata->siop_store_hv_input_limit_current_2nd))
						*input_current = battery->pdata->siop_store_hv_input_limit_current_2nd;
					else if (*input_current > battery->pdata->siop_hv_input_limit_current_2nd)
						*input_current = battery->pdata->siop_hv_input_limit_current_2nd;
				} else if (battery->siop_level == 20 && battery->pdata->input_current_by_siop_20 > 0) {
					if (*input_current > battery->pdata->input_current_by_siop_20)
						*input_current = battery->pdata->input_current_by_siop_20;
				}
			}
#if defined(CONFIG_CCIC_NOTIFIER)
		} else if (is_pd_wire_type(battery->cable_type)) {
			if (battery->input_voltage &&
				*input_current > (60000 / battery->input_voltage))
				*input_current = 60000 / battery->input_voltage;
			else if (!battery->input_voltage && *input_current > 0u)
				/* arm64: 60000/0 == 0, so the original clamp stored 0 */
				*input_current = 0;
			/* 2 step(0) for PD type */
			if (battery->siop_level == 0 &&
				*input_current > battery->pdata->siop_hv_input_limit_current_2nd)
				*input_current = battery->pdata->siop_hv_input_limit_current_2nd;
#endif
		} else {
			if (*input_current > battery->pdata->siop_input_limit_current)
				*input_current = battery->pdata->siop_input_limit_current;
		}
	}

	pr_info("%s: incurr(%d), chgcurr(%d)\n", __func__, *input_current, *charging_current);
}

void sec_bat_change_pdo(struct sec_battery_info *battery, int vol)
{
	int target_pd_index = 0;

	if (is_pd_wire_type(battery->wire_status)) {

		if (vol == SEC_INPUT_VOLTAGE_9V) {
			/* select PDO greater than 5V */
#if defined(CONFIG_PDIC_PD30)
			target_pd_index = battery->pd_list.num_fpdo - 1;
#else
			target_pd_index = battery->pd_list.max_pd_count - 1;
#endif
		} else {
			/* select 5V PDO */
			target_pd_index = 0;
		}

		if (target_pd_index < 0 || target_pd_index >= MAX_PDO_NUM) {
			pr_info("%s: target_pd_index is wrong: %d\n", __func__, target_pd_index);
			return;
		}

		pr_info("%s: target_pd_index: %d, now_pd_index: %d\n", __func__,
			target_pd_index, battery->pd_list.now_pd_index);

		if (target_pd_index != battery->pd_list.now_pd_index) {
			/* change input current before request new pdo if new pdo's input current is less than now */
			if (sec_bat_pd_pdo_current(battery, target_pd_index) <
					battery->input_current) {
				union power_supply_propval value = {0, };

				value.intval = sec_bat_pd_pdo_current(battery, target_pd_index);
				battery->input_current = value.intval;
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_SELECT_PDO,
					SEC_BAT_CURRENT_EVENT_SELECT_PDO);
				psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CURRENT_MAX, value);
			}
			battery->pdic_ps_rdy = false;
			select_pdo(battery->pd_list.pd_info[target_pd_index].pdo_index);
		}
	}
}
#if !defined(CONFIG_SEC_FACTORY)
int get_chg_power_type(int ct, int ws, int pd_max_pw, int max_pw)
{
	bool is_pd = is_pd_wire_type(ct);

	if (is_wireless_type(ct))
		return NORMAL_TA;

	if (is_pd && pd_max_pw >= HV_CHARGER_STATUS_STANDARD4)
		return SFC_45W;
	if (is_pd && pd_max_pw >= HV_CHARGER_STATUS_STANDARD3)
		return SFC_25W;
	if (is_hv_wire_12v_type(ct) ||
		max_pw >= HV_CHARGER_STATUS_STANDARD2) /* 20000mW */
		return AFC_12V_OR_20W;
	if (is_hv_wire_type(ct) ||
		(is_pd && pd_max_pw >= HV_CHARGER_STATUS_STANDARD1) ||
		ws == SEC_BATTERY_CABLE_PREPARE_TA ||
		max_pw >= HV_CHARGER_STATUS_STANDARD1) /* 12000mW */
		return AFC_9V_OR_15W;

	return NORMAL_TA;
}

int sec_bat_check_power_type(
	int max_chg_pwr, int pd_max_chg_pwr, int ct, int ws, int is_apdo)
{
	int pt;

	if (!is_pd_wire_type(ct) || !is_apdo)
		return NORMAL_TA;

	pt = get_chg_power_type(ct, ws, pd_max_chg_pwr, max_chg_pwr);
	if (pt == SFC_45W || pt == SFC_25W)
		return pt;

	return NORMAL_TA;
}
#if defined(CONFIG_DIRECT_CHARGING)
static int sec_bat_check_lpm_power(int lpm, int pt)
{
	int ret = 0;

	if (pt == SFC_25W)
		ret |= 0x02;

	if (lpm)
		ret |= 0x01;

	return ret;
}
#endif
int sec_bat_check_lrp_temp_cond(int prev_step,
	int temp, int trig, int recov)
{
	if (trig <= temp)
		prev_step++;
	else if (recov >= temp)
		prev_step--;

	if (prev_step < LRP_NONE)
		prev_step = LRP_NONE;
	else if (prev_step > LRP_STEP2)
		prev_step = LRP_STEP2;

	return prev_step;
}

int sec_bat_check_lrp_step(
	struct sec_battery_info *battery, int temp, int pt, bool lcd_sts)
{
	int step = LRP_NONE;
	int lcd_st = LCD_OFF;
	int lrp_pt = LRP_NORMAL;
	int lrp_high_temp_st1;
	int lrp_high_temp_st2;
	int lrp_high_temp_recov_st1;
	int lrp_high_temp_recov_st2;

	if (lcd_sts)
		lcd_st = LCD_ON;

	if (pt == SFC_45W)
		lrp_pt = LRP_45W;
	else if (pt == SFC_25W)
		lrp_pt = LRP_25W;

	lrp_high_temp_st1 = battery->pdata->lrp_temp[lrp_pt].trig[ST1][lcd_st];
	lrp_high_temp_st2 = battery->pdata->lrp_temp[lrp_pt].trig[ST2][lcd_st];
	lrp_high_temp_recov_st1 = battery->pdata->lrp_temp[lrp_pt].recov[ST1][lcd_st];
	lrp_high_temp_recov_st2 = battery->pdata->lrp_temp[lrp_pt].recov[ST2][lcd_st];

	pr_info("%s: st1(%d), st2(%d), recv_st1(%d), recv_st2(%d), lrp(%d)\n", __func__,
		lrp_high_temp_st1, lrp_high_temp_st2,
		lrp_high_temp_recov_st1, lrp_high_temp_recov_st2, temp);

	switch (battery->lrp_step) {
	case LRP_STEP2:
		step = sec_bat_check_lrp_temp_cond(battery->lrp_step,
				temp, 900, lrp_high_temp_recov_st2);
		break;
	case LRP_STEP1:
		step = sec_bat_check_lrp_temp_cond(battery->lrp_step,
				temp, lrp_high_temp_st2, lrp_high_temp_recov_st1);
		break;
	case LRP_NONE:
		step = sec_bat_check_lrp_temp_cond(battery->lrp_step,
				temp, lrp_high_temp_st1, -200);
		break;
	default:
		break;
	}

	if ((battery->lrp_step != LRP_STEP1) && (step == LRP_STEP1))
		step = sec_bat_check_lrp_temp_cond(step,
				temp, lrp_high_temp_st2, lrp_high_temp_recov_st1);

	return step;
}

int sec_bat_get_lrp_step(struct sec_battery_info *battery)
{
	bool is_apdo = false;
	int power_type = NORMAL_TA;
	int ct = battery->cable_type, ws = battery->wire_status;

#if IS_ENABLED(CONFIG_DIRECT_CHARGING)
	is_apdo = (is_pd_apdo_wire_type(ct) && battery->pd_list.now_isApdo) ? 1 : 0;
#endif
	power_type = sec_bat_check_power_type(battery->max_charge_power,
				battery->pd_max_charge_power, ct, ws, is_apdo);

	return sec_bat_check_lrp_step(battery, battery->lrp, power_type, battery->lcd_status);
}

void sec_bat_check_lrp_temp(
	struct sec_battery_info *battery, int ct, int ws, int siop_level, bool lcd_sts, int *input_current, int *charging_current)
{
	int lrp_step = LRP_NONE;
	bool is_apdo = false;
	int power_type = NORMAL_TA;
	bool force_check = false;
	int ret = 0;
	int max_charging_current = 1800;

	if (battery->pdata->lrp_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return;

#if IS_ENABLED(CONFIG_DIRECT_CHARGING)
	is_apdo = (is_pd_apdo_wire_type(ct) && battery->pd_list.now_isApdo) ? 1 : 0;
#endif
	power_type = sec_bat_check_power_type(battery->max_charge_power,
				battery->pd_max_charge_power, ct, ws, is_apdo);

	lrp_step = sec_bat_check_lrp_step(battery, battery->lrp, power_type, lcd_sts);

	/* 15w afc or pd ta */
	if (power_type == NORMAL_TA) {
		ret = battery->input_voltage;
		if (((ret == SEC_INPUT_VOLTAGE_5V) && !lcd_sts) ||
			((ret != SEC_INPUT_VOLTAGE_5V) && lcd_sts))
			force_check = true;
		pr_info("%s: force_check(%d), ret(%d), lcd(%d)\n", __func__,
			(force_check ? 1 : 0), ret, (lcd_sts ? 1 : 0));
	}

	if ((lrp_step == LRP_STEP2) || (lrp_step == LRP_STEP1)) {
		if (*charging_current > max_charging_current)
			*charging_current = max_charging_current;

		if (is_pd_wire_type(ct)) {
			if (power_type == SFC_45W) {
				*input_current = battery->pdata->lrp_curr[LRP_45W].st_icl[lrp_step - 1];
				*charging_current = battery->pdata->lrp_curr[LRP_45W].st_fcc[lrp_step - 1];
			} else if (power_type == SFC_25W) {
				*input_current = battery->pdata->lrp_curr[LRP_25W].st_icl[lrp_step - 1];
				*charging_current = battery->pdata->lrp_curr[LRP_25W].st_fcc[lrp_step - 1];
			} else {
				if (lcd_sts) {
					if (battery->input_voltage &&
						*input_current > (60000 / battery->input_voltage))
						*input_current = 60000 / battery->input_voltage;
					else if (!battery->input_voltage && *input_current > 0u)
						/* arm64: 60000/0 == 0, so the original clamp stored 0 */
						*input_current = 0;
				} else {
					if (*input_current > battery->pdata->chg_input_limit_current)
						*input_current = battery->pdata->chg_input_limit_current;
					if (*charging_current > battery->pdata->chg_charging_limit_current)
						*charging_current = battery->pdata->chg_charging_limit_current;
				}
			}
		} else if (is_hv_wire_type(ct)) {
			if (is_hv_wire_12v_type(battery->cable_type)) {
				if (lcd_sts) {
					if (*input_current > battery->pdata->siop_hv_12v_input_limit_current)
						*input_current = battery->pdata->siop_hv_12v_input_limit_current;
				} else {
					if (*input_current > battery->pdata->chg_input_limit_current)
						*input_current = battery->pdata->chg_input_limit_current;
					if (*charging_current > battery->pdata->chg_charging_limit_current)
						*charging_current = battery->pdata->chg_charging_limit_current;
				}
			} else {
				if (lcd_sts) {
					if (*input_current > battery->pdata->siop_hv_input_limit_current)
						*input_current = battery->pdata->siop_hv_input_limit_current;
				} else {
					if (*input_current > battery->pdata->chg_input_limit_current)
						*input_current = battery->pdata->chg_input_limit_current;
					if (*charging_current > battery->pdata->chg_charging_limit_current)
						*charging_current = battery->pdata->chg_charging_limit_current;
				}
			}
		} else {
			if (*input_current > battery->pdata->siop_input_limit_current)
				*input_current = battery->pdata->siop_input_limit_current;
		}
		if ((battery->lrp_step != lrp_step) || force_check)
			pr_info(
				"%s:LRP:%d%%,%dmV,lrp_step(%d),lcd(%d),tlrp(%d),icl(%d),fcc(%d),ct(%d),is_apdo(%d),mcp(%d,%d)",
					__func__, battery->capacity, battery->voltage_now, lrp_step, lcd_sts,
					battery->lrp, *input_current, *charging_current, battery->cable_type,
					is_apdo, battery->pd_max_charge_power, battery->max_charge_power);
		battery->lrp_limit = true;
	} else if ((battery->lrp_limit == true) && (lrp_step == LRP_NONE)) {
		battery->lrp_limit = false;
		pr_info(
			"%s:LRP:SOC(%d),Vnow(%d),lrp_lim(%d),tlrp(%d),ct(%d)",
				__func__, battery->capacity, battery->voltage_now, battery->lrp_limit,
				battery->lrp, battery->cable_type);
	}
	battery->lrp_step = lrp_step;

	pr_info("%s: cable_type(%d), lrp_step(%d), lrp(%d)\n", __func__,
		ct, battery->lrp_step, battery->lrp);
}

#if defined(CONFIG_DIRECT_CHARGING)
void sec_bat_set_dchg_current(struct sec_battery_info *battery, int power_type, int is_apdo, int pt, int *input_current, int *charging_current)
{
	int temp_ic = *input_current, temp_cc = *charging_current;

	/* skip power_type check for non-use lrp_temp_check models */
	if (battery->pdata->lrp_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		power_type = NORMAL_TA;

	if (power_type == SFC_45W) {
		if (pt & 0x01) {
			*input_current = battery->pdata->lrp_curr[LRP_45W].st_icl[ST1];
			*charging_current = battery->pdata->lrp_curr[LRP_45W].st_fcc[ST1];
		} else {
			*input_current = battery->pdata->lrp_curr[LRP_45W].st_icl[ST2];
			*charging_current = battery->pdata->lrp_curr[LRP_45W].st_fcc[ST2];
		}
	} else if (power_type == SFC_25W) {
		if (pt & 0x01) {
			*input_current = battery->pdata->lrp_curr[LRP_25W].st_icl[ST1];
			*charging_current = battery->pdata->lrp_curr[LRP_25W].st_fcc[ST1];
		} else {
			*input_current = battery->pdata->lrp_curr[LRP_25W].st_icl[ST2];
			*charging_current = battery->pdata->lrp_curr[LRP_25W].st_fcc[ST2];
		}
	} else {
		if (battery->input_voltage == SEC_INPUT_VOLTAGE_5V) {
			*input_current = battery->pdata->default_input_current;
			*charging_current = battery->pdata->default_charging_current;
		} else {
			if (is_apdo) {
				*input_current = battery->pdata->dchg_input_limit_current;
				*charging_current = battery->pdata->dchg_charging_limit_current;
			} else {
				*input_current = battery->pdata->chg_input_limit_current;
				*charging_current = battery->pdata->chg_charging_limit_current;
			}
		}
	}
	if (temp_ic < *input_current || (power_type >= SFC_25W && temp_cc < *charging_current)) {
		pr_info("%s: do not set new icl(%d) cc(%d) because of old icl(%d) cc(%d)\n",
			__func__, *input_current, *charging_current, temp_ic, temp_cc);
		*input_current = temp_ic;
		*charging_current = temp_cc;
	}
}
#endif

static bool sec_bat_change_vbus(struct sec_battery_info *battery, int *input_current)
{
	if (battery->pdata->chg_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return false;

#if defined(CONFIG_SUPPORT_HV_CTRL)
	union power_supply_propval value;
	unsigned int target_vbus = SEC_INPUT_VOLTAGE_0V;
	int lrp_step = LRP_NONE;

	if (battery->store_mode || !battery->charging_enabled ||
		((battery->siop_level == 80) && is_wired_type(battery->cable_type))) {
		pr_info("%s : store_mode(%d) charging_enabled(%d) siop(%d) ct(%d)\n",
			__func__, battery->store_mode, battery->charging_enabled, battery->siop_level, battery->cable_type);
		return false;
	}

	if (is_hv_wire_type(battery->cable_type) &&
		(battery->cable_type != SEC_BATTERY_CABLE_QC30)) {

		if (battery->current_event & SEC_BAT_CURRENT_EVENT_AFC) {
			pr_info("%s: skip during current_event(0x%x)\n",
				__func__, battery->current_event);
			return false;
		}

		lrp_step = sec_bat_get_lrp_step(battery);
		pr_info("%s: lrp_step: %d\n", __func__, lrp_step);

		/* check target vbus */
		if (battery->vbus_limit)
			target_vbus = SEC_INPUT_VOLTAGE_0V;
		else if (battery->vbus_chg_by_full)
			target_vbus = SEC_INPUT_VOLTAGE_5V;
		else if (battery->siop_level >= 100 && lrp_step == LRP_NONE) {
			if (is_hv_wire_12v_type(battery->cable_type))
				target_vbus = SEC_INPUT_VOLTAGE_12V;
			else
				target_vbus = SEC_INPUT_VOLTAGE_9V;

			if (battery->vbus_chg_by_siop == SEC_INPUT_VOLTAGE_NONE)
				battery->vbus_chg_by_siop = target_vbus;

		} else if (battery->status == POWER_SUPPLY_STATUS_CHARGING)
			target_vbus = SEC_INPUT_VOLTAGE_5V;

		if (target_vbus == SEC_INPUT_VOLTAGE_0V) {
			pr_info("%s: skip set vbus %dV, level(%d), Cable(%s, %s, %d, %d)\n",
				__func__, target_vbus, battery->siop_level,
				sec_cable_type[battery->cable_type], sec_cable_type[battery->wire_status],
				battery->muic_cable_type, battery->pd_usb_attached);

			return false;
		}

		if (battery->vbus_chg_by_siop != target_vbus) {
			/* change input current to pre_afc_input_current */
			*input_current = battery->pdata->pre_afc_input_current;
			battery->charge_power = (battery->input_voltage * (*input_current)) / 10;
			value.intval = *input_current;
			psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CURRENT_MAX, value);
			battery->input_current = *input_current;

			/* set current event */
			cancel_delayed_work(&battery->afc_work);
			__pm_relax(battery->afc_wake_lock);
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_AFC,
						  (SEC_BAT_CURRENT_EVENT_CHG_LIMIT | SEC_BAT_CURRENT_EVENT_AFC));

			battery->chg_limit = false;
			battery->vbus_chg_by_siop = target_vbus;
			muic_afc_set_voltage(target_vbus/10);

			pr_info("%s: vbus set %dV by level(%d), Cable(%s, %s, %d, %d)\n",
				__func__, target_vbus, battery->siop_level,
				sec_cable_type[battery->cable_type], sec_cable_type[battery->wire_status],
				battery->muic_cable_type, battery->pd_usb_attached);

			return true;
		}
	}
#endif
	return false;
}

#if defined(CONFIG_DIRECT_CHARGING)
static void sec_bat_check_direct_chg_temp(struct sec_battery_info *battery, int *input_current, int *charging_current)
{
	int pt = 0;
	int ct = battery->cable_type, ws = battery->wire_status;
	bool is_apdo = false;
	int power_type = NORMAL_TA;

	if (battery->pdata->dchg_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return;

	is_apdo = (is_pd_apdo_wire_type(ct) && battery->pd_list.now_isApdo) ? 1 : 0;
	power_type = sec_bat_check_power_type(battery->max_charge_power,
				battery->pd_max_charge_power, ct, ws, is_apdo);

	pt = sec_bat_check_lpm_power(lpcharge, power_type);

	if (battery->siop_level >= 100) {
		if (!battery->chg_limit && is_apdo &&
			((battery->dchg_temp >= battery->pdata->dchg_high_temp[pt]) ||
			(battery->temperature >= battery->pdata->dchg_high_batt_temp[pt]))) {
			sec_bat_set_dchg_current(battery, power_type, pt, is_apdo, input_current, charging_current);
			battery->chg_limit = true;
		} else if (!battery->chg_limit && (!is_apdo) &&
			(battery->chg_temp >= battery->pdata->chg_high_temp)) {
			sec_bat_set_dchg_current(battery, power_type, pt, is_apdo, input_current, charging_current);
			battery->chg_limit = true;
		} else if (battery->chg_limit) {
			struct sec_charging_current *cc = sec_bat_charging_current(battery);

			if (((battery->dchg_temp <= battery->pdata->dchg_high_temp_recovery[pt]) &&
				(battery->temperature <= battery->pdata->dchg_high_batt_temp_recovery[pt]) &&
				is_apdo) || ((battery->chg_temp <= battery->pdata->chg_high_temp_recovery) &&
				(!is_apdo))) {
				*input_current = cc->input_current_limit;
				*charging_current = cc->fast_charging_current;
				battery->chg_limit = false;
			} else {
				sec_bat_set_dchg_current(battery, power_type, pt, is_apdo, input_current, charging_current);
				battery->chg_limit = true;
			}
		}
		pr_info("%s: cable_type(%d), chg_limit(%d) vbus_by_siop(%d) ic(%d) cc(%d)\n", __func__,
			battery->cable_type, battery->chg_limit, battery->vbus_chg_by_siop, *input_current, *charging_current);
	}
}
#endif

static void sec_bat_check_afc_temp(struct sec_battery_info *battery, int *input_current, int *charging_current)
{
	struct sec_charging_current *cc = sec_bat_charging_current(battery);

	if (battery->pdata->chg_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return;

#if defined(CONFIG_SUPPORT_HV_CTRL)
	if (battery->siop_level >= 100) {
		if (!battery->chg_limit && is_hv_wire_type(battery->cable_type) && (battery->chg_temp >= battery->pdata->chg_high_temp)) {
			*input_current = battery->pdata->chg_input_limit_current;
			*charging_current = battery->pdata->chg_charging_limit_current;
			battery->chg_limit = true;
		} else if (!battery->chg_limit && battery->max_charge_power >= (battery->pdata->pd_charging_charge_power - 500) && (battery->chg_temp >= battery->pdata->chg_high_temp)) {
			*input_current = battery->pdata->default_input_current;
			*charging_current = battery->pdata->default_charging_current;
			battery->chg_limit = true;
		} else if (battery->chg_limit && is_hv_wire_type(battery->cable_type)) {
			if (battery->chg_temp <= battery->pdata->chg_high_temp_recovery) {
				*input_current = cc->input_current_limit;
				*charging_current = cc->fast_charging_current;
				battery->chg_limit = false;
			} else {
				*input_current = battery->pdata->chg_input_limit_current;
				*charging_current = battery->pdata->chg_charging_limit_current;
				battery->chg_limit = true;
			}
		} else if (battery->chg_limit && battery->max_charge_power >= (battery->pdata->pd_charging_charge_power - 500)) {
			if (battery->chg_temp <= battery->pdata->chg_high_temp_recovery) {
				*input_current = cc->input_current_limit;
				*charging_current = cc->fast_charging_current;
				battery->chg_limit = false;
			} else {
				*input_current = battery->pdata->chg_input_limit_current;
				*charging_current = battery->pdata->chg_charging_limit_current;
				battery->chg_limit = true;
			}
		}
		pr_info("%s: cable_type(%d), chg_limit(%d) vbus_by_siop(%d)\n", __func__,
			battery->cable_type, battery->chg_limit, battery->vbus_chg_by_siop);
	}
#else
	if ((!battery->chg_limit && is_hv_wire_type(battery->cable_type) && (battery->chg_temp >= battery->pdata->chg_high_temp)) ||
		(battery->chg_limit && is_hv_wire_type(battery->cable_type) && (battery->chg_temp >= battery->pdata->chg_high_temp_recovery))) {
		*input_current = battery->pdata->chg_input_limit_current;
		*charging_current = battery->pdata->chg_charging_limit_current;
		battery->chg_limit = true;
	} else if (battery->chg_limit && is_hv_wire_type(battery->cable_type) && (battery->chg_temp <= battery->pdata->chg_high_temp_recovery)) {
		*input_current = cc->input_current_limit;
		*charging_current = cc->fast_charging_current;
		battery->chg_limit = false;
	}
#endif
}

#if defined(CONFIG_CCIC_NOTIFIER)
extern void select_pdo(int num);
static bool sec_bat_change_vbus_pd(struct sec_battery_info *battery, int *input_current)
{
#if defined(CONFIG_SUPPORT_HV_CTRL)
	int target_pd_index = 0;
	int lrp_step = LRP_NONE;

	if (battery->pdata->chg_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return false;

	if (battery->store_mode || !battery->charging_enabled ||
		((battery->siop_level == 80) && is_wired_type(battery->cable_type))) {
		pr_info("%s : store_mode(%d) charging_enabled(%d) siop(%d) ct(%d)\n",
			__func__, battery->store_mode, battery->charging_enabled, battery->siop_level, battery->cable_type);
		return false;
	}

	if (battery->cable_type == SEC_BATTERY_CABLE_PDIC) {
		if (battery->current_event & SEC_BAT_CURRENT_EVENT_SELECT_PDO) {
			pr_info("%s: skip during current_event(0x%x)\n",
				__func__, battery->current_event);
			return false;
		}

		lrp_step = sec_bat_get_lrp_step(battery);
		pr_info("%s: lrp_step: %d siop(%d)\n", __func__, lrp_step, battery->siop_level);

		if (battery->siop_level >= 100 && lrp_step == LRP_NONE) {
			/* select PDO greater than 5V */
			target_pd_index = battery->pd_list.max_pd_count - 1;
		} else {
			/* select 5V PDO */
			target_pd_index = 0;
		}

		if (target_pd_index < 0 || target_pd_index >= MAX_PDO_NUM) {
			pr_info("%s: target_pd_index is wrong: %d\n", __func__, target_pd_index);
			return false;
		}

		pr_info("%s: target_pd_index: %d, now_pd_index: %d\n", __func__,
			target_pd_index, battery->pd_list.now_pd_index);

		if (target_pd_index != battery->pd_list.now_pd_index) {
			/* change input current before request new pdo if new pdo's input current is less than now */
			if (sec_bat_pd_pdo_current(battery, target_pd_index) <
					battery->input_current) {
				union power_supply_propval value = {0, };

				*input_current = sec_bat_pd_pdo_current(battery, target_pd_index);

				value.intval = *input_current;
				battery->input_current = *input_current;
				sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_SELECT_PDO,
					SEC_BAT_CURRENT_EVENT_SELECT_PDO);
				psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CURRENT_MAX, value);
			}
			battery->pdic_ps_rdy = false;
			sec_bat_set_current_event(battery, SEC_BAT_CURRENT_EVENT_SELECT_PDO,
				SEC_BAT_CURRENT_EVENT_SELECT_PDO);
			select_pdo(battery->pd_list.pd_info[target_pd_index].pdo_index);
			return true;
		}
	}
#endif
	return false;
}

static void sec_bat_check_pdic_temp(struct sec_battery_info *battery, int *input_current, int *charging_current)
{
	if (battery->pdata->chg_temp_check_type == SEC_BATTERY_TEMP_CHECK_NONE)
		return;

	if (battery->pdic_ps_rdy && battery->siop_level >= 100 && !battery->lcd_status) {
		struct sec_charging_current *cc = sec_bat_charging_current(battery);

		if ((!battery->chg_limit && (battery->chg_temp >= battery->pdata->chg_high_temp)) ||
			(battery->chg_limit && (battery->chg_temp >= battery->pdata->chg_high_temp_recovery))) {
			if (battery->input_voltage) {
				*input_current = (battery->pdata->chg_input_limit_current *
					SEC_INPUT_VOLTAGE_9V) / battery->input_voltage;
			} else {
				/* arm64: div-by-zero yields 0, so the original stored 0 */
				*input_current = 0;
			}
			*charging_current = battery->pdata->chg_charging_limit_current;
			battery->chg_limit = true;
		} else if (battery->chg_limit && battery->chg_temp <= battery->pdata->chg_high_temp_recovery) {
			*input_current = cc->input_current_limit;
			*charging_current = cc->fast_charging_current;
			battery->chg_limit = false;
		}
		pr_info("%s: cable_type(%d), chg_limit(%d)\n", __func__,
			battery->cable_type, battery->chg_limit);
	}
}

static int sec_bat_check_pd_input_current(struct sec_battery_info *battery, int input_current)
{
	if (battery->current_event & SEC_BAT_CURRENT_EVENT_SELECT_PDO) {
		input_current = SELECT_PDO_INPUT_CURRENT;
		pr_info("%s: change input_current(%d), cable_type(%d)\n", __func__, input_current, battery->cable_type);
	}

	return input_current;
}
#endif
#endif
static int sec_bat_check_afc_input_current(struct sec_battery_info *battery, int input_current)
{
	if (battery->current_event & SEC_BAT_CURRENT_EVENT_AFC) {
		int work_delay = 0;

		if (!is_wireless_type(battery->cable_type)) {
			input_current = battery->pdata->pre_afc_input_current; // 1000mA
			work_delay = battery->pdata->pre_afc_work_delay;
		} else {
			input_current = battery->pdata->pre_wc_afc_input_current;
			/* do not reduce this time, this is for noble pad */
			work_delay = battery->pdata->pre_wc_afc_work_delay;
		}

		__pm_stay_awake(battery->afc_wake_lock);
		if (!delayed_work_pending(&battery->afc_work))
			queue_delayed_work(battery->monitor_wqueue,
				&battery->afc_work, msecs_to_jiffies(work_delay));

		pr_info("%s: change input_current(%d), cable_type(%d)\n", __func__, input_current, battery->cable_type);
	}

	return input_current;
}
#if defined(CONFIG_CCIC_NOTIFIER)
void sec_bat_get_input_current_in_power_list(struct sec_battery_info *battery)
{
	int pdo_num = battery->pdic_info.sink_status.current_pdo_num;
	int max_input_current = 0;

#if defined(CONFIG_PDIC_PD30)
	if (is_pd_apdo_wire_type(battery->wire_status) && battery->pd_list.now_isApdo)
		pdo_num = 1;
#endif

	max_input_current = battery->pdata->charging_current[SEC_BATTERY_CABLE_PDIC].input_current_limit =
		battery->pdic_info.sink_status.power_list[pdo_num].max_current;
#if defined(CONFIG_PDIC_PD30)
	battery->pdata->charging_current[SEC_BATTERY_CABLE_PDIC_APDO].input_current_limit =
		battery->pdic_info.sink_status.power_list[pdo_num].max_current;
#endif

	pr_info("%s:max_input_current : %dmA\n", __func__, max_input_current);
}

void sec_bat_get_charging_current_in_power_list(struct sec_battery_info *battery)
{
	int max_charging_current = 0, pd_power = 0;
	int pdo_num = battery->pdic_info.sink_status.current_pdo_num;

#if defined(CONFIG_PDIC_PD30)
	if (is_pd_apdo_wire_type(battery->wire_status) && battery->pd_list.now_isApdo)
		pdo_num = 1;
#endif

	pd_power = (battery->pdic_info.sink_status.power_list[pdo_num].max_voltage *
		battery->pdic_info.sink_status.power_list[pdo_num].max_current);

	/* We assume that output voltage to float voltage */
	max_charging_current = pd_power / (battery->pdata->chg_float_voltage / battery->pdata->chg_float_voltage_conv);
	max_charging_current = max_charging_current > battery->pdata->max_charging_current ?
		battery->pdata->max_charging_current : max_charging_current;
	battery->pdata->charging_current[SEC_BATTERY_CABLE_PDIC].fast_charging_current = max_charging_current;
#if defined(CONFIG_PDIC_PD30)
#if defined(CONFIG_STEP_CHARGING)
	if (is_pd_apdo_wire_type(battery->wire_status) && !battery->pd_list.now_isApdo &&
		battery->step_charging_status < 0)
#else
	if (is_pd_apdo_wire_type(battery->wire_status) && !battery->pd_list.now_isApdo)
#endif
		battery->pdata->charging_current[SEC_BATTERY_CABLE_PDIC_APDO].fast_charging_current = max_charging_current;
#endif
	battery->charge_power = pd_power / 1000;

	pr_info("%s:pd_charge_power : %dmW, max_charging_current : %dmA\n", __func__,
		battery->charge_power, max_charging_current);
}
#endif
#if defined(CONFIG_DUAL_BATTERY)
void sec_bat_divide_charging_current(struct sec_battery_info *battery, int charging_current)
{
	unsigned int main_current = 0, sub_current = 0, main_charging_rate = 0, sub_charging_rate = 0;

	if (charging_current <= 1700) {
		main_charging_rate = battery->pdata->main_zone1_current_rate;
		sub_charging_rate = battery->pdata->sub_zone1_current_rate;
	} else if (charging_current <= 3200) {
		main_charging_rate = battery->pdata->main_zone2_current_rate;
		sub_charging_rate = battery->pdata->sub_zone2_current_rate;
	} else {
		main_charging_rate = battery->pdata->main_zone3_current_rate;
		sub_charging_rate = battery->pdata->sub_zone3_current_rate;
	}

	main_current = (charging_current * main_charging_rate) / 100;
	sub_current = (charging_current * sub_charging_rate) / 100;

	pr_info("%s: main_charging_rate(%d), sub_charging_rate(%d)\n", __func__,
		main_charging_rate, sub_charging_rate);

	if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND) {
		if (battery->pdata->swelling_main_low_temp_current_2nd)
			main_current = battery->pdata->swelling_main_low_temp_current_2nd;
		if (battery->pdata->swelling_sub_low_temp_current_2nd)
			sub_current = battery->pdata->swelling_sub_low_temp_current_2nd;
	} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING) {
		if (battery->pdata->swelling_main_low_temp_current)
			main_current = battery->pdata->swelling_main_low_temp_current;
		if (battery->pdata->swelling_sub_low_temp_current)
			sub_current = battery->pdata->swelling_sub_low_temp_current;
	} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING) {
		if (battery->pdata->swelling_main_high_temp_current)
			main_current = battery->pdata->swelling_main_high_temp_current;
		if (battery->pdata->swelling_sub_high_temp_current)
			sub_current = battery->pdata->swelling_sub_high_temp_current;
	} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_3RD) {
		if (battery->pdata->swelling_main_low_temp_current_3rd)
			main_current = battery->pdata->swelling_main_low_temp_current_3rd;
		if (battery->pdata->swelling_sub_low_temp_current_3rd)
			sub_current = battery->pdata->swelling_sub_low_temp_current_3rd;
	}

	/* calculate main battery current */
	if (main_current > battery->pdata->max_main_charging_current)
		main_current = battery->pdata->max_main_charging_current;
	else if (main_current < battery->pdata->min_main_charging_current)
		main_current = battery->pdata->min_main_charging_current;

	/* calculate sub battery current */
	if (sub_current > battery->pdata->max_sub_charging_current)
		sub_current = battery->pdata->max_sub_charging_current;
	else if (sub_current < battery->pdata->min_sub_charging_current)
		sub_current = battery->pdata->min_sub_charging_current;

	battery->main_charging_current = main_current;
	battery->sub_charging_current = sub_current;
}

void sec_bat_set_divide_charging_current(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };

	pr_info("%s: charge_m(%d), charge_s(%d)\n", __func__,
		battery->main_charging_current, battery->sub_charging_current);

	value.intval = battery->main_charging_current;
	psy_do_property(battery->pdata->main_limiter_name, set,
		POWER_SUPPLY_EXT_PROP_FASTCHG_LIMIT_CURRENT, value);

	value.intval = battery->sub_charging_current;
	psy_do_property(battery->pdata->sub_limiter_name, set,
		POWER_SUPPLY_EXT_PROP_FASTCHG_LIMIT_CURRENT, value);
}
#endif
#if defined(CONFIG_DISABLE_MFC_IC)
void sec_bat_set_decrease_iout(struct sec_battery_info *battery, bool last_delay)
{
	union power_supply_propval value = {0, };
	int i = 0, step = 3, input_current[3] = {500, 300, 100};
	int prev_input_current = battery->input_current;

	for (i = 0 ; i < step ; i++) {
		if (prev_input_current > input_current[i]) {
			pr_info("@DIS_MFC %s: Wireless iout goes to %dmA before switch charging path to cable\n",
				__func__, input_current[i]);
			prev_input_current = input_current[i];
			value.intval = input_current[i];
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL, value);

			if ((i != step - 1) || last_delay)
				msleep(300);
		}
	}
}

void sec_bat_set_mfc_off(struct sec_battery_info *battery, bool need_ept)
{
	union power_supply_propval value = {0, };
	char wpc_en_status[2];

	if (need_ept) {
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_ONLINE, value);

		msleep(300);
	}

	wpc_en_status[0] = WPC_EN_CHARGING;
	wpc_en_status[1] = false;
	value.strval = wpc_en_status;
	psy_do_property(battery->pdata->wireless_charger_name, set,
		POWER_SUPPLY_EXT_PROP_WPC_EN, value);

	pr_info("@DIS_MFC %s: WC CONTROL: Disable\n", __func__);
}

void sec_bat_set_mfc_on(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	char wpc_en_status[2];

	wpc_en_status[0] = WPC_EN_CHARGING;
	wpc_en_status[1] = true;
	value.strval = wpc_en_status;
	psy_do_property(battery->pdata->wireless_charger_name, set,
		POWER_SUPPLY_EXT_PROP_WPC_EN, value);

	pr_info("@DIS_MFC %s: WC CONTROL: Enable\n", __func__);
}
#endif
int sec_bat_set_charging_current(struct sec_battery_info *battery)
{
	static int afc_init;
	union power_supply_propval value = {0, };
	struct sec_charging_current *cc = sec_bat_charging_current(battery);
	unsigned int input_current = cc->input_current_limit,
		charging_current = cc->fast_charging_current,
		topoff_current = (battery->charging_mode == SEC_BATTERY_CHARGING_2ND) ?
			battery->pdata->full_check_current_2nd : battery->pdata->full_check_current_1st;

	if (battery->aicl_current)
		input_current = battery->aicl_current;
	mutex_lock(&battery->iolock);
	if (is_nocharge_type(battery->cable_type)) {
	} else {
#if !defined(CONFIG_SEC_FACTORY)
		if (!(battery->current_event & SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL)) {
			input_current = sec_bat_check_mix_temp(battery, input_current);
		}
#endif

		/* check input current */
#if !defined(CONFIG_SEC_FACTORY)
		if (!(battery->current_event & SEC_BAT_CURRENT_EVENT_SKIP_HEATING_CONTROL)) {
			if (is_wireless_type(battery->cable_type))
				sec_bat_check_wpc_temp(battery, &input_current, &charging_current);
#if defined(CONFIG_CCIC_NOTIFIER)
			else if (battery->cable_type == SEC_BATTERY_CABLE_PDIC) {
				if (!sec_bat_change_vbus_pd(battery, &input_current))
					sec_bat_check_pdic_temp(battery, &input_current, &charging_current);
			}
#endif
#if defined(CONFIG_DIRECT_CHARGING)
			else if (is_pd_apdo_wire_type(battery->wire_status)) {
				sec_bat_check_direct_chg_temp(battery, &input_current, &charging_current);
			}
#endif
			else {
				if (!sec_bat_change_vbus(battery, &input_current))
					sec_bat_check_afc_temp(battery, &input_current, &charging_current);
			}
		}
#endif
#if defined(CONFIG_CCIC_NOTIFIER)
		if (battery->cable_type == SEC_BATTERY_CABLE_PDIC)
			input_current = sec_bat_check_pd_input_current(battery, input_current);
#endif
		/* set limited charging current during wireless power sharing with cable charging */
		if (battery->pdata->charging_limit_by_tx_check &&
			battery->wc_tx_enable &&
			(is_hv_wire_type(battery->cable_type) || is_pd_wire_type(battery->cable_type)))
			if (charging_current > battery->pdata->charging_limit_current_by_tx)
				charging_current = battery->pdata->charging_limit_current_by_tx;

		input_current = sec_bat_check_afc_input_current(battery, input_current);

		/* Set limited max power when store mode is set and LDU.
		 * Limited max power should be set with over 5% capacity since target could be turned off during boot up
		 */
		if ((battery->store_mode || !battery->charging_enabled) &&
			(battery->capacity >= 5)) {
			unsigned int store_input_max = 0;

			/* arm64: div-by-zero yields 0, so the original limit was 0 */
			if (battery->input_voltage)
				store_input_max = battery->pdata->store_mode_max_input_power /
					battery->input_voltage * 10;

			if (input_current > store_input_max)
				input_current = store_input_max;
		}

		sec_bat_get_charging_current_by_siop(battery, &input_current, &charging_current);
		sec_bat_check_lrp_temp(battery, battery->cable_type, battery->wire_status,
			battery->siop_level, battery->lcd_status, &input_current, &charging_current);

#if defined(CONFIG_ISDB_CHARGING_CONTROL)
		/* set input current as siop input limit with ISDB */
		if ((battery->current_event & SEC_BAT_CURRENT_EVENT_ISDB) &&
			(is_hv_wire_type(battery->cable_type) ||
			(battery->cable_type == SEC_BATTERY_CABLE_PDIC &&
			battery->pd_max_charge_power >= HV_CHARGER_STATUS_STANDARD1 &&
			battery->hv_pdo) ||
			battery->max_charge_power >= HV_CHARGER_STATUS_STANDARD1)) {
			if (input_current > battery->pdata->siop_hv_input_limit_current)
				input_current = battery->pdata->siop_hv_input_limit_current;
		}
#endif

		/* Calculate wireless input current under the specific conditions (wpc_sleep_mode, chg_limit)*/
		if (battery->wc_status != SEC_WIRELESS_PAD_NONE) {
			input_current = sec_bat_get_wireless_current(battery, input_current);
		}

		/* check swelling state */
		if (is_wireless_type(battery->cable_type)) {
			if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND) {
				charging_current = (charging_current > battery->pdata->swelling_wc_low_temp_current_2nd) ?
					battery->pdata->swelling_wc_low_temp_current_2nd : charging_current;
				topoff_current = (topoff_current > battery->pdata->swelling_low_temp_topoff) ?
					battery->pdata->swelling_low_temp_topoff : topoff_current;
			} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING) {
				charging_current = (charging_current > battery->pdata->swelling_wc_high_temp_current) ?
					battery->pdata->swelling_wc_high_temp_current : charging_current;
				topoff_current = (topoff_current > battery->pdata->swelling_high_temp_topoff) ?
					battery->pdata->swelling_high_temp_topoff : topoff_current;
			} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING) {
				charging_current = (charging_current > battery->pdata->swelling_wc_low_temp_current) ?
					battery->pdata->swelling_wc_low_temp_current : charging_current;
			} else if (battery->swelling_low_temp_3rd_ctrl && (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_3RD)) {
				charging_current = (charging_current > battery->pdata->swelling_wc_low_temp_current_3rd) ?
					battery->pdata->swelling_wc_low_temp_current_3rd : charging_current;
			}

			if (is_hv_wireless_type(battery->cable_type) && (battery->wpc_vout_ctrl_lcd_on && battery->lcd_status)) {
				input_current = 500;
				if (input_current != battery->input_current) {
					value.intval = input_current;
					psy_do_property(battery->pdata->charger_name, set,
							POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL, value);
				}
			}
		} else {
			if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_2ND) {
				if (charging_current > battery->pdata->swelling_low_temp_current_2nd) {
					charging_current = battery->pdata->swelling_low_temp_current_2nd;
				}
				topoff_current = (topoff_current > battery->pdata->swelling_low_temp_topoff) ?
					battery->pdata->swelling_low_temp_topoff : topoff_current;
			} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING) {
				if (charging_current > battery->pdata->swelling_high_temp_current) {
					charging_current = battery->pdata->swelling_high_temp_current;
				}
				topoff_current = (topoff_current > battery->pdata->swelling_high_temp_topoff) ?
					battery->pdata->swelling_high_temp_topoff : topoff_current;
			} else if (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING) {
				if (charging_current > battery->pdata->swelling_low_temp_current) {
					charging_current = battery->pdata->swelling_low_temp_current;
				}
			} else if (battery->swelling_low_temp_3rd_ctrl && (battery->current_event & SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING_3RD)) {
				if (charging_current > battery->pdata->swelling_low_temp_current_3rd) {
					charging_current = battery->pdata->swelling_low_temp_current_3rd;
				}
			}

			/* usb unconfigured or suspend*/
			if ((battery->cable_type == SEC_BATTERY_CABLE_USB) && !lpcharge) {
#if defined(CONFIG_LIMIT_CHARGING_DURING_CALL)
				if ((battery->current_event & SEC_BAT_CURRENT_EVENT_CALL)
						&& battery->capacity >= 95) {
					pr_info("%s: usb call\n", __func__);
					input_current = 200;
				}
#endif
#if defined(CONFIG_ENABLE_100MA_CHARGING_BEFORE_USB_CONFIGURED)
				if (battery->current_event & SEC_BAT_CURRENT_EVENT_USB_100MA) {
					pr_info("%s: usb unconfigured\n", __func__);
					input_current = USB_CURRENT_UNCONFIGURED;
					charging_current = USB_CURRENT_UNCONFIGURED;
				}
#endif
			}
			if (battery->current_event & SEC_BAT_CURRENT_EVENT_USB_SUSPENDED) {
				input_current = USB_CURRENT_UNCONFIGURED;
				charging_current = USB_CURRENT_UNCONFIGURED;
				pr_info("%s: usb suspended set current(%d)\n", __func__, input_current);
			}
		} /* is_wireless_type */
	} /* is_nocharge_type(battery->cable_type) */

	/* set input current, charging current */
	if ((battery->refresh_current) ||
		(battery->input_current != input_current) ||
		(battery->charging_current != charging_current)) {
		/* update charge power */
		battery->charge_power = (battery->input_voltage * input_current) / 10;
		if (battery->current_event & SEC_BAT_CURRENT_EVENT_HV_DISABLE) {
			if (battery->charge_power > battery->pdata->nv_charge_power)
				battery->charge_power = battery->pdata->nv_charge_power;
		}

		if (battery->charge_power > battery->max_charge_power)
			battery->max_charge_power = battery->charge_power;

#if defined(CONFIG_DUAL_BATTERY)
		sec_bat_divide_charging_current(battery, charging_current);
		if (battery->charging_current <= charging_current)
			sec_bat_set_divide_charging_current(battery);
#endif

		/* In wireless charging, must be set charging current before input current. */
		if (is_wireless_type(battery->cable_type)) {
			if (battery->charging_current < charging_current) {
				/* common charging current */
				value.intval = charging_current;
				psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CURRENT_AVG, value);
				/* common input current */
				if (battery->input_current != input_current) {
					value.intval = input_current;
					psy_do_property(battery->pdata->charger_name, set,
						POWER_SUPPLY_PROP_CURRENT_MAX, value);
					battery->input_current = input_current;
				}
			} else {
				/* wired charging current */
				value.intval = charging_current;
				psy_do_property(battery->pdata->charger_name, set,
						POWER_SUPPLY_PROP_CURRENT_NOW, value);
				/* common input current */
				value.intval = input_current;
				psy_do_property(battery->pdata->charger_name, set,
					POWER_SUPPLY_PROP_CURRENT_MAX, value);
				battery->input_current = input_current;
			}
		} else {
			/* common input current */
			value.intval = input_current;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CURRENT_MAX, value);
			battery->input_current = input_current;
			/* wired charging current */
			value.intval = charging_current;
			psy_do_property(battery->pdata->charger_name, set,
				POWER_SUPPLY_PROP_CURRENT_NOW, value);
		}

#if defined(CONFIG_DUAL_BATTERY)
		if (battery->charging_current > charging_current) {
			if (!is_wireless_type(battery->cable_type))
				sec_bat_set_divide_charging_current(battery);
		}
#endif

		if (charging_current <= 100)
			battery->charging_current = 100;
		else
			battery->charging_current = charging_current;

#if defined(CONFIG_DUAL_BATTERY)
		pr_info("%s: power(%d), input(%d), charge(%d), charge_m(%d), charge_s(%d)\n", __func__,
			battery->charge_power, battery->input_current, battery->charging_current, battery->main_charging_current, battery->sub_charging_current);
#else
		pr_info("%s: power(%d), input(%d), charge(%d)\n", __func__,
			battery->charge_power, battery->input_current, battery->charging_current);
#endif
	}

#if defined(CONFIG_DIRECT_CHARGING)
	if (battery->dc_float_voltage_set && battery->step_charging_status >= 0) {
		pr_info("%s : step float voltage = %d \n", __func__,
			battery->pdata->dc_step_chg_val_vfloat[battery->pdata->age_step][battery->step_charging_status]);
		value.intval = battery->pdata->dc_step_chg_val_vfloat[battery->pdata->age_step][battery->step_charging_status];
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_EXT_PROP_DIRECT_VOLTAGE_MAX, value);
		battery->dc_float_voltage_set = false;
	}
#endif

	/* set topoff current */
	if ((battery->refresh_current) ||
		(battery->topoff_current != topoff_current)) {
		value.intval = topoff_current;
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_CURRENT_FULL, value);
		battery->topoff_current = topoff_current;
	}
	if (!afc_init) {
		afc_init = true;
#if defined(CONFIG_AFC_CHARGER_MODE)
		value.intval = 1;
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_AFC_CHARGER_MODE,
			value);
#endif
	}

	battery->refresh_current = false;
	mutex_unlock(&battery->iolock);
	return 0;
}

void sec_bat_refresh_charging_current(struct sec_battery_info *battery)
{
	mutex_lock(&battery->iolock);
	battery->refresh_current = true;
	mutex_unlock(&battery->iolock);

	pr_info("%s: refresh charging current (input=%d, fcc=%d, topoff=%d)\n",
		__func__, battery->input_current, battery->charging_current, battery->topoff_current);
	queue_delayed_work(battery->monitor_wqueue, &battery->siop_level_work, 0);
}

int sec_bat_set_charge(struct sec_battery_info *battery,
			int chg_mode)
{
	union power_supply_propval val = {0, };
	ktime_t current_time = {0, };
	struct timespec ts = {0, };

#if defined(CONFIG_PREVENT_USB_CONN_OVERHEAT)
	if ((battery->cable_type == SEC_BATTERY_CABLE_HMT_CONNECTED) ||
		((battery->usb_temp_flag || (battery->misc_event & BATT_MISC_EVENT_TEMP_HICCUP_TYPE)) && (chg_mode != SEC_BAT_CHG_MODE_BUCK_OFF)))
		return 0;
#else
	if (battery->cable_type == SEC_BATTERY_CABLE_HMT_CONNECTED)
		return 0;
#endif

	if ((battery->current_event & SEC_BAT_CURRENT_EVENT_CHARGE_DISABLE ||
		battery->misc_event & BATT_MISC_EVENT_FULL_CAPACITY) &&
		(chg_mode == SEC_BAT_CHG_MODE_CHARGING)) {
		dev_info(battery->dev, "%s: charge disable by HMT or SOC\n", __func__);
		chg_mode = SEC_BAT_CHG_MODE_CHARGING_OFF;
	}

	battery->charger_mode = chg_mode;
	pr_info("%s set %s mode\n", __func__, sec_bat_charge_mode_str[chg_mode]);

	val.intval = battery->status;
	psy_do_property(battery->pdata->charger_name, set,
		POWER_SUPPLY_PROP_STATUS, val);
	current_time = ktime_get_boottime();
	ts = ktime_to_timespec(current_time);

	if (chg_mode == SEC_BAT_CHG_MODE_CHARGING) {
		/*Reset charging start time only in initial charging start */
		if (battery->charging_start_time == 0) {
			if (ts.tv_sec < 1)
				ts.tv_sec = 1;
			battery->charging_start_time = ts.tv_sec;
			battery->charging_next_time =
				battery->pdata->charging_reset_time;
		}
		battery->charging_block = false;
#if defined(CONFIG_DIRECT_CHARGING)
		if (is_pd_apdo_wire_type(battery->cable_type)) {
			sec_bat_reset_step_charging(battery);
			sec_bat_check_dc_step_charging(battery);
		}
#endif
	} else {
		battery->charging_start_time = 0;
		battery->charging_passed_time = 0;
		battery->charging_next_time = 0;
		battery->charging_fullcharged_time = 0;
		battery->full_check_cnt = 0;
		battery->charging_block = true;
#if defined(CONFIG_STEP_CHARGING)
		sec_bat_reset_step_charging(battery);
#endif
#if defined(CONFIG_BATTERY_CISD)
		battery->usb_overheat_check = false;
		battery->cisd.ab_vbat_check_count = 0;
#endif
	}

	battery->temp_highlimit_cnt = 0;
	battery->temp_high_cnt = 0;
	battery->temp_low_cnt = 0;
	battery->temp_recover_cnt = 0;

	if ((battery->wc_tx_enable && battery->buck_cntl_by_tx) &&
		(chg_mode != SEC_BAT_CHG_MODE_BUCK_OFF)) {
		dev_info(battery->dev, "@Tx_Mode %s: buck disable by Tx mode\n", __func__);
		chg_mode = SEC_BAT_CHG_MODE_BUCK_OFF;
	}
	val.intval = chg_mode;
	psy_do_property(battery->pdata->charger_name, set,
		POWER_SUPPLY_PROP_CHARGING_ENABLED, val);
	psy_do_property(battery->pdata->fuelgauge_name, set,
		POWER_SUPPLY_PROP_CHARGING_ENABLED, val);

#if defined(CONFIG_DUAL_BATTERY)
	if (!(battery->charging_mode == SEC_BATTERY_CHARGING_NONE && battery->status == POWER_SUPPLY_STATUS_FULL) &&
		!(battery->charging_mode == SEC_BATTERY_CHARGING_NONE && battery->swelling_mode == SWELLING_MODE_FULL)) {
		/* disable supplement mode execpt 2nd full charge and swelling charging such as charging, discharging, buck off */
		val.intval = 0;
		psy_do_property(battery->pdata->dual_battery_name, set,
		POWER_SUPPLY_PROP_CHARGING_ENABLED, val);
	}
#endif
	return 0;
}
void sec_bat_wc_cv_mode_check(struct sec_battery_info *battery)
{
	union power_supply_propval value = {0, };
	int is_otg_on = 0;

	psy_do_property(battery->pdata->charger_name, get,
			POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
	is_otg_on = value.intval;

	pr_info("%s: battery->wc_cv_mode = %d, otg(%d) \n", __func__, battery->wc_cv_mode, is_otg_on);

	if (battery->capacity >= battery->pdata->wireless_cc_cv && !is_otg_on) {
		pr_info("%s: 4.5W WC Changed Vout input current limit\n", __func__);
		battery->wc_cv_mode = true;
		sec_bat_set_charging_current(battery);
		value.intval = WIRELESS_VOUT_CC_CV_VOUT; // 5.5V
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
		value.intval = WIRELESS_VRECT_ADJ_ROOM_5; // 80mv
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
		if ((battery->cable_type == SEC_BATTERY_CABLE_WIRELESS ||
			battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_STAND ||
			battery->cable_type == SEC_BATTERY_CABLE_WIRELESS_TX)) {
			value.intval = WIRELESS_CLAMP_ENABLE;
			psy_do_property(battery->pdata->wireless_charger_name, set,
				POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION, value);
		}
		/* Change FOD values for CV mode */
		value.intval = POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE;
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_STATUS, value);
	}
}

void sec_bat_siop_level_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
			struct sec_battery_info, siop_level_work.work);

	pr_info("%s : set current by siop level(%d)\n", __func__, battery->siop_level);

	sec_bat_set_charging_current(battery);

	__pm_relax(battery->siop_level_wake_lock);
}
void sec_bat_check_input_voltage(struct sec_battery_info *battery)
{
	unsigned int voltage = 0;
	int cable_type = sec_bat_charging_current_index(battery->cable_type);
	int input_current = battery->pdata->charging_current[cable_type].input_current_limit;

	if (is_pd_wire_type(battery->cable_type)) {
		battery->max_charge_power = battery->pd_max_charge_power;
		return;
	} else if (is_hv_wire_12v_type(battery->cable_type))
		voltage = SEC_INPUT_VOLTAGE_12V;
	else if (is_hv_wire_9v_type(battery->cable_type))
		voltage = SEC_INPUT_VOLTAGE_9V;
	else if (battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_20 ||
			battery->cable_type == SEC_BATTERY_CABLE_HV_WIRELESS_20)
		voltage = battery->wc20_vout;
	else if (is_hv_wireless_type(battery->cable_type) ||
			battery->cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV)
		voltage = SEC_INPUT_VOLTAGE_10V;
	else if (is_nv_wireless_type(battery->cable_type))
		voltage = SEC_INPUT_VOLTAGE_5_5V;
	else
		voltage = SEC_INPUT_VOLTAGE_5V;

	battery->input_voltage = voltage;
	battery->charge_power = (voltage * input_current) / 10;
#if !defined(CONFIG_SEC_FACTORY)
	if (battery->charge_power > battery->max_charge_power)
#endif
	battery->max_charge_power = battery->charge_power;

	pr_info("%s: battery->input_voltage : %d.%dV, %dmW, %dmW)\n", __func__,
		battery->input_voltage/10, battery->input_voltage%10, battery->charge_power, battery->max_charge_power);
}
void sec_bat_afc_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, afc_work.work);
	union power_supply_propval value = {0, };

	dev_info(battery->dev, "%s: start\n", __func__);
	psy_do_property(battery->pdata->charger_name, get,
		POWER_SUPPLY_PROP_CURRENT_MAX, value);
	battery->current_max = value.intval;

	if (battery->current_event & SEC_BAT_CURRENT_EVENT_AFC) {
		sec_bat_set_current_event(battery, 0, SEC_BAT_CURRENT_EVENT_AFC);
		if ((battery->wc_status != SEC_WIRELESS_PAD_NONE &&
		     battery->current_max >= battery->pdata->pre_wc_afc_input_current) ||
		    ((is_hv_wire_type(battery->cable_type) || battery->cable_type == SEC_BATTERY_CABLE_TA) &&
		     battery->current_max >= battery->pdata->pre_afc_input_current)) {
			sec_bat_set_charging_current(battery);
			if (battery->cable_type == SEC_BATTERY_CABLE_TA)
				battery->cisd.cable_data[CISD_CABLE_TA]++;
		}
		if (battery->wc_tx_enable) {
			cancel_delayed_work(&battery->wpc_tx_work);
			__pm_stay_awake(battery->wpc_tx_wake_lock);
			queue_delayed_work(battery->monitor_wqueue,
					&battery->wpc_tx_work, 0);
		}
	}
	dev_info(battery->dev, "%s: End\n", __func__);
	__pm_relax(battery->afc_wake_lock);
}
