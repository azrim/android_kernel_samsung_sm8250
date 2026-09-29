// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung TSP Debug Log and Ring Buffer Framework
 *
 * Copyright (C) 2006-2026 Samsung Electronics Co., Ltd.
 *
 * Concurrency & Writer Serialization:
 *
 * Touch events are generated from hardware interrupts and processed in
 * threaded IRQ handlers, while factory commands and sysfs accesses execute
 * in process context. Writers to the TSP ring buffers may thus run
 * concurrently on different CPUs or interrupt contexts.
 *
 * Each ring buffer is protected by a dedicated irq-safe spinlock (ring->lock).
 * Writers format the message into a local stack buffer and atomically append
 * it to the ring buffer under spin_lock_irqsave(), ensuring:
 *  1. No corrupted or interleaved indices (head / fix).
 *  2. No partial or torn log lines.
 *  3. Safe invocation from any context (process, threaded IRQ, or timer).
 *
 * Procfs readers snapshot the current head index (READ_ONCE) and copy to
 * user buffers without holding spinlocks, preventing sleep-in-atomic bugs
 * during user page faults.
 */

#ifdef CONFIG_SEC_DEBUG_TSP_LOG

#include <linux/input/sec_tsp_log.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/sched/clock.h>
#include <linux/spinlock.h>

#define TSP_BUF_SIZE	512

/**
 * struct sec_tsp_ring - Append-only ring buffer with fixed-index wrap anchor
 * @buf: Allocated buffer storage
 * @size: Total capacity in bytes
 * @head: Current write offset
 * @fix: Wrap-around anchor established at boot or via sec_tsp_log_fix
 * @lock: Serializes all write access across CPUs and IRQ contexts
 */
struct sec_tsp_ring {
	char			*buf;
	unsigned int		size;
	unsigned int		head;
	unsigned int		fix;
	spinlock_t		lock;
};

static struct sec_tsp_ring sec_tsp_log_ring;
#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
#define MAIN_TOUCH	0
#define SUB_TOUCH	1
static struct sec_tsp_ring sec_tsp_raw_main_ring;
static struct sec_tsp_ring sec_tsp_raw_sub_ring;
#else
static struct sec_tsp_ring sec_tsp_raw_ring;
#endif
static struct sec_tsp_ring sec_tsp_cmd_hist_ring;
static struct sec_tsp_ring sec_tsp_sponge_ring;

/**
 * sec_tsp_ring_init - Allocate storage and initialize a ring buffer
 * @ring: Target ring buffer
 * @size: Allocation size in bytes
 *
 * Return: 0 on success, -ENOMEM on allocation failure.
 */
static int __init sec_tsp_ring_init(struct sec_tsp_ring *ring, unsigned int size)
{
	ring->buf = kmalloc(size, GFP_KERNEL);
	if (!ring->buf)
		return -ENOMEM;

	ring->size = size;
	ring->head = 0;
	ring->fix = 0;
	spin_lock_init(&ring->lock);

	return 0;
}

/**
 * sec_tsp_ring_write - Append a formatted string to a TSP log ring buffer
 * @ring: Target ring buffer
 * @str: String data to append
 * @len: Length of string in bytes (excluding terminating null)
 *
 * Serialized by @ring->lock. If the text does not fit between @ring->head and
 * the end of the buffer, the write wraps around to @ring->fix (the fixed-index
 * anchor established at boot or via sec_tsp_log_fix). Text that exceeds the
 * remaining buffer capacity even from the anchor is dropped.
 */
static void sec_tsp_ring_write(struct sec_tsp_ring *ring, const char *str, size_t len)
{
	unsigned long flags;

	if (unlikely(!ring->buf || !ring->size || !len))
		return;

	spin_lock_irqsave(&ring->lock, flags);

	if (ring->head + len > ring->size - 1) {
		if (ring->fix + len > ring->size - 1) {
			spin_unlock_irqrestore(&ring->lock, flags);
			return;
		}
		ring->head = ring->fix;
	}

	memcpy(ring->buf + ring->head, str, len);
	ring->head += len;
	ring->buf[ring->head] = '\0';

	spin_unlock_irqrestore(&ring->lock, flags);
}

/**
 * sec_tsp_ring_proc_read - Generic procfs read handler for TSP ring buffers
 * @ring: Target ring buffer
 * @buf: Userspace buffer
 * @len: Maximum bytes requested
 * @offset: Current file offset pointer
 *
 * Return: Bytes copied, 0 on EOF, or -EFAULT on copy failure.
 */
static ssize_t sec_tsp_ring_proc_read(struct sec_tsp_ring *ring, char __user *buf,
				      size_t len, loff_t *offset)
{
	loff_t pos = *offset;
	size_t head, count;

	if (!ring->buf)
		return 0;

	if (pos < 0)
		return -EINVAL;

	head = READ_ONCE(ring->head);
	if (pos >= head)
		return 0;

	count = min(len, head - (size_t)pos);
	if (copy_to_user(buf, ring->buf + pos, count))
		return -EFAULT;

	*offset += count;
	return count;
}

/**
 * sec_tsp_format_timestamp - Format standard SEC TSP timestamp prefix
 * @buf: Destination buffer
 * @buf_size: Buffer capacity
 *
 * Return: Number of characters written (excluding null byte).
 */
static size_t sec_tsp_format_timestamp(char *buf, size_t buf_size)
{
	unsigned long long t = local_clock();
	unsigned long nanosec_rem = do_div(t, 1000000000);

	return snprintf(buf, buf_size, "[%5lu.%06lu] ",
			(unsigned long)t, nanosec_rem / 1000);
}

void sec_debug_tsp_log(char *fmt, ...)
{
	va_list args;
	char line[TSP_BUF_SIZE + 64];
	size_t tlen, len;

	if (unlikely(!sec_tsp_log_ring.buf))
		return;

	tlen = sec_tsp_format_timestamp(line, sizeof(line));
	if (tlen >= sizeof(line))
		return;

	va_start(args, fmt);
	len = vsnprintf(line + tlen, sizeof(line) - tlen, fmt, args);
	va_end(args);

	len = min(len, sizeof(line) - tlen - 2);
	line[tlen + len] = '\n';
	line[tlen + len + 1] = '\0';

	sec_tsp_ring_write(&sec_tsp_log_ring, line, tlen + len + 1);
}
EXPORT_SYMBOL(sec_debug_tsp_log);

void sec_debug_tsp_log_msg(char *msg, char *fmt, ...)
{
	va_list args;
	char line[TSP_BUF_SIZE + 128];
	size_t tlen, prefix_len, len;

	if (unlikely(!sec_tsp_log_ring.buf))
		return;

	tlen = sec_tsp_format_timestamp(line, sizeof(line));
	if (tlen >= sizeof(line))
		return;

	prefix_len = snprintf(line + tlen, sizeof(line) - tlen, "%s : ",
			      msg ? msg : "");
	if (prefix_len >= sizeof(line) - tlen)
		return;

	va_start(args, fmt);
	len = vsnprintf(line + tlen + prefix_len,
			sizeof(line) - (tlen + prefix_len), fmt, args);
	va_end(args);

	len = min(len, sizeof(line) - (tlen + prefix_len) - 1);
	sec_tsp_ring_write(&sec_tsp_log_ring, line, tlen + prefix_len + len);
}
EXPORT_SYMBOL(sec_debug_tsp_log_msg);

#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
void sec_debug_tsp_raw_data(char *fmt, ...)
{
}
EXPORT_SYMBOL(sec_debug_tsp_raw_data);

void sec_debug_tsp_raw_data_msg(char mode, char *msg, char *fmt, ...)
{
	struct sec_tsp_ring *ring;
	va_list args;
	char line[TSP_BUF_SIZE + 128];
	size_t tlen, prefix_len, len;

	ring = (mode == MAIN_TOUCH) ? &sec_tsp_raw_main_ring : &sec_tsp_raw_sub_ring;
	if (unlikely(!ring->buf))
		return;

	tlen = sec_tsp_format_timestamp(line, sizeof(line));
	if (tlen >= sizeof(line))
		return;

	prefix_len = snprintf(line + tlen, sizeof(line) - tlen, "%s : ",
			      msg ? msg : "");
	if (prefix_len >= sizeof(line) - tlen)
		return;

	va_start(args, fmt);
	len = vsnprintf(line + tlen + prefix_len,
			sizeof(line) - (tlen + prefix_len), fmt, args);
	va_end(args);

	len = min(len, sizeof(line) - (tlen + prefix_len) - 1);
	sec_tsp_ring_write(ring, line, tlen + prefix_len + len);
}
EXPORT_SYMBOL(sec_debug_tsp_raw_data_msg);

void sec_tsp_raw_data_clear(char mode)
{
	struct sec_tsp_ring *ring = (mode == MAIN_TOUCH) ?
				    &sec_tsp_raw_main_ring : &sec_tsp_raw_sub_ring;
	unsigned long flags;

	if (!ring->buf)
		return;

	spin_lock_irqsave(&ring->lock, flags);
	ring->head = 0;
	memset(ring->buf, 0, ring->size);
	spin_unlock_irqrestore(&ring->lock, flags);
}
EXPORT_SYMBOL(sec_tsp_raw_data_clear);
#else
void sec_debug_tsp_raw_data(char *fmt, ...)
{
	va_list args;
	char line[TSP_BUF_SIZE + 64];
	size_t tlen, len;

	if (unlikely(!sec_tsp_raw_ring.buf))
		return;

	tlen = sec_tsp_format_timestamp(line, sizeof(line));
	if (tlen >= sizeof(line))
		return;

	va_start(args, fmt);
	len = vsnprintf(line + tlen, sizeof(line) - tlen, fmt, args);
	va_end(args);

	len = min(len, sizeof(line) - tlen - 2);
	line[tlen + len] = '\n';
	line[tlen + len + 1] = '\0';

	sec_tsp_ring_write(&sec_tsp_raw_ring, line, tlen + len + 1);
}
EXPORT_SYMBOL(sec_debug_tsp_raw_data);

void sec_debug_tsp_raw_data_msg(char *msg, char *fmt, ...)
{
	va_list args;
	char line[TSP_BUF_SIZE + 128];
	size_t tlen, prefix_len, len;

	if (unlikely(!sec_tsp_raw_ring.buf))
		return;

	tlen = sec_tsp_format_timestamp(line, sizeof(line));
	if (tlen >= sizeof(line))
		return;

	prefix_len = snprintf(line + tlen, sizeof(line) - tlen, "%s : ",
			      msg ? msg : "");
	if (prefix_len >= sizeof(line) - tlen)
		return;

	va_start(args, fmt);
	len = vsnprintf(line + tlen + prefix_len,
			sizeof(line) - (tlen + prefix_len), fmt, args);
	va_end(args);

	len = min(len, sizeof(line) - (tlen + prefix_len) - 1);
	sec_tsp_ring_write(&sec_tsp_raw_ring, line, tlen + prefix_len + len);
}
EXPORT_SYMBOL(sec_debug_tsp_raw_data_msg);

void sec_tsp_raw_data_clear(void)
{
	unsigned long flags;

	if (!sec_tsp_raw_ring.buf)
		return;

	spin_lock_irqsave(&sec_tsp_raw_ring.lock, flags);
	sec_tsp_raw_ring.head = 0;
	memset(sec_tsp_raw_ring.buf, 0, sec_tsp_raw_ring.size);
	spin_unlock_irqrestore(&sec_tsp_raw_ring.lock, flags);
}
EXPORT_SYMBOL(sec_tsp_raw_data_clear);
#endif

void sec_debug_tsp_command_history(char *buf)
{
	char line[256];
	size_t len;

	if (unlikely(!sec_tsp_cmd_hist_ring.buf || !buf))
		return;

	len = snprintf(line, sizeof(line), "%s ", buf);
	len = min(len, sizeof(line) - 1);

	sec_tsp_ring_write(&sec_tsp_cmd_hist_ring, line, len);
}
EXPORT_SYMBOL(sec_debug_tsp_command_history);

void sec_tsp_log_fix(void)
{
	char line[64];
	size_t tlen, flen;
	unsigned long flags;

	if (unlikely(!sec_tsp_log_ring.buf))
		return;

	tlen = sec_tsp_format_timestamp(line, sizeof(line));
	flen = snprintf(line + tlen, sizeof(line) - tlen, "FIX LOG!\n");

	sec_tsp_ring_write(&sec_tsp_log_ring, line, tlen + flen);

	spin_lock_irqsave(&sec_tsp_log_ring.lock, flags);
	sec_tsp_log_ring.fix = sec_tsp_log_ring.head;
	spin_unlock_irqrestore(&sec_tsp_log_ring.lock, flags);
}
EXPORT_SYMBOL(sec_tsp_log_fix);

void sec_tsp_sponge_log(char *buf)
{
	char line[256];
	size_t len;

	if (unlikely(!sec_tsp_sponge_ring.buf || !buf))
		return;

	len = snprintf(line, sizeof(line), "%s ", buf);
	len = min(len, sizeof(line) - 1);

	sec_tsp_ring_write(&sec_tsp_sponge_ring, line, len);
}
EXPORT_SYMBOL(sec_tsp_sponge_log);

static ssize_t sec_tsp_log_proc_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char *page;
	unsigned int val;

	if (!sec_tsp_log_ring.buf)
		return 0;

	if (count >= PAGE_SIZE)
		return -EINVAL;

	page = memdup_user_nul(buf, count);
	if (IS_ERR(page))
		return PTR_ERR(page);

	if (kstrtouint(page, 10, &val) != 0) {
		pr_info("%s\n", page);
		sec_debug_tsp_log("%s", page);
	}

	kfree(page);
	return count;
}

static ssize_t sec_tsp_raw_data_proc_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	char *page;
	unsigned int val;

#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
	if (!sec_tsp_raw_main_ring.buf)
		return 0;
#else
	if (!sec_tsp_raw_ring.buf)
		return 0;
#endif

	if (count >= PAGE_SIZE)
		return -EINVAL;

	page = memdup_user_nul(buf, count);
	if (IS_ERR(page))
		return PTR_ERR(page);

	if (kstrtouint(page, 10, &val) != 0) {
		pr_info("%s\n", page);
		sec_debug_tsp_raw_data("%s", page);
	}

	kfree(page);
	return count;
}

static ssize_t sec_tsp_log_read(struct file *file, char __user *buf,
				size_t len, loff_t *offset)
{
	return sec_tsp_ring_proc_read(&sec_tsp_log_ring, buf, len, offset);
}

static const struct file_operations tsp_msg_file_ops = {
	.owner		= THIS_MODULE,
	.read		= sec_tsp_log_read,
	.write		= sec_tsp_log_proc_write,
	.llseek		= generic_file_llseek,
};

#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
static ssize_t sec_tsp_raw_data_read(struct file *file, char __user *buf,
				     size_t len, loff_t *offset)
{
	loff_t pos = *offset;
	size_t main_head, sub_head, total, count;

	if (!sec_tsp_raw_main_ring.buf || !sec_tsp_raw_sub_ring.buf)
		return 0;

	if (pos < 0)
		return -EINVAL;

	main_head = READ_ONCE(sec_tsp_raw_main_ring.head);
	sub_head = READ_ONCE(sec_tsp_raw_sub_ring.head);
	total = main_head + sub_head;

	if (pos >= total)
		return 0;

	if (pos < main_head) {
		count = min(len, main_head - (size_t)pos);
		if (copy_to_user(buf, sec_tsp_raw_main_ring.buf + pos, count))
			return -EFAULT;
	} else {
		size_t sub_pos = (size_t)pos - main_head;

		count = min(len, sub_head - sub_pos);
		if (copy_to_user(buf, sec_tsp_raw_sub_ring.buf + sub_pos, count))
			return -EFAULT;
	}

	*offset += count;
	return count;
}
#else
static ssize_t sec_tsp_raw_data_read(struct file *file, char __user *buf,
				     size_t len, loff_t *offset)
{
	return sec_tsp_ring_proc_read(&sec_tsp_raw_ring, buf, len, offset);
}
#endif

static const struct file_operations tsp_raw_data_file_ops = {
	.owner		= THIS_MODULE,
	.read		= sec_tsp_raw_data_read,
	.write		= sec_tsp_raw_data_proc_write,
	.llseek		= generic_file_llseek,
};

static ssize_t sec_tsp_command_history_read(struct file *file, char __user *buf,
					    size_t len, loff_t *offset)
{
	return sec_tsp_ring_proc_read(&sec_tsp_cmd_hist_ring, buf, len, offset);
}

static const struct file_operations tsp_command_history_file_ops = {
	.owner		= THIS_MODULE,
	.read		= sec_tsp_command_history_read,
	.llseek		= generic_file_llseek,
};

static ssize_t sec_tsp_sponge_log_read(struct file *file, char __user *buf,
				       size_t len, loff_t *offset)
{
	return sec_tsp_ring_proc_read(&sec_tsp_sponge_ring, buf, len, offset);
}

static const struct file_operations tsp_sponge_log_file_ops = {
	.owner		= THIS_MODULE,
	.read		= sec_tsp_sponge_log_read,
	.llseek		= generic_file_llseek,
};

static int __init sec_tsp_log_late_init(void)
{
	struct proc_dir_entry *entry;

	if (sec_tsp_log_ring.buf) {
		entry = proc_create("tsp_msg", 0440, NULL, &tsp_msg_file_ops);
		if (entry)
			proc_set_size(entry, sec_tsp_log_ring.size);
		else
			pr_err("%s: failed to create proc entry tsp_msg\n", __func__);
	}

#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
	if (sec_tsp_raw_main_ring.buf) {
		entry = proc_create("tsp_raw_data", 0444, NULL, &tsp_raw_data_file_ops);
		if (entry)
			proc_set_size(entry, sec_tsp_raw_main_ring.size +
					     sec_tsp_raw_sub_ring.size);
		else
			pr_err("%s: failed to create proc entry tsp_raw_data\n", __func__);
	}
#else
	if (sec_tsp_raw_ring.buf) {
		entry = proc_create("tsp_raw_data", 0444, NULL, &tsp_raw_data_file_ops);
		if (entry)
			proc_set_size(entry, sec_tsp_raw_ring.size);
		else
			pr_err("%s: failed to create proc entry tsp_raw_data\n", __func__);
	}
#endif

	if (sec_tsp_cmd_hist_ring.buf) {
		entry = proc_create("tsp_cmd_hist", 0444, NULL, &tsp_command_history_file_ops);
		if (entry)
			proc_set_size(entry, sec_tsp_cmd_hist_ring.size);
		else
			pr_err("%s: failed to create proc entry tsp_cmd_hist\n", __func__);
	}

	if (sec_tsp_sponge_ring.buf) {
		entry = proc_create("tsp_sponge_log", 0444, NULL, &tsp_sponge_log_file_ops);
		if (entry)
			proc_set_size(entry, sec_tsp_sponge_ring.size);
		else
			pr_err("%s: failed to create proc entry tsp_sponge_log\n", __func__);
	}

	return 0;
}
late_initcall(sec_tsp_log_late_init);

static int __init __init_sec_tsp_log(void)
{
	int ret;

	ret = sec_tsp_ring_init(&sec_tsp_log_ring, SEC_TSP_LOG_BUF_SIZE);
	if (ret)
		return ret;

#ifdef CONFIG_TOUCHSCREEN_DUAL_FOLDABLE
	ret = sec_tsp_ring_init(&sec_tsp_raw_main_ring, SEC_TSP_RAW_DATA_BUF_SIZE / 2);
	if (ret)
		return ret;
	ret = sec_tsp_ring_init(&sec_tsp_raw_sub_ring, SEC_TSP_RAW_DATA_BUF_SIZE / 2);
	if (ret)
		return ret;
#else
	ret = sec_tsp_ring_init(&sec_tsp_raw_ring, SEC_TSP_RAW_DATA_BUF_SIZE);
	if (ret)
		return ret;
#endif

	ret = sec_tsp_ring_init(&sec_tsp_cmd_hist_ring, SEC_TSP_COMMAND_HISTORY_BUF_SIZE);
	if (ret)
		return ret;

	ret = sec_tsp_ring_init(&sec_tsp_sponge_ring, SEC_TSP_SPONGE_LOG_BUF_SIZE);
	if (ret)
		return ret;

	pr_info("%s: init done\n", __func__);
	return 0;
}
fs_initcall(__init_sec_tsp_log);

#endif /* CONFIG_SEC_DEBUG_TSP_LOG */
