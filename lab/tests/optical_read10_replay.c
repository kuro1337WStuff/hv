/* Host-only trial 134 replay. Never send this CDB to a device. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#define file_stat _stat64
#define file_fstat _fstat64
#define file_close _close
#define regular(mode) (((mode) & _S_IFMT) == _S_IFREG)
#else
#include <unistd.h>
#define file_stat stat
#define file_fstat fstat
#define file_close close
#define regular(mode) S_ISREG(mode)
#endif

#include "../optical_read10.h"

enum { BLOCK_BYTES = 2048, TRANSFER_BYTES = 2 * BLOCK_BYTES };
#define MEDIA_BYTES INT64_C(3839819776)

typedef struct {
    int fd;
    uint8_t bytes[TRANSFER_BYTES];
} FileView;

static int open_image(const char *path)
{
    struct file_stat image;
    int fd;
    if (file_stat(path, &image) != 0 || !regular(image.st_mode) || image.st_size != MEDIA_BYTES)
        return -1;
#ifdef _WIN32
    if (_sopen_s(&fd, path, _O_RDONLY | _O_BINARY, _SH_DENYWR, 0) != 0)
        return -1;
#else
    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY);
    _Static_assert(sizeof(off_t) >= 8, "64-bit file offsets required");
    if (fd < 0)
        return -1;
#endif
    if (file_fstat(fd, &image) != 0 || !regular(image.st_mode) || image.st_size != MEDIA_BYTES) {
        file_close(fd);
        return -1;
    }
    return fd;
}

static int output_pipe(void)
{
    struct file_stat sink;
#ifdef _WIN32
    return file_fstat(1, &sink) == 0 && (sink.st_mode & _S_IFMT) == _S_IFIFO &&
           _setmode(1, _O_BINARY) != -1;
#else
    return fstat(STDOUT_FILENO, &sink) == 0 && S_ISFIFO(sink.st_mode);
#endif
}

static const uint8_t *file_view(void *context, uint32_t lba, size_t bytes)
{
    FileView *file = context;
    int64_t offset = (int64_t)lba * BLOCK_BYTES;
    if (bytes != TRANSFER_BYTES || offset > MEDIA_BYTES - TRANSFER_BYTES)
        return NULL;
#ifdef _WIN32
    if (_lseeki64(file->fd, offset, SEEK_SET) != offset ||
        _read(file->fd, file->bytes, TRANSFER_BYTES) != TRANSFER_BYTES)
        return NULL;
#else
    if (pread(file->fd, file->bytes, TRANSFER_BYTES, offset) != TRANSFER_BYTES)
        return NULL;
#endif
    return file->bytes;
}

int main(int argc, char **argv)
{
    static const uint8_t cdb[] = {0x28, 0x00, 0x00, 0x03, 0xc5, 0x15, 0x00, 0x00, 0x02, 0x00};
    FileView file;
    uint8_t response[TRANSFER_BYTES];
    if (argc != 2 || !output_pipe()) {
        fputs("usage: optical-read10-replay IMAGE (requires a pipe on stdout)\n", stderr);
        return 1;
    }
    file.fd = open_image(argv[1]);
    if (file.fd < 0) {
        fputs("replay requires a readable regular ISO of the trial 134 size\n", stderr);
        return 1;
    }
    bool accepted = optical_read10(cdb, sizeof(cdb), MEDIA_BYTES / BLOCK_BYTES,
                                   file_view, &file, response, sizeof(response));
    if (file_close(file.fd) != 0 || !accepted) {
        fputs("fixed READ(10) refused\n", stderr);
        return 1;
    }
    if (fwrite(response, 1, sizeof(response), stdout) != sizeof(response) || fflush(stdout) != 0) {
        fputs("cannot return the complete READ(10) response\n", stderr);
        return 1;
    }
    return 0;
}
