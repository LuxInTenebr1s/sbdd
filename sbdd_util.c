#include "sbdd_priv.h"

struct block_device* sbdd_get_bdev_by_path(const char *path, fmode_t mode,
                                           void *claim)
{
    struct block_device *bdev;
    char *trimmed_path, *path_copy;

    path_copy = kmalloc(strlen(path) + 1, GFP_KERNEL);
    if (!path_copy)
        return ERR_PTR(-ENOMEM);

    strcpy(path_copy, path);
    trimmed_path = strim(path_copy);

    bdev = blkdev_get_by_path(trimmed_path, mode, claim);
    if (IS_ERR(bdev))
        pr_err("failed to acquire block device (%s): %ld\n", trimmed_path,
                                                           PTR_ERR(bdev));

    kfree(path_copy);
    return bdev;
}