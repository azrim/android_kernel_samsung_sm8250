// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Samsung Electronics. All rights reserved.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/sensor/sensors_core.h>

struct class *sensors_class;
struct class *sensors_event_class;
static struct device *symlink_dev;
static struct device *sensor_dev;
static struct input_dev *meta_input_dev;

static atomic_t sensor_count;
static DEFINE_MUTEX(sensors_core_lock);

int sensors_create_symlink(struct kobject *target, const char *name)
{
	int err;

	if (!target || !name)
		return -EINVAL;

	if (!symlink_dev) {
		pr_err("%s, symlink_dev is NULL!!!\n", __func__);
		return -ENODEV;
	}

	err = sysfs_create_link(&symlink_dev->kobj, target, name);
	if (err < 0) {
		pr_err("%s, %s failed!(%d)\n", __func__, name, err);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL(sensors_create_symlink);

void sensors_remove_symlink(struct kobject *target, const char *name)
{
	if (!symlink_dev)
		pr_err("%s, symlink_dev is NULL!!!\n", __func__);
	else if (target && name)
		sysfs_delete_link(&symlink_dev->kobj, target, name);
}
EXPORT_SYMBOL(sensors_remove_symlink);

static ssize_t set_flush(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t size)
{
	long val;
	u8 sensor_type;

	if (kstrtol(buf, 10, &val) < 0 || val < 0 || val > U8_MAX)
		return -EINVAL;

	sensor_type = (u8)val;

	mutex_lock(&sensors_core_lock);
	if (!meta_input_dev) {
		mutex_unlock(&sensors_core_lock);
		pr_err("[SENSOR CORE] meta_input_dev is NULL\n");
		return -ENODEV;
	}

	input_report_rel(meta_input_dev, REL_DIAL, 1);
	input_report_rel(meta_input_dev, REL_HWHEEL, sensor_type + 1);
	input_sync(meta_input_dev);
	mutex_unlock(&sensors_core_lock);

	pr_info("[SENSOR] flush %u\n", sensor_type);
	return size;
}

static DEVICE_ATTR(flush, 0220, NULL, set_flush);

static struct device_attribute *sensor_attr[] = {
	&dev_attr_flush,
	NULL,
};

int sensors_register(struct device **dev, void *drvdata,
		     struct device_attribute *attributes[], char *name)
{
	int ret = 0;
	int i;

	if (!dev || !name)
		return -EINVAL;

	mutex_lock(&sensors_core_lock);

	if (!sensors_class) {
		sensors_class = class_create(THIS_MODULE, "sensors");
		if (IS_ERR(sensors_class)) {
			ret = PTR_ERR(sensors_class);
			sensors_class = NULL;
			mutex_unlock(&sensors_core_lock);
			return ret;
		}
	}

	*dev = device_create(sensors_class, NULL, 0, drvdata, "%s", name);
	if (IS_ERR(*dev)) {
		ret = PTR_ERR(*dev);
		pr_err("[SENSORS CORE] device_create failed![%d]\n", ret);
		mutex_unlock(&sensors_core_lock);
		return ret;
	}

	if (attributes) {
		for (i = 0; attributes[i] != NULL; i++) {
			if (device_create_file(*dev, attributes[i]) < 0)
				pr_err("[SENSOR CORE] fail device_create_file(dev, attributes[%d])\n",
				       i);
		}
	}

	atomic_inc(&sensor_count);
	mutex_unlock(&sensors_core_lock);

	return 0;
}
EXPORT_SYMBOL(sensors_register);

void sensors_unregister(struct device *dev,
			struct device_attribute *attributes[])
{
	int i;

	if (!dev || !attributes)
		return;

	for (i = 0; attributes[i] != NULL; i++)
		device_remove_file(dev, attributes[i]);

	if (atomic_read(&sensor_count) > 0)
		atomic_dec(&sensor_count);
}
EXPORT_SYMBOL(sensors_unregister);

void destroy_sensor_class(void)
{
	mutex_lock(&sensors_core_lock);

	if (sensor_dev) {
		device_destroy(sensors_class, sensor_dev->devt);
		sensor_dev = NULL;
	}

	if (sensors_class) {
		class_destroy(sensors_class);
		sensors_class = NULL;
	}

	if (symlink_dev) {
		device_destroy(sensors_event_class, symlink_dev->devt);
		symlink_dev = NULL;
	}

	if (sensors_event_class) {
		class_destroy(sensors_event_class);
		sensors_event_class = NULL;
	}

	mutex_unlock(&sensors_core_lock);
}
EXPORT_SYMBOL(destroy_sensor_class);

int sensors_input_init(void)
{
	int ret;

	mutex_lock(&sensors_core_lock);

	if (meta_input_dev) {
		mutex_unlock(&sensors_core_lock);
		return 0;
	}

	meta_input_dev = input_allocate_device();
	if (!meta_input_dev) {
		mutex_unlock(&sensors_core_lock);
		pr_err("[SENSOR CORE] failed alloc meta dev\n");
		return -ENOMEM;
	}

	meta_input_dev->name = "meta_event";
	input_set_capability(meta_input_dev, EV_REL, REL_HWHEEL);
	input_set_capability(meta_input_dev, EV_REL, REL_DIAL);

	ret = input_register_device(meta_input_dev);
	if (ret < 0) {
		pr_err("[SENSOR CORE] failed register meta dev\n");
		input_free_device(meta_input_dev);
		meta_input_dev = NULL;
		mutex_unlock(&sensors_core_lock);
		return ret;
	}

	ret = sensors_create_symlink(&meta_input_dev->dev.kobj,
				     meta_input_dev->name);
	if (ret < 0) {
		pr_err("[SENSOR CORE] failed create meta symlink\n");
		input_unregister_device(meta_input_dev);
		meta_input_dev = NULL;
		mutex_unlock(&sensors_core_lock);
		return ret;
	}

	mutex_unlock(&sensors_core_lock);
	return 0;
}

void sensors_input_clean(void)
{
	mutex_lock(&sensors_core_lock);

	if (meta_input_dev) {
		sensors_remove_symlink(&meta_input_dev->dev.kobj,
				       meta_input_dev->name);
		input_unregister_device(meta_input_dev);
		meta_input_dev = NULL;
	}

	mutex_unlock(&sensors_core_lock);
}

static int __init sensors_class_init(void)
{
	int ret;

	pr_info("[SENSORS CORE] %s\n", __func__);

	sensors_class = class_create(THIS_MODULE, "sensors");
	if (IS_ERR(sensors_class)) {
		ret = PTR_ERR(sensors_class);
		pr_err("%s, create sensors_class is failed.(err=%d)\n",
		       __func__, ret);
		sensors_class = NULL;
		return ret;
	}

	sensor_dev = device_create(sensors_class, NULL, 0, NULL, "sensor_dev");
	if (IS_ERR(sensor_dev)) {
		ret = PTR_ERR(sensor_dev);
		pr_err("[SENSORS CORE] sensor_dev create failed![%d]\n", ret);
		sensor_dev = NULL;
		goto err_destroy_sensors_class;
	}

	ret = device_create_file(sensor_dev, *sensor_attr);
	if (ret < 0)
		pr_err("[SENSOR CORE] failed flush device_file\n");

	sensors_event_class = class_create(THIS_MODULE, "sensor_event");
	if (IS_ERR(sensors_event_class)) {
		ret = PTR_ERR(sensors_event_class);
		pr_err("%s, create sensors_event_class is failed.(err=%d)\n",
		       __func__, ret);
		sensors_event_class = NULL;
		goto err_destroy_sensor_dev;
	}

	symlink_dev = device_create(sensors_event_class, NULL, 0, NULL, "symlink");
	if (IS_ERR(symlink_dev)) {
		ret = PTR_ERR(symlink_dev);
		pr_err("[SENSORS CORE] symlink_dev create failed![%d]\n", ret);
		symlink_dev = NULL;
		goto err_destroy_sensors_event_class;
	}

	atomic_set(&sensor_count, 0);
	sensors_class->dev_uevent = NULL;

	ret = sensors_input_init();
	if (ret < 0)
		pr_err("[SENSORS CORE] sensors_input_init failed: %d\n", ret);

	return 0;

err_destroy_sensors_event_class:
	class_destroy(sensors_event_class);
	sensors_event_class = NULL;
err_destroy_sensor_dev:
	device_destroy(sensors_class, sensor_dev->devt);
	sensor_dev = NULL;
err_destroy_sensors_class:
	class_destroy(sensors_class);
	sensors_class = NULL;
	return ret;
}

static void __exit sensors_class_exit(void)
{
	sensors_input_clean();
	destroy_sensor_class();
}

subsys_initcall(sensors_class_init);
module_exit(sensors_class_exit);

MODULE_DESCRIPTION("Universal sensors core class");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
