#ifndef SBDD_PRIVATE_H
#define SBDD_PRIVATE_H

#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/bio.h>

#define SBDD_SECTOR_SHIFT      9
#define SBDD_SECTOR_SIZE       (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS       (1 << (20 - SBDD_SECTOR_SHIFT))

typedef int  (driver_init_fn) (const sector_t);
typedef void (driver_exit_fn) (void);

blk_qc_t sbdd_vm_make_request(struct request_queue *q, struct bio *bio);
int sbdd_vm_init(const sector_t capacity);
void sbdd_vm_exit(void);

blk_qc_t sbdd_proxy_make_request(struct request_queue *q, struct bio *bio);
int sbdd_proxy_init(const sector_t capacity);
void sbdd_proxy_exit(void);

struct block_device* sbdd_get_bdev_by_path(const char *path, fmode_t mode, void *claim);

#endif /* SBDD_PRIVATE_H */