#include "vita_files.h"

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define VITA_MAX_SEARCHES 8
#define VITA_PATH_MAX     512

struct VitaSearch
{
    DIR *directory;
    char path[VITA_PATH_MAX];
    char pattern[128];
    bool inUse;
};

static VitaSearch s_searches[VITA_MAX_SEARCHES];

// the engine builds paths with either separator, and only '/' opens anything here
static void VitaFiles_Normalize(const char *source, char *destination, uint32_t size)
{
    uint32_t i = 0;
    for (; source[i] && i + 1 < size; ++i)
        destination[i] = source[i] == '\\' ? '/' : source[i];
    destination[i] = 0;
}

// only '*' and '?' appear in the engine's specifications
static bool VitaFiles_Matches(const char *pattern, const char *name)
{
    while (*pattern)
    {
        if (*pattern == '*')
        {
            ++pattern;
            if (!*pattern)
                return true;
            for (const char *at = name; *at; ++at)
            {
                if (VitaFiles_Matches(pattern, at))
                    return true;
            }
            return false;
        }
        if (!*name)
            return false;
        if (*pattern != '?' && *pattern != *name)
            return false;
        ++pattern;
        ++name;
    }
    return !*name;
}

static bool VitaFiles_Fill(VitaSearch *search, _finddata64i32_t *data)
{
    struct dirent *entry;
    while ((entry = readdir(search->directory)) != NULL)
    {
        if (!VitaFiles_Matches(search->pattern, entry->d_name))
            continue;

        memset(data, 0, sizeof(*data));
        strncpy(data->name, entry->d_name, sizeof(data->name) - 1);

        char full[VITA_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", search->path, entry->d_name);

        struct stat info;
        if (stat(full, &info) == 0)
        {
            data->size = (long)info.st_size;
            data->time_write = (long)info.st_mtime;
            if (S_ISDIR(info.st_mode))
                data->attrib |= _A_SUBDIR;
        }
        return true;
    }
    return false;
}

extern "C" {

intptr_t _findfirst64i32(const char *specification, _finddata64i32_t *data)
{
    char normalized[VITA_PATH_MAX];
    VitaFiles_Normalize(specification, normalized, sizeof(normalized));

    for (uint32_t i = 0; i < VITA_MAX_SEARCHES; ++i)
    {
        if (s_searches[i].inUse)
            continue;

        VitaSearch *search = &s_searches[i];
        memset(search, 0, sizeof(*search));

        char *slash = strrchr(normalized, '/');
        if (slash)
        {
            *slash = 0;
            strncpy(search->path, normalized, sizeof(search->path) - 1);
            strncpy(search->pattern, slash + 1, sizeof(search->pattern) - 1);
        }
        else
        {
            strcpy(search->path, ".");
            strncpy(search->pattern, normalized, sizeof(search->pattern) - 1);
        }
        if (!search->pattern[0])
            strcpy(search->pattern, "*");

        search->directory = opendir(search->path);
        if (!search->directory)
            return -1;

        if (!VitaFiles_Fill(search, data))
        {
            closedir(search->directory);
            return -1;
        }

        search->inUse = true;
        return (intptr_t)(i + 1);
    }
    return -1;
}

int _findnext64i32(intptr_t handle, _finddata64i32_t *data)
{
    const intptr_t index = handle - 1;
    if (index < 0 || index >= VITA_MAX_SEARCHES || !s_searches[index].inUse)
        return -1;
    return VitaFiles_Fill(&s_searches[index], data) ? 0 : -1;
}

int _findclose(intptr_t handle)
{
    const intptr_t index = handle - 1;
    if (index < 0 || index >= VITA_MAX_SEARCHES || !s_searches[index].inUse)
        return -1;

    closedir(s_searches[index].directory);
    memset(&s_searches[index], 0, sizeof(s_searches[index]));
    return 0;
}

int _mkdir(const char *path)
{
    char normalized[VITA_PATH_MAX];
    VitaFiles_Normalize(path, normalized, sizeof(normalized));
    return mkdir(normalized, 0777);
}

int _rmdir(const char *path)
{
    char normalized[VITA_PATH_MAX];
    VitaFiles_Normalize(path, normalized, sizeof(normalized));
    return rmdir(normalized);
}

}
