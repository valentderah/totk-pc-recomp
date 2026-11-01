// TOTK Vulkan adaptation of MacNativeGames games/totk/tools/totk_xlate/Program.cs (MIT, see LICENSE; MNG commit b75be8cf).
// Provenance: that file is based on D2R Mobile dm/gfx/tools/dmsh_xlate/Program.cs. Changes here: SPIR-V for Vulkan is the final
// output (no MSL service, no VertexCounterLowering, no NEAR_IS_MINUS_W rewrite), depth clip control reported as supported,
// non-constant texture offsets reported as unsupported, --disasm dropped.
// Protocol (stdout carries replies only; diagnostics go to stderr):
//   totk_xlate --serve              one request per stdin line: {"job": {...}, "out": "<dir>"}; one reply line per request:
//                                   "ok <key>" or "fail <key> <message>"; a malformed line gets "fail ? <message>" and the loop continues.
//   totk_xlate <jobs.json> <outdir> batch mode: a JSON array of jobs, prints a summary, exit 1 if any failed.
//   job: { "key", "stage": "vs|ps|cs", "code": path (graphics = 0x50-byte SPH + instructions, compute = instructions), "c1": hex,
//          "tex": {slot: type}, "img": {slot: {type, fmt}}, "attr": {loc: float|sint|uint}, "fout": {idx: float|sint|uint},
//          "local": [x, y, z], "shared": bytes, "debug": bool, "reduced": bool, "softfloat64": bool,
//          "sbuf_align": int, "subgroup": int }
//   Output: <out>/<key>.spv and <out>/<key>.res.json (bindings, queried slots, notes).
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    const int TexBase = 0x168, SampBase = 0x568;   // NVN 分离贴图 / 采样器句柄在 c[2] 的字节基址（每槽 8 字节；dm/gfx/tools/maxwell_refs.py 全量实测） | byte base of NVN separate texture / sampler handles in c[2] (8 bytes per slot; full measurement in dm/gfx/tools/maxwell_refs.py)

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new(); public readonly SortedSet<int> TexQueried = new(), ImgQueried = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); }
        // c1 常数段（着色器包控制段前 0x100 字节）：间接跳转 BRX 的跳转表在这里（LDC c[1]）；不提供时翻译器解不出跳转目标、控制流出错（粒子着色器死循环）
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 读越界 +{offset:X}（c1 长 {_c1.Length}）"); return 0; }
        // 全部资源放进第 0 组，各占不同绑定号（spirv-cross 出成一个参数缓冲）；常量段号 index 的缓冲由翻译器给 binding = index+1 的惯例不依赖
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) TexQueried.Add(slot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (t == null) { Notes.Add($"贴图句柄 {handle:X}（常量段 {cbufSlot}，槽 {slot}）无 PC 类型，按 2D"); return SamplerType.Texture2D; }
            if (t.EndsWith("_depth")) t = t.Substring(0, t.Length - 6);   // depth-format texture: same sampler type here; the host binds a depth view
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) ImgQueried.Add(slot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"图像句柄 {handle:X} 槽 {slot} 无 PC 格式，按 R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // 配对 PC 的 groupshared 声明之和 | sum of the paired PC shader's groupshared declarations
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            // Vulkan: VK_EXT_depth_clip_control handles NEAR_IS_MINUS_W, so the shader is never rewritten ("depth_minus_w" in a job is ignored).
            const bool depthMinusW = false;
            // "early_fragment_tests": the fragment shader's NVN control byte +0x718 (the driver's early-Z-force, Ryujinx's
            // EarlyZForce); the SPIR-V then declares EarlyFragmentTests (MacNativeGames ae8f4f4).
            bool earlyZ = _job["early_fragment_tests"] is JsonValue ez && ez.TryGetValue<bool>(out bool eb) && eb;
            return new GpuGraphicsState(earlyZ, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, depthMinusW, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* 不做 Y 取反 | no Y flip */, true /* 原点左上 | origin upper-left */, false);
        }
        public bool QueryHostSupportsDepthClipControl() => true;
        // Vulkan: the ConstOffset / ConstOffsets image operands Ryujinx emits must be constants. A TLD4.PTP whose offsets
        // come from registers (agl_technique_pfx compute b940aa86, dfe7662f) otherwise becomes an OpConstantComposite over
        // computed ids, which spirv-val rejects; false makes TexturePass emulate only non-constant offsets.
        public bool QueryHostSupportsNonConstantTextureOffset() => false;
        // "softfloat64": true lowers FP64 operations to FP32 (Ryujinx's DoubleToFloat pass), for a host without shaderFloat64.
        public bool QueryHostSupportsShaderFloat64() => _job["softfloat64"] is not JsonValue v || !(v.TryGetValue<bool>(out bool b) && b);
        public bool QueryHostSupportsTextureShadowLod() => true;
        // Sub-texel precision bits of a host whose gathers round differently from Maxwell (AMD, Intel): Ryujinx then
        // biases gather coordinates so the same texels are picked (TexturePass.InsertCoordGatherBias). 0: no bias.
        public int QueryHostGatherBiasPrecision() => _job["gather_bias"] is JsonNode n ? (int)n : 0;
        // 作业 reduced = true：放宽精度——翻译器只给强制精确的运算打 NoContraction（缺省全部打：保持 Maxwell 分开舍入的逐位结果，代价是 MSL 里大量禁优化的 spvFMul 等）
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // Host answers for GPUs other than the RX 9070 (runtime/vk_caps.c); absent from the job they keep Ryujinx's defaults.
        // "sbuf_align": n: the host binds each storage buffer at its guest address rounded down to n (minStorageBufferOffsetAlignment
        // above 4), so storage offsets count from there (Ryujinx's unaligned path; the multi-target path uses n too).
        public bool QueryHasUnalignedStorageBuffer() => _job["sbuf_align"] is JsonNode;
        public int QueryHostStorageBufferOffsetAlignment() => _job["sbuf_align"] is JsonNode n ? (int)n : 16;
        // "subgroup": n: lanes per subgroup when the host cannot pin 32 (AMD GCN/Vega run 64): ballots and votes span n lanes.
        public int QueryHostSubgroupSize() => _job["subgroup"] is JsonNode n ? (int)n : 32;
    }


    internal static string Translate(JsonObject job, string outDir)   // also called by the --service mode (Service.cs)
    {
        string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                var acc = new Accessor(code, job);
                // 作业 debug = false：关 DebugMode（DebugMode 会跳过 AST 优化器，产物满是临时变量；不改语义）
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0);
                var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                ShaderProgram p = ctx.Translate();
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                    ["tex_queried"] = new JsonArray(acc.TexQueried.Select(x => (JsonNode)x).ToArray()), ["img_queried"] = new JsonArray(acc.ImgQueried.Select(x => (JsonNode)x).ToArray()),
                };
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add((JsonNode)new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                return null;
            }
            catch (Exception e) { if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); return $"{e.GetType().Name}: {e.Message.Split('\n')[0]}"; }
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--serve")
        {
            // One request per line: {"job": {...}, "out": "<dir>"}; one reply line "ok <key>" / "fail <key> <message>".
            string line;
            while ((line = Console.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                string key = "?", err;
                try
                {
                    var req = JsonNode.Parse(line).AsObject(); var job = req["job"].AsObject(); string o = (string)req["out"];
                    key = (string)job["key"];
                    Directory.CreateDirectory(o);
                    err = Translate(job, o);
                }
                catch (Exception e) { err = $"{e.GetType().Name}: {e.Message.Split((char)10)[0]}"; }
                Console.WriteLine(err == null ? $"ok {key}" : $"fail {key} {err}");
                Console.Out.Flush();
            }
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string err = Translate(job, outDir);
            if (err == null) ok++; else { bad++; Console.Error.WriteLine($"【红】{(string)job["key"]}：{err}"); }
        }
        Console.WriteLine($"翻译 {ok} 成功，{bad} 失败");
        return bad == 0 ? 0 : 1;
    }
}
