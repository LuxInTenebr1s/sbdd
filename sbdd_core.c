#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sbdd_priv.h"

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/genhd.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>

#define SBDD_NAME "sbdd"

enum sbdd_mode {
	SBDD_MODE_NONE   = -1,
	SBDD_MODE_VM     = 0,
	SBDD_MODE_PROXY  = 1,
	SBDD_MODE_RAID1	 = 2,
	SBDD_MODE_MAX,
};

enum sbdd_state {
	SBDD_STATE_UNINIT,
	SBDD_STATE_CREATED,
	SBDD_STATE_DELETED,
};

struct sbdd_drv_ops {
	make_request_fn *dr_make_request;
	driver_init_fn *dr_init;
	driver_exit_fn *dr_exit;
};

struct sbdd {
	wait_queue_head_t       exitwait;
	atomic_t                deleting;
	atomic_t                refs_cnt;
	sector_t                capacity;
	struct gendisk          *gd;
	struct request_queue    *q;
	struct sbdd_drv_ops     ops;
};

static struct sbdd      __sbdd;
static int              __sbdd_major = 0;
static unsigned long    __sbdd_capacity_mib = 100;
static int              __sbdd_mode = SBDD_MODE_NONE;

static enum sbdd_state	__sbdd_state = SBDD_STATE_UNINIT;
static DEFINE_MUTEX(__sbdd_mtx);

static blk_qc_t sbdd_make_request(struct request_queue *q, struct bio *bio)
{
	blk_qc_t ret = 0;

	if (atomic_read(&__sbdd.deleting)) {
		pr_err("unable to process bio while deleting\n");
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	atomic_inc(&__sbdd.refs_cnt);

	/* Let the driver make the real request */
	ret = __sbdd.ops.dr_make_request(q, bio);

	if (atomic_dec_and_test(&__sbdd.refs_cnt))
		wake_up(&__sbdd.exitwait);

	return ret;
}

static void sbdd_set_ops(enum sbdd_mode mode, struct sbdd_drv_ops *ops)
{
	switch (mode) {
	case SBDD_MODE_PROXY:
		ops->dr_make_request = sbdd_proxy_make_request;
		ops->dr_init = sbdd_proxy_init;
		ops->dr_exit = sbdd_proxy_exit;
		break;
	case SBDD_MODE_RAID1:
		ops->dr_make_request = sbdd_raid1_make_request;
		ops->dr_init = sbdd_raid1_init;
		ops->dr_exit = sbdd_raid1_exit;
		break;
	case SBDD_MODE_VM:
	default:
		ops->dr_make_request = sbdd_vm_make_request;
		ops->dr_init = sbdd_vm_init;
		ops->dr_exit = sbdd_vm_exit;
	}
}

/*
There are no read or write operations. These operations are performed by
the request() function associated with the request queue of the disk.
*/
static struct block_device_operations const __sbdd_bdev_ops = {
	.owner = THIS_MODULE,
};

static int sbdd_create_disk(void)
{
	int ret = 0;

	/* Each time creating disk we take the possibly modified value */
	__sbdd.capacity = (sector_t)__sbdd_capacity_mib * SBDD_MIB_SECTORS;

	/* Call sbdd driver initialization function with specified capacity */
	ret = __sbdd.ops.dr_init(__sbdd.capacity);
	if (ret) {
		pr_err("failed to initialize driver: %d\n", ret);
		return ret;
	}

	init_waitqueue_head(&__sbdd.exitwait);

	pr_info("allocating queue\n");
	__sbdd.q = blk_alloc_queue(GFP_KERNEL);
	if (!__sbdd.q) {
		pr_err("call blk_alloc_queue() failed\n");
		return -EINVAL;
	}
	blk_queue_make_request(__sbdd.q, sbdd_make_request);

	/* Configure queue */
	blk_queue_logical_block_size(__sbdd.q, SBDD_SECTOR_SIZE);

	/* A disk must have at least one minor */
	pr_info("allocating disk\n");
	__sbdd.gd = alloc_disk(1);

	/* Configure gendisk */
	__sbdd.gd->queue = __sbdd.q;
	__sbdd.gd->major = __sbdd_major;
	__sbdd.gd->first_minor = 0;
	__sbdd.gd->fops = &__sbdd_bdev_ops;
	/* Represents name in /proc/partitions and /sys/block */
	scnprintf(__sbdd.gd->disk_name, DISK_NAME_LEN, SBDD_NAME);
	set_capacity(__sbdd.gd, __sbdd.capacity);

	/*
	Allocating gd does not make it available, add_disk() required.
	After this call, gd methods can be called at any time. Should not be
	called before the driver is fully initialized and ready to process reqs.
	*/
	pr_info("adding disk\n");
	add_disk(__sbdd.gd);

	return ret;
}

static void sbdd_delete(void)
{
	atomic_set(&__sbdd.deleting, 1);

	wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

	/* gd will be removed only after the last reference put */
	if (__sbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__sbdd.gd);
	}

	if (__sbdd.q) {
		pr_info("cleaning up queue\n");
		blk_cleanup_queue(__sbdd.q);
	}

	if (__sbdd.gd)
		put_disk(__sbdd.gd);

	/* Let the driver release all the resources */
	__sbdd.ops.dr_exit();

	memset(&__sbdd, 0, sizeof(struct sbdd));
}

static int sbdd_create(enum sbdd_mode mode)
{
	int ret = 0;

	if (mode != SBDD_MODE_NONE) {
	    sbdd_set_ops(mode, &__sbdd.ops);
	    ret = sbdd_create_disk();
	} else {
	    pr_info("no mode specified, doing nothing\n");
		return -EINVAL;
	}

	if (ret != 0)
		sbdd_delete();
		
	return ret;
}

static int sbdd_mode_param_set(const char *val, const struct kernel_param *kp)
{
	int ret;
	int mode;

	ret = kstrtoint(val, 10, &mode);
	if (ret != 0 || mode <= SBDD_MODE_NONE || mode >= SBDD_MODE_MAX)
	    return -EINVAL;

	(void)param_set_int(val, kp);

	/* When module is inited use the parameter to reset blk to a new mode */
	mutex_lock(&__sbdd_mtx);
	if (__sbdd_state == SBDD_STATE_UNINIT) {
		ret = 0;
		goto out;
	}

	/* Mode parameter change triggers disk reset */
	if (__sbdd_state == SBDD_STATE_CREATED)
		sbdd_delete();

	ret = sbdd_create(__sbdd_mode);
	if (ret) {
	    pr_err("changing mode failed: %d\n", ret);
		__sbdd_state = SBDD_STATE_DELETED;
	} else {
		__sbdd_state = SBDD_STATE_CREATED;
	}

out:
	mutex_unlock(&__sbdd_mtx);
	return ret;
}

/*
Note __init is for the kernel to drop this function after
initialization complete making its memory available for other uses.
There is also __initdata note, same but used for variables.
*/
static int __init sbdd_init(void)
{
	pr_info("starting initialization...\n");
	/*
	This call is somewhat redundant, but used anyways by tradition.
	The number is to be displayed in /proc/devices (0 for auto).
	*/
	pr_info("registering blkdev\n");
	__sbdd_major = register_blkdev(0, SBDD_NAME);
	if (__sbdd_major < 0) {
		pr_err("call register_blkdev() failed with %d\n", __sbdd_major);
		return -EBUSY;
	}

	/* No mutex required because no one allowed to modify this state but init */
	if (sbdd_create(__sbdd_mode) == 0)
		__sbdd_state = SBDD_STATE_CREATED;
	else
		__sbdd_state = SBDD_STATE_DELETED;

	/* In case sbdd_create() failed it can be triggered again via sysfs */
	return 0;
}

/*
Note __exit is for the compiler to place this code in a special ELF section.
Sometimes such functions are simply discarded (e.g. when module is built
directly into the kernel). There is also __exitdata note.
*/
static void __exit sbdd_exit(void)
{
	pr_info("exiting...\n");

	mutex_lock(&__sbdd_mtx);
	if (__sbdd_state == SBDD_STATE_CREATED)
		sbdd_delete();

	/* Set sbdd state to uninit so noone can access it */
	__sbdd_mode = SBDD_STATE_UNINIT;
	mutex_unlock(&__sbdd_mtx);

	if (__sbdd_major > 0) {
		pr_info("unregistering blkdev\n");
		unregister_blkdev(__sbdd_major, SBDD_NAME);
		__sbdd_major = 0;
	}

	pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Set desired capacity with insmod */
module_param_named(capacity_mib, __sbdd_capacity_mib, ulong, S_IRUGO | S_IWUSR);

static struct kernel_param_ops __sbdd_mode_param_ops = {
	.set = sbdd_mode_param_set,
	.get = param_get_int,
};

/*
Set mode of a sbdd driver. Mode value represents the number of a task in
test description text file. The callback is used to reset device in runtime
*/
module_param_cb(mode, &__sbdd_mode_param_ops, &__sbdd_mode, S_IRUGO | S_IWUSR);

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
