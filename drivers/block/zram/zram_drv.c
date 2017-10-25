/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#define KMSG_COMPONENT "zram"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#ifdef CONFIG_ZRAM_DEBUG
#define DEBUG
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>

#include "zram_drv.h"

/* Globals */
static int zram_major;
static struct zram *zram_devices;
static const char *default_compressor = "lzo";

/* Module params (documentation at end) */
static unsigned int num_devices = 1;

#define ZRAM_ATTR_RO(name)						\
static ssize_t zram_attr_##name##_show(struct device *d,		\
				struct device_attribute *attr, char *b)	\
{									\
	struct zram *zram = dev_to_zram(d);				\
	return scnprintf(b, PAGE_SIZE, "%llu\n",			\
		(u64)atomic64_read(&zram->stats.name));			\
}									\
static struct device_attribute dev_attr_##name =			\
	__ATTR(name, S_IRUGO, zram_attr_##name##_show, NULL);

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", zram->disksize);
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = init_done(zram);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t orig_data_size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return scnprintf(buf, PAGE_SIZE, "%llu\n",
		(u64)(atomic64_read(&zram->stats.pages_stored)) << PAGE_SHIFT);
}

static ssize_t mem_used_total_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val = 0;
	struct zram *zram = dev_to_zram(dev);
	struct zram *meta = zram->meta;

	down_read(&zram->init_lock);
	if (init_done(zram))
		val = zs_get_total_size_bytes(meta->mem_pool);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static ssize_t max_comp_streams_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = zram->max_comp_streams;
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t max_comp_streams_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int num;
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = kstrtoint(buf, 0, &num);
	if (ret < 0)
		return ret;
	if (num < 1)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		if (!zcomp_set_max_streams(zram->comp, num)) {
			pr_info("Cannot change max compression streams\n");
			ret = -EINVAL;
			goto out;
		}
	}

	zram->max_comp_streams = num;
	ret = len;
out:
	up_write(&zram->init_lock);
	return ret;
}

static ssize_t comp_algorithm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t sz;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->compressor, buf);
	up_read(&zram->init_lock);

	return sz;
}

static ssize_t comp_algorithm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	down_write(&zram->init_lock);
	if (init_done(zram)) {
		up_write(&zram->init_lock);
		pr_info("Can't change algorithm for initialized device\n");
		return -EBUSY;
	}
	strlcpy(zram->compressor, buf, sizeof(zram->compressor));
	up_write(&zram->init_lock);
	return len;
}

/* flag operations needs meta->tb_lock */
static int zram_test_flag(struct zram *meta, u32 index,
			enum zram_pageflags flag)
{
	return meta->table[index].value & BIT(flag);
}

static void zram_set_flag(struct zram *meta, u32 index,
			enum zram_pageflags flag)
{
	meta->table[index].value |= BIT(flag);
}

static void zram_clear_flag(struct zram *meta, u32 index,
			enum zram_pageflags flag)
{
	meta->table[index].value &= ~BIT(flag);
}

static inline int is_partial_io(struct bio_vec *bvec)
{
	return bvec->bv_len != PAGE_SIZE;
}

/*
 * Check if request is within bounds and aligned on zram logical blocks.
 */
static inline bool valid_io_request(struct zram *zram,
		sector_t start, unsigned int size)
{
	u64 end, bound;

	/* unaligned request */
	if (unlikely(start &
		     (ZRAM_SECTOR_PER_LOGICAL_BLOCK - 1)))
		return false;
	if (unlikely(size & (ZRAM_LOGICAL_BLOCK_SIZE - 1)))
		return false;

	end = start + (size >> SECTOR_SHIFT);
	bound = zram->disksize >> SECTOR_SHIFT;
	/* out of range range */
	if (unlikely(start >= bound || end > bound || start > end))
		return false;

	/* I/O request is valid */
	return true;
}

static void zram_meta_free(struct zram *meta)
{
	zs_destroy_pool(meta->mem_pool);
	vfree(meta->table);
	kfree(meta);
}

static bool zram_meta_alloc(struct zram *zram, u64 disksize)
{
	size_t num_pages;

	num_pages = disksize >> PAGE_SHIFT;
	zram->table = vzalloc(num_pages * sizeof(*zram->table));
	if (!zram->table)
		return false;

	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool) {
		vfree(zram->table);
		return false;
	}

	return true;
}

static void update_position(u32 *index, int *offset, struct bio_vec *bvec)
{
	if (*offset + bvec->bv_len >= PAGE_SIZE)
		(*index)++;
	*offset = (*offset + bvec->bv_len) % PAGE_SIZE;
}

static int page_zero_filled(void *ptr)
{
	unsigned int pos;
	unsigned long *page;

	page = (unsigned long *)ptr;

	for (pos = 0; pos != PAGE_SIZE / sizeof(*page); pos++) {
		if (page[pos])
			return 0;
	}

	return 1;
}

static void handle_zero_page(struct bio_vec *bvec)
{
	struct page *page = bvec->bv_page;
	void *user_mem;

	user_mem = kmap_atomic(page);
	if (is_partial_io(bvec))
		memset(user_mem + bvec->bv_offset, 0, bvec->bv_len);
	else
		clear_page(user_mem);
	kunmap_atomic(user_mem);

	flush_dcache_page(page);
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit_spinlock to
 * indicate this index entry is accessing.
 */
static void zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle = zram_get_handle(zram, index);

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		zram_set_element(zram, index, 0);
		atomic64_dec(&zram->stats.same_pages);
		atomic64_dec(&zram->stats.pages_stored);
		return;
	}

	if (!handle)
		return;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			&zram->stats.compr_data_size);
	atomic64_dec(&zram->stats.pages_stored);

	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
}

static int zram_decompress_page(struct zram *zram, struct page *page, u32 index)
{
	int ret;
	unsigned long handle;
	unsigned int size;
	void *src, *dst;

	if (zram_same_page_read(zram, index, page, 0, PAGE_SIZE))
		return 0;

	zram_slot_lock(zram, index);
	handle = zram_get_handle(zram, index);
	size = zram_get_obj_size(zram, index);

	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	if (size == PAGE_SIZE) {
		dst = kmap_atomic(page);
		memcpy(dst, src, PAGE_SIZE);
		kunmap_atomic(dst);
		ret = 0;
	} else {
		struct zcomp_strm *zstrm = zcomp_stream_get(zram->comp);

		dst = kmap_atomic(page);
		ret = zcomp_decompress(zstrm, src, size, dst);
		kunmap_atomic(dst);
		zcomp_stream_put(zram->comp);
	}
	zs_unmap_object(zram->mem_pool, handle);
	zram_slot_unlock(zram, index);

	/* Should NEVER happen. Return bio error if it does. */
	if (unlikely(ret))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

	return ret;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
				u32 index, int offset)
{
	int ret;
	struct page *page;

	page = bvec->bv_page;
	if (is_partial_io(bvec)) {
		/* Use a temporary buffer to decompress the page */
		page = alloc_page(GFP_NOIO|__GFP_HIGHMEM);
		if (!page)
			return -ENOMEM;
	}

	ret = zram_decompress_page(zram, page, index);
	if (unlikely(ret))
		goto out;

	if (is_partial_io(bvec)) {
		void *dst = kmap_atomic(bvec->bv_page);
		void *src = kmap_atomic(page);

		memcpy(dst + bvec->bv_offset, src + offset, bvec->bv_len);
		kunmap_atomic(src);
		kunmap_atomic(dst);
	}
out:
	if (is_partial_io(bvec))
		__free_page(page);

	return ret;
}

static int zram_bvec_rw(struct zram *zram, struct bio_vec *bvec, u32 index,
			int offset, bool is_write)
{
	unsigned long start_time = jiffies;
	int rw_acct = is_write ? REQ_OP_WRITE : REQ_OP_READ;
	int ret;

	generic_start_io_acct(rw_acct, bvec->bv_len >> SECTOR_SHIFT,
			&zram->disk->part0);

	if (!is_write) {
		atomic64_inc(&zram->stats.num_reads);
		ret = zram_bvec_read(zram, bvec, index, offset);
		flush_dcache_page(bvec->bv_page);
	} else {
		atomic64_inc(&zram->stats.num_writes);
		ret = zram_bvec_write(zram, bvec, index, offset);
	}

	generic_end_io_acct(rw_acct, &zram->disk->part0, start_time);

	if (unlikely(ret)) {
		if (!is_write)
			atomic64_inc(&zram->stats.failed_reads);
		else
			atomic64_inc(&zram->stats.failed_writes);
	}

	return ret;
}

/*
 * zram_bio_discard - handler on discard request
 * @index: physical block index in PAGE_SIZE units
 * @offset: byte offset within physical block
 */
static void zram_bio_discard(struct zram *zram, u32 index,
			     int offset, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */

	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			return;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_slot_lock(zram, index);
		zram_free_page(zram, index);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.notify_free);
		index++;
		n -= PAGE_SIZE;
	}
}

static void zram_reset_device(struct zram *zram, bool reset_capacity)
{
	struct zcomp *comp;
	u64 disksize;

	down_write(&zram->init_lock);

	if (!init_done(zram)) {
		up_write(&zram->init_lock);
		return;
	}

	comp = zram->comp;
	disksize = zram->disksize;
	zram->disksize = 0;

	set_capacity(zram->disk, 0);
	part_stat_set_all(&zram->disk->part0, 0);

	up_write(&zram->init_lock);
	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, disksize);
	memset(&zram->stats, 0, sizeof(zram->stats));
	zcomp_destroy(comp);

	if (reset_capacity)
		set_capacity(zram->disk, 0);
}

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize;
	struct zcomp *comp;
	struct zram *meta;
	struct zram *zram = dev_to_zram(dev);
	int err;

	disksize = memparse(buf, NULL);
	if (!disksize)
		return -EINVAL;

	disksize = PAGE_ALIGN(disksize);
	meta = zram_meta_alloc(disksize);
	if (!meta)
		return -ENOMEM;

	comp = zcomp_create(zram->compressor, zram->max_comp_streams);
	if (IS_ERR(comp)) {
		pr_info("Cannot initialise %s compressing backend\n",
				zram->compressor);
		err = PTR_ERR(comp);
		goto out_free_meta;
	}

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_destroy_comp;
	}

	zram->meta = meta;
	zram->comp = comp;
	zram->disksize = disksize;
	set_capacity(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);
	return len;

out_destroy_comp:
	up_write(&zram->init_lock);
	zcomp_destroy(comp);
out_free_meta:
	zram_meta_free(meta);
	return err;
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned short do_reset;
	struct zram *zram;
	struct block_device *bdev;

	zram = dev_to_zram(dev);
	bdev = bdget_disk(zram->disk, 0);

	if (!bdev)
		return -ENOMEM;

	/* Do not reset an active device! */
	if (bdev->bd_holders) {
		ret = -EBUSY;
		goto out;
	}

	ret = kstrtou16(buf, 10, &do_reset);
	if (ret)
		goto out;

	if (!do_reset) {
		ret = -EINVAL;
		goto out;
	}

	/* Make sure all pending I/O is finished */
	fsync_bdev(bdev);
	bdput(bdev);

	zram_reset_device(zram, true);
	return len;

out:
	bdput(bdev);
	return ret;
}

static void __zram_make_request(struct zram *zram, struct bio *bio)
{

	int offset;
	u32 index;
	struct bio_vec bvec;
	struct bvec_iter iter;

	index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	offset = (bio->bi_iter.bi_sector &
		  (SECTORS_PER_PAGE - 1)) << SECTOR_SHIFT;

	switch (bio_op(bio)) {
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zram_bio_discard(zram, index, offset, bio);
		bio_endio(bio);
		return;
	default:
		break;
	}

	bio_for_each_segment(bvec, bio, iter) {
		struct bio_vec bv = bvec;
		unsigned int unwritten = bvec.bv_len;

		do {
			bv.bv_len = min_t(unsigned int, PAGE_SIZE - offset,
							unwritten);
			if (zram_bvec_rw(zram, &bv, index, offset,
					op_is_write(bio_op(bio))) < 0)
				goto out;

			bv.bv_offset += bv.bv_len;
			unwritten -= bv.bv_len;

			update_position(&index, &offset, &bv);
		} while (unwritten);
	}

	bio_endio(bio);
	return;

out:
	bio_io_error(bio);
}

/*
 * Handler function for all zram I/O requests.
 */
static blk_qc_t zram_make_request(struct request_queue *queue, struct bio *bio)
{
	struct zram *zram = queue->queuedata;
	if (!valid_io_request(zram, bio->bi_iter.bi_sector,
					bio->bi_iter.bi_size)) {
		atomic64_inc(&zram->stats.invalid_io);
		goto error;
	}

	__zram_make_request(zram, bio);
	return BLK_QC_T_NONE;

error:
	bio_io_error(bio);
	return BLK_QC_T_NONE;
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	zram_slot_lock(zram, index);
	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);
	atomic64_inc(&zram->stats.notify_free);
}

static int zram_open(struct block_device *bdev, fmode_t mode)
{
	int ret = 0;
	struct zram *zram;

	WARN_ON(!mutex_is_locked(&bdev->bd_mutex));

	zram = bdev->bd_disk->private_data;
	/* zram was claimed to reset so open request fails */
	if (zram->claim)
		ret = -EBUSY;

	return ret;
}

static const struct block_device_operations zram_devops = {
	.open = zram_open;
	.swap_slot_free_notify = zram_slot_free_notify,
	.rw_page = zram_rw_page,
	.owner = THIS_MODULE
};

static DEVICE_ATTR(disksize, S_IRUGO | S_IWUSR,
		disksize_show, disksize_store);
static DEVICE_ATTR(initstate, S_IRUGO, initstate_show, NULL);
static DEVICE_ATTR(reset, S_IWUSR, NULL, reset_store);
static DEVICE_ATTR(orig_data_size, S_IRUGO, orig_data_size_show, NULL);
static DEVICE_ATTR(mem_used_total, S_IRUGO, mem_used_total_show, NULL);
static DEVICE_ATTR(max_comp_streams, S_IRUGO | S_IWUSR,
		max_comp_streams_show, max_comp_streams_store);
static DEVICE_ATTR(comp_algorithm, S_IRUGO | S_IWUSR,
		comp_algorithm_show, comp_algorithm_store);

ZRAM_ATTR_RO(num_reads);
ZRAM_ATTR_RO(num_writes);
ZRAM_ATTR_RO(failed_reads);
ZRAM_ATTR_RO(failed_writes);
ZRAM_ATTR_RO(invalid_io);
ZRAM_ATTR_RO(notify_free);
ZRAM_ATTR_RO(zero_pages);
ZRAM_ATTR_RO(compr_data_size);

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_num_reads.attr,
	&dev_attr_num_writes.attr,
	&dev_attr_failed_reads.attr,
	&dev_attr_failed_writes.attr,
	&dev_attr_invalid_io.attr,
	&dev_attr_notify_free.attr,
	&dev_attr_zero_pages.attr,
	&dev_attr_orig_data_size.attr,
	&dev_attr_compr_data_size.attr,
	&dev_attr_mem_used_total.attr,
	&dev_attr_max_comp_streams.attr,
	&dev_attr_comp_algorithm.attr,
	NULL,
};

static struct attribute_group zram_disk_attr_group = {
	.attrs = zram_disk_attrs,
};

static int create_device(void)
{
	struct zram *zram;
	struct request_queue *queue;
	int ret, device_id;

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;
	device_id = ret;

	init_rwsem(&zram->init_lock);

	queue = blk_alloc_queue(GFP_KERNEL);
	if (!queue) {
		pr_err("Error allocating disk queue for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_idr;
	}

	blk_queue_make_request(queue, zram_make_request);

	 /* gendisk structure */
	zram->disk = alloc_disk(1);
	if (!zram->disk) {
		pr_warn("Error allocating disk structure for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_queue;
	}

	zram->disk->major = zram_major;
	zram->disk->first_minor = device_id;
	zram->disk->fops = &zram_devops;
	zram->disk->queue = queue;
	zram->disk->queue->queuedata = zram;
	zram->disk->private_data = zram;
	snprintf(zram->disk->disk_name, 16, "zram%d", device_id);

	/* Actual capacity set using syfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);
	/* zram devices sort of resembles non-rotational disks */
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, zram->disk->queue);
	queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, zram->disk->queue);
	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(zram->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(zram->disk->queue,
					ZRAM_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(zram->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(zram->disk->queue, PAGE_SIZE);
	zram->disk->queue->limits.discard_granularity = PAGE_SIZE;
	zram->disk->queue->limits.max_discard_sectors = UINT_MAX;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, zram->disk->queue);
	/*
	 * zram_bio_discard() will clear all logical blocks if logical block
	 * size is identical with physical block size(PAGE_SIZE). But if it is
	 * different, we will skip discarding some parts of logical blocks in
	 * the part of the request range which isn't aligned to physical block
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		blk_queue_max_write_zeroes_sectors(zram->disk->queue, UINT_MAX);

	add_disk(zram->disk);

	ret = sysfs_create_group(&disk_to_dev(zram->disk)->kobj,
				&zram_disk_attr_group);
	if (ret < 0) {
		pr_warn("Error creating sysfs group");
		goto out_free_disk;
	}
	strlcpy(zram->compressor, default_compressor, sizeof(zram->compressor));
	zram->max_comp_streams = 1;
	return device_id;

out_free_disk:
	del_gendisk(zram->disk);
	put_disk(zram->disk);
out_free_queue:
	blk_cleanup_queue(zram->queue);
out_free_idr:
	idr_remove(&zram_index_idr, device_id);
out_free_dev:
	kfree(zram);
	return ret;
}

static void destroy_devices(void)
{
	class_unregister(&zram_control_class);
	idr_for_each(&zram_index_idr, &zram_remove_cb, NULL);
	idr_destroy(&zram_index_idr);
	unregister_blkdev(zram_major, "zram");
	cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
}

static int __init zram_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_ZCOMP_PREPARE, "block/zram:prepare",
				      zcomp_cpu_up_prepare, zcomp_cpu_dead);
	if (ret < 0)
		return ret;

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return ret;
	}

	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		class_unregister(&zram_control_class);
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return -EBUSY;
	}

	while (num_devices != 0) {
		mutex_lock(&zram_index_mutex);
		ret = zram_add();
		mutex_unlock(&zram_index_mutex);
		if (ret < 0)
			goto out_error;
		num_devices--;
	}

	return 0;

out_error:
	destroy_devices();
	return ret;
}

static void __exit zram_exit(void)
{
	destroy_devices();
	pr_debug("Cleanup done!\n");
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of zram devices");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_DESCRIPTION("Compressed RAM Block Device");
