#ifndef MINIDVC_REMOTE_H
#define MINIDVC_REMOTE_H

int remote_set_folder(const char *folder_id_or_url);
int remote_show(void);
int remote_auth(const char *client_id, const char *client_secret,
                const char *refresh_token);
int remote_push(const char *snapshot_name);
int remote_pull(const char *snapshot_name);
int remote_list(void);

#endif // MINIDVC_REMOTE_H
