#include "hash.h"

#include <openssl/evp.h>
#include <stdio.h>

#define BUFFER_SIZE (1024 * 1024)

int sha256_file(const char* path, char output[65])
{
	unsigned char buffer[BUFFER_SIZE];
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;

	FILE *file = fopen(path, "rb");

	if(file == NULL)
	{
		perror("Failed to open file");
		return -1;
	}

	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if(ctx == NULL)
	{
		fclose(file);
		return -1;
	}

	int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;

	while(ok)
	{
		size_t n = fread(buffer, 1, sizeof(buffer), file);

		if(n > 0 && EVP_DigestUpdate(ctx, buffer, n) != 1)
		{
			ok = 0;
			break;
		}

		if(n < sizeof(buffer))
		{
			if(ferror(file))
				ok = 0;

			break;
		}
	}

	if(ok)
	{
		ok = EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
	}

	EVP_MD_CTX_free(ctx);
	fclose(file);

	if(!ok || digest_len != 32)
		return -1;

	for(unsigned int i = 0; i < digest_len; i++)
	{
		snprintf(output + i * 2, 3, "%02x", digest[i]);
	}

	output[64] = '\0';

	return 0;
}
