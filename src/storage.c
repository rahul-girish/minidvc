#include "storage.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#define CHUNK_SIZE (1024 * 1024)

static int make_dir(const char* path)
{
	if(mkdir(path, 0755) == 0 || errno == EEXIST)
	{
		struct stat st;
		return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
	}
	return -1;
}

static int write_all(FILE *out, const void *buf, size_t size)
{
	return fwrite(buf, 1, size, out) == size ? 0 : -1;
}

static void hex_encode(const unsigned char *src, size_t len, char *dst)
{
	static const char hex[] = "0123456789abcdef";

	for(size_t i = 0; i < len; ++i)
	{
		dst[i * 2] = hex[src[i] >> 4];
		dst[i * 2 + 1] = hex[src[i] & 15];
	}

	dst[len * 2] = '\0';
}

static int valid_hash(const char *hash)
{
    if (strlen(hash) != 64)
        return 0;

    for (size_t i = 0; i < 64; ++i) {
        char c = hash[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static int object_path(const char *hash, char *out, size_t cap)
{
    int n = snprintf(out, cap,
        ".mydvc/objects/%.2s/%.2s/%s.zst",
        hash, hash + 2, hash);

    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static int prepare_object_dirs(const char *hash)
{
    char dir[128];

    if (make_dir(".mydvc") ||
        make_dir(".mydvc/tmp") ||
        make_dir(".mydvc/objects"))
        return -1;

    snprintf(dir, sizeof dir, ".mydvc/objects/%.2s", hash);
    if (make_dir(dir))
        return -1;

    snprintf(dir, sizeof dir, ".mydvc/objects/%.2s/%.2s",
             hash, hash + 2);
    return make_dir(dir);
}

int storage_add(const char *path)
{
    FILE *in = fopen(path, "rb");
    if (!in) {
        perror(path);
        return -1;
    }

    char tmp[] = ".mydvc/tmp/object-XXXXXX";
    if (make_dir(".mydvc") ||
        make_dir(".mydvc/tmp")) {
        fclose(in);
        fprintf(stderr, "minidvc: cannot create temporary directory\n");
        return -1;
    }

    int fd = mkstemp(tmp);
    if (fd < 0) {
        perror("mkstemp");
        fclose(in);
        return -1;
    }

    FILE *out = fdopen(fd, "wb");
    if (!out) {
        perror("fdopen");
        close(fd);
        unlink(tmp);
        fclose(in);
        return -1;
    }

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    ZSTD_CCtx *zc = ZSTD_createCCtx();

    unsigned char *ibuf = malloc(CHUNK_SIZE);
    unsigned char *obuf = malloc(ZSTD_CStreamOutSize());

    int ok = md && zc && ibuf && obuf;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (ok)
        ok = EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1;

    if (ok) {
        size_t zr = ZSTD_CCtx_setParameter(
            zc, ZSTD_c_compressionLevel, 3);
        ok = !ZSTD_isError(zr);
    }

    while (ok) {
        size_t n = fread(ibuf, 1, CHUNK_SIZE, in);

        if (n && EVP_DigestUpdate(md, ibuf, n) != 1) {
            ok = 0;
            break;
        }

        ZSTD_inBuffer input = { ibuf, n, 0 };

        while (ok && input.pos < input.size) {
            ZSTD_outBuffer output = {
                obuf, ZSTD_CStreamOutSize(), 0
            };

            size_t zr = ZSTD_compressStream2(
                zc, &output, &input, ZSTD_e_continue);

            if (ZSTD_isError(zr) ||
                write_all(out, obuf, output.pos))
                ok = 0;
        }

        if (!ok || n < CHUNK_SIZE) {
            if (ferror(in))
                ok = 0;
            break;
        }
    }

    if (ok) {
        size_t remaining;
        do {
            ZSTD_inBuffer input = { NULL, 0, 0 };
            ZSTD_outBuffer output = {
                obuf, ZSTD_CStreamOutSize(), 0
            };

            remaining = ZSTD_compressStream2(
                zc, &output, &input, ZSTD_e_end);

            if (ZSTD_isError(remaining) ||
                write_all(out, obuf, output.pos))
                ok = 0;
        } while (ok && remaining != 0);
    }

    if (ok)
        ok = EVP_DigestFinal_ex(md, digest, &digest_len) == 1
             && digest_len == 32;

    EVP_MD_CTX_free(md);
    ZSTD_freeCCtx(zc);
    free(ibuf);
    free(obuf);

    if (fclose(in) != 0)
        ok = 0;

    if (fclose(out) != 0)
        ok = 0;

    if (!ok) {
        fprintf(stderr, "minidvc: compression or hashing failed\n");
        unlink(tmp);
        return -1;
    }

    char hash[65];
    char dest[160];
    hex_encode(digest, 32, hash);

    if (prepare_object_dirs(hash) ||
        object_path(hash, dest, sizeof dest)) {
        fprintf(stderr, "minidvc: cannot prepare object path\n");
        unlink(tmp);
        return -1;
    }

    /*
     * link() publishes atomically without replacing an existing
     * object. Both paths are on the same filesystem.
     */
    if (link(tmp, dest) == 0) {
        unlink(tmp);
        printf("Stored %s\n", hash);
        return 0;
    }

    if (errno == EEXIST) {
		unlink(tmp);

		if (storage_verify(hash) != 0) {
			fprintf(stderr,
					"minidvc: existing object is corrupt: %s\n",
					hash);
			return -1;
		}

		printf("Object already exists and is valid: %s\n", hash);
		return 0;
	}

    perror("link");
    unlink(tmp);
    return -1;
}

int storage_verify(const char *hash)
{
    if (!valid_hash(hash)) {
        fprintf(stderr, "minidvc: invalid SHA-256 hash\n");
        return -1;
    }

    char path[160];
    if (object_path(hash, path, sizeof path))
        return -1;

    FILE *in = fopen(path, "rb");
    if (!in) {
        perror(path);
        return -1;
    }

    ZSTD_DCtx *zd = ZSTD_createDCtx();
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    unsigned char *ibuf = malloc(CHUNK_SIZE);
    unsigned char *obuf = malloc(ZSTD_DStreamOutSize());

    int ok = zd && md && ibuf && obuf;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    size_t remaining = 1;

    if (ok)
        ok = EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1;

    while (ok) {
        size_t n = fread(ibuf, 1, CHUNK_SIZE, in);

        ZSTD_inBuffer input = { ibuf, n, 0 };

        while (ok && input.pos < input.size) {
            ZSTD_outBuffer output = {
                obuf, ZSTD_DStreamOutSize(), 0
            };

            remaining = ZSTD_decompressStream(
                zd, &output, &input);

            if (ZSTD_isError(remaining)) {
                ok = 0;
                break;
            }

            if (output.pos &&
                EVP_DigestUpdate(md, obuf, output.pos) != 1) {
                ok = 0;
                break;
            }

            /* Reject trailing data after the compressed frame. */
            if (remaining == 0 && input.pos < input.size) {
                ok = 0;
                break;
            }
        }

        if (n < CHUNK_SIZE) {
            if (ferror(in) || remaining != 0)
                ok = 0;
            break;
        }
    }

    if (ok)
        ok = EVP_DigestFinal_ex(md, digest, &digest_len) == 1
             && digest_len == 32;

    char actual[65];
    if (ok) {
        hex_encode(digest, 32, actual);
        ok = strcmp(actual, hash) == 0;
    }

    free(ibuf);
    free(obuf);
    EVP_MD_CTX_free(md);
    ZSTD_freeDCtx(zd);

    if (fclose(in) != 0)
        ok = 0;

    if (!ok) {
        fprintf(stderr, "minidvc: verification failed for %s\n", hash);
        return -1;
    }

    printf("Verified %s\n", hash);
    return 0;
}

int storage_restore(const char *hash, const char *output_path)
{
    if (!valid_hash(hash) || !output_path || !*output_path) {
        fprintf(stderr, "minidvc: invalid hash or output path\n");
        return -1;
    }

    char src_path[160];
    if (object_path(hash, src_path, sizeof src_path) != 0)
        return -1;

    FILE *in = fopen(src_path, "rb");
    if (!in) {
        perror(src_path);
        return -1;
    }

    /*
     * Create the temporary file beside the destination so that
     * publishing it with link() stays on the same filesystem.
     */
    size_t path_len = strlen(output_path);
    static const char suffix[] = ".minidvc-XXXXXX";

    if (path_len > SIZE_MAX - sizeof suffix) {
        fclose(in);
        fprintf(stderr, "minidvc: output path too long\n");
        return -1;
    }

    char *tmp = malloc(path_len + sizeof suffix);
    if (!tmp) {
        fclose(in);
        return -1;
    }

    memcpy(tmp, output_path, path_len);
    memcpy(tmp + path_len, suffix, sizeof suffix);

    int fd = mkstemp(tmp);
    if (fd < 0) {
        perror("mkstemp");
        free(tmp);
        fclose(in);
        return -1;
    }

    FILE *out = fdopen(fd, "wb");
    if (!out) {
        perror("fdopen");
        close(fd);
        unlink(tmp);
        free(tmp);
        fclose(in);
        return -1;
    }

    ZSTD_DCtx *zd = ZSTD_createDCtx();
    EVP_MD_CTX *md = EVP_MD_CTX_new();

    unsigned char *ibuf = malloc(CHUNK_SIZE);
    unsigned char *obuf = malloc(ZSTD_DStreamOutSize());

    int ok = zd && md && ibuf && obuf;
    int frame_done = 0;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    size_t remaining = 1;

    if (ok)
        ok = EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1;

    while (ok) {
        size_t n = fread(ibuf, 1, CHUNK_SIZE, in);

        if (n == 0 && ferror(in)) {
            ok = 0;
            break;
        }

        ZSTD_inBuffer input = { ibuf, n, 0 };

        while (ok && input.pos < input.size) {
            if (frame_done) {
                /* Reject trailing bytes or concatenated frames. */
                ok = 0;
                break;
            }

            ZSTD_outBuffer output = {
                obuf, ZSTD_DStreamOutSize(), 0
            };

            remaining = ZSTD_decompressStream(
                zd, &output, &input);

            if (ZSTD_isError(remaining)) {
                ok = 0;
                break;
            }

            if (output.pos > 0) {
                if (EVP_DigestUpdate(
                        md, obuf, output.pos) != 1 ||
                    fwrite(obuf, 1, output.pos, out) != output.pos) {
                    ok = 0;
                    break;
                }
            }

            if (remaining == 0) {
                frame_done = 1;

                if (input.pos < input.size) {
                    ok = 0;
                    break;
                }
            }
        }

        if (n < CHUNK_SIZE)
            break;
    }

    if (ok)
        ok = frame_done &&
             EVP_DigestFinal_ex(md, digest, &digest_len) == 1 &&
             digest_len == 32;

    char actual[65];
    if (ok) {
        hex_encode(digest, 32, actual);
        ok = strcmp(actual, hash) == 0;
    }

    free(ibuf);
    free(obuf);
    ZSTD_freeDCtx(zd);
    EVP_MD_CTX_free(md);

    if (fclose(in) != 0)
        ok = 0;

    /*
     * Flush and close before publishing the verified output.
     */
    if (fflush(out) != 0)
        ok = 0;

    if (ok && fsync(fileno(out)) != 0)
        ok = 0;

    if (fclose(out) != 0)
        ok = 0;

    if (!ok) {
        fprintf(stderr,
                "minidvc: restore failed; output hash or I/O error\n");
        unlink(tmp);
        free(tmp);
        return -1;
    }

    /*
     * Hard-link publication is atomic and will not replace an
     * existing destination.
     */
    if (link(tmp, output_path) != 0) {
        perror(output_path);
        unlink(tmp);
        free(tmp);
        return -1;
    }

    unlink(tmp);
    free(tmp);

    printf("Restored %s\n", output_path);
    return 0;
}