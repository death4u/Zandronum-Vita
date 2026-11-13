// PS Vita: POSIX pieces newlib does not provide.
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>

extern "C" {

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	errno = ENOSYS;
	return MAP_FAILED;
}

int munmap(void *addr, size_t len)
{
	errno = ENOSYS;
	return -1;
}

int mprotect(void *addr, size_t len, int prot)
{
	return 0;
}

}

// ---------------------------------------------------------------------------
// fts(3): directory walks for ScanDirectory and directory resources.
// ---------------------------------------------------------------------------
#include <fts.h>
#include <dirent.h>
#include <string.h>
#include <string>
#include <vector>

struct _fts
{
	std::vector<FTSENT *> entries;
	size_t next;
	std::vector<std::string> skipped;
};

static FTSENT *vita_fts_make(const std::string &path, int level, bool isdir, const struct stat &st)
{
	FTSENT *e = new FTSENT;
	memset(e, 0, sizeof(*e));
	e->fts_path = strdup(path.c_str());
	e->fts_accpath = e->fts_path;
	const char *slash = strrchr(e->fts_path, '/');
	e->fts_name = (char *)(slash && slash[1] ? slash + 1 : e->fts_path);
	e->fts_level = level;
	e->fts_info = isdir ? FTS_D : FTS_F;
	e->fts_stat = st;
	e->fts_statp = &e->fts_stat;
	e->fts_pathlen = strlen(e->fts_path);
	e->fts_namelen = strlen(e->fts_name);
	return e;
}

static void vita_fts_walk(FTS *fts, std::string path, int level)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return;
	bool isdir = S_ISDIR(st.st_mode);
	fts->entries.push_back(vita_fts_make(path, level, isdir, st));
	if (!isdir)
		return;
	DIR *d = opendir(path.c_str());
	if (d == NULL)
		return;
	std::vector<std::string> names;
	struct dirent *de;
	while ((de = readdir(d)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		names.push_back(de->d_name);
	}
	closedir(d);
	std::string base = path;
	if (!base.empty() && base[base.size() - 1] != '/')
		base += '/';
	for (size_t i = 0; i < names.size(); i++)
		vita_fts_walk(fts, base + names[i], level + 1);
}

extern "C" FTS *fts_open(char * const *argv, int options, int (*compar)(const FTSENT **, const FTSENT **))
{
	FTS *fts = new FTS;
	fts->next = 0;
	for (int i = 0; argv[i] != NULL; i++)
	{
		std::string p = argv[i];
		while (p.size() > 1 && p[p.size() - 1] == '/' && p[p.size() - 2] != ':')
			p.erase(p.size() - 1);
		vita_fts_walk(fts, p, 0);
	}
	return fts;
}

extern "C" FTSENT *fts_read(FTS *fts)
{
	while (fts->next < fts->entries.size())
	{
		FTSENT *e = fts->entries[fts->next++];
		bool skip = false;
		for (size_t i = 0; i < fts->skipped.size() && !skip; i++)
		{
			const std::string &s = fts->skipped[i];
			skip = strncmp(e->fts_path, s.c_str(), s.size()) == 0;
		}
		if (!skip)
			return e;
	}
	return NULL;
}

extern "C" int fts_set(FTS *fts, FTSENT *ent, int instr)
{
	if (instr == FTS_SKIP && ent->fts_info == FTS_D)
		fts->skipped.push_back(std::string(ent->fts_path) + "/");
	return 0;
}

extern "C" int fts_close(FTS *fts)
{
	for (size_t i = 0; i < fts->entries.size(); i++)
	{
		free(fts->entries[i]->fts_path);
		delete fts->entries[i];
	}
	delete fts;
	return 0;
}

// ---------------------------------------------------------------------------
// ioctl(FIONBIO) for non-blocking sockets.
// ---------------------------------------------------------------------------
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdarg.h>

extern "C" int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (request == FIONBIO)
	{
		int on = arg ? *(int *)arg : 0;
		return setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &on, sizeof(on));
	}
	errno = ENOSYS;
	return -1;
}

// ---------------------------------------------------------------------------
// Process and timer calls that do not exist on the Vita. Callers already
// handle failure (polled timer, no external Timidity, no crash debugger).
// ---------------------------------------------------------------------------
#include <sys/time.h>
#include <sys/utsname.h>
#include <wordexp.h>
#include <fnmatch.h>
#include <ctype.h>

extern "C" {

int setitimer(int which, const struct itimerval *value, struct itimerval *ovalue)
{
	errno = ENOSYS;
	return -1;
}

int wordexp(const char *words, wordexp_t *pwordexp, int flags)
{
	return WRDE_NOSPACE;
}

void wordfree(wordexp_t *pwordexp)
{
}

int execvp(const char *file, char *const argv[])
{
	errno = ENOSYS;
	return -1;
}

int dup2(int oldfd, int newfd)
{
	errno = ENOSYS;
	return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
	errno = ECHILD;
	return -1;
}

// Shell-style pattern match: *, ? and [...] (with ! or ^ negation and ranges).
static int vita_fnmatch(const char *p, const char *s, int flags)
{
	for (; *p; p++, s++)
	{
		switch (*p)
		{
		case '*':
			while (p[1] == '*')
				p++;
			if (p[1] == '\0')
				return 0;
			for (; *s; s++)
				if (vita_fnmatch(p + 1, s, flags) == 0)
					return 0;
			return vita_fnmatch(p + 1, s, flags);
		case '?':
			if (*s == '\0')
				return FNM_NOMATCH;
			break;
		case '[':
		{
			if (*s == '\0')
				return FNM_NOMATCH;
			const char *q = p + 1;
			bool negate = (*q == '!' || *q == '^');
			if (negate)
				q++;
			bool match = false;
			bool first = true;
			while (*q && (first || *q != ']'))
			{
				first = false;
				char lo = *q, hi = *q;
				if (q[1] == '-' && q[2] && q[2] != ']')
				{
					hi = q[2];
					q += 2;
				}
				if ((unsigned char)*s >= (unsigned char)lo && (unsigned char)*s <= (unsigned char)hi)
					match = true;
				q++;
			}
			if (*q != ']')
			{	// no closing bracket: treat '[' literally
				if (*s != '[')
					return FNM_NOMATCH;
				break;
			}
			if (match == negate)
				return FNM_NOMATCH;
			p = q;
			break;
		}
		case '\\':
			if (!(flags & FNM_NOESCAPE) && p[1])
				p++;
			// fall through
		default:
			if (*p != *s)
				return FNM_NOMATCH;
			break;
		}
	}
	return *s == '\0' ? 0 : FNM_NOMATCH;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
	return vita_fnmatch(pattern, string, flags);
}

}

int uname(struct utsname *name)
{
	memset(name, 0, sizeof(*name));
	strcpy(name->sysname, "PS Vita");
	strcpy(name->nodename, "vita");
	strcpy(name->release, "3.60");
	strcpy(name->version, "HENkaku");
	strcpy(name->machine, "armv7l");
	return 0;
}
