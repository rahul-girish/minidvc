#include "hash.h"
#include "storage.h"
#include "manifest.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
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

	if (argc == 4 && strcmp(argv[1], "checkout") == 0)
    	return manifest_checkout(argv[2], argv[3]) == 0 ? 0 : 1;

    fprintf(stderr,
        "Usage:\n"
        "  %s hash <file>\n"
        "  %s add <file>\n"
        "  %s verify <sha256>\n"
		"  %s restore <sha256> <output>\n"
		"  %s snapshot <name> <file> [file ...]\n"
		"  %s checkout <name> <output-dir>\n",
        argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);

    return 2;
}
