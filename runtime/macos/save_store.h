// APFS transaction storage. Callers exclude writes while committing a snapshot.
#pragma once
#include <limits.h>

typedef struct {
    char committed[PATH_MAX], root[PATH_MAX], work[PATH_MAX], stage[PATH_MAX], previous[PATH_MAX];
} NativeSaveStore;

int native_save_open(NativeSaveStore *store, const char *committed, const char *transaction_root);
int native_save_commit(NativeSaveStore *store);
