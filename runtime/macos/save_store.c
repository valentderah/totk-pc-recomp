// A save is published with one atomic directory exchange. Interrupted writes
// remain in work; the previously committed generation stays independently valid.
#include "save_store.h"
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int path_join(char out[PATH_MAX], const char *base, const char *name) {
    if (snprintf(out, PATH_MAX, "%s/%s", base, name) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int fault(const char *step) {
    const char *crash = getenv("TOTK_SAVE_CRASH_AT");
    if (crash && !strcmp(crash, step)) _exit(86);
    const char *fail = getenv("TOTK_SAVE_FAIL_AT");
    if (fail && !strcmp(fail, step)) { errno = ENOSPC; return -1; }
    return 0;
}

static int sync_path(const char *path, int directory) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
    if (fd < 0) return -1;
    // The final publication barrier flushes the drive's entire queue, including
    // every file synced here. Draining that queue once per file stalls saves.
    int r = fsync(fd);
    int error = errno;
    close(fd);
    errno = error;
    return r;
}

static int publication_barrier(const char *root) {
    int fd=open(root,O_RDONLY|O_NOFOLLOW|O_DIRECTORY);
    if(fd<0)return -1;
    int result=fcntl(fd,F_FULLFSYNC), error=errno;
    close(fd);errno=error;
    return result;
}

static int remove_tree(const char *path) {
    struct stat root;
    if (lstat(path, &root)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(root.st_mode)) return unlink(path);
    DIR *dir = opendir(path);
    if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    int result = 0;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX];
        struct stat st;
        if (path_join(child, path, entry->d_name) || lstat(child, &st)) { result = -1; break; }
        result = S_ISDIR(st.st_mode) ? remove_tree(child) : unlink(child);
    }
    int error = errno;
    closedir(dir);
    if (!result) return rmdir(path);
    errno = error;
    return result;
}

// A clean launch retains only committed + previous. Interrupted changes are
// preserved separately; comparing bytes also catches writes made after Commit
// through an already-open descriptor, without trusting a stale dirty flag.
static int same_tree(const char *a, const char *b) {
    struct stat sa, sb;
    if (lstat(a, &sa) || lstat(b, &sb)) return 0;
    if (S_ISREG(sa.st_mode) && S_ISREG(sb.st_mode)) {
        if (sa.st_size != sb.st_size) return 0;
        int fa=open(a,O_RDONLY|O_NOFOLLOW), fb=open(b,O_RDONLY|O_NOFOLLOW), equal=1;
        if (fa<0 || fb<0) equal=0;
        char ca[65536], cb[65536];
        while(equal) {
            ssize_t na=read(fa,ca,sizeof ca), nb=read(fb,cb,sizeof cb);
            if(na<0 || nb!=na || (na && memcmp(ca,cb,(size_t)na))) {equal=0;break;}
            if(!na)break;
        }
        if(fa>=0)close(fa);if(fb>=0)close(fb);
        return equal;
    }
    if(!S_ISDIR(sa.st_mode)||!S_ISDIR(sb.st_mode))return 0;
    DIR *da=opendir(a), *db=opendir(b);
    int equal=da&&db, na=0, nb=0;
    struct dirent *e;
    while(equal) {
        errno=0;e=readdir(da);if(!e){if(errno)equal=0;break;}
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char pa[PATH_MAX],pb[PATH_MAX];na++;
        if(path_join(pa,a,e->d_name)||path_join(pb,b,e->d_name)||!same_tree(pa,pb))equal=0;
    }
    while(equal) {
        errno=0;e=readdir(db);if(!e){if(errno)equal=0;break;}
        if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))nb++;
    }
    if(da)closedir(da);if(db)closedir(db);
    return equal && na==nb;
}

static int snapshot(const char *source, const char *destination, int durable) {
    if (mkdir(destination, 0700)) return -1;
    DIR *dir = opendir(source);
    if (!dir) return -1;
    struct dirent *entry;
    int result = 0;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char from[PATH_MAX], to[PATH_MAX];
        struct stat st;
        if (path_join(from, source, entry->d_name) || path_join(to, destination, entry->d_name) || lstat(from, &st)) {
            result = -1; break;
        }
        if (S_ISDIR(st.st_mode)) result = snapshot(from, to, durable);
        else if (S_ISREG(st.st_mode)) {
            // COPYFILE_CLONE uses copy-on-write on APFS and falls back to a copy.
            result = copyfile(from, to, NULL, COPYFILE_DATA | COPYFILE_CLONE | COPYFILE_EXCL | COPYFILE_NOFOLLOW);
            if (!result && durable) result = sync_path(to, 0);
        } else { errno = EINVAL; result = -1; }
        if (result) break;
        errno = 0;
    }
    if (!result && errno) result = -1;
    int error = errno;
    closedir(dir);
    errno = error;
    return result ? result : durable ? sync_path(destination, 1) : 0;
}

static int preserve_work(NativeSaveStore *store) {
    struct stat st;
    if (lstat(store->work, &st)) return errno == ENOENT ? 0 : -1;
    if (same_tree(store->work, store->committed)) return remove_tree(store->work);
    char recovery[PATH_MAX];
    if (path_join(recovery, store->root, "recovery-XXXXXX") || !mkdtemp(recovery)) return -1;
    char path[PATH_MAX];
    if (path_join(path, recovery, "work") || rename(store->work, path)) return -1;
    return sync_path(store->root, 1);
}

int native_save_open(NativeSaveStore *store, const char *committed, const char *root) {
    if (strlen(committed) >= PATH_MAX || strlen(root) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    memset(store, 0, sizeof *store);
    strcpy(store->committed, committed);
    strcpy(store->root, root);
    if (path_join(store->work, root, "work") || path_join(store->stage, root, "stage") ||
        path_join(store->previous, root, "previous")) return -1;
    if (mkdir(root, 0700) && errno != EEXIST) return -1;
    if (mkdir(committed, 0700) && errno != EEXIST) return -1;
    struct stat a,b;
    if(lstat(root,&a)||lstat(committed,&b))return -1;
    if(!S_ISDIR(a.st_mode)||!S_ISDIR(b.st_mode)){errno=EINVAL;return -1;}
    if (preserve_work(store) || remove_tree(store->stage)) return -1;
    return snapshot(store->committed, store->work, 0);
}

int native_save_commit(NativeSaveStore *store) {
    if (fault("copy") || remove_tree(store->stage) || snapshot(store->work, store->stage, 1) ||
        fault("flush") || sync_path(store->root, 1) || fault("publish")) return -1;
    if (renameatx_np(AT_FDCWD, store->stage, AT_FDCWD, store->committed, RENAME_SWAP)) return -1;
    // After the exchange either complete generation may survive a power loss
    // until both parent directories are durable. Never acknowledge before that.
    if (fault("published")) return -1;
    char parent[PATH_MAX];
    strcpy(parent, store->committed);
    char *slash = strrchr(parent, '/');
    if (!slash) { errno = EINVAL; return -1; }
    *slash = 0;
    if (sync_path(parent, 1) || sync_path(store->root, 1) || fault("barrier") || publication_barrier(store->root)) return -1;
    // Backup rotation is after durability; a cleanup failure cannot undo Commit.
    if (!remove_tree(store->previous)) {
        if (!rename(store->stage, store->previous)) (void)sync_path(store->root, 1);
    }
    return 0;
}
