#ifndef MINIDVC_PROJECT_H
#define MINIDVC_PROJECT_H

#include <stddef.h>

#define PROJECT_FILE_PATH ".mydvc/project"
#define CREDENTIALS_FILE_PATH ".mydvc/credentials"
#define LEGACY_CONFIG_PATH ".mydvc/config"

struct project_info {
    char project_id[64];
    char project_name[256];
    char remote_folder_id[256];
};

struct credentials_info {
    char client_id[512];
    char client_secret[512];
    char refresh_token[1024];
};

int project_init(const char *custom_name);
int project_is_initialized(void);
int project_load(struct project_info *info);
int project_require(struct project_info *info);
int project_save(const struct project_info *info);

int credentials_load(struct credentials_info *creds);
int credentials_save(const struct credentials_info *creds);

#endif // MINIDVC_PROJECT_H
