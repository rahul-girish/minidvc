#ifndef MINIDVC_STORAGE_H
#define MINIDVC_STORAGE_H

int storage_add(const char* path);
int storage_verify(const char* hash);
int storage_restore(const char *hash, const char *output_path);

#endif	// MINIDVC_STORAGE_H
