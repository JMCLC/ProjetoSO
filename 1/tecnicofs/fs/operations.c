#include "operations.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tfs_init() {
    state_init();

    /* create root inode */
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    state_destroy();
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}


int tfs_lookup(char const *name) {
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return find_in_dir(ROOT_DIR_INUM, name);
}

int tfs_open(char const *name, int flags) {
    int inum;
    size_t offset;

    /* Checks if the path name is valid */
    if (!valid_pathname(name)) {
        return -1;
    }

    inum = tfs_lookup(name);
    if (inum >= 0) {
        /* The file already exists */
        inode_t *inode = inode_get(inum);
        if (inode == NULL) {
            return -1;
        }

        /* Trucate (if requested) */
        if (flags & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                for (int i = 0; i < BLOCK_NUMBER; i++)
                    if (data_block_free(inode->i_data_blocks[i]) == -1)
                        return -1;
                inode->i_size = 0;
            }
        }
        /* Determine initial offset */
        if (flags & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
    } else if (flags & TFS_O_CREAT) {
        /* The file doesn't exist; the flags specify that it should be created*/
        /* Create inode */
        inum = inode_create(T_FILE);
        if (inum == -1) {
            return -1;
        }
        
        /* Add entry in the root directory */
        if (add_dir_entry(ROOT_DIR_INUM, inum, name + 1) == -1) {
            inode_delete(inum); 
            return -1;
        }
        offset = 0;
        
    } else {
        return -1;
    }

    /* Finally, add entry to the open file table and
     * return the corresponding handle */
    return add_to_open_file_table(inum, offset);

    /* Note: for simplification, if file was created with TFS_O_CREAT and there
     * is an error adding an entry to the open file table, the file is not
     * opened but it remains created */
}


int tfs_close(int fhandle) { return remove_from_open_file_table(fhandle); }

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    } 
    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        return -1;
    }
    /* Determine how many bytes to write */
    // if (to_write + file->of_offset > BLOCK_SIZE * BLOCK_NUMBER) {
    //     to_write = BLOCK_SIZE - file->of_offset;
    //     printf("%d, %d, %d \n", (int) to_write, BLOCK_SIZE, (int)file->of_offset);
    // }
    if (to_write > 0) {
        if (inode->i_size == 0) {
            /* If empty file, allocate new block */
            for (int i = 0; i < BLOCK_NUMBER; i++)
                inode->i_data_blocks[i] = data_block_alloc();
        }
        
        if ((int)file->of_offset < BLOCK_SIZE * BLOCK_NUMBER) {
            for (int i = 0; i < BLOCK_NUMBER; i++) {
                void *currentBlock = data_block_get(inode->i_data_blocks[i]);
                if (currentBlock == NULL)
                    return -1;
                if (inode->i_data_blocks_space[i] >= (int) to_write) {
                    memcpy(currentBlock + file->of_offset, buffer, to_write);
                    inode->i_data_blocks_space[i] -= (int) to_write;
                    break;
                }
            }
        } else {
            if (*(inode->i_extra_blocks) == 0) {
                *(inode->i_extra_blocks) = data_block_alloc();
                *(inode->i_extra_blocks_space) = BLOCK_SIZE;
            }
            void *currentBlock = NULL;
            while (*(inode->i_extra_blocks) != 0) {
                if (*(inode->i_extra_blocks_space) >= to_write) {
                    currentBlock = data_block_get(*(inode->i_extra_blocks));
                    break;
                }
                inode->i_extra_blocks_space++;
                inode->i_extra_blocks++;
            }
            if (currentBlock == NULL) {
                *(inode->i_extra_blocks + 1) = data_block_alloc();
                *(inode->i_extra_blocks_space + 1) = BLOCK_SIZE;
                inode->i_extra_blocks_space++;
                inode->i_extra_blocks++;
                currentBlock = data_block_get(*(inode->i_extra_blocks));
            }
            *(inode->i_extra_blocks_space) = *(inode->i_extra_blocks_space) - (int) to_write;
            memcpy(currentBlock + file->of_offset, buffer, to_write);
        }

        /* The offset associated with the file handle is
         * incremented accordingly */
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }
    return (ssize_t)to_write;
}


ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }
    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        return -1;
    }

    /* Determine how many bytes to read */
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (to_read > 0) {
        /* Perform the actual read */
        if ((int)file->of_offset < BLOCK_SIZE * BLOCK_NUMBER) {
            for (int i = 0; i < BLOCK_NUMBER; i++) {
                void *currentBlock = data_block_get(inode->i_data_blocks[i]);
                if (currentBlock == NULL)
                    return -1;
                if (BLOCK_SIZE - (int) file->of_offset >= (int) to_read) {
                    memcpy(buffer, currentBlock + file->of_offset, to_read);
                    break;
                }
            }
        } else {
            void *currentBlock = NULL;
            while (*(inode->i_extra_blocks) != 0) {
                if (*(inode->i_extra_blocks_space) >= to_read) {
                    currentBlock = data_block_get(*(inode->i_extra_blocks));
                    memcpy(buffer, currentBlock + file->of_offset, to_read);
                    break;
                }
                inode->i_extra_blocks_space++;
                inode->i_extra_blocks++;
            }
        }
        /* The offset associated with the file handle is
         * incremented accordingly */
        file->of_offset += to_read;
    }

    return (ssize_t)to_read;
}

int tfs_copy_to_external_fs(char const *source_path, char const *dest_path) {
    int source = tfs_open(source_path, 0);
    FILE *dest = fopen(dest_path, "a");
    if (source == -1 || dest == NULL)
        return -1;
    char sourceBuffer[BLOCK_SIZE];
    for (int i = 0; i < BLOCK_NUMBER; i++) { // Should be changed to work with 11 + blocks
        tfs_read(source, sourceBuffer, BLOCK_SIZE);
        fwrite(sourceBuffer, 1, BLOCK_SIZE, dest);
    }
    fclose(dest);
    return 0;
}