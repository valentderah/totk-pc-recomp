// Included only in diagnostic builds. Exercise the full attachment ledger
// through the production render-pass path and verify all stored planes.
void mtl_probe_attachment_ledger(void) {
    mtl_init();
    enum { W = 16, H = 16 };
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> colors[8], depth;
    tex_t ct[8] = {0}, dt = {0};
    for (unsigned i = 0; i < 8; i++) {
        colors[i] = [g_dev newTextureWithDescriptor:td];
        if (!colors[i]) MDIE("attachment ledger probe color allocation");
        res_add(colors[i]);
        ct[i].serial = 0xfffffe10 + i;
        ct[i].d.width = W; ct[i].d.height = H; ct[i].d.format = 0x25;
        serial_put(ct[i].serial, colors[i]);
    }
    td.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    depth = [g_dev newTextureWithDescriptor:td];
    if (!depth) MDIE("attachment ledger probe depth allocation");
    res_add(depth);
    dt.serial = 0xfffffe20;
    dt.d.width = W; dt.d.height = H; dt.d.format = 0x36;
    serial_put(dt.serial, depth);
    id<MTLBuffer> cr = probe_buf(W * H * 4 * 8), zr = probe_buf(W * H * 4), sr = probe_buf(W * H);
    op_hdr end = {OP_END, 8};
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    R.has_rt = 1; R.rt.n = 8; R.rt.depth = &dt;
    for (unsigned i = 0; i < 8; i++) {
        R.rt.color[i] = &ct[i]; R.clear_c[i] = 1;
        R.cc[i] = MTLClearColorMake((i + 1) / 255.0, .5, .75, 1);
    }
    R.clear_d = R.clear_s = 1; R.cd = .5; R.cs = 0xa5;
    rp_begin();
    if (g_nsd != 10 || sizeof g_sd / sizeof *g_sd < (size_t)g_nsd)
        MDIE("attachment ledger requires eight color entries plus depth and stencil");
    rp_end();
    for (unsigned i = 0; i < 8; i++)
        enc_b_copy_texture_buffer(blit(), colors[i], 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), cr, i * W * H * 4, W * 4, W * H * 4);
    id<MTL4ComputeCommandEncoder> be = blit();
    [be copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:zr destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4 options:MTLBlitOptionDepthFromDepthStencil];
    [be copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:sr destinationOffset:0 destinationBytesPerRow:W destinationBytesPerImage:W * H options:MTLBlitOptionStencilFromDepthStencil];
    mtl_sub_commit(); mtl_finish();
    for (unsigned i = 0; i < W * H; i++) {
        for (unsigned a = 0; a < 8; a++) {
            const uint8_t *p = (const uint8_t *)cr.contents + (a * W * H + i) * 4;
            if (p[0] != a + 1 || abs(p[1] - 128) > 1 || abs(p[2] - 191) > 1 || p[3] != 255)
                MDIE("attachment ledger changed color %u pixel %u", a, i);
        }
        if (((const float *)zr.contents)[i] != .5f || ((const uint8_t *)sr.contents)[i] != 0xa5)
            MDIE("attachment ledger changed depth/stencil pixel %u", i);
    }
    memset(&R, 0, sizeof R); state_reset(); serial_del(dt.serial);
    for (unsigned i = 0; i < 8; i++) { serial_del(ct[i].serial); defer_obj_release(colors[i]); }
    for (id obj in @[depth, cr, zr, sr]) defer_obj_release(obj);
    printf("{\"probe\":\"attachment-ledger\",\"status\":\"pass\",\"checks\":%u}\n", W * H * 34);
}
