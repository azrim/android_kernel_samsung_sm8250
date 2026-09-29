// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung factory command driver
 *
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 *
 * State Machine:
 *
 *   [WAITING] (idle)
 *       |
 *       | write "cmd" (via cmd_store / sec_cmd_store_function)
 *       v
 *   [RUNNING]
 *       |
 *       +---> [OK]             (driver handler succeeded)
 *       |
 *       +---> [FAIL]           (driver handler failed)
 *       |
 *       +---> [NOT_APPLICABLE] (driver handler unsupported)
 *       |
 *       v
 *   [read "cmd_result"]
 *       |
 *       +---> [EXPAND]         (more result chunks available)
 *       |        |
 *       |        +---> [read next "cmd_result" chunk]
 *       |        |
 *       |        v
 *       +---> [WAITING]        (final chunk consumed, ready for next command)
 *
 * Lock Hierarchy:
 *
 *   fs_lock (outer)
 *     |
 *     +--> fifo_lock (middle)
 *            |
 *            +--> cmd_lock (leaf)
 *
 * Rules:
 *  1. fs_lock serialises sysfs store and read operations on cmd, cmd_result,
 *     and cmd_result_all.
 *  2. fifo_lock serialises kfifo queue access (kfifo_in, kfifo_out, reset).
 *  3. cmd_lock protects state transitions (cmd_state, cmd_is_running,
 *     expand indices, and cmd_result buffer updates).
 *  4. Never invert this order. No lock is ever acquired twice on any code path.
 */

#include <linux/input/sec_cmd.h>
#include <linux/input/sec_tsp_log.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/stat.h>
#include <linux/err.h>
#include <linux/sched/clock.h>
#include <linux/notifier.h>
#include <linux/ctype.h>

#if !defined(CONFIG_SEC_SYSFS) && !defined(CONFIG_DRV_SAMSUNG)
struct class *tsp_sec_class;
#endif

#ifdef USE_SEC_CMD_QUEUE
static void sec_cmd_store_function(struct sec_cmd_data *data);
#endif

static const char * const sec_cmd_status_str[] = {
	[SEC_CMD_STATUS_WAITING]	= "WAITING",
	[SEC_CMD_STATUS_RUNNING]	= "RUNNING",
	[SEC_CMD_STATUS_OK]		= "OK",
	[SEC_CMD_STATUS_FAIL]		= "FAIL",
	[SEC_CMD_STATUS_EXPAND]		= "EXPAND",
	[SEC_CMD_STATUS_NOT_APPLICABLE]	= "NOT_APPLICABLE",
};

/**
 * sec_cmd_lookup - Find command entry by name
 * @data: sec_cmd state instance
 * @name: Command name string
 *
 * Context: Caller must hold data->cmd_lock.
 * Return: Matching struct sec_cmd pointer, or NULL if not found.
 */
static struct sec_cmd *sec_cmd_lookup(struct sec_cmd_data *data, const char *name)
{
	struct sec_cmd *entry;

	list_for_each_entry(entry, &data->cmd_list_head, list) {
		if (entry->cmd_name && !strncmp(name, entry->cmd_name, SEC_CMD_STR_LEN))
			return entry;
	}
	return NULL;
}

/**
 * sec_cmd_parse_params - Parse comma-separated integers from command buffer
 * @data: sec_cmd state instance
 * @start: Start pointer in string
 * @end: End pointer in string
 *
 * Context: Caller must hold data->cmd_lock.
 * Return: Number of successfully parsed parameters.
 */
static int sec_cmd_parse_params(struct sec_cmd_data *data, const char *start,
				const char *end)
{
	const char *cur = start;
	int param_cnt = 0;

	while (cur < end && param_cnt < SEC_CMD_PARAM_NUM) {
		const char *next = memchr(cur, ',', end - cur);
		size_t tok_len = next ? (size_t)(next - cur) : (size_t)(end - cur);
		char buff[16];

		if (tok_len >= sizeof(buff))
			break;
		memcpy(buff, cur, tok_len);
		buff[tok_len] = '\0';

		if (kstrtoint(buff, 10, &data->cmd_param[param_cnt]) < 0)
			break;

		param_cnt++;
		cur = next ? (next + 1) : end;
	}
	return param_cnt;
}

void sec_cmd_set_cmd_exit(struct sec_cmd_data *data)
{
#ifdef USE_SEC_CMD_QUEUE
	bool has_pending = false;
	int left = 0;

	mutex_lock(&data->fifo_lock);
	left = kfifo_len(&data->cmd_queue) / sizeof(struct command);
	if (left > 0) {
		pr_info("%s: %s %s: do next cmd, left cmd[%d]\n",
			dev_name(data->fac_dev), SECLOG, __func__, left);
		has_pending = true;
	}

	mutex_lock(&data->cmd_lock);
	if (has_pending) {
		data->cmd_is_running = true;
		data->cmd_state = SEC_CMD_STATUS_RUNNING;
	} else {
		data->cmd_is_running = false;
	}
	mutex_unlock(&data->cmd_lock);
	mutex_unlock(&data->fifo_lock);

	if (has_pending) {
#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
		sec_cmd_store_function(data);
#else
		schedule_work(&data->cmd_work.work);
#endif
	}
#else
	mutex_lock(&data->cmd_lock);
	data->cmd_is_running = false;
	mutex_unlock(&data->cmd_lock);
#endif
}

#ifdef USE_SEC_CMD_QUEUE
static void cmd_exit_work(struct work_struct *work)
{
	struct sec_cmd_data *data = container_of(work, struct sec_cmd_data,
						 cmd_work.work);

	sec_cmd_store_function(data);
}
#endif

void sec_cmd_set_default_result(struct sec_cmd_data *data)
{
	if (!data || !data->cmd_result)
		return;

	mutex_lock(&data->cmd_lock);
	scnprintf(data->cmd_result, SEC_CMD_RESULT_STR_LEN_EXPAND, "%s:", data->cmd);
	mutex_unlock(&data->cmd_lock);
}

void sec_cmd_set_cmd_result(struct sec_cmd_data *data, char *buff, int len)
{
	size_t cur_len, add_len;

	if (!data || !data->cmd_result || !buff)
		return;

	mutex_lock(&data->cmd_lock);
	cur_len = strlen(data->cmd_result);
	add_len = strlen(buff);

	if (cur_len + add_len >= SEC_CMD_RESULT_STR_LEN_EXPAND) {
		pr_err("%s %s: cmd length is over (%zu)!!\n", SECLOG, __func__,
		       add_len);
		strlcat(data->cmd_result, "NG", SEC_CMD_RESULT_STR_LEN_EXPAND);
		mutex_unlock(&data->cmd_lock);
		return;
	}

	data->cmd_result_expand = (int)add_len / SEC_CMD_RESULT_STR_LEN;
	data->cmd_result_expand_count = 0;
	strlcat(data->cmd_result, buff, SEC_CMD_RESULT_STR_LEN_EXPAND);
	mutex_unlock(&data->cmd_lock);
}

void sec_cmd_set_cmd_result_all(struct sec_cmd_data *data, char *buff, int len,
				char *item)
{
	size_t cur_len, add_len;

	if (!data || !buff || !item)
		return;

	mutex_lock(&data->cmd_lock);
	cur_len = strlen(data->cmd_result_all);
	add_len = 1 + strlen(item) + 1 + strlen(buff);	/* " " + item + ":" + buff */

	if (cur_len + add_len >= sizeof(data->cmd_result_all)) {
		pr_err("%s: %s %s: cmd length is over (%zu)!!\n",
		       dev_name(data->fac_dev), SECLOG, __func__, cur_len + add_len);
		mutex_unlock(&data->cmd_lock);
		return;
	}

	data->item_count++;
	strlcat(data->cmd_result_all, " ", sizeof(data->cmd_result_all));
	strlcat(data->cmd_result_all, item, sizeof(data->cmd_result_all));
	strlcat(data->cmd_result_all, ":", sizeof(data->cmd_result_all));
	strlcat(data->cmd_result_all, buff, sizeof(data->cmd_result_all));
	mutex_unlock(&data->cmd_lock);
}

#ifdef USE_SEC_CMD_QUEUE
static void sec_cmd_store_function(struct sec_cmd_data *data)
{
	struct command cmd = { { 0 } };
	struct sec_cmd *entry = NULL;
	const char *buf, *delim_pos;
	char cmd_name[SEC_CMD_STR_LEN];
	char param_str[128] = { 0 };
	size_t count, cmd_name_len;
	int ret, i, param_cnt = 0, p_off = 0;

	if (!data) {
		pr_err("%s %s: No platform data found\n", SECLOG, __func__);
		return;
	}

	mutex_lock(&data->fifo_lock);
	if (!kfifo_len(&data->cmd_queue)) {
		pr_err("%s: %s %s: left cmd is nothing\n",
		       dev_name(data->fac_dev), SECLOG, __func__);
		mutex_unlock(&data->fifo_lock);
		mutex_lock(&data->cmd_lock);
		data->cmd_is_running = false;
		data->cmd_state = SEC_CMD_STATUS_WAITING;
		mutex_unlock(&data->cmd_lock);
		return;
	}

	ret = kfifo_out(&data->cmd_queue, &cmd, sizeof(cmd));
	if (!ret) {
		pr_err("%s: %s %s: kfifo_out failed, it seems empty, ret=%d\n",
		       dev_name(data->fac_dev), SECLOG, __func__, ret);
		mutex_unlock(&data->fifo_lock);
		return;
	}
	mutex_unlock(&data->fifo_lock);

	buf = cmd.cmd;
	count = strnlen(buf, SEC_CMD_STR_LEN);
	if (count > 0 && buf[count - 1] == '\n')
		count--;

	mutex_lock(&data->cmd_lock);
	for (i = 0; i < (int)ARRAY_SIZE(data->cmd_param); i++)
		data->cmd_param[i] = 0;

	memset(data->cmd, 0, sizeof(data->cmd));
	memcpy(data->cmd, buf, min(count, sizeof(data->cmd) - 1));

	delim_pos = memchr(buf, ',', count);
	if (delim_pos)
		cmd_name_len = delim_pos - buf;
	else
		cmd_name_len = count;

	cmd_name_len = min(cmd_name_len, sizeof(cmd_name) - 1);
	memcpy(cmd_name, buf, cmd_name_len);
	cmd_name[cmd_name_len] = '\0';

	pr_debug("%s: %s %s: COMMAND : %s\n",
		 dev_name(data->fac_dev), SECLOG, __func__, cmd_name);

	entry = sec_cmd_lookup(data, cmd_name);
	if (!entry)
		entry = sec_cmd_lookup(data, "not_support_cmd");

	if (delim_pos && entry)
		param_cnt = sec_cmd_parse_params(data, delim_pos + 1, buf + count);

	if (entry) {
		for (i = 0; i < param_cnt; i++) {
			if (i == 0)
				p_off += scnprintf(param_str + p_off,
						   sizeof(param_str) - p_off, " param =");
			p_off += scnprintf(param_str + p_off,
					   sizeof(param_str) - p_off, " %d",
					   data->cmd_param[i]);
		}
		pr_info("%s: %s %s: cmd = %s%s\n",
			dev_name(data->fac_dev), SECLOG, __func__,
			entry->cmd_name, param_str);
	} else {
		pr_info("%s: %s %s: cmd = %s(%s)\n",
			dev_name(data->fac_dev), SECLOG, __func__,
			cmd_name, "not_support_cmd");
	}
	mutex_unlock(&data->cmd_lock);

	if (entry && entry->cmd_func)
		entry->cmd_func(data);

	if (entry && entry->cmd_log) {
		unsigned long long t = local_clock();
		unsigned long nanosec_rem = do_div(t, 1000000000);
		char tbuf[32];

		snprintf(tbuf, sizeof(tbuf), "[r:%lu.%06lu]",
			 (unsigned long)t, nanosec_rem / 1000);
		sec_debug_tsp_command_history(tbuf);
	}
}

static ssize_t cmd_store(struct device *dev, struct device_attribute *devattr,
			 const char *buf, size_t count)
{
	struct sec_cmd_data *data = dev_get_drvdata(dev);
	struct command cmd = { { 0 } };
	struct sec_cmd *entry;
	bool running;

	if (!data) {
		pr_err("%s %s: No platform data found\n", SECLOG, __func__);
		return -EINVAL;
	}

	if (count >= SEC_CMD_STR_LEN || strnlen(buf, SEC_CMD_STR_LEN) >= SEC_CMD_STR_LEN) {
		pr_err("%s: %s %s: cmd length is over (%zu,%s)!!\n",
		       dev_name(data->fac_dev), SECLOG, __func__, count, buf);
		return -EINVAL;
	}

	strscpy(cmd.cmd, buf, sizeof(cmd.cmd));

	mutex_lock(&data->cmd_lock);
	list_for_each_entry(entry, &data->cmd_list_head, list) {
		if (entry->cmd_name &&
		    !strncmp(cmd.cmd, entry->cmd_name, strlen(entry->cmd_name))) {
			if (entry->cmd_log) {
				unsigned long long t = local_clock();
				unsigned long nanosec_rem = do_div(t, 1000000000);
				char task_info[40], tbuf[32];

				snprintf(tbuf, sizeof(tbuf), "[q:%lu.%06lu]",
					 (unsigned long)t, nanosec_rem / 1000);
				snprintf(task_info, sizeof(task_info), "\n[%d:%s:%s]",
					 current->pid, current->comm, dev_name(data->fac_dev));

				sec_debug_tsp_command_history(task_info);
				sec_debug_tsp_command_history(cmd.cmd);
				sec_debug_tsp_command_history(tbuf);
			}
			break;
		}
	}
	mutex_unlock(&data->cmd_lock);

	mutex_lock(&data->fifo_lock);
	if (kfifo_avail(&data->cmd_queue) &&
	    (kfifo_len(&data->cmd_queue) / sizeof(struct command) < SEC_CMD_MAX_QUEUE)) {
		kfifo_in(&data->cmd_queue, &cmd, sizeof(cmd));
		pr_info("%s: %s %s: push cmd: %s\n",
			dev_name(data->fac_dev), SECLOG, __func__, cmd.cmd);
	} else {
		pr_err("%s: %s %s: cmd_queue is full!!\n",
		       dev_name(data->fac_dev), SECLOG, __func__);
		kfifo_reset(&data->cmd_queue);
		pr_err("%s: %s %s: cmd_queue is reset!!\n",
		       dev_name(data->fac_dev), SECLOG, __func__);
		mutex_unlock(&data->fifo_lock);

		mutex_lock(&data->cmd_lock);
		data->cmd_is_running = false;
		data->cmd_state = SEC_CMD_STATUS_WAITING;
		mutex_unlock(&data->cmd_lock);
		return -ENOSPC;
	}

	mutex_lock(&data->cmd_lock);
	running = data->cmd_is_running;
	if (!running) {
		data->cmd_is_running = true;
		data->cmd_state = SEC_CMD_STATUS_RUNNING;
	}
	mutex_unlock(&data->cmd_lock);

	if (running) {
		pr_err("%s: %s %s: other cmd is running. Wait until previous cmd is done[%d]\n",
		       dev_name(data->fac_dev), SECLOG, __func__,
		       (int)(kfifo_len(&data->cmd_queue) / sizeof(struct command)));
		mutex_unlock(&data->fifo_lock);
		return -EBUSY;
	}
	mutex_unlock(&data->fifo_lock);

	mutex_lock(&data->fs_lock);
	sec_cmd_store_function(data);
	mutex_unlock(&data->fs_lock);

	return count;
}
#else /* !defined USE_SEC_CMD_QUEUE */
static ssize_t cmd_store(struct device *dev, struct device_attribute *devattr,
			 const char *buf, size_t count)
{
	struct sec_cmd_data *data = dev_get_drvdata(dev);
	struct sec_cmd *entry = NULL;
	const char *delim_pos;
	char cmd_name[SEC_CMD_STR_LEN];
	char param_str[128] = { 0 };
	size_t len, cmd_name_len;
	int i, param_cnt = 0, p_off = 0;

	if (!data) {
		pr_err("%s %s: No platform data found\n", SECLOG, __func__);
		return -EINVAL;
	}

	if (count >= SEC_CMD_STR_LEN || strnlen(buf, SEC_CMD_STR_LEN) >= SEC_CMD_STR_LEN) {
		pr_err("%s: %s %s: cmd length is over (%zu,%s)!!\n",
		       dev_name(data->fac_dev), SECLOG, __func__, count, buf);
		return -EINVAL;
	}

	mutex_lock(&data->cmd_lock);
	if (data->cmd_is_running) {
		pr_err("%s: %s %s: other cmd is running.\n",
		       dev_name(data->fac_dev), SECLOG, __func__);
		mutex_unlock(&data->cmd_lock);
		return -EBUSY;
	}

	data->cmd_is_running = true;
	data->cmd_state = SEC_CMD_STATUS_RUNNING;

	for (i = 0; i < (int)ARRAY_SIZE(data->cmd_param); i++)
		data->cmd_param[i] = 0;

	len = count;
	if (len > 0 && buf[len - 1] == '\n')
		len--;

	memset(data->cmd, 0, sizeof(data->cmd));
	memcpy(data->cmd, buf, min(len, sizeof(data->cmd) - 1));

	delim_pos = memchr(buf, ',', len);
	if (delim_pos)
		cmd_name_len = delim_pos - buf;
	else
		cmd_name_len = len;

	cmd_name_len = min(cmd_name_len, sizeof(cmd_name) - 1);
	memcpy(cmd_name, buf, cmd_name_len);
	cmd_name[cmd_name_len] = '\0';

	pr_debug("%s: %s %s: COMMAND = %s\n",
		 dev_name(data->fac_dev), SECLOG, __func__, cmd_name);

	entry = sec_cmd_lookup(data, cmd_name);
	if (!entry)
		entry = sec_cmd_lookup(data, "not_support_cmd");

	if (delim_pos && entry)
		param_cnt = sec_cmd_parse_params(data, delim_pos + 1, buf + len);

	if (entry) {
		for (i = 0; i < param_cnt; i++) {
			if (i == 0)
				p_off += scnprintf(param_str + p_off,
						   sizeof(param_str) - p_off, " param =");
			p_off += scnprintf(param_str + p_off,
					   sizeof(param_str) - p_off, " %d",
					   data->cmd_param[i]);
		}
		pr_info("%s: %s %s: cmd = %s%s\n",
			dev_name(data->fac_dev), SECLOG, __func__,
			entry->cmd_name, param_str);
	} else {
		pr_info("%s: %s %s: cmd = %s(%s)\n",
			dev_name(data->fac_dev), SECLOG, __func__,
			cmd_name, "not_support_cmd");
	}
	mutex_unlock(&data->cmd_lock);

	if (entry && entry->cmd_func)
		entry->cmd_func(data);

	return count;
}
#endif

static ssize_t cmd_status_show(struct device *dev,
			       struct device_attribute *devattr, char *buf)
{
	struct sec_cmd_data *data = dev_get_drvdata(dev);
	const char *status = "UNKNOWN";
	u8 state;

	if (!data) {
		pr_err("%s %s: No platform data found\n", SECLOG, __func__);
		return -EINVAL;
	}

	mutex_lock(&data->cmd_lock);
	state = data->cmd_state;
	if (state < ARRAY_SIZE(sec_cmd_status_str) && sec_cmd_status_str[state])
		status = sec_cmd_status_str[state];
	mutex_unlock(&data->cmd_lock);

	pr_debug("%s: %s %s: %d, %s\n", dev_name(data->fac_dev), SECLOG, __func__,
		 state, status);

	return scnprintf(buf, SEC_CMD_BUF_SIZE, "%s\n", status);
}

static ssize_t cmd_status_all_show(struct device *dev,
				   struct device_attribute *devattr, char *buf)
{
	struct sec_cmd_data *data = dev_get_drvdata(dev);
	const char *status = "UNKNOWN";
	u8 state;

	if (!data) {
		pr_err("%s %s: No platform data found\n", SECLOG, __func__);
		return -EINVAL;
	}

	mutex_lock(&data->cmd_lock);
	state = data->cmd_all_factory_state;
	if (state < ARRAY_SIZE(sec_cmd_status_str) && sec_cmd_status_str[state])
		status = sec_cmd_status_str[state];
	mutex_unlock(&data->cmd_lock);

	pr_debug("%s: %s %s: %d, %s\n", dev_name(data->fac_dev), SECLOG, __func__,
		 state, status);

	return scnprintf(buf, SEC_CMD_BUF_SIZE, "%s\n", status);
}

static ssize_t cmd_result_show(struct device *dev,
			       struct device_attribute *devattr, char *buf)
{
	struct sec_cmd_data *data = dev_get_drvdata(dev);
	size_t offset;
	int size;

	if (!data) {
		pr_err("%s %s: No platform data found\n", SECLOG, __func__);
		return -EINVAL;
	}

	mutex_lock(&data->fs_lock);
	mutex_lock(&data->cmd_lock);

	offset = (size_t)(SEC_CMD_RESULT_STR_LEN - 1) * data->cmd_result_expand_count;
	size = scnprintf(buf, SEC_CMD_RESULT_STR_LEN, "%s\n",
			 data->cmd_result + offset);

	if (data->cmd_result_expand_count != data->cmd_result_expand) {
		data->cmd_state = SEC_CMD_STATUS_EXPAND;
		data->cmd_result_expand_count++;
	} else {
		data->cmd_state = SEC_CMD_STATUS_WAITING;
	}

	mutex_unlock(&data->cmd_lock);

	pr_info("%s: %s %s: %s\n", dev_name(data->fac_dev), SECLOG, __func__, buf);

	sec_cmd_set_cmd_exit(data);

	mutex_unlock(&data->fs_lock);
	return size;
}

static ssize_t cmd_result_all_show(struct device *dev,
				   struct device_attribute *devattr, char *buf)
{
	struct sec_cmd_data *data = dev_get_drvdata(dev);
	int size;

	if (!data) {
		pr_err("%s %s: No platform data found\n", SECLOG, __func__);
		return -EINVAL;
	}

	mutex_lock(&data->fs_lock);
	mutex_lock(&data->cmd_lock);

	data->cmd_state = SEC_CMD_STATUS_WAITING;
	pr_info("%s: %s %s: %d, %s\n", dev_name(data->fac_dev), SECLOG, __func__,
		data->item_count, data->cmd_result_all);
	size = scnprintf(buf, SEC_CMD_RESULT_STR_LEN, "%d%s\n",
			 data->item_count, data->cmd_result_all);

	data->item_count = 0;
	memset(data->cmd_result_all, 0, sizeof(data->cmd_result_all));

	mutex_unlock(&data->cmd_lock);

	sec_cmd_set_cmd_exit(data);

	mutex_unlock(&data->fs_lock);
	return size;
}

static ssize_t cmd_list_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct sec_cmd_data *data = dev_get_drvdata(dev);
	struct sec_cmd *entry;
	char *buffer;
	int ret;

	if (!data)
		return -EINVAL;

	buffer = kzalloc(data->cmd_buffer_size + 32, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	scnprintf(buffer, 32, "++factory command list++\n");

	mutex_lock(&data->cmd_lock);
	list_for_each_entry(entry, &data->cmd_list_head, list) {
		if (entry->cmd_name && strncmp(entry->cmd_name, "not_support_cmd", 15)) {
			strlcat(buffer, entry->cmd_name, data->cmd_buffer_size + 32);
			strlcat(buffer, "\n", data->cmd_buffer_size + 32);
		}
	}
	mutex_unlock(&data->cmd_lock);

	ret = scnprintf(buf, SEC_CMD_BUF_SIZE, "%s\n", buffer);
	kfree(buffer);

	return ret;
}

static DEVICE_ATTR(cmd, 0220, NULL, cmd_store);
static DEVICE_ATTR_RO(cmd_status);
static DEVICE_ATTR_RO(cmd_status_all);
static DEVICE_ATTR_RO(cmd_result);
static DEVICE_ATTR_RO(cmd_result_all);
static DEVICE_ATTR_RO(cmd_list);

static struct attribute *sec_fac_attrs[] = {
	&dev_attr_cmd.attr,
	&dev_attr_cmd_status.attr,
	&dev_attr_cmd_status_all.attr,
	&dev_attr_cmd_result.attr,
	&dev_attr_cmd_result_all.attr,
	&dev_attr_cmd_list.attr,
	NULL,
};

static const struct attribute_group sec_fac_attr_group = {
	.attrs = sec_fac_attrs,
};

int sec_cmd_init(struct sec_cmd_data *data, struct sec_cmd *cmds, int len, int devt)
{
	const char *dev_name;
	int ret, i;

	INIT_LIST_HEAD(&data->cmd_list_head);

	data->cmd_buffer_size = 0;
	for (i = 0; i < len; i++) {
		list_add_tail(&cmds[i].list, &data->cmd_list_head);
		if (cmds[i].cmd_name)
			data->cmd_buffer_size += strlen(cmds[i].cmd_name) + 1;
	}

	mutex_init(&data->cmd_lock);
	mutex_init(&data->fs_lock);

	data->cmd_is_running = false;
	data->cmd_state = SEC_CMD_STATUS_WAITING;

	data->cmd_result = kzalloc(SEC_CMD_RESULT_STR_LEN_EXPAND, GFP_KERNEL);
	if (!data->cmd_result) {
		ret = -ENOMEM;
		goto err_alloc_cmd_result;
	}

#ifdef USE_SEC_CMD_QUEUE
	ret = kfifo_alloc(&data->cmd_queue,
			  SEC_CMD_MAX_QUEUE * sizeof(struct command), GFP_KERNEL);
	if (ret) {
		pr_err("%s %s: failed to alloc queue for cmd\n", SECLOG, __func__);
		goto err_alloc_queue;
	}
	mutex_init(&data->fifo_lock);
	INIT_DELAYED_WORK(&data->cmd_work, cmd_exit_work);
#endif

	switch (devt) {
	case SEC_CLASS_DEVT_TSP:
		dev_name = SEC_CLASS_DEV_NAME_TSP;
		break;
#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
	case SEC_CLASS_DEVT_TSP1:
		dev_name = SEC_CLASS_DEV_NAME_TSP1;
		break;
	case SEC_CLASS_DEVT_TSP2:
		dev_name = SEC_CLASS_DEV_NAME_TSP2;
		break;
#endif
	case SEC_CLASS_DEVT_TKEY:
		dev_name = SEC_CLASS_DEV_NAME_TKEY;
		break;
	case SEC_CLASS_DEVT_WACOM:
		dev_name = SEC_CLASS_DEV_NAME_WACOM;
		break;
	case SEC_CLASS_DEVT_SIDEKEY:
		dev_name = SEC_CLASS_DEV_NAME_SIDEKEY;
		break;
	default:
		pr_err("%s %s: not defined devt=%d\n", SECLOG, __func__, devt);
		ret = -ENODEV;
		goto err_get_dev_name;
	}

#if defined(CONFIG_SEC_SYSFS) || defined(CONFIG_DRV_SAMSUNG)
	data->fac_dev = sec_device_create(data, dev_name);
#else
	tsp_sec_class = class_create(THIS_MODULE, "tsp_sec");
	if (IS_ERR(tsp_sec_class)) {
		ret = PTR_ERR(tsp_sec_class);
		pr_err("%s %s: Failed to create class(sec) %d\n",
		       SECLOG, __func__, ret);
		goto err_sysfs_device;
	}
	data->fac_dev = device_create(tsp_sec_class, NULL, devt, data, dev_name);
#endif

	if (IS_ERR(data->fac_dev)) {
		ret = PTR_ERR(data->fac_dev);
		pr_err("%s %s: failed to create device for the sysfs\n",
		       SECLOG, __func__);
		goto err_sysfs_device;
	}

	dev_set_drvdata(data->fac_dev, data);

	ret = sysfs_create_group(&data->fac_dev->kobj, &sec_fac_attr_group);
	if (ret < 0) {
		pr_err("%s %s: failed to create sysfs group\n", SECLOG, __func__);
		goto err_sysfs_group;
	}

#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
	switch (devt) {
	case SEC_CLASS_DEVT_TSP1:
	case SEC_CLASS_DEVT_TSP2:
		sec_virtual_tsp_register(data);
		break;
	}
#endif

	pr_info("%s: %s %s: done\n", dev_name, SECLOG, __func__);
	return 0;

err_sysfs_group:
#if defined(CONFIG_SEC_SYSFS) || defined(CONFIG_DRV_SAMSUNG)
	sec_device_destroy(data->fac_dev->devt);
#else
	device_destroy(tsp_sec_class, devt);
#endif
err_sysfs_device:
err_get_dev_name:
#ifdef USE_SEC_CMD_QUEUE
	mutex_destroy(&data->fifo_lock);
	kfifo_free(&data->cmd_queue);
err_alloc_queue:
#endif
	kfree(data->cmd_result);
err_alloc_cmd_result:
	mutex_destroy(&data->fs_lock);
	mutex_destroy(&data->cmd_lock);
	INIT_LIST_HEAD(&data->cmd_list_head);
	return ret;
}

void sec_cmd_exit(struct sec_cmd_data *data, int devt)
{
	pr_info("%s: %s %s\n", dev_name(data->fac_dev), SECLOG, __func__);

	sysfs_remove_group(&data->fac_dev->kobj, &sec_fac_attr_group);
	dev_set_drvdata(data->fac_dev, NULL);

#if defined(CONFIG_SEC_SYSFS) || defined(CONFIG_DRV_SAMSUNG)
	sec_device_destroy(data->fac_dev->devt);
#else
	device_destroy(tsp_sec_class, devt);
#endif

#ifdef USE_SEC_CMD_QUEUE
	cancel_delayed_work_sync(&data->cmd_work);

	mutex_lock(&data->fifo_lock);
	kfifo_reset(&data->cmd_queue);
	mutex_unlock(&data->fifo_lock);

	mutex_destroy(&data->fifo_lock);
	kfifo_free(&data->cmd_queue);
#endif

	data->fac_dev = NULL;
	kfree(data->cmd_result);
	data->cmd_result = NULL;

	mutex_destroy(&data->fs_lock);
	mutex_destroy(&data->cmd_lock);
	INIT_LIST_HEAD(&data->cmd_list_head);
}

void sec_cmd_send_event_to_user(struct sec_cmd_data *data, char *test, char *result)
{
	char *event[5];
	char timestamp[32];
	char feature[32];
	char stest[32];
	char sresult[32];
	u64 curr_time;

	if (!data || !data->fac_dev)
		return;

	curr_time = ktime_to_ms(ktime_get());

	snprintf(timestamp, sizeof(timestamp), "TIMESTAMP=%d", (int)curr_time);
	snprintf(feature, sizeof(feature), "FEATURE=TSP");
	snprintf(stest, sizeof(stest), "TEST=%s", test ? test : "NULL");
	snprintf(sresult, sizeof(sresult), "RESULT=%s", result ? result : "NULL");

	pr_info("%s: %s %s: time:%s, feature:%s, test:%s, result:%s\n",
		dev_name(data->fac_dev), SECLOG, __func__,
		timestamp, feature, stest, sresult);

	event[0] = timestamp;
	event[1] = feature;
	event[2] = stest;
	event[3] = sresult;
	event[4] = NULL;

	kobject_uevent_env(&data->fac_dev->kobj, KOBJ_CHANGE, event);
}

static BLOCKING_NOTIFIER_HEAD(sec_input_notifier_list);

/*
 * sec_input_register_notify - Register universal input notifier
 * @nb: pointer to notifier block
 * @notifier_call: callback function
 * @priority: notifier priority
 */
void sec_input_register_notify(struct notifier_block *nb,
			       notifier_fn_t notifier_call, int priority)
{
	nb->notifier_call = notifier_call;
	nb->priority = priority;
	blocking_notifier_chain_register(&sec_input_notifier_list, nb);
}

/*
 * sec_input_unregister_notify - Unregister universal input notifier
 * @nb: pointer to notifier block
 */
void sec_input_unregister_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&sec_input_notifier_list, nb);
}

/*
 * sec_input_notify - Invoke input notifier chain
 * @nb: pointer to notifier block
 * @noti: notifier event ID
 * @v: event data pointer
 */
int sec_input_notify(struct notifier_block *nb, unsigned long noti, void *v)
{
	return blocking_notifier_call_chain(&sec_input_notifier_list, noti, v);
}

/*
 * sec_input_self_request_notify - Self test input notifier
 * @nb: pointer to notifier block
 */
int sec_input_self_request_notify(struct notifier_block *nb)
{
	return nb->notifier_call(nb, NOTIFIER_NOTHING, NULL);
}

MODULE_DESCRIPTION("Samsung factory command");
MODULE_LICENSE("GPL");
