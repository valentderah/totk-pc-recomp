// Diagnostic-only GPU coverage for attachment reads across pass boundaries.
static unsigned feedback_pixels(id<MTLBuffer> bytes, unsigned pixels, const uint8_t want[4], const char *name) {
    const uint8_t *p = bytes.contents;
    for (unsigned i = 0; i < pixels * 4; i++)
        if (abs((int)p[i] - want[i % 4]) > 1)
            MDIE("attachment feedback %s byte %u: got %u, expected %u", name, i, p[i], want[i % 4]);
    return pixels * 4;
}

static void feedback_clear(id<MTLTexture> texture, NSUInteger level, NSUInteger slice, MTLClearColor color) {
    blit_close();
    MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
    pd.colorAttachments[0].texture = texture;
    pd.colorAttachments[0].level = level;
    pd.colorAttachments[0].slice = slice;
    pd.colorAttachments[0].loadAction = MTLLoadActionClear;
    pd.colorAttachments[0].storeAction = MTLStoreActionStore;
    pd.colorAttachments[0].clearColor = color;
    enc_r_end(enc_r_begin(pd));
}

void mtl_probe_attachment_feedback(void) {
    mtl_init();
    enum { W = 8, H = 8 };
    unsigned checks = 0;
    NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A { texture2d<float,access::read> t [[id(0)]]; };\n"
        "vertex float4 vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
        "fragment float4 sample_rt(float4 p [[position]],constant A &a [[buffer(0)]]) { return a.t.read(uint2(p.xy))+float4(.125,0,0,0); }\n"
        "fragment float4 plain() { return float4(.5,.25,.5,1); }\n"
        "struct Views { texture2d<float,access::read> a [[id(0)]], b [[id(1)]]; };\n"
        "kernel void compare_views(constant Views &v [[buffer(0)]],device float4 *out [[buffer(1)]],uint2 p [[thread_position_in_grid]]) { out[p.y*4+p.x]=v.a.read(p)-v.b.read(p); }\n"
        "struct Writable { texture2d<float,access::write> t [[id(0)]]; };\n"
        "kernel void write_image(constant Writable &a [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { a.t.write(float4(.375,.25,.5,1),p); }\n";
    id<MTLLibrary> lib = totk_msl_library(source, &error);
    if (!lib) MDIE("attachment feedback shader: %s", error.localizedDescription.UTF8String);
    NvShader *vs = [NvShader new]; vs->fn = [lib newFunctionWithName:@"vs"]; vs->src = source;
    vs->io = @{}; strcpy(vs->key, "probe-feedback-vs");
    NvShader *fs[3]; NvProg *program[3]; prog_t pg[3] = {0};
    for (int i = 0; i < 3; i++) {
        fs[i] = [NvShader new]; fs[i]->stage = 1; fs[i]->src = source; fs[i]->io = @{};
        fs[i]->fn = [lib newFunctionWithName:i == 1 ? @"plain" : @"sample_rt"];
        snprintf(fs[i]->key, sizeof fs[i]->key, "probe-feedback-fs-%d", i);
        fs[i]->rf.color_mask = 1;
        if (i != 1) {
            fs[i]->rf.nb = 1;
            fs[i]->rf.b[0] = (nvn_rbind){.kind=i == 2 ? NVN_RB_IMG : NVN_RB_TEX, .dtt=MTLTextureType2D};
            fs[i]->ae = [fs[i]->fn newArgumentEncoderWithBufferIndex:0];
            fs[i]->arg_len = fs[i]->ae.encodedLength;
        }
        program[i] = [NvProg new]; program[i]->sh[0] = vs; program[i]->sh[1] = fs[i];
        pg[i].serial = 0xfffffd00 + i; serial_put(pg[i].serial, program[i]);
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:td];
    if (!target) MDIE("attachment feedback texture allocation"); res_add(target);
    id<MTLBuffer> rb = probe_buf(W * H * 4 * 4), storage = probe_buf(16384);
    tex_t rt = {.serial=0xfffffd10, .mem=storage.contents};
    rt.d = (nvn_texdesc){.width=W,.height=H,.depth=1,.levels=1,.format=0x25,.target=1,
        .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
    rt.L.size = 16384;
    tex_t alias = rt; alias.serial++;
    serial_put(rt.serial, target); serial_put(alias.serial, target);
    mtl_reg_tex(71, &rt, NULL); mtl_reg_tex(72, &alias, NULL);
    const char *names[] = {"pending clear", "barrier", "barrier before plain draw", "storage alias", "read-only image", "reuse without barrier"};
    for (int scenario = 0; scenario < 6; scenario++) {
        mtl_sub_begin(); op_hdr end = {OP_END,8}; mtl_exec((const uint8_t *)&end);
        uint8_t ops[4096], *w = lp_rt(ops, &rt, 0, 0);
        w = lp_sc(w, 0, 0, W, H); w = lp_clear(w, .125, .25, .5, 1);
        p_bind_handles binding = {.stage=1,.n=1,.h={scenario == 3 ? 72 : 71}};
        w = lp_rec(w, scenario == 4 ? OP_BIND_IMG : OP_BIND_TEX, &binding, sizeof binding);
        w = lp_prog(w, &pg[scenario == 4 ? 2 : 0]); w = lp_draw(w, 0);
        if (scenario == 1 || scenario == 2) {
            int32_t barrier = 0x16; w = lp_rec(w, OP_BARRIER, &barrier, sizeof barrier);
            if (scenario == 2) { w = lp_prog(w, &pg[1]); w = lp_draw(w, 0); w = lp_prog(w, &pg[0]); }
            w = lp_draw(w, 0);
        } else if (scenario == 5) w = lp_draw(w, 0);
        lp_end(w); mtl_exec(ops); rp_end();
        enc_b_copy_texture_buffer(blit(), target, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), rb, 0, W*4, W*H*4);
        mtl_sub_commit(); mtl_finish();
        uint8_t expected[4] = {scenario == 1 ? 96 : scenario == 2 ? 159 : 64,64,128,255};
        checks += feedback_pixels(rb, W*H, expected, names[scenario]);
    }
    // Views with the same starting slice may expose different layer counts.
    td.textureType = MTLTextureType2DArray; td.arrayLength = 4; td.mipmapLevelCount = 2;
    id<MTLTexture> array = [g_dev newTextureWithDescriptor:td]; res_add(array);
    tex_t ar = rt; ar.serial = 0xfffffd20; serial_put(ar.serial, array);
    mtl_sub_begin();
    for (NSUInteger level = 0; level < 2; level++) for (NSUInteger layer = 0; layer < 4; layer++)
        feedback_clear(array, level, layer, MTLClearColorMake((layer+level+1)/8.0,.25,.5,1));
    id<MTLTexture> snap = rtsnap_copy(0, &ar);
    id<MTLTexture> one = [array newTextureViewWithPixelFormat:array.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(1,1) slices:NSMakeRange(1,1)];
    id<MTLTexture> three = [array newTextureViewWithPixelFormat:array.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(1,1) slices:NSMakeRange(1,3)];
    id<MTLTexture> sv1 = rtsnap_view(snap, one), sv3 = rtsnap_view(snap, three);
    if (sv1.arrayLength != 1 || sv3.arrayLength != 3) MDIE("attachment feedback view layer count"); checks += 2;
    enc_b_copy_texture_buffer(blit(), sv3, 2, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), rb, 0, 16, 64);
    mtl_sub_commit(); mtl_finish();
    checks += feedback_pixels(rb, 16, (uint8_t[]){159,64,128,255}, "array mip and slice");
    // Storage sharing can create a format view, followed by a ranged/swizzled view.
    id<MTLTexture> parent = [array newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB textureType:MTLTextureType2DArray levels:NSMakeRange(1,1) slices:NSMakeRange(1,3)];
    id<MTLTexture> nested = [parent newTextureViewWithPixelFormat:parent.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0,1) slices:NSMakeRange(1,1) swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue,MTLTextureSwizzleGreen,MTLTextureSwizzleRed,MTLTextureSwizzleOne)];
    if (!parent || !nested) MDIE("attachment feedback nested view allocation");
    g_pass_rtsnap_mask = 1; g_pass_rtsnap_ser[0] = ar.serial;
    g_pass_rtsnap_base[0] = (__bridge void *)array; g_pass_rtsnap_tex[0] = snap;
    id<MTLTexture> redirected = rtsnap_bound(ar.serial + 1, nested);
    if (redirected == nested || rtsnap_root(redirected, NULL, NULL) != snap)
        MDIE("attachment feedback nested alias was not redirected");
    g_pass_rtsnap_mask = 0; checks++;
    id<MTLComputePipelineState> compare = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"compare_views"] error:&error];
    if (!compare) MDIE("attachment feedback view comparison: %s", error.localizedDescription.UTF8String);
    res_add(nested); res_add(parent);
    mtl_sub_begin();
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); enc_c_pso(ce, compare);
    id<MTLBuffer> arguments; size_t ao = arena_alloc(16, &arguments);
    uint64_t *ids = (uint64_t *)((uint8_t *)arguments.contents + ao);
    ids[0] = nested.gpuResourceID._impl; ids[1] = redirected.gpuResourceID._impl;
    enc_c_args(ce, arguments.gpuAddress + ao, 0); enc_c_args(ce, rb.gpuAddress, 1);
    enc_c_dispatch(ce, MTLSizeMake(1,1,1), MTLSizeMake(4,4,1)); enc_c_end(ce);
    mtl_sub_commit(); mtl_finish();
    for (int i = 0; i < 4*4*4; i++) {
        if (((float *)rb.contents)[i] != 0) MDIE("attachment feedback nested view changed pixel component %d", i);
        checks++;
    }
    // MSAA copies must carry stored samples, not leave the snapshot uninitialized.
    td.textureType = MTLTextureType2DMultisample; td.arrayLength = td.mipmapLevelCount = 1; td.sampleCount = 4;
    id<MTLTexture> msaa = [g_dev newTextureWithDescriptor:td];
    if (!msaa) MDIE("attachment feedback MSAA allocation"); res_add(msaa);
    tex_t mr = rt; mr.serial = 0xfffffd30; serial_put(mr.serial, msaa);
    mtl_sub_begin(); feedback_clear(msaa, 0, 0, MTLClearColorMake(.75,.25,.5,1));
    id<MTLTexture> ms = rtsnap_copy(0, &mr);
    feedback_clear(msaa, 0, 0, MTLClearColorMake(0,0,0,0));
    [blit() copyFromTexture:ms toTexture:msaa]; blit_close();
    MTL4RenderPassDescriptor *resolve = [MTL4RenderPassDescriptor new];
    resolve.colorAttachments[0].texture = msaa;
    resolve.colorAttachments[0].resolveTexture = target;
    resolve.colorAttachments[0].loadAction = MTLLoadActionLoad;
    resolve.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    enc_r_end(enc_r_begin(resolve));
    enc_b_copy_texture_buffer(blit(), target, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), rb, 0, W*4, W*H*4);
    mtl_sub_commit(); mtl_finish();
    checks += feedback_pixels(rb, W*H, (uint8_t[]){191,64,128,255}, "MSAA samples");
    // Writable images keep their original storage even when a read snapshot exists.
    td.textureType = MTLTextureType2D; td.sampleCount = 1;
    td.usage |= MTLTextureUsageShaderWrite;
    id<MTLTexture> image = [g_dev newTextureWithDescriptor:td]; res_add(image);
    tex_t it = rt; it.serial = 0xfffffd40; serial_put(it.serial, image); mtl_reg_tex(73, &it, NULL);
    NvShader *cs = [NvShader new]; cs->fn = [lib newFunctionWithName:@"write_image"]; cs->stage = 5;
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG,.rw=1,.dtt=MTLTextureType2D};
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0]; cs->arg_len = cs->ae.encodedLength;
    strcpy(cs->key, "probe-feedback-writer");
    id<MTLComputePipelineState> writer = [g_dev newComputePipelineStateWithFunction:cs->fn error:&error];
    if (!writer) MDIE("attachment feedback image writer: %s", error.localizedDescription.UTF8String);
    mtl_sub_begin(); feedback_clear(image, 0, 0, MTLClearColorMake(0,0,0,0));
    p_bind_handles image_binding = {.stage=5,.n=1,.h={73}}; state_op(OP_BIND_IMG, &image_binding);
    R.rt.n = 1; R.rt.color[0] = &it; R.rt.depth = NULL;
    NvProg *writes = [NvProg new]; writes->sh[1] = cs;
    if (draw_rt_access(writes) != (1u << 16)) MDIE("attachment feedback writer requested an unused snapshot"); checks++;
    g_pass_rtsnap_tex[0] = rtsnap_copy(0, &it); g_pass_rtsnap_base[0] = (__bridge void *)image;
    g_pass_rtsnap_ser[0] = it.serial; g_pass_rtsnap_mask = 1;
    ce = enc_c_begin(NULL); enc_c_pso(ce, writer); encode_args((id<MTLCommandEncoder>)ce, cs, 5, 0);
    enc_c_dispatch(ce, MTLSizeMake(1,1,1), MTLSizeMake(W,H,1)); enc_c_end(ce); g_pass_rtsnap_mask = 0;
    enc_b_copy_texture_buffer(blit(), image, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), rb, 0, W*4, W*H*4);
    mtl_sub_commit(); mtl_finish();
    checks += feedback_pixels(rb, W*H, (uint8_t[]){96,64,128,255}, "image writes");
    memset(&R, 0, sizeof R); state_reset();
    mtl_reg_tex(71, NULL, NULL); mtl_reg_tex(72, NULL, NULL); mtl_reg_tex(73, NULL, NULL);
    serial_del(rt.serial); serial_del(alias.serial); serial_del(ar.serial); serial_del(mr.serial); serial_del(it.serial);
    for (int i = 0; i < 3; i++) serial_del(pg[i].serial);
    for (id obj in @[target,array,parent,nested,msaa,image,rb,storage]) defer_obj_release(obj);
    printf("{\"probe\":\"attachment-feedback\",\"status\":\"pass\",\"checks\":%u}\n", checks);
}
