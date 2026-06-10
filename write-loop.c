#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

// 32KB
#define FILE_SIZE 0x8000

int main(int argc, char **argv) {
    char *path;
    if (argc == 1) {
        path = "/mnt/host/llfs/mnt/out";
    } else {
        path = argv[1];
    }
    int fd = open(path, O_WRONLY | O_CREAT);
    if (fd < 0) {
        fprintf(stderr, "open error\n");
        return 1;
    }

    for (unsigned int i = 0;; i--) {
        if (lseek(fd, 0, 1) >= FILE_SIZE) {
            lseek(fd, 0, 0);
        }
        char buf[0x20];
        snprintf(buf, 0x1f, "%08x", i);
        write(fd, buf, 8);
        sync();
    }

    return 0;
}
