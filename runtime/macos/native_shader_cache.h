// Runtime translations and pipeline manifests belong to one toolchain/driver.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <sys/sysctl.h>

static NSString *native_shader_cache_resolve(NSString *base, NSString *identity, NSString *os, uint64_t gpu) {
    NSCharacterSet *hex = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"];
    if (identity.length != 64 || [identity rangeOfCharacterFromSet:hex.invertedSet].location != NSNotFound || !os.length)
        return nil;
    NSString *generation = [NSString stringWithFormat:@"%@-%@-%llx", identity, os, (unsigned long long)gpu];
    return [base.lastPathComponent isEqual:generation] ? base : [base stringByAppendingPathComponent:generation];
}

static NSString *native_shader_toolchain_identity(NSString *root) {
    return [[NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"build/shader-service/totk_xlate.id"]
        encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

static NSString *native_shader_cache_path(NSString *root, NSString *base) {
    NSString *identity = native_shader_toolchain_identity(root);
    char os[64] = {0}; size_t size = sizeof os;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (sysctlbyname("kern.osversion", os, &size, NULL, 0) || !device) return nil;
    return native_shader_cache_resolve(base, identity, @(os), device.registryID);
}
