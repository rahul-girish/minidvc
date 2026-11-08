#ifndef MINIDVC_MANIFEST_H
#define MINIDVC_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#define MINIDVC_MAX_FILES 4096

struct snapshot_summary {
    char name[256];
    int64_t created;
    size_t file_count;
    uint64_t total_size;
};

int manifest_snapshot(const char *name, int count, char **paths);
int manifest_checkout(const char *name, const char *output_dir);

int manifest_valid_name(const char *name);
int manifest_file_path(const char *name, char *out, size_t cap);
int manifest_read_hashes(const char *manifest_filepath,
                         char hashes[][65], int max_hashes);
int manifest_read_summary(const char *manifest_filepath,
                          const char *snapshot_name,
                          struct snapshot_summary *out);
int manifest_import_file(const char *name, const char *tmp_path);

void manifest_print_summaries(struct snapshot_summary *items, size_t count);
int manifest_list_local(void);

#endif
