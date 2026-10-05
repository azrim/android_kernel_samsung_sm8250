/*
 *  sec_battery_fwupd.c
 *  Samsung Mobile Battery Driver - wireless firmware update
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "include/sec_battery.h"

#if defined(CONFIG_WIRELESS_FIRMWARE_UPDATE)
bool sec_bat_check_boost_mfc_condition(struct sec_battery_info *battery, int mode)
{
	union power_supply_propval value = {0, };
	int boost_status = 0, wpc_det = 0, mst_pwr_en = 0, boot_recov = 0;

	dev_info(battery->dev, "%s \n", __func__);

	/* wpc_det stays 0 unless the caller asks for the RX_INIT check */
	if (mode == SEC_WIRELESS_RX_INIT) {
		psy_do_property(battery->pdata->wireless_charger_name, get,
			POWER_SUPPLY_EXT_PROP_WIRELESS_INITIAL_WC_CHECK, value);
		wpc_det = value.intval;
	}

	if (gpio_is_valid(battery->pdata->mst_pwr_en))
		mst_pwr_en = gpio_get_value(battery->pdata->mst_pwr_en);
	else
		pr_info("%s: invalid gpio(mst_pwr_en)\n", __func__);

	psy_do_property(battery->pdata->charger_name, get,
		POWER_SUPPLY_EXT_PROP_CHARGE_BOOST, value);
	boost_status = value.intval;

	boot_recov = is_boot_recovery();

	pr_info("%s wpc_det(%d), mst_pwr_en(%d), boost_status(%d), boot_recov(%d)\n",
		__func__, wpc_det, mst_pwr_en, boost_status, boot_recov);

	return !boost_status && !wpc_det && !mst_pwr_en && !boot_recov;
}

void sec_bat_fw_update_work(struct sec_battery_info *battery, int mode)
{
	union power_supply_propval value = {0, };
	int ret = 0;

	dev_info(battery->dev, "%s \n", __func__);

	__pm_wakeup_event(battery->vbus_wake_lock, jiffies_to_msecs(HZ * 10));

	switch (mode) {
	case SEC_WIRELESS_RX_SDCARD_MODE:
	case SEC_WIRELESS_RX_BUILT_IN_MODE:
	case SEC_WIRELESS_RX_SPU_MODE:
		mfc_fw_update = true;
		value.intval = mode;
		ret = psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_POWERED_OTG_CONTROL, value);
		if (ret < 0)
			mfc_fw_update = false;
		break;
	case SEC_WIRELESS_TX_ON_MODE:
		value.intval = true;
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL, value);

		value.intval = mode;
		psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_POWERED_OTG_CONTROL, value);
		break;
	case SEC_WIRELESS_TX_OFF_MODE:
		value.intval = false;
		psy_do_property(battery->pdata->charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL, value);
		break;
	default:
		break;
	}
}

void sec_bat_fw_init_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, fw_init_work.work);

	union power_supply_propval value = {0, };
	int ret = 0;

#if defined(CONFIG_WIRELESS_IC_PARAM)
	psy_do_property(battery->pdata->wireless_charger_name, get,
		POWER_SUPPLY_EXT_PROP_WIRELESS_CHECK_FW_VER, value);
	if (value.intval) {
		pr_info("%s: wireless firmware is already updated.\n", __func__);
		return;
	}
#endif
	if (sec_bat_check_boost_mfc_condition(battery, SEC_WIRELESS_RX_INIT) &&
		battery->capacity > 30 && !lpcharge) {
		mfc_fw_update = true;
		value.intval = SEC_WIRELESS_RX_INIT;
		ret = psy_do_property(battery->pdata->wireless_charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_POWERED_OTG_CONTROL, value);
		if (ret < 0)
			mfc_fw_update = false;
	}
}
#endif
#if defined(CONFIG_UPDATE_BATTERY_DATA)
void sec_bat_update_data_work(struct work_struct *work)
{
	struct sec_battery_info *battery = container_of(work,
				struct sec_battery_info, batt_data_work.work);

	sec_battery_update_data(battery->data_path);
	__pm_relax(battery->batt_data_wake_lock);
}
#endif
