/*
 *  chardev.c: Creates a read-only char device that says how many times
 *  you've read from the dev file
 */

#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/fs.h>

#define OPTIMSOC_NA_HWADDR 0xe0100000 

#define DEVICE_NAME "optimsoc"

#define SUCCESS 0

static int Major;
static int Device_Open = 0;
static int *w, *r;

irqreturn_t irq_handler(int irq, void *foobar)
{
	printk(KERN_INFO "interrupt fired\n");

	return (IRQ_HANDLED);
}

/* 
 * Called when a process tries to open the device file, like
 * "cat /dev/mycharfile"
 */
static int device_open(struct inode *inode, struct file *file)
{
	if (Device_Open > 0)
		return (-EBUSY);

	w = r = ioremap_nocache(OPTIMSOC_NA_HWADDR, 4096);

	Device_Open++;
	try_module_get(THIS_MODULE);

	return (SUCCESS);
}

/* 
 * Called when a process closes the device file.
 */
static int device_release(struct inode *inode, struct file *file)
{
	Device_Open--;
	module_put(THIS_MODULE);

	return (SUCCESS);
}

/* 
 * Called when a process, which already opened the dev file, attempts to
 * read from it.
 */
static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t * offset)
{
	w += 512;
	*w = *r;

	return (0);
}

/*  
 * Called when a process writes to dev file: echo "hi" > /dev/hello 
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
 * This function is called when the module is loaded
 */
static int __init hello_init_module(void)
{
	int ret;

	Major = register_chrdev(0, DEVICE_NAME, &fops);

	if (Major < 0) {
	  printk(KERN_ALERT "Registering char device failed with %d\n", Major);
	  return Major;
	}

	ret = request_irq(5, irq_handler, IRQF_SHARED | IRQF_TRIGGER_HIGH, "optimsoc-na-handler", (void *)(irq_handler));

	/* Failed to register interrupt handler. */
	if (ret != SUCCESS)
		return (ret);

	printk(KERN_INFO "%s: major number %d", DEVICE_NAME, Major);
	printk(KERN_INFO "%s: invoke 'mknod /dev/%s c %d 0' to instantiate this device", DEVICE_NAME, DEVICE_NAME, Major);

	return (ret);
}

/*
 * This function is called when the module is unloaded
 */
static void __exit hello_cleanup_module(void)
{
	unregister_chrdev(Major, DEVICE_NAME);
}

module_init(hello_init_module);
module_exit(hello_cleanup_module);


MODULE_INFO(intree, "Y");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pedro H. Penna");
