#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_FILES 100
#define MAX_DATA_SIZE 4096
#define PARTITION_PATH "/dev/mmcblk0p3"

typedef struct {
    char name[256];
    uint32_t size;
    time_t mtime;
    bool is_directory;
    bool in_use;
    char data[MAX_DATA_SIZE];
} pifs_inode_t;

int main() {
    // calloc will reset bytes to 0, so we don't have to manually set in_use to false for all inodes
    pifs_inode_t *disk_memory = calloc(MAX_FILES, sizeof(pifs_inode_t));

    // init inode 0 as the root directory
    // name is empty because find_inode_index expects "" for "/"
    strcpy(disk_memory[0].name, ""); 
    disk_memory[0].size = 0;
    disk_memory[0].mtime = time(NULL);
    disk_memory[0].is_directory = true;
    disk_memory[0].in_use = true;

    // open partition, create if not exist with rw permissions
    int fd = open(PARTITION_PATH, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) {
        perror("Error opening partition.");
        return 1;
    }

    if (write(fd, disk_memory, MAX_FILES * sizeof(pifs_inode_t)) < 0) {
        perror("Failed to write filesystem");
        close(fd);
        return 1;
    }

    printf("PIFS formatted successfully and root as inode 0\n");

    close(fd);
    free(disk_memory);
    return 0;
}