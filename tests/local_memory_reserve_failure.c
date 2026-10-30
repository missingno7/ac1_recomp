/* Inject a reservation failure into the actual prepared memory implementation. */
#include <stddef.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
static LPVOID WINAPI failed_reservation(LPVOID address, SIZE_T bytes, DWORD flags, DWORD protection)
{
    (void)address;(void)bytes;(void)flags;(void)protection;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;
}
#define VirtualAlloc failed_reservation
#else
#include <sys/mman.h>
static void *failed_reservation(void *address,size_t bytes,int protection,int flags,int fd,off_t offset)
{
    (void)address;(void)bytes;(void)protection;(void)flags;(void)fd;(void)offset;
    errno=ENOMEM;return MAP_FAILED;
}
#define mmap failed_reservation
#endif
#include "guest_memory.c"

int main(void)
{
    guest_address_space *space=guest_address_space_create_direct();
    if(space){guest_address_space_destroy(space);return 1;}
    return 0;
}
