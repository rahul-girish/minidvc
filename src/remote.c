#define _POSIX_C_SOURCE 200809L

#include "remote.h"
#include "manifest.h"
#include "project.h"
#include "storage.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_API_BASE "https://www.googleapis.com"
#define DEFAULT_TOKEN_URL "https://oauth2.googleapis.com/token"
#define FOLDER_MIME "application/vnd.google-apps.folder"

struct remote_file {
    char id[256];
    char name[512];
};

struct remote_file_list {
    struct remote_file *items;
    size_t count;
    size_t cap;
};

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return -1;
}

static int ensure_tmp_dir(void)
{
    if (ensure_dir(".mydvc") != 0 || ensure_dir(".mydvc/tmp") != 0) {
        fprintf(stderr, "minidvc: cannot create .mydvc/tmp directory\n");
        return -1;
    }
    return 0;
}

static const char *get_api_base(void)
{
    const char *env = getenv("MINIDVC_GDRIVE_API_BASE");
    return (env && *env) ? env : DEFAULT_API_BASE;
}

static const char *get_token_url(void)
{
    const char *env = getenv("MINIDVC_GDRIVE_OAUTH_TOKEN_URL");
    return (env && *env) ? env : DEFAULT_TOKEN_URL;
}

static int valid_drive_id(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (len < 3 || len >= 256)
        return 0;

    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int extract_folder_id(const char *input, char *out, size_t cap)
{
    if (!input || !*input)
        return -1;

    const char *p = strstr(input, "/folders/");
    if (p) {
        p += strlen("/folders/");
        size_t len = 0;
        while (p[len] && p[len] != '/' && p[len] != '?' &&
               p[len] != '#' && p[len] != '&') {
            ++len;
        }
        if (len == 0 || len >= cap)
            return -1;
        memcpy(out, p, len);
        out[len] = '\0';
    } else {
        if (strlen(input) >= cap)
            return -1;
        strcpy(out, input);
    }

    return valid_drive_id(out) ? 0 : -1;
}

static void trim_newline(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

static int url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (size_t i = 0; src[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            if (pos + 1 >= cap)
                return -1;
            dst[pos++] = (char)c;
        } else {
            if (pos + 3 >= cap)
                return -1;
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 15];
        }
    }

    if (pos >= cap)
        return -1;
    dst[pos] = '\0';
    return 0;
}

static int url_decode(const char *src, size_t len, char *dst, size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < len; ++i) {
        if (pos + 1 >= cap)
            return -1;
        if (src[i] == '%' && i + 2 < len &&
            isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[pos++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[pos++] = ' ';
        } else {
            dst[pos++] = src[i];
        }
    }
    dst[pos] = '\0';
    return 0;
}

/*
 * Minimal, bounds-checked JSON helpers for Google OAuth2 & Drive API v3.
 */
static const char *json_find_key_in_range(const char *start, const char *end,
                                          const char *key)
{
    size_t key_len = strlen(key);
    const char *p = start;

    while (p < end) {
        p = memchr(p, '"', (size_t)(end - p));
        if (!p || p + key_len + 2 >= end)
            return NULL;

        if (memcmp(p + 1, key, key_len) == 0 && p[key_len + 1] == '"') {
            const char *after = p + key_len + 2;
            while (after < end && (*after == ' ' || *after == '\t' ||
                                   *after == '\n' || *after == '\r'))
                ++after;
            if (after < end && *after == ':') {
                ++after;
                while (after < end && (*after == ' ' || *after == '\t' ||
                                       *after == '\n' || *after == '\r'))
                    ++after;
                return after;
            }
        }
        ++p;
    }
    return NULL;
}

static int json_extract_string_range(const char *start, const char *end,
                                     const char *key, char *out, size_t cap)
{
    const char *val = json_find_key_in_range(start, end, key);
    if (!val || val >= end || *val != '"')
        return -1;

    ++val;
    size_t pos = 0;
    while (val < end && *val != '"') {
        char c = *val++;
        if (c == '\\') {
            if (val >= end)
                return -1;
            char esc = *val++;
            switch (esc) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: c = esc; break;
            }
        }
        if (pos + 1 >= cap)
            return -1;
        out[pos++] = c;
    }

    if (val >= end || *val != '"')
        return -1;

    out[pos] = '\0';
    return 0;
}

static int json_get_string(const char *json, const char *key,
                           char *out, size_t cap)
{
    if (!json)
        return -1;
    return json_extract_string_range(json, json + strlen(json), key, out, cap);
}

static int json_get_bool(const char *json, const char *key, int *out)
{
    if (!json)
        return -1;
    const char *end = json + strlen(json);
    const char *val = json_find_key_in_range(json, end, key);
    if (!val)
        return -1;

    if ((size_t)(end - val) >= 4 && strncmp(val, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if ((size_t)(end - val) >= 5 && strncmp(val, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static char *read_file_string(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/*
 * Execute an HTTP request via curl without invoking a shell.
 * Headers and sensitive tokens are passed via a 0600 temporary config file.
 */
static int http_request(const char *method,
                        const char *url,
                        const char *bearer_token,
                        const char *content_type,
                        const char *post_data,
                        const char *upload_file_path,
                        const char *out_body_path,
                        const char *out_header_path,
                        long *http_status)
{
    if (ensure_tmp_dir() != 0)
        return -1;

    char cfg_path[] = ".mydvc/tmp/curl-cfg-XXXXXX";
    int cfg_fd = mkstemp(cfg_path);
    if (cfg_fd < 0) {
        perror("mkstemp");
        return -1;
    }

    if (fchmod(cfg_fd, 0600) != 0) {
        close(cfg_fd);
        unlink(cfg_path);
        return -1;
    }

    FILE *cfg = fdopen(cfg_fd, "w");
    if (!cfg) {
        close(cfg_fd);
        unlink(cfg_path);
        return -1;
    }

    int ok = 1;
    if (bearer_token && *bearer_token) {
        if (fprintf(cfg, "header = \"Authorization: Bearer %s\"\n",
                    bearer_token) < 0)
            ok = 0;
    }
    if (ok && content_type && *content_type) {
        if (fprintf(cfg, "header = \"Content-Type: %s\"\n",
                    content_type) < 0)
            ok = 0;
    }
    if (fclose(cfg) != 0)
        ok = 0;

    if (!ok) {
        unlink(cfg_path);
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        unlink(cfg_path);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(cfg_path);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);

        const char *argv[32];
        int argc = 0;

        argv[argc++] = "curl";
        argv[argc++] = "-sS";
        argv[argc++] = "-L";
        argv[argc++] = "--connect-timeout";
        argv[argc++] = "30";
        argv[argc++] = "-X";
        argv[argc++] = method;
        argv[argc++] = "--config";
        argv[argc++] = cfg_path;
        argv[argc++] = "-o";
        argv[argc++] = out_body_path;
        argv[argc++] = "-w";
        argv[argc++] = "%{http_code}";

        if (out_header_path) {
            argv[argc++] = "-D";
            argv[argc++] = out_header_path;
        }

        if (post_data) {
            argv[argc++] = "--data-raw";
            argv[argc++] = post_data;
        }

        char upload_arg[4096];
        if (upload_file_path) {
            snprintf(upload_arg, sizeof upload_arg, "@%s", upload_file_path);
            argv[argc++] = "--data-binary";
            argv[argc++] = upload_arg;
        }

        argv[argc++] = url;
        argv[argc] = NULL;

        execvp("curl", (char *const *)argv);
        perror("curl");
        _exit(127);
    }

    close(pipefd[1]);

    char status_buf[32];
    ssize_t r = read(pipefd[0], status_buf, sizeof(status_buf) - 1);
    close(pipefd[0]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    unlink(cfg_path);

    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0 || r <= 0) {
        fprintf(stderr, "minidvc: HTTP request failed (%s %s)\n", method, url);
        return -1;
    }

    status_buf[r] = '\0';
    *http_status = strtol(status_buf, NULL, 10);
    return 0;
}

static int http_json_request(const char *method,
                             const char *url,
                             const char *bearer_token,
                             const char *content_type,
                             const char *post_data,
                             char **out_json,
                             long *http_status)
{
    if (ensure_tmp_dir() != 0)
        return -1;

    char body_path[] = ".mydvc/tmp/http-body-XXXXXX";
    int fd = mkstemp(body_path);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    close(fd);

    int rc = http_request(method, url, bearer_token, content_type,
                          post_data, NULL, body_path, NULL, http_status);
    if (rc != 0) {
        unlink(body_path);
        return -1;
    }

    *out_json = read_file_string(body_path);
    unlink(body_path);

    return *out_json ? 0 : -1;
}

static void print_google_error(const char *context, long status,
                               const char *json)
{
    char msg[512];
    if (json && json_get_string(json, "message", msg, sizeof msg) == 0) {
        fprintf(stderr, "minidvc: %s (HTTP %ld: %s)\n", context, status, msg);
    } else if (json &&
               json_get_string(json, "error_description", msg, sizeof msg) == 0) {
        fprintf(stderr, "minidvc: %s (HTTP %ld: %s)\n", context, status, msg);
    } else {
        fprintf(stderr, "minidvc: %s (HTTP %ld)\n", context, status);
    }
}

static int oauth_get_access_token(const struct credentials_info *creds,
                                  char *access_token, size_t cap)
{
    if (!creds->client_id[0] || !creds->client_secret[0] ||
        !creds->refresh_token[0]) {
        fprintf(stderr,
                "minidvc: missing Google Drive OAuth credentials; run 'minidvc auth' first\n");
        return -1;
    }

    char cid_enc[1024], csec_enc[1024], rtok_enc[2048];
    if (url_encode(creds->client_id, cid_enc, sizeof cid_enc) != 0 ||
        url_encode(creds->client_secret, csec_enc, sizeof csec_enc) != 0 ||
        url_encode(creds->refresh_token, rtok_enc, sizeof rtok_enc) != 0) {
        fprintf(stderr, "minidvc: credential string too long\n");
        return -1;
    }

    char post_body[4608];
    int n = snprintf(post_body, sizeof post_body,
                     "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                     cid_enc, csec_enc, rtok_enc);
    if (n < 0 || (size_t)n >= sizeof post_body)
        return -1;

    char *json = NULL;
    long status = 0;
    if (http_json_request("POST", get_token_url(), NULL,
                          "application/x-www-form-urlencoded",
                          post_body, &json, &status) != 0) {
        return -1;
    }

    if (status != 200 ||
        json_get_string(json, "access_token", access_token, cap) != 0) {
        print_google_error("OAuth token refresh failed", status, json);
        free(json);
        return -1;
    }

    free(json);
    return 0;
}

static int oauth_exchange_code(const char *client_id,
                               const char *client_secret,
                               const char *auth_code,
                               const char *redirect_uri,
                               char *refresh_token_out,
                               size_t cap)
{
    char cid_enc[1024], csec_enc[1024], code_enc[2048], redir_enc[512];
    if (url_encode(client_id, cid_enc, sizeof cid_enc) != 0 ||
        url_encode(client_secret, csec_enc, sizeof csec_enc) != 0 ||
        url_encode(auth_code, code_enc, sizeof code_enc) != 0 ||
        url_encode(redirect_uri, redir_enc, sizeof redir_enc) != 0) {
        return -1;
    }

    char post_body[4608];
    int n = snprintf(post_body, sizeof post_body,
                     "client_id=%s&client_secret=%s&code=%s&redirect_uri=%s&grant_type=authorization_code",
                     cid_enc, csec_enc, code_enc, redir_enc);
    if (n < 0 || (size_t)n >= sizeof post_body)
        return -1;

    char *json = NULL;
    long status = 0;
    if (http_json_request("POST", get_token_url(), NULL,
                          "application/x-www-form-urlencoded",
                          post_body, &json, &status) != 0) {
        return -1;
    }

    if (status != 200 ||
        json_get_string(json, "refresh_token", refresh_token_out, cap) != 0) {
        print_google_error("failed to exchange authorization code for refresh token",
                           status, json);
        free(json);
        return -1;
    }

    free(json);
    return 0;
}

static int parse_code_from_input(const char *input, char *code_out, size_t cap)
{
    const char *code_param = strstr(input, "code=");
    if (code_param) {
        code_param += 5;
        size_t len = 0;
        while (code_param[len] && code_param[len] != '&' &&
               code_param[len] != ' ' && code_param[len] != '\r' &&
               code_param[len] != '\n') {
            ++len;
        }
        if (len == 0)
            return -1;
        return url_decode(code_param, len, code_out, cap);
    }

    while (*input == ' ' || *input == '\t')
        ++input;
    size_t len = strlen(input);
    while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r' ||
                       input[len - 1] == ' ' || input[len - 1] == '\t')) {
        --len;
    }
    if (len == 0)
        return -1;
    return url_decode(input, len, code_out, cap);
}

static int interactive_oauth_flow(const char *client_id,
                                  const char *client_secret,
                                  char *refresh_token_out,
                                  size_t cap)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(listen_fd, 1) != 0) {
        perror("bind/listen");
        close(listen_fd);
        return -1;
    }

    socklen_t addr_len = sizeof addr;
    if (getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        perror("getsockname");
        close(listen_fd);
        return -1;
    }

    unsigned int port = ntohs(addr.sin_port);
    char redirect_uri[128];
    snprintf(redirect_uri, sizeof redirect_uri, "http://127.0.0.1:%u", port);

    char cid_enc[1024], redir_enc[256], scope_enc[256];
    url_encode(client_id, cid_enc, sizeof cid_enc);
    url_encode(redirect_uri, redir_enc, sizeof redir_enc);
    url_encode("https://www.googleapis.com/auth/drive", scope_enc, sizeof scope_enc);

    printf("Open the following URL in your browser to authorize minidvc for Google Drive:\n\n"
           "  https://accounts.google.com/o/oauth2/v2/auth?client_id=%s&redirect_uri=%s&response_type=code&scope=%s&access_type=offline&prompt=consent\n\n"
           "Waiting for browser callback on %s (or paste the redirected URL / authorization code here): ",
           cid_enc, redir_enc, scope_enc, redirect_uri);
    fflush(stdout);

    struct pollfd pfds[2];
    pfds[0].fd = listen_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = STDIN_FILENO;
    pfds[1].events = POLLIN;

    char auth_code[1024] = {0};
    int got_code = 0;

    while (!got_code) {
        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            close(listen_fd);
            return -1;
        }

        if (pfds[0].revents & POLLIN) {
            int conn = accept(listen_fd, NULL, NULL);
            if (conn >= 0) {
                char req[4096];
                ssize_t n = read(conn, req, sizeof(req) - 1);
                if (n > 0) {
                    req[n] = '\0';
                    if (parse_code_from_input(req, auth_code, sizeof auth_code) == 0) {
                        got_code = 1;
                        static const char resp[] =
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "Connection: close\r\n\r\n"
                            "<!DOCTYPE html><html><body>"
                            "<h2>minidvc: Google Drive authentication successful!</h2>"
                            "<p>You may close this window and return to your terminal.</p>"
                            "</body></html>";
                        ssize_t wr = write(conn, resp, sizeof(resp) - 1);
                        (void)wr;
                        printf("\nReceived authorization callback.\n");
                    }
                }
                close(conn);
            }
        }

        if (!got_code && (pfds[1].revents & (POLLIN | POLLHUP))) {
            char line[2048];
            if (!fgets(line, sizeof line, stdin))
                break;
            if (parse_code_from_input(line, auth_code, sizeof auth_code) == 0)
                got_code = 1;
        }
    }

    close(listen_fd);

    if (!got_code) {
        fprintf(stderr, "\nminidvc: no authorization code received\n");
        return -1;
    }

    return oauth_exchange_code(client_id, client_secret, auth_code,
                               redirect_uri, refresh_token_out, cap);
}

/*
 * Check that the target Google Drive folder exists and that the caller has
 * appropriate collaborative permissions (read for pull/list, write for push).
 */
static int gdrive_verify_folder(const char *access_token,
                                const char *folder_id,
                                int require_write)
{
    char url[1024];
    snprintf(url, sizeof url,
             "%s/drive/v3/files/%s?supportsAllDrives=true"
             "&fields=id,name,mimeType,trashed,capabilities(canAddChildren,canListChildren)",
             get_api_base(), folder_id);

    char *json = NULL;
    long status = 0;
    if (http_json_request("GET", url, access_token, NULL, NULL,
                          &json, &status) != 0) {
        return -1;
    }

    if (status == 404) {
        fprintf(stderr,
                "minidvc: remote Google Drive folder '%s' not found "
                "(ensure the folder exists and is shared with your Google account)\n",
                folder_id);
        free(json);
        return -1;
    }

    if (status == 401 || status == 403) {
        print_google_error("permission denied accessing remote Google Drive folder",
                           status, json);
        free(json);
        return -1;
    }

    if (status != 200) {
        print_google_error("failed to inspect remote Google Drive folder",
                           status, json);
        free(json);
        return -1;
    }

    char mime[128] = {0};
    if (json_get_string(json, "mimeType", mime, sizeof mime) == 0 &&
        strcmp(mime, FOLDER_MIME) != 0) {
        fprintf(stderr,
                "minidvc: remote ID '%s' is not a Google Drive folder (mimeType: %s)\n",
                folder_id, mime);
        free(json);
        return -1;
    }

    int trashed = 0;
    if (json_get_bool(json, "trashed", &trashed) == 0 && trashed) {
        fprintf(stderr,
                "minidvc: remote Google Drive folder '%s' is in the trash\n",
                folder_id);
        free(json);
        return -1;
    }

    int can_list = 1;
    if (json_get_bool(json, "canListChildren", &can_list) == 0 && !can_list) {
        fprintf(stderr,
                "minidvc: insufficient Google Drive permissions on folder '%s': "
                "cannot list folder contents\n",
                folder_id);
        free(json);
        return -1;
    }

    if (require_write) {
        int can_add = 1;
        if (json_get_bool(json, "canAddChildren", &can_add) == 0 && !can_add) {
            fprintf(stderr,
                    "minidvc: insufficient Google Drive permissions on folder '%s': "
                    "Editor/Contributor permission ('canAddChildren') is required to push\n",
                    folder_id);
            free(json);
            return -1;
        }
    }

    free(json);
    return 0;
}

static int gdrive_find_or_create_subfolder(const char *access_token,
                                           const char *parent_id,
                                           const char *name,
                                           int create_if_missing,
                                           char *out_id,
                                           size_t cap)
{
    char raw_q[512];
    snprintf(raw_q, sizeof raw_q,
             "'%s' in parents and name='%s' and mimeType='%s' and trashed=false",
             parent_id, name, FOLDER_MIME);

    char q_enc[1536];
    if (url_encode(raw_q, q_enc, sizeof q_enc) != 0)
        return -1;

    char url[2048];
    snprintf(url, sizeof url,
             "%s/drive/v3/files?supportsAllDrives=true&includeItemsFromAllDrives=true"
             "&q=%s&fields=files(id,name)",
             get_api_base(), q_enc);

    char *json = NULL;
    long status = 0;
    if (http_json_request("GET", url, access_token, NULL, NULL,
                          &json, &status) != 0) {
        return -1;
    }

    if (status != 200) {
        print_google_error("failed to query remote Drive subfolder", status, json);
        free(json);
        return -1;
    }

    const char *files_pos = strstr(json, "\"files\"");
    if (files_pos &&
        json_extract_string_range(files_pos, json + strlen(json),
                                  "id", out_id, cap) == 0) {
        free(json);
        return 0;
    }
    free(json);

    if (!create_if_missing)
        return 1; /* Not found */

    char create_url[1024];
    snprintf(create_url, sizeof create_url,
             "%s/drive/v3/files?supportsAllDrives=true&fields=id",
             get_api_base());

    char body[1024];
    snprintf(body, sizeof body,
             "{\"name\":\"%s\",\"mimeType\":\"%s\",\"parents\":[\"%s\"]}",
             name, FOLDER_MIME, parent_id);

    json = NULL;
    if (http_json_request("POST", create_url, access_token,
                          "application/json", body, &json, &status) != 0) {
        return -1;
    }

    if ((status != 200 && status != 201) ||
        json_get_string(json, "id", out_id, cap) != 0) {
        print_google_error("failed to create remote Drive subfolder", status, json);
        free(json);
        return -1;
    }

    free(json);
    return 0;
}

/*
 * Resolve the project's isolated namespace on Google Drive:
 *   <remote-root>/projects/<project-id>/manifests
 *   <remote-root>/projects/<project-id>/objects
 *
 * Returns:
 *   0  - resolved (and created if create_if_missing != 0)
 *   1  - namespace does not exist yet (only when create_if_missing == 0)
 *  -1  - error
 */
static int resolve_project_namespace(const char *access_token,
                                     const char *root_folder_id,
                                     const char *project_id,
                                     int create_if_missing,
                                     char *manifests_id_out,
                                     size_t m_cap,
                                     char *objects_id_out,
                                     size_t o_cap)
{
    char projects_dir_id[256] = {0};
    int rc = gdrive_find_or_create_subfolder(access_token, root_folder_id,
                                             "projects", create_if_missing,
                                             projects_dir_id,
                                             sizeof projects_dir_id);
    if (rc != 0)
        return rc;

    char proj_dir_id[256] = {0};
    rc = gdrive_find_or_create_subfolder(access_token, projects_dir_id,
                                         project_id, create_if_missing,
                                         proj_dir_id, sizeof proj_dir_id);
    if (rc != 0)
        return rc;

    if (manifests_id_out && m_cap > 0) {
        manifests_id_out[0] = '\0';
        rc = gdrive_find_or_create_subfolder(access_token, proj_dir_id,
                                             "manifests", create_if_missing,
                                             manifests_id_out, m_cap);
        if (rc < 0 || (create_if_missing && rc != 0))
            return -1;
    }

    if (objects_id_out && o_cap > 0) {
        objects_id_out[0] = '\0';
        rc = gdrive_find_or_create_subfolder(access_token, proj_dir_id,
                                             "objects", create_if_missing,
                                             objects_id_out, o_cap);
        if (rc < 0 || (create_if_missing && rc != 0))
            return -1;
    }

    return 0;
}

static void file_list_free(struct remote_file_list *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int file_list_append(struct remote_file_list *list,
                            const char *id, const char *name)
{
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 64;
        struct remote_file *n = realloc(list->items,
                                        new_cap * sizeof(*list->items));
        if (!n)
            return -1;
        list->items = n;
        list->cap = new_cap;
    }

    snprintf(list->items[list->count].id,
             sizeof list->items[list->count].id, "%s", id);
    snprintf(list->items[list->count].name,
             sizeof list->items[list->count].name, "%s", name);
    ++list->count;
    return 0;
}

static const char *file_list_find_id(const struct remote_file_list *list,
                                     const char *name)
{
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].name, name) == 0)
            return list->items[i].id;
    }
    return NULL;
}

static int parse_files_array(const char *json, struct remote_file_list *list)
{
    const char *end = json + strlen(json);
    const char *files_val = json_find_key_in_range(json, end, "files");
    if (!files_val)
        return -1;

    const char *p = strchr(files_val, '[');
    if (!p)
        return -1;
    ++p;

    while (p < end) {
        while (p < end && *p != '{' && *p != ']')
            ++p;
        if (p >= end || *p == ']')
            break;

        const char *obj_start = p++;
        int depth = 1;
        int in_str = 0;
        while (p < end && depth > 0) {
            if (*p == '\\' && in_str) {
                ++p;
                if (p < end)
                    ++p;
                continue;
            }
            if (*p == '"')
                in_str = !in_str;
            else if (!in_str && *p == '{')
                ++depth;
            else if (!in_str && *p == '}')
                --depth;
            ++p;
        }
        const char *obj_end = p;

        char id[256], name[512];
        if (json_extract_string_range(obj_start, obj_end, "id", id, sizeof id) == 0 &&
            json_extract_string_range(obj_start, obj_end, "name", name, sizeof name) == 0) {
            if (file_list_append(list, id, name) != 0)
                return -1;
        }
    }

    return 0;
}

static int gdrive_list_folder(const char *access_token,
                              const char *folder_id,
                              struct remote_file_list *list)
{
    memset(list, 0, sizeof(*list));

    char raw_q[512];
    snprintf(raw_q, sizeof raw_q,
             "'%s' in parents and trashed=false", folder_id);

    char q_enc[1536];
    if (url_encode(raw_q, q_enc, sizeof q_enc) != 0)
        return -1;

    char page_token[512] = {0};

    for (;;) {
        char url[3072];
        if (page_token[0]) {
            char pt_enc[1024];
            if (url_encode(page_token, pt_enc, sizeof pt_enc) != 0) {
                file_list_free(list);
                return -1;
            }
            snprintf(url, sizeof url,
                     "%s/drive/v3/files?supportsAllDrives=true"
                     "&includeItemsFromAllDrives=true&pageSize=1000"
                     "&q=%s&pageToken=%s&fields=nextPageToken,files(id,name)",
                     get_api_base(), q_enc, pt_enc);
        } else {
            snprintf(url, sizeof url,
                     "%s/drive/v3/files?supportsAllDrives=true"
                     "&includeItemsFromAllDrives=true&pageSize=1000"
                     "&q=%s&fields=nextPageToken,files(id,name)",
                     get_api_base(), q_enc);
        }

        char *json = NULL;
        long status = 0;
        if (http_json_request("GET", url, access_token, NULL, NULL,
                              &json, &status) != 0) {
            file_list_free(list);
            return -1;
        }

        if (status != 200) {
            print_google_error("failed to list remote Drive folder", status, json);
            free(json);
            file_list_free(list);
            return -1;
        }

        if (parse_files_array(json, list) != 0) {
            free(json);
            file_list_free(list);
            return -1;
        }

        if (json_get_string(json, "nextPageToken",
                            page_token, sizeof page_token) != 0 ||
            !page_token[0]) {
            free(json);
            break;
        }
        free(json);
    }

    return 0;
}

static int parse_location_header(const char *hdr_path, char *out, size_t cap)
{
    FILE *f = fopen(hdr_path, "r");
    if (!f)
        return -1;

    char line[2048];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncasecmp(line, "Location:", 9) == 0) {
            char *val = line + 9;
            while (*val == ' ' || *val == '\t')
                ++val;
            trim_newline(val);
            if (*val && strlen(val) < cap) {
                strcpy(out, val);
                found = 1;
            }
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/*
 * Upload a file to a Google Drive parent folder using the Drive v3 resumable
 * upload protocol (supports arbitrary file sizes without buffering in memory).
 */
static int gdrive_upload_file(const char *access_token,
                              const char *parent_id,
                              const char *remote_name,
                              const char *local_path)
{
    if (ensure_tmp_dir() != 0)
        return -1;

    char init_url[1024];
    snprintf(init_url, sizeof init_url,
             "%s/upload/drive/v3/files?uploadType=resumable&supportsAllDrives=true",
             get_api_base());

    char metadata[1024];
    snprintf(metadata, sizeof metadata,
             "{\"name\":\"%s\",\"parents\":[\"%s\"]}",
             remote_name, parent_id);

    char body_path[] = ".mydvc/tmp/up-body-XXXXXX";
    char hdr_path[] = ".mydvc/tmp/up-hdr-XXXXXX";
    int bfd = mkstemp(body_path);
    if (bfd < 0)
        return -1;
    close(bfd);

    int hfd = mkstemp(hdr_path);
    if (hfd < 0) {
        unlink(body_path);
        return -1;
    }
    close(hfd);

    long status = 0;
    if (http_request("POST", init_url, access_token,
                     "application/json; charset=UTF-8",
                     metadata, NULL, body_path, hdr_path, &status) != 0) {
        unlink(body_path);
        unlink(hdr_path);
        return -1;
    }

    if (status != 200) {
        char *err = read_file_string(body_path);
        print_google_error("failed to initiate Google Drive resumable upload",
                           status, err);
        free(err);
        unlink(body_path);
        unlink(hdr_path);
        return -1;
    }

    char upload_url[2048];
    int loc_ok = parse_location_header(hdr_path, upload_url, sizeof upload_url);
    unlink(hdr_path);

    if (loc_ok != 0) {
        fprintf(stderr, "minidvc: missing Location header in resumable upload response\n");
        unlink(body_path);
        return -1;
    }

    if (http_request("PUT", upload_url, access_token,
                     "application/octet-stream",
                     NULL, local_path, body_path, NULL, &status) != 0) {
        unlink(body_path);
        return -1;
    }

    if (status != 200 && status != 201) {
        char *err = read_file_string(body_path);
        print_google_error("failed to upload file content to Google Drive",
                           status, err);
        free(err);
        unlink(body_path);
        return -1;
    }

    unlink(body_path);
    return 0;
}

static int gdrive_download_file(const char *access_token,
                                const char *file_id,
                                const char *dest_tmp_path)
{
    char url[1024];
    snprintf(url, sizeof url,
             "%s/drive/v3/files/%s?alt=media&supportsAllDrives=true",
             get_api_base(), file_id);

    long status = 0;
    if (http_request("GET", url, access_token, NULL, NULL, NULL,
                     dest_tmp_path, NULL, &status) != 0) {
        return -1;
    }

    if (status != 200) {
        char *err = read_file_string(dest_tmp_path);
        print_google_error("failed to download file from Google Drive",
                           status, err);
        free(err);
        return -1;
    }

    return 0;
}

int remote_set_folder(const char *folder_id_or_url)
{
    struct project_info proj;
    if (project_require(&proj) != 0)
        return -1;

    char folder_id[256];
    if (extract_folder_id(folder_id_or_url, folder_id, sizeof folder_id) != 0) {
        fprintf(stderr,
                "minidvc: invalid Google Drive folder ID or URL: '%s'\n",
                folder_id_or_url ? folder_id_or_url : "");
        return -1;
    }

    snprintf(proj.remote_folder_id, sizeof proj.remote_folder_id, "%s", folder_id);
    if (project_save(&proj) != 0)
        return -1;

    printf("Configured Google Drive remote folder '%s' for project '%s' (%s)\n",
           proj.remote_folder_id, proj.project_name, proj.project_id);
    return 0;
}

int remote_show(void)
{
    struct project_info proj;
    int has_proj = (project_load(&proj) == 0);

    struct credentials_info creds;
    credentials_load(&creds);

    printf("Project & Remote Configuration:\n");
    printf("  Project Name:  %s\n",
           has_proj && proj.project_name[0] ? proj.project_name : "(not initialized)");
    printf("  Project ID:    %s\n",
           has_proj && proj.project_id[0] ? proj.project_id : "(not initialized)");
    printf("  Folder ID:     %s\n",
           proj.remote_folder_id[0] ? proj.remote_folder_id : "(not set)");
    if (has_proj && proj.remote_folder_id[0]) {
        printf("  Remote Path:   %s/projects/%s/\n",
               proj.remote_folder_id, proj.project_id);
    }
    printf("  Client ID:     %s\n",
           creds.client_id[0] ? creds.client_id : "(not set)");
    printf("  Client Secret: %s\n",
           creds.client_secret[0] ? "(configured)" : "(not set)");
    printf("  Refresh Token: %s\n",
           creds.refresh_token[0] ? "(configured)" : "(not set)");
    return 0;
}

int remote_auth(const char *client_id, const char *client_secret,
                const char *refresh_token)
{
    if (!client_id || !*client_id || !client_secret || !*client_secret) {
        fprintf(stderr, "minidvc: client_id and client_secret are required\n");
        return -1;
    }

    struct credentials_info creds;
    credentials_load(&creds);
    snprintf(creds.client_id, sizeof creds.client_id, "%s", client_id);
    snprintf(creds.client_secret, sizeof creds.client_secret, "%s", client_secret);

    if (refresh_token && *refresh_token) {
        snprintf(creds.refresh_token, sizeof creds.refresh_token,
                 "%s", refresh_token);
    } else {
        if (interactive_oauth_flow(client_id, client_secret,
                                   creds.refresh_token,
                                   sizeof creds.refresh_token) != 0) {
            return -1;
        }
    }

    char access_token[2048];
    if (oauth_get_access_token(&creds, access_token, sizeof access_token) != 0) {
        fprintf(stderr, "minidvc: failed to verify Google Drive credentials\n");
        return -1;
    }

    if (credentials_save(&creds) != 0)
        return -1;

    /* Migrate and remove legacy .mydvc/config if it existed */
    struct project_info proj;
    if (project_load(&proj) == 0) {
        project_save(&proj);
        unlink(LEGACY_CONFIG_PATH);
    }

    printf("Authenticated with Google Drive and saved local credentials to %s\n",
           CREDENTIALS_FILE_PATH);
    return 0;
}

static int push_object_if_missing(const char *access_token,
                                  const char *objects_folder_id,
                                  struct remote_file_list *remote_objects,
                                  const char *hash,
                                  int *uploaded_count)
{
    char remote_name[80];
    snprintf(remote_name, sizeof remote_name, "%s.zst", hash);

    if (file_list_find_id(remote_objects, remote_name) != NULL)
        return 0;

    if (storage_verify(hash) != 0)
        return -1;

    char local_path[160];
    if (storage_object_path(hash, local_path, sizeof local_path) != 0)
        return -1;

    printf("Uploading object %s...\n", hash);
    if (gdrive_upload_file(access_token, objects_folder_id,
                           remote_name, local_path) != 0) {
        return -1;
    }

    file_list_append(remote_objects, "uploaded", remote_name);
    ++(*uploaded_count);
    return 0;
}

static int push_snapshot_by_name(const char *access_token,
                                 const char *manifests_folder_id,
                                 const char *objects_folder_id,
                                 struct remote_file_list *remote_manifests,
                                 struct remote_file_list *remote_objects,
                                 const char *name,
                                 int *uploaded_manifests,
                                 int *uploaded_objects)
{
    char mpath[4096];
    if (manifest_file_path(name, mpath, sizeof mpath) != 0) {
        fprintf(stderr, "minidvc: invalid snapshot name: %s\n", name);
        return -1;
    }

    struct stat st;
    if (stat(mpath, &st) != 0) {
        fprintf(stderr, "minidvc: local snapshot '%s' not found\n", name);
        return -1;
    }

    char (*hashes)[65] = malloc(MINIDVC_MAX_FILES * sizeof(*hashes));
    if (!hashes)
        return -1;

    int count = manifest_read_hashes(mpath, hashes, MINIDVC_MAX_FILES);
    if (count < 0) {
        free(hashes);
        return -1;
    }

    for (int i = 0; i < count; ++i) {
        if (push_object_if_missing(access_token, objects_folder_id,
                                   remote_objects, hashes[i],
                                   uploaded_objects) != 0) {
            free(hashes);
            return -1;
        }
    }
    free(hashes);

    char remote_mname[512];
    snprintf(remote_mname, sizeof remote_mname, "%s.manifest", name);
    if (file_list_find_id(remote_manifests, remote_mname) == NULL) {
        printf("Uploading manifest '%s'...\n", name);
        if (gdrive_upload_file(access_token, manifests_folder_id,
                               remote_mname, mpath) != 0) {
            return -1;
        }
        file_list_append(remote_manifests, "uploaded", remote_mname);
        ++(*uploaded_manifests);
    }

    return 0;
}

int remote_push(const char *snapshot_name)
{
    if (snapshot_name && !manifest_valid_name(snapshot_name)) {
        fprintf(stderr, "minidvc: invalid snapshot name: %s\n", snapshot_name);
        return -1;
    }

    struct project_info proj;
    if (project_require(&proj) != 0)
        return -1;

    if (!proj.remote_folder_id[0]) {
        fprintf(stderr,
                "minidvc: remote folder not configured; run 'minidvc remote set <folder-id>' first\n");
        return -1;
    }

    struct credentials_info creds;
    credentials_load(&creds);

    char access_token[2048];
    if (oauth_get_access_token(&creds, access_token, sizeof access_token) != 0)
        return -1;

    if (gdrive_verify_folder(access_token, proj.remote_folder_id, 1) != 0)
        return -1;

    char manifests_folder_id[256], objects_folder_id[256];
    if (resolve_project_namespace(access_token, proj.remote_folder_id,
                                  proj.project_id, 1,
                                  manifests_folder_id, sizeof manifests_folder_id,
                                  objects_folder_id, sizeof objects_folder_id) != 0) {
        return -1;
    }

    struct remote_file_list remote_manifests = {0};
    struct remote_file_list remote_objects = {0};

    if (gdrive_list_folder(access_token, manifests_folder_id,
                           &remote_manifests) != 0 ||
        gdrive_list_folder(access_token, objects_folder_id,
                           &remote_objects) != 0) {
        file_list_free(&remote_manifests);
        file_list_free(&remote_objects);
        return -1;
    }

    int uploaded_manifests = 0;
    int uploaded_objects = 0;
    int rc = 0;

    if (snapshot_name) {
        rc = push_snapshot_by_name(access_token, manifests_folder_id,
                                   objects_folder_id, &remote_manifests,
                                   &remote_objects, snapshot_name,
                                   &uploaded_manifests, &uploaded_objects);
    } else {
        DIR *d = opendir(".mydvc/manifests");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                size_t len = strlen(e->d_name);
                if (len <= 9 ||
                    strcmp(e->d_name + len - 9, ".manifest") != 0)
                    continue;

                char name[256];
                if (len - 9 >= sizeof name)
                    continue;
                memcpy(name, e->d_name, len - 9);
                name[len - 9] = '\0';

                if (!manifest_valid_name(name))
                    continue;

                if (push_snapshot_by_name(access_token, manifests_folder_id,
                                          objects_folder_id,
                                          &remote_manifests, &remote_objects,
                                          name, &uploaded_manifests,
                                          &uploaded_objects) != 0) {
                    rc = -1;
                    break;
                }
            }
            closedir(d);
        } else if (errno != ENOENT) {
            perror(".mydvc/manifests");
            rc = -1;
        }
    }

    file_list_free(&remote_manifests);
    file_list_free(&remote_objects);

    if (rc != 0)
        return -1;

    printf("Pushed project '%s' to Google Drive (%d object(s), %d manifest(s) uploaded)\n",
           proj.project_name, uploaded_objects, uploaded_manifests);
    return 0;
}

static int pull_object_by_hash(const char *access_token,
                               const struct remote_file_list *remote_objects,
                               const char *hash,
                               int *downloaded_objects)
{
    if (storage_has_object(hash))
        return storage_verify(hash);

    char remote_name[80];
    snprintf(remote_name, sizeof remote_name, "%s.zst", hash);

    const char *file_id = file_list_find_id(remote_objects, remote_name);
    if (!file_id) {
        fprintf(stderr,
                "minidvc: object %s not found in remote Google Drive folder\n",
                hash);
        return -1;
    }

    if (ensure_tmp_dir() != 0)
        return -1;

    char tmp[] = ".mydvc/tmp/dl-object-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    close(fd);

    printf("Downloading object %s...\n", hash);
    if (gdrive_download_file(access_token, file_id, tmp) != 0) {
        unlink(tmp);
        return -1;
    }

    int rc = storage_import_object(hash, tmp);
    unlink(tmp);

    if (rc != 0)
        return -1;

    printf("Stored and verified %s\n", hash);
    ++(*downloaded_objects);
    return 0;
}

static int pull_snapshot_by_name(const char *access_token,
                                 const struct remote_file_list *remote_manifests,
                                 const struct remote_file_list *remote_objects,
                                 const char *name,
                                 int *downloaded_manifests,
                                 int *downloaded_objects)
{
    char mpath[4096];
    if (manifest_file_path(name, mpath, sizeof mpath) != 0) {
        fprintf(stderr, "minidvc: invalid snapshot name: %s\n", name);
        return -1;
    }

    struct stat st;
    if (stat(mpath, &st) != 0) {
        char remote_mname[512];
        snprintf(remote_mname, sizeof remote_mname, "%s.manifest", name);
        const char *file_id = file_list_find_id(remote_manifests, remote_mname);
        if (!file_id) {
            fprintf(stderr,
                    "minidvc: snapshot '%s' not found on Google Drive remote\n",
                    name);
            return -1;
        }

        if (ensure_tmp_dir() != 0)
            return -1;

        char tmp[] = ".mydvc/tmp/dl-manifest-XXXXXX";
        int fd = mkstemp(tmp);
        if (fd < 0) {
            perror("mkstemp");
            return -1;
        }
        close(fd);

        printf("Downloading manifest '%s'...\n", name);
        if (gdrive_download_file(access_token, file_id, tmp) != 0) {
            unlink(tmp);
            return -1;
        }

        int rc = manifest_import_file(name, tmp);
        unlink(tmp);
        if (rc != 0)
            return -1;

        ++(*downloaded_manifests);
    }

    char (*hashes)[65] = malloc(MINIDVC_MAX_FILES * sizeof(*hashes));
    if (!hashes)
        return -1;

    int count = manifest_read_hashes(mpath, hashes, MINIDVC_MAX_FILES);
    if (count < 0) {
        free(hashes);
        return -1;
    }

    for (int i = 0; i < count; ++i) {
        if (pull_object_by_hash(access_token, remote_objects,
                                hashes[i], downloaded_objects) != 0) {
            free(hashes);
            return -1;
        }
    }

    free(hashes);
    return 0;
}

int remote_pull(const char *snapshot_name)
{
    if (snapshot_name && !manifest_valid_name(snapshot_name)) {
        fprintf(stderr, "minidvc: invalid snapshot name: %s\n", snapshot_name);
        return -1;
    }

    struct project_info proj;
    if (project_require(&proj) != 0)
        return -1;

    if (!proj.remote_folder_id[0]) {
        fprintf(stderr,
                "minidvc: remote folder not configured; run 'minidvc remote set <folder-id>' first\n");
        return -1;
    }

    struct credentials_info creds;
    credentials_load(&creds);

    char access_token[2048];
    if (oauth_get_access_token(&creds, access_token, sizeof access_token) != 0)
        return -1;

    if (gdrive_verify_folder(access_token, proj.remote_folder_id, 0) != 0)
        return -1;

    char manifests_folder_id[256] = {0};
    char objects_folder_id[256] = {0};

    int ns_rc = resolve_project_namespace(access_token, proj.remote_folder_id,
                                          proj.project_id, 0,
                                          manifests_folder_id,
                                          sizeof manifests_folder_id,
                                          objects_folder_id,
                                          sizeof objects_folder_id);
    if (ns_rc < 0)
        return -1;

    struct remote_file_list remote_manifests = {0};
    struct remote_file_list remote_objects = {0};

    if (ns_rc == 0 && manifests_folder_id[0] &&
        gdrive_list_folder(access_token, manifests_folder_id,
                           &remote_manifests) != 0) {
        return -1;
    }
    if (ns_rc == 0 && objects_folder_id[0] &&
        gdrive_list_folder(access_token, objects_folder_id,
                           &remote_objects) != 0) {
        file_list_free(&remote_manifests);
        return -1;
    }

    int downloaded_manifests = 0;
    int downloaded_objects = 0;
    int rc = 0;

    if (snapshot_name) {
        rc = pull_snapshot_by_name(access_token, &remote_manifests,
                                   &remote_objects, snapshot_name,
                                   &downloaded_manifests, &downloaded_objects);
    } else {
        for (size_t i = 0; i < remote_manifests.count; ++i) {
            const char *fname = remote_manifests.items[i].name;
            size_t len = strlen(fname);
            if (len <= 9 || strcmp(fname + len - 9, ".manifest") != 0)
                continue;

            char name[256];
            if (len - 9 >= sizeof name)
                continue;
            memcpy(name, fname, len - 9);
            name[len - 9] = '\0';

            if (!manifest_valid_name(name))
                continue;

            if (pull_snapshot_by_name(access_token, &remote_manifests,
                                      &remote_objects, name,
                                      &downloaded_manifests,
                                      &downloaded_objects) != 0) {
                rc = -1;
                break;
            }
        }
    }

    file_list_free(&remote_manifests);
    file_list_free(&remote_objects);

    if (rc != 0)
        return -1;

    printf("Pulled project '%s' from Google Drive (%d object(s), %d manifest(s) downloaded)\n",
           proj.project_name, downloaded_objects, downloaded_manifests);
    return 0;
}

int remote_list(void)
{
    struct project_info proj;
    if (project_require(&proj) != 0)
        return -1;

    if (!proj.remote_folder_id[0]) {
        fprintf(stderr,
                "minidvc: remote folder not configured; run 'minidvc remote set <folder-id>' first\n");
        return -1;
    }

    struct credentials_info creds;
    credentials_load(&creds);

    char access_token[2048];
    if (oauth_get_access_token(&creds, access_token, sizeof access_token) != 0)
        return -1;

    if (gdrive_verify_folder(access_token, proj.remote_folder_id, 0) != 0)
        return -1;

    char manifests_folder_id[256] = {0};
    int ns_rc = resolve_project_namespace(access_token, proj.remote_folder_id,
                                          proj.project_id, 0,
                                          manifests_folder_id,
                                          sizeof manifests_folder_id,
                                          NULL, 0);
    if (ns_rc < 0)
        return -1;

    if (ns_rc == 1 || !manifests_folder_id[0]) {
        manifest_print_summaries(NULL, 0);
        return 0;
    }

    struct remote_file_list remote_manifests = {0};
    if (gdrive_list_folder(access_token, manifests_folder_id,
                           &remote_manifests) != 0) {
        return -1;
    }

    struct snapshot_summary *summaries = NULL;
    size_t count = 0;
    size_t cap = 0;
    int rc = 0;

    for (size_t i = 0; i < remote_manifests.count; ++i) {
        const char *fname = remote_manifests.items[i].name;
        size_t len = strlen(fname);
        if (len <= 9 || strcmp(fname + len - 9, ".manifest") != 0)
            continue;

        char name[256];
        if (len - 9 >= sizeof name)
            continue;
        memcpy(name, fname, len - 9);
        name[len - 9] = '\0';

        if (!manifest_valid_name(name))
            continue;

        struct snapshot_summary sum;
        int got_sum = 0;

        char local_mpath[4096];
        struct stat st;
        if (manifest_file_path(name, local_mpath, sizeof local_mpath) == 0 &&
            stat(local_mpath, &st) == 0 &&
            manifest_read_summary(local_mpath, name, &sum) == 0) {
            got_sum = 1;
        } else {
            if (ensure_tmp_dir() != 0) {
                rc = -1;
                break;
            }

            char tmp[] = ".mydvc/tmp/list-manifest-XXXXXX";
            int fd = mkstemp(tmp);
            if (fd < 0) {
                perror("mkstemp");
                rc = -1;
                break;
            }
            close(fd);

            if (gdrive_download_file(access_token,
                                     remote_manifests.items[i].id,
                                     tmp) != 0) {
                unlink(tmp);
                rc = -1;
                break;
            }

            if (manifest_read_summary(tmp, name, &sum) == 0)
                got_sum = 1;

            unlink(tmp);
        }

        if (got_sum) {
            if (count == cap) {
                size_t new_cap = cap ? cap * 2 : 32;
                struct snapshot_summary *n = realloc(summaries,
                                                     new_cap * sizeof(*summaries));
                if (!n) {
                    rc = -1;
                    break;
                }
                summaries = n;
                cap = new_cap;
            }
            summaries[count++] = sum;
        }
    }

    file_list_free(&remote_manifests);

    if (rc != 0) {
        free(summaries);
        return -1;
    }

    manifest_print_summaries(summaries, count);
    free(summaries);
    return 0;
}
