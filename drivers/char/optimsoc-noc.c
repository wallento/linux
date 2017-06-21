/*
 * OpTiMSoC NoC Driver - https://www.optimsoc.org
 *
 * Copyright(C) 2017 Pedro Henrique Penna <pedrohenriquepenna@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/fs.h>

/*
 * Success.
 */
#define SUCCESS 0

/*
 * Device name.
 */
#define DEVICE_NAME "optimsoc-noc"

/*
 * Base hardware address for NoC adapter.
 */
#define OPTIMSOC_NOC_HWADDR 0xe0100000 

/*
 * NoC adapter memory size.
 */
#define OPTIMSOC_NOC_MEM_SIZE 4096

/*
 * NoC adapter IRQ number.
 */
#define OPTIMSOC_NOC_IRQ 5

/*
 * Major device number.
 */
static int major;

/**
 * Read pointer.
 */
static int *rptr = NULL;

/**
 * Write pointer.
 */
static int *wptr = NULL;

/*
 * Number of threads using the device. 
 */
static int nopen = 0;

/*
 * Handles a NoC IRQ.
 */
static irqreturn_t irq_handler(int irq, void *opaque)
{
	printk(KERN_INFO "interrupt fired\n");

	return (IRQ_HANDLED);
}

/*
 * Opens the NoC device.
 */
static int device_open(struct inode *inode, struct file *file)
{
	/* Device already in use. */
	if (nopen > 0)
		return (-EBUSY);

	wptr = rptr = 
		ioremap_nocache(OPTIMSOC_NOC_HWADDR, OPTIMSOC_NOC_MEM_SIZE);

	nopen++;
	try_module_get(THIS_MODULE);

	return (SUCCESS);
}

/*
 * Closes the NoC device.
 */
static int device_release(struct inode *inode, struct file *file)
{
	nopen--;
	module_put(THIS_MODULE);

	return (SUCCESS);
}

/*
 * Reads bytes from the NoC device.
 */
static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t * offset)
{
	wptr += 512;
	*wptr = *rptr;

	return (0);
}

/*
 * Writes butes from the NoC device.
 */
static ssize_t device_write(struct file *filp, const char *buff, size_t len, loff_t * off)
{
	return (-EINVAL);
}

static struct file_operations fops = {
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release
};


/*
 * Initializes the device driver module.
 */
static int __init optimsoc_module_init(void)
{
	int ret;

	/* Register interrupt device driver. */
	major = register_chrdev(0, DEVICE_NAME, &fops);
	if (major < 0)
	{
		printk(KERN_ALERT "%s: failed to register device", DEVICE_NAME);

		return (major);
	}

	/* Register interrupt handler. */
	ret = request_irq(OPTIMSOC_NOC_IRQ, irq_handler,IRQF_SHARED | IRQF_TRIGGER_HIGH, "optimsoc-noc-handler", (void *)(irq_handler));
	if (ret != SUCCESS)
		return (ret);

	printk(KERN_INFO "%s: major number %d", DEVICE_NAME, major);
	printk(KERN_INFO "%s: invoke 'mknod /dev/%s c %d 0' to instantiate this device", DEVICE_NAME, DEVICE_NAME, major);

	return (ret);
}

/*
 * Unloads the device driver module.
 */
static void __exit optimsoc_module_cleanup(void)
{
	unregister_chrdev(major, DEVICE_NAME);
}

module_init(optimsoc_module_init);
module_exit(optimsoc_module_cleanup);

MODULE_INFO(intree, "Y");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pedro H. Penna");
