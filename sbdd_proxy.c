#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sbdd_priv.h"

#include <linux/moduleparam.h>
#include <linux/string.h>

#define SBDD_PROXY_MODE   (FMODE_READ | FMODE_WRITE)

static struct block_device  *__sbdd_proxy_dev = NULL;
static char                 *__sbdd_proxy_path = NULL;

static void sbdd_proxy_bio_end_io(struct bio *bio)
{
    struct bio *orig_bio = (struct bio*)bio->bi_private;

    if (bio->bi_status) {
        pr_err("error while executing proxy bio: %d", bio->bi_status);
        orig_bio->bi_status = bio->bi_status;
    }

    bio_put(bio);
    bio_endio(orig_bio);
}

blk_qc_t sbdd_proxy_make_request(struct request_queue *q, struct bio *bio)
{
    struct bio *cloned_bio;

    cloned_bio = bio_clone_fast(bio, GFP_KERNEL, &fs_bio_set);
    if (!cloned_bio) {
        pr_err("failed to clone proxy bio");
        return BLK_STS_IOERR;
    }

    bio_set_dev(cloned_bio, __sbdd_proxy_dev);
    cloned_bio->bi_end_io = sbdd_proxy_bio_end_io;
    cloned_bio->bi_private = bio;
    submit_bio(cloned_bio);

    return BLK_STS_OK;
}

int sbdd_proxy_init(const sector_t capacity)
{
    struct block_device *bdev;

    bdev = sbdd_get_bdev_by_path(__sbdd_proxy_path, SBDD_PROXY_MODE, NULL);
    if (IS_ERR(bdev))
        return PTR_ERR(bdev);

    if (get_capacity(bdev->bd_disk) < capacity) {
        pr_err("proxy disk is too small!\n");
        blkdev_put(bdev, SBDD_PROXY_MODE);

        return -EINVAL;
    }

    __sbdd_proxy_dev = bdev;
    return 0;

}

void sbdd_proxy_exit(void)
{
    if (__sbdd_proxy_dev) {
        blkdev_put(__sbdd_proxy_dev, SBDD_PROXY_MODE);
        __sbdd_proxy_dev = NULL;
    }
}

/* Set parameter for proxy path to get blkdev from */
module_param_named(proxy, __sbdd_proxy_path, charp, S_IRUGO | S_IWUSR);