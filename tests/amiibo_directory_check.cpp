// Synthetic-file checks of the production Amiibo browser and the real C++ library's iterator.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static std::string fault_folder;
static bool fault_enabled;
static bool fault_injected;
static unsigned ordinary_delivered;
static unsigned intercepted_reads;

static bool target_directory(DIR *directory) {
    if (!fault_enabled) return false;
    int fd = dirfd(directory);
    char proc[64], path[PATH_MAX + 1];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, path, sizeof path - 1);
    if (n < 0) return false;
    path[n] = 0;
    return fault_folder == path;
}

template <class Entry, class Read>
static Entry *read_entry(DIR *directory, Read real_read) {
    bool target = target_directory(directory);
    if (target) {
        intercepted_reads++;
        if (ordinary_delivered) {
            fault_injected = true;
            errno = EIO;
            return nullptr;
        }
    }
    Entry *entry = real_read(directory);
    if (target && entry && strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ordinary_delivered++;
    return entry;
}

extern "C" struct dirent *readdir(DIR *directory) {
    using Function = struct dirent *(*)(DIR *);
    static Function real_read = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "readdir"));
    if (!real_read) abort();
    return read_entry<struct dirent>(directory, real_read);
}

extern "C" struct dirent64 *readdir64(DIR *directory) {
    using Function = struct dirent64 *(*)(DIR *);
    static Function real_read = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "readdir64"));
    if (!real_read) abort();
    return read_entry<struct dirent64>(directory, real_read);
}

#include "amiibo_functions.cpp"

static void start_fault() {
    fault_enabled = true;
    fault_injected = false;
    ordinary_delivered = intercepted_reads = 0;
}

static void collect(void *context, const char *name) {
    static_cast<std::set<std::string> *>(context)->insert(name);
}

static bool refresh_with_failure() {
    start_fault();
    bool threw = false;
    try {
        refresh_dir();
    } catch (const std::filesystem::filesystem_error &error) {
        threw = true;
        printf("refresh_dir threw: %s\n", error.what());
    }
    printf("refresh_dir: injected=%d ordinary=%u intercepted=%u exception=%d\n",
           fault_injected, ordinary_delivered, intercepted_reads, threw);
    if (!fault_injected || ordinary_delivered != 1) {
        fprintf(stderr, "fixture did not inject an error after one real entry\n");
        exit(2);
    }
    fault_enabled = false;
    return threw;
}

static bool collect_with_failure() {
    start_fault();
    bool threw = false;
    std::set<std::string> names;
    try {
        amiibo_collect_filenames(collect, &names);
    } catch (const std::filesystem::filesystem_error &error) {
        threw = true;
        printf("amiibo_collect_filenames threw: %s\n", error.what());
    }
    printf("amiibo_collect_filenames: injected=%d ordinary=%u intercepted=%u exception=%d collected=%zu\n",
           fault_injected, ordinary_delivered, intercepted_reads, threw, names.size());
    if (!fault_injected || ordinary_delivered != 1 || names.size() != 1) {
        fprintf(stderr, "fixture did not deliver one filename before the error\n");
        exit(2);
    }
    fault_enabled = false;
    return threw;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::filesystem::path directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    for (const char *name : {"first.bin", "second.bin", "third.nfc"}) {
        std::ofstream out(directory / name, std::ios::binary);
        out << "synthetic fixture only\n";
        if (!out) return 2;
    }
    fault_folder = std::filesystem::canonical(directory).string();
    g_current_dir = fault_folder;
    bool refresh_threw = refresh_with_failure();
    bool collect_threw = collect_with_failure();
    refresh_dir();
    std::set<std::string> refreshed;
    for (const auto &entry : g_entries) if (!entry.is_dir) refreshed.insert(entry.name);
    std::set<std::string> collected;
    amiibo_collect_filenames(collect, &collected);
    const std::set<std::string> expected = {"first.bin", "second.bin", "third.nfc"};
    bool retry_ok = refreshed == expected && collected == expected;
    printf("normal retry lists and collects all synthetic filenames: %s\n", retry_ok ? "yes" : "NO");
    if (!retry_ok) return 2;
    printf("amiibo directory checks: %d escaping exceptions (zero expected after a fix)\n",
           int(refresh_threw) + int(collect_threw));
    return refresh_threw || collect_threw ? 1 : 0;
}