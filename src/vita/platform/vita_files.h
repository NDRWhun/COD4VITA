// The MSVC directory-walk and path calls the engine uses, over newlib.
#pragma once

#include <stdint.h>

#define _A_SUBDIR 0x10

struct _finddata64i32_t
{
    unsigned int attrib;
    long time_create;
    long time_access;
    long time_write;
    long size;
    char name[260];
};

extern "C" {

intptr_t _findfirst64i32(const char *specification, _finddata64i32_t *data);
int _findnext64i32(intptr_t handle, _finddata64i32_t *data);
int _findclose(intptr_t handle);

int _mkdir(const char *path);
int _rmdir(const char *path);

}
