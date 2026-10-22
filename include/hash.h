#ifndef MINIDVC_HASH_H
#define MINIDVC_HASH_H

#include <stddef.h>

int sha256_file(const char* path, char output[65]);

#endif	// MINIDVC_HASH_H
