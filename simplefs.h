#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include <linux/types.h>
#include <linux/limits.h>
#include <linux/ioctl.h>

#define SIMPLEFS_NAME "simplefs"
#define SIMPLEFS_MAGIC 0x00534653
#define SIMPLEFS_SECTOR_SIZE 512
#define SIMPLEFS_IOCTL_MAGIC 'S'

struct simplefs_ioctl_hash_entry {
	char name[NAME_MAX + 1];
	__u64 start_sector;
	__u32 sectors;
	__u32 hash;
};

struct simplefs_ioctl_hashes {
	__u64 entries;
	__u32 capacity;
	__u32 count;
};

struct simplefs_ioctl_mapping {
	char name[NAME_MAX + 1];
	__u64 start_sector;
	__u32 sectors;
	__u32 reserved;
};

#define SIMPLEFS_IOCTL_ZERO _IO(SIMPLEFS_IOCTL_MAGIC, 1)
#define SIMPLEFS_IOCTL_ERASE _IO(SIMPLEFS_IOCTL_MAGIC, 2)
#define SIMPLEFS_IOCTL_HASHES _IOWR(SIMPLEFS_IOCTL_MAGIC, 3, struct simplefs_ioctl_hashes)
#define SIMPLEFS_IOCTL_MAPPING _IOWR(SIMPLEFS_IOCTL_MAGIC, 4, struct simplefs_ioctl_mapping)

#ifdef __KERNEL__

struct simplefs_super_disk {
	__le32 magic;
	__le32 max_name_len;
	__le32 file_sectors;
	__le32 reserved32;

	__le64 disk_sectors;
	__le64 sb_first_sector;
	__le64 sb_second_sector;
	__le64 file_count;
	__le32 checksum;

	__u8 reserved[SIMPLEFS_SECTOR_SIZE - 52];
};

struct simplefs_file_info {
	char name[NAME_MAX + 1];
	sector_t start_sector;
	u32 sectors;
	u64 ino;
};

struct simplefs_sb_info {
	struct simplefs_super_disk disk_super;
	u64 file_count;
	u64 disk_sectors;
	u32 max_name_len;
	u32 file_sectors;
	bool erased;
};

#endif

#endif
