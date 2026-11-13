// PS Vita compat: ioctl() only for FIONBIO (non-blocking sockets).
#ifndef VITA_COMPAT_SYS_IOCTL_H
#define VITA_COMPAT_SYS_IOCTL_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef FIONBIO
#define FIONBIO 0x5421
#endif
#ifndef FIONREAD
#define FIONREAD 0x541B
#endif
int ioctl(int fd, unsigned long request, ...);
#ifdef __cplusplus
}
#endif
#endif
