// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung QUEST (NAD/QDAF) debugging driver.
 *
 * COPYRIGHT(C) 2006-2020 Samsung Electronics Co., Ltd. All Rights Reserved.
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/sec_class.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/syscalls.h>
#include <linux/fcntl.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/sec_debug.h>
#include <linux/sec_param.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/uaccess.h>
#include <linux/sec_quest.h>
#include <linux/sec_quest_param.h>
#include <linux/sec_quest_qdaf.h>

#if defined(CONFIG_ARCH_LAHAINA)
#define CONFIG_SEC_QUEST_NOT_TRIGGER_SMDDL_QDAF
#define CONFIG_SEC_QUEST_NOT_TRIGGER_MAIN_QDAF
#endif

#ifdef CONFIG_SEC_QUEST_BPS_CLASSIFIER
#include <linux/sec_quest_bps_classifier.h>

extern struct bps_info bps_envs;
#endif

/* param data */
extern struct param_quest_t param_quest_data;
extern struct param_quest_ddr_result_t param_quest_ddr_result_data;
extern unsigned int param_api_gpio_test;
extern char param_api_gpio_test_result[256];

/* sysfs devices */
struct device *sec_nad;
#if defined(CONFIG_SEC_FACTORY)
struct device *sec_nad_balancer;
static struct kobj_uevent_env quest_uevent;
#endif

/* etc */
extern unsigned int lpcharge;
struct mutex sysfs_common_lock;

#if defined(CONFIG_SEC_FACTORY)
static int erased;
static int step_to_smd_quest_hlos;
#if defined(CONFIG_SEC_QUEST_AUTO_TRIGGER_KWORKER)
struct delayed_work trigger_quest_work;
#define WAIT_TIME_BEFORE_TRIGGER_MSECS 60000
#endif
static int call_main_qdaf_after_finighing_main_quest;
static int boot_count;
static int testmode_enabled;
static int testmode_quefi_enabled = 1;
static int testmode_suefi_enabled = 1;
static int testmode_ddrscan_enabled = 1;
#endif

/* Synchronize with enum quest_enum_item in sec_quest.h */
char *STR_ITEM[ITEM_ITEMSCOUNT] = {
	"",
	"HLOS",
#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
	"HLOSDUMMY",
#elif defined(CONFIG_SEC_QUEST_HLOS_NATURESCENE_SMD)
	"HLOSNATURESCENE",
#endif
	"FUSION",
	"QUEFI",
	"SUEFI_LIGHT",
	"SUEFI_HEAVY",
	"DDR_SCAN_LOADER",
	"DDR_SCAN_RAMDUMP_ENCACHE",
	"DDR_SCAN_RAMDUMP_DISCACHE",
	"DDR_SCAN_UEFI",
	"SMDDL_QDAF",
	"DDRTRAINING",
};

/* Synchronize with enum quest_enum_smd_subitem in sec_quest.h */
char *STR_SUBITEM[SUBITEM_ITEMSCOUNT] = {
	"",
	"DDR_SCAN",
#if defined(CONFIG_SEC_QUEST_EDL)
	"SUEFI_GROUP1",
	"SUEFI_GROUP2",
	"SUEFI_GROUP3",
	"SUEFI_GROUP4",
	"SUEFI_GROUP5",
	"SUEFI_GROUP6",
	"SUEFI_GROUP7",
	"SUEFI_GROUP8",
	"SUEFI_GROUP9",
	"QUEFI_GROUP1",
	"QUEFI_GROUP2",
	"QUEFI_GROUP3",
	"QUEFI_GROUP4",
	"QUEFI_GROUP5",
	"QUEFI_GROUP6",
	"QUEFI_GROUP7",
	"QUEFI_GROUP8",
	"QUEFI_GROUP9",
#else
	"QUEFI",
	"SUEFI_CRYPTO",
	"SUEFI_COMPLEX",
#endif
#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
	"HLOS_DUMMY",
#elif defined(CONFIG_SEC_QUEST_HLOS_NATURESCENE_SMD)
	"HLOS_NATURESCENE",
	"HLOS_AOSSTHERMALDIFF",
#else
	"HLOS_CRYPTO",
	"HLOS_ICACHE",
	"HLOS_CCOHERENCY",
	"HLOS_QMESADDR",
	"HLOS_QMESACACHE",
	"HLOS_SUSPEND",
	"HLOS_VDDMIN",
	"HLOS_THERMAL",
	"HLOS_UFS",
	"FUSION_A75G",
	"FUSION_Q65G",
#endif
#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
	"SMDDL_QDAF"
#endif
};

#if defined(CONFIG_SEC_FACTORY)

#if defined(CONFIG_SEC_QUEST_BPS_CLASSIFIER)
static void sec_quest_bps_print_info(struct bps_info *bfo, const char *info)
{
	QUEST_PRINT("\n=====================================================\n"
		    " BPS info : %s\n"
		    "=====================================================\n"
		    " magic[0] = 0x%x\n"
		    " magic[1] = 0x%x\n"
		    "=====================================================\n"
		    " sp : %d\n"
		    " wp : %d\n"
		    " dp : %d\n"
		    " kp : %d\n"
		    " mp : %d\n"
		    " tp : %d\n"
		    " cp : %d\n"
		    "=====================================================\n"
		    " pc_lr_cnt = %d\n"
		    " pc_lr_last_idx = %d\n"
		    " tzerr_cnt = %d\n"
		    " klg_cnt = %d\n"
		    " dn_cnt = %d\n"
		    " build_id = %s\n"
		    "=====================================================\n",
		    info, bfo->magic[0], bfo->magic[1], bfo->up_cnt.sp,
		    bfo->up_cnt.wp, bfo->up_cnt.dp, bfo->up_cnt.kp,
		    bfo->up_cnt.mp, bfo->up_cnt.tp, bfo->up_cnt.cp,
		    bfo->pc_lr_cnt, bfo->pc_lr_last_idx,
		    bfo->tzerr_cnt, bfo->klg_cnt, bfo->dn_cnt,
		    bfo->build_id);
}

static void sec_quest_bps_param_read(void)
{
	if (sec_quest_bps_env_initialized)
		return;

	quest_load_param_quest_bps_data();
	sec_quest_bps_print_info(&bps_envs, "kernel");
	sec_quest_bps_env_initialized = true;
}
#endif

/* panic notifier functions */
static int quest_debug_panic_handler(struct notifier_block *nb,
				     unsigned long l, void *buf)
{
	QUEST_PRINT("%s : print param_quest_data\n", __func__);
	quest_print_param_quest_data();

	return NOTIFY_DONE;
}

static struct notifier_block quest_panic_block = {
	.notifier_call = quest_debug_panic_handler,
};

/* helper functions */
static int call_user_prg(char **argv, int wait)
{
	int ret_userapp;
	char *envp[5] = {
		"HOME=/",
		"PATH=/system/bin/quest:/system/bin:/system/xbin",
		"ANDROID_DATA=/data",
		"ANDROID_ROOT=/system",
		NULL
	};

	ret_userapp = call_usermodehelper(argv[0], argv, envp, wait);
	if (!ret_userapp) {
		QUEST_PRINT("%s is executed. ret_userapp = %d\n", argv[0], ret_userapp);
		return 0;
	}

	QUEST_PRINT("%s is NOT executed. ret_userapp = %d\n", argv[0], ret_userapp);
	return ret_userapp;
}

static int do_quest(void)
{
	char *argv[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
	char log_path[64] = { '\0' };
	int ret;

	QUEST_PRINT("%s : curr_step=%d\n", __func__, param_quest_data.curr_step);

	switch (param_quest_data.curr_step) {
	case STEP_SMDDL:
		argv[0] = QUESTHLOS_PROG_SMD;
		snprintf(log_path, sizeof(log_path), "logPath:%s", SMD_QUEST_LOGPATH);
		argv[1] = log_path;
#if defined(CONFIG_SEC_DDR_SKP)
		argv[2] = "Reboot";
#endif
		break;
	case STEP_CAL1:
#if defined(CONFIG_SEC_QUEST_CAL_HLOS_SUPPORT_FUSION)
		argv[0] = QUESTHLOS_PROG_MAIN_CAL;
		snprintf(log_path, sizeof(log_path), "logPath:%s", CAL_QUEST_LOGPATH);
		argv[1] = log_path;
		argv[2] = "Reboot";
		QUEST_PRINT("reboot option enabled\n");
		argv[3] = "hlosTestDisabled:1";
		QUEST_PRINT("hlosTestDisabled option enabled\n");
		argv[4] = "fusionTestEnabled:1";
		QUEST_PRINT("fusionTestEnabled option enabled\n");
		argv[5] = "qdafTestEnabled:1";
		QUEST_PRINT("qdafTestEnabled option enabled\n");
		break;
#endif
	case STEP_CALX:
		argv[0] = QUESTHLOS_PROG_MAIN_CAL;
		snprintf(log_path, sizeof(log_path), "logPath:%s", CAL_QUEST_LOGPATH);
		argv[1] = log_path;
		argv[2] = "Reboot";
		QUEST_PRINT("reboot option enabled\n");
		break;
	case STEP_TESTMODE:
		argv[0] = QUESTHLOS_PROG_MAIN_CAL;
		snprintf(log_path, sizeof(log_path), "logPath:%s", CAL_QUEST_LOGPATH);
		argv[1] = log_path;
		argv[2] = "Reboot";
		QUEST_PRINT("reboot option enabled\n");
		argv[3] = "testmodeTestEnabled:1";
		QUEST_PRINT("testmodeTestEnabled option enabled\n");
		break;
	case STEP_MAIN:
		argv[0] = QUESTHLOS_PROG_MAIN_CAL;
		snprintf(log_path, sizeof(log_path), "logPath:%s", MAIN_QUEST_LOGPATH);
		argv[1] = log_path;
		argv[2] = "Reboot";
		QUEST_PRINT("reboot option enabled\n");
		argv[3] = "1800";
#if defined(CONFIG_SEC_QUEST_MAIN_HLOS_SUPPORT_FUSION)
		argv[4] = "fusionTestEnabled:1";
		QUEST_PRINT("fusionTestEnabled option enabled\n");
#endif
		break;
	default:
		QUEST_PRINT("invalid step\n");
		return -EINVAL;
	}

	ret = call_user_prg(argv, UMH_WAIT_EXEC);

	return ret;
}

static void move_questresult_to_sub_dir(int quest_step)
{
	char *argv[4] = { NULL, NULL, NULL, NULL };

	argv[0] = MOVE_QUESTRESULT_PRG;
	switch (quest_step) {
	case STEP_SMDDL:
		argv[1] = SMD_QUEST_LOGPATH;
		break;
	case STEP_CAL1:
	case STEP_CALX:
	case STEP_TESTMODE:
		argv[1] = CAL_QUEST_LOGPATH;
		break;
	case STEP_MAIN:
		argv[1] = MAIN_QUEST_LOGPATH;
		break;
	default:
		return;
	}
	QUEST_PRINT("%s : will move questresult files to %s\n", __func__, argv[1]);

	call_user_prg(argv, UMH_WAIT_PROC);
}

static int call_quest_debugging_sh(const char *action, int wait)
{
	char *argv[4] = { NULL, NULL, NULL, NULL };
	char step_str[32];

	QUEST_PRINT("%s : will call %s with action (%s)\n", __func__, QUEST_DEBUGGING_PRG, action);

	argv[0] = QUEST_DEBUGGING_PRG;
	argv[1] = (char *)action;
	snprintf(step_str, sizeof(step_str), "step:%d", param_quest_data.curr_step);
	argv[2] = step_str;
	return call_user_prg(argv, wait);
}

static enum quest_enum_item_result check_item_result(uint64_t item_result, uint32_t max_cnt)
{
	enum quest_enum_item_result result;
	int i;

	if (item_result == 0)
		return ITEM_RESULT_NONE;

	/* check fail first */
	for (i = 1; i < max_cnt; i++) {
		result = QUEST_GET_ITEM_SUBITEM_RESULT(item_result, i);
		if (result == ITEM_RESULT_FAIL)
			return ITEM_RESULT_FAIL;
	}

	/* check incompleted */
	for (i = 1; i < max_cnt; i++) {
		result = QUEST_GET_ITEM_SUBITEM_RESULT(item_result, i);
		if (result == ITEM_RESULT_INCOMPLETED)
			return ITEM_RESULT_INCOMPLETED;
	}

	return ITEM_RESULT_PASS;
}

static int check_if_incompleted_item_result_exist(uint64_t item_result, uint32_t max_cnt)
{
	enum quest_enum_item_result result;
	int i;

	if (item_result == 0)
		return 0;

	for (i = 1; i < max_cnt; i++) {
		result = QUEST_GET_ITEM_SUBITEM_RESULT(item_result, i);
		if (result == ITEM_RESULT_INCOMPLETED)
			return 1;
	}

	return 0;
}

/* check smd_subitem_result and return result string */
static int get_smd_subitem_result_string(char *buf, size_t buf_len, int piece)
{
	int i, failed_cnt = 0;

	if (piece == SUBITEM_ITEMSCOUNT) {
		buf[0] = '\0';
		for (i = SUBITEM_NONE + 1; i < SUBITEM_ITEMSCOUNT; i++) {
			switch (QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_subitem_result, i)) {
			case ITEM_RESULT_FAIL:
				strlcat(buf, "[F]", buf_len);
				failed_cnt++;
				break;
			case ITEM_RESULT_PASS:
				strlcat(buf, "[P]", buf_len);
				break;
			default:
				strlcat(buf, "[X]", buf_len);
				break;
			}
		}
	} else {
		switch (QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_subitem_result, piece)) {
		case ITEM_RESULT_FAIL:
			strscpy(buf, "FAIL", buf_len);
			failed_cnt++;
			break;
		case ITEM_RESULT_PASS:
			strscpy(buf, "PASS", buf_len);
			break;
		default:
			strscpy(buf, "NA", buf_len);
			break;
		}
	}

	return failed_cnt;
}

#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
static void check_and_update_qdaf_result(void)
{
	int qdaf_failed_cnt;

	qdaf_failed_cnt = get_qdaf_failed_cnt();
	if (qdaf_failed_cnt > 0) {
		QUEST_PRINT("%s : ITEM_SMDDLQDAF was failed (failed_cnt=%d)\n", __func__, qdaf_failed_cnt);
		QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, ITEM_SMDDLQDAF, ITEM_RESULT_FAIL);
		QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_subitem_result, SUBITEM_SMDDLQDAF, ITEM_RESULT_FAIL);
	} else {
		QUEST_PRINT("%s : ITEM_SMDDLQDAF was succeeded\n", __func__);
		QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, ITEM_SMDDLQDAF, ITEM_RESULT_PASS);
		QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_subitem_result, SUBITEM_SMDDLQDAF, ITEM_RESULT_PASS);
	}
	quest_sync_param_quest_data();
}
#endif

static void run_qdaf_in_background(enum quest_qdaf_action_t action)
{
#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
	if (param_quest_data.curr_step == STEP_SMDDL &&
	    (action == QUEST_QDAF_ACTION_CONTROL_START_WITH_PANIC ||
	     action == QUEST_QDAF_ACTION_CONTROL_START_WITHOUT_PANIC)) {
		QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, ITEM_SMDDLQDAF, ITEM_RESULT_INCOMPLETED);
		QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_subitem_result, SUBITEM_SMDDLQDAF, ITEM_RESULT_INCOMPLETED);
		quest_sync_param_quest_data();
	}
#endif
	call_qdaf_from_quest_driver(action, UMH_WAIT_EXEC);
}

/* initializer functions */
static void make_debugging_files(void)
{
#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
	/* refer to qpnp_pon_reason (index=boot_reason-1) */
	QUEST_PRINT("%s : boot_reason was %d\n", __func__, boot_reason);
#endif

	/* updatebootcount: do not call with UMH_WAIT_PROC to avoid init thread race */
	call_quest_debugging_sh("action:updatebootcount", UMH_WAIT_EXEC);
	msleep(1000);

	boot_count = call_quest_debugging_sh("action:getbootcount", UMH_WAIT_PROC);
	boot_count = (boot_count >= 0) ? (boot_count >> 8) : 0;
	QUEST_PRINT("%s : boot_count = %d\n", __func__, boot_count);

	call_quest_debugging_sh("action:ls", UMH_WAIT_PROC);
	call_quest_debugging_sh("action:resethist", UMH_WAIT_PROC);
	call_quest_debugging_sh("action:lastkmsg", UMH_WAIT_PROC);
}

static void check_abnormal_param(void)
{
	/* Checked by quest_check_abnormal_param_quest_data() in XBL */
}

/* initialize step for smd, cal and main; move uefi log to output_log_path */
static void setup_scenario(void)
{
	switch (param_quest_data.curr_step) {
	case STEP_SMDDL: {
		enum quest_enum_item_result qdaf_item_result, hlos_item_result, total_result;
		uint64_t smd_item_result_without_qdaf;
		int exist_incompleted = 0;

		QUEST_PRINT("%s : (step=%d) smd scenario\n", __func__, STEP_SMDDL);

		move_questresult_to_sub_dir(STEP_SMDDL);

		qdaf_item_result = QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, ITEM_SMDDLQDAF);
		hlos_item_result = QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD);
		smd_item_result_without_qdaf = param_quest_data.smd_item_result;
		QUEST_SET_ITEM_SUBITEM_RESULT(smd_item_result_without_qdaf, ITEM_SMDDLQDAF, ITEM_RESULT_PASS);
		exist_incompleted = check_if_incompleted_item_result_exist(smd_item_result_without_qdaf, ITEM_ITEMSCOUNT);
		total_result = check_item_result(smd_item_result_without_qdaf, ITEM_ITEMSCOUNT);

		if (hlos_item_result == ITEM_RESULT_INCOMPLETED) {
			QUEST_PRINT("%s : (step=%d) reboot while running quest_hlos\n", __func__, STEP_SMDDL);
			QUEST_PRINT("%s : Let's check lastkmsg\n", __func__);

			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
			quest_initialize_curr_step();
		} else if (total_result == ITEM_RESULT_FAIL) {
			QUEST_PRINT("%s : (step=%d) total_result == ITEM_RESULT_FAIL, so initialize step\n", __func__, STEP_SMDDL);
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
			quest_initialize_curr_step();
		}
#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
		else if (qdaf_item_result == ITEM_RESULT_INCOMPLETED) {
			QUEST_PRINT("%s : (step=%d) maybe booting after executing ITEM_SMDDLQDAF\n",
				    __func__, STEP_SMDDL);

			check_and_update_qdaf_result();
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
			quest_initialize_curr_step();
		} else if (exist_incompleted == 1) {
			QUEST_PRINT("%s : (step=%d) incompleted at boot items, so ignore running hlos and just run smddl qdaf\n",
				    __func__, STEP_SMDDL);

			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD, ITEM_RESULT_INCOMPLETED);
			quest_sync_param_quest_data();

			if (boot_count == 1) {
#if defined(CONFIG_SEC_QUEST_NOT_TRIGGER_SMDDL_QDAF)
				QUEST_PRINT("%s : SMDDL line, but skip to run SMDDL QDAF\n", __func__);
				check_and_update_qdaf_result();
				QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
				quest_initialize_curr_step();
#else
				QUEST_PRINT("%s : SMDDL line, so run SMDDL QDAF\n", __func__);
				run_qdaf_in_background(QUEST_QDAF_ACTION_CONTROL_START_WITHOUT_PANIC);
#endif
			} else {
				QUEST_PRINT("%s : ERASE seq, so do not run SMDDL QDAF and finish step\n", __func__);
				check_and_update_qdaf_result();
				QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
				quest_initialize_curr_step();
			}
		}
#endif
		break;
	}
	case STEP_TESTMODE: {
		int idx;
		int ddr_err_total = param_quest_ddr_result_data.ddr_err_addr_total;

		if (ddr_err_total != 0) {
			QUEST_PRINT("%s : ddr_err_addr_total=%d\n", __func__, ddr_err_total);

			if (ddr_err_total > MAX_DDR_ERR_ADDR_CNT)
				ddr_err_total = MAX_DDR_ERR_ADDR_CNT;

			for (idx = 0; idx < ddr_err_total; idx++) {
				QUEST_PRINT("ddr err addr : 0x%llx\n", param_quest_ddr_result_data.ddr_err_addr[idx]);
				param_quest_ddr_result_data.ddr_err_addr[idx] = 0;
			}
			param_quest_ddr_result_data.ddr_err_addr_total = 0;
			quest_sync_param_quest_ddr_result_data();

			QUEST_PRINT("%s : trigger panic\n", __func__);
			panic("ddrscan failed");
		}
		fallthrough;
	}
	case STEP_CAL1:
	case STEP_CALX:
		QUEST_PRINT("%s : (step=%d) cal scenario\n", __func__, param_quest_data.curr_step);

		move_questresult_to_sub_dir(param_quest_data.curr_step);

		if (param_quest_data.hlos_remained_count == 0) {
			QUEST_PRINT("%s : scenario ends\n", __func__);
			quest_initialize_curr_step();
		}
		break;
	case STEP_MAIN:
		QUEST_PRINT("%s : (step=%d) maybe booting completing main scenario\n", __func__, STEP_MAIN);

		move_questresult_to_sub_dir(STEP_MAIN);
		quest_initialize_curr_step();

#if !defined(CONFIG_SEC_QUEST_NOT_TRIGGER_MAIN_QDAF)
		call_main_qdaf_after_finighing_main_quest = 1;
#endif
		break;
	default: {
		int qdaf_failed_cnt;

		QUEST_PRINT("%s : (step=%d) default actions\n", __func__, param_quest_data.curr_step);

		qdaf_failed_cnt = get_qdaf_failed_cnt();
		QUEST_PRINT("%s : qdaf failed_cnt = %d\n", __func__, qdaf_failed_cnt);
		break;
	}
	}
}

static int initialized;
static void __initialize(void)
{
	if (likely(initialized))
		return;
	initialized = 1;

	QUEST_PRINT("%s +++\n", __func__);

	quest_load_param_quest_data();
	quest_load_param_quest_ddr_result_data();
	quest_load_param_api_gpio_test();
	quest_load_param_api_gpio_test_result();

	quest_print_param_quest_data();

#if defined(CONFIG_SEC_QUEST_BPS_CLASSIFIER)
	sec_quest_bps_param_read();
#endif

	make_debugging_files();
	check_abnormal_param();
	setup_scenario();

	QUEST_PRINT("%s ---\n", __func__);
}

static void quest_sysfs_enter(const char *function)
{
	mutex_lock(&sysfs_common_lock);
	__initialize();
	QUEST_PRINT("%s +++\n", function);
}

static void quest_sysfs_exit(const char *function)
{
	QUEST_PRINT("%s ---\n", function);
	mutex_unlock(&sysfs_common_lock);
}

#define QUEST_SYSFS_ENTER() quest_sysfs_enter(__func__)
#define QUEST_SYSFS_EXIT()  quest_sysfs_exit(__func__)

/* NAD_END */
static ssize_t nad_end_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	char result[20] = { '\0' };
	int failed = 0;
#if defined(CONFIG_SEC_QUEST_HLOS_NATURESCENE_SMD)
	enum quest_enum_item_result hlos_result = ITEM_RESULT_NONE;
#endif

	QUEST_SYSFS_ENTER();

	if (sscanf(buf, "%19s", result) != 1) {
		count = -EINVAL;
		goto out;
	}
	failed = strcmp(result, "quest_pass") ? 1 : 0;

	switch (param_quest_data.curr_step) {
	case STEP_SMDDL:
		if (failed) {
			QUEST_PRINT("%s : SMD quest_hlos was failed (%s)\n", __func__, result);
			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD, ITEM_RESULT_FAIL);
		} else {
			QUEST_PRINT("%s : SMD quest_hlos was succeeded\n", __func__);
			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD, ITEM_RESULT_PASS);
#if defined(CONFIG_SEC_QUEST_HLOS_NATURESCENE_SMD)
			hlos_result = QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_subitem_result, SUBITEM_QUESTHLOSNATURESCENE);
			if (hlos_result == ITEM_RESULT_INCOMPLETED) {
				QUEST_PRINT("%s : SUBITEM_QUESTHLOSNATURESCENE was incompleted, so update SMD quest_hlos as incompleted\n", __func__);
				QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD, ITEM_RESULT_INCOMPLETED);
			}
#endif
		}
		quest_sync_param_quest_data();

#if defined(CONFIG_SEC_DDR_SKP)
		param_quest_data.curr_step = STEP_CALX;
		quest_sync_param_quest_data();
#endif

#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
		if (boot_count == 1) {
#if defined(CONFIG_SEC_QUEST_NOT_TRIGGER_SMDDL_QDAF)
			QUEST_PRINT("%s : SMDDL line, but skip to run SMDDL QDAF\n", __func__);
			check_and_update_qdaf_result();
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
			quest_initialize_curr_step();
#else
			QUEST_PRINT("%s : SMDDL line, so run SMDDL QDAF\n", __func__);
			run_qdaf_in_background(QUEST_QDAF_ACTION_CONTROL_START_WITHOUT_PANIC);
#endif
		} else {
			QUEST_PRINT("%s : ERASE seq, so do not run SMDDL QDAF and finish step\n", __func__);
			check_and_update_qdaf_result();
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
			quest_initialize_curr_step();
		}
#else
		QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_subitem_result);
		quest_initialize_curr_step();
#endif

		kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, quest_uevent.envp);
#if defined(CONFIG_SEC_DDR_SKP)
		if (failed) {
			QUEST_PRINT("%s : trigger panic\n", __func__);
			panic("%s", result);
		}
#endif
		break;
	case STEP_CAL1:
	case STEP_CALX:
	case STEP_TESTMODE:
		if (failed) {
			QUEST_PRINT("%s : CAL quest_hlos was failed (%s)\n", __func__, result);
			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.cal_item_result, QUESTHLOS_HLOS_ITEM_MAIN_CAL, ITEM_RESULT_FAIL);
			quest_sync_param_quest_data();

			if (param_quest_data.curr_step == STEP_TESTMODE) {
				QUEST_PRINT("%s : ************* DO NOT INITIALIZE STEP AND COUNT for continuoly repeating *************\n", __func__);
				QUEST_PRINT("%s : trigger panic\n", __func__);
				panic("%s", result);
			}

			QUEST_PRINT("%s : initialize param and step\n", __func__);
			param_quest_data.hlos_remained_count = 0;
			param_quest_data.ddrscan_remained_count = 0;
#if defined(CONFIG_SEC_QUEST_UEFI)
			param_quest_data.quefi_remained_count = 0;
#endif
#if defined(CONFIG_SEC_QUEST_UEFI_ENHANCEMENT)
			param_quest_data.suefi_remained_count = 0;
#endif
			quest_initialize_curr_step();

			QUEST_PRINT("%s : trigger panic\n", __func__);
			panic("%s", result);
		} else {
			QUEST_PRINT("%s : CAL quest_hlos was succeeded\n", __func__);
			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.cal_item_result, QUESTHLOS_HLOS_ITEM_MAIN_CAL, ITEM_RESULT_PASS);
			quest_sync_param_quest_data();

			kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, quest_uevent.envp);
		}
		break;
	case STEP_MAIN:
		if (failed) {
			QUEST_PRINT("%s : MAIN quest_hlos was failed (%s)\n", __func__, result);
			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.main_item_result, QUESTHLOS_HLOS_ITEM_MAIN_CAL, ITEM_RESULT_FAIL);
			quest_sync_param_quest_data();

			QUEST_PRINT("%s : trigger panic\n", __func__);
			panic("%s", result);
		} else {
			QUEST_PRINT("%s : MAIN quest_hlos was succeeded\n", __func__);
			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.main_item_result, QUESTHLOS_HLOS_ITEM_MAIN_CAL, ITEM_RESULT_PASS);
			quest_sync_param_quest_data();

			kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, quest_uevent.envp);
		}
		break;
	}

out:
	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_WO(nad_end);

/* NAD_ACAT */
static ssize_t nad_acat_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	enum quest_enum_item_result total_result = ITEM_RESULT_NONE;
	ssize_t count = 0;

	QUEST_SYSFS_ENTER();

	total_result = check_item_result(param_quest_data.cal_item_result, ITEM_ITEMSCOUNT);
	QUEST_PRINT("%s : cal_item_result(%llu) total_result(%d)\n",
		    __func__, param_quest_data.cal_item_result, total_result);

#if defined(CONFIG_SEC_QUEST_ALWAYS_RETURN_PASS_FOR_ACAT)
	QUEST_PRINT("%s : CONFIG_SEC_QUEST_ALWAYS_RETURN_PASS_FOR_ACAT enabled, so return pass\n", __func__);
	total_result = ITEM_RESULT_PASS;
#endif

	if (param_quest_data.curr_step == STEP_TESTMODE && total_result == ITEM_RESULT_FAIL) {
		QUEST_PRINT("%s : in the case of STEP_TESTMODEC, force to return ITEM_RESULT_PASS to continue repeating\n", __func__);
		total_result = ITEM_RESULT_PASS;
	}

	switch (total_result) {
	case ITEM_RESULT_PASS:
		QUEST_PRINT("ACAT QUEST PASS\n");
		count = snprintf(buf, BUFF_SZ, "OK_ACAT_NONE\n");
		break;
	case ITEM_RESULT_FAIL:
		QUEST_PRINT("ACAT QUEST FAIL\n");
		count = snprintf(buf, BUFF_SZ, "NG_ACAT_ASV\n");
		break;
	case ITEM_RESULT_INCOMPLETED:
		QUEST_PRINT("ACAT QUEST INCOMPLETED\n");
		count = snprintf(buf, BUFF_SZ, "OK\n");
		break;
	case ITEM_RESULT_NONE:
		QUEST_PRINT("ACAT QUEST NOT_TESTED\n");
		count = snprintf(buf, BUFF_SZ, "OK\n");
		break;
	}

	QUEST_SYSFS_EXIT();
	return count;
}

static ssize_t nad_acat_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int idx = 0;
	int quest_loop_count, dram_loop_count;
	char temp[QUEST_BUFF_SIZE * 3];
	char quest_cmd[QUEST_CMD_LIST][QUEST_BUFF_SIZE];
	char *quest_ptr, *string;

	QUEST_SYSFS_ENTER();

	if (unlikely(erased || strncmp(buf, "nad_acat", 8))) {
		QUEST_PRINT("%s : exceptional cases (erased=%d, buf=%s)\n",
			    __func__, erased, buf);
		goto out;
	}

	memset(quest_cmd, 0, sizeof(quest_cmd));
	strscpy(temp, buf, sizeof(temp));
	string = temp;
	while (idx < QUEST_CMD_LIST && (quest_ptr = strsep(&string, ",")) != NULL)
		strscpy(quest_cmd[idx++], quest_ptr, sizeof(quest_cmd[idx - 1]));

	if (idx < 3) {
		count = -EINVAL;
		goto out;
	}

	if (kstrtoint(quest_cmd[1], 10, &quest_loop_count) ||
	    kstrtoint(quest_cmd[2], 10, &dram_loop_count)) {
		count = -EINVAL;
		goto out;
	}
	QUEST_PRINT("%s : nad_acat%d,%d\n", __func__, quest_loop_count, dram_loop_count);

	if (quest_loop_count == 0 && dram_loop_count == 0) {
		if (unlikely(step_to_smd_quest_hlos && param_quest_data.curr_step == STEP_SMDDL)) {
			QUEST_PRINT("%s : cur_step==STEP_SMDDL and will triger quest_hlos\n", __func__);
			QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD, ITEM_RESULT_INCOMPLETED);
			quest_sync_param_quest_data();
			do_quest();
			goto out;
		}

		if (unlikely((param_quest_data.curr_step != STEP_TESTMODE &&
			      param_quest_data.curr_step != STEP_CALX &&
			      param_quest_data.curr_step != STEP_CAL1) ||
			     param_quest_data.hlos_remained_count <= 0)) {
			QUEST_PRINT("%s : exceptional cases (step=%d, hlos_cnt=%d)\n",
				    __func__, param_quest_data.curr_step, param_quest_data.hlos_remained_count);
			goto out;
		}

		QUEST_PRINT("%s : will triger quest_hlos\n", __func__);
		param_quest_data.hlos_remained_count--;
		QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.cal_item_result, QUESTHLOS_HLOS_ITEM_MAIN_CAL, ITEM_RESULT_INCOMPLETED);
		quest_sync_param_quest_data();

		do_quest();
	} else {
		QUEST_PRINT("%s : update step, item_result and remained_count\n", __func__);

		if (testmode_enabled) {
			QUEST_PRINT("%s : testmode_enabled, so set step as STEP_TESTMODE\n", __func__);
			param_quest_data.curr_step = STEP_TESTMODE;
			param_quest_data.cal_item_result = 0;
			param_quest_data.hlos_remained_count = quest_loop_count;
			param_quest_data.quefi_remained_count = 0;
			param_quest_data.suefi_remained_count = 0;
			param_quest_data.ddrscan_remained_count = 0;

			QUEST_PRINT("%s : testmode_enabled, so set count as feature and property value\n", __func__);
#if defined(CONFIG_SEC_QUEST_UEFI)
			if (testmode_quefi_enabled)
				param_quest_data.quefi_remained_count = quest_loop_count;
#endif
#if defined(CONFIG_SEC_QUEST_UEFI_ENHANCEMENT)
			if (testmode_suefi_enabled)
				param_quest_data.suefi_remained_count = quest_loop_count;
#endif
			if (testmode_ddrscan_enabled)
				param_quest_data.ddrscan_remained_count = quest_loop_count;
		} else {
			if (quest_loop_count == 1 && dram_loop_count == 0) {
				param_quest_data.curr_step = STEP_CAL1;
				param_quest_data.cal_item_result = 0;
				param_quest_data.ddrscan_remained_count = 0;
				param_quest_data.hlos_remained_count = quest_loop_count;
				param_quest_data.quefi_remained_count = 0;
				param_quest_data.suefi_remained_count = 0;
			} else {
				param_quest_data.curr_step = STEP_CALX;
				param_quest_data.cal_item_result = 0;
				param_quest_data.ddrscan_remained_count = dram_loop_count;
				param_quest_data.hlos_remained_count = quest_loop_count;

#if defined(CONFIG_SEC_QUEST_UEFI)
				if (quest_loop_count != 1)
					param_quest_data.quefi_remained_count = quest_loop_count;
				else
					param_quest_data.quefi_remained_count = 0;
#else
				param_quest_data.quefi_remained_count = 0;
#endif
#if defined(CONFIG_SEC_QUEST_UEFI_ENHANCEMENT)
				if (quest_loop_count != 1)
					param_quest_data.suefi_remained_count = quest_loop_count;
				else
					param_quest_data.suefi_remained_count = 0;
#else
				param_quest_data.suefi_remained_count = 0;
#endif
			}
		}
		quest_sync_param_quest_data();

		QUEST_PRINT("%s : not trigger quest_hlos and not reboot\n", __func__);
	}

out:
	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_RW(nad_acat);

/* NAD_STAT */
#if defined(CONFIG_SEC_QUEST_BPS_CLASSIFIER)
static void make_bps_stat_string(char *bps_str, size_t len)
{
	snprintf(bps_str, len, "%d_%d_%d_%d_%d_%d_%d_%d_%d_%d_%d_%d",
		 bps_envs.up_cnt.sp, bps_envs.up_cnt.wp, bps_envs.up_cnt.dp,
		 bps_envs.up_cnt.kp, bps_envs.up_cnt.mp, bps_envs.up_cnt.tp,
		 bps_envs.up_cnt.cp, bps_envs.pc_lr_cnt, bps_envs.pc_lr_last_idx,
		 bps_envs.tzerr_cnt, bps_envs.klg_cnt, bps_envs.dn_cnt);
}
#endif

static void make_additional_stat_string(char *additional_str, size_t max_len)
{
	char str_bps[CPR_BPS_SZ_BYTE];

#if defined(CONFIG_SEC_QUEST_BPS_CLASSIFIER)
	if (sec_quest_bps_env_initialized &&
	    bps_envs.magic[1] == QUEST_BPS_CLASSIFIER_MAGIC2)
		make_bps_stat_string(str_bps, sizeof(str_bps));
	else
		snprintf(str_bps, sizeof(str_bps), "-");
#else
	snprintf(str_bps, sizeof(str_bps), "-");
#endif

	snprintf(additional_str, max_len,
		 "FQUITH(%d),FQUETH(%d),FSUITH(%d),FSUETH(%d),FSSIR(%llX),FDET(%d),FQUET(%d),FSUET(%d),FQUPT(%d),FBR(%.2s),FHST(%d),FHET(%d),FHITH(%d),FHMTH(%d),FNSR(%d),FMATD(%d),SSIR(%llX),DET(%d),QUET(%d),SUET(%d),QUPT(%d),SCT(%d),SCTH(%d),BR(%.2s),HST(%d),HET(%d),HITH(%d),HMTH(%d),NSR(%d),MATD(%d),BPS(%s),CHIP(%llX),CPER(%d)",
		 param_quest_data.smd_quefi_init_thermal_first, param_quest_data.smd_quefi_end_thermal_first,
		 param_quest_data.smd_suefi_init_thermal_first, param_quest_data.smd_suefi_end_thermal_first,
		 param_quest_data.smd_subitem_result_first,
		 param_quest_data.smd_ddrscan_elapsed_time_first,
		 param_quest_data.smd_quefi_elapsed_time_first,
		 param_quest_data.smd_suefi_elapsed_time_first,
		 param_quest_data.smd_quefi_total_pause_time_first,
		 param_quest_data.smd_boot_reason_first,
		 param_quest_data.smd_hlos_start_time_first, param_quest_data.smd_hlos_elapsed_time_first,
		 param_quest_data.smd_hlos_init_thermal_first, param_quest_data.smd_hlos_max_thermal_first,
		 param_quest_data.smd_ns_repeats_first,
		 param_quest_data.smd_max_aoss_thermal_diff_first,
		 param_quest_data.smd_subitem_result,
		 param_quest_data.smd_ddrscan_elapsed_time,
		 param_quest_data.smd_quefi_elapsed_time,
		 param_quest_data.smd_suefi_elapsed_time,
		 param_quest_data.smd_quefi_total_pause_time,
		 param_quest_data.smd_ft_self_cooling_time,
		 param_quest_data.smd_ft_thermal_after_self_cooling,
		 param_quest_data.smd_boot_reason,
		 param_quest_data.smd_hlos_start_time, param_quest_data.smd_hlos_elapsed_time,
		 param_quest_data.smd_hlos_init_thermal, param_quest_data.smd_hlos_max_thermal,
		 param_quest_data.smd_ns_repeats,
		 param_quest_data.smd_max_aoss_thermal_diff,
		 str_bps,
		 (param_quest_data.ap_serial) ? ((param_quest_data.real_smd_register_value) ?
			param_quest_data.ap_serial ^ (0xfff00000 | param_quest_data.real_smd_register_value) : 0) : 0,
		 param_quest_data.smd_cper);

	QUEST_PRINT("%s : additional_str : %s\n", __func__, additional_str);
}

static ssize_t nad_stat_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
	enum quest_enum_item_result qdaf_result = ITEM_RESULT_NONE;
	uint64_t smd_item_result_without_qdaf;
	int exist_incompleted = 0;
#endif
	enum quest_enum_item_result total_result = ITEM_RESULT_NONE;
	enum quest_enum_item_result hlos_result = ITEM_RESULT_NONE;
	ssize_t count = 0;
	char additional_str[MAX_LEN_STR] = { '\0' };

	QUEST_SYSFS_ENTER();

#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
	qdaf_result = QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, ITEM_SMDDLQDAF);
	if (qdaf_result == ITEM_RESULT_PASS || qdaf_result == ITEM_RESULT_FAIL) {
		QUEST_PRINT("%s : ITEM_SMDDLQDAF was completed, so include its result into total_result\n", __func__);
		total_result = check_item_result(param_quest_data.smd_item_result, ITEM_ITEMSCOUNT);
		exist_incompleted = check_if_incompleted_item_result_exist(param_quest_data.smd_item_result, ITEM_ITEMSCOUNT);
	} else {
		QUEST_PRINT("%s : ITEM_SMDDLQDAF was not completed, so ignore its result from total_result\n", __func__);
		smd_item_result_without_qdaf = param_quest_data.smd_item_result;
		QUEST_SET_ITEM_SUBITEM_RESULT(smd_item_result_without_qdaf, ITEM_SMDDLQDAF, ITEM_RESULT_PASS);
		total_result = check_item_result(smd_item_result_without_qdaf, ITEM_ITEMSCOUNT);
		exist_incompleted = check_if_incompleted_item_result_exist(smd_item_result_without_qdaf, ITEM_ITEMSCOUNT);
	}
	QUEST_PRINT("%s : smd_item_result(%llu) total_result(%d)\n",
		    __func__, param_quest_data.smd_item_result, total_result);

	if ((total_result == ITEM_RESULT_FAIL && exist_incompleted) ||
	    param_quest_data.curr_step != STEP_SMDDL) {
		QUEST_PRINT("%s : in this case, the curr_step is none due to incompleted subitem\n", __func__);
		QUEST_PRINT("%s : let's skip hlos subitem as the policy with incompletion\n", __func__);
	} else if (total_result != ITEM_RESULT_INCOMPLETED) {
		hlos_result = QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD);
		QUEST_PRINT("%s : the result of quest_hlos at smd_item_result (%d)\n",
			    __func__, hlos_result);
		if (hlos_result == ITEM_RESULT_NONE) {
			QUEST_PRINT("%s : set step_to_smd_quest_hlos=1\n", __func__);
			step_to_smd_quest_hlos = 1;
			QUEST_PRINT("SMD QUEST NOT_TESTED\n");
			count = snprintf(buf, MAX_LEN_STR, "NOT_TESTED\n");
			goto out;
		} else if (hlos_result == ITEM_RESULT_INCOMPLETED &&
			   param_quest_data.curr_step == STEP_SMDDL) {
			QUEST_PRINT("SMD QUEST TESTING\n");
			count = snprintf(buf, MAX_LEN_STR, "TESTING\n");
			goto out;
		}
	}
#else
	if (param_quest_data.curr_step == STEP_SMDDL) {
		hlos_result = QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.smd_item_result, QUESTHLOS_HLOS_ITEM_SMD);
		QUEST_PRINT("%s : hlos_result(%d) total_result(%d)\n",
			    __func__, hlos_result, total_result);

		if (hlos_result == ITEM_RESULT_NONE) {
			QUEST_PRINT("%s : set step_to_smd_quest_hlos=1\n", __func__);
			step_to_smd_quest_hlos = 1;
			QUEST_PRINT("SMD QUEST NOT_TESTED\n");
			count = snprintf(buf, MAX_LEN_STR, "NOT_TESTED\n");
			goto out;
		} else if (hlos_result == ITEM_RESULT_INCOMPLETED) {
			QUEST_PRINT("SMD QUEST TESTING\n");
			count = snprintf(buf, MAX_LEN_STR, "TESTING\n");
			goto out;
		}
	}

	total_result = check_item_result(param_quest_data.smd_item_result, ITEM_ITEMSCOUNT);
#endif

	make_additional_stat_string(additional_str, sizeof(additional_str));

	switch (total_result) {
	case ITEM_RESULT_PASS:
		QUEST_PRINT("%s : SMD QUEST PASS\n", __func__);
		count = snprintf(buf, MAX_LEN_STR, "OK_3.1,%s\n", additional_str);
		break;
	case ITEM_RESULT_FAIL: {
		char str_result[BUFF_SZ - 14] = { '\0' };

		get_smd_subitem_result_string(str_result, sizeof(str_result), SUBITEM_ITEMSCOUNT);
		QUEST_PRINT("%s : SMD QUEST FAIL\n", __func__);
		count = snprintf(buf, MAX_LEN_STR, "NG_3.1_FAIL_%s,%s\n", str_result, additional_str);
		break;
	}
	case ITEM_RESULT_INCOMPLETED:
		QUEST_PRINT("%s : SMD QUEST INCOMPLETED\n", __func__);
		count = snprintf(buf, MAX_LEN_STR, "RE_WORK,%s\n", additional_str);
		break;
	case ITEM_RESULT_NONE:
		QUEST_PRINT("%s : SMD QUEST NOT_TESTED\n", __func__);
		count = snprintf(buf, MAX_LEN_STR, "NOT_TESTED\n");
		break;
	}

out:
	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_RO(nad_stat);

/* NAD_RESULT */
static ssize_t nad_result_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	ssize_t info_size = 0;
	int i;

	QUEST_SYSFS_ENTER();

	for (i = SUBITEM_NONE + 1; i < SUBITEM_ITEMSCOUNT; i++) {
		char str_result[QUEST_BUFF_SIZE] = { '\0' };

		get_smd_subitem_result_string(str_result, sizeof(str_result), i);
		info_size += snprintf(buf + info_size, MAX_LEN_STR - info_size,
				      "\"%s\":\"%s\",", STR_SUBITEM[i], str_result);
	}
	info_size += snprintf(buf + info_size, MAX_LEN_STR - info_size, "\n");

	QUEST_PRINT("%s : smd_subitem_result(%llu)=%s\n", __func__, param_quest_data.smd_subitem_result, buf);

	QUEST_SYSFS_EXIT();
	return info_size;
}

static ssize_t nad_result_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	enum quest_enum_item_result _result = ITEM_RESULT_NONE;
	enum quest_enum_smd_subitem item = SUBITEM_NONE;
	char test_name[QUEST_BUFF_SIZE * 2] = { '\0' };
	char result_string[QUEST_BUFF_SIZE] = { '\0' };
	char temp[QUEST_BUFF_SIZE * 3] = { '\0' };
	char quest_test[2][QUEST_BUFF_SIZE * 2];
	char *quest_ptr, *string;
	int idx = 0;

	QUEST_SYSFS_ENTER();

	memset(quest_test, 0, sizeof(quest_test));
	strscpy(temp, buf, sizeof(temp));
	string = temp;
	while (idx < 2 && (quest_ptr = strsep(&string, ",")) != NULL)
		strscpy(quest_test[idx++], quest_ptr, sizeof(quest_test[idx - 1]));

	if (idx < 2)
		goto out;

	if (sscanf(quest_test[0], "%s", test_name) != 1 ||
	    sscanf(quest_test[1], "%s", result_string) != 1)
		goto out;

	QUEST_PRINT("%s : test_name(%s), test result(%s)\n", __func__, test_name, result_string);

	if (TEST_PASS(result_string))
		_result = ITEM_RESULT_PASS;
	else if (TEST_FAIL(result_string))
		_result = ITEM_RESULT_FAIL;
	else if (TEST_NA(result_string))
		_result = ITEM_RESULT_INCOMPLETED;
	else
		_result = ITEM_RESULT_NONE;

#if defined(CONFIG_SEC_QUEST_HLOS_DUMMY_SMD)
	if (TEST_QDAF(test_name))
		item = SUBITEM_SMDDLQDAF;
	else if (TEST_DUMMY(test_name))
		item = SUBITEM_QUESTHLOSDUMMY;
#elif defined(CONFIG_SEC_QUEST_HLOS_NATURESCENE_SMD)
	if (TEST_NATURESCENE(test_name))
		item = SUBITEM_QUESTHLOSNATURESCENE;
	else if (TEST_AOSSTHERMALDIFF(test_name))
		item = SUBITEM_QUESTHLOSAOSSTHERMALDIFF;
#else
	if (TEST_CRYPTO(test_name))
		item = SUBITEM_QUESTHLOSCRYPTO;
	else if (TEST_ICACHE(test_name))
		item = SUBITEM_QUESTHLOSICACHE;
	else if (TEST_CCOHERENCY(test_name))
		item = SUBITEM_QUESTHLOSCCOHERENCY;
	else if (TEST_QMESADDR(test_name))
		item = SUBITEM_QUESTHLOSQMESADDR;
	else if (TEST_QMESACACHE(test_name))
		item = SUBITEM_QUESTHLOSQMESACACHE;
	else if (TEST_SUSPEND(test_name))
		item = SUBITEM_QUESTHLOSSUSPEND;
	else if (TEST_VDDMIN(test_name))
		item = SUBITEM_QUESTHLOSVDDMIN;
	else if (TEST_THERMAL(test_name))
		item = SUBITEM_QUESTHLOSTHERMAL;
	else if (TEST_UFS(test_name))
		item = SUBITEM_QUESTHLOSUFS;
	else if (TEST_A75G(test_name))
		item = SUBITEM_QUESTFUSIONA75G;
	else if (TEST_Q65G(test_name))
		item = SUBITEM_QUESTFUSIONQ65G;
#endif

	if (unlikely(item == SUBITEM_NONE || param_quest_data.curr_step != STEP_SMDDL)) {
		QUEST_PRINT("%s : exceptional cases (item=%d, step=%d)\n",
			    __func__, item, param_quest_data.curr_step);
		goto out;
	}

	QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.smd_subitem_result, item, _result);
	quest_sync_param_quest_data();

out:
	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_RW(nad_result);

/* NAD_ERASE */
static ssize_t nad_erase_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	char *argv[4] = { NULL, NULL, NULL, NULL };

	QUEST_SYSFS_ENTER();

	if (unlikely(erased || strncmp(buf, "erase", 5))) {
		QUEST_PRINT("%s : exceptional cases (erased=%d, buf=%s)\n",
			    __func__, erased, buf);
		goto out;
	}

	erased = 1;

	argv[0] = ERASE_QUEST_PRG;
	argv[1] = "all";
	call_user_prg(argv, UMH_WAIT_EXEC);

	quest_clear_param_quest_data();

	if (!strncmp(buf, "eraseall", 8)) {
		QUEST_PRINT("%s : clear also first_xxx just for debugging purpose\n", __func__);
		param_quest_data.smd_subitem_result_first = 0;
		param_quest_data.smd_quefi_init_thermal_first = 0;
		param_quest_data.smd_quefi_end_thermal_first = 0;
		param_quest_data.smd_suefi_init_thermal_first = 0;
		param_quest_data.smd_suefi_end_thermal_first = 0;
		param_quest_data.smd_ddrscan_elapsed_time_first = 0;
		param_quest_data.smd_quefi_elapsed_time_first = 0;
		param_quest_data.smd_suefi_elapsed_time_first = 0;
		param_quest_data.smd_quefi_total_pause_time_first = 0;
		param_quest_data.smd_boot_reason_first[0] = '\0';
		param_quest_data.smd_hlos_start_time_first = 0;
		param_quest_data.smd_hlos_elapsed_time_first = 0;
		param_quest_data.smd_hlos_init_thermal_first = 0;
		param_quest_data.smd_hlos_max_thermal_first = 0;
		param_quest_data.smd_ns_repeats_first = 0;
		param_quest_data.smd_max_aoss_thermal_diff_first = 0;
		param_quest_data.real_smd_register_value = 0;
		param_quest_data.ap_serial = 0;
		param_quest_data.num_smd_try = 0;
		param_quest_data.smd_cper = 0;
		quest_sync_param_quest_data();
	}

	param_api_gpio_test = 0;
	param_api_gpio_test_result[0] = '\0';
	quest_sync_param_api_gpio_test();
	quest_sync_param_api_gpio_test_result();

out:
	QUEST_SYSFS_EXIT();
	return count;
}

static ssize_t nad_erase_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int is_erased;

	mutex_lock(&sysfs_common_lock);
	is_erased = erased;
	mutex_unlock(&sysfs_common_lock);

	if (is_erased)
		return snprintf(buf, BUFF_SZ, "OK\n");

	return snprintf(buf, BUFF_SZ, "NG\n");
}
static DEVICE_ATTR_RW(nad_erase);

/* BALANCER */
static ssize_t balancer_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	QUEST_SYSFS_ENTER();

	if (unlikely(erased || param_quest_data.curr_step == STEP_MAIN || strncmp(buf, "start", 5))) {
		QUEST_PRINT("%s : exceptional cases (erased=%d, curr_step=%d, buf=%s)\n",
			    __func__, erased, param_quest_data.curr_step, buf);
		goto out;
	}

	QUEST_PRINT("%s : start STEP_MAIN\n", __func__);
	param_quest_data.curr_step = STEP_MAIN;
	param_quest_data.main_item_result = 0;
#if defined(CONFIG_SEC_A73XQ_PROJECT)
	param_quest_data.hlos_remained_count = 3;
#else
	param_quest_data.hlos_remained_count = 1;
#endif
#if defined(CONFIG_SEC_QUEST_UEFI)
#if defined(CONFIG_SEC_A73XQ_PROJECT)
	param_quest_data.quefi_remained_count = 3 * MAIN_QUEST_QUEFI_REPEATS;
#else
	param_quest_data.quefi_remained_count = MAIN_QUEST_QUEFI_REPEATS;
#endif
#endif
#if defined(CONFIG_SEC_QUEST_UEFI_ENHANCEMENT)
#if defined(CONFIG_SEC_A73XQ_PROJECT)
	param_quest_data.suefi_remained_count = 3;
#else
	param_quest_data.suefi_remained_count = 1;
#endif
#endif
#if defined(CONFIG_SEC_A73XQ_PROJECT)
	param_quest_data.ddrscan_remained_count = 3;
#else
	param_quest_data.ddrscan_remained_count = 1;
#endif

	param_quest_data.hlos_remained_count--;
	QUEST_SET_ITEM_SUBITEM_RESULT(param_quest_data.main_item_result, QUESTHLOS_HLOS_ITEM_MAIN_CAL, ITEM_RESULT_INCOMPLETED);

	quest_sync_param_quest_data();

	do_quest();

out:
	QUEST_SYSFS_EXIT();
	return count;
}

static ssize_t balancer_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int count;
	enum quest_enum_item_result total_result = ITEM_RESULT_NONE;

	QUEST_SYSFS_ENTER();

	if (call_main_qdaf_after_finighing_main_quest == 1) {
		QUEST_PRINT("%s : call MAIN QDAF\n", __func__);
		run_qdaf_in_background(QUEST_QDAF_ACTION_CONTROL_START_WITH_PANIC);
	}

	total_result = check_item_result(param_quest_data.main_item_result, ITEM_ITEMSCOUNT);
	QUEST_PRINT("%s : main_item_result(%llu) total_result(%d)\n",
		    __func__, param_quest_data.main_item_result, total_result);
	switch (total_result) {
	case ITEM_RESULT_PASS:
		QUEST_PRINT("MAIN QUEST PASS\n");
		count = snprintf(buf, BUFF_SZ, "OK_2.0\n");
		break;
	case ITEM_RESULT_FAIL:
		QUEST_PRINT("MAIN QUEST FAIL\n");
		count = snprintf(buf, BUFF_SZ, "NG_2.0_FAIL\n");
		break;
	case ITEM_RESULT_INCOMPLETED:
		QUEST_PRINT("MAIN QUEST INCOMPLETED\n");
		count = snprintf(buf, BUFF_SZ, "OK\n");
		break;
	case ITEM_RESULT_NONE:
		QUEST_PRINT("MAIN QUEST NOT_TESTED\n");
		count = snprintf(buf, BUFF_SZ, "OK\n");
		break;
	}

	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_RW(balancer);

/* MAIN_NAD_TIMEOUT */
static ssize_t timeout_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	QUEST_SYSFS_ENTER();
	QUEST_SYSFS_EXIT();
	return snprintf(buf, BUFF_SZ, "%d\n", STEP_MAIN_HLOS_TIMEOUT);
}
static DEVICE_ATTR_RO(timeout);

/* MAIN_NAD_RUN */
static ssize_t run_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	QUEST_SYSFS_ENTER();
	QUEST_SYSFS_EXIT();
	return count;
}

static ssize_t run_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	QUEST_SYSFS_ENTER();
	QUEST_SYSFS_EXIT();
	return snprintf(buf, BUFF_SZ, "END\n");
}
static DEVICE_ATTR_RW(run);

/* NAD_QMVS_REMAIN_COUNT */
static ssize_t nad_qmvs_remain_count_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	ssize_t count;

	QUEST_SYSFS_ENTER();
	count = snprintf(buf, BUFF_SZ, "%d\n", param_quest_data.hlos_remained_count);
	QUEST_SYSFS_EXIT();

	return count;
}
static DEVICE_ATTR_RO(nad_qmvs_remain_count);

/* NAD_DDRTEST_REMAIN_COUNT */
static ssize_t nad_ddrtest_remain_count_show(struct device *dev,
					     struct device_attribute *attr, char *buf)
{
	ssize_t count;

	QUEST_SYSFS_ENTER();
	count = snprintf(buf, BUFF_SZ, "%d\n", param_quest_data.ddrscan_remained_count);
	QUEST_SYSFS_EXIT();

	return count;
}
static DEVICE_ATTR_RO(nad_ddrtest_remain_count);

/* NAD_DRAM */
static ssize_t nad_dram_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	ssize_t count;
	enum quest_enum_item_result ddrscan_result = ITEM_RESULT_NONE;

	QUEST_SYSFS_ENTER();

	ddrscan_result = QUEST_GET_ITEM_SUBITEM_RESULT(param_quest_data.cal_item_result, ITEM_DDRSCANRAMDUMPDISCACHE);

#if defined(CONFIG_SEC_QUEST_ALWAYS_RETURN_PASS_FOR_ACAT)
	QUEST_PRINT("%s : CONFIG_SEC_QUEST_ALWAYS_RETURN_PASS_FOR_ACAT enabled, so return pass\n", __func__);
	ddrscan_result = ITEM_RESULT_PASS;
#endif

	if (ddrscan_result == ITEM_RESULT_PASS)
		count = snprintf(buf, BUFF_SZ, "OK_DRAM\n");
	else if (ddrscan_result == ITEM_RESULT_FAIL)
		count = snprintf(buf, BUFF_SZ, "NG_DRAM_DATA\n");
	else
		count = snprintf(buf, BUFF_SZ, "NO_DRAMTEST\n");

	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_RO(nad_dram);

/* NAD_DRAM_ERR_ADDR */
static ssize_t nad_dram_err_addr_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	ssize_t count;
	u32 total;
	int i;

	QUEST_SYSFS_ENTER();

	total = min_t(u32, param_quest_ddr_result_data.ddr_err_addr_total,
		      MAX_DDR_ERR_ADDR_CNT);

	count = scnprintf(buf, PAGE_SIZE, "Total : %d\n\n",
			  param_quest_ddr_result_data.ddr_err_addr_total);

	if (total > 0 && count > 1) {
		/* Overwrite second newline to keep entry lines compact per ABI */
		count--;
		for (i = 0; i < total; i++) {
			count += scnprintf(buf + count, PAGE_SIZE - count,
					   "[%d] 0x%llx\n", i,
					   param_quest_ddr_result_data.ddr_err_addr[i]);
		}
	}

	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_RO(nad_dram_err_addr);

/* NAD_SUPPORT */
static ssize_t nad_support_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return snprintf(buf, BUFF_SZ, "SUPPORT\n");
}
static DEVICE_ATTR_RO(nad_support);

/* NAD_LOGS */
static ssize_t nad_logs_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int fd, idx = 0;
	char path[100] = { '\0' };
	char temp[1] = { '\0' };
	char tempbuf[BUFF_SZ] = { '\0' };
	mm_segment_t old_fs = get_fs();

	QUEST_PRINT("%s : file = %s\n", __func__, buf);

	set_fs(KERNEL_DS);
	if (sscanf(buf, "%99s", path) != 1) {
		set_fs(old_fs);
		return -EINVAL;
	}

	fd = ksys_open(path, O_RDONLY, 0);
	if (fd >= 0) {
		while (ksys_read(fd, temp, 1) == 1) {
			if (idx >= BUFF_SZ - 1) {
				tempbuf[idx] = '\0';
				QUEST_PRINT("%s", tempbuf);
				idx = 0;
			}
			tempbuf[idx++] = temp[0];
			if (temp[0] == '\n') {
				tempbuf[idx] = '\0';
				QUEST_PRINT("%s", tempbuf);
				idx = 0;
			}
		}
		ksys_close(fd);
	} else {
		QUEST_PRINT("%s : file does not exist\n", __func__);
	}
	set_fs(old_fs);

	return count;
}
static DEVICE_ATTR_WO(nad_logs);

/* NAD_API */
static ssize_t nad_api_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	ssize_t count;

	QUEST_SYSFS_ENTER();

	if (param_api_gpio_test)
		count = snprintf(buf, BUFF_SZ, "%s", param_api_gpio_test_result);
	else
		count = snprintf(buf, BUFF_SZ, "NONE\n");

	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_RO(nad_api);

/* NAD_INFO */
static ssize_t nad_info_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	char info_name[QUEST_BUFF_SIZE * 2] = { '\0' };
	char temp[QUEST_BUFF_SIZE * 3] = { '\0' };
	char quest_test[2][QUEST_BUFF_SIZE * 2];
	char *quest_ptr, *string;
	int result_val;
	int idx = 0;

	QUEST_SYSFS_ENTER();

	QUEST_PRINT("buf : %s count : %d\n", buf, (int)count);
	if (count > sizeof(temp) || count < 4) {
		QUEST_PRINT("result cmd size too long : QUEST_BUFF_SIZE<%d\n", (int)count);
		count = -EINVAL;
		goto out;
	}

	memset(quest_test, 0, sizeof(quest_test));
	strscpy(temp, buf, sizeof(temp));
	string = temp;
	while (idx < 2 && (quest_ptr = strsep(&string, ",")) != NULL)
		strscpy(quest_test[idx++], quest_ptr, sizeof(quest_test[idx - 1]));

	if (idx < 2) {
		count = -EINVAL;
		goto out;
	}

	if (sscanf(quest_test[0], "%s", info_name) != 1 ||
	    kstrtoint(quest_test[1], 10, &result_val)) {
		count = -EINVAL;
		goto out;
	}

	if (!strcmp("thermal", info_name))
		param_quest_data.thermal = result_val;
	else if (!strcmp("clock", info_name))
		param_quest_data.tested_clock = result_val;

	QUEST_PRINT("info_name : %s, result=%d\n", info_name, result_val);
	quest_sync_param_quest_data();

out:
	QUEST_SYSFS_EXIT();
	return count;
}

static ssize_t nad_info_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	ssize_t info_size = 0;

	QUEST_SYSFS_ENTER();

	info_size += snprintf(buf + info_size, MAX_LEN_STR - info_size,
			      "\"REMAIN_CNT\":\"%d\",",
			      param_quest_data.hlos_remained_count);
	info_size += snprintf(buf + info_size, MAX_LEN_STR - info_size,
			      "\"THERMAL\":\"%d\",", param_quest_data.thermal);
	info_size += snprintf(buf + info_size, MAX_LEN_STR - info_size,
			      "\"CLOCK\":\"%d\",", param_quest_data.tested_clock);

	QUEST_SYSFS_EXIT();
	return info_size;
}
static DEVICE_ATTR_RW(nad_info);

/* NAD_VERSION */
static ssize_t nad_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	QUEST_SYSFS_ENTER();
	QUEST_SYSFS_EXIT();
	return snprintf(buf, BUFF_SZ, "SM8150.0103.01.1030RELEASE\n");
}
static DEVICE_ATTR_RO(nad_version);

#if defined(CONFIG_SEC_QUEST_AUTO_TRIGGER_KWORKER) || defined(CONFIG_SEC_QUEST_AUTO_TRIGGER_INIT_WRITE)
static void quest_auto_trigger(const char *test_name)
{
	char *argv[4] = { QUESTHLOS_PROG_MAIN_CAL, "logPath:/data/log/quest", "Reboot", NULL };
	char *envp[5] = {
		"HOME=/",
		"PATH=/system/bin/quest:/system/bin:/system/xbin",
		"ANDROID_DATA=/data",
		"ANDROID_ROOT=/system",
		NULL
	};
	int ret;
	int run_hlos = 0;

	mutex_lock(&sysfs_common_lock);
	initialized = 1;

	QUEST_PRINT("%s : will trigger quest\n", __func__);
	QUEST_PRINT("%s : test_name (%s)\n", __func__, test_name);

	if (!strncmp(test_name, "KILLNOW", 7)) {
		QUEST_PRINT("%s : will kill quests.sh now\n", __func__);
		mutex_unlock(&sysfs_common_lock);

		argv[0] = QUEST_DEBUGGING_PRG;
		argv[1] = "action:killnow";
		argv[2] = NULL;
		ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
		QUEST_PRINT("%s : call_usermodehelper(ret=%d)\n", __func__, ret);
		return;
	}
	if (!strncmp(test_name, "SYSREBOOT", 9)) {
		QUEST_PRINT("%s : will reboot system now\n", __func__);
		mutex_unlock(&sysfs_common_lock);

		argv[0] = QUEST_DEBUGGING_PRG;
		argv[1] = "action:sysreboot";
		argv[2] = NULL;
		ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
		QUEST_PRINT("%s : call_usermodehelper(ret=%d)\n", __func__, ret);
		return;
	}

	if (!strncmp(test_name, "HLOSUEFI", 8) || !strncmp(test_name, "UEFIHLOS", 8)) {
		QUEST_PRINT("%s : set CALX\n", __func__);
		param_quest_data.curr_step = STEP_CALX;
		param_quest_data.hlos_remained_count = 1;
		param_quest_data.quefi_remained_count = 1;
		param_quest_data.suefi_remained_count = 1;
		param_quest_data.ddrscan_remained_count = 0;
		quest_sync_param_quest_data();
	} else if (!strncmp(test_name, "HLOSONLY", 8)) {
		QUEST_PRINT("%s : set CALX\n", __func__);
		param_quest_data.curr_step = STEP_CALX;
		param_quest_data.hlos_remained_count = 1;
		param_quest_data.quefi_remained_count = 0;
		param_quest_data.suefi_remained_count = 0;
		param_quest_data.ddrscan_remained_count = 0;
		quest_sync_param_quest_data();
	} else if (!strncmp(test_name, "UEFIONLY", 8)) {
		QUEST_PRINT("%s : set CALX\n", __func__);
		param_quest_data.curr_step = STEP_CALX;
		param_quest_data.hlos_remained_count = 0;
		param_quest_data.quefi_remained_count = 1;
		param_quest_data.suefi_remained_count = 1;
		param_quest_data.ddrscan_remained_count = 0;
		quest_sync_param_quest_data();
	} else {
		QUEST_PRINT("%s : wrong test_name\n", __func__);
		mutex_unlock(&sysfs_common_lock);
		return;
	}

	if (param_quest_data.hlos_remained_count > 0) {
		QUEST_PRINT("%s : run hlos\n", __func__);
		param_quest_data.hlos_remained_count--;
		quest_sync_param_quest_data();
		run_hlos = 1;
	}

	mutex_unlock(&sysfs_common_lock);

	if (run_hlos) {
		ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
		QUEST_PRINT("%s : call_usermodehelper(ret=%d)\n", __func__, ret);
	} else {
		msleep(3000);
		kernel_restart(NULL);
		QUEST_PRINT("%s : reboot and run uefi\n", __func__);
	}
}
#endif

#if defined(CONFIG_SEC_QUEST_AUTO_TRIGGER_INIT_WRITE)
static ssize_t nad_auto_trigger_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	char test_name[QUEST_BUFF_SIZE * 3] = { '\0' };

	strscpy(test_name, buf, sizeof(test_name));
	quest_auto_trigger(test_name);
	return count;
}
static DEVICE_ATTR_WO(nad_auto_trigger);
#endif

#if defined(CONFIG_SEC_QUEST_AUTO_TRIGGER_KWORKER)
static void delayed_quest_work_func(struct work_struct *work)
{
	quest_auto_trigger("HLOSUEFI");
}
#endif

/* NAD_TESTMODE */
static ssize_t nad_testmode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int idx = 0, flag = 0;
	char temp[QUEST_BUFF_SIZE * 3];
	char quest_cmd[QUEST_CMD_LIST][QUEST_BUFF_SIZE];
	char *quest_ptr, *string;

	QUEST_SYSFS_ENTER();

	memset(quest_cmd, 0, sizeof(quest_cmd));
	strscpy(temp, buf, sizeof(temp));
	string = temp;
	while (idx < QUEST_CMD_LIST && (quest_ptr = strsep(&string, ",")) != NULL)
		strscpy(quest_cmd[idx++], quest_ptr, sizeof(quest_cmd[idx - 1]));

	if (idx < 2)
		goto out;

	QUEST_PRINT("%s : %s(%s)\n", __func__, quest_cmd[0], quest_cmd[1]);

	if (kstrtoint(quest_cmd[1], 10, &flag))
		goto out;

	if (!strncmp(quest_cmd[0], "testmode", 8)) {
		testmode_enabled = flag;
	} else if (!strncmp(quest_cmd[0], "quefi", 5)) {
		testmode_quefi_enabled = flag;
		if (flag)
			param_quest_data.quefi_remained_count = param_quest_data.hlos_remained_count;
		else
			param_quest_data.quefi_remained_count = 0;
	} else if (!strncmp(quest_cmd[0], "suefi", 5)) {
		testmode_suefi_enabled = flag;
		if (flag)
			param_quest_data.suefi_remained_count = param_quest_data.hlos_remained_count;
		else
			param_quest_data.suefi_remained_count = 0;
	} else if (!strncmp(quest_cmd[0], "ddrscan", 7)) {
		testmode_ddrscan_enabled = flag;
		if (flag)
			param_quest_data.ddrscan_remained_count = param_quest_data.hlos_remained_count;
		else
			param_quest_data.ddrscan_remained_count = 0;
	}
	quest_sync_param_quest_data();

out:
	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_WO(nad_testmode);

/* NAD_INIT_STEP */
static ssize_t nad_init_step_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	QUEST_SYSFS_ENTER();

	if (unlikely(strncmp(buf, "init_step", 9))) {
		QUEST_PRINT("%s : exceptional cases (buf=%s)\n", __func__, buf);
		goto out;
	}

	QUEST_PRINT("%s : call quest_initialize_curr_step\n", __func__);
	quest_initialize_curr_step();

out:
	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_WO(nad_init_step);

/* NAD_SMD_INFO */
static ssize_t nad_smd_info_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int idx = 0, val;
	char temp[QUEST_CMD_LIST * QUEST_CMD_SIZE];
	char quest_cmd[QUEST_CMD_LIST][QUEST_CMD_SIZE];
	char *quest_ptr, *string;

	QUEST_SYSFS_ENTER();

	if (param_quest_data.curr_step != STEP_SMDDL) {
		QUEST_PRINT("%s : The smd info should be updated only at SMDDL\n", __func__);
		goto out;
	}

	memset(quest_cmd, 0, sizeof(quest_cmd));
	strscpy(temp, buf, sizeof(temp));
	string = temp;
	while (idx < QUEST_CMD_LIST && (quest_ptr = strsep(&string, ",")) != NULL)
		strscpy(quest_cmd[idx++], quest_ptr, sizeof(quest_cmd[idx - 1]));

	if (idx < 2)
		goto out;

	QUEST_PRINT("%s : %s(%s)\n", __func__, quest_cmd[0], quest_cmd[1]);

	if (!strncmp(quest_cmd[0], "smd_boot_reason", 15)) {
		strscpy(param_quest_data.smd_boot_reason, quest_cmd[1], 3);
		QUEST_UPDATE_SMDDL_INFO_WITH_STRING(param_quest_data.smd_boot_reason_first, quest_cmd[1], 2);
	} else if (!strncmp(quest_cmd[0], "smd_hlos_start_time", 19)) {
		if (kstrtoint(quest_cmd[1], 10, &val) == 0) {
			param_quest_data.smd_hlos_start_time = val;
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_hlos_start_time);
		}
	} else if (!strncmp(quest_cmd[0], "smd_hlos_elapsed_time", 21)) {
		if (kstrtoint(quest_cmd[1], 10, &val) == 0) {
			param_quest_data.smd_hlos_elapsed_time = val;
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_hlos_elapsed_time);
		}
	} else if (!strncmp(quest_cmd[0], "smd_hlos_init_thermal", 21)) {
		if (kstrtoint(quest_cmd[1], 10, &val) == 0) {
			param_quest_data.smd_hlos_init_thermal = val;
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_hlos_init_thermal);
		}
	} else if (!strncmp(quest_cmd[0], "smd_hlos_max_thermal", 20)) {
		if (kstrtoint(quest_cmd[1], 10, &val) == 0) {
			param_quest_data.smd_hlos_max_thermal = val;
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_hlos_max_thermal);
		}
	} else if (!strncmp(quest_cmd[0], "smd_ns_repeats", 14)) {
		if (kstrtoint(quest_cmd[1], 10, &val) == 0) {
			param_quest_data.smd_ns_repeats = val;
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_ns_repeats);
		}
	} else if (!strncmp(quest_cmd[0], "smd_max_aoss_thermal_diff", 25)) {
		if (kstrtoint(quest_cmd[1], 10, &val) == 0) {
			param_quest_data.smd_max_aoss_thermal_diff = val;
			QUEST_UPDATE_SMDDL_INFO(param_quest_data.smd_max_aoss_thermal_diff);
		}
	}

	quest_sync_param_quest_data();

out:
	QUEST_SYSFS_EXIT();
	return count;
}
static DEVICE_ATTR_WO(nad_smd_info);

static struct device_attribute * const sec_nad_factory_attrs[] = {
	&dev_attr_nad_stat,
	&dev_attr_nad_ddrtest_remain_count,
	&dev_attr_nad_qmvs_remain_count,
	&dev_attr_nad_erase,
	&dev_attr_nad_acat,
	&dev_attr_nad_dram,
	&dev_attr_nad_support,
	&dev_attr_nad_logs,
	&dev_attr_nad_end,
	&dev_attr_nad_dram_err_addr,
	&dev_attr_nad_result,
	&dev_attr_nad_api,
	&dev_attr_nad_info,
	&dev_attr_nad_version,
	&dev_attr_nad_testmode,
	&dev_attr_nad_smd_info,
	&dev_attr_nad_init_step,
#if defined(CONFIG_SEC_QUEST_AUTO_TRIGGER_INIT_WRITE)
	&dev_attr_nad_auto_trigger,
#endif
};

static struct device_attribute * const sec_nad_balancer_attrs[] = {
	&dev_attr_balancer,
	&dev_attr_timeout,
	&dev_attr_run,
};

#endif /* CONFIG_SEC_FACTORY */

/* QUEST_FV_FLASHED */
static ssize_t quest_fv_flashed_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int flashed;

	mutex_lock(&sysfs_common_lock);
	quest_load_param_quest_data();
	flashed = param_quest_data.quest_fv_flashed;
	mutex_unlock(&sysfs_common_lock);

	return snprintf(buf, BUFF_SZ, "%d\n", flashed);
}
static DEVICE_ATTR_RO(quest_fv_flashed);

static int __init sec_quest_init(void)
{
	int ret;
#if defined(CONFIG_SEC_FACTORY)
	int i, j;
#endif

	/* Skip quest init when device goes to lp charging */
	if (lpcharge)
		return 0;

	mutex_init(&sysfs_common_lock);

	sec_nad = sec_device_create(NULL, "sec_nad");
	if (IS_ERR(sec_nad)) {
		QUEST_PRINT("%s Failed to create device(sec_nad)!\n", __func__);
		return PTR_ERR(sec_nad);
	}

	ret = device_create_file(sec_nad, &dev_attr_quest_fv_flashed);
	if (ret) {
		QUEST_PRINT("%s: Failed to create device file\n", __func__);
		goto err_destroy_sec_nad;
	}

#if defined(CONFIG_SEC_FACTORY)
	sec_nad_balancer = sec_device_create(NULL, "sec_nad_balancer");
	if (IS_ERR(sec_nad_balancer)) {
		QUEST_PRINT("%s Failed to create device(sec_nad_balancer)!\n", __func__);
		ret = PTR_ERR(sec_nad_balancer);
		goto err_remove_fv_flashed;
	}

	for (i = 0; i < ARRAY_SIZE(sec_nad_factory_attrs); i++) {
		ret = device_create_file(sec_nad, sec_nad_factory_attrs[i]);
		if (ret) {
			QUEST_PRINT("%s: Failed to create device file %s\n",
				    __func__, sec_nad_factory_attrs[i]->attr.name);
			for (j = 0; j < i; j++)
				device_remove_file(sec_nad, sec_nad_factory_attrs[j]);
			goto err_destroy_balancer;
		}
	}

	for (i = 0; i < ARRAY_SIZE(sec_nad_balancer_attrs); i++) {
		ret = device_create_file(sec_nad_balancer, sec_nad_balancer_attrs[i]);
		if (ret) {
			QUEST_PRINT("%s: Failed to create balancer device file %s\n",
				    __func__, sec_nad_balancer_attrs[i]->attr.name);
			for (j = 0; j < i; j++)
				device_remove_file(sec_nad_balancer, sec_nad_balancer_attrs[j]);
			goto err_remove_factory_attrs;
		}
	}

	if (add_uevent_var(&quest_uevent, "NAD_TEST=%s", "DONE")) {
		QUEST_PRINT("%s : uevent NAD_TEST_AND_PASS is failed to add\n", __func__);
		ret = -ENOMEM;
		goto err_remove_balancer_attrs;
	}

#if defined(CONFIG_SEC_QUEST_AUTO_TRIGGER_KWORKER)
	INIT_DELAYED_WORK(&trigger_quest_work, delayed_quest_work_func);
	schedule_delayed_work(&trigger_quest_work, msecs_to_jiffies(WAIT_TIME_BEFORE_TRIGGER_MSECS));
#endif

	atomic_notifier_chain_register(&panic_notifier_list, &quest_panic_block);
#endif

	return 0;

#if defined(CONFIG_SEC_FACTORY)
err_remove_balancer_attrs:
	for (i = 0; i < ARRAY_SIZE(sec_nad_balancer_attrs); i++)
		device_remove_file(sec_nad_balancer, sec_nad_balancer_attrs[i]);
err_remove_factory_attrs:
	for (i = 0; i < ARRAY_SIZE(sec_nad_factory_attrs); i++)
		device_remove_file(sec_nad, sec_nad_factory_attrs[i]);
err_destroy_balancer:
	sec_device_destroy(sec_nad_balancer->devt);
err_remove_fv_flashed:
	device_remove_file(sec_nad, &dev_attr_quest_fv_flashed);
#endif
err_destroy_sec_nad:
	sec_device_destroy(sec_nad->devt);
	return ret;
}

module_init(sec_quest_init);
MODULE_DESCRIPTION("Samsung QUEST NAD/QDAF Debugging Driver");
MODULE_LICENSE("GPL v2");
