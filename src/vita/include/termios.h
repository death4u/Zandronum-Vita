// PS Vita compat: no terminal.
#ifndef VITA_COMPAT_TERMIOS_H
#define VITA_COMPAT_TERMIOS_H
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
#define NCCS 32
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_cc[NCCS]; };
#define ICANON 0x2
#define ECHO   0x8
#define TCSANOW 0
#define VMIN 6
#define VTIME 5
static inline int tcgetattr(int fd, struct termios *t) { (void)fd; (void)t; return -1; }
static inline int tcsetattr(int fd, int a, const struct termios *t) { (void)fd; (void)a; (void)t; return -1; }
#endif
