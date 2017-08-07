#ifndef SCULL_H
#define SCULL_H

extern int scull_quantum;
extern int scull_qset;

struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data;
	int quantum;
	int qset;
	unsigned long size;
	unsigned int access_key;
	struct semaphore sem;
	struct cdev cdev;
};

ssize_t scull_read(struct file *filp, char __user *buf,size_t count, loff_t *offp);
ssize_t scull_write(struct file *filp,const char __user *buf,size_t count,loff_t *f_pos);
int scull_trim(struct scull_dev *dev);

int scull_p_init(dev_t first_devno);
void scull_p_exit(void);
int scull_access_init(dev_t firstdev);
void scull_access_cleanup(void);

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

#endif