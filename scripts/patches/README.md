# SPIRV-Cross fragment output mask

`spirv-cross-fragment-output-mask.patch` targets SPIRV-Cross
`6c09849fe88c48eaed08413aa022aaa136a3a057`, the revision used by the existing
MoltenVK 1.4.2 dependency. SPIRV-Cross is Apache-2.0 licensed.

The original converter retains masked array elements in the fragment return
structure while omitting their mandatory Metal color attributes. It also
discards an entire output array if its first location is masked, even when a
later location is enabled. The patch retains the local array and its stores,
exports only enabled elements, and checks the whole variable's location range.

Validation uses `test/spirv_fragment_output_probe.cpp` built with the converter's
core, parser, parsed IR, CFG, GLSL and MSL sources. Run:

```
python3 test/spirv_fragment_output_test.py /path/to/probe /path/to/metal-checker
```

The optional Metal checker is `test/metal_source_check.m`, compiled with the
existing macOS Metal and Foundation frameworks. Seven masks cover no outputs,
the first or last output, an inactive first output, sparse and all outputs.
They passed both interface checks and actual Metal compilation after the patch;
the original converter failed Metal compilation with mask 1.

`scripts/fetch-macos-deps.sh` invokes `scripts/build-macos-moltenvk.sh` to
include the patch in the actual runtime. The builder pins the existing
MoltenVK internal components by revision and archive SHA-256, uses local
CMake targets without dependency auto-resolution, and preserves cereal
pipeline caches, SPIRV-Tools and private-API capabilities. It uses the existing
compiler, Python and CMake; full Xcode is not required. Sources and archives
are cached in `.deps/moltenvk-sources` and `.deps/moltenvk-source-archives`.

The driver's pipeline-cache identity includes the source/build/patch digest,
so caches produced by a different converter are not reused. The signed dylib
is installed at `.deps/lib/libMoltenVK.dylib`; bundling includes its licenses.
An unchanged build reuses the fingerprint stamp without rebuilding.

`make gl-fragment-output-test` now passes actual draws and pixel readback:
both legacy GLSL output forms and seven GLSL 300 array masks, including no
color outputs, an inactive first output, sparse and all outputs. UnitySampleGame
also ran 600 frames through normal startup without the former Metal compiler
or GL draw errors. These checks establish this compiler fix, not general APK
compatibility. The APK's shader source is unchanged.
