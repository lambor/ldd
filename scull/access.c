#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/cdev.h>
#include <linux/tty.h>
#include <asm/atomic.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock_types.h>
#include <linux/semaphore.h>
#include <linux/uidgid.h>

#include "scull.h"


static dev_t scull_a_firstdev;

static struct scull_dev scull_s_device;
static atomic_t scull_s_available = ATOMIC_INIT(1);

static int scull_s_open(struct inode *inode,struct file *filp)
{
    struct scull_dev *dev=&scull_s_device;
    if(!atomic_dec_and_test(&scull_s_available))
    {
        atomic_inc(&scull_s_available);
        return -EBUSY;
    }

    if((filp->f_flags & O_ACCMODE) == O_WRONLY)
        scull_trim(dev);
    filp->private_data = dev;
    return 0;
}

static int scull_s_release(struct inode *inode,struct file *filp) {
    atomic_inc(&scull_s_available);
    return 0;
}

struct file_operations scull_sngl_fops = {
    .owner = THIS_MODULE,
    .read = scull_read,
    .write = scull_write,
    .open = scull_s_open,
    .release = scull_s_release,
};

static struct scull_dev scull_u_device;
static int scull_u_count;
static uid_t scull_u_owner;
DEFINE_SPINLOCK(scull_u_lock);

static int scull_u_open(struct inode *inode,struct file *filp)
{
    struct scull_dev *dev = &scull_u_device;
    spin_lock(&scull_u_lock);
    if(scull_u_count && 
        (scull_u_owner != current->cred->uid.val) &&
        (scull_u_owner != current->cred->euid.val) &&
        !capable(CAP_DAC_OVERRIDE)) 
    {
        spin_unlock(&scull_u_lock);
        return -EBUSY;
    }

    if(scull_u_count == 0)
        scull_u_owner = current->cred->uid.val;
    
    scull_u_count++;
    spin_unlock(&scull_u_lock);

    if((filp->f_flags & O_ACCMODE) == O_WRONLY)
        scull_trim(dev);
    filp->private_data = dev;
    return 0;
}

static int scull_u_release(struct inode *inode,struct file *filp)
{
    spin_lock(&scull_u_lock);
    scull_u_count--;
    spin_unlock(&scull_u_lock);
    return 0;
}

struct file_operations scull_user_fops = {
    .owner = THIS_MODULE,
    .read = scull_read,
    .write = scull_write,
    .open = scull_u_open,
    .release = scull_u_release,
};

static struct scull_dev scull_w_device;
static int scull_w_count;
static uid_t scull_w_owner;
static DECLARE_WAIT_QUEUE_HEAD(scull_w_wait);
DEFINE_SPINLOCK(scull_w_lock);

static inline int scull_w_available(void)
{
    return scull_w_count == 0||
        scull_w_owner == current->cred->uid.val ||
        scull_w_owner == current->cred->euid.val ||
        capable(CAP_DAC_OVERRIDE);
}

static int scull_w_open(struct inode *inode,struct file *filp)
{
    struct scull_dev *dev = &scull_w_device;
    spin_lock(&scull_w_lock);
    while(!scull_w_available()) {
        spin_unlock(&scull_w_lock);
        if(filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if(wait_event_interruptible(scull_w_wait,scull_w_available()))
            return -ERESTARTSYS;
        spin_lock(&scull_w_lock);
    }
    if(scull_w_count == 0)
        scull_w_owner = current->cred->uid.val;
    scull_w_count++;
    spin_unlock(&scull_w_lock);
    if((filp->f_flags & O_ACCMODE) == O_WRONLY)
        scull_trim(dev);
    filp->private_data = dev;
    return 0;
}

static int scull_w_release(struct inode *inode,struct file *filp) {
    int temp;
    
    spin_lock(&scull_w_lock);
    scull_w_count--;
    temp = scull_w_count;
    spin_unlock(&scull_w_lock);

    if(temp == 0)
        wake_up_interruptible_sync(&scull_w_wait);
    return 0;
}

struct file_operations scull_wusr_fops = {
    .owner = THIS_MODULE,
    .read = scull_read,
    .write = scull_write,
    .open = scull_w_open,
    .release = scull_w_release,
};

struct scull_listitem {
    struct scull_dev device;
    dev_t key;
    struct list_head list;
};

static LIST_HEAD(scull_c_list);
DEFINE_SPINLOCK(scull_c_lock);

static struct scull_dev scull_c_device; 

static struct scull_dev *scull_c_lookfor_device(dev_t key) {
    struct scull_listitem *lptr;

    list_for_each_entry(lptr,&scull_c_list,list)
    {
        if(lptr->key == key)
            return &(lptr->device);
    }

    lptr = kmalloc(sizeof(struct scull_listitem),GFP_KERNEL);
    if(!lptr)
        return NULL;

    memset(lptr,0,sizeof(struct scull_listitem));
    lptr->key = key;
    scull_trim(&(lptr->device));
    sema_init(&(lptr->device.sem),1);

    list_add(&lptr->list,&scull_c_list);
    return &(lptr->device);

}

static  int scull_c_open(struct inode *inode,struct file *filp)
{
    struct scull_dev *dev;
    dev_t key;

    if(current->signal->tty) {
        PDEBUG("Process \"%s\" has no ctl tty\n",current->comm);
        return -EINVAL;
    }
    key = tty_devnum(current->signal->tty);
    spin_lock(&scull_c_lock);
    dev = scull_c_lookfor_device(key);
    spin_unlock(&scull_c_lock);
    if(!dev)
        return -ENOMEM;
    
    if((filp->f_flags & O_ACCMODE) == O_WRONLY)
        scull_trim(dev);
    filp->private_data = dev;

    return 0;
}

static int scull_c_release(struct inode *inode, struct file *filp)
{
	/*
	 * Nothing to do, because the device is persistent.
	 * A `real' cloned device should be freed on last close
	 */
	return 0;
}

struct file_operations scull_priv_fops = {
    .owner = THIS_MODULE,
    .read = scull_read,
    .write = scull_write,
    .open = scull_c_open,
    .release = scull_c_release,
};


static struct scull_adev_info
{
    char *name;
    struct scull_dev *sculldev;
    struct file_operations *fops;
} scull_access_devs[] = {
    {"scullsingle",&scull_s_device,&scull_sngl_fops},
    {"sculluid",&scull_u_device,&scull_user_fops},
    {"scullwuid",&scull_w_device,&scull_wusr_fops},
    {"scullpriv",&scull_c_device,&scull_priv_fops}
};
#define SCULL_N_ADEVS 4

static void scull_access_setup(dev_t devno,struct scull_adev_info *devinfo)
{
    struct scull_dev *dev = devinfo->sculldev;
    int err;

    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    sema_init(&(dev->sem),1);

    cdev_init(&dev->cdev,devinfo->fops);
    kobject_set_name(&dev->cdev.kobj,devinfo->name);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add(&dev->cdev,devno,1);

    if(err)
    {
        printk(KERN_NOTICE "Error %d adding %s \n",err,devinfo->name);
        kobject_put(&dev->cdev.kobj);
    } else
        printk(KERN_NOTICE "%s registered at %x\n",devinfo->name,devno);
}

int scull_access_init(dev_t firstdev)
{
    int result,i;

    result = register_chrdev_region(firstdev,SCULL_N_ADEVS,"sculla");
    if(result < 0) {
        printk(KERN_NOTICE "sculla: device number registration failed\n");
        return 0;
    }
    scull_a_firstdev = firstdev;

    for(i=0;i<SCULL_N_ADEVS;i++)
        scull_access_setup(firstdev+i,scull_access_devs+i);
    return SCULL_N_ADEVS;
}

void scull_access_cleanup(void)
{
    struct scull_listitem *lptr,*next;
    int i;

    for(i=0;i<SCULL_N_ADEVS;i++) {
        struct scull_dev *dev = scull_access_devs[i].sculldev;
        cdev_del(&dev->cdev);
        scull_trim(dev);
    }

    list_for_each_entry_safe(lptr,next,&scull_c_list,list) {
        list_del(&lptr->list);
        scull_trim(&(lptr->device));
        kfree(lptr);
    }

    unregister_chrdev_region(scull_a_firstdev,SCULL_N_ADEVS);
    return;
}