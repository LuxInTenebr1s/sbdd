#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sbdd_priv.h"

#include <linux/moduleparam.h>

#define SBDD_RAID_DEVICES   16
#define SBDD_RAID_FMODE     (FMODE_READ | FMODE_WRITE | FMODE_EXCL)

struct counted_bio {
    struct bio   *bio;
    atomic_t     ref_cnt;
};

struct sbdd_raid {
    char                  *path_dup;
    char                  *path[SBDD_RAID_DEVICES];
    int                   path_cnt;
    struct block_device   *bdev[SBDD_RAID_DEVICES];
    int                   bdev_cnt;
};

static struct sbdd_raid __sbdd_raid;

static char  *__sbdd_raid_param = "";
static DEFINE_MUTEX(__sbdd_mtx);

static int sbdd_raid_param_set(const char *val, const struct kernel_param *kp)
{
    char *token, *dup_ptr;

    mutex_lock(&__sbdd_mtx);
    if (__sbdd_raid.path_dup)
        kfree(__sbdd_raid.path_dup);

    __sbdd_raid.path_cnt = 0;
    __sbdd_raid.path_dup = kmalloc(strlen(val) + 1, GFP_KERNEL);
    if (!__sbdd_raid.path_dup)
        return -ENOMEM;
    strcpy(__sbdd_raid.path_dup, val);

    dup_ptr = __sbdd_raid.path_dup;
    while ((token = strsep(&dup_ptr, ","))) {
        __sbdd_raid.path[__sbdd_raid.path_cnt++] = token;
        if (__sbdd_raid.path_cnt >= SBDD_RAID_DEVICES)
            break;
    }

    param_set_charp(val, kp);

    mutex_unlock(&__sbdd_mtx);
    return 0;
}

static void sbdd_raid1_end_write_io(struct bio *bio)
{
    struct counted_bio *cbio = (struct counted_bio*)bio->bi_private;

    if (bio->bi_status) {
        pr_err("error while executing read1 write: %d\n", bio->bi_status);
        cbio->bio->bi_status = -EIO;
    }
    bio_put(bio);

    if (atomic_dec_and_test(&cbio->ref_cnt)) {
        bio_endio(cbio->bio);
        kfree(cbio);
    }
}

static void sbdd_raid1_end_read_io(struct bio *bio)
{
    struct bio *orig_bio = (struct bio*)bio->bi_private;

    if (bio->bi_status) {
        pr_err("error while executing raid1 read: %d", bio->bi_status);
        orig_bio->bi_status = bio->bi_status;
    }

    bio_put(bio);
    bio_endio(orig_bio);
}

static void sbdd_free_bios(struct bio **bio, int cnt)
{
    int i;
    for (i = 0; i < cnt; ++i)
        bio_put(bio[i]);
}

static int sbdd_raid1_write_request(struct bio *bio)
{
    int i;
    struct bio *cloned_bio[SBDD_RAID_DEVICES];

    struct counted_bio *cbio = kmalloc(sizeof(struct counted_bio), GFP_KERNEL);
    if (!cbio)
        return -ENOMEM;
    atomic_set(&cbio->ref_cnt, __sbdd_raid.bdev_cnt);
    cbio->bio = bio;

    for (i = 0; i < __sbdd_raid.bdev_cnt; ++i) {
        cloned_bio[i] = bio_clone_fast(bio, GFP_KERNEL, &fs_bio_set);
        if (!cloned_bio[i]) {
            sbdd_free_bios(cloned_bio, i);
            kfree(cbio);
            return -EBUSY;
        }

        bio_set_dev(cloned_bio[i], __sbdd_raid.bdev[i]);
        cloned_bio[i]->bi_end_io = sbdd_raid1_end_write_io;
        cloned_bio[i]->bi_private = cbio;
    }

    for (i = 0; i < __sbdd_raid.bdev_cnt; ++i)
        submit_bio(cloned_bio[i]);

    return 0;
}

static int sbdd_raid1_read_request(struct bio *bio)
{
    struct bio *cloned_bio;

    cloned_bio = bio_clone_fast(bio, GFP_KERNEL, &fs_bio_set);
    if (!cloned_bio)
        return -EBUSY;

    /* For simplicity always read the first device */
    bio_set_dev(cloned_bio, __sbdd_raid.bdev[0]);
    cloned_bio->bi_end_io = sbdd_raid1_end_read_io;
    cloned_bio->bi_private = bio;
    submit_bio(cloned_bio);

    return 0;
}

blk_qc_t sbdd_raid1_make_request(struct request_queue *q, struct bio *bio)
{
    int ret;

    if (bio_data_dir(bio)) {
        ret = sbdd_raid1_write_request(bio);
    }
    else
        ret = sbdd_raid1_read_request(bio);

    if (ret) {
        pr_err("raid1 request failed: %d\n", ret);
        bio_io_error(bio);
        return BLK_STS_IOERR;
    }
    return BLK_STS_OK;
}

static void sbdd_free_bdev(void)
{
    int i;
    for (i = 0; i < __sbdd_raid.bdev_cnt; ++i) {
        blkdev_put(__sbdd_raid.bdev[i], SBDD_RAID_FMODE);
        __sbdd_raid.bdev[i] = NULL;
    }
    __sbdd_raid.bdev_cnt = 0;
}

static int sbdd_populade_bdev(sector_t capacity)
{
    int i;

    if (!__sbdd_raid.path_cnt)
        return -EINVAL;

    for (i = 0; i < __sbdd_raid.path_cnt; ++i) {
        struct block_device *bdev;

        bdev = sbdd_get_bdev_by_path(__sbdd_raid.path[i], SBDD_RAID_FMODE, &__sbdd_raid);
        if (IS_ERR(bdev))
            return PTR_ERR(bdev);

        if (get_capacity(bdev->bd_disk) < capacity) {
            pr_err("device (%s) is too small!\n", __sbdd_raid.path[i]);
            blkdev_put(bdev, SBDD_RAID_FMODE);
            return -EINVAL;
        }

        __sbdd_raid.bdev[i] = bdev;
        ++__sbdd_raid.bdev_cnt;
    }

    return 0;
}

int sbdd_raid1_init(const sector_t capacity)
{
    int ret;

    mutex_lock(&__sbdd_mtx);
    ret = sbdd_populade_bdev(capacity);
    if (ret != 0)
        sbdd_free_bdev();
    mutex_unlock(&__sbdd_mtx);

    return ret;
}

void sbdd_raid1_exit(void)
{
    sbdd_free_bdev();

    mutex_lock(&__sbdd_mtx);
    if (__sbdd_raid.path_dup)
        kfree(__sbdd_raid.path_dup);
 
    memset(&__sbdd_raid, 0, sizeof(struct sbdd_raid));
    mutex_unlock(&__sbdd_mtx);
}

static struct kernel_param_ops __sbdd_raid_param_ops = {
    .set = sbdd_raid_param_set,
    .get = param_get_charp,
};

/* Set raid paths parameter to get blk devices */
module_param_cb(raid, &__sbdd_raid_param_ops, &__sbdd_raid_param, S_IRUGO | S_IWUSR);