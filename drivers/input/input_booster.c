// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Input Booster core driver
 */

#include <linux/input/input_booster.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>

/*
 * Locking Hierarchy & Concurrency Model:
 *
 * 1. ib_type_lock (spinlock_irq):
 *    - Protects the input hot path in evdev.c (client event buffer traversal
 *      and trigger slot allocation via ib_trigger_get_slot()).
 *    - Never sleeps; leaf lock in interrupt/softirq context.
 *
 * 2. trigger_ib_lock (mutex):
 *    - Serializes instance lifecycle management (instance creation,
 *      finding active instances, and destruction).
 *    - Prevents concurrent trigger events and timeout worker from racing
 *      on the same instance in ib_list[dev_type].
 *
 * 3. ib->lock (per-instance mutex):
 *    - Protects instance state transitions (rel_flag, isHeadFinished).
 *    - Must be acquired AFTER trigger_ib_lock if both are held.
 *    - NEVER held while calling into pm_qos / msm_bus hardware backends.
 *
 * 4. rel_ib_lock (mutex):
 *    - Serializes hardware boost application (ib_set_booster, ib_release_booster).
 *    - Taken independently to ensure only one thread programs frequency/voltage
 *      subsystems at a time.
 *
 * 5. write_ib_lock (spinlock):
 *    - Protects additions and deletions to ib_list[dev_type].
 *    - RCU read-side (rcu_read_lock) used for lockless lookups.
 *
 * 6. write_qos_lock (spinlock):
 *    - Protects additions, removals, and value updates in qos_list[res_id].
 *    - RCU read-side (rcu_read_lock) used for get_qos_value().
 *
 * Lock Order:
 *   trigger_ib_lock -> ib->lock -> write_ib_lock / write_qos_lock
 *   rel_ib_lock is independent and never nested within ib->lock or spinlocks.
 *   No lock is ever acquired twice on any single execution path.
 */

spinlock_t write_ib_lock;
spinlock_t write_qos_lock;
spinlock_t ib_type_lock;
struct mutex trigger_ib_lock;
struct mutex rel_ib_lock;

struct workqueue_struct *ev_unbound_wq;
struct workqueue_struct *ib_unbound_highwq;

int total_ib_cnt;
int ib_init_succeed;
int level_value = IB_MAX;

unsigned int debug_flag;
unsigned int enable_event_booster;

int release_val[MAX_RES_COUNT];
int device_count;
struct t_ib_device_tree *ib_device_trees;
struct t_ib_trigger *ib_trigger;
int max_resource_size;

struct list_head *ib_list;
struct list_head *qos_list;

int trigger_cnt;
int send_ev_enable;

/*
 * Claim a free trigger slot for the caller, or return -EBUSY when every slot is
 * still owned by a queued/running worker.
 */
int ib_trigger_get_slot(void)
{
	int slot = trigger_cnt;
	int i;

	for (i = 0; i < MAX_IB_COUNT; i++) {
		/* Pairs with smp_store_release in trigger_input_booster */
		if (!smp_load_acquire(&ib_trigger[slot].in_use))
			break;
		slot = (slot + 1) % MAX_IB_COUNT;
	}

	if (i == MAX_IB_COUNT)
		return -EBUSY;

	/* Pairs with smp_load_acquire in trigger_input_booster */
	smp_store_release(&ib_trigger[slot].in_use, 1);
	trigger_cnt = (slot + 1) % MAX_IB_COUNT;

	return slot;
}

bool is_validate_uniqid(unsigned int uniq_id)
{
	int dev_type;
	struct t_ib_info *ib;

	rcu_read_lock();
	for (dev_type = 0; dev_type < MAX_DEVICE_TYPE_NUM; dev_type++) {
		list_for_each_entry_rcu(ib, &ib_list[dev_type], list) {
			if (ib->uniq_id == uniq_id) {
				rcu_read_unlock();
				return false;
			}
		}
	}
	rcu_read_unlock();
	return true;
}

struct t_ib_info *find_release_ib(int dev_type, int key_id)
{
	struct t_ib_info *ib;

	if (dev_type < 0 || dev_type >= MAX_DEVICE_TYPE_NUM)
		return NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(ib, &ib_list[dev_type], list) {
		if (ib->key_id == key_id && ib->rel_flag == FLAG_OFF) {
			rcu_read_unlock();
			return ib;
		}
	}
	rcu_read_unlock();
	return NULL;
}

unsigned long get_qos_value(int res_id)
{
	struct t_ib_target *tv;
	int max_val = 0;

	if (res_id < 0 || res_id >= max_resource_size)
		return 0;

	rcu_read_lock();
	list_for_each_entry_rcu(tv, &qos_list[res_id], list) {
		if (tv->value > max_val)
			max_val = tv->value;
	}
	rcu_read_unlock();

	return max_val;
}

static void update_instance_targets(int uniq_id, struct t_ib_device_tree *dt,
				    bool is_tail)
{
	int res_type;

	spin_lock(&write_qos_lock);
	for (res_type = 0; res_type < max_resource_size; res_type++) {
		int target_val = is_tail ? dt->res[res_type].tail_value :
					   dt->res[res_type].head_value;
		struct t_ib_target *tv;

		if (target_val == 0)
			continue;

		list_for_each_entry(tv, &qos_list[res_type], list) {
			if (tv->uniq_id == uniq_id) {
				tv->value = target_val;
				break;
			}
		}
	}
	spin_unlock(&write_qos_lock);
}

static void detach_instance_targets(int uniq_id, int *qos_values,
				    long *rel_flags)
{
	int res_type;

	spin_lock(&write_qos_lock);
	for (res_type = 0; res_type < max_resource_size; res_type++) {
		struct t_ib_target *tv;

		list_for_each_entry(tv, &qos_list[res_type], list) {
			if (tv->uniq_id == uniq_id) {
				list_del_rcu(&tv->list);
				kfree_rcu(tv, rcu);
				break;
			}
		}
	}
	spin_unlock(&write_qos_lock);

	for (res_type = 0; res_type < max_resource_size; res_type++) {
		int max_val = 0;
		struct t_ib_target *tv;

		rcu_read_lock();
		if (list_empty(&qos_list[res_type])) {
			rel_flags[res_type] = 1;
		} else {
			list_for_each_entry_rcu(tv, &qos_list[res_type], list) {
				if (tv->value > max_val)
					max_val = tv->value;
			}
		}
		rcu_read_unlock();
		qos_values[res_type] = max_val;
	}
}

void remove_ib_instance(struct t_ib_info *target_ib)
{
	int dev_type = target_ib->ib_dt->type;
	struct t_ib_info *ib;
	bool found = false;

	/*
	 * Serialize with trigger_input_booster(). find_release_ib() returns an
	 * RCU-protected pointer, and trigger_input_booster() accesses it while
	 * holding trigger_ib_lock. Holding trigger_ib_lock here ensures an
	 * instance cannot be freed while the trigger path is modifying it.
	 */
	mutex_lock(&trigger_ib_lock);

	spin_lock(&write_ib_lock);
	list_for_each_entry(ib, &ib_list[dev_type], list) {
		if (ib == target_ib) {
			list_del_rcu(&target_ib->list);
			found = true;
			break;
		}
	}
	spin_unlock(&write_ib_lock);

	mutex_unlock(&trigger_ib_lock);

	if (found)
		kfree_rcu(target_ib, rcu);
	else
		pr_err(ITAG "Del Ib Fail Id : %d\n", target_ib->uniq_id);
}

void press_state_func(struct work_struct *work)
{
	struct t_ib_info *target_ib =
		container_of(work, struct t_ib_info, ib_state_work[IB_HEAD]);
	int qos_values[MAX_RES_COUNT] = {0};
	int res_type;

	update_instance_targets(target_ib->uniq_id, target_ib->ib_dt, false);

	for (res_type = 0; res_type < max_resource_size; res_type++)
		qos_values[res_type] = get_qos_value(res_type);

	mutex_lock(&rel_ib_lock);
	ib_set_booster(qos_values);
	mutex_unlock(&rel_ib_lock);

	queue_delayed_work(ib_unbound_highwq,
			   &target_ib->ib_timeout_work[IB_HEAD],
			   msecs_to_jiffies(target_ib->ib_dt->head_time));
}

void press_timeout_func(struct work_struct *work)
{
	struct t_ib_info *target_ib =
		container_of(work, struct t_ib_info, ib_timeout_work[IB_HEAD].work);
	int qos_values[MAX_RES_COUNT] = {0};
	long rel_flags[MAX_RES_COUNT] = {0};

	if (target_ib->ib_dt->tail_time != 0) {
		queue_work(ib_unbound_highwq,
			   &target_ib->ib_state_work[IB_TAIL]);
	} else {
		detach_instance_targets(target_ib->uniq_id, qos_values, rel_flags);

		mutex_lock(&rel_ib_lock);
		ib_release_booster(rel_flags);
		ib_set_booster(qos_values);
		mutex_unlock(&rel_ib_lock);

		remove_ib_instance(target_ib);
	}
}

void release_state_func(struct work_struct *work)
{
	struct t_ib_info *target_ib =
		container_of(work, struct t_ib_info, ib_state_work[IB_TAIL]);
	int qos_values[MAX_RES_COUNT] = {0};
	int res_type;
	bool do_tail_timeout;

	mutex_lock(&target_ib->lock);
	target_ib->isHeadFinished = 1;
	do_tail_timeout = (target_ib->rel_flag == FLAG_ON);
	mutex_unlock(&target_ib->lock);

	update_instance_targets(target_ib->uniq_id, target_ib->ib_dt, true);

	for (res_type = 0; res_type < max_resource_size; res_type++)
		qos_values[res_type] = get_qos_value(res_type);

	mutex_lock(&rel_ib_lock);
	ib_set_booster(qos_values);
	mutex_unlock(&rel_ib_lock);

	if (do_tail_timeout &&
	    !delayed_work_pending(&target_ib->ib_timeout_work[IB_TAIL])) {
		queue_delayed_work(ib_unbound_highwq,
				   &target_ib->ib_timeout_work[IB_TAIL],
				   msecs_to_jiffies(target_ib->ib_dt->tail_time));
	}
}

void release_timeout_func(struct work_struct *work)
{
	struct t_ib_info *target_ib =
		container_of(work, struct t_ib_info, ib_timeout_work[IB_TAIL].work);
	int qos_values[MAX_RES_COUNT] = {0};
	long rel_flags[MAX_RES_COUNT] = {0};

	detach_instance_targets(target_ib->uniq_id, qos_values, rel_flags);

	mutex_lock(&rel_ib_lock);
	ib_release_booster(rel_flags);
	ib_set_booster(qos_values);
	mutex_unlock(&rel_ib_lock);

	remove_ib_instance(target_ib);
}

struct t_ib_info *create_ib_instance(struct t_ib_trigger *p_ib_trigger, int uniq_id)
{
	int dev_type = p_ib_trigger->dev_type;
	struct t_ib_info *ib;

	ib = kmalloc(sizeof(*ib), GFP_KERNEL);
	if (!ib)
		return NULL;

	ib->dev_name = p_ib_trigger->dev_name;
	ib->key_id = p_ib_trigger->key_id;
	ib->uniq_id = uniq_id;
	ib->press_flag = FLAG_OFF;
	ib->rel_flag = FLAG_OFF;
	ib->isHeadFinished = 0;
	ib->ib_dt = &ib_device_trees[dev_type];

	INIT_WORK(&ib->ib_state_work[IB_HEAD], press_state_func);
	INIT_DELAYED_WORK(&ib->ib_timeout_work[IB_HEAD], press_timeout_func);
	INIT_WORK(&ib->ib_state_work[IB_TAIL], release_state_func);
	INIT_DELAYED_WORK(&ib->ib_timeout_work[IB_TAIL], release_timeout_func);
	mutex_init(&ib->lock);

	spin_lock(&write_ib_lock);
	list_add_tail_rcu(&ib->list, &ib_list[dev_type]);
	spin_unlock(&write_ib_lock);

	return ib;
}

void trigger_input_booster(struct work_struct *work)
{
	struct t_ib_trigger *p_ib_trigger =
		container_of(work, struct t_ib_trigger, ib_trigger_work);
	int dev_type;
	struct t_ib_info *ib;

	if (!p_ib_trigger)
		return;

	dev_type = p_ib_trigger->dev_type;
	if (dev_type < 0 || dev_type >= MAX_DEVICE_TYPE_NUM ||
	    !ib_device_trees[dev_type].label)
		goto out_release_slot;

	pr_booster("IB Trigger :: %s(%d) %s || key_id : %d\n",
		   ib_device_trees[dev_type].label, dev_type,
		   (p_ib_trigger->event_type) ? "PRESS" : "RELEASE",
		   p_ib_trigger->key_id);

	mutex_lock(&trigger_ib_lock);

	if (p_ib_trigger->event_type == BOOSTER_ON) {
		unsigned int uniq_id = 0;
		int res_type;
		int i;

		if (find_release_ib(dev_type, p_ib_trigger->key_id)) {
			pr_booster(ITAG "IB Trigger :: ib already exist. Key(%d)\n",
				   p_ib_trigger->key_id);
			goto out_unlock;
		}

		/* Claim a free unique ID bounded to MAX_IB_COUNT */
		for (i = 0; i < MAX_IB_COUNT; i++) {
			uniq_id = total_ib_cnt++;
			if (total_ib_cnt == MAX_IB_COUNT)
				total_ib_cnt = 0;

			if (is_validate_uniqid(uniq_id))
				break;
		}

		if (i == MAX_IB_COUNT) {
			pr_err(ITAG "all %d uniq ids in use, drop event\n",
			       MAX_IB_COUNT);
			goto out_unlock;
		}

		ib = create_ib_instance(p_ib_trigger, uniq_id);
		if (!ib)
			goto out_unlock;

		ib->press_flag = FLAG_ON;

		/* Insert resource targets under a single write_qos_lock section */
		spin_lock(&write_qos_lock);
		for (res_type = 0; res_type < max_resource_size; res_type++) {
			if (ib->ib_dt->res[res_type].head_value != 0) {
				struct t_ib_target *tv;

				tv = kmalloc(sizeof(*tv), GFP_ATOMIC);
				if (!tv)
					continue;

				tv->uniq_id = ib->uniq_id;
				tv->value = 0;
				list_add_tail_rcu(&tv->list, &qos_list[res_type]);
			}
		}
		spin_unlock(&write_qos_lock);

		queue_work(ib_unbound_highwq, &ib->ib_state_work[IB_HEAD]);
	} else {
		ib = find_release_ib(dev_type, p_ib_trigger->key_id);
		if (!ib) {
			pr_debug("IB is null on release\n");
			goto out_unlock;
		}

		mutex_lock(&ib->lock);
		ib->rel_flag = FLAG_ON;
		if (ib->isHeadFinished &&
		    !delayed_work_pending(&ib->ib_timeout_work[IB_TAIL])) {
			queue_delayed_work(ib_unbound_highwq,
					   &ib->ib_timeout_work[IB_TAIL],
					   msecs_to_jiffies(ib->ib_dt->tail_time));
		}
		mutex_unlock(&ib->lock);
	}

out_unlock:
	mutex_unlock(&trigger_ib_lock);

out_release_slot:
	/* Pairs with smp_load_acquire in ib_trigger_get_slot */
	smp_store_release(&p_ib_trigger->in_use, 0);
}

unsigned int create_uniq_id(int type, int code, int slot)
{
	pr_booster("Create Key Id -> type(%d), code(%d), slot(%d)\n",
		   type, code, slot);
	return ((unsigned int)type << (TYPE_BITS + CODE_BITS)) |
	       ((unsigned int)code << CODE_BITS) |
	       (unsigned int)slot;
}

void ib_auto_test(int type, int code, int val)
{
	send_ev_enable = 1;
}

SYSFS_CLASS(enable_event, enable_event_booster, "%u\n")
SYSFS_CLASS(debug_level, debug_flag, "%u\n")
SYSFS_CLASS(sendevent, send_ev_enable, "%d\n")
HEAD_TAIL_SYSFS_DEVICE(head)
HEAD_TAIL_SYSFS_DEVICE(tail)
LEVEL_SYSFS_DEVICE(level)

struct attribute *dvfs_attributes[] = {
	&dev_attr_head.attr,
	&dev_attr_tail.attr,
	&dev_attr_level.attr,
	NULL,
};

struct attribute_group dvfs_attr_group = {
	.attrs = dvfs_attributes,
};

void init_sysfs_device(struct class *sysfs_class, struct t_ib_device_tree *ib_dt)
{
	struct device *sysfs_dev;
	int ret;

	sysfs_dev = device_create(sysfs_class, NULL, 0, ib_dt, "%s", ib_dt->label);
	if (IS_ERR(sysfs_dev)) {
		pr_booster("[Input Booster] Failed to create %s sysfs device[%ld]\n",
			   ib_dt->label, PTR_ERR(sysfs_dev));
		return;
	}

	ret = sysfs_create_group(&sysfs_dev->kobj, &dvfs_attr_group);
	if (ret)
		pr_booster("[Input Booster] Failed to create %s sysfs group\n",
			   ib_dt->label);
}

int is_ib_init_succeed(void)
{
	return (ib_trigger && ib_device_trees && ib_list && qos_list) ? 1 : 0;
}

void input_booster_exit(void)
{
	int i;

	kfree(ib_trigger);
	ib_trigger = NULL;

	if (ib_device_trees) {
		for (i = 0; i < MAX_DEVICE_TYPE_NUM; i++)
			kfree(ib_device_trees[i].res);
		kfree(ib_device_trees);
		ib_device_trees = NULL;
	}

	kfree(ib_list);
	ib_list = NULL;

	kfree(qos_list);
	qos_list = NULL;

	if (ev_unbound_wq) {
		destroy_workqueue(ev_unbound_wq);
		ev_unbound_wq = NULL;
	}
	if (ib_unbound_highwq) {
		destroy_workqueue(ib_unbound_highwq);
		ib_unbound_highwq = NULL;
	}

	input_booster_exit_vendor();
}

void input_booster_init(void)
{
	struct device_node *np;
	struct device_node *cnp;
	char rel_val_str[100];
	char *rel_val_ptr = rel_val_str;
	const char *rel_vals;
	const char *max_res_prop;
	const char *token;
	int res_type = 0;
	int i;

	spin_lock_init(&write_ib_lock);
	spin_lock_init(&write_qos_lock);
	spin_lock_init(&ib_type_lock);
	mutex_init(&trigger_ib_lock);
	mutex_init(&rel_ib_lock);

	ev_unbound_wq = alloc_ordered_workqueue("ev_unbound_wq", WQ_HIGHPRI);
	ib_unbound_highwq = alloc_workqueue("ib_unbound_high_wq",
					    WQ_UNBOUND | WQ_HIGHPRI,
					    MAX_IB_COUNT);
	if (!ev_unbound_wq || !ib_unbound_highwq)
		goto err_free;

	ib_trigger = kcalloc(MAX_IB_COUNT, sizeof(*ib_trigger), GFP_KERNEL);
	if (!ib_trigger)
		goto err_free;

	for (i = 0; i < MAX_IB_COUNT; i++)
		INIT_WORK(&ib_trigger[i].ib_trigger_work, trigger_input_booster);

	ib_device_trees = kcalloc(MAX_DEVICE_TYPE_NUM, sizeof(*ib_device_trees), GFP_KERNEL);
	if (!ib_device_trees)
		goto err_free;

	ib_list = kcalloc(MAX_DEVICE_TYPE_NUM, sizeof(*ib_list), GFP_KERNEL);
	if (!ib_list)
		goto err_free;

	for (i = 0; i < MAX_DEVICE_TYPE_NUM; i++) {
		INIT_LIST_HEAD(&ib_list[i]);
		ib_device_trees[i].type = i;
		ib_device_trees[i].res = kcalloc(MAX_RES_COUNT,
						 sizeof(struct t_ib_res_info),
						 GFP_KERNEL);
		if (!ib_device_trees[i].res)
			goto err_free;
		for (res_type = 0; res_type < MAX_RES_COUNT; res_type++)
			ib_device_trees[i].res[res_type].res_id = res_type;
	}

	np = of_find_compatible_node(NULL, NULL, "input_booster");
	if (!np)
		goto err_free;

	max_res_prop = of_get_property(np, "max_resource_count", NULL);
	if (!max_res_prop || kstrtoint(max_res_prop, 10, &max_resource_size)) {
		pr_err(ITAG "max_resource_count missing or invalid\n");
		goto err_free_node;
	}

	if (max_resource_size > MAX_RES_COUNT) {
		pr_err(ITAG "max_resource_count %d clamped to MAX_RES_COUNT %d\n",
		       max_resource_size, MAX_RES_COUNT);
		max_resource_size = MAX_RES_COUNT;
	}

	qos_list = kcalloc(max_resource_size, sizeof(*qos_list), GFP_KERNEL);
	if (!qos_list)
		goto err_free_node;

	rel_vals = of_get_property(np, "ib_release_values", NULL);
	if (!rel_vals) {
		pr_err(ITAG "ib_release_values missing from DT\n");
		goto err_free_node;
	}

	strscpy(rel_val_str, rel_vals, sizeof(rel_val_str));
	res_type = 0;
	while ((token = strsep(&rel_val_ptr, ",")) != NULL &&
	       res_type < max_resource_size) {
		if (!kstrtoint(token, 10, &release_val[res_type]))
			INIT_LIST_HEAD(&qos_list[res_type]);
		res_type++;
	}

	if (res_type < max_resource_size) {
		pr_err(ITAG "release value parse fail\n");
		goto err_free_node;
	}

	device_count = 0;
	for_each_child_of_node(np, cnp) {
		struct t_ib_device_tree *ib_dt;
		struct device_node *res_node;
		u32 type = 0;

		if (of_property_read_u32(cnp, "input_booster,type", &type))
			continue;

		if (type >= MAX_DEVICE_TYPE_NUM) {
			pr_err(ITAG "device type %u >= MAX_DEVICE_TYPE_NUM\n", type);
			continue;
		}

		ib_dt = &ib_device_trees[type];
		ib_dt->label = of_get_property(cnp, "input_booster,label", NULL);
		of_property_read_u32(cnp, "input_booster,head_time", &ib_dt->head_time);
		of_property_read_u32(cnp, "input_booster,tail_time", &ib_dt->tail_time);

		res_node = of_find_compatible_node(cnp, NULL, "resource");
		if (res_node) {
			struct device_node *child_res;
			int res_idx = 0;

			for_each_child_of_node(res_node, child_res) {
				if (res_idx >= max_resource_size) {
					of_node_put(child_res);
					break;
				}
				ib_dt->res[res_idx].res_id = res_idx;
				ib_dt->res[res_idx].label =
					of_get_property(child_res, "resource,label", NULL);
				of_property_read_u32_index(child_res, "resource,value",
							   IB_HEAD,
							   &ib_dt->res[res_idx].head_value);
				of_property_read_u32_index(child_res, "resource,value",
							   IB_TAIL,
							   &ib_dt->res[res_idx].tail_value);
				res_idx++;
			}
			of_node_put(res_node);
		}
		device_count++;
	}

	ib_init_succeed = is_ib_init_succeed();
	of_node_put(np);

	if (ib_init_succeed) {
		struct class *sysfs_class;
		int ib_type;
		int ret;

		sysfs_class = class_create(THIS_MODULE, "input_booster");
		if (!IS_ERR(sysfs_class)) {
			INIT_SYSFS_CLASS(enable_event)
			INIT_SYSFS_CLASS(debug_level)
			INIT_SYSFS_CLASS(sendevent)

			for (ib_type = 0; ib_type < MAX_DEVICE_TYPE_NUM; ib_type++) {
				if (ib_device_trees[ib_type].label)
					init_sysfs_device(sysfs_class, &ib_device_trees[ib_type]);
			}
		}
	}

#if !defined(CONFIG_ARCH_QCOM) && !defined(CONFIG_ARCH_EXYNOS)
	pr_err(ITAG "At least, one vendor feature needed\n");
#else
	input_booster_init_vendor(release_val);
#endif
	return;

err_free_node:
	of_node_put(np);
err_free:
	input_booster_exit();
}

