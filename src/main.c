#include "hash.h"
#include "manifest.h"
#include "project.h"
#include "remote.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
        "minidvc - Lightweight, project-aware data versioning tool\n\n"
        "Usage:\n"
        "  %s init [project-name]\n"
        "      Initialize a minidvc project with a unique UUID in the current directory\n\n"
        "  %s snapshot <name> <file-or-directory>...\n"
        "      Create an immutable snapshot preserving project-relative file paths\n\n"
        "  %s list [--local | --remote]\n"
        "      List available snapshots (defaults to --remote)\n\n"
        "  %s push [snapshot]\n"
        "      Upload local snapshot(s) and objects to the project's Google Drive namespace\n\n"
        "  %s pull [snapshot]\n"
        "      Download remote snapshot(s) and objects for the current project\n\n"
        "  %s checkout <snapshot> [output-dir]\n"
        "      Restore snapshot files to their project-relative paths (or into output-dir)\n\n"
        "  %s remote set <folder-id-or-url>\n"
        "      Configure the shared Google Drive folder for this project\n\n"
        "  %s remote show\n"
        "      Display project metadata, remote namespace, and local auth status\n\n"
        "  %s auth <client-id> <client-secret> [refresh-token]\n"
        "      Authenticate with Google Drive OAuth 2.0 (stored locally in .mydvc/credentials)\n\n"
        "Low-level object commands:\n"
        "  %s hash <file>\n"
        "  %s add <file>\n"
        "  %s verify <sha256>\n"
        "  %s restore <sha256> <output>\n\n"
        "  %s help\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    if (argc >= 2 &&
        (strcmp(argv[1], "help") == 0 ||
         strcmp(argv[1], "--help") == 0 ||
         strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    if ((argc == 2 || argc == 3) && strcmp(argv[1], "init") == 0)
        return project_init(argc == 3 ? argv[2] : NULL) == 0 ? 0 : 1;

    if (argc == 3 && strcmp(argv[1], "hash") == 0) {
        char digest[65];

        if (sha256_file(argv[2], digest) != 0) {
            fprintf(stderr, "minidvc: failed to hash '%s'\n",
                    argv[2]);
            return 1;
        }

        puts(digest);
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "add") == 0)
        return storage_add(argv[2]) == 0 ? 0 : 1;

    if (argc == 3 && strcmp(argv[1], "verify") == 0)
        return storage_verify(argv[2]) == 0 ? 0 : 1;

    if (argc == 4 && strcmp(argv[1], "restore") == 0)
        return storage_restore(argv[2], argv[3]) == 0 ? 0 : 1;

    if (argc >= 4 && strcmp(argv[1], "snapshot") == 0)
        return manifest_snapshot(argv[2], argc - 3, &argv[3]) == 0 ? 0 : 1;

    if ((argc == 3 || argc == 4) && strcmp(argv[1], "checkout") == 0)
        return manifest_checkout(argv[2],
                                 argc == 4 ? argv[3] : NULL) == 0 ? 0 : 1;

    if ((argc == 2 || argc == 3) && strcmp(argv[1], "list") == 0) {
        if (argc == 2 || strcmp(argv[2], "--remote") == 0)
            return remote_list() == 0 ? 0 : 1;
        if (strcmp(argv[2], "--local") == 0)
            return manifest_list_local() == 0 ? 0 : 1;
        fprintf(stderr, "minidvc: unknown option for list: %s\n", argv[2]);
        print_usage(stderr, argv[0]);
        return 2;
    }

    if (argc == 4 && strcmp(argv[1], "remote") == 0 &&
        strcmp(argv[2], "set") == 0)
        return remote_set_folder(argv[3]) == 0 ? 0 : 1;

    if ((argc == 2 || (argc == 3 && strcmp(argv[2], "show") == 0)) &&
        strcmp(argv[1], "remote") == 0)
        return remote_show() == 0 ? 0 : 1;

    if ((argc == 4 || argc == 5) && strcmp(argv[1], "auth") == 0)
        return remote_auth(argv[2], argv[3],
                           argc == 5 ? argv[4] : NULL) == 0 ? 0 : 1;

    if ((argc == 2 || argc == 3) && strcmp(argv[1], "push") == 0)
        return remote_push(argc == 3 ? argv[2] : NULL) == 0 ? 0 : 1;

    if ((argc == 2 || argc == 3) && strcmp(argv[1], "pull") == 0)
        return remote_pull(argc == 3 ? argv[2] : NULL) == 0 ? 0 : 1;

    print_usage(stderr, argv[0]);
    return 2;
}
