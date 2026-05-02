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
#include <utime.h>

#define MAX_FILES 100
#define MAX_DATA_SIZE 4096
#define PARTITION_PATH "/dev/mmcblk0p3"

// INODE
typedef struct {
    char name[256];
    uint32_t size;
    time_t atime; // timestamp for last access
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
int pifs_unlink(const char *);
int pifs_rmdir(const char *);
int pifs_rename(const char *, const char *);
int pifs_utime(const char *, struct utimbuf *);
int pifs_write(const char *, const char *, size_t, off_t, struct fuse_file_info *);
int find_children(int, int *, int);
int is_descendant(int, int);
void* pifs_init();
void pifs_destroy(void *private_data);
int find_inode_index(const char *path);
int pifs_truncate(const char *path, off_t size);
int pifs_access(const char *path, int mask);


// HACK: avoiding implicit declaration warnings (just a stub)
int pifs_access(const char *path, int mask) {
    (void) path;
    (void) mask;
    return 0;
}

// to test files with echo "..." > file.txt, we need a simple truncate
int pifs_truncate(const char *path, off_t size) {
    printf("truncate: (path=%s, size=%lld)\n", path, (long long)size);

    // check if the file actually exists
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }
    // make sure its not a directory
    pifs_inode_t *inode = &disk_memory[inode_index];
    if (inode->is_directory) {
        return -EISDIR;
    }

    inode->size = (uint32_t)size;
    inode->atime = time(NULL);
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
    .unlink     = pifs_unlink,
    .rmdir      = pifs_rmdir,
    .truncate   = pifs_truncate,
    .open       = pifs_open,
    .read       = pifs_read,
    .release    = pifs_release,
    .write      = pifs_write,
    .rename     = pifs_rename,
    .utime      = pifs_utime,
    .init       = pifs_init,
    .destroy    = pifs_destroy,
    .access     = pifs_access
};

/*
 * Helper function to find the parent inode index and leaf name for a given path. 
 * For example, if the path is "/foo/bar/baz.txt", 
 * this function will find the inode index of "/foo/bar" and return "baz.txt" as the leaf name. 
 * If the path is "/foo.txt", it will return the root inode index (0) as the parent and "foo.txt" 
 * as the leaf name. If the path is "/", it will return an error since it has no parent.
 */
static int find_parent_inode(const char *path, int *parent_inode, const char **leaf_name) {
    char temp[256]; // temporary buffer to manipulate the path 
    size_t len = strlen(path); 

    // validate path length and ensure it is not empty or too long
    if (len == 0 || len >= sizeof(temp)) {
        return -EINVAL;
    }

    // find the last slash in the original path
    const char *last_slash_in_path = strrchr(path, '/');
    if (last_slash_in_path == NULL) {
        return -EINVAL;
    }

    // if it's the first character, then the parent is root
    if (last_slash_in_path == path) {
        *parent_inode = 0;
        *leaf_name = last_slash_in_path + 1;
        return (*leaf_name[0] == '\0') ? -EINVAL : 0;
    }

    // copy the path to a temporary buffer and terminate at the last slash to isolate the parent path
    strcpy(temp, path);
    char *last_slash = strrchr(temp, '/');
    *last_slash = '\0';
    *parent_inode = find_inode_index(temp); // find the parent inode index using the modified path
    if (*parent_inode == -1) {
        return -ENOENT;
    }

    // set the leaf name to the part of the path after the last slash from the original path, and validate that it's not empty
    *leaf_name = last_slash_in_path + 1;
    if (*leaf_name[0] == '\0') {
        return -EINVAL;
    }

    return 0;
}

// helper function to clear an inode's data and metadata, effectively marking it as free
static void clear_inode(int inode_index) {
    memset(&disk_memory[inode_index], 0, sizeof(pifs_inode_t));
}

// recursive helper function to find the inode index for a given path, starting from a specified parent inode index
int find_inode_index_recursive(int current_parent, const char *path) {
    // base case: if the path is empty or just "/", we found it
    if (path == NULL || strlen(path) == 0 || strcmp(path, "/") == 0) {
        return current_parent;
    }

    // just skip leading slash 
    const char *start = (path[0] == '/') ? path + 1 : path;

    // extract next split, "/", component from the path /foo/bar/ -> "foo"
    char component[256];
    const char *next_slash = strchr(start, '/');
    int len;

    if (next_slash) {
        len = next_slash - start;
    } else {
        len = strlen(start);
    }

    // move component into buffer
    strncpy(component, start, len);
    component[len] = '\0'; 

    // search for a child of current_parent with the name of component
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

    // if more path left, recursive deeper
    if (next_slash && strlen(next_slash + 1) > 0) {
        return find_inode_index_recursive(found_idx, next_slash + 1);
    }

    return found_idx;
}

// wrapper for recursive 
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

// helper to check if a child could be a descendant of a parent
int is_descendant(int possible_parent, int possible_child) {
    if (possible_parent == possible_child) {
        return 1;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (disk_memory[i].in_use && disk_memory[i].parent_inode == possible_parent) {
            if (i == possible_child) {
                return 1;
            }
            if (disk_memory[i].is_directory && is_descendant(i, possible_child)) {
                return 1;
            }
        }
    }

    return 0;
}


/* Create a file */
int pifs_mknod(const char *path, mode_t mode, dev_t dev) {
    (void) dev;
    (void) mode;
    printf("mknod: (path=%s)\n", path);

    // check if file already exists
    if (find_inode_index(path) != -1) {
        return -EEXIST;
    }

    // find parent inode and leaf name
    int parent_idx = 0;
    const char *filename = NULL;
    if (find_parent_inode(path, &parent_idx, &filename) != 0) {
        return -ENOENT;
    }

    // find a free slot for the new inode (starting from 1 since 0 is root)
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

    // initialize the new inode for the file
    strcpy(disk_memory[free_slot].name, filename);
    disk_memory[free_slot].size = 0;
    disk_memory[free_slot].atime = time(NULL);
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

    // check if directory already exists
    if (find_inode_index(path) != -1) {
        return -EEXIST;
    }

    // find parent inode and leaf name
    int parent_idx = 0;
    const char *dirname = NULL;
    if (find_parent_inode(path, &parent_idx, &dirname) != 0) {
        return -ENOENT;
    }

    // find a free slot for the new inode (starting from 1 since 0 is root)
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

    // initialize the new inode for the directory
    strcpy(disk_memory[free_slot].name, dirname);
    disk_memory[free_slot].size = 0;
    disk_memory[free_slot].atime = time(NULL);
    disk_memory[free_slot].mtime = time(NULL);
    disk_memory[free_slot].is_directory = true;
    disk_memory[free_slot].in_use = true;
    disk_memory[free_slot].parent_inode = parent_idx;
    memset(disk_memory[free_slot].data, 0, MAX_DATA_SIZE);

    printf("mkdir: Created directory '%s' at inode %d\n", dirname, free_slot);

    return 0;
}

/* Delete a file */
int pifs_unlink(const char *path) {
    printf("unlink: (path=%s)\n", path);

    // find the inode index for the file to be deleted
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    // prevent deletion of the root directory
    if (inode_index == 0) {
        return -EPERM;
    }

    // check if it's a directory, if so return error since unlink is for files
    pifs_inode_t *inode = &disk_memory[inode_index];
    if (inode->is_directory) {
        return -EISDIR;
    }

    // clear the inode to mark it as free
    clear_inode(inode_index);
    return 0;
}

/* Delete an empty directory */
int pifs_rmdir(const char *path) {
    printf("rmdir: (path=%s)\n", path);

    // find the inode index for the directory to be deleted
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    // prevent deletion of the root directory
    if (inode_index == 0) {
        return -EBUSY;
    }

    // check if it's a directory, if not return error since rmdir is for directories
    pifs_inode_t *inode = &disk_memory[inode_index];
    if (!inode->is_directory) {
        return -ENOTDIR;
    }

    // check if the directory is empty by looking for any inodes that have this directory as their parent
    int children[MAX_FILES];
    int child_count = find_children(inode_index, children, MAX_FILES);
    if (child_count > 0) {
        return -ENOTEMPTY;
    }

    // clear the inode to mark it as free
    clear_inode(inode_index);
    return 0;
}

int pifs_rename(const char *from, const char *to) {
    printf("rename: (from=%s, to=%s)\n", from, to);

    if (strcmp(from, to) == 0) return 0;

    int from_idx = find_inode_index(from);
    if (from_idx == -1) return -ENOENT;
    if (from_idx == 0)  return -EINVAL; // Cannot move/rename root

    int to_parent_idx = 0;
    const char *to_name = NULL;
    if (find_parent_inode(to, &to_parent_idx, &to_name) != 0) {
        return -ENOENT;
    }

    // check destination name length
    if (strlen(to_name) >= sizeof(disk_memory[from_idx].name)) {
        return -ENAMETOOLONG;
    }

    // check if target already exists
    int target_idx = find_inode_index(to);
    if (target_idx != -1) {
        pifs_inode_t *target = &disk_memory[target_idx];
        
        // standard POSIX disallows renaming a file over an existing directory
        if (target->is_directory) {
            return -EISDIR; 
        }
        
        clear_inode(target_idx);
    }

    pifs_inode_t *inode = &disk_memory[from_idx];

    // cycle prevention: cannot move a directory into one of its descendants
    if (inode->is_directory && is_descendant(from_idx, to_parent_idx)) {
        return -EINVAL;
    }

    // move it by updating the inode's name, parent, and modification time
    strcpy(inode->name, to_name);
    inode->parent_inode = to_parent_idx;
    inode->mtime = time(NULL);

    return 0;
}

// update the access and modification times of a file or directory
// can set buf to provide specific times or set to NULL to use the current time for both
int pifs_utime(const char *path, struct utimbuf *buf) {
    printf("utime: (path=%s)\n", path);

    // find the inode index for the given path
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    // if buf is NULL, set both atime and mtime to the current time; otherwise, use the provided times
    pifs_inode_t *inode = &disk_memory[inode_index];

    if (buf == NULL) {
        time_t now = time(NULL);
        inode->atime = now;
        inode->mtime = now;
    } else {
        inode->atime = buf->actime;
        inode->mtime = buf->modtime;
    }

    return 0;
}

/* Write to a file */
int pifs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    printf("write: (path=%s, size=%zu, offset=%lld)\n", path, size, (long long)offset);

    // find the inode index for the file to be written to
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    pifs_inode_t *inode = &disk_memory[inode_index];

    // check if it's a directory, if so return error since we cannot write to directories
    if (inode->is_directory) {
        return -EISDIR;
    }

    // check if the write would go beyond the maximum data size for the file
    // if it does, we cut off the write to fit within the limit
    if ((size_t)offset + size > MAX_DATA_SIZE) {
        size = MAX_DATA_SIZE - (size_t)offset;
        // if offset is already beyond MAX_DATA_SIZE, then we cannot write anything
        if (size == 0) {
            return -ENOSPC;
        }
    }

    // perform the write by copying data from the input buffer to the inode's data at the specified offset
    memcpy(inode->data + offset, buf, size);

    // update the file size if the write extends beyond the current size
    if ((size_t)offset + size > inode->size) {
        inode->size = (uint32_t)((size_t)offset + size);
    }

    // set times to current time on write (null is now)
    inode->atime = time(NULL);
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

    // initialize the stat structure to zero to avoid returning garbage values for unused fields
    memset(stbuf, 0, sizeof(struct stat));

    // find the inode index for the given path
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    pifs_inode_t *inode = &disk_memory[inode_index];

    // set file type and permissions 
    if (inode->is_directory) {
        stbuf->st_mode = S_IFDIR | 0755; // directories have 755 permissions by default
        stbuf->st_nlink = 2; // directories have at least 2 links (itself and its parent)
    } else {
        stbuf->st_mode = S_IFREG | 0644; // regular files have 644 permissions by default
        stbuf->st_nlink = 1; // regular files have 1 link
        stbuf->st_size = inode->size; // set the file size from the inode's size
    }

    // set the access and modification times from the inode's atime and mtime
    stbuf->st_atime = inode->atime; 
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

    // find the inode index for the given path
    int dir_index = find_inode_index(path);
    if (dir_index == -1) {
        return -ENOENT;
    }

    // check if it's a directory
    if (!disk_memory[dir_index].is_directory) {
        return -ENOTDIR;
    }

    // add the "." and ".." entries for the current directory
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // find all children of the directory and add them to the buffer using the filler function
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
    (void) fi; // we are not using file handles in this simple implementation, so we ignore the fi argument
    printf("open: (path=%s)\n", path);

    // find the inode index for the given path
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    // check if it's a directory (cant open directory as files)
    if (disk_memory[inode_index].is_directory) {
        return -EISDIR;
    }

    return 0;
}

/*
 * Read size bytes from the given file into the buffer buf, beginning offset bytes into the file. See read(2) for full details.
 * Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. Required for any sensible filesystem.
*/
int pifs_read( const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi ) {
    (void) fi;
    printf("read: (path=%s)\n", path);

    // find the inode index for the given path
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    pifs_inode_t *inode = &disk_memory[inode_index];

    // check if it's a directory, if so return error since we cannot read from directories
    if (inode->is_directory) {
        return -EISDIR;
    }

    // if offset is beyond the end of the file, return 0 to indicate EOF
    if ((size_t)offset >= inode->size) {
        return 0;
    }

    // if the read would go beyond the end of the file, cut it off to fit within the file size
    if ((size_t)offset + size > inode->size) {
        size = inode->size - (size_t)offset;
    }

    // perform the read by copying data from the inode's data at the specified offset into the output buffer
    memcpy(buf, inode->data + offset, size);
    inode->atime = time(NULL);

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

    // validate that the inode still exists
    int inode_index = find_inode_index(path);
    if (inode_index == -1) {
        return -ENOENT;
    }

    // update access time on close
    pifs_inode_t *inode = &disk_memory[inode_index];
    if (inode->is_directory) {
        return -EISDIR;
    }

    inode->atime = time(NULL);
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

    // allocate memory for inodes to simulate disk storage; this will be loaded from the partition in a real implementation
    disk_memory = (pifs_inode_t *)malloc(MAX_FILES * sizeof(pifs_inode_t));
    if (disk_memory == NULL) {
        fprintf(stderr, "Failed to allocate memory for inodes\n");
        exit(EXIT_FAILURE);
    }
    
    // open partition for reading
    int fd = open(PARTITION_PATH, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open partition for reading");
        free(disk_memory);
        exit(EXIT_FAILURE);
    }

    // read the inodes from the partition into memory; i
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

    // write to partition writing the buffer with inodes
    int fd = open(PARTITION_PATH, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open partition for writing");
        exit(EXIT_FAILURE);
    }
    write(fd, disk_memory, MAX_FILES * sizeof(pifs_inode_t));
    close(fd);

    // free the allocated memory for inodes
    free(disk_memory);
    printf("successfully saved to partition and cleaned up memory\n");
}


int main( int argc, char *argv[] ) {
    fuse_main( argc, argv, &pifs_oper );

    return 0;
}
