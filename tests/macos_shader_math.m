// GPU differential check of the precise helpers emitted by SPIRV-Cross.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static NSString *helpers(const char *path) {
    NSString *source = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    NSRange start = [source rangeOfString:@"template<typename T>"], end = [source rangeOfString:@"struct "];
    if (!source || start.location == NSNotFound || end.location == NSNotFound || end.location <= start.location) {
        fprintf(stderr, "Missing generated arithmetic helpers: %s\n", path); exit(1);
    }
    return [source substringWithRange:NSMakeRange(start.location, end.location - start.location)];
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 3) return 2;
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\nnamespace reference {\n%@\n}\nnamespace candidate {\n%@\n}\n"
        "kernel void check(device const uint4 *input [[buffer(0)]], device uint4 *output [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
        "float4 x=as_type<float4>(input[i]);\n"
        "float a=reference::spvFMul(x.x,x.y), b=candidate::spvFMul(x.x,x.y);\n"
        "output[i*4]=uint4(as_type<uint>(a),as_type<uint>(b),as_type<uint>(reference::spvFAdd(a,x.z)),as_type<uint>(candidate::spvFAdd(b,x.z)));\n"
        "float4 ra=reference::spvFAdd(reference::spvFMul(x,x.yzwx),x.zwxy);\n"
        "float4 rb=candidate::spvFAdd(candidate::spvFMul(x,x.yzwx),x.zwxy);\n"
        "output[i*4+1]=as_type<uint4>(ra);output[i*4+2]=as_type<uint4>(rb);\n"
        "output[i*4+3]=uint4(as_type<uint>(fma(x.x,x.y,x.z)));\n}\n", helpers(argv[1]), helpers(argv[2])];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_2; options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> lib = [device newLibraryWithSource:source options:options error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&error];
    if (!pipeline) return 1;
    enum { N = 262144 };
    id<MTLBuffer> input = [device newBufferWithLength:N * 16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:N * 64 options:MTLResourceStorageModeShared];
    uint32_t *in = input.contents, seed = 0x12345678;
    const uint32_t edge[] = {0,0x80000000,0x3f800000,0xbf800000,0x3f800001,0x3f7fffff,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00000,1,0x80000001,0x00800000,0x80800000};
    for (unsigned i = 0; i < N; i++) for (unsigned c = 0; c < 4; c++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        in[i * 4 + c] = i < 3375 ? edge[(i / (c == 0 ? 1 : c == 1 ? 15 : 225)) % 15] : seed;
    }
    // Independent expected results: preserved negative zero and a case that
    // distinguishes separate multiplication/addition from a fused operation.
    in[0] = 0x80000000; in[1] = 0x3f800000; in[2] = 0x80000000;
    in[4] = 0x3f800001; in[5] = 0x3f7ffffe; in[6] = 0xbf800000;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
    [encoder setComputePipelineState:pipeline]; [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "%s\n", cb.error.description.UTF8String); return 1; }
    const uint32_t *out = output.contents;
    unsigned mismatch = 0, nan_payload = 0, contraction = 0;
    for (unsigned i = 0; i < N; i++) {
        const uint32_t *p = out + i * 16;
        for (unsigned c = 0; c < 6; c++) {
            uint32_t a = c < 2 ? p[c * 2] : p[4 + c - 2];
            uint32_t b = c < 2 ? p[c * 2 + 1] : p[8 + c - 2];
            if (a == b) continue;
            if ((a & 0x7fffffff) > 0x7f800000 && (b & 0x7fffffff) > 0x7f800000) nan_payload++;
            else mismatch++;
        }
        if (p[2] != p[12]) contraction++;
    }
    BOOL expected = out[0] == 0x80000000 && out[1] == 0x80000000 &&
        out[2] == 0x80000000 && out[3] == 0x80000000 && out[18] == 0 && out[19] == 0 && out[28] != 0;
    printf("{\"probe\":\"shader-math\",\"status\":\"%s\",\"checks\":%u,\"mismatches\":%u,\"nan_payload_differences\":%u,\"contraction_controls\":%u}\n",
        !mismatch && expected && contraction ? "pass" : "fail", N * 6, mismatch, nan_payload, contraction);
    return mismatch || !expected || !contraction;
}}
