// SPDX-License-Identifier: GPL-2.0
/*
 * a96t3x6.c -- Linux driver for A96T3X6 chip as grip sensor
 *
 * Copyright (C) 2017 Samsung Electronics Co.Ltd
 * Author: YunJae Hwang <yjz.hwang@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/wakelock.h>
#include <asm/unaligned.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/kernel.h>
#if defined(CONFIG_MUIC_NOTIFIER)
#include <linux/muic/muic.h>
#include <linux/muic/muic_notifier.h>
#endif
#if defined(CONFIG_CCIC_NOTIFIER)
#include <linux/ccic/ccic_notifier.h>
#endif
#if defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
#include <linux/usb/manager/usb_typec_manager_notifier.h>
#endif
#if defined(CONFIG_VBUS_NOTIFIER)
#include <linux/vbus_notifier.h>
#endif
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#ifdef CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER
#include <linux/hall.h>
#endif
#include "a96t3x6.h"

#if defined(CONFIG_TABLET_MODEL_CONCEPT)
#include <linux/input/pogo_i2c_notifier.h>
#endif

#define COUNTRY_KOR 1
#define COUNTRY_ETC 0

/*
 * ============================================================================
 * Lock Hierarchy and Concurrency Model:
 *
 * 1. Subsystem Locks (Outer / Top level):
 *    - device_lock(&client->dev.mutex): Acquired by the driver core during
 *      probe, remove, shutdown, suspend, and resume.
 *    - input_dev->mutex: Acquired by the input subsystem during device open/close.
 *    - sysfs attribute locks (kernfs active reference): Acquired by VFS/sysfs
 *      during show/store callbacks.
 *
 * 2. Driver Hardware / Serialization Mutex (data->lock):
 *    - Serializes all I2C transactions and multi-step register sequences to
 *      the A96T3X6 hardware against concurrent callers (threaded IRQ, sysfs,
 *      and workqueues).
 *    - Hierarchy rule: Never acquire subsystem locks (1) while holding data->lock.
 *    - Re-entrancy rule: Linux mutexes are non-recursive. Low-level I2C primitives
 *      (__a96t3x6_i2c_*_locked) assume data->lock is ALREADY held by the caller.
 *      Public wrappers (a96t3x6_i2c_*) acquire data->lock internally. Never call
 *      a public wrapper while holding data->lock.
 *
 * 3. Wake Lock Leaf Primitives (data->grip_wake_lock):
 *    - Leaf synchronization primitive (wakelock spinlock); safe to acquire
 *      from process/threaded-irq contexts with or without data->lock held.
 *
 * Teardown Order:
 *    Disable wakeup -> Unregister external notifiers -> Free IRQ (waits for
 *    running threaded handler) -> Cancel all work synchronously -> Unregister
 *    sysfs & sensor devices -> Power off HW & free GPIOs -> Destroy locks -> Free memory.
 * ============================================================================
 */

struct a96t3x6_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct input_dev *noti_input_dev;
	struct device *dev;
	struct mutex lock;
	struct delayed_work debug_work;
#ifdef CONFIG_SENSORS_FW_VENDOR
	struct delayed_work firmware_work;
#endif
#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
	struct delayed_work reset_work;
#endif
#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
	|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	struct work_struct cmdon_work;
	struct work_struct cmdoff_work;
#endif
	struct wake_lock grip_wake_lock;

	atomic_t enable;

	const struct firmware *firm_data_bin;
	const u8 *firm_data_ums;
	char phys[32];
	long firm_size;
	int irq;
	int noti_enable;

	u16 grip_p_thd;
	u16 grip_r_thd;
	u16 grip_n_thd;
	u16 grip_baseline;
	u16 grip_raw;
	u16 grip_raw_d;
	u16 grip_event;
	u16 diff;
	u16 diff_d;

#ifdef CONFIG_SEC_FACTORY
	int irq_count;
	int abnormal_mode;
	s16 max_diff;
	s16 max_normal_diff;
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	s16 max_diff_2ch;
	s16 max_normal_diff_2ch;
#endif
#endif
	int irq_en_cnt;
	u8 fw_update_state;
	u8 fw_ver;
	u8 md_ver;
	u8 fw_ver_bin;
	u8 md_ver_bin;
	u8 checksum_h;
	u8 checksum_h_bin;
	u8 checksum_l;
	u8 checksum_l_bin;
	u8 fw_write_result;
	u8 earjack_noise;

	bool skip_event;
	bool resume_called;
	bool bringup;
	bool probe_done;
	bool sar_mode;
	bool sar_enable_off;
	bool earjack;
	bool first_working;

	s16 is_unknown_mode;
	s16 motion;

	int ldo_en;
	int grip_int;
	const char *dvdd_vreg_name;
	struct regulator *dvdd_vreg;
	int (*power)(void *pdata, bool on);
	const char *chipid;
	const char *fw_path;

	int firmup_cmd;
	int debug_count;
	int firmware_count;

	int hall_prev_state;
	int cert_hall_prev_state;
#if defined(CONFIG_WACOM_HALL)
	int wacom_hall_prev_state;
#endif
#if defined(CONFIG_CCIC_NOTIFIER) && defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	int ccic_pre_attach;
	bool manager_notifier_registered;
#endif

#if defined(CONFIG_MUIC_NOTIFIER)
	struct notifier_block cpuidle_muic_nb;
#endif
#if defined(CONFIG_CCIC_NOTIFIER)
	struct notifier_block cpuidle_ccic_nb;
#endif
#if defined(CONFIG_VBUS_NOTIFIER)
	struct notifier_block vbus_nb;
#endif
#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
	struct notifier_block hall_nb;
#endif
#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	struct notifier_block pogo_nb;
#endif
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	u16 grip_p_thd_2ch;
	u16 grip_r_thd_2ch;
	u16 grip_n_thd_2ch;
	u16 grip_baseline_2ch;
	u16 grip_raw_2ch;
	u16 grip_raw_d_2ch;
	u16 diff_2ch;
	u16 diff_d_2ch;
	u16 grip_event_2ch;
	int is_unknown_mode_2ch;
	bool first_working_2ch;
#endif
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	int mcc;
	u16 default_threshold;
	u16 mcc_threshold;
#endif
#ifdef CONFIG_TABLET_MODEL_CONCEPT
	int country_code;
#endif
};

static bool recovery_mode;

static int a96t3x6_recovery_mode_check(char *str)
{
	if (strncmp(str, "1", 1) == 0)
		recovery_mode = true;
	else
		recovery_mode = false;

	GRIP_INFO("recovery_mode = %d\n", recovery_mode);
	return true;
}
EXPORT_SYMBOL(a96t3x6_recovery_mode_check);
__setup("androidboot.boot_recovery=", a96t3x6_recovery_mode_check);

/* Low-level I2C helpers */
static int __a96t3x6_i2c_read_locked(struct i2c_client *client,
				     u8 reg, u8 *val, unsigned int len)
{
	struct i2c_msg msg;
	int ret;
	int retry = 3;

	msg.addr = client->addr;
	msg.flags = I2C_M_WR;
	msg.len = 1;
	msg.buf = &reg;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			break;

		GRIP_INFO("fail(address set)(%d)(%d)\n", retry, ret);
		usleep_range(10000, 11000);
	}
	if (ret < 0)
		return ret;

	retry = 3;
	msg.flags = I2C_M_RD;
	msg.len = len;
	msg.buf = val;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			return 0;

		GRIP_INFO("fail(data read)(%d)(%d)\n", retry, ret);
		usleep_range(10000, 11000);
	}
	return ret;
}

static int a96t3x6_i2c_read(struct i2c_client *client,
			    u8 reg, u8 *val, unsigned int len)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->lock);
	ret = __a96t3x6_i2c_read_locked(client, reg, val, len);
	mutex_unlock(&data->lock);

	return ret;
}

static int __a96t3x6_i2c_read_data_locked(struct i2c_client *client, u8 *val,
					  unsigned int len)
{
	struct i2c_msg msg;
	int ret;
	int retry = 3;

	msg.addr = client->addr;
	msg.flags = I2C_M_RD;
	msg.len = len;
	msg.buf = val;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			return 0;

		GRIP_INFO("fail(data read)(%d)\n", retry);
		usleep_range(10000, 11000);
	}
	return ret;
}

static int a96t3x6_i2c_read_data(struct i2c_client *client, u8 *val,
				 unsigned int len)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->lock);
	ret = __a96t3x6_i2c_read_data_locked(client, val, len);
	mutex_unlock(&data->lock);

	return ret;
}

static int __a96t3x6_i2c_write_locked(struct i2c_client *client, u8 reg, u8 *val)
{
	struct i2c_msg msg[1];
	unsigned char buf[2];
	int ret;
	int retry = 3;

	buf[0] = reg;
	buf[1] = *val;
	msg->addr = client->addr;
	msg->flags = I2C_M_WR;
	msg->len = 2;
	msg->buf = buf;

	while (retry--) {
		ret = i2c_transfer(client->adapter, msg, 1);
		if (ret >= 0)
			return 0;

		GRIP_INFO("fail(%d)\n", retry);
		usleep_range(10000, 11000);
	}
	return ret;
}

static int a96t3x6_i2c_write(struct i2c_client *client, u8 reg, u8 *val)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->lock);
	ret = __a96t3x6_i2c_write_locked(client, reg, val);
	mutex_unlock(&data->lock);

	return ret;
}

/* Forward declarations */
static void a96t3x6_reset(struct a96t3x6_data *data);
static void a96t3x6_diff_getdata(struct a96t3x6_data *data, int log);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
static void a96t3x6_2ch_diff_getdata(struct a96t3x6_data *data);
#endif
static void a96t3x6_check_first_status(struct a96t3x6_data *data, int enable);
#ifdef CONFIG_SENSORS_FW_VENDOR
static int a96t3x6_fw_check(struct a96t3x6_data *data);
static void a96t3x6_set_firmware_work(struct a96t3x6_data *data, u8 enable,
				      unsigned int time_ms);
#endif

static void a96t3x6_sar_only_mode(struct a96t3x6_data *data, int on)
{
#ifdef CONFIG_SENSORS_A96T3X6_BLOCK_SAR_ONLY
	pr_debug("No action with sar only mode\n");
#else
	int ret;
	u8 cmd;
	u8 r_buf;

	if (data->sar_mode == on) {
		GRIP_INFO("skip already %s\n", (on == 1) ? "sar only mode" : "normal mode");
		return;
	}

	if (on == 1)
		cmd = CMD_ON;
	else
		cmd = CMD_OFF;

	GRIP_INFO("%s, cmd=%x\n", (on == 1) ? "sar only mode" : "normal mode", cmd);

	ret = a96t3x6_i2c_write(data->client, REG_SAR_MODE, &cmd);
	if (ret < 0)
		GRIP_INFO("i2c read fail(%d)", ret);

	usleep_range(40000, 40000);

	ret = a96t3x6_i2c_read(data->client, REG_SAR_MODE, &r_buf, 1);
	if (ret < 0) {
		GRIP_INFO("i2c read fail(%d)\n", ret);
	} else {
		GRIP_INFO("read reg = %x\n", r_buf);

		if (r_buf == CMD_ON)
			data->sar_mode = 1;
		else
			data->sar_mode = 0;
	}
#endif
}

static void grip_always_active(struct a96t3x6_data *data, int on)
{
	int ret, retry = 3;
	u8 cmd, r_buf;

	GRIP_INFO("Grip always active mode %d\n", on);

	if (on == 1)
		cmd = CMD_ON;
	else
		cmd = CMD_OFF;

	while (retry--) {
		ret = a96t3x6_i2c_write(data->client, REG_GRIP_ALWAYS_ACTIVE, &cmd);
		if (ret < 0) {
			GRIP_INFO("i2c write fail(%d)\n", ret);
			continue;
		}

		msleep(20);

		ret = a96t3x6_i2c_read(data->client, REG_GRIP_ALWAYS_ACTIVE, &r_buf, 1);
		if (ret < 0) {
			GRIP_ERR("i2c read fail(%d)\n", ret);
			continue;
		}

		if ((cmd == CMD_ON && r_buf == GRIP_ALWAYS_ACTIVE_READY) ||
		    (cmd == CMD_OFF && r_buf == CMD_OFF))
			break;
		else
			GRIP_INFO("Wrong value 0x%x, retry again %d\n", r_buf, retry);
	}

	if (retry < 0)
		GRIP_INFO("failed to change grip always active mode\n");
	else
		GRIP_INFO("cmd 0x%x, return 0x%x\n", cmd, r_buf);
}

static void a96t3x6_sar_sensing(struct a96t3x6_data *data, int on)
{
	u8 cmd;
	int ret;

	GRIP_INFO("%s", (on) ? "on" : "off");

	if (on)
		cmd = CMD_ON;
	else
		cmd = CMD_OFF;

	ret = a96t3x6_i2c_write(data->client, REG_SAR_SENSING, &cmd);
	if (ret < 0)
		GRIP_INFO("failed to %s grip sensing\n", (on) ? "enable" : "disable");
}

static void a96t3x6_reset_for_bootmode(struct a96t3x6_data *data)
{
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	int ret;
	u8 cmd = CMD_RESET;

	GRIP_INFO("\n");
	ret = a96t3x6_i2c_write(data->client, REG_RESET, &cmd);
	if (ret < 0)
		GRIP_INFO("i2c read fail(%d)\n", ret);
#else
	GRIP_INFO("\n");

	if (data->power)
		data->power(data, false);
	usleep_range(50000, 50000);
	if (data->power)
		data->power(data, true);
#endif
}

static int a96t3x6_set_enable(struct a96t3x6_data *data, int enable)
{
	u8 cmd;
	u8 reg_value = 0;
	int ret;
	int pre_enable = atomic_read(&data->enable);

	GRIP_INFO("enable = %d pre_enable = %d\n", enable, pre_enable);

	ret = a96t3x6_i2c_read(data->client, REG_SAR_ENABLE, &reg_value, 1);

	if ((pre_enable == 1 && reg_value == CMD_ON) || (pre_enable == 0 && reg_value == CMD_OFF)) {
		if (pre_enable == enable) {
			GRIP_INFO("skip\n");
			return -EINVAL;
		}
	}

	if (enable) {
		cmd = CMD_ON;

		ret = a96t3x6_i2c_write(data->client, REG_SAR_ENABLE, &cmd);
		if (ret < 0) {
			GRIP_ERR("failed to enable grip irq\n");
			atomic_set(&data->enable, 0);
			return -EIO;
		}

		a96t3x6_check_first_status(data, enable);

		enable_irq(data->irq);
		enable_irq_wake(data->irq);

		data->irq_en_cnt++;
		atomic_set(&data->enable, 1);
		return 0;
	}

	cmd = CMD_OFF;

	disable_irq_wake(data->irq);
	disable_irq(data->irq);

	ret = a96t3x6_i2c_write(data->client, REG_SAR_ENABLE, &cmd);
	if (ret < 0)
		GRIP_ERR("failed to disable grip irq\n");

	atomic_set(&data->enable, 0);
	return 0;
}

static void a96t3x6_reset(struct a96t3x6_data *data)
{
	int enable = atomic_read(&data->enable);

	if (!enable)
		return;

	GRIP_INFO("start\n");
	disable_irq_nosync(data->irq);

	a96t3x6_reset_for_bootmode(data);
	usleep_range(RESET_DELAY, RESET_DELAY);

	if (enable) {
		if (a96t3x6_set_enable(data, 1))
			enable_irq(data->irq);
	}

	GRIP_INFO("done\n");
}

static void a96t3x6_diff_getdata(struct a96t3x6_data *data, int log)
{
	int ret;
	int retry = 3;
	u8 r_buf[4] = {0,};

	while (retry--) {
		ret = a96t3x6_i2c_read(data->client, REG_SAR_DIFFDATA, r_buf, 4);
		if (ret == 0)
			break;
		GRIP_INFO("read failed(%d)\n", retry);
		usleep_range(10000, 10000);
	}

	data->diff = (r_buf[0] << 8) | r_buf[1];
	data->diff_d = (r_buf[2] << 8) | r_buf[3];
	if (log)
		GRIP_INFO("%u\n", data->diff);
}

#ifdef CONFIG_SENSORS_A96T3X6_2CH
static void a96t3x6_2ch_diff_getdata(struct a96t3x6_data *data)
{
	int ret;
	int retry = 3;
	u8 r_buf[4] = {0,};

	while (retry--) {
		ret = a96t3x6_i2c_read(data->client, REG_SAR_DIFFDATA_D_2CH, r_buf, 4);
		if (ret == 0)
			break;
		GRIP_ERR("read failed(%d)\n", retry);
		usleep_range(10000, 10000);
	}

	data->diff_2ch = (r_buf[0] << 8) | r_buf[1];
	data->diff_d_2ch = (r_buf[2] << 8) | r_buf[3];

	GRIP_INFO("2ch %u\n", data->diff_2ch);
}
#endif

static void a96t3x6_enter_unknown_mode(struct a96t3x6_data *data, int type)
{
	if (data->noti_enable && !data->skip_event) {
		data->motion = 0;
		data->first_working = false;
		if (data->is_unknown_mode == UNKNOWN_OFF) {
			data->is_unknown_mode = UNKNOWN_ON;
			if (!data->skip_event) {
				input_report_rel(data->input_dev, REL_X, data->is_unknown_mode);
				input_sync(data->input_dev);
			}
			GRIP_INFO("UNKNOWN Re-enter\n");
		} else {
			GRIP_INFO("already UNKNOWN\n");
		}

#ifdef CONFIG_SENSORS_A96T3X6_2CH
		data->first_working_2ch = false;
		if (data->is_unknown_mode_2ch == UNKNOWN_OFF) {
			data->is_unknown_mode_2ch = UNKNOWN_ON;
			if (!data->skip_event) {
				input_report_rel(data->input_dev, REL_Y, data->is_unknown_mode_2ch);
				input_sync(data->input_dev);
			}
			GRIP_INFO("2ch UNKNOWN Re-enter\n");
		} else {
			GRIP_INFO("2ch already UNKNOWN\n");
		}
#endif
		input_report_rel(data->noti_input_dev, REL_X, type);
		input_sync(data->noti_input_dev);
	}
}

static void a96t3x6_check_first_status(struct a96t3x6_data *data, int enable)
{
	u8 r_buf[4];
	u16 grip_thd;
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	u16 grip_thd_2ch;
#endif

	if (data->skip_event) {
		GRIP_INFO("skip event..\n");
		return;
	}

	a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD, r_buf, 4);
	grip_thd = (r_buf[0] << 8) | r_buf[1];

	a96t3x6_diff_getdata(data, 1);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD_2CH, r_buf, 4);
	grip_thd_2ch = (r_buf[0] << 8) | r_buf[1];

	a96t3x6_2ch_diff_getdata(data);
#endif
	if (grip_thd < data->diff)
		input_report_rel(data->input_dev, REL_MISC, 1);
	else
		input_report_rel(data->input_dev, REL_MISC, 2);

#ifdef CONFIG_SENSORS_A96T3X6_2CH
	if (grip_thd_2ch < data->diff_2ch)
		input_report_rel(data->input_dev, REL_DIAL, 1);
	else
		input_report_rel(data->input_dev, REL_DIAL, 2);
#endif
	input_report_rel(data->input_dev, REL_X, data->is_unknown_mode);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	input_report_rel(data->input_dev, REL_Y, data->is_unknown_mode_2ch);
#endif
	input_sync(data->input_dev);
}

static void a96t3x6_check_diff_and_cap(struct a96t3x6_data *data)
{
	u8 r_buf[2] = {0, 0};
	u8 cmd = 0x20;
	int ret;
	int value = 0;

	ret = a96t3x6_i2c_write(data->client, REG_SAR_TOTALCAP, &cmd);
	if (ret < 0)
		GRIP_ERR("write fail(%d)\n", ret);

	usleep_range(20, 20);

	ret = a96t3x6_i2c_read(data->client, REG_SAR_TOTALCAP_READ, r_buf, 2);
	if (ret < 0)
		GRIP_ERR("fail(%d)\n", ret);

	value = (r_buf[0] << 8) | r_buf[1];
	GRIP_INFO("Cap Read %d\n", value);

	a96t3x6_diff_getdata(data, 1);
}

static void a96t3x6_grip_sw_reset(struct a96t3x6_data *data)
{
	int ret, retry = 3;
	u8 cmd = CMD_SW_RESET;

	GRIP_INFO("\n");

	while (retry--) {
		a96t3x6_check_diff_and_cap(data);
		usleep_range(10000, 10000);
	}

	ret = a96t3x6_i2c_write(data->client, REG_SW_RESET, &cmd);
	if (ret < 0)
		GRIP_ERR("fail(%d)\n", ret);
	else
		usleep_range(35000, 35000);
}

static int a96t3x6_get_hallic_state(char *file_path)
{
	char hall_buf[6];
	int ret = -ENODEV;
	int hall_state = -1;
	mm_segment_t old_fs;
	struct file *filep;

	memset(hall_buf, 0, sizeof(hall_buf));
	old_fs = get_fs();
	set_fs(KERNEL_DS);

	filep = filp_open(file_path, O_RDONLY, 0666);
	if (IS_ERR(filep)) {
		set_fs(old_fs);
		return hall_state;
	}

	ret = filep->f_op->read(filep, hall_buf,
				sizeof(hall_buf) - 1, &filep->f_pos);
	if (ret != sizeof(hall_buf) - 1)
		goto exit;

	if (strcmp(hall_buf, "CLOSE") == 0)
		hall_state = HALL_CLOSE_STATE;

exit:
	filp_close(filep, current->files);
	set_fs(old_fs);

	return hall_state;
}

#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
static void a96t3x6_reset_work_func(struct work_struct *work)
{
	struct a96t3x6_data *data = container_of((struct delayed_work *)work,
						struct a96t3x6_data, reset_work);

	a96t3x6_grip_sw_reset(data);
}
#endif

#ifdef CONFIG_SENSORS_FW_VENDOR
static void a96t3x6_firmware_work_func(struct work_struct *work)
{
	struct a96t3x6_data *data = container_of((struct delayed_work *)work,
						struct a96t3x6_data, firmware_work);
	int ret;
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	u8 r_buf[2];
#endif

	GRIP_INFO("called\n");

	if (recovery_mode) {
		GRIP_INFO("recovery mode: skip fw check\n");
		return;
	}

	ret = a96t3x6_fw_check(data);
	if (ret) {
		if (data->firmware_count++ < FIRMWARE_VENDOR_CALL_CNT) {
			GRIP_ERR("failed to load firmware ret %d(%d)\n", ret,
				 data->firmware_count);
			schedule_delayed_work(&data->firmware_work,
					      msecs_to_jiffies(1000));
			return;
		}
		GRIP_ERR("final retry failed\n");
#ifdef CONFIG_SEC_SENSORS_ENG_DEBUG
		panic("grip fw doesn't exist\n");
#endif
	} else {
		GRIP_INFO("fw check success\n");
	}

#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	ret = a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD, r_buf, 2);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		data->default_threshold = 0;
	}
	data->default_threshold = (r_buf[0] << 8) | r_buf[1];
	GRIP_INFO("default threshold = %d\n", data->default_threshold);
#endif
}
#endif

#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
	|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
static void cmdon_work_func(struct work_struct *work)
{
	struct a96t3x6_data *grip_data = container_of(work, struct a96t3x6_data, cmdon_work);
	u8 cmd = CMD_ON;

	if (grip_data)
		a96t3x6_i2c_write(grip_data->client, REG_TSPTA, &cmd);
}

static void cmdoff_work_func(struct work_struct *work)
{
	struct a96t3x6_data *grip_data = container_of(work, struct a96t3x6_data, cmdoff_work);
	u8 cmd = CMD_OFF;

	if (grip_data)
		a96t3x6_i2c_write(grip_data->client, REG_TSPTA, &cmd);
}
#endif

#if defined(CONFIG_CCIC_NOTIFIER) && defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
static int a96t3x6_ccic_handle_notification(struct notifier_block *nb,
					    unsigned long action, void *dev_data)
{
	CC_NOTI_ATTACH_TYPEDEF usb_typec_info = *(CC_NOTI_ATTACH_TYPEDEF *)dev_data;
	struct a96t3x6_data *grip_data = container_of(nb, struct a96t3x6_data, cpuidle_ccic_nb);
	u8 cmd = CMD_ON;

	GRIP_INFO("src = %d, id = %d, attach = %d\n",
		  usb_typec_info.src, usb_typec_info.id, usb_typec_info.attach);

	if (grip_data->ccic_pre_attach == usb_typec_info.attach)
		return 0;

	if (usb_typec_info.src == CCIC_NOTIFY_DEV_CCIC &&
	    usb_typec_info.dest == CCIC_NOTIFY_DEV_BATTERY) {
#if defined(CONFIG_SENSORS_SKIP_CABLE_RESET) && defined(CONFIG_TABLET_MODEL_CONCEPT)
		GRIP_INFO("grip reset skip for tablet\n");
#else
		if (usb_typec_info.id == CCIC_NOTIFY_ID_ATTACH &&
		    usb_typec_info.attach == MUIC_NOTIFY_CMD_DETACH) {
			cmd = CMD_ON;
			schedule_work(&grip_data->cmdon_work);
			a96t3x6_enter_unknown_mode(grip_data, TYPE_USB);
		} else if (usb_typec_info.id == CCIC_NOTIFY_ID_POWER_STATUS &&
			   usb_typec_info.attach == MUIC_NOTIFY_CMD_ATTACH) {
			cmd = CMD_OFF;
			schedule_work(&grip_data->cmdoff_work);
			a96t3x6_enter_unknown_mode(grip_data, TYPE_USB);
		} else {
			return 0;
		}
#endif
		GRIP_INFO("PDTA id = %d, attach = %d, cmd = %d\n",
			  usb_typec_info.id, usb_typec_info.attach, cmd);
	} else if (usb_typec_info.src == CCIC_NOTIFY_DEV_MUIC &&
		   usb_typec_info.dest == CCIC_NOTIFY_DEV_BATTERY) {
		switch (usb_typec_info.cable_type) {
		case ATTACHED_DEV_NONE_MUIC:
		case ATTACHED_DEV_JIG_UART_OFF_MUIC:
		case ATTACHED_DEV_JIG_UART_OFF_VB_MUIC:
		case ATTACHED_DEV_JIG_UART_OFF_VB_OTG_MUIC:
		case ATTACHED_DEV_JIG_UART_OFF_VB_FG_MUIC:
		case ATTACHED_DEV_JIG_UART_ON_MUIC:
		case ATTACHED_DEV_JIG_UART_ON_VB_MUIC:
		case ATTACHED_DEV_JIG_USB_OFF_MUIC:
		case ATTACHED_DEV_JIG_USB_ON_MUIC:
#ifdef CONFIG_SENSORS_A96T3X6_UNSUPPORT_OTG
		case ATTACHED_DEV_OTG_MUIC:
#endif
			GRIP_INFO("skip cable = %d, attach = %d\n",
				  usb_typec_info.cable_type, usb_typec_info.attach);
			break;
		default:
#if defined(CONFIG_SENSORS_SKIP_CABLE_RESET) && defined(CONFIG_TABLET_MODEL_CONCEPT)
			GRIP_INFO("grip reset skip for tablet\n");
#else
			if (usb_typec_info.attach == MUIC_NOTIFY_CMD_DETACH) {
				cmd = CMD_ON;
				schedule_work(&grip_data->cmdon_work);
			} else if (usb_typec_info.attach == MUIC_NOTIFY_CMD_ATTACH) {
				cmd = CMD_OFF;
				schedule_work(&grip_data->cmdoff_work);
			} else {
				schedule_work(&grip_data->cmdon_work);
			}
			a96t3x6_enter_unknown_mode(grip_data, TYPE_USB);
#endif
			GRIP_INFO("attach = %d, cmd = %d\n", usb_typec_info.attach, cmd);
			break;
		}
	} else {
		return 0;
	}

	grip_data->ccic_pre_attach = usb_typec_info.attach;
	return 0;
}
#endif

#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
static int a96t3x6_hall_ic_notify(struct notifier_block *nb,
				  unsigned long flip_cover, void *v)
{
#if IS_ENABLED(CONFIG_TABLET_MODEL_CONCEPT)
	struct hall_notifier_context *hall_notifier = v;
#endif
	struct a96t3x6_data *data = container_of(nb, struct a96t3x6_data, hall_nb);

	if (!data) {
		GRIP_ERR("data is null\n");
		return 0;
	}

#ifdef CONFIG_SENSORS_A96T3X6_2CH
	GRIP_INFO("set flip unknown mode(flip %s,prev %d %d)\n",
		  flip_cover ? "close" : "open", data->is_unknown_mode, data->is_unknown_mode_2ch);
#else
	GRIP_INFO("set flip unknown mode(flip %s,prev %d)\n",
		  flip_cover ? "close" : "open", data->is_unknown_mode);
#endif

#if IS_ENABLED(CONFIG_TABLET_MODEL_CONCEPT)
	if (flip_cover) {
		GRIP_INFO("%s attach\n", hall_notifier->name);
		if (strncmp(hall_notifier->name, "certify_hall", sizeof("certify_hall") - 1) == 0 ||
		    strncmp(hall_notifier->name, "hall_wacom", sizeof("hall_wacom") - 1) == 0) {
			schedule_delayed_work(&data->reset_work, msecs_to_jiffies(1));
			GRIP_INFO("reset only without unknown, %s\n", hall_notifier->name);
		} else if (strncmp(hall_notifier->name, "hall", sizeof("hall") - 1) == 0) {
			GRIP_INFO("reset skip, %s\n", hall_notifier->name);
		} else {
			GRIP_INFO("%s is not defined, hall_notifier_name\n", hall_notifier->name);
		}
	}
#else
	if (flip_cover)
		schedule_delayed_work(&data->reset_work, msecs_to_jiffies(1));
	a96t3x6_enter_unknown_mode(data, TYPE_HALL);
#endif
	return 0;
}
#endif

#if defined(CONFIG_TABLET_MODEL_CONCEPT)
static int a96t3x6_pogo_notifier(struct notifier_block *nb,
				 unsigned long action, void *pogo_data)
{
	struct a96t3x6_data *data = container_of(nb, struct a96t3x6_data, pogo_nb);

	switch (action) {
	case POGO_NOTIFIER_ID_ATTACHED:
#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
		schedule_delayed_work(&data->reset_work, msecs_to_jiffies(1));
#else
		a96t3x6_grip_sw_reset(data);
#endif
		GRIP_INFO("pogo attach\n");
		break;
	case POGO_NOTIFIER_ID_DETACHED:
		GRIP_INFO("pogo dettach\n");
		break;
	}
	return 0;
}
#endif

static void a96t3x6_check_first_working(struct a96t3x6_data *data, int channel_num)
{
	if (data->noti_enable && data->motion) {
		if (channel_num == 1) {
			if (data->grip_p_thd < data->diff) {
				if (!data->first_working) {
					data->first_working = true;
					GRIP_INFO("first working detected %d\n", data->diff);
				}
			} else {
				if (data->first_working &&
				    (data->is_unknown_mode == UNKNOWN_ON)) {
					data->is_unknown_mode = UNKNOWN_OFF;
					GRIP_INFO("Release detected %d, unknown mode off\n", data->diff);
				}
			}
		}
#ifdef CONFIG_SENSORS_A96T3X6_2CH
		else if (channel_num == 2) {
			if (data->grip_p_thd_2ch < data->diff_2ch) {
				if (!data->first_working_2ch) {
					data->first_working_2ch = true;
					GRIP_INFO("2ch first working detected %d\n", data->diff_2ch);
				}
			} else {
				if (data->first_working_2ch &&
				    (data->is_unknown_mode_2ch == UNKNOWN_ON)) {
					data->is_unknown_mode_2ch = UNKNOWN_OFF;
					GRIP_INFO("2ch Release detected %d, unknown mode off\n", data->diff_2ch);
				}
			}
		}
#endif
	}
}

static void a96t3x6_set_debug_work(struct a96t3x6_data *data, u8 enable,
				   unsigned int time_ms)
{
	pr_debug("enable = %d\n", enable);

	if (enable == 1) {
		data->debug_count = 0;
		schedule_delayed_work(&data->debug_work,
				      msecs_to_jiffies(time_ms));
	} else {
		cancel_delayed_work_sync(&data->debug_work);
	}
}

#ifdef CONFIG_SENSORS_FW_VENDOR
static void a96t3x6_set_firmware_work(struct a96t3x6_data *data, u8 enable,
				      unsigned int time_ms)
{
	GRIP_INFO("%s\n", enable ? "enabled" : "disabled");

	if (enable == 1) {
		data->firmware_count = 0;
		schedule_delayed_work(&data->firmware_work,
				      msecs_to_jiffies(time_ms));
	} else {
		cancel_delayed_work_sync(&data->firmware_work);
	}
}
#endif

static void a96t3x6_debug_work_func(struct work_struct *work)
{
	struct a96t3x6_data *data = container_of((struct delayed_work *)work,
						struct a96t3x6_data, debug_work);
	int hall_state, cert_hall_state;
#if defined(CONFIG_WACOM_HALL)
	int wacom_hall_state;
#endif

	if (data->resume_called) {
		data->resume_called = false;
		a96t3x6_sar_only_mode(data, 0);
		schedule_delayed_work(&data->debug_work, msecs_to_jiffies(1000));
		return;
	}

	hall_state = a96t3x6_get_hallic_state(HALL_PATH);
	cert_hall_state = a96t3x6_get_hallic_state(HALLIC_CERT_PATH);
#if defined(CONFIG_WACOM_HALL)
	wacom_hall_state = a96t3x6_get_hallic_state(WACOM_HALL_PATH);
#endif

#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	if (hall_state == HALL_CLOSE_STATE && data->hall_prev_state != hall_state) {
		GRIP_DEBUG("%s - hall is closed %d %d\n", __func__, hall_state, cert_hall_state);
		GRIP_INFO("%s - hall reset skip for tablet only\n", __func__);
	} else if ((cert_hall_state == HALL_CLOSE_STATE &&
		    data->cert_hall_prev_state != cert_hall_state)
#if defined(CONFIG_WACOM_HALL)
		   || (wacom_hall_state == HALL_CLOSE_STATE &&
		       data->wacom_hall_prev_state != wacom_hall_state)
#endif
		) {
		GRIP_INFO("%s - certi hall is closed %d\n", __func__, cert_hall_state);
#if defined(CONFIG_WACOM_HALL)
		GRIP_INFO("%s - wacom hall is closed %d\n", __func__, wacom_hall_state);
#endif
		a96t3x6_grip_sw_reset(data);
	}
#else
	if ((hall_state == HALL_CLOSE_STATE &&
	     data->hall_prev_state != hall_state) ||
	    (cert_hall_state == HALL_CLOSE_STATE &&
	     data->cert_hall_prev_state != cert_hall_state)
#if defined(CONFIG_WACOM_HALL)
	    || (wacom_hall_state == HALL_CLOSE_STATE &&
		data->wacom_hall_prev_state != wacom_hall_state)
#endif
		) {
		GRIP_DEBUG("%s - hall is closed %d %d\n", __func__, hall_state, cert_hall_state);
#if defined(CONFIG_WACOM_HALL)
		GRIP_INFO("%s - wacom hall is closed %d\n", __func__, wacom_hall_state);
#endif
		a96t3x6_grip_sw_reset(data);
	}
#endif

	if ((data->hall_prev_state != hall_state) ||
	    (data->cert_hall_prev_state != cert_hall_state)
#if defined(CONFIG_WACOM_HALL)
	    || (data->wacom_hall_prev_state != wacom_hall_state)
#endif
		) {
		a96t3x6_enter_unknown_mode(data, TYPE_HALL);
	}

	data->hall_prev_state = hall_state;
	data->cert_hall_prev_state = cert_hall_state;
#if defined(CONFIG_WACOM_HALL)
	data->wacom_hall_prev_state = wacom_hall_state;
#endif

#ifdef CONFIG_SEC_FACTORY
	if (atomic_read(&data->enable)) {
		if (data->abnormal_mode) {
			a96t3x6_diff_getdata(data, 1);
			if (data->max_normal_diff < data->diff)
				data->max_normal_diff = data->diff;
#ifdef CONFIG_SENSORS_A96T3X6_2CH
			a96t3x6_2ch_diff_getdata(data);
			if (data->max_normal_diff_2ch < data->diff_2ch)
				data->max_normal_diff_2ch = data->diff_2ch;
#endif
		}
	}
#endif

	if (data->debug_count >= GRIP_LOG_TIME) {
		a96t3x6_diff_getdata(data, 1);
		if (data->is_unknown_mode == UNKNOWN_ON && data->motion)
			a96t3x6_check_first_working(data, 1);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
		a96t3x6_2ch_diff_getdata(data);
		if (data->is_unknown_mode_2ch == UNKNOWN_ON && data->motion)
			a96t3x6_check_first_working(data, 2);
#endif
		data->debug_count = 0;
	} else {
		if (data->is_unknown_mode == UNKNOWN_ON && data->motion) {
			a96t3x6_diff_getdata(data, 0);
			a96t3x6_check_first_working(data, 1);
		}
#ifdef CONFIG_SENSORS_A96T3X6_2CH
		if (data->is_unknown_mode_2ch == UNKNOWN_ON && data->motion) {
			a96t3x6_2ch_diff_getdata(data);
			a96t3x6_check_first_working(data, 2);
		}
#endif
		data->debug_count++;
	}

#if defined(CONFIG_CCIC_NOTIFIER) && defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	if (!data->manager_notifier_registered) {
		int ret = manager_notifier_register(&data->cpuidle_ccic_nb,
						    a96t3x6_ccic_handle_notification,
						    MANAGER_NOTIFY_CCIC_SENSORHUB);
		GRIP_INFO("notifier register ret = %d\n", ret);
		if (ret >= 0)
			data->manager_notifier_registered = true;
	}
#endif
	schedule_delayed_work(&data->debug_work, msecs_to_jiffies(2000));
}

static irqreturn_t a96t3x6_interrupt(int irq, void *dev_id)
{
	struct a96t3x6_data *data = dev_id;
	struct i2c_client *client = data->client;
	int ret, retry;
	u8 buf;
	int grip_data;
	u8 grip_press = 0;
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	int grip_data_2ch;
	u8 grip_press_2ch = 0;
#endif

	wake_lock(&data->grip_wake_lock);

	ret = a96t3x6_i2c_read(client, REG_BTNSTATUS, &buf, 1);
	if (ret < 0) {
		retry = 3;
		while (retry--) {
			GRIP_INFO("read fail(%d)\n", retry);
			ret = a96t3x6_i2c_read(client, REG_BTNSTATUS, &buf, 1);
			if (ret == 0)
				break;
			usleep_range(10000, 11000);
		}
		if (retry < 0) {
			a96t3x6_reset(data);
			wake_unlock(&data->grip_wake_lock);
			return IRQ_HANDLED;
		}
	}

	GRIP_DEBUG("buf = 0x%02x\n", buf);

	grip_data = (buf >> 4) & 0x03;
	grip_press = !(grip_data % 2);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	grip_data_2ch = buf & 0x03;
	grip_press_2ch = !(grip_data_2ch % 2);
#endif

	if (grip_data) {
		if (data->skip_event) {
			GRIP_DEBUG("int was generated, but event skipped\n");
		} else {
			if (grip_press) {
				input_report_rel(data->input_dev, REL_MISC, 1);
				if (data->is_unknown_mode == UNKNOWN_ON && data->motion)
					data->first_working = true;
			} else {
				input_report_rel(data->input_dev, REL_MISC, 2);
				if (data->is_unknown_mode == UNKNOWN_ON && data->motion) {
					if (data->first_working) {
						GRIP_DEBUG("unknown mode off\n");
						data->is_unknown_mode = UNKNOWN_OFF;
					}
				}
			}

			input_report_rel(data->input_dev, REL_X, data->is_unknown_mode);
			input_sync(data->input_dev);
			data->grip_event = grip_press;
		}
	}
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	if (grip_data_2ch) {
		if (data->skip_event) {
			GRIP_DEBUG("2ch int was generated, but event skipped\n");
		} else {
			if (grip_press_2ch) {
				input_report_rel(data->input_dev, REL_DIAL, 1);
				if (data->is_unknown_mode_2ch == UNKNOWN_ON && data->motion)
					data->first_working_2ch = true;
			} else {
				input_report_rel(data->input_dev, REL_DIAL, 2);
				if (data->is_unknown_mode_2ch == UNKNOWN_ON && data->motion) {
					if (data->first_working_2ch) {
						GRIP_DEBUG("2ch unknown mode off\n");
						data->is_unknown_mode_2ch = UNKNOWN_OFF;
					}
				}
			}

			input_report_rel(data->input_dev, REL_Y, data->is_unknown_mode_2ch);
			input_sync(data->input_dev);
			data->grip_event_2ch = grip_press_2ch;
		}
	}
#endif
	a96t3x6_diff_getdata(data, 0);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	a96t3x6_2ch_diff_getdata(data);
#endif

#ifdef CONFIG_SEC_FACTORY
	if (data->abnormal_mode) {
		if (data->grip_event) {
			if (data->max_diff < data->diff)
				data->max_diff = data->diff;
			data->irq_count++;
		}
#ifdef CONFIG_SENSORS_A96T3X6_2CH
		if (data->grip_event_2ch) {
			if (data->max_diff_2ch < data->diff_2ch)
				data->max_diff_2ch = data->diff_2ch;
			data->irq_count++;
		}
#endif
	}
#endif
	if (grip_data)
		GRIP_INFO("%s %x\n", grip_press ? "grip P" : "grip R", buf);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	if (grip_data_2ch)
		GRIP_INFO("2ch %s %x\n", grip_press_2ch ? "grip P" : "grip R", buf);
#endif
	wake_unlock(&data->grip_wake_lock);
	return IRQ_HANDLED;
}

static int a96t3x6_get_raw_data(struct a96t3x6_data *data)
{
	int ret;
	u8 r_buf[4] = {0,};

	ret = a96t3x6_i2c_read(data->client, REG_SAR_RAWDATA, r_buf, 4);
	if (ret < 0) {
		GRIP_INFO("fail(%d)\n", ret);
		data->grip_raw = 0;
		data->grip_raw_d = 0;
		return ret;
	}

	data->grip_raw = (r_buf[0] << 8) | r_buf[1];
	data->grip_raw_d = (r_buf[2] << 8) | r_buf[3];

	GRIP_INFO("grip_raw = %d\n", data->grip_raw);
	return ret;
}

#ifdef CONFIG_SENSORS_A96T3X6_2CH
static int a96t3x6_get_2ch_raw_data(struct a96t3x6_data *data)
{
	int ret;
	u8 r_buf[4] = {0,};

	ret = a96t3x6_i2c_read(data->client, REG_SAR_RAWDATA_2CH, r_buf, 4);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		data->grip_raw_2ch = 0;
		data->grip_raw_d_2ch = 0;
		return ret;
	}

	data->grip_raw_2ch = (r_buf[0] << 8) | r_buf[1];
	data->grip_raw_d_2ch = (r_buf[2] << 8) | r_buf[3];

	GRIP_INFO("2ch grip_raw = %d\n", data->grip_raw_2ch);
	return ret;
}
#endif

/* Firmware Management */
static int a96t3x6_get_fw_version(struct a96t3x6_data *data, bool bootmode)
{
	struct i2c_client *client = data->client;
	u8 buf;
	int ret;
	int retry = 3;

	grip_always_active(data, 1);

	ret = a96t3x6_i2c_read(client, REG_FW_VER, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			GRIP_ERR("read fail(%d)\n", retry);
			if (!bootmode)
				a96t3x6_reset(data);
			else
				goto err_grip_revert_mode;
			ret = a96t3x6_i2c_read(client, REG_FW_VER, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry <= 0)
			goto err_grip_revert_mode;
	}
	data->fw_ver = buf;

	retry = 3;
	ret = a96t3x6_i2c_read(client, REG_MODEL_NO, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			GRIP_ERR("read fail(%d)\n", retry);
			if (!bootmode)
				a96t3x6_reset(data);
			else
				goto err_grip_revert_mode;
			ret = a96t3x6_i2c_read(client, REG_MODEL_NO, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry <= 0)
			goto err_grip_revert_mode;
	}
	data->md_ver = buf;

	GRIP_INFO(" fw = 0x%x, md = 0x%x\n", data->fw_ver, data->md_ver);
	grip_always_active(data, 0);
	return 0;

err_grip_revert_mode:
	grip_always_active(data, 0);
	return -EIO;
}

static int a96t3x6_load_fw_kernel(struct a96t3x6_data *data)
{
	int ret = 0;

	ret = request_firmware(&data->firm_data_bin,
			       data->fw_path, &data->client->dev);
	if (ret) {
		GRIP_ERR("request_firmware fail.\n");
		return ret;
	}
	data->firm_size = data->firm_data_bin->size;
	if (data->firm_size < 10) {
		GRIP_ERR("firmware too small: %ld\n", data->firm_size);
		release_firmware(data->firm_data_bin);
		data->firm_data_bin = NULL;
		return -EINVAL;
	}
	data->fw_ver_bin = data->firm_data_bin->data[5];
	data->md_ver_bin = data->firm_data_bin->data[1];
	GRIP_INFO("fw = 0x%x, md = 0x%x\n", data->fw_ver_bin, data->md_ver_bin);

	data->checksum_h_bin = data->firm_data_bin->data[8];
	data->checksum_l_bin = data->firm_data_bin->data[9];

	GRIP_INFO("crc 0x%x 0x%x\n", data->checksum_h_bin, data->checksum_l_bin);
	return 0;
}

static int a96t3x6_load_fw(struct a96t3x6_data *data, u8 cmd)
{
	struct file *fp;
	mm_segment_t old_fs;
	long fsize, nread;
	int ret = 0;

	switch (cmd) {
	case BUILT_IN:
		break;

	case SDCARD:
		old_fs = get_fs();
		set_fs(get_ds());
		fp = filp_open(TK_FW_PATH_SDCARD, O_RDONLY, 0400);
		if (IS_ERR(fp)) {
			GRIP_ERR("%s open error (%d)\n", TK_FW_PATH_SDCARD, (int)PTR_ERR(fp));
			ret = -ENOENT;
			goto fail_sdcard_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;
		data->firm_data_ums = kzalloc((size_t)fsize, GFP_KERNEL);
		if (!data->firm_data_ums) {
			GRIP_ERR("fail to kzalloc for fw\n");
			ret = -ENOMEM;
			goto fail_sdcard_kzalloc;
		}

		nread = vfs_read(fp,
				 (char __user *)data->firm_data_ums, fsize, &fp->f_pos);
		if (nread != fsize) {
			GRIP_ERR("fail to vfs_read file\n");
			ret = -EINVAL;
			goto fail_sdcard_size;
		}
		if (fsize < 10) {
			GRIP_ERR("sdcard firmware too small: %ld\n", fsize);
			ret = -EINVAL;
			goto fail_sdcard_size;
		}
		filp_close(fp, current->files);
		set_fs(old_fs);
		data->firm_size = nread;
		break;

	default:
		ret = -EINVAL;
		break;
	}
	GRIP_INFO("fw_size : %lu, success (%u)\n", data->firm_size, cmd);
	return ret;

fail_sdcard_size:
	kfree(data->firm_data_ums);
	data->firm_data_ums = NULL;
fail_sdcard_kzalloc:
	filp_close(fp, current->files);
fail_sdcard_open:
	set_fs(old_fs);
	return ret;
}

static void a96t3x6_release_fw(struct a96t3x6_data *data, u8 cmd)
{
	switch (cmd) {
	case BUILT_IN:
		if (data->firm_data_bin) {
			release_firmware(data->firm_data_bin);
			data->firm_data_bin = NULL;
		}
		break;
	case SDCARD:
		kfree(data->firm_data_ums);
		data->firm_data_ums = NULL;
		break;
	default:
		break;
	}
}

static int a96t3x6_check_busy(struct a96t3x6_data *data)
{
	int ret, count = 0;
	unsigned char val = 0x00;

	do {
		ret = i2c_master_recv(data->client, &val, sizeof(val));
		if (val)
			count++;
		else
			break;

		if (count > 1000)
			break;
	} while (1);

	if (count > 1000)
		GRIP_ERR("busy %d\n", count);
	return ret;
}

static int a96t3x6_i2c_read_checksum(struct a96t3x6_data *data, u8 cmd)
{
	unsigned char buf[6] = {0xAC, 0x9E, 0x00, 0x00, 0x3F, 0xFF};
	unsigned char buf2[1] = {0x00};
	unsigned char checksum[6] = {0, };
	int ret;

#ifdef CONFIG_SENSORS_A96T3X6_RESET
	if (cmd == BUILT_IN) {
		buf[4] = data->checksum_h_bin;
		buf[5] = data->checksum_l_bin;
	} else if (cmd == SDCARD) {
		buf[4] = data->firm_data_ums[8];
		buf[5] = data->firm_data_ums[9];
	}
#endif
	i2c_master_send(data->client, buf, 6);
	usleep_range(5000, 6000);

	i2c_master_send(data->client, buf2, 1);
	usleep_range(5000, 6000);

	ret = a96t3x6_i2c_read_data(data->client, checksum, 6);

	GRIP_INFO("ret:%d [%X][%X][%X][%X][%X][%X]\n", ret,
		  checksum[0], checksum[1], checksum[2], checksum[3], checksum[4], checksum[5]);
	data->checksum_h = checksum[4];
	data->checksum_l = checksum[5];

#ifdef CONFIG_SENSORS_A96T3X6_RESET
	data->fw_write_result = checksum[3];
	GRIP_INFO("Write Result (P:0x5A, F:0x0A) : %X\n", checksum[3]);

	if (checksum[3] != WRITE_RESULT_PASS)
		return -EIO;
#endif
	return 0;
}

static int a96t3x6_fw_write(struct a96t3x6_data *data, unsigned char *addrH,
			    unsigned char *addrL, unsigned char *val)
{
	int length = 36, ret = 0;
	unsigned char buf[36];

	buf[0] = 0xAC;
	buf[1] = 0x7A;
	memcpy(&buf[2], addrH, 1);
	memcpy(&buf[3], addrL, 1);
	memcpy(&buf[4], val, 32);

	ret = i2c_master_send(data->client, buf, length);
	if (ret != length) {
		GRIP_ERR("write fail[%x%x], %d\n", *addrH, *addrL, ret);
		return ret;
	}

	usleep_range(3000, 3000);
	a96t3x6_check_busy(data);
	return 0;
}

static int a96t3x6_fw_mode_enter(struct a96t3x6_data *data)
{
	unsigned char buf[2] = {0xAC, 0x5B};
	u8 cmd = 0;
	int ret = 0;

	GRIP_INFO("cmd send\n");
	ret = i2c_master_send(data->client, buf, 2);
	if (ret != 2) {
		GRIP_ERR("write fail\n");
		return -EIO;
	}

	ret = i2c_master_recv(data->client, &cmd, 1);
	GRIP_INFO("cmd receive %2x, %2x\n", data->firmup_cmd, cmd);
	if (data->firmup_cmd != cmd) {
		GRIP_ERR("cmd not matched, firmup fail (ret = %d)\n", ret);
		return -EPROTO;
	}
	return 0;
}

static int a96t3x6_flash_erase(struct a96t3x6_data *data)
{
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	unsigned char buf[2] = {0xAC, 0x2E};
#else
	unsigned char buf[2] = {0xAC, 0x2D};
#endif
	int ret = 0;

	ret = i2c_master_send(data->client, buf, 2);
	if (ret != 2) {
		GRIP_ERR("write fail\n");
		return -EIO;
	}
	return 0;
}

static int a96t3x6_fw_mode_exit(struct a96t3x6_data *data)
{
	unsigned char buf[2] = {0xAC, 0xE1};
	int ret = 0;

	ret = i2c_master_send(data->client, buf, 2);
	if (ret != 2) {
		GRIP_ERR("write fail\n");
		return -EIO;
	}
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	usleep_range(RESET_DELAY, RESET_DELAY);
#endif
	return 0;
}

static int a96t3x6_fw_update(struct a96t3x6_data *data, u8 cmd)
{
	int ret, i = 0;
	int count;
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	int retry = BOOT_ENTER_CHECK_RETRY_COUNT;
	int retry2 = BOOT_ENTER_RETRY_COUNT;
#else
	int retry = 5;
#endif
	unsigned short address;
	unsigned char addrH, addrL;
	unsigned char buf[32] = {0, };

	GRIP_INFO("start\n");

	count = data->firm_size / 32;
	address = USER_CODE_ADDRESS;
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	while (retry2 > 0) {
		a96t3x6_reset_for_bootmode(data);
		retry = 10;
		while (retry > 0) {
			usleep_range(BOOT_DELAY, BOOT_DELAY);

			ret = a96t3x6_fw_mode_enter(data);
			if (ret < 0) {
				GRIP_ERR("a96t3x6_fw_mode_enter fail, retry : %d, %d\n",
					 (BOOT_ENTER_RETRY_COUNT - retry + 1),
					 (BOOT_ENTER_RETRY_COUNT - retry2 + 1));
			} else {
				retry2 = 0;
				retry = 0;
				break;
			}
			retry--;
		}
		retry2--;
	}

	if (ret < 0 && retry2 == 0) {
		GRIP_ERR("a96t3x6_fw_mode_enter fail\n");
		return ret;
	}
#else
	while (retry > 0) {
		a96t3x6_reset_for_bootmode(data);
		usleep_range(BOOT_DELAY, BOOT_DELAY);

		ret = a96t3x6_fw_mode_enter(data);
		if (ret < 0)
			GRIP_ERR("a96t3x6_fw_mode_enter fail, retry : %d\n",
				 ((5 - retry) + 1));
		else
			break;

		retry--;
	}

	if (ret < 0 && retry == 0) {
		GRIP_ERR("a96t3x6_fw_mode_enter fail\n");
		return ret;
	}
#endif
	usleep_range(5000, 5000);
	GRIP_INFO("fw_mode_cmd sent\n");

	ret = a96t3x6_flash_erase(data);
	usleep_range(FLASH_DELAY, FLASH_DELAY);

	GRIP_INFO("fw_write start\n");
	for (i = 1; i < count; i++) {
		addrH = (unsigned char)((address >> 8) & 0xFF);
		addrL = (unsigned char)(address & 0xFF);
		if (cmd == BUILT_IN)
			memcpy(buf, &data->firm_data_bin->data[i * 32], 32);
		else if (cmd == SDCARD)
			memcpy(buf, &data->firm_data_ums[i * 32], 32);

		ret = a96t3x6_fw_write(data, &addrH, &addrL, buf);
		if (ret < 0) {
			GRIP_ERR("err, no device : %d\n", ret);
			return ret;
		}

		address += 0x20;
		memset(buf, 0, 32);
	}

	ret = a96t3x6_i2c_read_checksum(data, cmd);
	GRIP_INFO("checksum read%d\n", ret);

	ret = a96t3x6_fw_mode_exit(data);
	GRIP_INFO("fw_write end\n");
	return ret;
}

static int a96t3x6_flash_fw(struct a96t3x6_data *data, bool probe, u8 cmd)
{
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	int retry = 10;
#else
	int retry = 2;
#endif
	int ret;

	ret = a96t3x6_get_fw_version(data, probe);
	if (ret)
		data->fw_ver = 0;

	ret = a96t3x6_load_fw(data, cmd);
	if (ret) {
		GRIP_ERR("fw load fail\n");
		return ret;
	}

	while (retry--) {
		ret = a96t3x6_fw_update(data, cmd);
		if (ret < 0)
			break;
#ifdef CONFIG_SENSORS_A96T3X6_RESET
		if (data->fw_write_result != WRITE_RESULT_PASS) {
			GRIP_INFO("fw write fail\n");
			ret = -EIO;
			continue;
		}
#endif
		if (cmd == BUILT_IN) {
			if ((data->checksum_h != data->checksum_h_bin) ||
			    (data->checksum_l != data->checksum_l_bin)) {
				GRIP_ERR("checksum fail.(0x%x,0x%x),(0x%x,0x%x) retry:%d\n",
					 data->checksum_h, data->checksum_l,
					 data->checksum_h_bin, data->checksum_l_bin, retry);
				ret = -EIO;
				continue;
			}
		}
#ifdef CONFIG_SENSORS_A96T3X6_RESET
		else {
			if ((data->checksum_h != data->firm_data_ums[8]) ||
			    (data->checksum_l != data->firm_data_ums[9])) {
				GRIP_ERR("(SD card) checksum fail.(0x%x,0x%x),(0x%x,0x%x) retry:%d\n",
					 data->checksum_h, data->checksum_l,
					 data->firm_data_ums[8], data->firm_data_ums[9],
					 retry);
				ret = -EIO;
				continue;
			}
		}
#endif
		a96t3x6_reset_for_bootmode(data);
		usleep_range(RESET_DELAY, RESET_DELAY);

		ret = a96t3x6_get_fw_version(data, true);
		if (ret) {
			GRIP_ERR("fw version read fail\n");
			ret = -EIO;
			continue;
		}

		if (data->fw_ver == 0) {
			GRIP_ERR("fw version fail (0x%x)\n", data->fw_ver);
			ret = -EIO;
			continue;
		}

		if ((cmd == BUILT_IN) && (data->fw_ver != data->fw_ver_bin)) {
			GRIP_ERR("fw version fail 0x%x, 0x%x\n",
				 data->fw_ver, data->fw_ver_bin);
			ret = -EIO;
			continue;
		}
		ret = 0;
		break;
	}

	a96t3x6_release_fw(data, cmd);
	return ret;
}

static int a96t3x6_fw_check(struct a96t3x6_data *data)
{
	int ret;
	int fw_up = 0;
	u8 r_buf[4] = {0,};

	if (data->bringup) {
		GRIP_INFO("bring up mode. skip firmware check\n");
		return 0;
	}

	ret = a96t3x6_load_fw_kernel(data);
#ifdef CONFIG_SENSORS_FW_VENDOR
	if (ret) {
		GRIP_ERR("fw was not loaded yet from ueventd\n",
			 data->md_ver, data->md_ver_bin);
		return ret;
	}
#elif defined(CONFIG_SEC_SENSORS_ENG_DEBUG)
	if (ret)
		panic("grip fw doesn't exist\n");
#endif
	ret = a96t3x6_get_fw_version(data, true);
	if (!ret) {
		if (data->fw_ver < data->fw_ver_bin)
			fw_up |= 0x01;

		if (data->fw_ver > TEST_FIRMWARE_DETECT_VER)
			fw_up |= 0x02;
	}

	if (data->md_ver != data->md_ver_bin) {
		GRIP_ERR("MD version is different.(IC %x, BN %x). Do force FW update\n",
			 data->md_ver, data->md_ver_bin);
		fw_up |= 0x04;
	}
#if defined(CONFIG_SENSORS_A96T3X6_FORCE_FW_UPDATE) && defined(CONFIG_SEC_FACTORY)
	GRIP_INFO("fw force update for abnormal fw devices\n");
	fw_up |= 0x08;
#endif
	if (fw_up) {
		GRIP_INFO("fw update (0x%x -> 0x%x) request = %d\n",
			  data->fw_ver, data->fw_ver_bin, fw_up);
		ret = a96t3x6_flash_fw(data, true, BUILT_IN);
		if (ret)
			GRIP_ERR("failed to a96t3x6_flash_fw (%d)\n", ret);
		else
			GRIP_INFO("fw update success\n");
	} else {
		/*
		 * Firmware is already current, so a96t3x6_flash_fw() (which is
		 * what normally calls a96t3x6_release_fw()) is skipped; release
		 * the blob loaded by a96t3x6_load_fw_kernel() above.
		 */
		a96t3x6_release_fw(data, BUILT_IN);
	}

	ret = a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD, r_buf, 4);
	if (ret < 0) {
		GRIP_ERR("fail(%d)\n", ret);
		data->grip_p_thd = 0;
		data->grip_r_thd = 0;
	}
	data->grip_p_thd = (r_buf[0] << 8) | r_buf[1];
	data->grip_r_thd = (r_buf[2] << 8) | r_buf[3];

	return ret;
}

/*
 * Table-driven sysfs infrastructure
 */
enum a96t3x6_attr_id {
	ATTR_THRESHOLD,
	ATTR_TOTAL_CAP,
	ATTR_SAR_ENABLE,
	ATTR_SW_RESET,
	ATTR_SW_RESET_READY,
	ATTR_EARJACK,
	ATTR_GRIP,
	ATTR_BASELINE,
	ATTR_RAW,
	ATTR_REF_CAP,
	ATTR_GAIN,
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	ATTR_GAIN_2CH,
#endif
	ATTR_CHECK,
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	ATTR_SAR_ONLY_MODE,
	ATTR_SAR_PRESS_THRESHOLD,
	ATTR_SAR_RELEASE_THRESHOLD,
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	ATTR_SAR_PRESS_THRESHOLD_2CH,
	ATTR_SAR_RELEASE_THRESHOLD_2CH,
#endif
#endif
#ifdef CONFIG_SEC_FACTORY
	ATTR_IRQ_COUNT,
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	ATTR_IRQ_COUNT_2CH,
#endif
#endif
	ATTR_NAME,
	ATTR_VENDOR,
	ATTR_FIRM_VERSION_PHONE,
	ATTR_FIRM_VERSION_PANEL,
	ATTR_FIRM_UPDATE,
	ATTR_FIRM_UPDATE_STATUS,
	ATTR_IRQ_STATE,
	ATTR_IRQ_EN_CNT,
	ATTR_REG_RW,
	ATTR_CRC_CHECK,
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	ATTR_CH_COUNT,
	ATTR_THRESHOLD_2CH,
	ATTR_TOTAL_CAP_2CH,
	ATTR_GRIP_2CH,
	ATTR_BASELINE_2CH,
	ATTR_RAW_2CH,
	ATTR_CHECK_2CH,
	ATTR_UNKNOWN_STATE_2CH,
#endif
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	ATTR_MCC,
#endif
#ifdef CONFIG_SENSOR_A96T3X6_LDO_SHARE
	ATTR_REGISTER_RECOVER,
#endif
	ATTR_MOTION,
	ATTR_UNKNOWN_STATE,
	ATTR_NOTI_ENABLE,
#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	ATTR_COUNTRY_CODE,
#endif
	ATTR_ENABLE,
};

struct a96t3x6_attribute {
	struct device_attribute dev_attr;
	enum a96t3x6_attr_id id;
};

#define to_a96t3x6_attr(_dev_attr) \
	container_of(_dev_attr, struct a96t3x6_attribute, dev_attr)

#define A96T3X6_ATTR_RO(_name, _id) \
	{ .dev_attr = __ATTR(_name, 0444, a96t3x6_attr_show, NULL), .id = _id }

#define A96T3X6_ATTR_WO(_name, _mode, _id) \
	{ .dev_attr = __ATTR(_name, _mode, NULL, a96t3x6_attr_store), .id = _id }

#define A96T3X6_ATTR_RW(_name, _mode, _id) \
	{ .dev_attr = __ATTR(_name, _mode, a96t3x6_attr_show, a96t3x6_attr_store), .id = _id }

static ssize_t a96t3x6_crc_check(struct a96t3x6_data *data, char *buf)
{
	int ret;
#ifndef CONFIG_SENSORS_A96T3X6_CRC_CHECK
	unsigned char cmd[3] = {0x1B, 0x00, 0x10};
	unsigned char checksum[2] = {0, };
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	unsigned char checksumReset[2] = {0, };
	unsigned int checksumTemp = 0;
#endif

	i2c_master_send(data->client, cmd, 3);
	usleep_range(50 * 1000, 50 * 1000);

	ret = a96t3x6_i2c_read(data->client, 0x1B, checksum, 2);
	if (ret < 0) {
		GRIP_ERR("i2c read fail\n");
		return snprintf(buf, PAGE_SIZE, "NG,0000\n");
	}
#ifdef CONFIG_SENSORS_A96T3X6_RESET
	checksumTemp = ((checksum[0] << 8) & 0xFF00) + (checksum[1] & 0x00FF);
	checksumTemp = checksumTemp - 0x5A;

	checksumReset[0] = (unsigned char)((checksumTemp >> 8) & 0xFF);
	checksumReset[1] = (unsigned char)(checksumTemp & 0xFF);

	GRIP_INFO("CRC:%02x%02x, BIN:%02x%02x\n", checksumReset[0], checksumReset[1],
		  data->checksum_h_bin, data->checksum_l_bin);

	if ((checksumReset[0] == data->checksum_h_bin) &&
	    (checksumReset[1] == data->checksum_l_bin)) {
		return snprintf(buf, PAGE_SIZE, "OK,%02x%02x\n",
				checksumReset[0], checksumReset[1]);
	}
	return snprintf(buf, PAGE_SIZE, "NG,%02x%02x\n",
			checksumReset[0], checksumReset[1]);
#else
	GRIP_INFO("CRC:%02x%02x, BIN:%02x%02x\n", checksum[0], checksum[1],
		  data->checksum_h_bin, data->checksum_l_bin);

	if ((checksum[0] != data->checksum_h_bin) ||
	    (checksum[1] != data->checksum_l_bin))
		return snprintf(buf, PAGE_SIZE, "NG,%02x%02x\n",
				checksum[0], checksum[1]);
	return snprintf(buf, PAGE_SIZE, "OK,%02x%02x\n",
			checksum[0], checksum[1]);
#endif
#else
	unsigned char cmd = 0xAA;
	unsigned char val = 0xFF;
	unsigned char crc_check = CRC_FAIL;
	unsigned char retry = 10;

	grip_always_active(data, 1);

	ret = a96t3x6_i2c_write(data->client, REG_FW_VER, &cmd);
	if (ret < 0)
		GRIP_ERR("crc checking enter failed\n");

	while (retry--) {
		msleep(400);

		ret = a96t3x6_i2c_read(data->client, REG_FW_VER, &val, 1);
		if (ret < 0)
			GRIP_ERR("crc read failed\n");

		GRIP_INFO("crc check value = 0x%2x\n", val);

		if (val == 0x00) {
			GRIP_ERR("crc check fail\n");
		} else {
			GRIP_INFO("crc check normal\n");
			crc_check = CRC_PASS;
			break;
		}
	}
	grip_always_active(data, 0);

	if (crc_check == CRC_PASS)
		return snprintf(buf, PAGE_SIZE, "OK,%02x\n", val);
	return snprintf(buf, PAGE_SIZE, "NG,%02x\n", val);
#endif
}

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
static int a96t3x6_store_threshold_reg(struct a96t3x6_data *data,
				       const char *buf, u8 reg)
{
	int ret, threshold;
	u8 cmd[2];

	ret = sscanf(buf, "%11d", &threshold);
	if (ret != 1) {
		GRIP_ERR("failed to read thresold, buf is %s\n", buf);
		return -EINVAL;
	}

	if (threshold > 0xff) {
		cmd[0] = (threshold >> 8) & 0xff;
		cmd[1] = 0xff & threshold;
	} else if (threshold < 0) {
		cmd[0] = 0x0;
		cmd[1] = 0x0;
	} else {
		cmd[0] = 0x0;
		cmd[1] = (u8)threshold;
	}

	GRIP_INFO("buf : %d, threshold : %d\n", threshold,
		  (cmd[0] << 8) | cmd[1]);

	ret = a96t3x6_i2c_write(data->client, reg, &cmd[0]);
	if (ret != 0) {
		GRIP_INFO("failed to write press_threhold data1");
		return ret;
	}
	ret = a96t3x6_i2c_write(data->client, reg + 0x01, &cmd[1]);
	if (ret != 0) {
		GRIP_INFO("failed to write press_threhold data2");
		return ret;
	}
	return 0;
}
#endif

static ssize_t a96t3x6_attr_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct a96t3x6_attribute *aattr = to_a96t3x6_attr(attr);
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	u8 r_buf[4];
	int ret;

	if (!data)
		return -ENODEV;

	switch (aattr->id) {
	case ATTR_THRESHOLD:
#ifdef CONFIG_SENSORS_A96T3X6_2CH
		ret = a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_p_thd = 0;
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		data->grip_p_thd = (r_buf[0] << 8) | r_buf[1];

		ret = a96t3x6_i2c_read(data->client, REG_SAR_RELEASE_THRESHOLD, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_r_thd = 0;
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		data->grip_r_thd = (r_buf[0] << 8) | r_buf[1];
#else
		ret = a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD, r_buf, 4);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_p_thd = 0;
			data->grip_r_thd = 0;
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		data->grip_p_thd = (r_buf[0] << 8) | r_buf[1];
		data->grip_r_thd = (r_buf[2] << 8) | r_buf[3];
#endif
		ret = a96t3x6_i2c_read(data->client, REG_SAR_NOISE_THRESHOLD, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_n_thd = 0;
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		data->grip_n_thd = (r_buf[0] << 8) | r_buf[1];
		return snprintf(buf, PAGE_SIZE, "%u,%u,%u\n",
			       data->grip_p_thd, data->grip_r_thd, data->grip_n_thd);

	case ATTR_TOTAL_CAP: {
		u8 cmd = 0x20;
		int value;

		ret = a96t3x6_i2c_write(data->client, REG_SAR_TOTALCAP, &cmd);
		if (ret < 0)
			GRIP_ERR("write fail(%d)\n", ret);

		usleep_range(10, 20);

		ret = a96t3x6_i2c_read(data->client, REG_SAR_TOTALCAP_READ, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		value = (r_buf[0] << 8) | r_buf[1];
		GRIP_INFO("grip total cap : %d\n", value);
		value /= 100;
		return snprintf(buf, PAGE_SIZE, "%d\n", value);
	}

	case ATTR_SAR_ENABLE:
		return snprintf(buf, PAGE_SIZE, "%u\n", !data->skip_event);

	case ATTR_ENABLE:
		return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&data->enable));

	case ATTR_SW_RESET_READY: {
		int retry = 10;
		u8 sw_buf[1] = {0};

		GRIP_INFO("Wait start\n");
		msleep(500);

		while (retry--) {
			ret = a96t3x6_i2c_read(data->client, REG_SW_RESET, sw_buf, 1);
			if (sw_buf[0] == 0x20)
				break;
			if (ret < 0)
				GRIP_ERR("failed(%d)\n", retry);
			msleep(100);
		}
		GRIP_INFO("expect 0x20 read 0x%x\n", sw_buf[0]);
		a96t3x6_check_diff_and_cap(data);
		return snprintf(buf, PAGE_SIZE, "1\n");
	}

	case ATTR_GRIP: {
		int retry = 3;

		memset(r_buf, 0, sizeof(r_buf));
		while (retry--) {
			ret = a96t3x6_i2c_read(data->client, REG_SAR_DIFFDATA, r_buf, 4);
			if (ret == 0)
				break;
			GRIP_ERR("read failed(%d)\n", retry);
			usleep_range(10000, 10000);
		}
		data->diff = (r_buf[0] << 8) | r_buf[1];
		data->diff_d = (r_buf[2] << 8) | r_buf[3];
		return snprintf(buf, PAGE_SIZE, "%u,%u\n", data->diff, data->diff_d);
	}

	case ATTR_BASELINE:
		ret = a96t3x6_i2c_read(data->client, REG_SAR_BASELINE, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_baseline = 0;
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		}
		data->grip_baseline = (r_buf[0] << 8) | r_buf[1];
#ifdef DEBUG_FACTORY_APP_ENABLE
		return snprintf(buf, PAGE_SIZE, "%u/%s\n", data->grip_baseline,
				(data->motion) ? "MOTION_DETECT" : "MOTION_NON_DETECT");
#else
		return snprintf(buf, PAGE_SIZE, "%u\n", data->grip_baseline);
#endif

	case ATTR_RAW:
		ret = a96t3x6_get_raw_data(data);
		if (ret < 0)
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
#ifdef DEBUG_FACTORY_APP_ENABLE
		return snprintf(buf, PAGE_SIZE, "%u/%s,%u\n", data->grip_raw,
				(data->is_unknown_mode == 1) ? "UNKNOWN" : "NORMAL",
				data->grip_raw_d);
#else
		return snprintf(buf, PAGE_SIZE, "%u,%u\n", data->grip_raw, data->grip_raw_d);
#endif

	case ATTR_REF_CAP: {
		int ref_cap;

		ret = a96t3x6_i2c_read(data->client, REG_REF_CAP, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		}
		ref_cap = (r_buf[0] << 8) | r_buf[1];
		ref_cap /= 100;
		GRIP_INFO("Ref Cap : %x,%x\n", r_buf[0], r_buf[1]);
		GRIP_INFO("Ref Cap / 100 : %d\n", ref_cap);
		return snprintf(buf, PAGE_SIZE, "%d\n", ref_cap);
	}

	case ATTR_GAIN: {
		u8 r_buf1[3], r_buf2[3];

		ret = a96t3x6_i2c_read(data->client, REG_GAINDATA, r_buf1, 3);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		}
		GRIP_ERR("Gain : %d,%d,%d\n", (int)r_buf1[0], (int)r_buf1[1], (int)r_buf1[2]);

		ret = a96t3x6_i2c_read(data->client, REG_REF_GAINDATA, r_buf2, 3);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		}
		GRIP_ERR("Ref Gain : %d,%d,%d\n", (int)r_buf2[0], (int)r_buf2[1], (int)r_buf2[2]);
		return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d,%d,%d\n",
				(int)r_buf1[0], (int)r_buf1[1], (int)r_buf1[2],
				(int)r_buf2[0], (int)r_buf2[1], (int)r_buf2[2]);
	}

#ifdef CONFIG_SENSORS_A96T3X6_2CH
	case ATTR_GAIN_2CH: {
		u8 r_buf1[3], r_buf2[3];

		ret = a96t3x6_i2c_read(data->client, REG_GAINDATA_2CH, r_buf1, 3);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		}
		GRIP_ERR("Gain : %d,%d,%d\n", (int)r_buf1[0], (int)r_buf1[1], (int)r_buf1[2]);

		ret = a96t3x6_i2c_read(data->client, REG_REF_GAINDATA, r_buf2, 3);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		}
		GRIP_ERR("Ref Gain : %d,%d,%d\n", (int)r_buf2[0], (int)r_buf2[1], (int)r_buf2[2]);
		return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d,%d,%d\n",
				(int)r_buf1[0], (int)r_buf1[1], (int)r_buf1[2],
				(int)r_buf2[0], (int)r_buf2[1], (int)r_buf2[2]);
	}
#endif

	case ATTR_CHECK:
		a96t3x6_diff_getdata(data, 1);
		return snprintf(buf, PAGE_SIZE, "%d\n", data->grip_event);

#ifdef CONFIG_SEC_FACTORY
	case ATTR_IRQ_COUNT: {
		int result = data->irq_count ? -1 : 0;
		s16 max_diff_val = data->irq_count ? data->max_diff :
				   data->max_normal_diff;

		return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n",
				result, data->irq_count, max_diff_val);
	}
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	case ATTR_IRQ_COUNT_2CH: {
		int result = data->irq_count ? -1 : 0;
		s16 max_diff_val_2ch = data->irq_count ? data->max_diff_2ch :
				       data->max_normal_diff_2ch;

		return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n",
				result, data->irq_count, max_diff_val_2ch);
	}
#endif
#endif

	case ATTR_NAME:
		return snprintf(buf, PAGE_SIZE, "%s\n", MODEL_NAME);

	case ATTR_VENDOR:
		return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR_NAME);

	case ATTR_FIRM_VERSION_PHONE:
		return snprintf(buf, PAGE_SIZE, "0x%02x%02x\n", data->md_ver_bin, data->fw_ver_bin);

	case ATTR_FIRM_VERSION_PANEL:
		ret = a96t3x6_get_fw_version(data, false);
		if (ret < 0) {
			GRIP_ERR("read fail\n");
			data->fw_ver = 0;
		}
		return snprintf(buf, PAGE_SIZE, "0x%02x%02x\n", data->md_ver, data->fw_ver);

	case ATTR_FIRM_UPDATE_STATUS:
		GRIP_INFO("%d\n", data->fw_update_state);
		if (data->fw_update_state == 0)
			return snprintf(buf, PAGE_SIZE, "PASS\n");
		else if (data->fw_update_state == 1)
			return snprintf(buf, PAGE_SIZE, "Downloading\n");
		else if (data->fw_update_state == 2)
			return snprintf(buf, PAGE_SIZE, "Fail\n");
		return 0;

	case ATTR_IRQ_STATE: {
		int status = gpio_get_value(data->grip_int);

		GRIP_INFO("status=%d\n", status);
		return snprintf(buf, PAGE_SIZE, "%d\n", status);
	}

	case ATTR_IRQ_EN_CNT:
		GRIP_INFO("irq_en_cnt=%d\n", data->irq_en_cnt);
		return snprintf(buf, PAGE_SIZE, "%d\n", data->irq_en_cnt);

	case ATTR_REG_RW: {
		u8 val = 0;
		int offset = 0, i;

		for (i = 0; i < 128; i++) {
			a96t3x6_i2c_read(data->client, i, &val, 1);
			GRIP_INFO("%s: reg=%02X val=%02X\n", __func__, i, val);
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
					   "reg=0x%x val=0x%x\n", i, val);
		}
		return offset;
	}

	case ATTR_CRC_CHECK:
		return a96t3x6_crc_check(data, buf);

#ifdef CONFIG_SENSORS_A96T3X6_2CH
	case ATTR_CH_COUNT:
		return snprintf(buf, PAGE_SIZE, "2\n");

	case ATTR_THRESHOLD_2CH:
		ret = a96t3x6_i2c_read(data->client, REG_SAR_THRESHOLD_2CH, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_p_thd_2ch = 0;
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		data->grip_p_thd_2ch = (r_buf[0] << 8) | r_buf[1];

		ret = a96t3x6_i2c_read(data->client, REG_SAR_RELEASE_THRESHOLD_2CH, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_r_thd_2ch = 0;
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		data->grip_r_thd_2ch = (r_buf[0] << 8) | r_buf[1];

		ret = a96t3x6_i2c_read(data->client, REG_SAR_NOISE_THRESHOLD_2CH, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_n_thd_2ch = 0;
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		data->grip_n_thd_2ch = (r_buf[0] << 8) | r_buf[1];
		return snprintf(buf, PAGE_SIZE, "%u,%u,%u\n",
			       data->grip_p_thd_2ch, data->grip_r_thd_2ch,
			       data->grip_n_thd_2ch);

	case ATTR_TOTAL_CAP_2CH: {
		u8 cmd = 0x20;
		int value;

		ret = a96t3x6_i2c_write(data->client, REG_SAR_TOTALCAP, &cmd);
		if (ret < 0)
			GRIP_ERR("write fail(%d)\n", ret);

		usleep_range(10, 20);

		ret = a96t3x6_i2c_read(data->client, REG_SAR_TOTALCAP_READ_2CH, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			return snprintf(buf, PAGE_SIZE, "%u\n", 0);
		}
		value = (r_buf[0] << 8) | r_buf[1];
		GRIP_INFO("grip 2ch total cap : %d\n", value);
		value /= 100;
		return snprintf(buf, PAGE_SIZE, "%d\n", value);
	}

	case ATTR_GRIP_2CH: {
		int retry = 3;

		memset(r_buf, 0, sizeof(r_buf));
		while (retry--) {
			ret = a96t3x6_i2c_read(data->client, REG_SAR_DIFFDATA_D_2CH, r_buf, 4);
			if (ret == 0)
				break;
			GRIP_ERR("read failed(%d)\n", retry);
			usleep_range(10000, 10000);
		}
		data->diff_2ch = (r_buf[0] << 8) | r_buf[1];
		data->diff_d_2ch = (r_buf[2] << 8) | r_buf[3];
		return snprintf(buf, PAGE_SIZE, "%u,%u\n", data->diff_2ch, data->diff_d_2ch);
	}

	case ATTR_BASELINE_2CH:
		ret = a96t3x6_i2c_read(data->client, REG_SAR_BASELINE_2CH, r_buf, 2);
		if (ret < 0) {
			GRIP_ERR("fail(%d)\n", ret);
			data->grip_baseline_2ch = 0;
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		}
		data->grip_baseline_2ch = (r_buf[0] << 8) | r_buf[1];
		return snprintf(buf, PAGE_SIZE, "%u\n", data->grip_baseline_2ch);

	case ATTR_RAW_2CH:
		ret = a96t3x6_get_2ch_raw_data(data);
		if (ret < 0)
			return snprintf(buf, PAGE_SIZE, "%d\n", 0);
		return snprintf(buf, PAGE_SIZE, "%u,%u\n",
				data->grip_raw_2ch, data->grip_raw_d_2ch);

	case ATTR_CHECK_2CH:
		a96t3x6_2ch_diff_getdata(data);
		return snprintf(buf, PAGE_SIZE, "%d\n", data->grip_event_2ch);

	case ATTR_UNKNOWN_STATE_2CH:
		return snprintf(buf, PAGE_SIZE, "%s\n",
				(data->is_unknown_mode_2ch == 1) ? "UNKNOWN" : "NORMAL");
#endif

#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	case ATTR_MCC:
		return snprintf(buf, PAGE_SIZE, "%d\n", data->mcc);
#endif

	case ATTR_MOTION:
		if (data->motion)
			return snprintf(buf, PAGE_SIZE, "motion_detect\n");
		return snprintf(buf, PAGE_SIZE, "motion_non_detect\n");

	case ATTR_UNKNOWN_STATE:
		return snprintf(buf, PAGE_SIZE, "%s\n",
				(data->is_unknown_mode == 1) ? "UNKNOWN" : "NORMAL");

	case ATTR_NOTI_ENABLE:
		GRIP_INFO("noti_enable = %d\n", data->noti_enable);
		return snprintf(buf, PAGE_SIZE, "%d\n", data->noti_enable);

#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	case ATTR_COUNTRY_CODE:
		GRIP_INFO("%s - country_code = %s\n", __func__,
			  data->country_code == 1 ? "KOR" : "ETC(EUR,JPN,CHN)");
		return snprintf(buf, PAGE_SIZE, "%d\n", data->country_code);
#endif

	default:
		return -EINVAL;
	}
}

static ssize_t a96t3x6_attr_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct a96t3x6_attribute *aattr = to_a96t3x6_attr(attr);
	struct a96t3x6_data *data = dev_get_drvdata(dev);
	int ret;

	if (!data)
		return -ENODEV;

	switch (aattr->id) {
	case ATTR_SAR_ENABLE:
	case ATTR_ENABLE: {
		int enable;

		ret = sscanf(buf, "%2d", &enable);
		if (ret != 1) {
			GRIP_ERR("cmd read err\n");
			return count;
		}

		if (!(enable >= 0 && enable <= 3)) {
			GRIP_ERR("wrong command(%d)\n", enable);
			return count;
		}

		GRIP_INFO("enable = %d\n", enable);

		if (enable == 2) {
			data->skip_event = true;
			data->motion = 1;
			data->is_unknown_mode = UNKNOWN_OFF;
			data->first_working = false;

			input_report_rel(data->input_dev, REL_MISC, 2);
			input_report_rel(data->input_dev, REL_X, UNKNOWN_OFF);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
			data->is_unknown_mode_2ch = UNKNOWN_OFF;
			data->first_working_2ch = false;
			input_report_rel(data->input_dev, REL_DIAL, 2);
			input_report_rel(data->input_dev, REL_Y, UNKNOWN_OFF);
#endif
			input_sync(data->input_dev);
		} else if (enable == 3) {
			data->skip_event = false;
		} else {
			a96t3x6_set_enable(data, enable);
		}
		return count;
	}

	case ATTR_SW_RESET: {
		u8 cmd;

		ret = kstrtou8(buf, 2, &cmd);
		if (ret) {
			GRIP_ERR("cmd read err\n");
			return count;
		}

		if (cmd != 1) {
			GRIP_ERR("wrong command(%d)\n", cmd);
			return count;
		}

		data->grip_event = 0;
#ifdef CONFIG_SENSORS_A96T3X6_2CH
		data->grip_event_2ch = 0;
#endif
		GRIP_INFO("cmd(%d)\n", cmd);
		a96t3x6_grip_sw_reset(data);
		return count;
	}

	case ATTR_EARJACK: {
		int earjack;

		ret = sscanf(buf, "%2d", &earjack);
		if (ret != 1) {
			GRIP_ERR("cmd read err\n");
			return count;
		}

		if (!(earjack == 0 || earjack == 1)) {
			GRIP_ERR("wrong command(%d)\n", earjack);
			return count;
		}

		if (!data->earjack_noise) {
			if (earjack == 1)
				a96t3x6_sar_only_mode(data, 1);
			else
				a96t3x6_sar_only_mode(data, 0);
		} else {
			if (earjack == 1) {
				a96t3x6_set_enable(data, 0);
				a96t3x6_sar_sensing(data, 0);
				data->grip_event = 0;
				input_report_rel(data->input_dev, REL_MISC, 2);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
				data->grip_event_2ch = 0;
				input_report_rel(data->input_dev, REL_DIAL, 2);
#endif
				if (data->skip_event)
					input_report_rel(data->input_dev, REL_X, UNKNOWN_OFF);
				else
					input_report_rel(data->input_dev, REL_X,
							 data->is_unknown_mode);
				input_sync(data->input_dev);
			} else {
				a96t3x6_grip_sw_reset(data);
				a96t3x6_sar_sensing(data, 1);
				a96t3x6_set_enable(data, 1);
			}
		}

		data->earjack = earjack;
		GRIP_INFO("earjack was %s\n", (earjack) ? "inserted" : "removed");
		return count;
	}

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	case ATTR_SAR_ONLY_MODE: {
		int mode;

		ret = sscanf(buf, "%2d", &mode);
		if (ret != 1 || !(mode == 0 || mode == 1)) {
			GRIP_ERR("wrong command(%d)\n", mode);
			return count;
		}
		GRIP_INFO("mode(%d)\n", mode);
		a96t3x6_sar_only_mode(data, mode);
		return count;
	}

	case ATTR_SAR_PRESS_THRESHOLD:
		a96t3x6_store_threshold_reg(data, buf, REG_SAR_THRESHOLD);
		return count;

	case ATTR_SAR_RELEASE_THRESHOLD:
		a96t3x6_store_threshold_reg(data, buf, REG_SAR_THRESHOLD + 0x02);
		return count;

#ifdef CONFIG_SENSORS_A96T3X6_2CH
	case ATTR_SAR_PRESS_THRESHOLD_2CH:
		a96t3x6_store_threshold_reg(data, buf, REG_SAR_THRESHOLD_2CH);
		return count;

	case ATTR_SAR_RELEASE_THRESHOLD_2CH:
		a96t3x6_store_threshold_reg(data, buf, REG_SAR_THRESHOLD_2CH + 0x02);
		return count;
#endif
#endif

#ifdef CONFIG_SEC_FACTORY
	case ATTR_IRQ_COUNT: {
		u8 onoff;

		ret = kstrtou8(buf, 10, &onoff);
		if (ret < 0) {
			GRIP_ERR("kstrtou8 failed.(%d)\n", ret);
			return count;
		}

		mutex_lock(&data->lock);
		if (onoff == 0) {
			data->abnormal_mode = 0;
		} else if (onoff == 1) {
			data->abnormal_mode = 1;
			data->irq_count = 0;
			data->max_diff = 0;
			data->max_normal_diff = 0;
		} else {
			GRIP_ERR("Invalid value.(%d)\n", onoff);
		}
		mutex_unlock(&data->lock);

		GRIP_INFO("result : %d\n", onoff);
		return count;
	}

#ifdef CONFIG_SENSORS_A96T3X6_2CH
	case ATTR_IRQ_COUNT_2CH: {
		u8 onoff;

		ret = kstrtou8(buf, 10, &onoff);
		if (ret < 0) {
			GRIP_ERR("kstrtou8 failed.(%d)\n", ret);
			return count;
		}

		mutex_lock(&data->lock);
		if (onoff == 0) {
			data->abnormal_mode = 0;
		} else if (onoff == 1) {
			data->abnormal_mode = 1;
			data->irq_count = 0;
			data->max_diff_2ch = 0;
			data->max_normal_diff_2ch = 0;
		} else {
			GRIP_ERR("Invalid value.(%d)\n", onoff);
		}
		mutex_unlock(&data->lock);

		GRIP_INFO("result : %d\n", onoff);
		return count;
	}
#endif
#endif

	case ATTR_FIRM_UPDATE: {
		int enable = atomic_read(&data->enable);
		u8 cmd;

		switch (*buf) {
		case 's':
		case 'S':
			cmd = BUILT_IN;
			break;
		case 'i':
		case 'I':
			cmd = SDCARD;
			break;
		default:
			data->fw_update_state = 2;
			goto fw_update_out;
		}

		data->fw_update_state = 1;
		disable_irq(data->irq);

		if (cmd == BUILT_IN) {
			ret = a96t3x6_load_fw_kernel(data);
			if (ret) {
				GRIP_ERR("failed to load firmware(%d)\n", ret);
				enable_irq(data->irq);
				goto fw_update_out;
			}
			GRIP_INFO("fw version read success (%d)\n", ret);
		}
		ret = a96t3x6_flash_fw(data, false, cmd);

		if (enable) {
			cmd = CMD_ON;
			ret = a96t3x6_i2c_write(data->client, REG_SAR_ENABLE, &cmd);
			if (ret < 0)
				GRIP_INFO("failed to enable grip irq\n");

			a96t3x6_check_first_status(data, 1);
		}

		enable_irq(data->irq);
		if (ret) {
			GRIP_ERR("failed to flash firmware(%d)\n", ret);
			data->fw_update_state = 2;
		} else {
			GRIP_INFO("success\n");
			data->fw_update_state = 0;
		}

#if defined(CONFIG_SENSOR_A96T3X6_LDO_SHARE)
		GRIP_INFO("a96t3x6 register recovery\n");
		input_report_rel(data->input_dev, REL_WHEEL, 1);
		input_sync(data->input_dev);
#endif

fw_update_out:
		GRIP_INFO("fw_update_state = %d\n", data->fw_update_state);
		return count;
	}

	case ATTR_REG_RW: {
		int regist = 0, val = 0;
		u8 cmd = 0;

		if (sscanf(buf, "%4x,%4x", &regist, &val) != 2) {
			GRIP_ERR("%s - The number of data are wrong\n", __func__);
			return -EINVAL;
		}

		GRIP_INFO("reg=0x%2x value=0x%2x\n", regist, val);
		cmd = (u8)val;
		a96t3x6_i2c_write(data->client, (u8)regist, &cmd);
		return count;
	}

#ifdef CONFIG_SENSORS_A96T3X6_2CH
	case ATTR_UNKNOWN_STATE_2CH: {
		int val;

		ret = kstrtoint(buf, 10, &val);
		if (ret) {
			GRIP_INFO("2ch Invalid Argument\n");
			return ret;
		}

		if (val == 1)
			a96t3x6_enter_unknown_mode(data, TYPE_FORCE);
		else if (val == 0)
			data->is_unknown_mode_2ch = UNKNOWN_OFF;
		else
			GRIP_INFO("2ch Invalid Argument(%d)\n", val);

		GRIP_INFO("2ch %u\n", val);
		return count;
	}
#endif

#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	case ATTR_MCC: {
		int mcc;
		u8 p_thd_h, p_thd_l;

		ret = kstrtoint(buf, 10, &mcc);
		if (ret) {
			GRIP_ERR("Invalid Argument\n");
			return ret;
		}

		data->mcc = mcc;
		GRIP_INFO("mcc value %d\n", data->mcc);

		if ((data->mcc != 001) && (data->mcc != 440) && (data->mcc != 441) &&
		    (data->mcc != 450) && (data->mcc != 460)) {
			p_thd_h = (data->default_threshold >> 8) & 0xff;
			p_thd_l = data->default_threshold & 0xff;
			GRIP_INFO("default threshold %u\n", data->default_threshold);
		} else {
			p_thd_h = (data->mcc_threshold >> 8) & 0xff;
			p_thd_l = data->mcc_threshold & 0xff;
			GRIP_INFO("mcc threshold %u\n", data->mcc_threshold);
		}

		GRIP_INFO("pthd_h = 0x%2x pthd_l = 0x%2x\n", p_thd_h, p_thd_l);
		a96t3x6_i2c_write(data->client, REG_SAR_THRESHOLD, &p_thd_h);
		a96t3x6_i2c_write(data->client, REG_SAR_THRESHOLD + 1, &p_thd_l);
		return count;
	}
#endif

#ifdef CONFIG_SENSOR_A96T3X6_LDO_SHARE
	case ATTR_REGISTER_RECOVER: {
		int current_state = atomic_read(&data->enable);
		u8 reg_value = 0, cmd = 0, check = 0;

		GRIP_INFO("Register recover\n");
		ret = kstrtou8(buf, 10, &check);
		if (check == 1) {
			ret = a96t3x6_i2c_read(data->client, REG_SAR_ENABLE, &reg_value, 1);
			if (ret < 0) {
				GRIP_ERR("fail(%d)\n", ret);
				return count;
			}
			GRIP_INFO("reg=0x24 val=%02X\n", reg_value);

			if (current_state && (reg_value == CMD_OFF)) {
				GRIP_ERR("REG_SAR_ENABLE register recover after HW reset\n");
				cmd = CMD_ON;
				ret = a96t3x6_i2c_write(data->client, REG_SAR_ENABLE, &cmd);
				if (ret < 0)
					GRIP_INFO("failed to enable grip irq\n");
				a96t3x6_check_first_status(data, 1);
			}

			ret = a96t3x6_i2c_read(data->client, REG_SAR_SENSING, &reg_value, 1);
			if (ret < 0) {
				GRIP_ERR("fail(%d)\n", ret);
				return count;
			}
			GRIP_INFO("reg=0x25 val=%02X\n", reg_value);

			if (data->earjack_noise && !current_state && data->earjack) {
				if (reg_value == CMD_ON) {
					GRIP_ERR("REG_SAR_SENSING register recover after HW reset\n");
					cmd = CMD_OFF;
					ret = a96t3x6_i2c_write(data->client,
								REG_SAR_SENSING, &cmd);
					if (ret < 0)
						GRIP_INFO("failed to enable grip irq\n");
					a96t3x6_check_first_status(data, 1);
				}
			}
		} else {
			GRIP_INFO("Unsupported Command\n");
		}
		return count;
	}
#endif

	case ATTR_MOTION: {
		int val;

		ret = kstrtoint(buf, 10, &val);
		if (ret) {
			GRIP_INFO("Invalid Argument\n");
			return ret;
		}

		if (val == 0 || val == 1) {
			GRIP_INFO("motion event %s\n", val ? "" : "off");
			data->motion = val;
		} else {
			GRIP_INFO("Invalid Argument : %u\n", val);
		}
		return count;
	}

	case ATTR_UNKNOWN_STATE: {
		int val;

		ret = kstrtoint(buf, 10, &val);
		if (ret) {
			GRIP_INFO("Invalid Argument\n");
			return ret;
		}

		if (val == 1)
			a96t3x6_enter_unknown_mode(data, TYPE_FORCE);
		else if (val == 0)
			data->is_unknown_mode = UNKNOWN_OFF;
		else
			GRIP_INFO("Invalid Argument(%d)\n", val);

		GRIP_INFO("%u\n", val);
		return count;
	}

	case ATTR_NOTI_ENABLE: {
		u8 enable;

		ret = kstrtou8(buf, 2, &enable);
		if (ret) {
			GRIP_ERR("invalid argument\n");
			return count;
		}

		data->noti_enable = enable;
		GRIP_INFO("new_value=%d\n", (int)enable);

		if (data->noti_enable)
			a96t3x6_enter_unknown_mode(data, TYPE_BOOT);
		return count;
	}

#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	case ATTR_COUNTRY_CODE: {
		u8 country_code;

		ret = kstrtou8(buf, 2, &country_code);
		if (ret) {
			GRIP_INFO("invalid argument\n");
			return count;
		}

		GRIP_INFO("country_code=%d\n", (int)country_code);
		data->country_code = country_code;
		return count;
	}
#endif

	default:
		return -EINVAL;
	}
}

/* Master attribute table for sensors_register */
static struct a96t3x6_attribute a96t3x6_attrs[] = {
	A96T3X6_ATTR_RO(grip_threshold, ATTR_THRESHOLD),
	A96T3X6_ATTR_RO(grip_total_cap, ATTR_TOTAL_CAP),
	A96T3X6_ATTR_RW(grip_sar_enable, 0664, ATTR_SAR_ENABLE),
	A96T3X6_ATTR_WO(grip_sw_reset, 0220, ATTR_SW_RESET),
	A96T3X6_ATTR_RO(grip_sw_reset_ready, ATTR_SW_RESET_READY),
	A96T3X6_ATTR_WO(grip_earjack, 0220, ATTR_EARJACK),
	A96T3X6_ATTR_RO(grip, ATTR_GRIP),
	A96T3X6_ATTR_RO(grip_baseline, ATTR_BASELINE),
	A96T3X6_ATTR_RO(grip_raw, ATTR_RAW),
	A96T3X6_ATTR_RO(grip_ref_cap, ATTR_REF_CAP),
	A96T3X6_ATTR_RO(grip_gain, ATTR_GAIN),
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	A96T3X6_ATTR_RO(grip_gain_2ch, ATTR_GAIN_2CH),
#endif
	A96T3X6_ATTR_RO(grip_check, ATTR_CHECK),
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
	A96T3X6_ATTR_WO(grip_sar_only_mode, 0220, ATTR_SAR_ONLY_MODE),
	A96T3X6_ATTR_WO(grip_sar_press_threshold, 0220, ATTR_SAR_PRESS_THRESHOLD),
	A96T3X6_ATTR_WO(grip_sar_release_threshold, 0220, ATTR_SAR_RELEASE_THRESHOLD),
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	A96T3X6_ATTR_WO(grip_sar_press_threshold_2ch, 0220, ATTR_SAR_PRESS_THRESHOLD_2CH),
	A96T3X6_ATTR_WO(grip_sar_release_threshold_2ch, 0220, ATTR_SAR_RELEASE_THRESHOLD_2CH),
#endif
#endif
#ifdef CONFIG_SEC_FACTORY
	A96T3X6_ATTR_RW(grip_irq_count, 0664, ATTR_IRQ_COUNT),
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	A96T3X6_ATTR_RW(grip_irq_count_2ch, 0664, ATTR_IRQ_COUNT_2CH),
#endif
#endif
	A96T3X6_ATTR_RO(name, ATTR_NAME),
	A96T3X6_ATTR_RO(vendor, ATTR_VENDOR),
	A96T3X6_ATTR_RO(grip_firm_version_phone, ATTR_FIRM_VERSION_PHONE),
	A96T3X6_ATTR_RO(grip_firm_version_panel, ATTR_FIRM_VERSION_PANEL),
	A96T3X6_ATTR_WO(grip_firm_update, 0220, ATTR_FIRM_UPDATE),
	A96T3X6_ATTR_RO(grip_firm_update_status, ATTR_FIRM_UPDATE_STATUS),
	A96T3X6_ATTR_RO(grip_irq_state, ATTR_IRQ_STATE),
	A96T3X6_ATTR_RO(grip_irq_en_cnt, ATTR_IRQ_EN_CNT),
	A96T3X6_ATTR_RW(grip_reg_rw, 0664, ATTR_REG_RW),
	A96T3X6_ATTR_RO(grip_crc_check, ATTR_CRC_CHECK),
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	A96T3X6_ATTR_RO(ch_count, ATTR_CH_COUNT),
	A96T3X6_ATTR_RO(grip_threshold_2ch, ATTR_THRESHOLD_2CH),
	A96T3X6_ATTR_RO(grip_total_cap_2ch, ATTR_TOTAL_CAP_2CH),
	A96T3X6_ATTR_RO(grip_2ch, ATTR_GRIP_2CH),
	A96T3X6_ATTR_RO(grip_baseline_2ch, ATTR_BASELINE_2CH),
	A96T3X6_ATTR_RO(grip_raw_2ch, ATTR_RAW_2CH),
	A96T3X6_ATTR_RO(grip_check_2ch, ATTR_CHECK_2CH),
	A96T3X6_ATTR_RW(unknown_state_2ch, 0444, ATTR_UNKNOWN_STATE_2CH),
#endif
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	A96T3X6_ATTR_RW(mcc, 0664, ATTR_MCC),
#endif
#ifdef CONFIG_SENSOR_A96T3X6_LDO_SHARE
	A96T3X6_ATTR_WO(grip_register_recover, 0220, ATTR_REGISTER_RECOVER),
#endif
	A96T3X6_ATTR_RW(motion, 0664, ATTR_MOTION),
	A96T3X6_ATTR_RW(unknown_state, 0664, ATTR_UNKNOWN_STATE),
	A96T3X6_ATTR_RW(noti_enable, 0664, ATTR_NOTI_ENABLE),
#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	A96T3X6_ATTR_RW(country_code, 0664, ATTR_COUNTRY_CODE),
#endif
};

/* Attribute group for input_dev ("enable") */
static struct a96t3x6_attribute a96t3x6_input_attr_enable =
	A96T3X6_ATTR_RW(enable, 0664, ATTR_ENABLE);

static struct attribute *a96t3x6_input_attrs[] = {
	&a96t3x6_input_attr_enable.dev_attr.attr,
	NULL,
};

static const struct attribute_group a96t3x6_input_attr_group = {
	.attrs = a96t3x6_input_attrs,
};

/* Array of pointers expected by sensors_register */
static struct device_attribute *grip_sensor_attributes[ARRAY_SIZE(a96t3x6_attrs) + 1];

static void a96t3x6_init_sysfs_attrs(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(a96t3x6_attrs); i++)
		grip_sensor_attributes[i] = &a96t3x6_attrs[i].dev_attr;
	grip_sensor_attributes[ARRAY_SIZE(a96t3x6_attrs)] = NULL;
}

/* Power & DT Configuration */
static int a96t3x6_power_onoff(void *pdata, bool on)
{
	struct a96t3x6_data *data = (struct a96t3x6_data *)pdata;
	int ret = 0;
	int voltage = 0;
	int reg_enabled = 0;

	if (data->ldo_en) {
		ret = gpio_request(data->ldo_en, "a96t3x6_ldo_en");
		if (ret < 0) {
			SENSOR_ERR("gpio %d request failed %d\n", data->ldo_en, ret);
			return ret;
		}
		gpio_set_value(data->ldo_en, on);
		GRIP_INFO("ldo_en power %d\n", on);
		gpio_free(data->ldo_en);
	}

	if (data->dvdd_vreg_name) {
		if (data->dvdd_vreg == NULL) {
			data->dvdd_vreg = regulator_get(NULL, data->dvdd_vreg_name);
			if (IS_ERR(data->dvdd_vreg)) {
				data->dvdd_vreg = NULL;
				GRIP_ERR("failed to get dvdd_vreg %s\n", data->dvdd_vreg_name);
			}
		}
	}

	if (data->dvdd_vreg) {
		voltage = regulator_get_voltage(data->dvdd_vreg);
		reg_enabled = regulator_is_enabled(data->dvdd_vreg);
		GRIP_INFO("dvdd_vreg reg_enabled=%d voltage=%d\n", reg_enabled, voltage);
	}

	if (on) {
		if (data->dvdd_vreg) {
			if (reg_enabled == 0) {
				ret = regulator_enable(data->dvdd_vreg);
				if (ret) {
					GRIP_ERR("dvdd reg enable fail\n");
					return ret;
				}
				GRIP_INFO("dvdd_vreg turned on\n");
			}
		}
	} else {
		if (data->dvdd_vreg) {
			if (reg_enabled == 1) {
				ret = regulator_disable(data->dvdd_vreg);
				if (ret) {
					GRIP_ERR("dvdd reg disable fail\n");
					return ret;
				}
				GRIP_INFO("dvdd_vreg turned off\n");
			}
		}
	}

	GRIP_INFO("%s\n", on ? "on" : "off");
	return ret;
}

static int a96t3x6_irq_init(struct device *dev, struct a96t3x6_data *data)
{
	int ret = 0;

	ret = gpio_request(data->grip_int, "a96t3x6_IRQ");
	if (ret < 0) {
		GRIP_ERR("gpio %d request failed (%d)\n", data->grip_int, ret);
		return ret;
	}

	ret = gpio_direction_input(data->grip_int);
	if (ret < 0) {
		GRIP_ERR("failed to set direction input gpio %d(%d)\n",
			 data->grip_int, ret);
		gpio_free(data->grip_int);
		return ret;
	}
	data->power = a96t3x6_power_onoff;
	return ret;
}

static int a96t3x6_parse_dt(struct a96t3x6_data *data, struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct pinctrl *p;
	int ret;
	enum of_gpio_flags flags;
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	u32 mcc_thd;
#endif

	data->grip_int = of_get_named_gpio(np, "a96t3x6,irq_gpio", 0);
	if (data->grip_int < 0) {
		GRIP_ERR("Cannot get grip_int\n");
		return data->grip_int;
	}

	data->ldo_en = of_get_named_gpio_flags(np, "a96t3x6,ldo_en", 0, &flags);
	if (data->ldo_en < 0) {
		GRIP_ERR("fail to get ldo_en\n");
		data->ldo_en = 0;
	} else {
		ret = gpio_request(data->ldo_en, "a96t3x6_ldo_en");
		if (ret < 0) {
			GRIP_ERR("gpio %d request failed %d\n", data->ldo_en, ret);
			return ret;
		}
#ifdef CONFIG_SENSOR_A96T3X6_LDO_SHARE
		gpio_direction_output(data->ldo_en, 1);
#else
		gpio_direction_output(data->ldo_en, 0);
#endif
		gpio_free(data->ldo_en);
	}

	if (of_property_read_string_index(np, "a96t3x6,dvdd_vreg_name", 0,
					  (const char **)&data->dvdd_vreg_name)) {
		data->dvdd_vreg_name = NULL;
	}
	GRIP_INFO("dvdd_vreg_name: %s\n", data->dvdd_vreg_name);

	ret = of_property_read_string(np, "a96t3x6,fw_path",
				      (const char **)&data->fw_path);
	if (ret < 0) {
		GRIP_ERR("failed to read fw_path %d\n", ret);
		data->fw_path = TK_FW_PATH_BIN;
	}
	GRIP_INFO("fw path %s\n", data->fw_path);

	data->bringup = of_property_read_bool(np, "a96t3x6,bringup");

	ret = of_property_read_u32(np, "a96t3x6,firmup_cmd", &data->firmup_cmd);
	if (ret < 0)
		data->firmup_cmd = 0;

	ret = of_property_read_u8(np, "a96t3x6,earjack_noise",
				  &data->earjack_noise);
	if (ret < 0) {
		GRIP_INFO("failed to get the earjack noise dt\n");
		data->earjack_noise = 0;
	} else {
		GRIP_INFO("grip earjack noise block is adjusting %d \n", (int)data->earjack_noise);
		data->earjack_noise = 1;
	}

	p = devm_pinctrl_get_select_default(dev);
	if (IS_ERR(p))
		GRIP_INFO("failed pinctrl_get\n");

	GRIP_INFO("grip_int:%d, ldo_en:%d\n", data->grip_int, data->ldo_en);

#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_MCC_THRESHOLD_CHANGE)
	ret = of_property_read_u32(np, "a96t3x6,thd_mcc", &mcc_thd);
	if (ret < 0) {
		GRIP_ERR("failed to get thd_mcc\n");
		data->mcc_threshold = 0;
	} else {
		data->mcc_threshold = (u16)mcc_thd;
		GRIP_INFO("thd_mcc = %d \n", data->mcc_threshold);
	}
#endif
	return 0;
}

static int a96t3x6_input_init(struct a96t3x6_data *data)
{
	struct input_dev *input_dev = data->input_dev;
	struct input_dev *noti_input_dev = data->noti_input_dev;

	input_dev->name = MODULE_NAME;
	input_dev->id.bustype = BUS_I2C;
	input_set_capability(input_dev, EV_REL, REL_X);
	input_set_capability(input_dev, EV_REL, REL_MISC);
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	input_set_capability(input_dev, EV_REL, REL_Y);
	input_set_capability(input_dev, EV_REL, REL_DIAL);
#endif
#ifdef CONFIG_SENSOR_A96T3X6_LDO_SHARE
	input_set_capability(input_dev, EV_REL, REL_WHEEL);
#endif
	input_set_drvdata(input_dev, data);

	noti_input_dev->name = MODULE_NOTIFIER_NAME;
	noti_input_dev->id.bustype = BUS_I2C;
	input_set_capability(noti_input_dev, EV_REL, REL_X);
	input_set_drvdata(noti_input_dev, data);

	return 0;
}

static int a96t3x6_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct a96t3x6_data *data;
	int ret;
#ifdef CONFIG_SENSORS_FW_VENDOR
	u8 buf;
#endif

	GRIP_INFO("start (0x%x)\n", client->addr);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		GRIP_ERR("i2c_check_functionality fail\n");
		return -EIO;
	}

	data = kzalloc(sizeof(struct a96t3x6_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->input_dev = input_allocate_device();
	if (!data->input_dev) {
		ret = -ENOMEM;
		goto err_alloc_input;
	}

	data->noti_input_dev = input_allocate_device();
	if (!data->noti_input_dev) {
		ret = -ENOMEM;
		goto err_alloc_noti;
	}

	data->client = client;
	data->probe_done = false;
	data->earjack = 0;
	data->skip_event = false;
	data->sar_mode = false;
	data->is_unknown_mode = UNKNOWN_ON;
	data->first_working = false;
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	data->is_unknown_mode_2ch = UNKNOWN_ON;
	data->first_working_2ch = false;
#endif
	data->motion = 0;

	wake_lock_init(&data->grip_wake_lock, WAKE_LOCK_SUSPEND, "grip wake lock");

	ret = a96t3x6_parse_dt(data, &client->dev);
	if (ret) {
		GRIP_ERR("failed to a96t3x6_parse_dt\n");
		goto err_dt;
	}

	ret = a96t3x6_irq_init(&client->dev, data);
	if (ret) {
		GRIP_ERR("failed to init reg\n");
		goto err_irq_init;
	}

	if (data->power) {
		data->power(data, true);
#ifdef CONFIG_SENSORS_FW_VENDOR
		usleep_range(INIT_DELAY, INIT_DELAY);
#else
		usleep_range(RESET_DELAY, RESET_DELAY);
#endif
	}

	data->irq = -1;
	client->irq = gpio_to_irq(data->grip_int);
	mutex_init(&data->lock);
	i2c_set_clientdata(client, data);

#ifndef CONFIG_SENSORS_FW_VENDOR
	ret = a96t3x6_fw_check(data);
	if (ret) {
		GRIP_ERR("failed to firmware check (%d)\n", ret);
		goto err_fw_check;
	}
#else
	ret = a96t3x6_i2c_read(client, REG_MODEL_NO, &buf, 1);
	if (ret) {
		GRIP_ERR("i2c is failed %d\n", ret);
		goto err_fw_check;
	}
	GRIP_INFO("i2c is normal, model_no = 0x%2x\n", buf);
#endif

	a96t3x6_input_init(data);

	INIT_DELAYED_WORK(&data->debug_work, a96t3x6_debug_work_func);
#ifdef CONFIG_SENSORS_FW_VENDOR
	INIT_DELAYED_WORK(&data->firmware_work, a96t3x6_firmware_work_func);
#endif
#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
	INIT_DELAYED_WORK(&data->reset_work, a96t3x6_reset_work_func);
#endif
#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	pogo_notifier_register(&data->pogo_nb, a96t3x6_pogo_notifier, POGO_NOTIFY_DEV_SENSOR);
#endif
#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
	|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	INIT_WORK(&data->cmdon_work, cmdon_work_func);
	INIT_WORK(&data->cmdoff_work, cmdoff_work_func);
#endif

	ret = input_register_device(data->input_dev);
	if (ret) {
		GRIP_ERR("failed to register input dev (%d)\n", ret);
		goto err_input_reg;
	}

	ret = sensors_create_symlink(&data->input_dev->dev.kobj, data->input_dev->name);
	if (ret < 0) {
		GRIP_ERR("Failed to create sysfs symlink\n");
		goto err_symlink;
	}

	ret = sysfs_create_group(&data->input_dev->dev.kobj, &a96t3x6_input_attr_group);
	if (ret < 0) {
		GRIP_ERR("Failed to create sysfs group\n");
		goto err_sysfs_group;
	}

	ret = input_register_device(data->noti_input_dev);
	if (ret) {
		GRIP_ERR("failed to register noti input dev (%d)\n", ret);
		goto err_noti_reg;
	}

	a96t3x6_init_sysfs_attrs();
	ret = sensors_register(&data->dev, data, grip_sensor_attributes, MODULE_NAME);
	if (ret) {
		GRIP_ERR("could not register grip_sensor(%d)\n", ret);
		goto err_sensor_reg;
	}

	ret = request_threaded_irq(client->irq, NULL, a96t3x6_interrupt,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT, MODEL_NAME, data);
	if (ret < 0) {
		GRIP_ERR("Failed to register interrupt\n");
		goto err_req_irq;
	}
	disable_irq(client->irq);

	data->irq = client->irq;
	/*
	 * Keep data->dev pointing at the sensors device created by
	 * sensors_register() so that sensors_unregister() in the error and
	 * remove paths removes the attributes from the device they were
	 * created on.
	 */
	device_init_wakeup(&client->dev, true);

	a96t3x6_set_debug_work(data, 1, 20000);
#ifdef CONFIG_SENSORS_FW_VENDOR
	a96t3x6_set_firmware_work(data, 1, 200);
#endif
#ifdef CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER
	data->hall_nb.priority = 1;
	data->hall_nb.notifier_call = a96t3x6_hall_ic_notify;
	hall_ic_register_notify(&data->hall_nb);
#endif
	data->motion = 1;
	data->first_working = false;
	data->is_unknown_mode = UNKNOWN_OFF;
#ifdef CONFIG_SENSORS_A96T3X6_2CH
	data->first_working_2ch = false;
	data->is_unknown_mode_2ch = UNKNOWN_OFF;
#endif

	GRIP_INFO("done\n");
	data->probe_done = true;
	data->resume_called = false;
	return 0;

err_req_irq:
	sensors_unregister(data->dev, grip_sensor_attributes);
err_sensor_reg:
	input_unregister_device(data->noti_input_dev);
	data->noti_input_dev = NULL;
err_noti_reg:
	sysfs_remove_group(&data->input_dev->dev.kobj, &a96t3x6_input_attr_group);
err_sysfs_group:
	sensors_remove_symlink(&data->input_dev->dev.kobj, data->input_dev->name);
err_symlink:
	input_unregister_device(data->input_dev);
	data->input_dev = NULL;
err_input_reg:
#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	pogo_notifier_unregister(&data->pogo_nb);
#endif
	cancel_delayed_work_sync(&data->debug_work);
#ifdef CONFIG_SENSORS_FW_VENDOR
	cancel_delayed_work_sync(&data->firmware_work);
#endif
#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
	cancel_delayed_work_sync(&data->reset_work);
#endif
#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
	|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	cancel_work_sync(&data->cmdoff_work);
	cancel_work_sync(&data->cmdon_work);
#endif
err_fw_check:
	mutex_destroy(&data->lock);
	if (data->power)
		data->power(data, false);
	gpio_free(data->grip_int);
err_irq_init:
err_dt:
	wake_lock_destroy(&data->grip_wake_lock);
	if (data->noti_input_dev)
		input_free_device(data->noti_input_dev);
err_alloc_noti:
	if (data->input_dev)
		input_free_device(data->input_dev);
err_alloc_input:
	kfree(data);
	GRIP_ERR("failed\n");
	return ret;
}

static int a96t3x6_remove(struct i2c_client *client)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);

	if (!data)
		return 0;

	/* 1. Disable device wakeup and unregister all external notifiers */
	device_init_wakeup(&client->dev, false);

#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
	hall_ic_unregister_notify(&data->hall_nb);
#endif
#if defined(CONFIG_TABLET_MODEL_CONCEPT)
	pogo_notifier_unregister(&data->pogo_nb);
#endif
#if defined(CONFIG_CCIC_NOTIFIER) && defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	if (data->manager_notifier_registered) {
		manager_notifier_unregister(&data->cpuidle_ccic_nb);
		data->manager_notifier_registered = false;
	}
#endif

	/*
	 * 2. Remove every sysfs entry point before tearing the instance down:
	 *    a store callback (e.g. grip enable) must not run against a freed
	 *    IRQ or a torn-down input device.
	 */
	sensors_unregister(data->dev, grip_sensor_attributes);

	if (data->input_dev) {
		sysfs_remove_group(&data->input_dev->dev.kobj,
				   &a96t3x6_input_attr_group);
		sensors_remove_symlink(&data->input_dev->dev.kobj,
				       data->input_dev->name);
	}

	/* 3. Free IRQ synchronously before cancelling works and freeing data */
	if (data->irq >= 0) {
		free_irq(data->irq, data);
		data->irq = -1;
	}

	/* 4. Cancel all pending and running delayed/standard works synchronously */
	cancel_delayed_work_sync(&data->debug_work);
#ifdef CONFIG_SENSORS_FW_VENDOR
	cancel_delayed_work_sync(&data->firmware_work);
#endif
#if defined(CONFIG_SENSORS_A96T3X6_HALL_NOTIFIER)
	cancel_delayed_work_sync(&data->reset_work);
#endif
#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
	|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	cancel_work_sync(&data->cmdoff_work);
	cancel_work_sync(&data->cmdon_work);
#endif

	/* 5. Unregister input devices */
	if (data->input_dev) {
		input_unregister_device(data->input_dev);
		data->input_dev = NULL;
	}

	if (data->noti_input_dev) {
		input_unregister_device(data->noti_input_dev);
		data->noti_input_dev = NULL;
	}

	/* 6. Power off hardware and release GPIO */
	if (data->power)
		data->power(data, false);

	if (gpio_is_valid(data->grip_int))
		gpio_free(data->grip_int);

	/* 7. Destroy synchronization primitives and free driver memory */
	wake_lock_destroy(&data->grip_wake_lock);
	mutex_destroy(&data->lock);
	kfree(data);

	return 0;
}

static int a96t3x6_suspend(struct device *dev)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	data->resume_called = false;
	pr_debug("%s\n", __func__);
	a96t3x6_sar_only_mode(data, 1);
	a96t3x6_set_debug_work(data, 0, 1000);

	return 0;
}

static int a96t3x6_resume(struct device *dev)
{
	struct a96t3x6_data *data = dev_get_drvdata(dev);

	pr_debug("%s\n", __func__);
	data->resume_called = true;
	a96t3x6_set_debug_work(data, 1, 0);

	return 0;
}

static void a96t3x6_shutdown(struct i2c_client *client)
{
	struct a96t3x6_data *data = i2c_get_clientdata(client);

	if (!data)
		return;

	GRIP_INFO("%s\n", __func__);

#if defined(CONFIG_CCIC_NOTIFIER) || defined(CONFIG_PDIC_NOTIFIER) \
	|| defined(CONFIG_MUIC_NOTIFIER) || defined(CONFIG_USB_TYPEC_MANAGER_NOTIFIER)
	cancel_work_sync(&data->cmdoff_work);
	cancel_work_sync(&data->cmdon_work);
#endif
	a96t3x6_set_debug_work(data, 0, 1000);

	if (data->irq >= 0)
		disable_irq(data->irq);

	if (data->power)
		data->power(data, false);
}

static const struct i2c_device_id a96t3x6_device_id[] = {
	{MODULE_NAME, 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, a96t3x6_device_id);

#ifdef CONFIG_OF
static const struct of_device_id a96t3x6_match_table[] = {
	{ .compatible = "a96t3x6", },
	{ },
};
#else
#define a96t3x6_match_table NULL
#endif

static const struct dev_pm_ops a96t3x6_pm_ops = {
	.suspend = a96t3x6_suspend,
	.resume = a96t3x6_resume,
};

static struct i2c_driver a96t3x6_driver = {
	.probe = a96t3x6_probe,
	.remove = a96t3x6_remove,
	.shutdown = a96t3x6_shutdown,
	.id_table = a96t3x6_device_id,
	.driver = {
		.name = MODEL_NAME,
		.owner = THIS_MODULE,
		.of_match_table = a96t3x6_match_table,
		.pm = &a96t3x6_pm_ops,
	},
};

static int __init a96t3x6_init(void)
{
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
	if (lpcharge) {
		GRIP_ERR("%s: lpm : Do not load driver\n", __func__);
		return 0;
	}
#endif
	return i2c_add_driver(&a96t3x6_driver);
}

static void __exit a96t3x6_exit(void)
{
	i2c_del_driver(&a96t3x6_driver);
}

module_init(a96t3x6_init);
module_exit(a96t3x6_exit);

MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Grip sensor driver for A96T3X6 chip");
MODULE_LICENSE("GPL");
