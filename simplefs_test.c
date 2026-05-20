#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "simplefs.h"

static int walk_files(const char *mountpoint, int do_write, int do_check)
{
	DIR *dir = opendir(mountpoint);
	struct dirent *entry;
	int checked = 0;

	if (!dir) {
		printf("cannot open mountpoint\n");
		return 1;
	}

	while ((entry = readdir(dir)) != NULL) {
		char path[4096];
		char write_buf[32];
		char read_buf[32];
		char first_bytes[8];
		int value;
		int len;
		int fd;
		ssize_t read_len;
		int saved;
		int empty;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;

		snprintf(path, sizeof(path), "%s/%s", mountpoint, entry->d_name);

		if (do_write) {
			value = rand() % 10000;
			len = snprintf(write_buf, sizeof(write_buf), "%d\n", value);
		} else {
			value = 0;
			len = 8;
		}

		fd = open(path, do_write ? O_RDWR : O_RDONLY);
		if (fd < 0)
			return 1;

		if (do_write) {
			if (write(fd, write_buf, len) != len) {
				close(fd);
				return 1;
			}
		}

		if (!do_write) {
			saved = 0;
			empty = 1;

			while ((read_len = read(fd, read_buf, sizeof(read_buf))) > 0) {
				for (int i = 0; i < read_len; i++) {
					if (saved < 8)
						first_bytes[saved++] = read_buf[i];
					if (read_buf[i])
						empty = 0;
				}
			}

			printf("%s: read bytes:", entry->d_name);
			for (int i = 0; i < saved; i++)
				printf(" %02x", (unsigned char)first_bytes[i]);
			if (empty)
				printf(" empty");
			printf("\n");

			close(fd);
			checked++;
			continue;
		}

		lseek(fd, 0, SEEK_SET);
		memset(read_buf, 0, sizeof(read_buf));
		read_len = read(fd, read_buf, len);
		if (read_len != len) {
			close(fd);
			return 1;
		}

		close(fd);

		if (do_check) {
			if (memcmp(write_buf, read_buf, len)) {
				printf("%s: wrote %d, read %s\n",
				       entry->d_name, value, read_buf);
				closedir(dir);
				return 1;
			}
		}

		if (do_write) {
			read_buf[len - 1] = '\0';
			printf("%s: wrote %d, read %s\n",
			       entry->d_name, value, read_buf);
		}

		checked++;
	}

	closedir(dir);
	printf("checked files: %d\n", checked);
	return 0;
}

static int test_files(const char *mountpoint)
{
	return walk_files(mountpoint, 1, 1);
}

static int write_files(const char *mountpoint)
{
	return walk_files(mountpoint, 1, 0);
}

static int read_files(const char *mountpoint)
{
	return walk_files(mountpoint, 0, 0);
}

static int print_hashes(const char *mountpoint)
{
	struct simplefs_ioctl_hash_entry *entries;
	struct simplefs_ioctl_hashes req;
	int fd;
	__u32 i;
	int capacity = 10000;

	entries = calloc(capacity, sizeof(*entries));
	if (!entries)
		return 1;

	req.entries = (uintptr_t)entries;
	req.capacity = capacity;
	req.count = 0;

	fd = open(mountpoint, O_RDONLY);
	if (fd < 0)
		return 1;

	ioctl(fd, SIMPLEFS_IOCTL_HASHES, &req);
	close(fd);

	for (i = 0; i < req.count && i < req.capacity; i++) {
		printf("%s: sector=%llu sectors=%u hash=0x%08x\n",
		       entries[i].name,
		       (unsigned long long)entries[i].start_sector,
		       entries[i].sectors,
		       entries[i].hash);
	}

	free(entries);
	return 0;
}

static int print_mapping(const char *mountpoint, const char *name)
{
	struct simplefs_ioctl_mapping map;
	int fd;

	memset(&map, 0, sizeof(map));
	snprintf(map.name, sizeof(map.name), "%s", name);

	fd = open(mountpoint, O_RDONLY);
	if (fd < 0)
		return 1;

	ioctl(fd, SIMPLEFS_IOCTL_MAPPING, &map);
	close(fd);

	printf("%s: sector=%llu sectors=%u\n",
	       map.name,
	       (unsigned long long)map.start_sector,
	       map.sectors);
	return 0;
}

static int simple_ioctl(const char *mountpoint, unsigned long command)
{
	int fd = open(mountpoint, O_RDONLY);

	if (fd < 0)
		return 1;

	ioctl(fd, command);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mountpoint;

	if (argc < 2)
		return 1;

	mountpoint = argv[1];
	srand(time(NULL));

	if (argc == 2)
		return test_files(mountpoint);

	if (!strcmp(argv[2], "write"))
		return write_files(mountpoint);

	if (!strcmp(argv[2], "read"))
		return read_files(mountpoint);

	if (!strcmp(argv[2], "zero"))
		return simple_ioctl(mountpoint, SIMPLEFS_IOCTL_ZERO);

	if (!strcmp(argv[2], "erase"))
		return simple_ioctl(mountpoint, SIMPLEFS_IOCTL_ERASE);

	if (!strcmp(argv[2], "hashes"))
		return print_hashes(mountpoint);

	if (!strcmp(argv[2], "mapping") && argc > 3)
		return print_mapping(mountpoint, argv[3]);

	printf("unknown command\n");
	return 1;
}
