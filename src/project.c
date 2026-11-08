#define _POSIX_C_SOURCE 200809L

#include "project.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return -1;
}

static void trim_newline(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

static int valid_uuid(const char *s)
{
    if (!s || strlen(s) != 36)
        return 0;

    for (size_t i = 0; i < 36; ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return 0;
        } else {
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
                return 0;
        }
    }
    return 1;
}

static int generate_uuid_v4(char out[37])
{
    unsigned char b[16];
    if (RAND_bytes(b, sizeof b) != 1) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0)
            return -1;
        ssize_t n = read(fd, b, sizeof b);
        close(fd);
        if (n != (ssize_t)sizeof b)
            return -1;
    }

    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);

    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3],
             b[4], b[5],
             b[6], b[7],
             b[8], b[9],
             b[10], b[11], b[12], b[13], b[14], b[15]);
    return 0;
}

static int check_ancestor_initialized(char *found_dir, size_t cap)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd))
        return 0;

    char dir[4096];
    snprintf(dir, sizeof dir, "%s", cwd);

    for (;;) {
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            break;
        *slash = '\0';

        char probe[4200];
        snprintf(probe, sizeof probe, "%s/.mydvc/project", dir);
        struct stat st;
        if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(found_dir, cap, "%s", dir);
            return 1;
        }
    }
    return 0;
}

static void default_project_name(char *out, size_t cap)
{
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) {
        const char *base = strrchr(cwd, '/');
        base = (base && base[1] != '\0') ? base + 1 : "minidvc-project";
        snprintf(out, cap, "%s", base);
    } else {
        snprintf(out, cap, "minidvc-project");
    }

    for (size_t i = 0; out[i] != '\0'; ++i) {
        if (out[i] == '\n' || out[i] == '\r' || out[i] == '\t')
            out[i] = '_';
    }
}

/*
 * Migrate legacy .mydvc/config if present:
 * - Moves OAuth credentials into .mydvc/credentials (0600)
 * - Copies folder_id into project_info if not already set
 */
static void migrate_legacy_config(struct project_info *proj,
                                  struct credentials_info *creds)
{
    FILE *f = fopen(LEGACY_CONFIG_PATH, "r");
    if (!f)
        return;

    char legacy_folder[256] = {0};
    char legacy_cid[512] = {0};
    char legacy_csec[512] = {0};
    char legacy_rtok[1024] = {0};

    char line[2048];
    while (fgets(line, sizeof line, f)) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';

        if (strcmp(line, "folder_id") == 0)
            snprintf(legacy_folder, sizeof legacy_folder, "%s", eq);
        else if (strcmp(line, "client_id") == 0)
            snprintf(legacy_cid, sizeof legacy_cid, "%s", eq);
        else if (strcmp(line, "client_secret") == 0)
            snprintf(legacy_csec, sizeof legacy_csec, "%s", eq);
        else if (strcmp(line, "refresh_token") == 0)
            snprintf(legacy_rtok, sizeof legacy_rtok, "%s", eq);
    }
    fclose(f);

    if (proj && !proj->remote_folder_id[0] && legacy_folder[0])
        snprintf(proj->remote_folder_id, sizeof proj->remote_folder_id,
                 "%s", legacy_folder);

    if (creds) {
        if (!creds->client_id[0] && legacy_cid[0])
            snprintf(creds->client_id, sizeof creds->client_id, "%s", legacy_cid);
        if (!creds->client_secret[0] && legacy_csec[0])
            snprintf(creds->client_secret, sizeof creds->client_secret, "%s", legacy_csec);
        if (!creds->refresh_token[0] && legacy_rtok[0])
            snprintf(creds->refresh_token, sizeof creds->refresh_token, "%s", legacy_rtok);
    }
}

int project_is_initialized(void)
{
    struct stat st;
    return stat(PROJECT_FILE_PATH, &st) == 0 && S_ISREG(st.st_mode);
}

int project_load(struct project_info *info)
{
    memset(info, 0, sizeof(*info));

    FILE *f = fopen(PROJECT_FILE_PATH, "r");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            trim_newline(line);
            if (line[0] == '\0' || line[0] == '#')
                continue;
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq++ = '\0';

            if (strcmp(line, "project_id") == 0)
                snprintf(info->project_id, sizeof info->project_id, "%s", eq);
            else if (strcmp(line, "project_name") == 0)
                snprintf(info->project_name, sizeof info->project_name, "%s", eq);
            else if (strcmp(line, "remote_folder_id") == 0)
                snprintf(info->remote_folder_id, sizeof info->remote_folder_id, "%s", eq);
        }
        fclose(f);
    }

    migrate_legacy_config(info, NULL);

    const char *env_folder = getenv("MINIDVC_GDRIVE_FOLDER_ID");
    if (env_folder && *env_folder)
        snprintf(info->remote_folder_id, sizeof info->remote_folder_id, "%s", env_folder);

    return (info->project_id[0] && valid_uuid(info->project_id)) ? 0 : -1;
}

int project_require(struct project_info *info)
{
    if (!project_is_initialized()) {
        fprintf(stderr,
                "minidvc: not a minidvc project (missing %s); run 'minidvc init' first\n",
                PROJECT_FILE_PATH);
        return -1;
    }

    if (project_load(info) != 0) {
        fprintf(stderr,
                "minidvc: invalid or corrupted project file %s\n",
                PROJECT_FILE_PATH);
        return -1;
    }
    return 0;
}

int project_save(const struct project_info *info)
{
    if (!info || !valid_uuid(info->project_id)) {
        fprintf(stderr, "minidvc: invalid project_id\n");
        return -1;
    }

    if (ensure_dir(".mydvc") != 0) {
        perror(".mydvc");
        return -1;
    }

    char tmp[] = ".mydvc/project.tmp-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }

    if (fchmod(fd, 0644) != 0) {
        perror("fchmod");
        close(fd);
        unlink(tmp);
        return -1;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        perror("fdopen");
        close(fd);
        unlink(tmp);
        return -1;
    }

    int ok = fprintf(f,
                     "project_id=%s\n"
                     "project_name=%s\n"
                     "remote_folder_id=%s\n",
                     info->project_id,
                     info->project_name[0] ? info->project_name : "minidvc-project",
                     info->remote_folder_id) >= 0;

    if (ok && fflush(f) != 0)
        ok = 0;
    if (ok && fsync(fileno(f)) != 0)
        ok = 0;
    if (fclose(f) != 0)
        ok = 0;

    if (!ok) {
        unlink(tmp);
        fprintf(stderr, "minidvc: failed to write %s\n", PROJECT_FILE_PATH);
        return -1;
    }

    if (rename(tmp, PROJECT_FILE_PATH) != 0) {
        perror(PROJECT_FILE_PATH);
        unlink(tmp);
        return -1;
    }

    return 0;
}

int project_init(const char *custom_name)
{
    if (project_is_initialized()) {
        struct project_info existing;
        if (project_load(&existing) == 0) {
            fprintf(stderr,
                    "minidvc: project '%s' is already initialized in this directory (id: %s)\n",
                    existing.project_name, existing.project_id);
        } else {
            fprintf(stderr,
                    "minidvc: project is already initialized in this directory (%s exists)\n",
                    PROJECT_FILE_PATH);
        }
        return -1;
    }

    char ancestor[4096];
    if (check_ancestor_initialized(ancestor, sizeof ancestor)) {
        fprintf(stderr,
                "minidvc: refusing to initialize nested project inside existing project at '%s'\n",
                ancestor);
        return -1;
    }

    if (custom_name && (!*custom_name || strchr(custom_name, '\n') ||
                        strchr(custom_name, '\r'))) {
        fprintf(stderr, "minidvc: invalid project name\n");
        return -1;
    }

    struct project_info info;
    memset(&info, 0, sizeof info);

    if (generate_uuid_v4(info.project_id) != 0) {
        fprintf(stderr, "minidvc: failed to generate project UUID\n");
        return -1;
    }

    if (custom_name && *custom_name)
        snprintf(info.project_name, sizeof info.project_name, "%s", custom_name);
    else
        default_project_name(info.project_name, sizeof info.project_name);

    struct credentials_info creds;
    memset(&creds, 0, sizeof creds);
    migrate_legacy_config(&info, &creds);

    if (ensure_dir(".mydvc") != 0 ||
        ensure_dir(".mydvc/objects") != 0 ||
        ensure_dir(".mydvc/manifests") != 0 ||
        ensure_dir(".mydvc/tmp") != 0) {
        perror(".mydvc");
        return -1;
    }

    if (project_save(&info) != 0)
        return -1;

    if (creds.client_id[0] || creds.client_secret[0] || creds.refresh_token[0]) {
        if (credentials_save(&creds) == 0)
            unlink(LEGACY_CONFIG_PATH);
    }

    printf("Initialized minidvc project '%s' (id: %s)\n",
           info.project_name, info.project_id);
    return 0;
}

int credentials_load(struct credentials_info *creds)
{
    memset(creds, 0, sizeof(*creds));

    FILE *f = fopen(CREDENTIALS_FILE_PATH, "r");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            trim_newline(line);
            if (line[0] == '\0' || line[0] == '#')
                continue;
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq++ = '\0';

            if (strcmp(line, "client_id") == 0)
                snprintf(creds->client_id, sizeof creds->client_id, "%s", eq);
            else if (strcmp(line, "client_secret") == 0)
                snprintf(creds->client_secret, sizeof creds->client_secret, "%s", eq);
            else if (strcmp(line, "refresh_token") == 0)
                snprintf(creds->refresh_token, sizeof creds->refresh_token, "%s", eq);
        }
        fclose(f);
    }

    migrate_legacy_config(NULL, creds);

    const char *env_cid = getenv("MINIDVC_GDRIVE_CLIENT_ID");
    if (env_cid && *env_cid)
        snprintf(creds->client_id, sizeof creds->client_id, "%s", env_cid);

    const char *env_csec = getenv("MINIDVC_GDRIVE_CLIENT_SECRET");
    if (env_csec && *env_csec)
        snprintf(creds->client_secret, sizeof creds->client_secret, "%s", env_csec);

    const char *env_rtok = getenv("MINIDVC_GDRIVE_REFRESH_TOKEN");
    if (env_rtok && *env_rtok)
        snprintf(creds->refresh_token, sizeof creds->refresh_token, "%s", env_rtok);

    return 0;
}

int credentials_save(const struct credentials_info *creds)
{
    if (ensure_dir(".mydvc") != 0) {
        perror(".mydvc");
        return -1;
    }

    char tmp[] = ".mydvc/credentials.tmp-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }

    if (fchmod(fd, 0600) != 0) {
        perror("fchmod");
        close(fd);
        unlink(tmp);
        return -1;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        perror("fdopen");
        close(fd);
        unlink(tmp);
        return -1;
    }

    int ok = fprintf(f,
                     "client_id=%s\n"
                     "client_secret=%s\n"
                     "refresh_token=%s\n",
                     creds->client_id,
                     creds->client_secret,
                     creds->refresh_token) >= 0;

    if (ok && fflush(f) != 0)
        ok = 0;
    if (ok && fsync(fileno(f)) != 0)
        ok = 0;
    if (fclose(f) != 0)
        ok = 0;

    if (!ok) {
        unlink(tmp);
        fprintf(stderr, "minidvc: failed to write %s\n", CREDENTIALS_FILE_PATH);
        return -1;
    }

    if (rename(tmp, CREDENTIALS_FILE_PATH) != 0) {
        perror(CREDENTIALS_FILE_PATH);
        unlink(tmp);
        return -1;
    }

    return 0;
}
