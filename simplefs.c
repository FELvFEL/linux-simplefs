#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/build_bug.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "simplefs.h"

static char *disk_name = "";
static unsigned long sb_first_sector = 0;
static unsigned long sb_second_sector = 128;
static unsigned int max_filename_len = 16;
static unsigned int max_file_sectors = 1;

#define SIMPLEFS_FNV_OFFSET 2166136261U
#define SIMPLEFS_FNV_PRIME 16777619U

module_param(disk_name, charp, 0444);
module_param(sb_first_sector, ulong, 0444);
module_param(sb_second_sector, ulong, 0444);
module_param(max_filename_len, uint, 0444);
module_param(max_file_sectors, uint, 0444);

static const struct super_operations simplefs_sops;
static const struct inode_operations simplefs_dir_iops;
static const struct file_operations simplefs_dir_fops;
static const struct file_operations simplefs_file_fops;

static u32 simplefs_hash_update(u32 hash, const void *data, size_t len)
{
	const u8 *p = data;

	while (len--) {
		hash *= SIMPLEFS_FNV_PRIME;
		hash ^= *p++;
	}

	return hash;
}

static u32 simplefs_hash(const void *data, size_t len)
{
	return simplefs_hash_update(SIMPLEFS_FNV_OFFSET, data, len);
}

static u32 simplefs_checksum(struct simplefs_super_disk *disk_sb)
{
	__le32 old_checksum = disk_sb->checksum;
	u32 checksum;

	disk_sb->checksum = 0;
	checksum = simplefs_hash(disk_sb, sizeof(*disk_sb));
	disk_sb->checksum = old_checksum;

	return checksum;
}

static void simplefs_get_sb_sectors(u64 *first, u64 *second)
{
	*first = sb_first_sector;
	*second = sb_second_sector;

	if (*first > *second)
		swap(*first, *second);
}

static u64 simplefs_count_region_files(u64 start, u64 end, u32 file_sectors)
{
	if (end <= start)
		return 0;

	return (end - start) / file_sectors;
}

static u64 simplefs_count_files(u64 disk_sectors, u32 file_sectors)
{
	u64 first;
	u64 second;

	simplefs_get_sb_sectors(&first, &second);

	return simplefs_count_region_files(0, first, file_sectors) +
	       simplefs_count_region_files(first + 1, second, file_sectors) +
	       simplefs_count_region_files(second + 1, disk_sectors,
					   file_sectors);
}

static bool simplefs_file_start(struct simplefs_sb_info *sbi, u64 index,
				sector_t *start_sector)
{
	u64 first;
	u64 second;
	u64 region_start;
	u64 region_end;
	u64 count;

	simplefs_get_sb_sectors(&first, &second);

	region_start = 0;
	region_end = first;
	count = simplefs_count_region_files(region_start, region_end,
					    sbi->file_sectors);
	if (index < count) {
		*start_sector = region_start + index * sbi->file_sectors;
		return true;
	}
	index -= count;

	region_start = first + 1;
	region_end = second;
	count = simplefs_count_region_files(region_start, region_end,
					    sbi->file_sectors);
	if (index < count) {
		*start_sector = region_start + index * sbi->file_sectors;
		return true;
	}
	index -= count;

	region_start = second + 1;
	region_end = sbi->disk_sectors;
	count = simplefs_count_region_files(region_start, region_end,
					    sbi->file_sectors);
	if (index < count) {
		*start_sector = region_start + index * sbi->file_sectors;
		return true;
	}

	return false;
}

static int simplefs_file_info(struct simplefs_sb_info *sbi, u64 index,
			      struct simplefs_file_info *info)
{
	int len;

	if (index >= sbi->file_count)
		return -ENOENT;

	memset(info, 0, sizeof(*info));
	len = snprintf(info->name, sizeof(info->name), "f%llu",
		       (unsigned long long)index + 1);
	if (len < 0 || len > sbi->max_name_len)
		return -ENAMETOOLONG;

	if (!simplefs_file_start(sbi, index, &info->start_sector))
		return -EUCLEAN;

	info->sectors = sbi->file_sectors;
	info->ino = index + 2;
	return 0;
}

static bool simplefs_parse_file_index(struct simplefs_sb_info *sbi,
				      const unsigned char *name, u32 len,
				      u64 *index)
{
	u64 value = 0;
	u32 i;

	if (len < 2 || len > sbi->max_name_len || name[0] != 'f' ||
	    name[1] == '0')
		return false;

	for (i = 1; i < len; i++) {
		u8 digit;

		if (name[i] < '0' || name[i] > '9')
			return false;

		digit = name[i] - '0';
		if (value > (U64_MAX - digit) / 10)
			return false;
		value = value * 10 + digit;
	}

	if (!value || value > sbi->file_count)
		return false;

	*index = value - 1;
	return true;
}

static int simplefs_validate_files(struct simplefs_sb_info *sbi)
{
	struct simplefs_file_info info;
	u64 expected_count;

	if (!sbi->file_sectors || sbi->max_name_len < 2 ||
	    sbi->max_name_len > NAME_MAX)
		return -EINVAL;

	expected_count = simplefs_count_files(sbi->disk_sectors,
					      sbi->file_sectors);
	if (sbi->file_count != expected_count)
		return -EUCLEAN;

	if (!sbi->file_count)
		return 0;

	return simplefs_file_info(sbi, sbi->file_count - 1, &info);
}

static int simplefs_read_super_copy(struct super_block *sb, sector_t sector,
				    struct simplefs_super_disk *disk_sb)
{
	struct buffer_head *bh = sb_bread(sb, sector);

	if (!bh)
		return -EIO;

	memcpy(disk_sb, bh->b_data, sizeof(*disk_sb));
	brelse(bh);

	if (le32_to_cpu(disk_sb->magic) != SIMPLEFS_MAGIC)
		return 0;

	if (le32_to_cpu(disk_sb->checksum) != simplefs_checksum(disk_sb))
		return -EUCLEAN;

	if (le64_to_cpu(disk_sb->sb_first_sector) != sb_first_sector ||
	    le64_to_cpu(disk_sb->sb_second_sector) != sb_second_sector)
		return -EINVAL;

	return 1;
}

static int simplefs_write_super_copy(struct super_block *sb, sector_t sector,
				     struct simplefs_super_disk *disk_sb)
{
	struct buffer_head *bh = sb_bread(sb, sector);

	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
	memcpy(bh->b_data, disk_sb, sizeof(*disk_sb));
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static void simplefs_make_super(struct simplefs_super_disk *disk_sb,
				u64 disk_sectors)
{
	memset(disk_sb, 0, sizeof(*disk_sb));
	disk_sb->magic = cpu_to_le32(SIMPLEFS_MAGIC);
	disk_sb->max_name_len = cpu_to_le32(max_filename_len);
	disk_sb->file_sectors = cpu_to_le32(max_file_sectors);
	disk_sb->disk_sectors = cpu_to_le64(disk_sectors);
	disk_sb->sb_first_sector = cpu_to_le64(sb_first_sector);
	disk_sb->sb_second_sector = cpu_to_le64(sb_second_sector);
	disk_sb->file_count = cpu_to_le64(simplefs_count_files(disk_sectors,
							       max_file_sectors));
	disk_sb->checksum = cpu_to_le32(simplefs_checksum(disk_sb));
}

static int simplefs_load_super(struct super_block *sb,
			       struct simplefs_sb_info *sbi, u64 disk_sectors)
{
	struct simplefs_super_disk *second;
	int first_state;
	int second_state;
	int ret;

	second = kmalloc(sizeof(*second), GFP_KERNEL);
	if (!second)
		return -ENOMEM;

	first_state = simplefs_read_super_copy(sb, sb_first_sector,
					       &sbi->disk_super);
	second_state = simplefs_read_super_copy(sb, sb_second_sector, second);

	if (first_state < 0 && second_state < 0) {
		ret = first_state;
		goto out;
	}

	if (first_state <= 0) {
		if (second_state > 0)
			sbi->disk_super = *second;
		else if (first_state == 0 && second_state == 0)
			simplefs_make_super(&sbi->disk_super, disk_sectors);
		else {
			ret = first_state < 0 ? first_state : second_state;
			goto out;
		}
	}

	ret = simplefs_write_super_copy(sb, sb_first_sector, &sbi->disk_super);
	if (ret)
		goto out;

	ret = simplefs_write_super_copy(sb, sb_second_sector, &sbi->disk_super);
	if (ret)
		goto out;

	sbi->file_count = le64_to_cpu(sbi->disk_super.file_count);
	sbi->disk_sectors = le64_to_cpu(sbi->disk_super.disk_sectors);
	sbi->file_sectors = le32_to_cpu(sbi->disk_super.file_sectors);
	sbi->max_name_len = le32_to_cpu(sbi->disk_super.max_name_len);

	ret = simplefs_validate_files(sbi);

out:
	kfree(second);
	return ret;
}

static struct inode *simplefs_get_inode(struct super_block *sb, umode_t mode,
					u64 ino)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = ino;
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();

	if (S_ISDIR(mode)) {
		inode->i_op = &simplefs_dir_iops;
		inode->i_fop = &simplefs_dir_fops;
		set_nlink(inode, 2);
	} else {
		struct simplefs_sb_info *sbi = sb->s_fs_info;

		inode->i_fop = &simplefs_file_fops;
		inode->i_size = (loff_t)sbi->file_sectors * SIMPLEFS_SECTOR_SIZE;
		set_nlink(inode, 1);
	}

	return inode;
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct simplefs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct simplefs_file_info info;
	struct inode *inode;
	u64 index;
	int ret;

	if (sbi->erased ||
	    !simplefs_parse_file_index(sbi, dentry->d_name.name,
				       dentry->d_name.len, &index)) {
		d_add(dentry, NULL);
		return NULL;
	}

	ret = simplefs_file_info(sbi, index, &info);
	if (ret)
		return ERR_PTR(ret);

	inode = simplefs_get_inode(dir->i_sb, S_IFREG | 0666, info.ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	d_add(dentry, inode);
	return NULL;
}

static int simplefs_iterate(struct file *file, struct dir_context *ctx)
{
	struct simplefs_sb_info *sbi = file_inode(file)->i_sb->s_fs_info;
	u64 i;

	if (!dir_emit_dots(file, ctx))
		return 0;

	if (sbi->erased)
		return 0;

	if (ctx->pos < 2)
		ctx->pos = 2;

	for (i = ctx->pos - 2; i < sbi->file_count; i++) {
		struct simplefs_file_info info;
		int ret;

		ret = simplefs_file_info(sbi, i, &info);
		if (ret)
			return ret;

		if (!dir_emit(ctx, info.name, strlen(info.name), info.ino,
			      DT_REG))
			return 0;

		ctx->pos = i + 3;
	}

	return 0;
}

static ssize_t simplefs_read(struct file *file, char __user *buf, size_t len,
			     loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct simplefs_file_info info;
	loff_t file_size;
	loff_t available;
	size_t copied = 0;
	int ret;

	if (sbi->erased)
		return -ENODEV;
	if (*ppos < 0)
		return -EINVAL;
	if (inode->i_ino < 2)
		return -EUCLEAN;

	ret = simplefs_file_info(sbi, inode->i_ino - 2, &info);
	if (ret)
		return ret;

	file_size = (loff_t)info.sectors * SIMPLEFS_SECTOR_SIZE;
	if (*ppos >= file_size)
		return 0;

	available = file_size - *ppos;
	if (len > (size_t)available)
		len = (size_t)available;

	while (copied < len) {
		struct buffer_head *bh;
		loff_t pos = *ppos;
		sector_t sector = info.start_sector + pos / SIMPLEFS_SECTOR_SIZE;
		unsigned int offset = pos % SIMPLEFS_SECTOR_SIZE;
		size_t part = min_t(size_t, len - copied,
				    SIMPLEFS_SECTOR_SIZE - offset);

		bh = sb_bread(inode->i_sb, sector);
		if (!bh)
			return copied ? copied : -EIO;

		if (copy_to_user(buf + copied, bh->b_data + offset, part)) {
			brelse(bh);
			return copied ? copied : -EFAULT;
		}

		brelse(bh);
		*ppos += part;
		copied += part;
	}

	return copied;
}

static ssize_t simplefs_write(struct file *file, const char __user *buf,
			      size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct simplefs_file_info info;
	loff_t file_size;
	loff_t available;
	size_t copied = 0;
	int ret;

	if (sbi->erased)
		return -ENODEV;
	if (!len)
		return 0;
	if (*ppos < 0)
		return -EINVAL;
	if (inode->i_ino < 2)
		return -EUCLEAN;

	ret = simplefs_file_info(sbi, inode->i_ino - 2, &info);
	if (ret)
		return ret;

	file_size = (loff_t)info.sectors * SIMPLEFS_SECTOR_SIZE;
	if (*ppos >= file_size)
		return -ENOSPC;

	available = file_size - *ppos;
	if (len > (size_t)available)
		len = (size_t)available;

	while (copied < len) {
		struct buffer_head *bh;
		loff_t pos = *ppos;
		sector_t sector = info.start_sector + pos / SIMPLEFS_SECTOR_SIZE;
		unsigned int offset = pos % SIMPLEFS_SECTOR_SIZE;
		size_t part = min_t(size_t, len - copied,
				    SIMPLEFS_SECTOR_SIZE - offset);

		bh = sb_bread(inode->i_sb, sector);
		if (!bh)
			return copied ? copied : -EIO;

		lock_buffer(bh);
		if (copy_from_user(bh->b_data + offset, buf + copied, part)) {
			unlock_buffer(bh);
			brelse(bh);
			return copied ? copied : -EFAULT;
		}

		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		*ppos += part;
		copied += part;
	}

	return copied;
}

static int simplefs_zero_sector(struct super_block *sb, sector_t sector)
{
	struct buffer_head *bh = sb_bread(sb, sector);

	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static int simplefs_zero_file(struct super_block *sb,
			      struct simplefs_file_info *info)
{
	u32 i;
	int ret;

	for (i = 0; i < info->sectors; i++) {
		ret = simplefs_zero_sector(sb, info->start_sector + i);
		if (ret)
			return ret;
	}

	return 0;
}

static int simplefs_zero_files(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	u64 i;
	int ret;

	for (i = 0; i < sbi->file_count; i++) {
		struct simplefs_file_info info;

		ret = simplefs_file_info(sbi, i, &info);
		if (ret)
			return ret;

		ret = simplefs_zero_file(sb, &info);
		if (ret)
			return ret;
	}

	return 0;
}

static int simplefs_hash_file(struct super_block *sb,
			      struct simplefs_file_info *info, u32 *hash)
{
	u32 i;

	*hash = SIMPLEFS_FNV_OFFSET;

	for (i = 0; i < info->sectors; i++) {
		struct buffer_head *bh;

		bh = sb_bread(sb, info->start_sector + i);
		if (!bh)
			return -EIO;

		*hash = simplefs_hash_update(*hash, bh->b_data,
					     SIMPLEFS_SECTOR_SIZE);
		brelse(bh);
	}

	return 0;
}

static long simplefs_ioctl_hashes(struct super_block *sb, unsigned long arg)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct simplefs_ioctl_hash_entry __user *entries;
	struct simplefs_ioctl_hashes req;
	u64 i;
	int ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	entries = (struct simplefs_ioctl_hash_entry __user *)(unsigned long)req.entries;
	req.count = sbi->file_count > U32_MAX ? U32_MAX : (u32)sbi->file_count;

	for (i = 0; i < sbi->file_count && i < req.capacity; i++) {
		struct simplefs_ioctl_hash_entry entry;
		struct simplefs_file_info info;

		ret = simplefs_file_info(sbi, i, &info);
		if (ret)
			return ret;

		memset(&entry, 0, sizeof(entry));
		snprintf(entry.name, sizeof(entry.name), "%s", info.name);
		entry.start_sector = info.start_sector;
		entry.sectors = info.sectors;

		ret = simplefs_hash_file(sb, &info, &entry.hash);
		if (ret)
			return ret;

		if (copy_to_user(&entries[i], &entry, sizeof(entry)))
			return -EFAULT;
	}

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long simplefs_ioctl_mapping(struct super_block *sb, unsigned long arg)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct simplefs_ioctl_mapping map;
	struct simplefs_file_info info;
	size_t len;
	u64 index;
	int ret;

	if (copy_from_user(&map, (void __user *)arg, sizeof(map)))
		return -EFAULT;

	map.name[NAME_MAX] = '\0';
	len = strnlen(map.name, sizeof(map.name));
	if (!simplefs_parse_file_index(sbi, (unsigned char *)map.name, len,
				       &index))
		return -ENOENT;

	ret = simplefs_file_info(sbi, index, &info);
	if (ret)
		return ret;

	map.start_sector = info.start_sector;
	map.sectors = info.sectors;
	map.reserved = 0;

	if (copy_to_user((void __user *)arg, &map, sizeof(map)))
		return -EFAULT;

	return 0;
}

static long simplefs_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	int ret;

	if (sbi->erased && cmd != SIMPLEFS_IOCTL_ERASE)
		return -ENODEV;

	switch (cmd) {
	case SIMPLEFS_IOCTL_ZERO:
		return simplefs_zero_files(sb);
	case SIMPLEFS_IOCTL_ERASE:
		if (sbi->erased)
			return -ENODEV;

		ret = simplefs_zero_files(sb);
		if (ret)
			return ret;

		ret = simplefs_zero_sector(sb, le64_to_cpu(sbi->disk_super.sb_first_sector));
		if (ret)
			return ret;

		ret = simplefs_zero_sector(sb,
					   le64_to_cpu(sbi->disk_super.sb_second_sector));
		if (ret)
			return ret;

		sbi->erased = true;
		return 0;
	case SIMPLEFS_IOCTL_HASHES:
		return simplefs_ioctl_hashes(sb, arg);
	case SIMPLEFS_IOCTL_MAPPING:
		return simplefs_ioctl_mapping(sb, arg);
	default:
		return -ENOTTY;
	}
}

static void simplefs_put_super(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;

	kfree(sbi);
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct simplefs_sb_info *sbi;
	struct inode *root;
	u64 disk_sectors;
	int ret;

	if (sb_first_sector == sb_second_sector || !max_file_sectors ||
	    max_filename_len < 2 || max_filename_len > NAME_MAX)
		return -EINVAL;

	if (!sb_set_blocksize(sb, SIMPLEFS_SECTOR_SIZE))
		return -EINVAL;

	disk_sectors = bdev_nr_bytes(sb->s_bdev) / SIMPLEFS_SECTOR_SIZE;
	if (sb_first_sector >= disk_sectors || sb_second_sector >= disk_sectors)
		return -EINVAL;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_magic = SIMPLEFS_MAGIC;
	sb->s_op = &simplefs_sops;
	sb->s_fs_info = sbi;

	ret = simplefs_load_super(sb, sbi, disk_sectors);
	if (ret)
		goto fail;

	root = simplefs_get_inode(sb, S_IFDIR | 0755, 1);
	if (!root) {
		ret = -ENOMEM;
		goto fail;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto fail;
	}

	return 0;

fail:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *type, int flags,
				     const char *dev_name, void *data)
{
	if (disk_name && disk_name[0])
		dev_name = disk_name;

	return mount_bdev(type, flags, dev_name, data, simplefs_fill_super);
}

static const struct super_operations simplefs_sops = {
	.put_super = simplefs_put_super,
	.drop_inode = generic_delete_inode,
};

static const struct inode_operations simplefs_dir_iops = {
	.lookup = simplefs_lookup,
};

static const struct file_operations simplefs_dir_fops = {
	.owner = THIS_MODULE,
	.iterate_shared = simplefs_iterate,
	.unlocked_ioctl = simplefs_ioctl,
	.llseek = generic_file_llseek,
};

static const struct file_operations simplefs_file_fops = {
	.owner = THIS_MODULE,
	.read = simplefs_read,
	.write = simplefs_write,
	.unlocked_ioctl = simplefs_ioctl,
	.llseek = generic_file_llseek,
};

static struct file_system_type simplefs_type = {
	.owner = THIS_MODULE,
	.name = SIMPLEFS_NAME,
	.mount = simplefs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
	BUILD_BUG_ON(sizeof(struct simplefs_super_disk) != SIMPLEFS_SECTOR_SIZE);
	return register_filesystem(&simplefs_type);
}

static void __exit simplefs_exit(void)
{
	unregister_filesystem(&simplefs_type);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SimpleFS");
