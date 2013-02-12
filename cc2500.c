/*
cc2500.c
Copyright Ryan Voots, 2013
Based on cc2500.c by Scott Ellis, 2010

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

cc2500 v0.1 loads and registers a spi driver for a device at the bus/
cable select specified by the constants SPI_BUS.SPI_BUS_CS1
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <asm/uaccess.h>

#define USER_BUFF_SIZE 1024

#define SPI_BUS 1
#define SPI_BUS_CS1 1
#define SPI_BUS_SPEED 1000000

const char this_driver_name[] = "cc2500";

struct cc2500_dev {
	struct semaphore spi_sem;
	struct semaphore fop_sem;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct spi_device *spi_device;
	char *user_buff;
};

static struct cc2500_dev cc2500_dev;

static ssize_t cc2500_read(struct file *filp, char __user * buff, size_t count,
			  loff_t * offp)
{
	size_t len;
	ssize_t status = 0;

	if (!buff)
		return -EFAULT;

	if (*offp > 0)
		return 0;

	if (down_interruptible(&cc2500_dev.fop_sem))
		return -ERESTARTSYS;

	if (!cc2500_dev.spi_device)
		strcpy(cc2500_dev.user_buff, "spi_device is NULL\n");
	else if (!cc2500_dev.spi_device->master)
		strcpy(cc2500_dev.user_buff, "spi_device->master is NULL\n");
	else
		sprintf(cc2500_dev.user_buff, "%s ready on SPI%d.%d\n",
			this_driver_name,
			cc2500_dev.spi_device->master->bus_num,
			cc2500_dev.spi_device->chip_select);

	len = strlen(cc2500_dev.user_buff);

	if (len < count)
		count = len;

	if (copy_to_user(buff, cc2500_dev.user_buff, count)) {
		printk(KERN_ALERT "cc2500_read(): copy_to_user() failed\n");
		status = -EFAULT;
	} else {
		*offp += count;
		status = count;
	}

	up(&cc2500_dev.fop_sem);

	return status;
}

static int cc2500_open(struct inode *inode, struct file *filp)
{
	int status = 0;

	if (down_interruptible(&cc2500_dev.fop_sem))
		return -ERESTARTSYS;

	if (!cc2500_dev.user_buff) {
		cc2500_dev.user_buff = kmalloc(USER_BUFF_SIZE, GFP_KERNEL);
		if (!cc2500_dev.user_buff)
			status = -ENOMEM;
	}

	up(&cc2500_dev.fop_sem);

	return status;
}

static int cc2500_probe(struct spi_device *spi_device)
{
	if (down_interruptible(&cc2500_dev.spi_sem))
		return -EBUSY;

	cc2500_dev.spi_device = spi_device;

	up(&cc2500_dev.spi_sem);

	return 0;
}

static int cc2500_remove(struct spi_device *spi_device)
{
	if (down_interruptible(&cc2500_dev.spi_sem))
		return -EBUSY;

	cc2500_dev.spi_device = NULL;

	up(&cc2500_dev.spi_sem);

	return 0;
}

static int __init add_cc2500_device_to_bus(void)
{
	struct spi_master *spi_master;
	struct spi_device *spi_device;
	struct device *pdev;
	char buff[64];
	int status = 0;

	spi_master = spi_busnum_to_master(SPI_BUS);
	if (!spi_master) {
		printk(KERN_ALERT "spi_busnum_to_master(%d) returned NULL\n",
		       SPI_BUS);
		printk(KERN_ALERT "Missing modprobe omap2_mcspi?\n");
		return -1;
	}

	spi_device = spi_alloc_device(spi_master);
	if (!spi_device) {
		put_device(&spi_master->dev);
		printk(KERN_ALERT "spi_alloc_device() failed\n");
		return -1;
	}

	spi_device->chip_select = SPI_BUS_CS1;

/* Check whether this SPI bus.cs is already claimed */
	snprintf(buff, sizeof(buff), "%s.%u",
		 dev_name(&spi_device->master->dev), spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);
	if (pdev) {
/* We are not going to use this spi_device, so free it */
		spi_dev_put(spi_device);

/*
* There is already a device configured for this bus.cs
* It is okay if it us, otherwise complain and fail.
*/
		if (pdev->driver && pdev->driver->name &&
		    strcmp(this_driver_name, pdev->driver->name)) {
			printk(KERN_ALERT
			       "Driver [%s] already registered for %s\n",
			       pdev->driver->name, buff);
			status = -1;
		}
	} else {
		spi_device->max_speed_hz = SPI_BUS_SPEED;
		spi_device->mode = SPI_MODE_0;
		spi_device->bits_per_word = 8;
		spi_device->irq = -1;
		spi_device->controller_state = NULL;
		spi_device->controller_data = NULL;
		strlcpy(spi_device->modalias, this_driver_name, SPI_NAME_SIZE);

		status = spi_add_device(spi_device);
		if (status < 0) {
			spi_dev_put(spi_device);
			printk(KERN_ALERT "spi_add_device() failed: %d\n",
			       status);
		}
	}

	put_device(&spi_master->dev);

	return status;
}

static struct spi_driver cc2500_driver = {
	.driver = {
		   .name = this_driver_name,
		   .owner = THIS_MODULE,
		   },
	.probe = cc2500_probe,
	.remove = __devexit_p(cc2500_remove),
};

static int __init cc2500_init_spi(void)
{
	int error;

	error = spi_register_driver(&cc2500_driver);
	if (error < 0) {
		printk(KERN_ALERT "spi_register_driver() failed %d\n", error);
		return error;
	}

	error = add_cc2500_device_to_bus();
	if (error < 0) {
		printk(KERN_ALERT "add_cc2500_to_bus() failed\n");
		spi_unregister_driver(&cc2500_driver);
		return error;
	}

	return 0;
}

static const struct file_operations cc2500_fops = {
	.owner = THIS_MODULE,
	.read = cc2500_read,
	.open = cc2500_open,
};

static int __init cc2500_init_cdev(void)
{
	int error;

	cc2500_dev.devt = MKDEV(0, 0);

	error = alloc_chrdev_region(&cc2500_dev.devt, 0, 1, this_driver_name);
	if (error < 0) {
		printk(KERN_ALERT "alloc_chrdev_region() failed: %d \n", error);
		return -1;
	}

	cdev_init(&cc2500_dev.cdev, &cc2500_fops);
	cc2500_dev.cdev.owner = THIS_MODULE;

	error = cdev_add(&cc2500_dev.cdev, cc2500_dev.devt, 1);
	if (error) {
		printk(KERN_ALERT "cdev_add() failed: %d\n", error);
		unregister_chrdev_region(cc2500_dev.devt, 1);
		return -1;
	}

	return 0;
}

static int __init cc2500_init_class(void)
{
	cc2500_dev.class = class_create(THIS_MODULE, this_driver_name);

	if (!cc2500_dev.class) {
		printk(KERN_ALERT "class_create() failed\n");
		return -1;
	}

	if (!device_create(cc2500_dev.class, NULL, cc2500_dev.devt, NULL,
			   this_driver_name)) {
		printk(KERN_ALERT "device_create(..., %s) failed\n",
		       this_driver_name);
		class_destroy(cc2500_dev.class);
		return -1;
	}

	return 0;
}

static int __init cc2500_init(void)
{
	memset(&cc2500_dev, 0, sizeof(cc2500_dev));

	sema_init(&cc2500_dev.spi_sem, 1);
	sema_init(&cc2500_dev.fop_sem, 1);

	if (cc2500_init_cdev() < 0)
		goto fail_1;

	if (cc2500_init_class() < 0)
		goto fail_2;

	if (cc2500_init_spi() < 0)
		goto fail_3;

	return 0;

 fail_3:
	device_destroy(cc2500_dev.class, cc2500_dev.devt);
	class_destroy(cc2500_dev.class);

 fail_2:
	cdev_del(&cc2500_dev.cdev);
	unregister_chrdev_region(cc2500_dev.devt, 1);

 fail_1:
	return -1;
}

module_init(cc2500_init);

static void __exit cc2500_exit(void)
{
	spi_unregister_device(cc2500_dev.spi_device);
	spi_unregister_driver(&cc2500_driver);

	device_destroy(cc2500_dev.class, cc2500_dev.devt);
	class_destroy(cc2500_dev.class);

	cdev_del(&cc2500_dev.cdev);
	unregister_chrdev_region(cc2500_dev.devt, 1);

	if (cc2500_dev.user_buff)
		kfree(cc2500_dev.user_buff);
}

module_exit(cc2500_exit);

MODULE_AUTHOR("Ryan Voots");
MODULE_DESCRIPTION("cc2500 module - driver for cc2500 radio transciever");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
