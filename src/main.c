#include "hash.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
	if(argc != 3 || strcmp(argv[1], "hash") != 0)
	{
		fprintf(stderr, "Usage: %s hash <file>\n", argv[0]);
		return 2;
	}		

	char digest[65];
	
	if(sha256_file(argv[2], digest) != 0)
	{
		fprintf(stderr, "minidvc: failed to hash '%s'\n", argv[2]);
		return 1;
	}

	printf("%s", digest);
	return 0;
}
