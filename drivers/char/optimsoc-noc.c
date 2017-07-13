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

#define OPTIMSOC_NA_REGS       (OPTIMSOC_NA_BASE_VADDR + 0x00000)
#define OPTIMSOC_NA_TILEID     (OPTIMSOC_NA_REGS       + 0x0)
#define OPTIMSOC_NA_NUMTILES   (OPTIMSOC_NA_REGS       + 0x4)
#define OPTIMSOC_NA_COREBASE   (OPTIMSOC_NA_REGS       + 0x10)
#define OPTIMSOC_NA_TOTALCORES (OPTIMSOC_NA_REGS       + 0x18)
#define OPTIMSOC_NA_GMEM_SIZE  (OPTIMSOC_NA_REGS       + 0x1c)
#define OPTIMSOC_NA_GMEM_TILE  (OPTIMSOC_NA_REGS       + 0x20)
#define OPTIMSOC_NA_LMEM_SIZE  (OPTIMSOC_NA_REGS       + 0x24)
#define OPTIMSOC_NA_CT_NUM     (OPTIMSOC_NA_REGS       + 0x28)
#define OPTIMSOC_NA_SEED       (OPTIMSOC_NA_REGS       + 0x2c)
#define OPTIMSOC_NA_CT_LIST    (OPTIMSOC_NA_REGS       + 0x200)

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

#define OPTIMSOC_DEST_MSB 31
#define OPTIMSOC_DEST_LSB 27
#define OPTIMSOC_CLASS_MSB 26
#define OPTIMSOC_CLASS_LSB 24
#define OPTIMSOC_CLASS_NUM 8
#define OPTIMSOC_SRC_MSB 23
#define OPTIMSOC_SRC_LSB 19

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
 * Number of threads using the device. 
 */
static int nopen = 0;

/*
 * Buffers.
 */
static uint32_t *_buffer;

static volatile uint32_t *_domains_ready;

/*
 * Class handlers.
 */
void (*cls_handlers[OPTIMSOC_CLASS_NUM])(uint32_t*,size_t);

/*
 * Number of end points.
 */
static uint32_t _num_endpoints;

#define EXTRACT(x,msb,lsb) ((x>>lsb) & ~(~0 << (msb-lsb+1)))
#define SET(x,v,msb,lsb) (((~0 << ((msb)+1) | ~(~0 << (lsb)))&x) | \
		(((v) & ~(~0<<((msb)-(lsb)+1))) << (lsb)))

int optimsoc_get_tilerank(unsigned int tile) {
	int i;
    uint16_t *ctlist = (uint16_t*) OPTIMSOC_NA_CT_LIST;
    for (i = 0; i < OPTIMSOC_NA_CT_NUM; i++) {
        if (ctlist[i] == tile) {
            return i;
        }
    }
    return -1;
}

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
            uint32_t header;
            uint8_t class;
			uint32_t src;
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
                    _buffer[i] = receive(ep);
            }

            header = _buffer[0];
			
            /** Extract class. */
            class = EXTRACT(header, OPTIMSOC_CLASS_MSB, OPTIMSOC_CLASS_LSB);
            if (class == OPTIMSOC_CLASS_NUM-1)
			{
                uint32_t ready = (header & 0x2) >> 1;
                if (ready)
				{
                    uint32_t tile, domain;
                    uint8_t endpoint;
                    tile = EXTRACT(header, OPTIMSOC_SRC_MSB, OPTIMSOC_SRC_LSB);
                    domain = optimsoc_get_tilerank(tile);
                    endpoint = EXTRACT(header, 9, 2);
                    _domains_ready[domain] |= 1 << endpoint;
                }
            }

            // Call respective class handler
            if (cls_handlers[class] == 0) {
				printk(KERN_ALERT "%s: dropping packet of unknown class %d", OPTIMSOC_NA_NAME, class);
                continue;
            }

            src = (_buffer[0]>>OPTIMSOC_SRC_LSB) & 0x1f;

            cls_handlers[class](_buffer,size);

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
	/* Device already in use. */
	if (nopen > 0)
		return (-EBUSY);

	OPTIMSOC_NA_BASE_VADDR = (uint32_t) ioremap_nocache(OPTIMSOC_NA_BASE_HWADDR, OPTIMSOC_NA_MEM_SIZE);

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

    /* Reset class handlers. */
    for (i = 0; i <OPTIMSOC_CLASS_NUM; i++) {
        cls_handlers[i] = NULL;
    }

    _num_endpoints = (uint32_t)(REG_NUMEP);
    _domains_ready = kmalloc(OPTIMSOC_NA_CT_NUM*sizeof(uint32_t), GFP_KERNEL);

	/* Allocate buffer. */
    _buffer = kmalloc(OPTIMSOC_NA_MAX_PACKET_SIZE*sizeof(uint32_t), GFP_KERNEL);

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
