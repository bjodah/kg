#ifndef KG_DIRENT_H
#define KG_DIRENT_H

#ifdef _WIN32

#include <windows.h>

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_DIR 4
#endif

struct dirent {
	char d_name[MAX_PATH];
	unsigned char d_type;
};

typedef struct kg_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#endif

#endif
