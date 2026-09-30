// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Input Booster - evdev event filter
 */

#ifdef CONFIG_SEC_INPUT_BOOSTER

static inline bool chk_next_data(struct evdev_client *dev, int idx, int input_type)
{
	int next_idx = (idx + 1) & (dev->bufsize - 1);
	int next_type = dev->buffer[next_idx].type;
	int next_code = dev->buffer[next_idx].code;

	switch (input_type) {
	case BTN_TOUCH:
		return (next_type == EV_ABS && next_code == ABS_PRESSURE);
	case EV_KEY:
		return (next_type == EV_KEY);
	default:
		return false;
	}
}

static inline int chk_boost_on_off(struct evdev_client *dev, int idx, int dev_type)
{
	int val;

	if (dev_type < 0 || dev_type >= MAX_DEVICE_TYPE_NUM)
		return -EINVAL;

	val = dev->buffer[idx].value;

	if (dev_type == SPEN || dev_type == HOVER) {
		if (!dev->mt_event[dev_type] && val)
			return BOOSTER_ON;
		if (dev->mt_event[dev_type] && !val)
			return BOOSTER_OFF;
		return -EINVAL;
	}

	if (dev_type == TOUCH || dev_type == MULTI_TOUCH)
		return (val >= 0) ? BOOSTER_ON : BOOSTER_OFF;

	return (val > 0) ? BOOSTER_ON : BOOSTER_OFF;
}

/*
 * get_device_type : Identify the device type and boost action for an input packet.
 *
 * Scans the evdev_client ring buffer between *cur_idx and head to find
 * the trigger event. Once classified, returns BOOSTER_ON (1) or BOOSTER_OFF (0),
 * or negative errno if the packet contains no booster event.
 */
int get_device_type(struct evdev_client *dev, unsigned int *key_id,
		    int *cur_idx, int head)
{
	int mask;
	int i;
	int ret_val = -EINVAL;
	int dev_type = NONE_TYPE_DEVICE;
	int uniq_slot = 0;
	int target_idx = 0;

	if (unlikely(!dev))
		return -EINVAL;

	if (unlikely(dev->ev_cnt > MAX_EVENTS)) {
		*cur_idx = head;
		return -EINVAL;
	}

	mask = dev->bufsize - 1;
	i = *cur_idx;
	dev->device_type = NONE_TYPE_DEVICE;

	for (; i != head; i = (i + 1) & mask) {
		const struct input_event *ev = &dev->buffer[i];

		if (ev->type == EV_SYN || ev->code == SYN_REPORT)
			break;

		if (ev->type == EV_KEY) {
			target_idx = i;
			switch (ev->code) {
			case BTN_TOUCH:
				if (chk_next_data(dev, i, BTN_TOUCH))
					dev_type = SPEN;
				break;
			case BTN_TOOL_PEN:
				dev_type = HOVER;
				break;
			case KEY_BACK:
			case KEY_HOMEPAGE:
			case KEY_RECENT:
				dev_type = TOUCH_KEY;
				break;
			case KEY_VOLUMEUP:
			case KEY_VOLUMEDOWN:
			case KEY_POWER:
			case KEY_WINK:
				dev_type = KEY;
				break;
			default:
				break;
			}
		} else if (ev->type == EV_ABS && ev->code == ABS_MT_TRACKING_ID) {
			target_idx = i;
			if (ev->value >= 0) {
				dev->touch_slot_cnt++;
				if (dev->touch_slot_cnt == 1) {
					dev_type = TOUCH;
					uniq_slot = 1;
				} else if (dev->touch_slot_cnt == 2) {
					dev_type = MULTI_TOUCH;
					uniq_slot = 2;
				}
			} else {
				if (dev->touch_slot_cnt > 0)
					dev->touch_slot_cnt--;
				if (dev->touch_slot_cnt == 0) {
					dev_type = TOUCH;
					uniq_slot = 1;
				} else if (dev->touch_slot_cnt == 1) {
					dev_type = MULTI_TOUCH;
					uniq_slot = 2;
				}
			}
		} else if (ev->type == EV_MSC && ev->code == MSC_SCAN) {
			if (!chk_next_data(dev, i, EV_KEY))
				break;

			target_idx = (i + 1) & mask;
			uniq_slot = dev->buffer[target_idx].code;
			switch (uniq_slot) {
			case BTN_LEFT:
			case BTN_RIGHT:
			case BTN_MIDDLE:
				dev_type = MOUSE;
				break;
			default:
				dev_type = KEYBOARD;
				break;
			}
		}

		if (dev_type != NONE_TYPE_DEVICE) {
			*key_id = create_uniq_id(ev->type, ev->code, uniq_slot);
			ret_val = chk_boost_on_off(dev, target_idx, dev_type);
			break;
		}
	}

	*cur_idx = (i == head) ? head : ((i + 1) & mask);
	dev->device_type = dev_type;
	return ret_val;
}

/*
 * input_booster : Hot-path hook called on SYN_REPORT under ib_type_lock.
 *
 * Inspects recent events, determines if boost threshold is crossed,
 * claims a lockless trigger slot, and queues work onto ev_unbound_wq.
 * No memory allocations or sleeps are permitted in this path.
 */
void input_booster(struct evdev_client *dev, int dev_head)
{
	int mask, head, cur_idx;

	if (unlikely(!dev || !ib_init_succeed || dev->ev_cnt == 0))
		return;

	mask = dev->bufsize - 1;
	head = dev_head;
	cur_idx = (head - dev->ev_cnt) & mask;

	while (cur_idx != head) {
		unsigned int key_id = 0;
		int enable = get_device_type(dev, &key_id, &cur_idx, head);
		int dev_type = dev->device_type;
		int slot;

		if (enable < 0 || key_id == 0)
			continue;

		if (dev_type <= NONE_TYPE_DEVICE || dev_type >= MAX_DEVICE_TYPE_NUM)
			continue;

		if (enable == BOOSTER_ON)
			dev->mt_event[dev_type]++;
		else if (dev->mt_event[dev_type] > 0)
			dev->mt_event[dev_type]--;

		slot = ib_trigger_get_slot();
		if (unlikely(slot < 0))
			continue;

		if (dev->evdev && dev->evdev->handle.dev &&
		    dev->evdev->handle.dev->name) {
			strscpy(ib_trigger[slot].dev_name,
				dev->evdev->handle.dev->name,
				sizeof(ib_trigger[slot].dev_name));
		} else {
			ib_trigger[slot].dev_name[0] = '\0';
		}

		ib_trigger[slot].key_id = key_id;
		ib_trigger[slot].event_type = enable;
		ib_trigger[slot].dev_type = dev_type;

		if (!queue_work(ev_unbound_wq, &ib_trigger[slot].ib_trigger_work)) {
			/* Pairs with acquire in ib_trigger_get_slot */
			smp_store_release(&ib_trigger[slot].in_use, 0);
		}
	}
}

#endif /* CONFIG_SEC_INPUT_BOOSTER */
