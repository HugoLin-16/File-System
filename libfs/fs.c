#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define FAT_EOC 0xFFFF
#define FAT_SIZE 2048

struct __attribute__ ((packed))superblock
{
	uint64_t signature;
	uint16_t total_block;
	uint16_t root_dir_block_index;
	uint16_t data_block_start_index;
	uint16_t number_of_data_block;
	uint8_t	number_of_fat_block;
	uint8_t padding[4079];
};

struct __attribute__ ((packed)) root_directory
{
	uint8_t filename[16];
	uint32_t size_of_file;
	uint16_t index_first_data_block;
	uint8_t padding[10];
};

struct file_descriptor
{
	bool inUse;
	int file_rdir_index;
	size_t offset;
};

static struct superblock Superblock;
static struct root_directory Root_Directory[FS_FILE_MAX_COUNT];
static uint16_t *FAT;
static struct file_descriptor Open_FD_Table[FS_OPEN_MAX_COUNT];

int readFAT(void)
{
	for(int i = 0; i < Superblock.number_of_fat_block; i++) {
		// if the last FAT block is not fully filled, a bounce buffer is needed
		if (i == Superblock.number_of_fat_block - 1 &&
			Superblock.number_of_data_block % FAT_SIZE != 0) {
			uint16_t buffer[FAT_SIZE];
			// uint16_t *buffer = (uint16_t*)malloc(FAT_SIZE * sizeof(uint16_t));
			if (block_read(i + 1, buffer) == -1) return -1;
			memcpy((&FAT[i * FAT_SIZE]), buffer, Superblock.number_of_data_block % FAT_SIZE);
		}
		else {
			if (block_read(i + 1, (&FAT[i * FAT_SIZE])) == -1) return -1;
		}
	}
	return 0;
}

int writeFAT(void)
{
	for(int i = 0; i < Superblock.number_of_fat_block; i++) {
		// if the last FAT block is not fully filled, a bounce buffer is needed
		if (i == Superblock.number_of_fat_block - 1 &&
			Superblock.number_of_data_block % FAT_SIZE != 0) {
			uint16_t buffer[FAT_SIZE];
			// need to set to 0 since the buffer is not full and initialized
			memset(buffer, 0, BLOCK_SIZE);
			memcpy(buffer, (&FAT[i * FAT_SIZE]), Superblock.number_of_data_block % FAT_SIZE);
			if (block_write(i + 1, buffer) == -1) return -1;
		}
		else {
			if (block_write(i + 1, (&FAT[i * FAT_SIZE])) == -1) return -1;
		}
	}
	return 0;
}

int fs_mount(const char *diskname)
{
	if (!diskname || block_disk_open(diskname) == -1) return -1;

	if (block_read(0, &Superblock) == -1) return -1;

	// check disk name
	if (Superblock.signature != 6000536558536704837) return -1;
	// check format
	if (block_disk_count() != Superblock.total_block) return -1;

	// each data block requires one entry in FAT block, and one entry is 16 bits
	FAT = NULL;
	FAT = (uint16_t*)malloc(Superblock.number_of_data_block * sizeof(uint16_t));
	if (readFAT() == -1) return -1;
    if (FAT[0] != FAT_EOC) return -1;
	// read root directory
	if (block_read(Superblock.root_dir_block_index, &Root_Directory) == -1) return -1;

	for(int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
		Open_FD_Table[i].inUse = false;
		Open_FD_Table[i].file_rdir_index = -1;
	}

	return 0;
}

int fs_umount(void)
{
	if (FAT == NULL || writeFAT() == -1 ) return -1;

	for(int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
		if (Open_FD_Table[i].inUse) return -1;
	}

	if (block_write(Superblock.root_dir_block_index, &Root_Directory) == -1) return -1;
	if (block_disk_close() == -1) return -1;
	free(FAT);
	FAT = NULL;
	return 0;
}

int fs_info(void)
{
	if (FAT == NULL) return -1;

	int free_fat = 0, free_rdir = 0;
	printf("FS Info:\n");
	printf("total_blk_count=%u\n", Superblock.total_block);
	printf("fat_blk_count=%u\n", Superblock.number_of_fat_block);
	printf("rdir_blk=%u\n", Superblock.root_dir_block_index);
	printf("data_blk=%u\n", Superblock.data_block_start_index);
	printf("data_blk_count=%u\n", Superblock.number_of_data_block);

	// count # of free FAT entries and # of free root directory entries
	for(int i = 1; i < Superblock.number_of_data_block; i++) {
		if (FAT[i] == 0) free_fat++;
	}
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if ((char)Root_Directory[i].filename[0] == '\0') free_rdir++;
	}

	printf("fat_free_ratio=%d/%u\n", free_fat, Superblock.number_of_data_block);
	printf("rdir_free_ratio=%d/%u\n", free_rdir, FS_FILE_MAX_COUNT);
	return 0;
}

int fs_create(const char *filename)
{
	if (FAT == NULL || !filename) return -1;
	if (strlen(filename) > FS_FILENAME_LEN - 1) return -1;

	bool found_empty = false, file_exist = false;
	int empty_entry_index = 0;

	// check for empty entry and duplicate filename
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if ((char)Root_Directory[i].filename[0] == '\0' && !found_empty) {
			found_empty = true;
			empty_entry_index = i;
		}
		if (strcmp((char*)Root_Directory[i].filename, filename) == 0) {
			file_exist = true;
			break;
		}
	}

	if (!found_empty || file_exist) return -1;
	strcpy((char*)Root_Directory[empty_entry_index].filename, filename);
	Root_Directory[empty_entry_index].size_of_file = 0;
	Root_Directory[empty_entry_index].index_first_data_block = FAT_EOC;
	return 0;
}

int fs_delete(const char *filename)
{
	if (FAT == NULL || !filename) return -1;
	if (strlen(filename) > FS_FILENAME_LEN - 1) return -1;

	int curr_fat_index = -1, next_fat_index = -1, rdir_index = -1;
	// find file in root directory and reset every information
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (strcmp((char*)Root_Directory[i].filename, filename) == 0) {
			rdir_index = i;
			curr_fat_index = Root_Directory[i].index_first_data_block;
			Root_Directory[i].filename[0] = '\0';
			Root_Directory[i].index_first_data_block = FAT_EOC;
			Root_Directory[i].size_of_file = 0;
			break;
		}
	}

	// no file to delete
	if (curr_fat_index == -1) return -1;

	// file is open
	for(int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
		if (Open_FD_Table[i].file_rdir_index == rdir_index) return -1;
	}

	// free data in FAT
	while(curr_fat_index != FAT_EOC) {
		next_fat_index = FAT[curr_fat_index];
		FAT[curr_fat_index] = 0;
		curr_fat_index = next_fat_index;
	}
	return 0;
}

int fs_ls(void)
{
	if (!FAT) return -1;
	printf("FS Ls:\n");
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if ((char)Root_Directory[i].filename[0] != '\0') {
			printf("file: %s, size: %u, data_blk: %d\n",
				(char*)Root_Directory[i].filename, Root_Directory[i].size_of_file,
				Root_Directory[i].index_first_data_block);
		}
	}
	return 0;
}

int fs_open(const char *filename)
{
	if (!filename || !FAT) return -1;
	if (strlen(filename) > FS_FILENAME_LEN - 1) return -1;

	int fd = -1, rdir_index = 0;

	// check if excess fs open max and find available fd
	for(int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
		if (!Open_FD_Table[i].inUse) {
			fd = i;
			break;
		}
	}
	if (fd == -1) return -1;
    if (rdir_index == FS_FILE_MAX_COUNT) return -1;
	// check if file exist
	for(rdir_index = 0; rdir_index < FS_FILE_MAX_COUNT; rdir_index++) {
		if (strcmp((char*)Root_Directory[rdir_index].filename, filename) == 0) {
			break;
		}
	}

	Open_FD_Table[fd].inUse = true;
	Open_FD_Table[fd].file_rdir_index = rdir_index;
	Open_FD_Table[fd].offset = 0;

	return fd;
}

int fs_close(int fd)
{
	// out of bounds or not currently open
	if (!FAT || fd > FS_OPEN_MAX_COUNT - 1 || fd < 0) return -1;
	if (!Open_FD_Table[fd].inUse) return -1;

	Open_FD_Table[fd].inUse = false;
	Open_FD_Table[fd].file_rdir_index = -1;
	Open_FD_Table[fd].offset = 0;
	return 0;
}

int fs_stat(int fd)
{
	// out of bounds or not currently open
	if (!FAT || fd > FS_OPEN_MAX_COUNT - 1 || fd < 0) return -1;
	if (!Open_FD_Table[fd].inUse) return -1;
	return (int)Root_Directory[Open_FD_Table[fd].file_rdir_index].size_of_file;
}

int fs_lseek(int fd, size_t offset)
{
	// out of bounds or not currently open
	if (!FAT || fd > FS_OPEN_MAX_COUNT - 1 || fd < 0) return -1;
	if (!Open_FD_Table[fd].inUse) return -1;
	if (fs_stat(fd) == -1) return -1;
	if (Root_Directory[Open_FD_Table[fd].file_rdir_index].size_of_file < offset) return -1;

	Open_FD_Table[fd].offset = offset;
	return 0;
}

// find the index of the data block corresponding to the offset
int data_block_offset(int fd)
{
	int file_to_rdir_index = Open_FD_Table[fd].file_rdir_index;
	int curr_index = Root_Directory[file_to_rdir_index].index_first_data_block;

	for(int i = 0; i < (int)Open_FD_Table[fd].offset / BLOCK_SIZE; i++) {
		curr_index = FAT[curr_index];
	}
	return curr_index;
}

// return -1 if FAT is full
int find_empty_FAT()
{
	int index = 0;
	for(index = 0; index < Superblock.number_of_data_block; index++) {
		if (FAT[index] == 0) break;
	}
	if (index == Superblock.number_of_data_block) return -1;
	return index;
}

// find the number of data block need to access
int num_data_block_access(size_t count, size_t curr_offset)
{
	int number = 1;
	size_t remain_count = 0;
	if (curr_offset > BLOCK_SIZE - 1) curr_offset = curr_offset % BLOCK_SIZE;

	remain_count = count - BLOCK_SIZE + curr_offset;
	number = number + remain_count / BLOCK_SIZE;
    if (remain_count % BLOCK_SIZE != 0) {
        number += 1;
    }

	return number;
}

int max(int a, int b)
{
	return a > b ? a : b;
}

int min(int a, int b)
{
	return a < b ? a : b;
}

int fs_write(int fd, void *buf, size_t count)
{
	// out of bounds or not currently open
	if (!FAT || (fd > FS_OPEN_MAX_COUNT - 1) || fd < 0 || !buf) return -1;
	if (!Open_FD_Table[fd].inUse || buf == NULL) return -1;
	if (count == 0) return 0;

	// find data block corresponding to the offset
	int curr_data_block_index = 0, empty_index = 0;;
	if(Root_Directory[Open_FD_Table[fd].file_rdir_index].index_first_data_block
		== FAT_EOC) {
		empty_index = find_empty_FAT();
		if (empty_index == -1) return 0;
		FAT[empty_index] = FAT_EOC;
		Root_Directory[Open_FD_Table[fd].file_rdir_index].index_first_data_block
			= empty_index;
	}

	curr_data_block_index = data_block_offset(fd);

	// int next_data_block_index = 0;
	uint8_t buffer[BLOCK_SIZE];

	size_t curr_offset = Open_FD_Table[fd].offset;
	int data_block_access = num_data_block_access(count, curr_offset);
	size_t count_written = 0;
	if (curr_offset >= BLOCK_SIZE) curr_offset -= BLOCK_SIZE;

	for(int i = 0; i < data_block_access; i++) {
		if (FAT[curr_data_block_index] == FAT_EOC && i < data_block_access - 1) {
			empty_index = find_empty_FAT();
			if (empty_index == -1) return count_written;
			FAT[curr_data_block_index] = empty_index;
			FAT[empty_index] = FAT_EOC;
		}
		block_read(Superblock.data_block_start_index + curr_data_block_index, buffer);
		if (i == 0) {
			memcpy(buffer + curr_offset, buf, min(count, BLOCK_SIZE - curr_offset));
			count_written += min(count, BLOCK_SIZE - curr_offset);
		}
		else if (i == data_block_access - 1) {
			memcpy(buffer, buf + count_written, count - count_written);
			count_written += count - count_written;
		}
		else {
			memcpy(buffer, buf + count_written, BLOCK_SIZE);
			count_written += BLOCK_SIZE;
		}
		block_write(Superblock.data_block_start_index + curr_data_block_index, buffer);
		curr_data_block_index = FAT[curr_data_block_index];
	}

	Open_FD_Table[fd].offset += count_written;
	uint32_t file_size = Root_Directory[Open_FD_Table[fd].file_rdir_index].size_of_file;
	// take the maximum because file only gets larger
	Root_Directory[Open_FD_Table[fd].file_rdir_index].size_of_file =
		max(Open_FD_Table[fd].offset, file_size);
	return count_written;
}

int fs_read(int fd, void *buf, size_t count)
{
	// out of bounds or not currently open
	if (!FAT || (fd > FS_OPEN_MAX_COUNT - 1) || fd < 0) return -1;
	if (!Open_FD_Table[fd].inUse || buf == NULL) return -1;
	if (count == 0) return 0;

	// find data block corresponding to the offset
	int curr_data_block_index = data_block_offset(fd);
	uint8_t buffer[BLOCK_SIZE];

	size_t curr_offset = Open_FD_Table[fd].offset;
	size_t count_read = 0;

	int data_block_access = 0;
	if ((int)curr_offset < fs_stat(fd) - 1) {
		data_block_access = num_data_block_access(count, curr_offset);
	}

	if (curr_offset >= BLOCK_SIZE) curr_offset -= BLOCK_SIZE;

	for(int i = 0; i < data_block_access; i++) {
		block_read(Superblock.data_block_start_index + curr_data_block_index, buffer);
		if (i == 0) {
			memcpy(buf + count_read, buffer + curr_offset, min(count, BLOCK_SIZE - curr_offset));
			count_read += min(count, BLOCK_SIZE - curr_offset);
		}
		else if (i == data_block_access - 1) {
			memcpy(buf + count_read, buffer, count - count_read);
			count_read += count - count_read;
		}
		else {
			memcpy(buf + count_read, buffer, BLOCK_SIZE);
			count_read += BLOCK_SIZE;
		}
		curr_data_block_index = FAT[curr_data_block_index];
		if (curr_data_block_index == FAT_EOC) break;
	}

	Open_FD_Table[fd].offset += count_read;
	return count_read;
}
