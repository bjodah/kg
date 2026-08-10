#ifndef KG_PLATFORM_H
#define KG_PLATFORM_H

/* The editor's small operating-system seam.  The normal build uses the
 * POSIX interfaces directly; the Windows build keeps the editor's byte
 * oriented core unchanged and supplies the few terminal, directory and
 * child-process primitives that do not exist in the Microsoft CRT. */

#ifdef _WIN32

#ifndef _CRT_DECLARE_NONSTDC_NAMES
#define _CRT_DECLARE_NONSTDC_NAMES 1
#endif

#include <fcntl.h>
#include <direct.h>
#include <io.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <windows.h>

#ifdef FILE_UNKNOWN
#undef FILE_UNKNOWN
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef ssize_t
typedef intptr_t ssize_t;
#endif

/* MSVC's C mode does not provide the POSIX process types.  A Windows
 * process handle is carried in pid_t by process.c; no caller observes it as
 * a Unix PID. */
typedef intptr_t pid_t;
typedef int mode_t;

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) 0
#endif

#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef ESTALE
#define ESTALE EINVAL
#endif

struct pollfd {
	int fd;
	short events;
	short revents;
};

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010

int kg_poll(struct pollfd *fds, unsigned long count, int timeout_ms);
void kg_platform_init(void);
void kg_normalize_path(char *path);
int kg_gettimeofday(struct timeval *tv, void *timezone);
char *kg_getcwd(char *buffer, size_t size);
char *kg_realpath(const char *path, char *resolved);
int kg_getopt(int argc, char *const argv[], const char *options);
extern int kg_optind;
int kg_strnlen(const char *text, size_t maxlen);
void *kg_aligned_alloc(size_t alignment, size_t size);
void kg_aligned_free(void *memory);
void *kg_memmem(const void *haystack, size_t haystack_size,
    const void *needle, size_t needle_size);
int kg_usleep(unsigned int usec);
int kg_console_enable(void);
void kg_console_disable(void);
int kg_console_window_size(int fd, int *rows, int *cols);
int kg_fd_read_available(int fd, void *buf, size_t size);
int kg_fd_set_nonblocking(int fd);
int kg_pipe_cloexec(int fds[2]);
ssize_t kg_read(int fd, void *buf, size_t count);
ssize_t kg_write(int fd, const void *buf, size_t count);
int kg_mkstemp(char *template_name);
int kg_rename(const char *oldpath, const char *newpath);
int kg_fsync(int fd);
ssize_t kg_getline(char **line, size_t *capacity, FILE *stream);

#define read kg_read
#define write kg_write
#define close _close
#define isatty _isatty
#define fileno _fileno
#define access _access
#define chdir _chdir
#define getcwd kg_getcwd
#define unlink _unlink
#define rmdir _rmdir
#define strdup _strdup
#define lstat stat
#define strnlen kg_strnlen
#define aligned_alloc kg_aligned_alloc
#define memmem kg_memmem
#define usleep kg_usleep
#define gettimeofday kg_gettimeofday
#define realpath kg_realpath
#define poll kg_poll
#define mkstemp kg_mkstemp
#define rename kg_rename
#define fsync kg_fsync
#define getline kg_getline

#ifndef R_OK
#define R_OK 4
#endif
#ifndef F_OK
#define F_OK 0
#endif

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#else

/* POSIX already has the types the Windows branch above has to typedef
 * itself, so the seam is one include wide here.  It is an export because
 * platform.h is where the rest of kg asks for pid_t and ssize_t: a module
 * that includes only this header is complete in both configurations, and
 * include-what-you-use, which sees just this branch, would otherwise send
 * every such module to <sys/types.h> and leave its Windows build without
 * the types. */
#include <sys/types.h> // IWYU pragma: export

#endif

#endif
