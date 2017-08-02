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

#undef PDEBUG
#ifdef SCULL_DEBUG
#ifdef __KERNEL__
#define PDEBUG(fmt, args...) printk(KERN_DEBUG "scull: " fmt, ##args)
#else
#define PDEBUG(fmt, args...) fprintf(stderr, fmt, ##args)
#endif
#else
#define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

static int scull_major = 0;
static int scull_minor = 0;

const int scull_nr_devs = 4;

struct scull_dev *scull_devices;

ssize_t scull_p_read(struct file *filp, char __user *buf,size_t count, loff_t *offp);
ssize_t scull_p_write(struct file *filp,const char __user *buf,size_t count,loff_t *f_pos);
int scull_open(struct inode *inode,struct file *filp);
int scull_release(struct inode *inode, struct file *filp);

struct file_operations scull_p_fops = {
	.owner = THIS_MODULE,
	//.llseek = scull_llseek,
	.read = scull_p_read,
	.write = scull_p_write,
	//.ioctl = scull_ioctl,
	.open = scull_open,
	.release = scull_release,
};

struct scull_pipe {
	wait_queue_head_t inq, outq;
	char *buffer, *end;
	int buffersize;
	char *rp,*wp;
	int nreaders,nwriters;
	struct fasync_struct *async_queue;
	struct semaphore sem;
	struct cdev cdev;
}

int scull_open(struct inode *inode,struct file *filp) {
	struct scull_dev *dev;
	dev = container_of(inode->i_cdev,struct scull_dev,cdev);
	filp->private_data = dev;

	return 0;
}

int scull_release(struct inode *inode, struct file *filp) {
	return 0;
}

ssize_t scull_p_read(struct file *filp, char __user *buf,size_t count, loff_t *f_pos) {
	struct scull_dev *dev = filp->private_data;

	if(down_interruptible(&dev->sem)) return -ERESTARTSYS;

	while(dev->rp == dev->wp) {
		up(&dev->sem);
		if(filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" reading: going to sleep\n",current->comm);
		if(wait_event_interruptible(dev->inq,(dev->rp != dev->wp)))
			return -ERESTARTSYS;
		if(down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}

	if(dev->wp > dev->rp)
		count = min(count,(size_t)(dev->wp-dev->rp));
	else
		count = min(count,(size-t)(dev->end-dev->rp));
	
	if(copy_to_user(buf,dev->rp,count)) {
		up(&dev->sem);
		return -EFAULT;
	}

	if(dev->rp == dev->end)
		dev->rp = dev->buffer;

	up(&dev->sem);

	wake_up_interruptible(&dev->outq);
	PDEBUG("\"%s\" did read %li bytes\n",current->comm,(long count));
	return count;
}

static int spacefree(struct scull_pipe *dev) {
	if(dev->wp == dev->rp) return dev->buffer-1;
	return ((dev->wp + dev->buffer - dev->rp) % dev->buffersize) -1;
}

static int scull_getwritespace(struct scull_pipe *dev,struct file *filp) {

	while(spacefree(dev) == 0) {
		DEFINE_WAIT(wait);
		up(&dev->sem);
		if(filp->f_flags & O_NONBLOCK) return -EAGAIN;
		PDEBUG("\"%s\" writing: going to sleep\n",current->comm);
		prepare_to_wait(&dev->outq,&wait,TASK_INTERRUPTIBLE);
		if(spacefree(dev) == 0)
			schedule();
		finish_wait(&dev->outq,&wait);
		if(signal_pending(current))
			return -ERESTARTSYS;
		if(down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}
	return 0;
}

ssize_t scull_p_write(struct file *filp,const char __user *buf,size_t count,loff_t *f_pos) {
	struct scull_pipe *dev = filp->private_data;
	int result;

	if(down_interruptible(&dev->sem)) {
		return -ERESTARTSYS;
	}

	result = scull_getwritespace(dev,filp);
	if(result)
		return result;

	count = min(count,(size_t)spacefree(dev));

	if(dev->wp >= dev->rp) {
		count = min(count,(size_t)(dev->end - dev->wp));
	} else {
		count = min(count,(size_t)(dev->rp - dev->wp - 1));
	}

	PDEBUG("Going to accept %li bytes to %p from %p\n",(long)count, dev->wp,buf);

	if(copy_from_user(dev->wp,buf,count)) {
		up(&dev->sem);
		return -EFAULT;
	}
	dev->wp += count;
	if(dev->wp == dev->end)
		dev->wp = dev->buffer;
	up(&dev->sem);

	wake_up_interruptible(&dev->inq);

	if(dev->async_queue)
		kill_fasync(&dev->async_queue,SIGIO,POLL_IN);
	
	PDEBUG("\"%s\" did write %li bytes\n",current->comm,(long) count);
	return count;
}

static dev_t scull_setup_dev(struct scull_pipe *dev, dev_t devno, int index) {
	int err;
	cdev_init(&dev->cdev,&scull_p_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_p_fops;
	err = cdev_add(&dev->cdev,devno,1);
	if(err)
		printk(KERN_ALERT"Error %d adding scull pipe %d",err,index);
	else
		devno++;
	return devno
}

dev_t scull_p_init(dev_t devno) 
{
	int err = -1;
	dev_t dev = 0;
	int i;	
	
	scull_devices = kmalloc(scull_nr_devs*sizeof(struct scull_dev),GFP_KERNEL);
	if(!scull_devices)
	{
		err = -ENOMEM;
		goto free_chrdev;
	}
	memset(scull_devices,0,scull_nr_devs*sizeof(struct scull_dev));

	for(i=0;i<scull_nr_devs;i++) {
		sema_init(&scull_devices[i].sem,1);
		devno = scull_setup_dev(&scull_devices[i],devno,i);
	}

	return 0;

free_chrdev:
	unregister_chrdev_region(MKDEV(scull_major,scull_minor),4);
fail:
	return err;
}

void scull_p_exit(void)
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
}

MODULE_LICENSE("GPL");

module_init(scull_init);
module_exit(scull_exit);