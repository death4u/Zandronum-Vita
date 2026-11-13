// PS Vita compat: no memory mapping. Callers fall back to read().
#ifndef VITA_COMPAT_SYS_MMAN_H
#define VITA_COMPAT_SYS_MMAN_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
#ifdef __cplusplus
}
#endif
#endif
