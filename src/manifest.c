#define _POSIX_C_SOURCE 200809L

#include "manifest.h"
#include "hash.h"
#include "project.h"
#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_PATH_LEN 4096

struct entry {
    char hash[65];
    uint64_t size;
    char name[MAX_PATH_LEN];
};

struct entry_list {
    struct entry *items;
    size_t count;
    size_t cap;
};

static void entry_list_free(struct entry_list *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int entry_list_find(const struct entry_list *list, const char *relpath)
{
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].name, relpath) == 0)
            return 1;
    }
    return 0;
}

static int entry_list_append(struct entry_list *list,
                             const char *relpath,
                             uint64_t size,
                             const char *hash)
{
    if (list->count >= MINIDVC_MAX_FILES) {
        fprintf(stderr, "minidvc: too many files in snapshot (max %d)\n",
                MINIDVC_MAX_FILES);
        return -1;
    }

    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 64;
        if (new_cap > MINIDVC_MAX_FILES)
            new_cap = MINIDVC_MAX_FILES;
        struct entry *n = realloc(list->items, new_cap * sizeof(*list->items));
        if (!n)
            return -1;
        list->items = n;
        list->cap = new_cap;
    }

    struct entry *e = &list->items[list->count];
    if (hash)
        snprintf(e->hash, sizeof e->hash, "%s", hash);
    else
        e->hash[0] = '\0';
    e->size = size;
    snprintf(e->name, sizeof e->name, "%s", relpath);
    ++list->count;
    return 0;
}

static int valid_name(const char *s)
{
    if (!s || !*s)
        return 0;

    for (; *s; ++s) {
        if (!((*s >= 'a' && *s <= 'z') ||
              (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_'))
            return 0;
    }

    return 1;
}

/*
 * Validate and normalize a project-relative path.
 * - Strips leading "./"
 * - Collapses redundant "/" and strips trailing "/"
 * - Rejects absolute paths, "..", internal ".", backslashes, control chars
 * - Rejects internal directories (.mydvc, .git)
 * - Preserves spaces in filenames and directory names
 */
static int normalize_relpath(const char *input, char *out, size_t cap)
{
    if (!input || !*input)
        return -1;

    if (input[0] == '/' || input[0] == '\\')
        return -1;

    for (const char *p = input; *p; ++p) {
        if (*p == '\\' || *p == '\t' || *p == '\n' || *p == '\r')
            return -1;
    }

    const char *p = input;
    while (p[0] == '.' && p[1] == '/') {
        p += 2;
        while (*p == '/')
            ++p;
    }

    if (*p == '\0' || strcmp(p, ".") == 0)
        return -1;

    size_t pos = 0;
    int first_comp = 1;

    while (*p) {
        while (*p == '/')
            ++p;
        if (!*p)
            break;

        const char *start = p;
        while (*p && *p != '/')
            ++p;
        size_t len = (size_t)(p - start);

        if (len == 0)
            break;

        if ((len == 1 && start[0] == '.') ||
            (len == 2 && start[0] == '.' && start[1] == '.')) {
            return -1;
        }

        if (first_comp) {
            if ((len == 6 && memcmp(start, ".mydvc", 6) == 0) ||
                (len == 4 && memcmp(start, ".git", 4) == 0)) {
                return -1;
            }
            first_comp = 0;
        }

        if (pos > 0) {
            if (pos + 1 >= cap)
                return -1;
            out[pos++] = '/';
        }

        if (pos + len >= cap)
            return -1;
        memcpy(out + pos, start, len);
        pos += len;
    }

    if (pos == 0)
        return -1;

    out[pos] = '\0';
    return 0;
}

/*
 * Check that a normalized relative path is strictly strictly normalized
 * (used when validating manifest entries).
 */
static int valid_manifest_relpath(const char *s)
{
    char norm[MAX_PATH_LEN];
    if (!s || !*s)
        return 0;
    if (normalize_relpath(s, norm, sizeof norm) != 0)
        return 0;
    return strcmp(s, norm) == 0;
}

static int verify_no_symlink_components(const char *relpath)
{
    char partial[MAX_PATH_LEN];
    size_t len = strlen(relpath);
    if (len >= sizeof partial)
        return -1;

    for (size_t i = 0; i <= len; ++i) {
        if (relpath[i] == '/' || relpath[i] == '\0') {
            memcpy(partial, relpath, i);
            partial[i] = '\0';
            if (partial[0] == '\0')
                continue;

            struct stat st;
            if (lstat(partial, &st) == 0) {
                if (S_ISLNK(st.st_mode))
                    return -1;
            }
        }
    }
    return 0;
}

static int compare_entries(const void *a, const void *b)
{
    const struct entry *x = a;
    const struct entry *y = b;
    return strcmp(x->name, y->name);
}

static int manifest_path(const char *name, char *out, size_t cap)
{
    if (!valid_name(name))
        return -1;

    int n = snprintf(out, cap,
                     ".mydvc/manifests/%s.manifest", name);

    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static int is_ignored_entry_name(const char *name)
{
    if (!name || !*name)
        return 1;
    if (strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0 ||
        strcmp(name, ".mydvc") == 0 ||
        strcmp(name, ".git") == 0)
        return 1;
    if (strncmp(name, ".minidvc-", 9) == 0 ||
        strncmp(name, ".tmp-", 5) == 0 ||
        strstr(name, ".minidvc-") != NULL)
        return 1;
    return 0;
}

static int collect_path_recursive(const char *relpath,
                                  struct entry_list *list)
{
    if (verify_no_symlink_components(relpath) != 0) {
        fprintf(stderr, "minidvc: symlink paths are not supported: %s\n",
                relpath);
        return -1;
    }

    struct stat st;
    if (lstat(relpath, &st) != 0) {
        fprintf(stderr, "minidvc: file or directory not found: %s\n", relpath);
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        if (entry_list_find(list, relpath))
            return 0;
        return entry_list_append(list, relpath, (uint64_t)st.st_size, NULL);
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(relpath);
        if (!d) {
            perror(relpath);
            return -1;
        }

        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (is_ignored_entry_name(de->d_name))
                continue;

            char child[MAX_PATH_LEN];
            int n = snprintf(child, sizeof child, "%s/%s",
                             relpath, de->d_name);
            if (n < 0 || (size_t)n >= sizeof child) {
                closedir(d);
                fprintf(stderr, "minidvc: path too long: %s/%s\n",
                        relpath, de->d_name);
                return -1;
            }

            struct stat cst;
            if (lstat(child, &cst) != 0)
                continue;

            if (S_ISLNK(cst.st_mode))
                continue;

            if (S_ISREG(cst.st_mode) || S_ISDIR(cst.st_mode)) {
                if (collect_path_recursive(child, list) != 0) {
                    closedir(d);
                    return -1;
                }
            }
        }

        closedir(d);
        return 0;
    }

    fprintf(stderr, "minidvc: not a regular file or directory: %s\n", relpath);
    return -1;
}

int manifest_snapshot(const char *name, int count, char **paths)
{
    char final_path[MAX_PATH_LEN];

    if (!valid_name(name) || count < 1 ||
        manifest_path(name, final_path, sizeof final_path)) {
        fprintf(stderr, "minidvc: invalid snapshot name or arguments\n");
        return -1;
    }

    struct stat mst;
    if (stat(final_path, &mst) == 0) {
        fprintf(stderr, "minidvc: snapshot '%s' already exists\n", name);
        return -1;
    }

    struct entry_list list = {0};

    for (int i = 0; i < count; ++i) {
        char relpath[MAX_PATH_LEN];
        if (normalize_relpath(paths[i], relpath, sizeof relpath) != 0) {
            fprintf(stderr,
                    "minidvc: invalid or outside-project path: %s\n",
                    paths[i]);
            entry_list_free(&list);
            return -1;
        }

        if (collect_path_recursive(relpath, &list) != 0) {
            entry_list_free(&list);
            return -1;
        }
    }

    if (list.count == 0) {
        fprintf(stderr, "minidvc: no regular files found to snapshot\n");
        entry_list_free(&list);
        return -1;
    }

    qsort(list.items, list.count, sizeof(list.items[0]), compare_entries);

    uint64_t total_size = 0;
    for (size_t i = 0; i < list.count; ++i) {
        if (sha256_file(list.items[i].name, list.items[i].hash) != 0) {
            fprintf(stderr, "minidvc: cannot hash: %s\n", list.items[i].name);
            entry_list_free(&list);
            return -1;
        }

        if (storage_add(list.items[i].name) != 0) {
            entry_list_free(&list);
            return -1;
        }

        total_size += list.items[i].size;
    }

    if (mkdir(".mydvc", 0755) != 0 && errno != EEXIST) {
        entry_list_free(&list);
        return -1;
    }

    if (mkdir(".mydvc/manifests", 0755) != 0 && errno != EEXIST) {
        entry_list_free(&list);
        return -1;
    }

    char temp_path[] = ".mydvc/manifests/.tmp-XXXXXX";
    int fd = mkstemp(temp_path);

    if (fd < 0) {
        perror("mkstemp");
        entry_list_free(&list);
        return -1;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(temp_path);
        entry_list_free(&list);
        return -1;
    }

    int64_t created = (int64_t)time(NULL);
    struct project_info proj;
    int has_proj = (project_load(&proj) == 0);

    int ok = fprintf(f,
                     "MINIDVC-MANIFEST 2\n"
                     "created: %" PRId64 "\n"
                     "files: %zu\n"
                     "size: %" PRIu64 "\n",
                     created, list.count, total_size) >= 0;

    if (ok && has_proj && proj.project_id[0]) {
        if (fprintf(f, "project_id: %s\n", proj.project_id) < 0)
            ok = 0;
    }

    if (ok && fprintf(f, "\n") < 0)
        ok = 0;

    for (size_t i = 0; ok && i < list.count; ++i) {
        if (fprintf(f, "%s\t%" PRIu64 "\t%s\n",
                    list.items[i].hash,
                    list.items[i].size,
                    list.items[i].name) < 0)
            ok = 0;
    }

    if (ok && fflush(f) != 0)
        ok = 0;

    if (ok && fsync(fileno(f)) != 0)
        ok = 0;

    if (fclose(f) != 0)
        ok = 0;

    if (!ok) {
        unlink(temp_path);
        entry_list_free(&list);
        fprintf(stderr, "minidvc: failed to write manifest\n");
        return -1;
    }

    /*
     * Publish without replacing a previous snapshot with the same name.
     */
    if (link(temp_path, final_path) != 0) {
        perror(final_path);
        unlink(temp_path);
        entry_list_free(&list);
        return -1;
    }

    unlink(temp_path);
    printf("Created snapshot '%s' (%zu file%s, %" PRIu64 " bytes)\n",
           name, list.count, list.count == 1 ? "" : "s", total_size);
    entry_list_free(&list);
    return 0;
}

static int parse_uint64_str(const char *s, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_int64_str(const char *s, int64_t *out)
{
    if (!s || !*s)
        return -1;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -1;
    *out = (int64_t)v;
    return 0;
}

/*
 * Parse both MINIDVC-MANIFEST 2 and legacy MINIDVC-MANIFEST 1 files.
 */
static int parse_manifest_full(const char *path,
                               struct entry_list *entries,
                               struct snapshot_summary *summary_out,
                               int compute_v1_sizes)
{
    memset(entries, 0, sizeof(*entries));
    if (summary_out)
        memset(summary_out, 0, sizeof(*summary_out));

    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, f);
    if (len < 0) {
        free(line);
        fclose(f);
        fprintf(stderr, "minidvc: invalid manifest: %s\n", path);
        return -1;
    }

    int version = 0;
    if (strcmp(line, "MINIDVC-MANIFEST 2\n") == 0)
        version = 2;
    else if (strcmp(line, "MINIDVC-MANIFEST 1\n") == 0)
        version = 1;

    if (version == 0) {
        free(line);
        fclose(f);
        fprintf(stderr, "minidvc: unsupported manifest header: %s\n", path);
        return -1;
    }

    int ok = 1;
    int64_t hdr_created = 0;
    uint64_t hdr_files = 0;
    uint64_t hdr_size = 0;
    int has_hdr_files = 0;
    int has_hdr_size = 0;

    if (version == 2) {
        while (ok && (len = getline(&line, &cap, f)) >= 0) {
            if (len == 0 || line[len - 1] != '\n') {
                ok = 0;
                break;
            }
            line[len - 1] = '\0';
            if (line[0] == '\0')
                break; /* End of header block */

            if (strncmp(line, "created: ", 9) == 0) {
                if (parse_int64_str(line + 9, &hdr_created) != 0)
                    ok = 0;
            } else if (strncmp(line, "files: ", 7) == 0) {
                if (parse_uint64_str(line + 7, &hdr_files) != 0)
                    ok = 0;
                has_hdr_files = 1;
            } else if (strncmp(line, "size: ", 6) == 0) {
                if (parse_uint64_str(line + 6, &hdr_size) != 0)
                    ok = 0;
                has_hdr_size = 1;
            } else if (strncmp(line, "project_id: ", 12) == 0) {
                /* Optional project_id metadata */
            } else {
                ok = 0;
            }
        }
    } else {
        struct stat st;
        if (stat(path, &st) == 0)
            hdr_created = (int64_t)st.st_mtime;
    }

    uint64_t calc_size = 0;

    while (ok && (len = getline(&line, &cap, f)) >= 0) {
        if (len == 0 || line[len - 1] != '\n') {
            ok = 0;
            break;
        }
        line[len - 1] = '\0';

        if (version == 2) {
            char *tab1 = strchr(line, '\t');
            if (!tab1) {
                ok = 0;
                break;
            }
            *tab1++ = '\0';

            char *tab2 = strchr(tab1, '\t');
            if (!tab2) {
                ok = 0;
                break;
            }
            *tab2++ = '\0';

            uint64_t fsize = 0;
            if (!storage_valid_hash(line) ||
                parse_uint64_str(tab1, &fsize) != 0 ||
                !valid_manifest_relpath(tab2) ||
                entry_list_find(entries, tab2)) {
                ok = 0;
                break;
            }

            if (entry_list_append(entries, tab2, fsize, line) != 0) {
                ok = 0;
                break;
            }
            calc_size += fsize;
        } else {
            char *tab = strchr(line, '\t');
            if (!tab) {
                ok = 0;
                break;
            }
            *tab++ = '\0';

            if (!storage_valid_hash(line) ||
                !valid_manifest_relpath(tab) ||
                entry_list_find(entries, tab)) {
                ok = 0;
                break;
            }

            uint64_t fsize = 0;
            if (compute_v1_sizes && storage_has_object(line)) {
                if (storage_uncompressed_size(line, &fsize) != 0)
                    fsize = 0;
            }

            if (entry_list_append(entries, tab, fsize, line) != 0) {
                ok = 0;
                break;
            }
            calc_size += fsize;
        }
    }

    if (ferror(f) || entries->count == 0)
        ok = 0;

    if (ok && version == 2) {
        if ((has_hdr_files && hdr_files != entries->count) ||
            (has_hdr_size && hdr_size != calc_size))
            ok = 0;
    }

    free(line);
    fclose(f);

    if (!ok) {
        entry_list_free(entries);
        fprintf(stderr, "minidvc: invalid manifest: %s\n", path);
        return -1;
    }

    if (summary_out) {
        summary_out->created = hdr_created;
        summary_out->file_count = entries->count;
        summary_out->total_size = calc_size;
    }

    return (int)entries->count;
}

int manifest_valid_name(const char *name)
{
    return name && valid_name(name);
}

int manifest_file_path(const char *name, char *out, size_t cap)
{
    return manifest_path(name, out, cap);
}

int manifest_read_hashes(const char *manifest_filepath,
                         char hashes[][65], int max_hashes)
{
    struct entry_list entries = {0};
    int count = parse_manifest_full(manifest_filepath, &entries, NULL, 0);
    if (count < 0)
        return -1;

    if (count > max_hashes) {
        entry_list_free(&entries);
        return -1;
    }

    for (int i = 0; i < count; ++i)
        strcpy(hashes[i], entries.items[i].hash);

    entry_list_free(&entries);
    return count;
}

int manifest_read_summary(const char *manifest_filepath,
                          const char *snapshot_name,
                          struct snapshot_summary *out)
{
    struct entry_list entries = {0};
    if (parse_manifest_full(manifest_filepath, &entries, out, 1) < 0)
        return -1;
    entry_list_free(&entries);

    if (snapshot_name)
        snprintf(out->name, sizeof out->name, "%s", snapshot_name);
    return 0;
}

int manifest_import_file(const char *name, const char *tmp_path)
{
    char final_path[MAX_PATH_LEN];
    if (!name || !tmp_path ||
        manifest_path(name, final_path, sizeof final_path) != 0) {
        fprintf(stderr, "minidvc: invalid manifest name\n");
        return -1;
    }

    struct entry_list entries = {0};
    if (parse_manifest_full(tmp_path, &entries, NULL, 0) < 0)
        return -1;
    entry_list_free(&entries);

    if (mkdir(".mydvc", 0755) != 0 && errno != EEXIST)
        return -1;

    if (mkdir(".mydvc/manifests", 0755) != 0 && errno != EEXIST)
        return -1;

    if (link(tmp_path, final_path) == 0)
        return 0;

    if (errno == EEXIST) {
        if (parse_manifest_full(final_path, &entries, NULL, 0) < 0)
            return -1;
        entry_list_free(&entries);
        return 0;
    }

    perror(final_path);
    return -1;
}

static int ensure_parent_dirs(const char *filepath)
{
    char buf[MAX_PATH_LEN];
    size_t len = strlen(filepath);
    if (len >= sizeof buf)
        return -1;

    memcpy(buf, filepath, len + 1);

    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            struct stat st;
            if (lstat(buf, &st) == 0) {
                if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
                    fprintf(stderr,
                            "minidvc: invalid parent directory component: %s\n",
                            buf);
                    return -1;
                }
            } else {
                if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                    perror(buf);
                    return -1;
                }
            }
            buf[i] = '/';
        }
    }
    return 0;
}

int manifest_checkout(const char *name, const char *output_dir)
{
    char path[MAX_PATH_LEN];

    if (manifest_path(name, path, sizeof path) != 0) {
        fprintf(stderr, "minidvc: invalid snapshot name\n");
        return -1;
    }

    const char *base_dir = (output_dir && *output_dir) ? output_dir : ".";
    int use_root = (strcmp(base_dir, ".") == 0);

    if (!use_root) {
        if (ensure_parent_dirs(base_dir) != 0)
            return -1;
        struct stat st;
        if (mkdir(base_dir, 0755) != 0) {
            if (errno != EEXIST ||
                lstat(base_dir, &st) != 0 ||
                !S_ISDIR(st.st_mode)) {
                perror(base_dir);
                return -1;
            }
        }
    }

    struct entry_list entries = {0};
    int count = parse_manifest_full(path, &entries, NULL, 0);
    if (count < 0)
        return -1;

    /*
     * Pre-flight validation:
     * 1. Ensure no destination file already exists (no silent overwrite).
     * 2. Ensure all referenced objects exist locally and pass verification.
     */
    for (int i = 0; i < count; ++i) {
        char dest[MAX_PATH_LEN];
        int n;
        if (use_root)
            n = snprintf(dest, sizeof dest, "%s", entries.items[i].name);
        else
            n = snprintf(dest, sizeof dest, "%s/%s",
                         base_dir, entries.items[i].name);

        if (n < 0 || (size_t)n >= sizeof dest) {
            fprintf(stderr, "minidvc: output path too long\n");
            entry_list_free(&entries);
            return -1;
        }

        struct stat dst;
        if (lstat(dest, &dst) == 0) {
            fprintf(stderr,
                    "minidvc: refusing to overwrite existing file: %s\n",
                    dest);
            entry_list_free(&entries);
            return -1;
        }

        if (!storage_has_object(entries.items[i].hash)) {
            fprintf(stderr,
                    "minidvc: missing local object %s for %s (run 'minidvc pull %s' first)\n",
                    entries.items[i].hash, entries.items[i].name, name);
            entry_list_free(&entries);
            return -1;
        }
    }

    for (int i = 0; i < count; ++i) {
        char dest[MAX_PATH_LEN];
        if (use_root)
            snprintf(dest, sizeof dest, "%s", entries.items[i].name);
        else
            snprintf(dest, sizeof dest, "%s/%s",
                     base_dir, entries.items[i].name);

        if (ensure_parent_dirs(dest) != 0) {
            entry_list_free(&entries);
            return -1;
        }

        if (storage_restore(entries.items[i].hash, dest) != 0) {
            fprintf(stderr,
                    "minidvc: checkout incomplete; failed on %s\n",
                    entries.items[i].name);
            entry_list_free(&entries);
            return -1;
        }
    }

    entry_list_free(&entries);
    if (use_root)
        printf("Checked out snapshot '%s' (%d file%s)\n",
               name, count, count == 1 ? "" : "s");
    else
        printf("Checked out snapshot '%s' to %s (%d file%s)\n",
               name, base_dir, count, count == 1 ? "" : "s");
    return 0;
}

static int compare_summaries_desc(const void *a, const void *b)
{
    const struct snapshot_summary *x = a;
    const struct snapshot_summary *y = b;
    if (x->created != y->created)
        return (y->created > x->created) ? 1 : -1;
    return strcmp(x->name, y->name);
}

static void format_size(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(out, cap, "%.1f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (uint64_t)1024 * 1024) {
        snprintf(out, cap, "%.1f MB",
                 (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(out, cap, "%.1f KB",
                 (double)bytes / 1024.0);
    } else {
        snprintf(out, cap, "%" PRIu64 " B", bytes);
    }
}

static void format_timestamp(int64_t ts, char *out, size_t cap)
{
    if (ts <= 0) {
        snprintf(out, cap, "unknown");
        return;
    }

    time_t t = (time_t)ts;
    struct tm tm_val;
    if (localtime_r(&t, &tm_val) == NULL) {
        snprintf(out, cap, "unknown");
        return;
    }

    strftime(out, cap, "%Y-%m-%d %H:%M", &tm_val);
}

void manifest_print_summaries(struct snapshot_summary *items, size_t count)
{
    if (count > 1)
        qsort(items, count, sizeof(items[0]), compare_summaries_desc);

    printf("%-16s  %-18s  %-9s  %s\n",
           "NAME", "CREATED", "FILES", "SIZE");
    printf("------------------------------------------------------------\n");

    if (count == 0) {
        printf("(no snapshots found)\n");
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        char time_str[32];
        char size_str[32];
        format_timestamp(items[i].created, time_str, sizeof time_str);
        format_size(items[i].total_size, size_str, sizeof size_str);
        printf("%-16s  %-18s  %-9zu  %s\n",
               items[i].name, time_str, items[i].file_count, size_str);
    }
}

int manifest_list_local(void)
{
    DIR *d = opendir(".mydvc/manifests");
    if (!d) {
        if (errno == ENOENT) {
            manifest_print_summaries(NULL, 0);
            return 0;
        }
        perror(".mydvc/manifests");
        return -1;
    }

    struct snapshot_summary *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len <= 9 || strcmp(de->d_name + len - 9, ".manifest") != 0)
            continue;

        char name[256];
        if (len - 9 >= sizeof name)
            continue;
        memcpy(name, de->d_name, len - 9);
        name[len - 9] = '\0';

        if (!valid_name(name))
            continue;

        char mpath[MAX_PATH_LEN];
        if (manifest_path(name, mpath, sizeof mpath) != 0)
            continue;

        struct snapshot_summary sum;
        if (manifest_read_summary(mpath, name, &sum) != 0)
            continue;

        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 32;
            struct snapshot_summary *n = realloc(items, new_cap * sizeof(*items));
            if (!n) {
                free(items);
                closedir(d);
                return -1;
            }
            items = n;
            cap = new_cap;
        }
        items[count++] = sum;
    }
    closedir(d);

    manifest_print_summaries(items, count);
    free(items);
    return 0;
}
