/* mbed Microcontroller Library
 * Copyright (c) 2006-2012 ARM Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "mbed.h"
#include "SPIFFileSystem.h"
#include "errno.h"
#include "spiffs_nucleus.h"


////// Converting functions //////
static int spiffs_toerror(int err) {
    switch (err) {
        case SPIFFS_OK:                         return 0;
        case SPIFFS_ERR_NOT_MOUNTED:            return -EINVAL;
        case SPIFFS_ERR_FULL:                   return -ENOSPC;
        case SPIFFS_ERR_NOT_FOUND:              return -ENOENT;
        case SPIFFS_ERR_END_OF_OBJECT:          return -EOF;
        case SPIFFS_ERR_DELETED:                return -ENOENT;
        case SPIFFS_ERR_OUT_OF_FILE_DESCS:      return -ENOMEM;
        case SPIFFS_ERR_FILE_CLOSED:            return -EINVAL;
        case SPIFFS_ERR_FILE_DELETED:           return -ENOENT;
        case SPIFFS_ERR_BAD_DESCRIPTOR:         return -EBADF;
        case SPIFFS_ERR_NOT_WRITABLE:           return -EINVAL;
        case SPIFFS_ERR_NOT_READABLE:           return -EINVAL;
        case SPIFFS_ERR_CONFLICTING_NAME:       return -EEXIST;
        case SPIFFS_ERR_NOT_CONFIGURED:         return -EINVAL;
        case SPIFFS_ERR_NOT_A_FS:               return -ENODEV;
        case SPIFFS_ERR_MOUNTED:                return -EINVAL;
        case SPIFFS_ERR_ERASE_FAIL:             return -EIO;
        case SPIFFS_ERR_NO_DELETED_BLOCKS:      return -ENOSPC;
        case SPIFFS_ERR_FILE_EXISTS:            return -EEXIST;
        case SPIFFS_ERR_NOT_A_FILE:             return -EISDIR;
        case SPIFFS_ERR_NAME_TOO_LONG:          return -ENAMETOOLONG;
        default: return err;
    }
}

static spiffs_flags spiffs_fromflags(int flags) {
    return (
        (((flags & 3) == O_RDONLY) ? SPIFFS_O_RDONLY : 0) |
        (((flags & 3) == O_WRONLY) ? SPIFFS_O_WRONLY : 0) |
        (((flags & 3) == O_RDWR)   ? SPIFFS_O_RDWR   : 0) |
        ((flags & O_CREAT)  ? SPIFFS_O_CREAT  : 0) |
        ((flags & O_EXCL)   ? SPIFFS_O_EXCL   : 0) |
        ((flags & O_TRUNC)  ? SPIFFS_O_TRUNC  : 0) |
        ((flags & O_APPEND) ? SPIFFS_O_APPEND : 0));
}

static int spiffs_fromwhence(int whence) {
    switch (whence) {
        case SEEK_SET: return SPIFFS_SEEK_SET;
        case SEEK_CUR: return SPIFFS_SEEK_CUR;
        case SEEK_END: return SPIFFS_SEEK_END;
        default: return 0;
    }
}

static int spiffs_tomode(spiffs_obj_type type) {
    int mode = S_IRWXU | S_IRWXG | S_IRWXO;
    switch (type) {
        case SPIFFS_TYPE_DIR:       return mode | S_IFDIR;
        case SPIFFS_TYPE_FILE:      return mode | S_IFREG;
        case SPIFFS_TYPE_HARD_LINK: return mode | S_IFLNK;
        case SPIFFS_TYPE_SOFT_LINK: return mode | S_IFLNK;
        default: return 0;
    }
}

static int spiffs_totype(spiffs_obj_type type) {
    switch (type) {
        case SPIFFS_TYPE_DIR:       return DT_DIR;
        case SPIFFS_TYPE_FILE:      return DT_REG;
        case SPIFFS_TYPE_HARD_LINK: return DT_LNK;
        case SPIFFS_TYPE_SOFT_LINK: return DT_LNK;
        default: return DT_UNKNOWN;
    }
}


////// Block device operations //////
static s32_t spiffs_bd_read(spiffs *spiffs, u32_t addr, u32_t size, u8_t *buffer) {
    BlockDevice *bd = (BlockDevice *)spiffs->user_data;
    return bd->read(buffer, addr, size);
}

static s32_t spiffs_bd_write(spiffs *spiffs, u32_t addr, u32_t size, u8_t *buffer) {
    BlockDevice *bd = (BlockDevice *)spiffs->user_data;
    return bd->program(buffer, addr, size);
}

static s32_t spiffs_bd_erase(spiffs *spiffs, u32_t addr, u32_t size) {
    BlockDevice *bd = (BlockDevice *)spiffs->user_data;
    return bd->erase(addr, size);
}


////// Generic filesystem operations //////

// Filesystem implementation (See SPIFFilySystem.h)
SPIFFileSystem::SPIFFileSystem(const char *name, BlockDevice *bd,
        u32_t log_page_size, u32_t log_block_size)
        : FileSystem(name)
        , _work_buffer(NULL), _fd_buffer(NULL), _cache_buffer(NULL)
        , _log_page_size(log_page_size)
        , _log_block_size(log_block_size) {
    if (bd) {
        mount(bd);
    }
}

SPIFFileSystem::~SPIFFileSystem()
{
    // nop if unmounted
    unmount();
}

int SPIFFileSystem::mount(BlockDevice *bd, bool check) {
    _bd = bd;
    int err = _bd->init();
    if (err) {
        return err;
    }

    // Setup config
    memset(&_config, 0, sizeof(_config));
    _config.phys_size = bd->size();
    _config.phys_addr = 0;
    _config.phys_erase_block = bd->get_erase_size();
    _config.log_block_size = bd->get_erase_size();
    if (_config.log_block_size < _log_block_size) {
        _config.log_block_size = _log_block_size;
    }

    _config.log_page_size = bd->get_program_size();
    if (_config.log_page_size < _log_page_size) {
        _config.log_page_size = _log_page_size;
    }

    // Setup block device
    _spiffs.user_data = bd;
    _config.hal_read_f = spiffs_bd_read;
    _config.hal_write_f = spiffs_bd_write;
    _config.hal_erase_f = spiffs_bd_erase;

    // Allocate buffers
    _work_buffer = (u8_t*)malloc(2*_config.log_page_size);
    if (!_work_buffer) {
        return -ENOMEM;
    }

    uint32_t fd_buffer_size = MBED_SPIFFS_FILEDESCS * sizeof(spiffs_fd);
    _fd_buffer = (u8_t*)malloc(fd_buffer_size);
    if (!_fd_buffer) {
        return -ENOMEM;
    }

    uint32_t cache_buffer_size = sizeof(spiffs_cache) + MBED_SPIFFS_CACHEPAGES*(
            sizeof(spiffs_cache_page) + _config.log_page_size);
    _cache_buffer = (u8_t*)malloc(cache_buffer_size);
    if (!_cache_buffer) {
        return -ENOMEM;
    }

    err = SPIFFS_mount(&_spiffs, &_config,
        _work_buffer,
        _fd_buffer, fd_buffer_size,
        _cache_buffer, cache_buffer_size,
        NULL);
    if (err) {
        return spiffs_toerror(err);
    }

    if (check) {
        err = SPIFFS_check(&_spiffs);
        if (err) {
            return err;
        }
    }

    return 0;
}

int SPIFFileSystem::unmount()
{
    if (_work_buffer) {
        free(_work_buffer);
        _work_buffer = NULL;
    }

    if (_fd_buffer) {
        free(_fd_buffer);
        _fd_buffer = NULL;
    }

    if (_cache_buffer) {
        free(_cache_buffer);
        _cache_buffer = NULL;
    }

    if (_bd) {
        SPIFFS_unmount(&_spiffs);

        int err = _bd->deinit();
        if (err) {
            return err;
        }

        _bd = NULL;
    }
    
    return 0;
}

int SPIFFileSystem::format(BlockDevice *bd,
        u32_t log_page_size, u32_t log_block_size) {
    SPIFFileSystem spiffs(NULL, NULL, log_page_size, log_block_size);

    int err = spiffs.mount(bd, false);
    if (err < 0 && err != -ENODEV) {
        return spiffs_toerror(err);
    }

    if (err != SPIFFS_ERR_NOT_A_FS) {
        SPIFFS_unmount(&spiffs._spiffs);
    }

    err = SPIFFS_format(&spiffs._spiffs);
    if (err) {
        return spiffs_toerror(err);
    }

    return spiffs.unmount();
}

int SPIFFileSystem::remove(const char *path) {
    int err = SPIFFS_remove(&_spiffs, path);
    return spiffs_toerror(err);
}

int SPIFFileSystem::rename(const char *oldpath, const char *newpath) {
    int err = SPIFFS_rename(&_spiffs, oldpath, newpath);
    return spiffs_toerror(err);
}

int SPIFFileSystem::stat(const char *path, struct stat *st) {
    spiffs_stat s;
    int err = SPIFFS_stat(&_spiffs, path, &s);
    if (err) {
        return spiffs_toerror(err);
    }

    st->st_size = s.size;
    st->st_mode = spiffs_tomode(s.type);
    return 0;
}


////// File operations //////
int SPIFFileSystem::file_open(fs_file_t *file, const char *path, int flags) {
    spiffs_file f = SPIFFS_open(&_spiffs, path, spiffs_fromflags(flags), 0);
    if (f < 0) {
        return spiffs_toerror(SPIFFS_errno(&_spiffs));
    }

    *file = (void*)(intptr_t)f;
    return 0;
}

int SPIFFileSystem::file_close(fs_file_t file) {
    spiffs_file f = (spiffs_file)(intptr_t)file;
    int err = SPIFFS_close(&_spiffs, f);
    return spiffs_toerror(err);
}

ssize_t SPIFFileSystem::file_read(fs_file_t file, void *buffer, size_t len) {
    spiffs_file f = (spiffs_file)(intptr_t)file;
    int res = SPIFFS_read(&_spiffs, f, (u8_t*)buffer, len);
    return spiffs_toerror(res);
}

ssize_t SPIFFileSystem::file_write(fs_file_t file, const void *buffer, size_t len) {
    spiffs_file f = (spiffs_file)(intptr_t)file;
    int res = SPIFFS_write(&_spiffs, f, (u8_t*)buffer, len);
    return spiffs_toerror(res);
}

off_t SPIFFileSystem::file_seek(fs_file_t file, off_t offset, int whence) {
    spiffs_file f = (spiffs_file)(intptr_t)file;
    off_t res = SPIFFS_lseek(&_spiffs, f, offset, spiffs_fromwhence(whence));
    return spiffs_toerror(res);
}

off_t SPIFFileSystem::file_tell(fs_file_t file) {
    spiffs_file f = (spiffs_file)(intptr_t)file;
    off_t res = SPIFFS_tell(&_spiffs, f);
    return spiffs_toerror(res);
}

off_t SPIFFileSystem::file_size(fs_file_t file) {
    spiffs_file f = (spiffs_file)(intptr_t)file;
    spiffs_stat s;
    int err = SPIFFS_fstat(&_spiffs, f, &s);
    if (err) {
        return spiffs_toerror(err);
    }

    return spiffs_toerror(s.size);
}


////// Dir operations //////
int SPIFFileSystem::dir_open(fs_dir_t *dir, const char *path) {
    spiffs_DIR *d = new spiffs_DIR;
    *dir = SPIFFS_opendir(&_spiffs, path, d);
    if (!*dir) {
        delete d;
        return spiffs_toerror(SPIFFS_errno(&_spiffs));
    }

    return 0;
}

int SPIFFileSystem::dir_close(fs_dir_t dir) {
    spiffs_DIR *d = (spiffs_DIR *)dir;
    int err = SPIFFS_closedir(d);
    delete d;
    return spiffs_toerror(err);
}

ssize_t SPIFFileSystem::dir_read(fs_dir_t dir, struct dirent *ent) {
    spiffs_DIR *d = (spiffs_DIR *)dir;
    struct spiffs_dirent e;
    struct spiffs_dirent *done = SPIFFS_readdir(d, &e);
    if (!done) {
        return 0;
    }

    ent->d_type = spiffs_totype(e.type);
    strcpy(ent->d_name, (const char*)e.name);
    return 1;
}

