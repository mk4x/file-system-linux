#include <fuse.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_FILES 100
#define MAX_DATA_SIZE 4096
#define PARTITION_PATH "/dev/mmcblk0p3"

// INODE
typedef struct {
    char name[256];
    uint32_t size;
    time_t mtime; // timestamp for last modification
    bool is_directory;
    bool in_use;
    int parent_inode;
    char data[MAX_DATA_SIZE];
} pifs_inode_t;

pifs_inode_t *disk_memory = NULL; // global memory buffer to simulate disk storage


int pifs_getattr( const char *, struct stat * );
int pifs_readdir( const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info * );
int pifs_open( const char *, struct fuse_file_info * );
int pifs_read( const char *, char *, size_t, off_t, struct fuse_file_info * );
int pifs_release(const char *path, struct fuse_file_info *fi);
int pifs_mknod(const char *, mode_t, dev_t);
int pifs_mkdir(const char *, mode_t);
int pifs_write(const char *, const char *, size_t, off_t, struct fuse_file_info *);
int find_children(int, int *, int);
void* pifs_init();
void pifs_destroy(void *private_data);
int find_inode_index(const char *path);
int pifs_truncate(const char *path, off_t size);
int pifs_access(const char *path, int mask);


// HACK: tells compiler to ignore unused parameter warnings
int pifs_access(const char *path, int mask) {
    (void) path;
    (void) mask;
    return 0;
}

// to test files with echo "..." > file.txt, we need a simple truncate
int pifs_truncate(const char *path, off_t size) {
    printf("truncate: (path=%s, size=%lld)\n", path, (long long)size);
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    pifs_inode_t *inode = &disk_memory[inode_index];
    if (inode->is_directory) {
        return -EISDIR;
    }

    inode->size = (uint32_t)size;
    inode->mtime = time(NULL);
    return 0;
}

/*
 * See descriptions in fuse source code usually located in /usr/include/fuse/fuse.h
 * Notice: The version on Github may be a newer version than you have installed test
 */
static struct fuse_operations pifs_oper = {
    .getattr    = pifs_getattr,
    .readdir    = pifs_readdir,
    .mknod      = pifs_mknod,
    .mkdir      = pifs_mkdir,
    .unlink     = NULL,
    .rmdir      = NULL,
    .truncate   = pifs_truncate,
    .open       = pifs_open,
    .read       = pifs_read,
    .release    = pifs_release,
    .write      = pifs_write,
    .rename     = NULL,
    .utime      = NULL,
    .init       = pifs_init,
    .destroy    = pifs_destroy,
    .access     = pifs_access
};

int find_inode_index_recursive(int current_parent, const char *path) {
    // Base case: if the path is empty or just "/", we found it
    if (path == NULL || strlen(path) == 0 || strcmp(path, "/") == 0) {
        return current_parent;
    }

    // Skip leading slash for processing
    const char *start = (path[0] == '/') ? path + 1 : path;

    // Extract the next component (the folder or file name before the next '/')
    char component[256];
    const char *next_slash = strchr(start, '/');
    int len;

    if (next_slash) {
        len = next_slash - start;
    } else {
        len = strlen(start);
    }

    strncpy(component, start, len);
    component[len] = '\0';

    // Search for this component inside the current parent
    int found_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (disk_memory[i].in_use &&
            disk_memory[i].parent_inode == current_parent &&
            strcmp(disk_memory[i].name, component) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx == -1) return -1;

    // If there's more path left, recurse deeper
    if (next_slash && strlen(next_slash + 1) > 0) {
        return find_inode_index_recursive(found_idx, next_slash + 1);
    }

    return found_idx;
}

// Wrapper to make it easy to call
int find_inode_index(const char *path) {
    if (strcmp(path, "/") == 0) return 0;
    return find_inode_index_recursive(0, path);
}


/* helper function to find all files belonging to a parent directory */
int find_children(int parent_index, int *children_indices, int max_children) {
    int count = 0;
    for (int i = 0; i < MAX_FILES && count < max_children; i++) {
        if (disk_memory[i].in_use && disk_memory[i].parent_inode == parent_index && i != 0) {
            children_indices[count++] = i;
        }
    }
    return count;
}


/* Create a file */
int pifs_mknod(const char *path, mode_t mode, dev_t dev) {
    (void) dev;
    (void) mode;
    printf("mknod: (path=%s)\n", path);

    if (find_inode_index(path) != -1) {
        return -EEXIST;
    }

    int free_slot = -1;
    for (int i = 1; i < MAX_FILES; i++) {
        if (!disk_memory[i].in_use) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        return -ENOSPC;
    }

    char *last_slash = strrchr(path, '/');
    int parent_idx = 0;

    if (last_slash != path) {
        char parent_path[256];
        strncpy(parent_path, path, last_slash - path);
        parent_path[last_slash - path] = '\0';
        parent_idx = find_inode_index(parent_path);
    }

    const char *filename = last_slash + 1;

    strcpy(disk_memory[free_slot].name, filename);
    disk_memory[free_slot].size = 0;
    disk_memory[free_slot].mtime = time(NULL);
    disk_memory[free_slot].is_directory = false;
    disk_memory[free_slot].in_use = true;
    disk_memory[free_slot].parent_inode = parent_idx;
    memset(disk_memory[free_slot].data, 0, MAX_DATA_SIZE);

    printf("mknod: Created file '%s' at inode %d\n", filename, free_slot);

    return 0;
}

/* Create a directory */
int pifs_mkdir(const char *path, mode_t mode) {
    (void) mode;
    printf("mkdir: (path=%s)\n", path);

    if (find_inode_index(path) != -1) {
        return -EEXIST;
    }

    int free_slot = -1;
    for (int i = 1; i < MAX_FILES; i++) {
        if (!disk_memory[i].in_use) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        return -ENOSPC;
    }

    char *last_slash = strrchr(path, '/');
    int parent_idx = 0;

    if (last_slash != path) {
        char parent_path[256];
        strncpy(parent_path, path, last_slash - path);
        parent_path[last_slash - path] = '\0';
        parent_idx = find_inode_index(parent_path);
    }

    const char *dirname = last_slash + 1;

    strcpy(disk_memory[free_slot].name, dirname);
    disk_memory[free_slot].size = 0;
    disk_memory[free_slot].mtime = time(NULL);
    disk_memory[free_slot].is_directory = true;
    disk_memory[free_slot].in_use = true;
    disk_memory[free_slot].parent_inode = parent_idx;
    memset(disk_memory[free_slot].data, 0, MAX_DATA_SIZE);

    printf("mkdir: Created directory '%s' at inode %d\n", dirname, free_slot);

    return 0;
}

/* Write to a file */
int pifs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    printf("write: (path=%s, size=%zu, offset=%lld)\n", path, size, (long long)offset);

    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    pifs_inode_t *inode = &disk_memory[inode_index];

    if (inode->is_directory) {
        return -EISDIR;
    }

    if ((size_t)offset + size > MAX_DATA_SIZE) {
        size = MAX_DATA_SIZE - (size_t)offset;
        if (size == 0) {
            return -ENOSPC;
        }
    }

    memcpy(inode->data + offset, buf, size);

    if ((size_t)offset + size > inode->size) {
        inode->size = (uint32_t)((size_t)offset + size);
    }

    inode->mtime = time(NULL);

    printf("write: wrote %zu bytes to file '%s'\n", size, inode->name);

    return size;
}

/*
 * Return file attributes.
 * The "stat" structure is described in detail in the stat(2) manual page.
 * For the given pathname, this should fill in the elements of the "stat" structure.
 * If a field is meaningless or semi-meaningless (e.g., st_ino) then it should be set to 0 or given a "reasonable" value.
 * This call is pretty much required for a usable filesystem.
*/
int pifs_getattr( const char *path, struct stat *stbuf ) {
    printf("getattr: (path=%s)\n", path);

    memset(stbuf, 0, sizeof(struct stat));

    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    pifs_inode_t *inode = &disk_memory[inode_index];

    if (inode->is_directory) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = inode->size;
    }

    stbuf->st_mtime = inode->mtime;

    return 0;
}

/*
 * Return one or more directory entries (struct dirent) to the caller. This is one of the most complex FUSE functions.
 * Required for essentially any filesystem, since it's what makes ls and a whole bunch of other things work.
 * The readdir function is somewhat like read, in that it starts at a given offset and returns results in a caller-supplied buffer.
 * However, the offset not a byte offset, and the results are a series of struct dirents rather than being uninterpreted bytes.
 * To make life easier, FUSE provides a "filler" function that will help you put things into the buffer.
 *
 * The general plan for a complete and correct readdir is:
 *
 * 1. Find the first directory entry following the given offset (see below).
 * 2. Optionally, create a struct stat that describes the file as for getattr (but FUSE only looks at st_ino and the file-type bits of st_mode).
 * 3. Call the filler function with arguments of buf, the null-terminated filename, the address of your struct stat
 *    (or NULL if you have none), and the offset of the next directory entry.
 * 4. If filler returns nonzero, or if there are no more files, return 0.
 * 5. Find the next file in the directory.
 * 6. Go back to step 2.
 * From FUSE's point of view, the offset is an uninterpreted off_t (i.e., an unsigned integer).
 * You provide an offset when you call filler, and it's possible that such an offset might come back to you as an argument later.
 * Typically, it's simply the byte offset (within your directory layout) of the directory entry, but it's really up to you.
 *
 * It's also important to note that readdir can return errors in a number of instances;
 * in particular it can return -EBADF if the file handle is invalid, or -ENOENT if you use the path argument and the path doesn't exist.
*/
int pifs_readdir( const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi ) {
    (void) offset;
    (void) fi;
    printf("readdir: (path=%s)\n", path);

    int dir_index = find_inode_index(path);
    if (dir_index == -1) {
        return -ENOENT;
    }

    if (!disk_memory[dir_index].is_directory) {
        return -ENOTDIR;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    int children[MAX_FILES];
    int child_count = find_children(dir_index, children, MAX_FILES);

    for (int i = 0; i < child_count; i++) {
        filler(buf, disk_memory[children[i]].name, NULL, 0);
    }

    return 0;
}

/*
 * Open a file.
 * If you aren't using file handles, this function should just check for existence and permissions and return either success or an error code.
 * If you use file handles, you should also allocate any necessary structures and set fi->fh.
 * In addition, fi has some other fields that an advanced filesystem might find useful; see the structure definition in fuse_common.h for very brief commentary.
 * Link: https://github.com/libfuse/libfuse/blob/0c12204145d43ad4683136379a130385ef16d166/include/fuse_common.h#L50
*/
int pifs_open( const char *path, struct fuse_file_info *fi ) {
    printf("open: (path=%s)\n", path);
    return 0;
}

/*
 * Read size bytes from the given file into the buffer buf, beginning offset bytes into the file. See read(2) for full details.
 * Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. Required for any sensible filesystem.
*/
int pifs_read( const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi ) {
    (void) fi;
    printf("read: (path=%s)\n", path);

    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    pifs_inode_t *inode = &disk_memory[inode_index];

    if (inode->is_directory) {
        return -EISDIR;
    }

    if ((size_t)offset >= inode->size) {
        return 0;
    }

    if ((size_t)offset + size > inode->size) {
        size = inode->size - (size_t)offset;
    }

    memcpy(buf, inode->data + offset, size);

    printf("read: read %zu bytes from file '%s' (offset=%lld)\n", size, inode->name, (long long)offset);

    return size;
}

/*
 * This is the only FUSE function that doesn't have a directly corresponding system call, although close(2) is related.
 * Release is called when FUSE is completely done with a file; at that point, you can free up any temporarily allocated data structures.
 */
int pifs_release(const char *path, struct fuse_file_info *fi) {
    (void) fi;
    printf("release: (path=%s)\n", path);
    return 0;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the `private_data` field of
 * `struct fuse_context` to all file operations, and as a
 * parameter to the destroy() method. It overrides the initial
 * value provided to fuse_main() / fuse_new().
 */
void* pifs_init() {
    printf("init filesystem\n");

    disk_memory = (pifs_inode_t *)malloc(MAX_FILES * sizeof(pifs_inode_t));
    if (disk_memory == NULL) {
        fprintf(stderr, "Failed to allocate memory for inodes\n");
        exit(EXIT_FAILURE);
    }

    int fd = open(PARTITION_PATH, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open partition for reading");
        free(disk_memory);
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_read = read(fd, disk_memory, MAX_FILES * sizeof(pifs_inode_t));
    if (bytes_read < 0) {
        perror("Failed to read from partition");
        close(fd);
        free(disk_memory);
        exit(EXIT_FAILURE);
    }

    printf("Read %zd bytes from partition\n", bytes_read);
    close(fd);

    return NULL;
}

/**
 * Clean up filesystem
 * Called on filesystem exit.
 */
void pifs_destroy(void *private_data) {
    (void) private_data;
    printf("destroy filesystem\n");
    printf("saving to partition %s\n", PARTITION_PATH);

    int fd = open(PARTITION_PATH, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open partition for writing");
        exit(EXIT_FAILURE);
    }
    write(fd, disk_memory, MAX_FILES * sizeof(pifs_inode_t));
    close(fd);

    free(disk_memory);
    printf("successfully saved to partition and cleaned up memory\n");
}


int main( int argc, char *argv[] ) {
    fuse_main( argc, argv, &pifs_oper );

    return 0;
}
