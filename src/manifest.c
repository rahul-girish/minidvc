#define _POSIX_C_SOURCE 200809L

#include "manifest.h"
#include "hash.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILES 256
#define MAX_PATH_LEN 4096

struct entry {
    char hash[65];
    char name[MAX_PATH_LEN];
};

static int valid_name(const char *s)
{
    if (!*s)
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

static int valid_filename(const char *s)
{
    if (!*s || strcmp(s, ".") == 0 ||
        strcmp(s, "..") == 0)
        return 0;

    for (; *s; ++s) {
        if (*s == '/' || *s == '\t' ||
            *s == '\n' || *s == '\r')
            return 0;
    }

    return 1;
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

int manifest_snapshot(const char *name, int count, char **paths)
{
    char final_path[MAX_PATH_LEN];

    if (!valid_name(name) || count < 1 || count > MAX_FILES ||
        manifest_path(name, final_path, sizeof final_path)) {
        fprintf(stderr, "minidvc: invalid snapshot name or file count\n");
        return -1;
    }

    struct entry entries[MAX_FILES];

    for (int i = 0; i < count; ++i) {
        const char *input = paths[i];
        const char *base = strrchr(input, '/');
        base = base ? base + 1 : input;

        if (!valid_filename(base) ||
            strlen(base) >= sizeof entries[i].name) {
            fprintf(stderr, "minidvc: unsupported filename: %s\n", input);
            return -1;
        }

        for (int j = 0; j < i; ++j) {
            if (strcmp(entries[j].name, base) == 0) {
                fprintf(stderr,
                        "minidvc: duplicate filename: %s\n", base);
                return -1;
            }
        }

        struct stat st;
        if (stat(input, &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "minidvc: not a regular file: %s\n", input);
            return -1;
        }

        if (sha256_file(input, entries[i].hash) != 0) {
            fprintf(stderr, "minidvc: cannot hash: %s\n", input);
            return -1;
        }

        if (storage_add(input) != 0)
            return -1;

        strcpy(entries[i].name, base);
    }

    qsort(entries, count, sizeof entries[0], compare_entries);

    if (mkdir(".mydvc", 0755) != 0 && errno != EEXIST)
        return -1;

    if (mkdir(".mydvc/manifests", 0755) != 0 &&
        errno != EEXIST)
        return -1;

    char temp_path[] = ".mydvc/manifests/.tmp-XXXXXX";
    int fd = mkstemp(temp_path);

    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(temp_path);
        return -1;
    }

    int ok = fprintf(f, "MINIDVC-MANIFEST 1\n") >= 0;

    for (int i = 0; ok && i < count; ++i) {
        if (fprintf(f, "%s\t%s\n",
                    entries[i].hash, entries[i].name) < 0)
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
        fprintf(stderr, "minidvc: failed to write manifest\n");
        return -1;
    }

    /*
     * Publish without replacing a previous snapshot with the same name.
     */
    if (link(temp_path, final_path) != 0) {
        perror(final_path);
        unlink(temp_path);
        return -1;
    }

    unlink(temp_path);
    printf("Created snapshot '%s' (%d files)\n", name, count);
    return 0;
}

int manifest_checkout(const char *name, const char *output_dir)
{
    char path[MAX_PATH_LEN];

    if (manifest_path(name, path, sizeof path) != 0 ||
        !output_dir || !*output_dir) {
        fprintf(stderr, "minidvc: invalid snapshot or output directory\n");
        return -1;
    }

    struct stat st;
    if (mkdir(output_dir, 0755) != 0) {
        if (errno != EEXIST ||
            stat(output_dir, &st) != 0 ||
            !S_ISDIR(st.st_mode)) {
            perror(output_dir);
            return -1;
        }
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, f);
    int ok = len >= 0 &&
             strcmp(line, "MINIDVC-MANIFEST 1\n") == 0;

    struct entry entries[MAX_FILES];
    int count = 0;

    while (ok && (len = getline(&line, &cap, f)) >= 0) {
        if (len == 0 || line[len - 1] != '\n' ||
            count == MAX_FILES) {
            ok = 0;
            break;
        }

        line[len - 1] = '\0';
        char *tab = strchr(line, '\t');

        if (!tab) {
            ok = 0;
            break;
        }

        *tab++ = '\0';

        if (strlen(line) != 64 || !valid_filename(tab) ||
            strlen(tab) >= sizeof entries[count].name) {
            ok = 0;
            break;
        }

        for (int i = 0; i < count; ++i) {
            if (strcmp(entries[i].name, tab) == 0)
                ok = 0;
        }

        if (!ok)
            break;

        strcpy(entries[count].hash, line);
        strcpy(entries[count].name, tab);
        ++count;
    }

    if (ferror(f) || count == 0)
        ok = 0;

    free(line);
    fclose(f);

    if (!ok) {
        fprintf(stderr, "minidvc: invalid manifest: %s\n", path);
        return -1;
    }

    for (int i = 0; i < count; ++i) {
        char dest[MAX_PATH_LEN];

        int n = snprintf(dest, sizeof dest, "%s/%s",
                         output_dir, entries[i].name);

        if (n < 0 || (size_t)n >= sizeof dest) {
            fprintf(stderr, "minidvc: output path too long\n");
            return -1;
        }

        if (storage_restore(entries[i].hash, dest) != 0) {
            fprintf(stderr,
                    "minidvc: checkout incomplete; failed on %s\n",
                    entries[i].name);
            return -1;
        }
    }

    printf("Checked out snapshot '%s' to %s\n", name, output_dir);
    return 0;
}