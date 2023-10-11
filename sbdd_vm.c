#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sbdd_priv.h"

#include <linux/vmalloc.h>
#include <linux/bvec.h>
#include <linux/spinlock_types.h>
#include <linux/errno.h>

struct sbdd_vm_drv {
	spinlock_t  datalock;
	u8          *data;
    sector_t    capacity;
};

static struct sbdd_vm_drv __sbdd_drv;

static sector_t sbdd_xfer(struct bio_vec* bvec, sector_t pos, int dir)
{
	void *buff = page_address(bvec->bv_page) + bvec->bv_offset;
	sector_t len = bvec->bv_len >> SBDD_SECTOR_SHIFT;
	size_t offset;
	size_t nbytes;

	if (pos + len > __sbdd_drv.capacity)
		len = __sbdd_drv.capacity - pos;

	offset = pos << SBDD_SECTOR_SHIFT;
	nbytes = len << SBDD_SECTOR_SHIFT;

	spin_lock(&__sbdd_drv.datalock);

	if (dir)
		memcpy(__sbdd_drv.data + offset, buff, nbytes);
	else
		memcpy(buff, __sbdd_drv.data + offset, nbytes);

	spin_unlock(&__sbdd_drv.datalock);

	pr_debug("pos=%6llu len=%4llu %s\n", pos, len, dir ? "written" : "read");

	return len;
}

static void sbdd_xfer_bio(struct bio *bio)
{
    struct bvec_iter iter;
    struct bio_vec bvec;
    int dir = bio_data_dir(bio);
    sector_t pos = bio->bi_iter.bi_sector;

    bio_for_each_segment(bvec, bio, iter)
        pos += sbdd_xfer(&bvec, pos, dir);
}

blk_qc_t sbdd_vm_make_request(struct request_queue *q, struct bio *bio)
{
	sbdd_xfer_bio(bio);
	bio_endio(bio);

    return BLK_STS_OK;
}

int sbdd_vm_init(const sector_t capacity)
{
	__sbdd_drv.data = vzalloc(capacity << SBDD_SECTOR_SHIFT);
	if (!__sbdd_drv.data) {
		pr_err("unable to alloc data\n");
		return -ENOMEM;
	}
    __sbdd_drv.capacity = capacity;
	spin_lock_init(&__sbdd_drv.datalock);

    return 0;
}

void sbdd_vm_exit(void)
{
	if (__sbdd_drv.data) {
		pr_info("freeing data\n");
		vfree(__sbdd_drv.data);
	}

    memset(&__sbdd_drv, 0, sizeof(struct sbdd_vm_drv));
}
