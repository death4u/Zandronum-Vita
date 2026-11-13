// PS Vita compat: minimal fts(3) (pre-order walk, FTS_SKIP supported).
#ifndef VITA_COMPAT_FTS_H
#define VITA_COMPAT_FTS_H
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FTS_LOGICAL  0x0002
#define FTS_PHYSICAL 0x0010
#define FTS_NOCHDIR  0x0004
#define FTS_D   1
#define FTS_F   8
#define FTS_DP  6
#define FTS_NS  10
#define FTS_SKIP 4
typedef struct _ftsent {
	char *fts_path;
	char *fts_name;
	char *fts_accpath;
	short fts_level;
	unsigned short fts_info;
	struct stat *fts_statp;
	size_t fts_pathlen;
	size_t fts_namelen;
	struct stat fts_stat;
} FTSENT;
typedef struct _fts FTS;
FTS *fts_open(char * const *argv, int options, int (*compar)(const FTSENT **, const FTSENT **));
FTSENT *fts_read(FTS *fts);
int fts_set(FTS *fts, FTSENT *ent, int instr);
int fts_close(FTS *fts);
#ifdef __cplusplus
}
#endif
#endif
