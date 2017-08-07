#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>

#include "scull.h"

int scull_quantum = 4000;
int scull_qset = 1000;
int scull_nr_devs = 4;

static int scull_major = 0;
static int scull_minor = 0;

struct scull_dev *scull_devices;

int scull_open(struct inode *inode,struct file *filp);
int scull_release(struct inode *inode, struct file *filp);

struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	//.llseek = scull_llseek,
	.read = scull_read,
	.write = scull_write,
	//.ioctl = scull_ioctl,
	.open = scull_open,
	.release = scull_release,
};

int scull_trim(struct scull_dev *dev) {
	struct scull_qset *next, *dptr;
	int qset = dev->qset;
	int i;

	for(dptr = dev->data;dptr;dptr=next) {
		if(dptr->data) {
			for(i=0;i<qset;i++)
				kfree(dptr->data[i]);
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
	}

	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	return 0;
} 

int scull_open(struct inode *inode,struct file *filp) {
	struct scull_dev *dev;
	dev = container_of(inode->i_cdev,struct scull_dev,cdev);
	filp->private_data = dev;

	if((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		scull_trim(dev);
	}

	return 0;
}

int scull_release(struct inode *inode, struct file *filp) {
	return 0;
}

static struct scull_qset *scull_follow(struct scull_dev *dev,int n) {
	struct scull_qset *qs = dev->data;

	if(!qs) {
		qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if(qs == NULL) return NULL;
		memset(qs,0,sizeof(struct scull_qset));
	}

	while(n--) {
		if(!qs->next) {
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if(qs->next == NULL) return NULL;
			memset(qs->next,0,sizeof(struct scull_qset));
		}
		qs = qs->next;
		continue;
	}
	return qs;
}

ssize_t scull_read(struct file *filp, char __user *buf,size_t count, loff_t *f_pos) {
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval = 0;

	if(down_interruptible(&dev->sem)) return -ERESTARTSYS;
	if(*f_pos >= dev->size) goto out;
	if(*f_pos + count > dev->size) count = dev->size - *f_pos;

	item = (long)*f_pos/itemsize;
	rest = (long)*f_pos%itemsize;
	s_pos = rest / quantum; q_pos = rest%quantum;

	dptr = scull_follow(dev,item);

	if(dptr == NULL || !dptr->data || !dptr->data[s_pos]) goto out;

	if(count > quantum - q_pos) count = quantum - q_pos;

	if(copy_to_user(buf,dptr->data[s_pos] + q_pos,count)) {
		retval = -EFAULT;
		goto out;
	}
	
	*f_pos += count;
	retval = count;

out:
	up(&dev->sem);
	return retval;
}

ssize_t scull_write(struct file *filp,const char __user *buf,size_t count,loff_t *f_pos) {
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item,s_pos,q_pos,rest;
	ssize_t retval = -ENOMEM;

	if(down_interruptible(&dev->sem)) return -ERESTARTSYS;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	dptr = scull_follow(dev,item);

	if(dptr == NULL) goto out;
	if(!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(char *),GFP_KERNEL);
		if(!dptr->data) goto out;
		memset(dptr->data,0,qset*sizeof(char *));
	}
	if(!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmalloc(quantum,GFP_KERNEL);
		if(!dptr->data[s_pos]) goto out;
	}

	if(count > quantum - q_pos) count = quantum - q_pos;

	if(copy_from_user(dptr->data[s_pos] + q_pos,buf,count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

	if(dev->size < *f_pos) dev->size = *f_pos;

out:
	up(&dev->sem);
	return retval;
}

static void scull_setup_dev(struct scull_dev *dev, int index) {
	int err, devno = MKDEV(scull_major,scull_minor+index);

	cdev_init(&dev->cdev,&scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	err = cdev_add(&dev->cdev,devno,1);
	if(err)
		printk(KERN_ALERT"Error %d adding scull%d",err,index);
}

static int __init scull_init(void) 
{
	int err = -1;
	dev_t dev = 0;
	int i;	
	
	printk(KERN_ALERT"Initializing scull device.\n");

	err = alloc_chrdev_region(&dev,0,4,"scull");

	if(err < 0)
	{
		printk(KERN_ALERT"Failed to alloc freg device.\n");
		goto fail;
	}

	scull_major = MAJOR(dev);
	scull_minor = MINOR(dev);


	scull_devices = kmalloc(scull_nr_devs*sizeof(struct scull_dev),GFP_KERNEL);
	if(!scull_devices)
	{
		err = -ENOMEM;
		goto free_chrdev;
	}
	memset(scull_devices,0,scull_nr_devs*sizeof(struct scull_dev));

	for(i=0;i<scull_nr_devs;i++) {
		scull_devices[i].quantum = scull_quantum;
		scull_devices[i].qset = scull_qset;
		sema_init(&scull_devices[i].sem,1);
		scull_setup_dev(&scull_devices[i],i);
	}
	dev += scull_nr_devs;
	dev += scull_p_init(dev);
	dev += scull_access_init(dev);
	return 0;

free_chrdev:
	unregister_chrdev_region(MKDEV(scull_major,scull_minor),4);
fail:
	return err;
}

static void __exit scull_exit(void)
{
	int i;
	dev_t devno = MKDEV(scull_major,scull_minor);
	printk(KERN_ALERT"Destroy scull device.\n");

	for(i=0;i<scull_nr_devs;i++) {
		scull_trim(scull_devices+i);
		cdev_del(&scull_devices[i].cdev);	
	}
	kfree(scull_devices);
	unregister_chrdev_region(devno,4);

	scull_p_exit();
	scull_access_cleanup();
}

MODULE_LICENSE("GPL");

module_init(scull_init);
module_exit(scull_exit);