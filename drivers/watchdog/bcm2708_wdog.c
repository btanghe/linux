/*
 * Broadcom BCM2708 watchdog driver.
 *
 * (c) Copyright 2010 Broadcom Europe Ltd
 * Copyright 2012 Simon Arlott
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * BCM2708 watchdog driver. Loosely based on wdt driver.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include "bcm2708_wdog.h"

struct bcm2708_wdog {
	struct list_head list;

	struct device *dev;
	struct resource res;
	void __iomem *pm;
	struct mutex lock;
	bool started;
	bool blocked;

	struct watchdog_info info;
};

#define MODULE_NAME "bcm2708_wdog"
#define SECS_TO_WDOG_TICKS(x) ((x) << 16)
#define WDOG_TICKS_TO_SECS(x) ((x) >> 16)

#define PM_RSTC				0x1c
#define PM_WDOG				0x24
/* Device resource must cover both registers */
#define PM_MINSZ			0x28

#define PM_PASSWORD			0x5a000000
#define PM_WDOG_RESET			0x00000000
#define PM_WDOG_TIME_SET		0x00100000
#define PM_RSTC_WRCFG_CLR		0xffffffcf
#define PM_RSTC_WRCFG_SET		0x00000030
#define PM_RSTC_WRCFG_FULL_RESET	0x00000020
#define PM_RSTC_RESET			0x00000102

/* Maximum timeout really is 16 seconds */
#define WD_MINT 1
#define WD_MAXT WDOG_TICKS_TO_SECS(PM_WDOG_TIME_SET)
#define WD_DEFT WD_MAXT

static unsigned int timeout = WD_DEFT;
module_param(timeout, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(timeout,
	"Default watchdog timeout in seconds. (0 < timeout <= "
		_MODULE_STRING(WD_MAXT) ", default="
		__MODULE_STRING(WD_DEFT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

LIST_HEAD(wdogs);
DEFINE_MUTEX(lock);

static void bcm2708_wdog_lock(struct bcm2708_wdog *wdog)
{
	mutex_lock(&wdog->lock);
}

static void bcm2708_wdog_unlock(struct bcm2708_wdog *wdog)
{
	mutex_unlock(&wdog->lock);
}

static int bcm2708_wdog_start(struct watchdog_device *dev)
{
	struct bcm2708_wdog *wdog = watchdog_get_drvdata(dev);
	u32 cur;

	bcm2708_wdog_lock(wdog);
	if (wdog->blocked) {
		bcm2708_wdog_unlock(wdog);
		return -EBUSY;
	}

	writel_relaxed((SECS_TO_WDOG_TICKS(dev->timeout) & PM_WDOG_TIME_SET)
		| PM_PASSWORD, wdog->pm + PM_WDOG);
	cur = readl_relaxed(wdog->pm + PM_RSTC);
	writel_relaxed(PM_PASSWORD | (cur & PM_RSTC_WRCFG_CLR)
		| PM_RSTC_WRCFG_FULL_RESET, wdog->pm + PM_RSTC);

	if (!wdog->started) {
		dev_info(wdog->dev, "watchdog started\n");
		wdog->started = true;
	}

	bcm2708_wdog_unlock(wdog);
	return 0;
}

static int bcm2708_wdog_stop(struct watchdog_device *dev)
{
	struct bcm2708_wdog *wdog = watchdog_get_drvdata(dev);

	bcm2708_wdog_lock(wdog);
	if (wdog->blocked) {
		bcm2708_wdog_unlock(wdog);
		return -EBUSY;
	}

	writel_relaxed(PM_PASSWORD | PM_RSTC_RESET, wdog->pm + PM_RSTC);

	if (wdog->started) {
		dev_info(wdog->dev, "watchdog stopped\n");
		wdog->started = false;
	}

	bcm2708_wdog_unlock(wdog);
	return 0;
}

static unsigned int bcm2708_wdog_get_timeleft(struct watchdog_device *dev)
{
	struct bcm2708_wdog *wdog = watchdog_get_drvdata(dev);
	unsigned int remaining;

	bcm2708_wdog_lock(wdog);
	if (wdog->blocked) {
		bcm2708_wdog_unlock(wdog);
		return -EBUSY;
	}

	remaining = WDOG_TICKS_TO_SECS(readl_relaxed(wdog->pm + PM_WDOG) & PM_WDOG_TIME_SET);

	bcm2708_wdog_unlock(wdog);
	return remaining;
}

void bcm2708_wdog_restart(char str, const char *cmd)
{
	struct bcm2708_wdog *wdog;
	u32 cur;

	list_for_each_entry(wdog, &wdogs, list) {
		bcm2708_wdog_lock(wdog);
		dev_info(wdog->dev, "firing watchdog\n");

		/* use a timeout of 10 ticks (~150us) */
		writel_relaxed(10 | PM_PASSWORD, wdog->pm + PM_WDOG);
		cur = readl_relaxed(wdog->pm + PM_RSTC);
		writel_relaxed(PM_PASSWORD | (cur & PM_RSTC_WRCFG_CLR)
			| PM_RSTC_WRCFG_FULL_RESET, wdog->pm + PM_RSTC);

		wdog->blocked = true;
		bcm2708_wdog_unlock(wdog);
	}
}

static struct watchdog_ops bcm2708_ops = {
	.owner = THIS_MODULE,
	.start = bcm2708_wdog_start,
	.stop = bcm2708_wdog_stop,
	.get_timeleft = bcm2708_wdog_get_timeleft
};

static int __devinit bcm2708_wdog_probe(struct platform_device *of_dev)
{
	struct watchdog_device *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	struct bcm2708_wdog *wdog = kzalloc(sizeof(*wdog), GFP_KERNEL);
	struct device_node *node = of_dev->dev.of_node;
	int ret;

	if (dev == NULL || wdog == NULL) {
		kfree(wdog);
		kfree(dev);
		return -ENOMEM;
	}

	ret = of_address_to_resource(node, 0, &wdog->res);
	if (ret)
		goto err;

	wdog->dev = &of_dev->dev;
	if (resource_size(&wdog->res) < PM_MINSZ) {
		dev_err(wdog->dev, "resource too small (%#x)\n",
			resource_size(&wdog->res));
		ret = -EINVAL;
		goto err;
	}

	if (!request_region(wdog->res.start, resource_size(&wdog->res),
			dev_name(wdog->dev))) {
		dev_err(wdog->dev, "resource %#lx unavailable\n",
			(unsigned long)wdog->res.start);
		ret = -EBUSY;
		goto err;
	}

	wdog->pm = ioremap(wdog->res.start, resource_size(&wdog->res));
	if (!wdog->pm) {
		dev_err(wdog->dev, "error mapping io at %#lx\n",
			(unsigned long)wdog->res.start);
		ret = -EIO;
		goto err;
	}

	mutex_init(&wdog->lock);
	wdog->started = false;
	wdog->blocked = false;
	dev->info = &wdog->info;
	dev->ops = &bcm2708_ops;
	watchdog_set_drvdata(dev, wdog);

	strncpy(wdog->info.identity, dev_name(wdog->dev),
		sizeof(wdog->info.identity));
	wdog->info.firmware_version = 1;
	wdog->info.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE
		| WDIOF_KEEPALIVEPING;

	dev->min_timeout = WD_MINT;
	dev->timeout = timeout;
	dev->max_timeout = WD_MAXT;
	WARN(dev->timeout < dev->min_timeout, "%u < %u\n", dev->timeout, dev->min_timeout);
	WARN(dev->timeout > dev->max_timeout, "%u > %u\n", dev->timeout, dev->max_timeout);
	watchdog_set_nowayout(dev, nowayout);

	ret = watchdog_register_device(dev);
	if (ret) {
		dev_err(wdog->dev,
			"cannot register watchdog (err=%d)\n", ret);
		goto err;
	}

	dev_info(wdog->dev, "at MMIO %#lx\n",
		(unsigned long)wdog->res.start);
	list_add_tail(&wdog->list, &wdogs);
	platform_set_drvdata(of_dev, dev);
	return 0;

err:
	kfree(wdog);
	kfree(dev);
	return ret;
}

static int __devexit bcm2708_wdog_remove(struct platform_device *of_dev)
{
	struct watchdog_device *dev = platform_get_drvdata(of_dev);
	struct bcm2708_wdog *wdog = watchdog_get_drvdata(dev);

	watchdog_unregister_device(dev);
	list_del(&wdog->list);
	release_region(wdog->res.start, resource_size(&wdog->res));
	kfree(wdog);
	kfree(dev);
	platform_set_drvdata(of_dev, NULL);
	return 0;
}

static struct of_device_id bcm2708_wdog_dt_match[] __devinitconst = {
	{ .compatible = "broadcom,bcm2708-pm-wdog" },
	{}
};
MODULE_DEVICE_TABLE(of, bcm2708_wdog_dt_match);

static struct platform_driver bcm2708_wdog_driver = {
	.probe = bcm2708_wdog_probe,
	.remove = __devexit_p(bcm2708_wdog_remove),
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = bcm2708_wdog_dt_match
	}
};

static void __exit bcm2708_wdog_exit(void)
{
	platform_driver_unregister(&bcm2708_wdog_driver);
}

static int __init bcm2708_wdog_init(void)
{
	int ret;

	/* Check that the timeout value is within it's range;
	 * if not reset to the default */
	if (timeout < WD_MINT || timeout > WD_MAXT) {
		timeout = WD_DEFT;
		printk(KERN_INFO MODULE_NAME ": timeout value must be %d <= timeout <= %d, using %d\n",
			WD_MINT, WD_MAXT, WD_DEFT);
	}

	printk(KERN_INFO MODULE_NAME ": default timeout=%d (nowayout=%d)\n",
		timeout, nowayout);

	ret = platform_driver_register(&bcm2708_wdog_driver);
	if (ret)
		printk(KERN_ERR MODULE_NAME ": registration failed (%d)\n", ret);

	return ret;
}

module_init(bcm2708_wdog_init);
module_exit(bcm2708_wdog_exit);

MODULE_AUTHOR("Luke Diamand, Simon Arlott");
MODULE_DESCRIPTION("Driver for BCM2708 watchdog");
MODULE_LICENSE("GPL");

