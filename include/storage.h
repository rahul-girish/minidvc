#ifndef MINIDVC_STORAGE_H
#define MINIDVC_STORAGE_H

#include <stddef.h>
#include <stdint.h>

int storage_add(const char* path);
int storage_verify(const char* hash);
int storage_restore(const char *hash, const char *output_path);

int storage_valid_hash(const char *hash);
int storage_object_path(const char *hash, char *out, size_t cap);
int storage_has_object(const char *hash);
int storage_import_object(const char *hash, const char *tmp_path);
int storage_uncompressed_size(const char *hash, uint64_t *out_size);

#endif	// MINIDVC_STORAGE_H
