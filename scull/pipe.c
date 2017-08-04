#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/poll.h>

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

const int scull_p_nr_devs = 4;
const int scull_p_buffer =  4000;

struct scull_pipe *scull_p_devices;

dev_t scull_p_devno;

ssize_t scull_p_read(struct file *filp, char __user *buf,size_t count, loff_t *offp);
ssize_t scull_p_write(struct file *filp,const char __user *buf,size_t count,loff_t *f_pos);
int scull_p_open(struct inode *inode,struct file *filp);
int scull_p_release(struct inode *inode, struct file *filp);
static int scull_p_fasync(int fd,struct file *filp,int mode);
static unsigned int scull_p_poll(struct file *filp, poll_table *wait);


struct file_operations scull_p_fops = {
	.owner = THIS_MODULE,
	//.llseek = scull_llseek,
	.read = scull_p_read,
	.write = scull_p_write,
	//.ioctl = scull_ioctl,
	.open = scull_p_open,
	.release = scull_p_release,
	.poll = scull_p_poll,
	.fasync = scull_p_fasync,
};

struct scull_pipe {
	wait_queue_head_t inq, outq;
	char *buffer, *end;
	int buffersize;
	char *rp,*wp;
	int nreaders,nwriters;
	struct fasync_struct *async_queue;
	struct mutex mutex;
	struct cdev cdev;
};

int scull_p_open(struct inode *inode,struct file *filp) {
	struct scull_pipe *dev;
	dev = container_of(inode->i_cdev,struct scull_pipe,cdev);
	filp->private_data = dev;

	if(mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;
	if(!dev->buffer) {
		dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
		if(!dev->buffer) {
			mutex_unlock(&dev->mutex);
			return -ENOMEM;
		}

		dev->buffersize = scull_p_buffer;
		dev->end = dev->buffer + dev->buffersize;
		dev->rp = dev->wp = dev->buffer;
	}

	if(filp->f_mode & FMODE_READ)
		dev->nreaders++;
	if(filp->f_mode & FMODE_WRITE)
		dev->nwriters++;
	mutex_unlock(&dev->mutex);

	return nonseekable_open(inode,filp);
}

int scull_p_release(struct inode *inode, struct file *filp) {
	struct scull_pipe *dev = filp->private_data;

	scull_p_fasync(-1,filp,0);
	mutex_lock(&dev->mutex);
	if(filp->f_mode & FMODE_READ)
		dev->nreaders --;
	if(filp->f_mode & FMODE_WRITE)
		dev->nwriters --;
	if(dev->nreaders + dev->nwriters == 0) {
		kfree(dev->buffer);
		dev->buffer = NULL;
	}
	mutex_unlock(&dev->mutex);
	return 0;
}

ssize_t scull_p_read(struct file *filp, char __user *buf,size_t count, loff_t *f_pos) {
	struct scull_pipe *dev = filp->private_data;

	if(mutex_lock_interruptible(&dev->mutex)) return -ERESTARTSYS;

	while(dev->rp == dev->wp) {
		mutex_unlock(&dev->mutex);
		
		if(filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" reading: going to sleep\n",current->comm);
		if(wait_event_interruptible(dev->inq,(dev->rp != dev->wp)))
			return -ERESTARTSYS;
		
		if(mutex_lock_interruptible(&dev->mutex))
			return -ERESTARTSYS;
	}

	if(dev->wp > dev->rp)
		count = min(count,(size_t)(dev->wp-dev->rp));
	else
		count = min(count,(size_t)(dev->end-dev->rp));
	
	if(copy_to_user(buf,dev->rp,count)) {
		mutex_unlock(&dev->mutex);
		return -EFAULT;
	}

	dev->rp += count;

	if(dev->rp == dev->end)
		dev->rp = dev->buffer;

	mutex_unlock(&dev->mutex);

	wake_up_interruptible(&dev->outq);
	PDEBUG("\"%s\" did read %li bytes\n",current->comm,(long)count);
	return count;
}

static int spacefree(struct scull_pipe *dev) {
	if(dev->wp == dev->rp) return dev->buffersize-1;
	return ((dev->wp + dev->buffersize - dev->rp) % dev->buffersize) -1;
}

static int scull_getwritespace(struct scull_pipe *dev,struct file *filp) {

	while(spacefree(dev) == 0) {
		DEFINE_WAIT(wait);
		mutex_unlock(&dev->mutex);
		if(filp->f_flags & O_NONBLOCK) return -EAGAIN;
		PDEBUG("\"%s\" writing: going to sleep\n",current->comm);
		prepare_to_wait(&dev->outq,&wait,TASK_INTERRUPTIBLE);
		if(spacefree(dev) == 0)
			schedule();
		finish_wait(&dev->outq,&wait);
		if(signal_pending(current))
			return -ERESTARTSYS;
		if(mutex_lock_interruptible(&dev->mutex))
			return -ERESTARTSYS;
	}
	return 0;
}

ssize_t scull_p_write(struct file *filp,const char __user *buf,size_t count,loff_t *f_pos) {
	struct scull_pipe *dev = filp->private_data;
	int result;

	if(mutex_lock_interruptible(&dev->mutex)) {
		return -ERESTARTSYS;
	}

	result = scull_getwritespace(dev,filp);
	if(result)
		return result;//不需要释放mutex，scull_getwritespace已经处理了

	count = min(count,(size_t)spacefree(dev));

	if(dev->wp >= dev->rp) {
		count = min(count,(size_t)(dev->end - dev->wp));
	} else {
		count = min(count,(size_t)(dev->rp - dev->wp - 1));
	}

	PDEBUG("Going to accept %li bytes to %p from %p\n",(long)count, dev->wp,buf);

	if(copy_from_user(dev->wp,buf,count)) {
		mutex_unlock(&dev->mutex);
		return -EFAULT;
	}
	dev->wp += count;
	if(dev->wp == dev->end)
		dev->wp = dev->buffer;
	mutex_unlock(&dev->mutex);

	wake_up_interruptible(&dev->inq);

	if(dev->async_queue)
		kill_fasync(&dev->async_queue,SIGIO,POLL_IN);
	
	PDEBUG("\"%s\" did write %li bytes\n",current->comm,(long) count);
	return count;
}

static void scull_p_setup_cdev(struct scull_pipe *dev, int index) {
	int err;
	cdev_init(&dev->cdev,&scull_p_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_p_fops;
	err = cdev_add(&dev->cdev,scull_p_devno + index,1);
	if(err)
		printk(KERN_ALERT"Error %d adding scull pipe %d",err,index);
}

int scull_p_init(dev_t first_devno) 
{
	int result,i;

	result = register_chrdev_region(first_devno,scull_p_nr_devs,"scullp");
	if(result < 0) {
		printk(KERN_NOTICE "Unable to get scullp region, error %d\n", result);
		goto fail;
	}
	
	scull_p_devno = first_devno;

	scull_p_devices = kmalloc(scull_p_nr_devs*sizeof(struct scull_pipe),GFP_KERNEL);
	if(!scull_p_devices)
	{
		printk(KERN_ALERT "Unable to malloc scullp devices, error %d\n", result);
		goto free_chrdev;
	}
	memset(scull_p_devices,0,scull_p_nr_devs*sizeof(struct scull_pipe));

	for(i=0;i<scull_p_nr_devs;i++) {
		init_waitqueue_head(&(scull_p_devices[i].inq));
		init_waitqueue_head(&(scull_p_devices[i].outq));
		mutex_init(&scull_p_devices[i].mutex);
		scull_p_setup_cdev(&scull_p_devices[i],i);
	}

	return scull_p_nr_devs;

free_chrdev:
	unregister_chrdev_region(first_devno,scull_p_nr_devs);
fail:
	return 0;
}

void scull_p_exit(void)
{
	int i;

	if(!scull_p_devices)
		return;

	printk(KERN_ALERT"Destroy scull pipe devices.\n");

	for(i=0;i<scull_p_nr_devs;i++) {
		cdev_del(&scull_p_devices[i].cdev);
		kfree(scull_p_devices[i].buffer);
	}
	kfree(scull_p_devices);
	unregister_chrdev_region(scull_p_devno,scull_p_nr_devs);
	scull_p_devices = NULL;
}

static unsigned int scull_p_poll(struct file *filp,poll_table *wait)
{
	struct scull_pipe *dev = filp->private_data;
	unsigned int mask = 0;

	mutex_lock(&dev->mutex);
	poll_wait(filp,&dev->inq,wait);
	poll_wait(filp,&dev->outq,wait);
	if(dev->rp != dev->wp)
		mask |= POLLIN | POLLRDNORM;
	if(spacefree(dev))
		mask |= POLLOUT | POLLWRNORM;
	mutex_unlock(&dev->mutex);
	return mask;
}

static int scull_p_fasync(int fd, struct file *filp, int mode)
{
	struct scull_pipe *dev = filp->private_data;

	return fasync_helper(fd, filp, mode, &dev->async_queue);
}