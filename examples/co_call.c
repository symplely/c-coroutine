#include "../include/coroutine.h"

int co_main(int argc, char *argv[])
{
    uv_file fd = co_fs_open(__FILE__, O_RDONLY, 0);
    printf("%d", fd);
    return 0;
}
