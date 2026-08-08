#include "platform.h"
#ifdef _WIN32
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "kg_dirent.h"

int kg_optind = 1;
static DWORD kg_saved_input_mode;
static DWORD kg_saved_output_mode;
static UINT kg_saved_input_cp;
static UINT kg_saved_output_cp;
static int kg_console_saved;
int kg_strnlen(const char *text, size_t maxlen)
{
	const char *end = text;

	while ((size_t)(end - text) < maxlen && *end) {
		end++;
	}
	return (int)(end - text);
}
void kg_platform_init(void)
{
	const char *home = getenv("HOME");
	const char *userprofile = getenv("USERPROFILE");

	if ((!home || !*home) && userprofile && *userprofile) {
		(void)_putenv_s("HOME", userprofile);
	}
}

void kg_normalize_path(char *path)
{
	if (!path) {
		return;
	}
	for (; *path; path++) {
		if (*path == '\\') {
			*path = '/';
		}
	}
}
void *kg_memmem(const void *haystack, size_t haystack_size,
    const void *needle, size_t needle_size)
{
	const unsigned char *text = haystack;
	const unsigned char *want = needle;

	if (needle_size == 0) {
		return (void *)text;
	}
	if (needle_size > haystack_size) {
		return NULL;
	}
	for (size_t i = 0; i <= haystack_size - needle_size; i++) {
		if (memcmp(text + i, want, needle_size) == 0) {
			return (void *)(text + i);
		}
	}
	return NULL;
}
void *kg_aligned_alloc(size_t alignment, size_t size)
{
	return _aligned_malloc(size, alignment);
}

void kg_aligned_free(void *memory)
{
	_aligned_free(memory);
}
int kg_usleep(unsigned int usec)
{
	Sleep((usec + 999U) / 1000U);
	return 0;
}
int kg_gettimeofday(struct timeval *tv, void *timezone)
{
	FILETIME ft;
	ULARGE_INTEGER ticks;
	uint64_t micros;

	(void)timezone;
	if (!tv) {
		errno = EINVAL;
		return -1;
	}
	GetSystemTimeAsFileTime(&ft);
	ticks.LowPart = ft.dwLowDateTime;
	ticks.HighPart = ft.dwHighDateTime;
	/* FILETIME starts at 1601-01-01; timeval starts at 1970-01-01. */
	micros = (ticks.QuadPart - UINT64_C(116444736000000000)) / 10;
	tv->tv_sec = (long)(micros / UINT64_C(1000000));
	tv->tv_usec = (long)(micros % UINT64_C(1000000));
	return 0;
}

char *kg_realpath(const char *path, char *resolved)
{
	char *allocated = resolved;

	if (!path) {
		errno = EINVAL;
		return 0;
	}
	if (!allocated) {
		allocated = malloc(PATH_MAX);
		if (!allocated) {
			errno = ENOMEM;
			return 0;
		}
	}
	if (!_fullpath(allocated, path, PATH_MAX)) {
		if (!resolved) {
			free(allocated);
		}
		return 0;
	}
	for (char *p = allocated; *p; p++) {
		if (*p == '\\') {
			*p = '/';
		}
	}
	return allocated;
}

char *kg_getcwd(char *buffer, size_t size)
{
	char *result = _getcwd(buffer, (int)size);

	if (result) {
		for (char *p = result; *p; p++) {
			if (*p == '\\') {
				*p = '/';
			}
		}
	}
	return result;
}

int kg_getopt(int argc, char *const argv[], const char *options)
{
	static int option_offset;
	const char *choice;
	char option;

	if (kg_optind >= argc) {
		return -1;
	}
	if (option_offset == 0) {
		if (argv[kg_optind][0] != '-' || argv[kg_optind][1] == '\0') {
			return -1;
		}
		if (strcmp(argv[kg_optind], "--") == 0) {
			kg_optind++;
			return -1;
		}
		option_offset = 1;
	}
	option = argv[kg_optind][option_offset++];
	choice = strchr(options, option);
	if (option == '\0') {
		option_offset = 0;
		kg_optind++;
		return '?';
	}
	if (!choice) {
		if (argv[kg_optind][option_offset] == '\0') {
			option_offset = 0;
			kg_optind++;
		}
		return '?';
	}
	if (argv[kg_optind][option_offset] == '\0') {
		option_offset = 0;
		kg_optind++;
	}
	return option;
}

static HANDLE kg_fd_handle(int fd)
{
	intptr_t value = _get_osfhandle(fd);

	return value == (intptr_t)-1 ? INVALID_HANDLE_VALUE : (HANDLE)value;
}

int kg_poll(struct pollfd *fds, unsigned long count, int timeout_ms)
{
	DWORD start = GetTickCount();

	for (;;) {
		unsigned long ready = 0;
		unsigned long i;

		for (i = 0; i < count; i++) {
			HANDLE handle = kg_fd_handle(fds[i].fd);
			DWORD type;
			DWORD available = 0;

			fds[i].revents = 0;
			if (fds[i].fd < 0 || handle == INVALID_HANDLE_VALUE) {
				continue;
			}
			type = GetFileType(handle);
			if (type == FILE_TYPE_PIPE && (fds[i].events & POLLIN)) {
				if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) {
					DWORD error = GetLastError();

					if (error == ERROR_BROKEN_PIPE
					    || error == ERROR_HANDLE_EOF
					    || error == ERROR_NO_DATA) {
						fds[i].revents = POLLHUP;
					}
				} else if (available > 0) {
					fds[i].revents = POLLIN;
				}
			} else if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) {
				fds[i].revents = fds[i].events;
			}
			if (type == FILE_TYPE_PIPE && (fds[i].events & POLLOUT)) {
				/* Anonymous pipe writes are synchronous.  The write side is
				 * closed as soon as its input is exhausted, so it is safe to
				 * let the pump attempt the next bounded write. */
				fds[i].revents |= POLLOUT;
			}
			if (fds[i].revents) {
				ready++;
			}
		}
		if (ready || timeout_ms == 0) {
			return (int)ready;
		}
		if (timeout_ms > 0
		    && GetTickCount() - start >= (DWORD)timeout_ms) {
			return 0;
		}
		Sleep(1);
	}
}

int kg_fd_set_nonblocking(int fd)
{
	(void)fd;
	/* Windows pipes are synchronously read only after kg_poll() or
	 * kg_fd_read_available() has established that data is present. */
	return 0;
}

ssize_t kg_read(int fd, void *buf, size_t count)
{
	return (ssize_t)_read(fd, buf, (unsigned)count);
}

ssize_t kg_write(int fd, const void *buf, size_t count)
{
	return (ssize_t)_write(fd, buf, (unsigned)count);
}

int kg_fd_read_available(int fd, void *buf, size_t size)
{
	HANDLE handle = kg_fd_handle(fd);
	DWORD available = 0;
	DWORD got = 0;

	if (handle == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) {
		DWORD error = GetLastError();
		if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF
		    || error == ERROR_NO_DATA) {
			return 0;
		}
		errno = EIO;
		return -1;
	}
	if (available == 0) {
		return -2; /* Pipe is alive but has no bytes right now. */
	}
	if (available > size) {
		available = (DWORD)size;
	}
	if (!ReadFile(handle, buf, available, &got, NULL)) {
		if (GetLastError() == ERROR_BROKEN_PIPE) {
			return 0;
		}
		errno = EIO;
		return -1;
	}
	return (int)got;
}

int kg_console_enable(void)
{
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD input_mode, output_mode;

	if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE
	    || !GetConsoleMode(input, &input_mode)
	    || !GetConsoleMode(output, &output_mode)) {
		return -1;
	}
	kg_saved_input_mode = input_mode;
	kg_saved_output_mode = output_mode;
	kg_saved_input_cp = GetConsoleCP();
	kg_saved_output_cp = GetConsoleOutputCP();
	if (!SetConsoleMode(input,
		(input_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
		     | ENABLE_PROCESSED_INPUT))
		    | ENABLE_VIRTUAL_TERMINAL_INPUT)
	    || !SetConsoleMode(output,
		output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
		    | DISABLE_NEWLINE_AUTO_RETURN)) {
		return -1;
	}
	(void)SetConsoleCP(CP_UTF8);
	(void)SetConsoleOutputCP(CP_UTF8);
	kg_console_saved = 1;
	return 0;
}

void kg_console_disable(void)
{
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);

	if (kg_console_saved) {
		(void)SetConsoleCP(kg_saved_input_cp);
		(void)SetConsoleOutputCP(kg_saved_output_cp);
		(void)SetConsoleMode(input, kg_saved_input_mode);
		(void)SetConsoleMode(output, kg_saved_output_mode);
		kg_console_saved = 0;
	}
}

int kg_console_window_size(int fd, int *rows, int *cols)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	HANDLE handle = kg_fd_handle(fd);

	if (handle == INVALID_HANDLE_VALUE
	    || !GetConsoleScreenBufferInfo(handle, &info)) {
		return -1;
	}
	*rows = info.srWindow.Bottom - info.srWindow.Top + 1;
	*cols = info.srWindow.Right - info.srWindow.Left + 1;
	return 0;
}

int kg_pipe_cloexec(int fds[2])
{
	HANDLE handle;

	if (_pipe(fds, 4096, _O_BINARY) != 0) {
		return -1;
	}
	for (int i = 0; i < 2; i++) {
		handle = (HANDLE)_get_osfhandle(fds[i]);
		if (handle == INVALID_HANDLE_VALUE
		    || !SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
			_close(fds[0]);
			_close(fds[1]);
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

int kg_mkstemp(char *template_name)
{
	if (_mktemp_s(template_name, PATH_MAX + 32) != 0) {
		errno = EINVAL;
		return -1;
	}
	return _open(template_name,
	    _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
}

int kg_rename(const char *oldpath, const char *newpath)
{
	if (MoveFileExA(oldpath, newpath,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
		return 0;
	}
	errno = EACCES;
	return -1;
}

int kg_fsync(int fd)
{
	return _commit(fd);
}

ssize_t kg_getline(char **line, size_t *capacity, FILE *stream)
{
	size_t length = 0;
	int c;

	if (!line || !capacity || !stream) {
		errno = EINVAL;
		return -1;
	}
	for (;;) {
		if (length + 1 >= *capacity) {
			size_t new_capacity = *capacity ? *capacity * 2 : 256;
			char *grown = realloc(*line, new_capacity);

			if (!grown) {
				errno = ENOMEM;
				return -1;
			}
			*line = grown;
			*capacity = new_capacity;
		}
		c = fgetc(stream);
		if (c == EOF) {
			if (length == 0) {
				return -1;
			}
			break;
		}
		(*line)[length++] = (char)c;
		if (c == '\n') {
			break;
		}
	}
	(*line)[length] = '\0';
	return (ssize_t)length;
}

struct kg_dir {
	HANDLE handle;
	WIN32_FIND_DATAA data;
	char pattern[MAX_PATH];
	int first;
	struct dirent entry;
};

DIR *opendir(const char *path)
{
	DIR *dir;
	size_t length;

	if (!path) {
		errno = EINVAL;
		return NULL;
	}
	dir = calloc(1, sizeof(*dir));
	if (!dir) {
		errno = ENOMEM;
		return NULL;
	}
	length = strlen(path);
	if (length + 2 >= sizeof(dir->pattern)) {
		free(dir);
		errno = ENAMETOOLONG;
		return NULL;
	}
	memcpy(dir->pattern, path, length);
	while (length > 0 && (dir->pattern[length - 1] == '/'
			|| dir->pattern[length - 1] == '\\')) {
		dir->pattern[--length] = '\0';
	}
	dir->pattern[length++] = '\\';
	dir->pattern[length++] = '*';
	dir->pattern[length] = '\0';
	dir->handle = FindFirstFileA(dir->pattern, &dir->data);
	if (dir->handle == INVALID_HANDLE_VALUE) {
		free(dir);
		errno = ENOENT;
		return NULL;
	}
	dir->first = 1;
	return dir;
}

struct dirent *readdir(DIR *dir)
{
	if (!dir) {
		errno = EBADF;
		return NULL;
	}
	if (!dir->first && !FindNextFileA(dir->handle, &dir->data)) {
		return NULL;
	}
	dir->first = 0;
	strncpy(dir->entry.d_name, dir->data.cFileName,
	    sizeof(dir->entry.d_name) - 1);
	dir->entry.d_name[sizeof(dir->entry.d_name) - 1] = '\0';
	dir->entry.d_type =
	    (dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR
							 : DT_UNKNOWN;
	return &dir->entry;
}

int closedir(DIR *dir)
{
	if (!dir) {
		errno = EBADF;
		return -1;
	}
	FindClose(dir->handle);
	free(dir);
	return 0;
}

#else

int kg_platform_anchor(void)
{
	return 0;
}

#endif
