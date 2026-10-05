// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msm_dsps.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <asm/arch_timer.h>

#ifdef CONFIG_SUPPORT_SSC_SPU
#include <linux/adsp/ssc_spu.h>
#endif

#include <soc/qcom/subsystem_restart.h>

#if defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_DUAL_6AXIS)
static bool pretest;
#endif

#define IMAGE_LOAD_CMD 1
#define IMAGE_UNLOAD_CMD 0
#define SSR_RESET_CMD 1
#define SET_PRETEST_SSR 2
#define CLR_PRETEST_SSR 3
#define SET_DHALL_SSR 4
#define CLR_DHALL_SSR 5
#define SSR_FORCE_RESET_CMD 9
#define CLASS_NAME	"ssc"
#define DRV_NAME	"sensors"
#define DRV_VERSION	"2.00"
#define QTICK_DIV_FACTOR	0x249F

#ifdef CONFIG_COMPAT
#define DSPS_IOCTL_READ_SLOW_TIMER32	_IOR(DSPS_IOCTL_MAGIC, 3, compat_uint_t)
#endif

struct sns_ssc_control {
	struct class *dev_class;
	dev_t dev_num;
	struct device *dev;
	struct cdev cdev;
};

static struct sns_ssc_control sns_ctl;

struct slpi_loader_private {
	struct mutex lock;
	void *pil_h;
	struct kobject *boot_slpi_obj;
};

static struct platform_device *slpi_private;
static struct work_struct slpi_ldr_work;
static unsigned int ssc_system_rev;

static ssize_t slpi_boot_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count);

static ssize_t slpi_ssr_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count);

static struct kobj_attribute slpi_boot_attribute =
	__ATTR(boot, 0220, NULL, slpi_boot_store);

static struct kobj_attribute slpi_ssr_attribute =
	__ATTR(ssr, 0220, NULL, slpi_ssr_store);

static struct attribute *attrs[] = {
	&slpi_boot_attribute.attr,
	&slpi_ssr_attribute.attr,
	NULL,
};

static const struct attribute_group slpi_attr_group = {
	.attrs = attrs,
};

#ifdef CONFIG_SUPPORT_SSC_SPU
static const char * const ver_info_path[SSC_CNT_MAX] = {
	SLPI_SPU_VER_INFO,
	SLPI_VER_INFO
};
static int ver_buf[SSC_CNT_MAX][SSC_VC_MAX];
static int fw_idx = SSC_ORI;

static bool slpi_need_update_spu(void)
{
	struct file *slpi_filp;
	mm_segment_t old_fs;
	int i, ret, vc_idx;
	char read_buf[FILE_LEN + 1];

	for (vc_idx = 0; vc_idx < SSC_CNT_MAX; vc_idx++) {
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		pr_info("idx:%d, path:%s\n", vc_idx, ver_info_path[vc_idx]);
		slpi_filp = filp_open(ver_info_path[vc_idx], O_RDONLY, 0440);
		if (IS_ERR(slpi_filp)) {
			ret = PTR_ERR(slpi_filp);
			pr_err("%s - Can't open :%s, %d\n",
			       __func__, ver_info_path[vc_idx], ret);
			set_fs(old_fs);
			return false;
		}

		memset(read_buf, 0, sizeof(read_buf));
		ret = vfs_read(slpi_filp, read_buf, FILE_LEN, &slpi_filp->f_pos);
		if (ret < 0) {
			pr_err("%s: fd read fail:%d\n", __func__, ret);
			filp_close(slpi_filp, current->files);
			set_fs(old_fs);
			return false;
		}

		filp_close(slpi_filp, current->files);
		set_fs(old_fs);

		read_buf[ret < sizeof(read_buf) ? ret : sizeof(read_buf) - 1] = '\0';
		ret = sscanf(read_buf, "%d,%4d-%2d-%2d %2d:%2d:%2d.%6d",
			     &ver_buf[vc_idx][SSC_CL],
			     &ver_buf[vc_idx][SSC_YEAR],
			     &ver_buf[vc_idx][SSC_MONTH],
			     &ver_buf[vc_idx][SSC_DATE],
			     &ver_buf[vc_idx][SSC_HOUR],
			     &ver_buf[vc_idx][SSC_MIN],
			     &ver_buf[vc_idx][SSC_SEC],
			     &ver_buf[vc_idx][SSC_MSEC]);

		pr_info("idx:%d, CL:%d, %d-%d-%d, %d:%d:%d.%d\n",
			vc_idx, ver_buf[vc_idx][SSC_CL],
			ver_buf[vc_idx][SSC_YEAR], ver_buf[vc_idx][SSC_MONTH],
			ver_buf[vc_idx][SSC_DATE], ver_buf[vc_idx][SSC_HOUR],
			ver_buf[vc_idx][SSC_MIN], ver_buf[vc_idx][SSC_SEC],
			ver_buf[vc_idx][SSC_MSEC]);
	}

	for (i = 0; i < SSC_VC_MAX; i++) {
		if (ver_buf[SSC_ORI][i] < ver_buf[SSC_SPU][i]) {
			pr_info("SLPI_SPU firmware laster, update!!, %d:%d,%d\n",
				i, ver_buf[SSC_ORI][i], ver_buf[SSC_SPU][i]);
			return true;
		} else if (ver_buf[SSC_ORI][i] > ver_buf[SSC_SPU][i]) {
			return false;
		}
	}

	return false;
}
#endif

unsigned int ssc_hw_rev(void)
{
	unsigned int rev = READ_ONCE(ssc_system_rev);

	pr_info("%s - rev = %u\n", __func__, rev);
	return rev;
}
EXPORT_SYMBOL(ssc_hw_rev);

static void slpi_load_fw(struct work_struct *work)
{
	struct platform_device *pdev = slpi_private;
	struct slpi_loader_private *priv = NULL;
	const char *firmware_name = NULL;
	u32 hw_rev = 0;
	int ret;

	if (!pdev) {
		pr_err("%s: Platform device null\n", __func__);
		goto fail;
	}

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev,
			"%s: Device tree information missing\n", __func__);
		goto fail;
	}

	ret = of_property_read_string(pdev->dev.of_node,
				      "qcom,firmware-name", &firmware_name);
	if (ret < 0) {
		pr_err("can't get fw name.\n");
		goto fail;
	}

	ret = of_property_read_u32(pdev->dev.of_node,
				   "qcom,ssc_hw_rev", &hw_rev);
	if (ret < 0) {
		pr_err("can't get ssc_hw_rev.\n");
	} else {
		WRITE_ONCE(ssc_system_rev, hw_rev);
		pr_info("%s - ssc_hw_rev = %u\n", __func__, hw_rev);
	}

	priv = platform_get_drvdata(pdev);
	if (!priv) {
		dev_err(&pdev->dev,
			"%s: Private data get failed\n", __func__);
		goto fail;
	}

	mutex_lock(&priv->lock);
	if (priv->pil_h && !IS_ERR(priv->pil_h)) {
		dev_dbg(&pdev->dev, "%s: SLPI image already loaded\n", __func__);
		mutex_unlock(&priv->lock);
		return;
	}

#ifdef CONFIG_SUPPORT_SSC_SPU
	if (slpi_need_update_spu()) {
		priv->pil_h = subsystem_get_with_fwname("slpi", "slpi_spu");
		if (IS_ERR(priv->pil_h)) {
			dev_err(&pdev->dev, "%s: pil get failed slpi_spu,\n",
				__func__);
			priv->pil_h = subsystem_get_with_fwname("slpi", firmware_name);
			if (IS_ERR(priv->pil_h)) {
				dev_err(&pdev->dev, "%s: pil get failed,\n",
					__func__);
				priv->pil_h = NULL;
				mutex_unlock(&priv->lock);
				goto fail;
			}
			fw_idx = SSC_ORI_AF_SPU_FAIL;
		} else {
			fw_idx = SSC_SPU;
		}
	} else
#endif
	{
		priv->pil_h = subsystem_get_with_fwname("slpi", firmware_name);
		if (IS_ERR(priv->pil_h)) {
			dev_err(&pdev->dev, "%s: pil get failed,\n",
				__func__);
			priv->pil_h = NULL;
			mutex_unlock(&priv->lock);
			goto fail;
		}
	}

	mutex_unlock(&priv->lock);
	dev_dbg(&pdev->dev, "%s: SLPI image is loaded\n", __func__);
	return;

fail:
	pr_err("%s: SLPI image loading failed\n", __func__);
}

static void slpi_loader_do(struct platform_device *pdev)
{
	if (!pdev) {
		pr_err("%s: Platform device null\n", __func__);
		return;
	}
	dev_dbg(&pdev->dev, "%s: scheduling work to load SLPI fw\n", __func__);
	schedule_work(&slpi_ldr_work);
}

static void slpi_loader_unload(struct platform_device *pdev)
{
	struct slpi_loader_private *priv;

	if (!pdev)
		return;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return;

	cancel_work_sync(&slpi_ldr_work);

	mutex_lock(&priv->lock);
	if (priv->pil_h && !IS_ERR(priv->pil_h)) {
		dev_dbg(&pdev->dev, "%s: calling subsystem put\n", __func__);
		subsystem_put(priv->pil_h);
		priv->pil_h = NULL;
	}
	mutex_unlock(&priv->lock);
}

static ssize_t slpi_ssr_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf,
			      size_t count)
{
	int ssr_cmd = 0;
	struct subsys_device *sns_dev;
	struct platform_device *pdev = slpi_private;
	struct slpi_loader_private *priv;

	pr_debug("%s: going to call slpi_ssr\n", __func__);

	if (kstrtoint(buf, 10, &ssr_cmd) < 0)
		return -EINVAL;

#if defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_DUAL_6AXIS)
	if (ssr_cmd == SET_PRETEST_SSR) {
		pr_info("[FACTORY] Set Pretest SSR. slpi will be restarted!!\n");
		WRITE_ONCE(pretest, true);
	} else if (ssr_cmd == CLR_PRETEST_SSR) {
		pr_info("[FACTORY] Clear Pretest SSR. return without slpi ssr!!\n");
		WRITE_ONCE(pretest, false);
		return count;
	} else if (ssr_cmd == SET_DHALL_SSR) {
		pr_info("[FACTORY] SET_DHALL_SSR. slpi will be restarted!!\n");
		WRITE_ONCE(pretest, true);
	} else if (ssr_cmd == CLR_DHALL_SSR) {
		pr_info("[FACTORY] CLR_DHALL_SSR. return without slpi ssr!!\n");
		WRITE_ONCE(pretest, false);
		return count;
	} else if (ssr_cmd != SSR_RESET_CMD) {
		return -EINVAL;
	}
#else
	if (ssr_cmd != SSR_RESET_CMD && ssr_cmd != SSR_FORCE_RESET_CMD)
		return -EINVAL;
#endif

	if (!pdev)
		return -ENODEV;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return -EINVAL;

	mutex_lock(&priv->lock);
	if (!priv->pil_h || IS_ERR(priv->pil_h)) {
		mutex_unlock(&priv->lock);
		return -EINVAL;
	}

	sns_dev = (struct subsys_device *)priv->pil_h;
	dev_err(&pdev->dev, "Something went wrong with SLPI, restarting\n");
	if (ssr_cmd == SSR_FORCE_RESET_CMD) {
		pr_info("Run Force SSR for SS\n");
		subsys_set_fssr(sns_dev, true);
	}
	/* subsystem_restart_dev has worker queue to handle */
	if (subsystem_restart_dev(sns_dev) != 0) {
		dev_err(&pdev->dev, "subsystem_restart_dev failed\n");
		mutex_unlock(&priv->lock);
		return -EINVAL;
	}

	mutex_unlock(&priv->lock);
	dev_dbg(&pdev->dev, "SLPI restarted\n");
	return count;
}

#if defined(CONFIG_SEC_FACTORY) && defined(CONFIG_SUPPORT_DUAL_6AXIS)
bool is_pretest(void)
{
	return READ_ONCE(pretest);
}
EXPORT_SYMBOL(is_pretest);
#endif

#ifdef CONFIG_DSP_SLEEP_RECOVERY
int slpi_ssr(void)
{
	struct subsys_device *slpi_dev;
	struct platform_device *pdev = slpi_private;
	struct slpi_loader_private *priv;
	int rc;

	if (!pdev)
		return -ENODEV;

	dev_dbg(&pdev->dev, "%s: going to call slpi ssr\n", __func__);

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return -EINVAL;

	mutex_lock(&priv->lock);
	if (!priv->pil_h || IS_ERR(priv->pil_h)) {
		mutex_unlock(&priv->lock);
		return -EINVAL;
	}

	slpi_dev = (struct subsys_device *)priv->pil_h;
	dev_info(&pdev->dev, "requesting for slpi restart\n");

#ifndef CONFIG_SEC_SLPI_SLEEP_DEBUG
	dev_info(&pdev->dev, "Set force slpi ssr regardless of debug level\n");
	subsys_set_fssr(slpi_dev, true);
#endif
	/* subsystem_restart_dev has worker queue to handle */
	rc = subsystem_restart_dev(slpi_dev);
	if (rc) {
		dev_err(&pdev->dev, "subsystem_restart_dev failed\n");
		mutex_unlock(&priv->lock);
		return rc;
	}

	mutex_unlock(&priv->lock);
	dev_info(&pdev->dev, "slpi restarted by intention\n");
	return 0;
}
EXPORT_SYMBOL(slpi_ssr);
#endif

static ssize_t slpi_boot_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf,
			       size_t count)
{
	int boot = 0;

	if (sscanf(buf, "%du", &boot) != 1)
		return -EINVAL;

	if (boot == IMAGE_LOAD_CMD) {
		pr_debug("%s: going to call slpi_loader_do\n", __func__);
		slpi_loader_do(slpi_private);
	} else if (boot == IMAGE_UNLOAD_CMD) {
		pr_debug("%s: going to call slpi_unloader\n", __func__);
		slpi_loader_unload(slpi_private);
	}
	return count;
}

static int slpi_loader_init_sysfs(struct platform_device *pdev)
{
	struct slpi_loader_private *priv;
	int ret;

	slpi_private = NULL;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	priv->boot_slpi_obj = kobject_create_and_add("boot_slpi", kernel_kobj);
	if (!priv->boot_slpi_obj) {
		dev_err(&pdev->dev, "%s: sysfs create and add failed\n",
			__func__);
		return -ENOMEM;
	}

	ret = sysfs_create_group(priv->boot_slpi_obj, &slpi_attr_group);
	if (ret) {
		dev_err(&pdev->dev, "%s: sysfs create group failed %d\n",
			__func__, ret);
		kobject_del(priv->boot_slpi_obj);
		kobject_put(priv->boot_slpi_obj);
		priv->boot_slpi_obj = NULL;
		return ret;
	}

	slpi_private = pdev;
	return 0;
}

static void slpi_loader_remove(struct platform_device *pdev)
{
	struct slpi_loader_private *priv;

	if (!pdev)
		return;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return;

	cancel_work_sync(&slpi_ldr_work);

	mutex_lock(&priv->lock);
	if (priv->pil_h && !IS_ERR(priv->pil_h)) {
		subsystem_put(priv->pil_h);
		priv->pil_h = NULL;
	}
	mutex_unlock(&priv->lock);

	if (priv->boot_slpi_obj) {
		sysfs_remove_group(priv->boot_slpi_obj, &slpi_attr_group);
		kobject_del(priv->boot_slpi_obj);
		kobject_put(priv->boot_slpi_obj);
		priv->boot_slpi_obj = NULL;
	}

	if (slpi_private == pdev)
		slpi_private = NULL;
}

/*
 * Read virtual QTimer clock ticks and scale down to 32KHz clock as used
 * in DSPS
 */
static u32 sns_read_qtimer(void)
{
	u64 val;

	val = arch_counter_get_cntvct();
	/*
	 * To convert ticks from 19.2 Mhz clock to 32768 Hz clock:
	 * x = (value * 32768) / 19200000
	 * This is same as first left shift the value by 4 bits, i.e. multiply
	 * by 16, and then divide by 0x249F. The latter is preferable since
	 * QTimer tick (value) is 56-bit, so (value * 32768) could overflow,
	 * while (value * 16) will never do
	 */
	val <<= 4;
	do_div(val, QTICK_DIV_FACTOR);

	return (u32)val;
}

static int sensors_ssc_open(struct inode *ip, struct file *fp)
{
	return 0;
}

static int sensors_ssc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long sensors_ssc_ioctl(struct file *file,
			      unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	u32 val = 0;

	switch (cmd) {
	case DSPS_IOCTL_READ_SLOW_TIMER:
#ifdef CONFIG_COMPAT
	case DSPS_IOCTL_READ_SLOW_TIMER32:
#endif
		val = sns_read_qtimer();
		ret = put_user(val, (u32 __user *)arg);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct file_operations sensors_ssc_fops = {
	.owner = THIS_MODULE,
	.open = sensors_ssc_open,
	.release = sensors_ssc_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = sensors_ssc_ioctl,
#endif
	.unlocked_ioctl = sensors_ssc_ioctl,
};

static int sensors_ssc_probe(struct platform_device *pdev)
{
	int ret;

	ret = slpi_loader_init_sysfs(pdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: Error in initing sysfs\n", __func__);
		return ret;
	}

	INIT_WORK(&slpi_ldr_work, slpi_load_fw);

	sns_ctl.dev_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(sns_ctl.dev_class)) {
		ret = PTR_ERR(sns_ctl.dev_class);
		pr_err("%s: class_create fail.\n", __func__);
		goto err_remove_loader;
	}

	ret = alloc_chrdev_region(&sns_ctl.dev_num, 0, 1, DRV_NAME);
	if (ret) {
		pr_err("%s: alloc_chrdev_region fail.\n", __func__);
		goto err_destroy_class;
	}

	sns_ctl.dev = device_create(sns_ctl.dev_class, NULL,
				    sns_ctl.dev_num, &sns_ctl, DRV_NAME);
	if (IS_ERR(sns_ctl.dev)) {
		ret = PTR_ERR(sns_ctl.dev);
		pr_err("%s: device_create fail.\n", __func__);
		goto err_unregister_chrdev;
	}

	cdev_init(&sns_ctl.cdev, &sensors_ssc_fops);
	sns_ctl.cdev.owner = THIS_MODULE;

	ret = cdev_add(&sns_ctl.cdev, sns_ctl.dev_num, 1);
	if (ret) {
		pr_err("%s: cdev_add fail.\n", __func__);
		goto err_destroy_device;
	}

	return 0;

err_destroy_device:
	device_destroy(sns_ctl.dev_class, sns_ctl.dev_num);
err_unregister_chrdev:
	unregister_chrdev_region(sns_ctl.dev_num, 1);
err_destroy_class:
	class_destroy(sns_ctl.dev_class);
err_remove_loader:
	slpi_loader_remove(pdev);
	return ret;
}

static int sensors_ssc_remove(struct platform_device *pdev)
{
	cancel_work_sync(&slpi_ldr_work);
	cdev_del(&sns_ctl.cdev);
	device_destroy(sns_ctl.dev_class, sns_ctl.dev_num);
	unregister_chrdev_region(sns_ctl.dev_num, 1);
	class_destroy(sns_ctl.dev_class);
	slpi_loader_remove(pdev);

	return 0;
}

static const struct of_device_id msm_ssc_sensors_dt_match[] = {
	{ .compatible = "qcom,msm-ssc-sensors" },
	{},
};
MODULE_DEVICE_TABLE(of, msm_ssc_sensors_dt_match);

static struct platform_driver sensors_ssc_driver = {
	.driver = {
		.name = "sensors-ssc",
		.of_match_table = msm_ssc_sensors_dt_match,
	},
	.probe = sensors_ssc_probe,
	.remove = sensors_ssc_remove,
};

static int __init sensors_ssc_init(void)
{
	int rc;

	pr_debug("%s driver version %s.\n", DRV_NAME, DRV_VERSION);
	rc = platform_driver_register(&sensors_ssc_driver);
	if (rc) {
		pr_err("%s: Failed to register sensors ssc driver\n",
		       __func__);
		return rc;
	}

	return 0;
}

static void __exit sensors_ssc_exit(void)
{
	platform_driver_unregister(&sensors_ssc_driver);
}

module_init(sensors_ssc_init);
module_exit(sensors_ssc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Sensors SSC driver");
