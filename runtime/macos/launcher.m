// Finder entry point. The signed bundle is read-only throughout a play session.
#import <AppKit/AppKit.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "native_shader_cache.h"

static int error(NSString *text) {
    fprintf(stderr, "%s\n", text.UTF8String);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"TotK Recomp";
    alert.informativeText = text;
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
    return 1;
}

static BOOL directory(NSString *path) {
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSBundle *bundle = NSBundle.mainBundle;
        NSString *root = [bundle.resourcePath stringByAppendingPathComponent:@"totk"];
        NSString *binary = [bundle.executablePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"totk-native"];
        NSData *data = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"build-info.json"]];
        NSDictionary *info = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (!info || access(binary.fileSystemRepresentation, X_OK)) {
            if (argc > 1) return 1;
            return error(@"The application is incomplete. Build or install it again.");
        }
        BOOL probes = [info[@"probe_only"] boolValue];
        NSString *version = info[@"version"];
        if (![version isEqual:@"v143"] && ![version isEqual:@"v121"]) return 1;
        if (argc == 2 && !strcmp(argv[1], "--check")) {
            printf("{\"native_app\":true,\"probe_only\":%s,\"version\":\"%s\"}\n", probes ? "true" : "false", version.UTF8String);
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "--selftest")) {
            execl(binary.fileSystemRepresentation, binary.fileSystemRepresentation, "--selftest", NULL);
            return 1;
        }
        if (probes) return error(@"The native ARM64 / Metal runtime is installed. This diagnostic build contains no game data. Prepare your game files and build the playable application.");
        NSString *support = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/TotKRecomp"];
        NSString *profile = [support stringByAppendingPathComponent:version];
        NSString *logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/TotKRecomp"];
        NSString *cache = native_shader_cache_path(root, [[NSHomeDirectory()
            stringByAppendingPathComponent:@"Library/Caches/TotKRecomp"] stringByAppendingPathComponent:version]);
        if (!cache) return error(@"Cannot identify the shader translator or macOS graphics cache.");
        NSString *slot = [support stringByAppendingPathComponent:@"game-slot"];
        if (!directory(profile) || !directory(logs) || !directory(cache) || !directory(slot))
            return error(@"Cannot create the saves, logs or shader cache folders.");
        // Keep the descriptor across exec so a second launch cannot rotate the
        // active process's log or open the same save transaction concurrently.
        int player_lock = open([[support stringByAppendingPathComponent:@"player.lock"] fileSystemRepresentation], O_CREAT | O_RDWR, 0600);
        if (player_lock < 0 || flock(player_lock, LOCK_EX | LOCK_NB))
            return error(@"TotK Recomp is already running, or its profile is unavailable.");
        const struct { const char *name; NSString *value; } settings[] = {
            {"TOTK_RESOURCE_ROOT", root}, {"DM_MSL_DIR", cache},
            {"TOTK_PROFILE_DIR", profile}, {"TOTK_SHADER_CACHE", cache}, {"TOTK_GAME_SLOT_DIR", slot},
            {"TOTK_ROMFS_PROOF_DIR", profile}, {"TOTK_SHARED_FONTS", @"extracted/firmware/shared-fonts"},
            {"TOTK_SHADER_CONTRIB", [cache stringByAppendingPathComponent:@"shader-list-contribution.txt"]},
            {"TOTK_CONTROLS", [support stringByAppendingPathComponent:@"controls.txt"]},
        };
        for (unsigned i=0; i<sizeof settings/sizeof *settings; ++i) setenv(settings[i].name, settings[i].value.fileSystemRepresentation, 1);
        setenv("TOTK_SLOT_OWNER", "user", 1);
        setenv("TOTK_SLOT_PURPOSE", "player", 1);
        setenv("TOTK_GPU_WIDEN", "1", 1);
        setenv("TOTK_GPU_BLOCKRT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1);
        NSString *locale = NSLocale.preferredLanguages.firstObject;
        if (locale) setenv("TOTK_LOCALE", locale.UTF8String, 0);
        if (chdir(root.fileSystemRepresentation)) return error(@"Cannot open the game's resource folder.");
        NSString *log = [logs stringByAppendingPathComponent:@"totk.log"];
        NSString *previous = [logs stringByAppendingPathComponent:@"totk.previous.log"];
        [NSFileManager.defaultManager removeItemAtPath:previous error:nil];
        [NSFileManager.defaultManager moveItemAtPath:log toPath:previous error:nil];
        FILE *file = fopen(log.fileSystemRepresentation, "w");
        if (!file) return error(@"Cannot create the game log.");
        dup2(fileno(file), STDOUT_FILENO); dup2(fileno(file), STDERR_FILENO); fclose(file);
        NSString *modules = [@"rewritten" stringByAppendingPathComponent:version];
        execl(binary.fileSystemRepresentation, binary.fileSystemRepresentation, modules.fileSystemRepresentation, "boot", NULL);
        return error([NSString stringWithFormat:@"Cannot start the game: %s", strerror(errno)]);
    }
}
