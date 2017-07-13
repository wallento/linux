/**
 * Copyright (c) 2012-2017 by the author(s)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * =================================================================
 *
 * Linux driver for OpTiMSoC network-on-chip adapter.
 *
 * Author(s):
 *   Pedro H. Penna <pedrohenriquepenna@gmail.com>
 *   Stefan Wallentowitz <stefan.wallentowitz@tum.de>
 */

#include <asm/io.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#define NR_ENDPOINTS 16

/*
 * Device name.
 */
#define OPTIMSOC_NA_NAME "noc"

/*
 * NoC adapter IRQ number.
 */
#define OPTIMSOC_NA_IRQ 5

/*
 * Base hardware address for NoC adapter.
 */
#define OPTIMSOC_NA_BASE_HWADDR 0xe0000000

/**
 * Base virtual address for NoC adapter
 */
static uint32_t OPTIMSOC_NA_BASE_VADDR = 0;

/*
 * NoC adapter memory size.
 */
#define OPTIMSOC_NA_MEM_SIZE 4096

#define BASE       (OPTIMSOC_NA_BASE_VADDR + 0x100000)
#define REG_NUMEP  BASE
#define EP_BASE    BASE + 0x2000
#define EP_OFFSET  0x2000
#define REG_SEND   0x0
#define REG_RECV   0x0
#define REG_ENABLE 0x4

/*
 * Maximum packet size (in words).
 */
#define OPTIMSOC_NA_MAX_PACKET_SIZE 32

/*
 * Success.
 */
#define SUCCESS 0

/*
 * Failure
 */
#define FAILURE -1

/*
 * Major device number.
 */
static int major;

/*
 * Number of end points.
 */
static uint32_t _num_endpoints;

static struct
{
	int nopen;        /* Number of processes using this device. */
	uint32_t *buffer; /* Input buffer.                          */
} adapters[NR_ENDPOINTS];


/*
 * Sends a word.
 */
static void send(unsigned ep, uint32_t word)
{
	uint32_t *addr;

	addr = (uint32_t *)(EP_BASE + ep*EP_OFFSET + REG_SEND);

	*addr = word;
}

/*
 * Receives a word.
 */
static uint32_t receive(unsigned ep)
{
	uint32_t *addr;

	addr = (uint32_t *)(EP_BASE + ep*EP_OFFSET + REG_RECV);

	return (*addr);
}


/*
 * Handles a NoC IRQ.
 */
static irqreturn_t irq_handler(int irq, void *opaque)
{
    while (1)
	{
		int i;
        uint16_t empty = 0;
		uint16_t ep;
        for (ep = 0; ep < _num_endpoints; ep++)
		{
			/* Get message size. */
            size_t size = receive(ep);

			/* There are no further messages in the buffer. */
            if (size==0)
			{
                empty++;
                continue;
            } 

			/* Message too long. */
			else if (size > OPTIMSOC_NA_MAX_PACKET_SIZE)
			{
				/* Drop packets. */
                for (i = 0; i < size; i++)
                    (void)receive(ep);
            }
			/* Copy message content to buffer. */
			else
			{
                for (i = 0; i < size; i++)
                    adapters[ep].buffer[i] = receive(ep);
            }
        }

        if (empty == _num_endpoints)
            break;
    }

	return (IRQ_HANDLED);
}

void optimsoc_mp_simple_send(uint16_t endpoint, size_t size, uint32_t *buf)
{
	int i;

	send(endpoint, size);
    for (i = 0; i < size; i++)
		send(endpoint, buf[i]);
}

/*
 * Opens the NoC device.
 */
static int device_open(struct inode *inode, struct file *file)
{
	unsigned minor;

	minor = iminor(inode);

	/* Device already in use. */
	if (adapters[minor].nopen > 0)
		return (-EBUSY);

	/* Allocate buffer. */
    adapters[minor].buffer = kmalloc(OPTIMSOC_NA_MAX_PACKET_SIZE*sizeof(uint32_t), GFP_KERNEL);

	adapters[minor].nopen++;
	try_module_get(THIS_MODULE);

	return (SUCCESS);
}

/*
 * Closes the NoC device.
 */
static int device_release(struct inode *inode, struct file *file)
{
	unsigned minor;

	minor = iminor(inode);

	adapters[minor].nopen--;
	module_put(THIS_MODULE);

	return (SUCCESS);
}

/*
 * Reads bytes from the NoC device.
 */
static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t * offset)
{

	unsigned minor;

	minor = iminor(filp->f_inode);

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
	int i;
	int ret;
	const unsigned long flags = IRQF_SHARED | IRQF_TRIGGER_HIGH;

	/* Register interrupt device driver. */
	major = register_chrdev(0, OPTIMSOC_NA_NAME, &fops);
	if (major < 0)
	{
		printk(KERN_ALERT "%s: failed to register driver", OPTIMSOC_NA_NAME);
		goto error0;
	}

	printk(KERN_INFO "%s: got major number %d", OPTIMSOC_NA_NAME, major);

	/* Register interrupt handler. */
	ret = request_irq(OPTIMSOC_NA_IRQ, irq_handler, flags, OPTIMSOC_NA_NAME "-handler", (void *)(irq_handler));
	if (ret != SUCCESS)
		goto error1;

	OPTIMSOC_NA_BASE_VADDR = (uint32_t) ioremap_nocache(OPTIMSOC_NA_BASE_HWADDR, OPTIMSOC_NA_MEM_SIZE);

    _num_endpoints = (uint32_t)(REG_NUMEP);

	/* Initialize devices. */
	for (i = 0; i < NR_ENDPOINTS; i++)
	{
		adapters[i].nopen = 0;
		adapters[i].buffer = NULL;
	}

	return (SUCCESS);

error1:
	unregister_chrdev(major, OPTIMSOC_NA_NAME);
error0:
	return (FAILURE);
}


/*
 * Unloads the device driver module.
 */
static void __exit optimsoc_module_cleanup(void)
{
	unregister_chrdev(major, OPTIMSOC_NA_NAME);
}

module_init(optimsoc_module_init);
module_exit(optimsoc_module_cleanup);

MODULE_INFO(intree, "Y");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pedro H. Penna");
