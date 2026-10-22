#ifndef MINIDVC_MANIFEST_H
#define MINIDVC_MANIFEST_H

int manifest_snapshot(const char *name, int count, char **paths);
int manifest_checkout(const char *name, const char *output_dir);

#endif