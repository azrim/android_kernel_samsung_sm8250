// SPDX-License-Identifier: GPL-2.0
/*
 * haptic motor driver for max77705_haptic.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "[VIB] " fmt

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define MOTOR_LRA			BIT(7)
#define MOTOR_EN			BIT(6)
#define EXT_PWM				0
#define DIVIDER_128			BIT(1)
#define MRDBTMER_MASK			0x7
#define MREN				BIT(3)
#define BIASEN				BIT(7)

struct max77705_haptic_drvdata {
	struct max77705_dev *max77705;
	struct i2c_client *i2c;
	struct max77705_haptic_pdata *pdata;
	struct delayed_work haptic_work;
	bool running;
	bool uvlo_pending;
};

static DEFINE_MUTEX(max77705_haptic_lock);
static struct max77705_haptic_drvdata *max77705_g_hap_data;

static int max77705_haptic_i2c(struct max77705_haptic_drvdata *drvdata, bool en)
{
	if (!drvdata || !drvdata->i2c || !max77705_g_hap_data) {
		pr_info("[VIB]%s null reference\n", __func__);
		return -ENODEV;
	}

	return max77705_update_reg(drvdata->i2c,
				   MAX77705_PMIC_REG_MCONFIG,
				   en ? 0xff : 0x0, MOTOR_LRA | MOTOR_EN);
}

#if defined(CONFIG_SS_VIBRATOR)
void max77705_vibtonz_en(bool en)
{
	struct max77705_haptic_drvdata *drvdata;

	mutex_lock(&max77705_haptic_lock);
	drvdata = max77705_g_hap_data;
	if (!drvdata) {
		mutex_unlock(&max77705_haptic_lock);
		return;
	}

	if (en) {
		if (drvdata->running) {
			mutex_unlock(&max77705_haptic_lock);
			return;
		}
		max77705_haptic_i2c(drvdata, true);
		drvdata->running = true;
	} else {
		if (!drvdata->running) {
			mutex_unlock(&max77705_haptic_lock);
			return;
		}
		max77705_haptic_i2c(drvdata, false);
		drvdata->running = false;
	}
	mutex_unlock(&max77705_haptic_lock);
}
EXPORT_SYMBOL(max77705_vibtonz_en);
#endif

static void max77705_haptic_init_reg(struct max77705_haptic_drvdata *drvdata)
{
	int ret;

	ret = max77705_update_reg(drvdata->i2c,
				  MAX77705_PMIC_REG_MAINCTRL1, 0xff, BIASEN);
	if (ret)
		pr_err("i2c REG_BIASEN update error %d\n", ret);

	ret = max77705_update_reg(drvdata->i2c,
				  MAX77705_PMIC_REG_MCONFIG, 0xff, MOTOR_LRA);
	if (ret)
		pr_err("i2c MOTOR_LRA update error %d\n", ret);

	ret = max77705_update_reg(drvdata->i2c,
				  MAX77705_PMIC_REG_MCONFIG, 0xff, DIVIDER_128);
	if (ret)
		pr_err("i2c DIVIDER_128 update error %d\n", ret);
}

static void uvlo_haptic_init_reg(struct work_struct *work)
{
	struct max77705_haptic_drvdata *drvdata =
		container_of(work, struct max77705_haptic_drvdata, haptic_work.work);
	u8 reg_data;

	drvdata->uvlo_pending = false;

	max77705_read_reg(drvdata->i2c,
			  MAX77705_PMIC_REG_MCONFIG, &reg_data);
	pr_debug("[VIB],before haptic reg init, data = %02x\n", reg_data);

	max77705_haptic_init_reg(drvdata);
#if defined(CONFIG_SS_VIBRATOR)
	max77705_vibtonz_en(true);
#endif

	max77705_read_reg(drvdata->i2c,
			  MAX77705_PMIC_REG_MCONFIG, &reg_data);
	pr_debug("[VIB],after haptic reg init, data = %02x\n", reg_data);
}

static irqreturn_t max77705_haptic_irq(int irq, void *data)
{
	struct max77705_haptic_drvdata *drvdata = data;

	pr_debug("%s: [VIB] UVLO INT occurred, init haptic reg\n", __func__);

	schedule_delayed_work(&drvdata->haptic_work, msecs_to_jiffies(1000));

	return IRQ_HANDLED;
}

#if defined(CONFIG_OF)
static struct max77705_haptic_pdata *of_max77705_haptic_dt(struct device *dev)
{
	struct device_node *np;
	struct max77705_haptic_pdata *pdata;
	u32 val;

	if (!dev->parent || !dev->parent->of_node)
		return NULL;

	np = dev->parent->of_node;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (of_property_read_u32(np, "haptic,mode", &val)) {
		pr_info("[VIB] %s: mode reference fail\n", __func__);
		pdata->mode = 1;
	} else {
		pdata->mode = (int)val;
	}

	if (of_property_read_u32(np, "haptic,divisor", &val)) {
		pr_info("[VIB] %s: divisor reference fail\n", __func__);
		pdata->divisor = 128;
	} else {
		pdata->divisor = (int)val;
	}

	pr_info("[VIB] %s: mode: %d\n", __func__, pdata->mode);
	pr_info("[VIB] %s: divisor: %d\n", __func__, pdata->divisor);

	return pdata;
}
#endif

static int max77705_haptic_probe(struct platform_device *pdev)
{
	struct max77705_dev *max77705;
	struct max77705_platform_data *max77705_pdata = NULL;
	struct max77705_haptic_pdata *pdata = NULL;
	struct max77705_haptic_drvdata *drvdata;
	int irq_base;
	int error;
	u8 reg_data;

	if (!pdev->dev.parent) {
		pr_err("%s: no parent device\n", __func__);
		return -ENODEV;
	}

	max77705 = dev_get_drvdata(pdev->dev.parent);
	if (!max77705) {
		pr_err("%s: parent max77705 drvdata not found\n", __func__);
		return -ENODEV;
	}

	if (max77705->dev)
		max77705_pdata = dev_get_platdata(max77705->dev);
	if (!max77705_pdata)
		max77705_pdata = max77705->pdata;

	if (max77705_pdata)
		pdata = max77705_pdata->haptic_data;

#if defined(CONFIG_OF)
	if (!pdata) {
		pdata = of_max77705_haptic_dt(&pdev->dev);
		if (unlikely(!pdata)) {
			pr_err("max77705-haptic : %s not found haptic dt!\n",
			       __func__);
			return -ENODEV;
		}
	}
#else
	if (unlikely(!pdata)) {
		pr_err("%s: no pdata\n", __func__);
		return -ENODEV;
	}
#endif /* CONFIG_OF */

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (unlikely(!drvdata))
		return -ENOMEM;

	platform_set_drvdata(pdev, drvdata);
	drvdata->max77705 = max77705;
	drvdata->i2c = max77705->i2c;
	drvdata->pdata = pdata;
	drvdata->running = false;
	drvdata->uvlo_pending = false;

	mutex_lock(&max77705_haptic_lock);
	max77705_g_hap_data = drvdata;
	mutex_unlock(&max77705_haptic_lock);

	INIT_DELAYED_WORK(&drvdata->haptic_work, uvlo_haptic_init_reg);

	max77705_haptic_init_reg(drvdata);
	max77705_read_reg(drvdata->i2c,
			  MAX77705_PMIC_REG_MCONFIG, &reg_data);

	irq_base = max77705_pdata ? max77705_pdata->irq_base : max77705->irq_base;
	pdata->irq = irq_base + MAX77705_SYSTEM_IRQ_SYSUVLO_INT;
	error = request_threaded_irq(pdata->irq, NULL, max77705_haptic_irq,
				     IRQF_NO_SUSPEND, "max77705_UVLO", drvdata);
	if (error < 0) {
		pr_err("%s: Failed to request IRQ #%d: %d\n",
		       __func__, pdata->irq, error);
		pdata->irq = 0;
	}

	return 0;
}

static int max77705_haptic_remove(struct platform_device *pdev)
{
	struct max77705_haptic_drvdata *drvdata = platform_get_drvdata(pdev);

	if (!drvdata)
		return 0;

	/*
	 * Quiesce the IRQ and the delayed work before freeing drvdata: the
	 * UVLO handler re-arms haptic_work, and uvlo_haptic_init_reg()
	 * dereferences drvdata, so freeing first would leave both running
	 * against freed memory.
	 */
	if (drvdata->pdata && drvdata->pdata->irq)
		free_irq(drvdata->pdata->irq, drvdata);
	cancel_delayed_work_sync(&drvdata->haptic_work);

	mutex_lock(&max77705_haptic_lock);
	max77705_haptic_i2c(drvdata, false);
	drvdata->running = false;
	max77705_g_hap_data = NULL;
	mutex_unlock(&max77705_haptic_lock);

	kfree(drvdata);
	return 0;
}

static int max77705_haptic_suspend(struct platform_device *pdev,
				   pm_message_t state)
{
	struct max77705_haptic_drvdata *drvdata = platform_get_drvdata(pdev);

	if (drvdata) {
		if (cancel_delayed_work_sync(&drvdata->haptic_work))
			drvdata->uvlo_pending = true;
	}

	return 0;
}

static int max77705_haptic_resume(struct platform_device *pdev)
{
	struct max77705_haptic_drvdata *drvdata = platform_get_drvdata(pdev);

	if (drvdata && drvdata->uvlo_pending) {
		drvdata->uvlo_pending = false;
		schedule_delayed_work(&drvdata->haptic_work,
				      msecs_to_jiffies(1000));
	}

	return 0;
}

static void max77705_haptic_shutdown(struct platform_device *pdev)
{
	struct max77705_haptic_drvdata *drvdata = platform_get_drvdata(pdev);

	pr_info("[VIB] %s : Disable Haptic\n", __func__);
	if (!drvdata)
		return;

	cancel_delayed_work_sync(&drvdata->haptic_work);

	mutex_lock(&max77705_haptic_lock);
	max77705_haptic_i2c(drvdata, false);
	drvdata->running = false;
	mutex_unlock(&max77705_haptic_lock);
}

static struct platform_driver max77705_haptic_driver = {
	.probe		= max77705_haptic_probe,
	.remove		= max77705_haptic_remove,
	.suspend	= max77705_haptic_suspend,
	.resume		= max77705_haptic_resume,
	.shutdown	= max77705_haptic_shutdown,
	.driver = {
		.name	= "max77705-haptic",
		.owner	= THIS_MODULE,
	},
};

static int __init max77705_haptic_init(void)
{
	return platform_driver_register(&max77705_haptic_driver);
}
module_init(max77705_haptic_init);

static void __exit max77705_haptic_exit(void)
{
	platform_driver_unregister(&max77705_haptic_driver);
}
module_exit(max77705_haptic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("max77705 haptic driver");
