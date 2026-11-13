# Graphics pipeline (Metal guest -> GLES host)

How an iOS Metal app's drawing becomes pixels on the Android surface, and where each piece lives.
This is the map to read before touching `src/gfx/` or the Metal half of `src/objc/`.

Related docs: [`environment-variables.md`](environment-variables.md)
(all runtime switches), [`rendering-investigation.md`](rendering-investigation.md) (measured title profiles,
diagnostic recipes, open questions).

---

## 1. Layer stack

```
 guest (arm64 iOS binary, IL2CPP Unity or native)
   |  ObjC selectors: +[MTLDevice newTextureWithDescriptor:] , -[MTLRenderCommandEncoder drawIndexedPrimitives:...]
   v
 src/objc/fake_foundation.hpp      fake Metal objects (kind 3 = device/resource, kind 5 = descriptor/bag)
 src/objc/fake_uikit.hpp           CAMetalLayer / CAMetalDrawable / CAMetalLayer.nextDrawable / UIScreen / UIView
   |  property bag + "#gl" handle
   v
 src/gfx/mtl_gles.hpp              Metal semantics -> GLES 3.2: textures, passes, pipelines, draws, blits
 src/gfx/msl_glsl.cpp              Metal Shading Language -> GLSL ES 3.20 (per MTLFunction entry)
 src/gfx/gles_backend.cpp          EGL context (root context shared by every guest thread), pbuffer, readback
   v
 GLES 3.2 driver -> SurfaceFlinger (app host) or PNG dumps (CLI host)
```

The CLI host (`build_cli.sh` -> `xnuroid_loader`) and the Android app host (`build_apk.sh` -> `libxnuroid.so`)
compile the same sources; only `src/app/` and the platform entry differ.

## 2. Object model

Every guest ObjC object is a `FakeObjcObj` with:

| Field | Meaning |
| :--- | :--- |
| `s` | class name for fakes (`mtl`, `CAMetalLayer`, ...) or, for a Metal **function**, the MTLFunction name |
| `props` | string-keyed `uint64_t` property bag. `foo` = last integer arg, `foo#d` = last double arg, `#gl` = host handle |
| `mem`, `len` | host allocation backing an `MTLBuffer` (guest writes land here directly) |
| `values`, `items`, `handlers` | child handles (arrays, containment, completion blocks) |
| `rect[]` | UIView/CALayer frame/bounds in points |

`gl_obj<T>(o)` reads `props["#gl"]` and casts it to a host pointer (`Tex*`, `Pipeline*`, `DepthStencil*`).
Property bag semantics ("setFoo:" stores, "foo" reads back) are the fallback for any unmodelled selector, so
an unimplemented Metal setter silently becomes a stored property that a later reader can use.

### 2.1 Textures (`Tex`, `mtl_gles.hpp` around `inline Tex* texture(FakeObjcObj*)`, line ~428)

A guest texture object is realized lazily on first use (as an attachment, sampler or blit source). The
realized fields come from the property bag:

| Key | Default | Notes |
| :--- | :--- | :--- |
| `width`, `height`, `depth` | 1 | `defaultBaseDescriptor`, `texture2DDescriptorWithPixelFormat:width:height:mipmapped:`, `setWidth:` etc. all land here |
| `pixelFormat` | 80 | 80 = `MTLPixelFormatBGRA8Unorm`, 70 = `BC1_RGBA`, 0x1702 is the GL attach target, not a format |
| `textureType` | 2 | `MTLTextureType2D`; 3 array, 4 msaa, 5 cube, 7 3D |
| `sampleCount`, `mipmapLevelCount`, `arrayLength` | 1 | feed `glTexStorage2D/3D` |
| `usage`, `storageMode`, `framebufferOnly` | - | metadata only; host memory is always CPU-visible |

`Tex` holds the GL name, target, internal format, size, `levels`, `samples`, `renders` (how many render
passes used it as an attachment) and `f` (format descriptor). Two guest objects never share a `Tex`.

A guest texture whose descriptor never got a width/height is realized at 1x1. Unity creates a family of
such 1x1 textures on purpose (its single-colour defaults: white/black/grey/bump), so a 1x1 sample is not
by itself a symptom.

### 2.2 Pipelines (`struct Pipeline`, ~line 171; built by `make_pipeline`, ~line 746)

Built from `newRenderPipelineStateWithDescriptor:` (both the `:error:` and
`:options:reflection:error:` variants match on the 37-character prefix).

Members: `prog` (GL program, 0 = unusable), `vs`/`fs` (`msl::Shader`), `attrs` (vertex attributes),
`layouts` (per-buffer stride/step/rate), `blend[8]`, `vs_units/fs_units` (texture unit per sampler),
`vs_bind/fs_bind` (UBO binding per block), `vs_name`/`fs_name` (MTLFunction names, diagnostics only).

`make_pipeline` runs: locate function sources (`fn->props["#library"]` -> `lib->s`) -> `msl::translate` ->
optional SOLID injections -> compile -> link -> bind sampler units and UBO blocks -> read the vertex
descriptor for attribute layout. Any failure returns a Pipeline with `prog == 0`, and the draw path then
drops that draw.

### 2.3 Encoders (`struct Encoder`, ~line 192)

One per `renderCommandEncoderWithDescriptor:`. Holds `pipe`, `vbuf[32]`/`fbuf[32]` (`BoundBuf`: guest buffer +
offset, or `set*Bytes:` copy), `vtex/ftex[32]`, `vsamp/fsamp[16]`, depth/stencil state, cull/winding,
`rt_w/rt_h`, `targets[9]` (GL attachment ids, `targets[8]` = depth) and resolve pairs.

### 2.4 Frames / drawables

`CAMetalLayer.nextDrawable` (`fake_uikit.hpp`, `next_drawable` ~line 275) keeps a pool of
`g_drawable_count` (default 3, `LOADER_PRESENT_CTL` can set 3..8) `CAMetalDrawable` objects. Each owns a
`Tex` with `usage = 4` (render target) and `storageMode = 2`. The pool is rebuilt when the size key changes.

**The layer's pixel size** comes from `layer_drawable_pixels()` (~line 258), used by both `nextDrawable` and
`-[CAMetalLayer drawableSize]`: `drawableSize` if set, else view bounds x `contentsScale`, else the screen.
Keeping the two readers on one function matters because guests size their viewport from `drawableSize`
while the drawable pool is sized from `nextDrawable`.

## 3. One guest frame

```
begin_pass(e, pass)                          mtl_gles.hpp ~1014
  bind thread FBO, attach every color/depth/stencil target from the descriptor
  glDrawBuffers, glCheckFramebufferStatus, apply LoadAction clears (colour is cleared only for
  loadAction==2; depth/stencil default to Clear, matching Metal)
  glViewport(0,0,attachW,attachH) and e->viewport/e->scissor = attachment size   <-- fallback only
  optional debug readback dump (LOADER_DEBUG_CLEAR_DUMP)

per draw:
  setRenderPipelineState:  -> e->pipe = gl_obj<Pipeline>(state)
  setVertexBuffer:offset:atIndex: / setVertexBytes: -> e->vbuf[i]
  setVertexTexture:/setFragmentTexture: -> e->vtex/ftex
  setViewport:            -> glViewport(...) + e->viewport        <-- the value that actually ships
  setScissorRect:         -> glScissor(...)
  setDepthStencilState: / setCullMode: / setFrontFacingWinding: / setBlendColor*
  drawIndexedPrimitives:... -> draw_indexed(e, prim, count, indexType, ibuf, ioff, instances)

draw_indexed                                 mtl_gles.hpp ~1416
  ready(e)                                   ~1353: e && e->valid && e->pipe && e->pipe->prog
  scan indices for maxv, glUseProgram, apply_state, bind_resources (UBOs + sampler units),
  bind_vertices (upload the accessed range into a ring buffer, set attribute pointers),
  glDrawElementsInstanced, then optional trace/coverage readback

commit / present
  presentDrawable: on the command buffer (Unity's newer surface) -> dump on the next commit
  -[CAMetalDrawable present] / presentAtTime: / presentAfterMinimumDuration: -> mtlgl::g_presented_drawable
```

### 3.1 Presentation and dumping paths

There are three presentation shapes in the wild, and the dump hook has to cover all of them:

| Guest call | Recorded in | Frame dump point |
| :--- | :--- | :--- |
| `[commandBuffer presentDrawable:d]` | commit path (`#present` bookkeeping) | `commit()` |
| `[drawable present]` / `presentAtTime:` / `presentAfterMinimumDuration:` | `mtlgl::g_presented_drawable = self` (fake_uikit ~line 689) | next `commit()` -> `dump_presented()` (~1598) |
| no present at all | nothing | `LOADER_APP_FRAME_DIR` path in `src/app/app_gl.cpp` for the app host |

`dump_presented()` reads the presented drawable's `Tex`. Because a guest can present a drawable from an
earlier frame while already rendering the next one, it prefers the last full-size **render target**
(`g_last_big_target_obj`, set by `begin_pass`) when that differs from the presented drawable.

`LOADER_FRAME_DIR` also receives `failed_<n>.metal/.glsl` for every shader the translator or compiler
rejects, and `prog_<id>.glsl` for traced programs.

## 4. Shader translation (`msl_glsl.cpp`, `msl::translate`)

Input: the whole `MTLLibrary` source plus the entry point name (`FakeObjcObj::s` of the
`newFunctionWithName:` result). Output: GLSL ES 3.20 plus metadata (`blocks`, `samplers`, `inputs`,
`color_outputs`).

Facts that matter when a translation is rejected:

* Top-level declarations are parsed by `parse_top` (~1140) and `parse_global_const` (~515).
  `constexpr`/`constant`/`static`/`const` prefixes are skipped before the type. A declaration with no `=`
  is a file-scope **sampler** (`constexpr sampler s(address::clamp_to_edge, filter::linear);`) and is
  registered in `samplers_` with unit 0 instead of emitted.
* Stage I/O structs are looked up in `structs_`; `io_name(out, field)` (~822) maps a stage-output field to
  `gl_Position` / `gl_PointSize` / `v_<field>` (vertex) or `o_<field>` / `gl_FragDepth` (fragment) using the
  member's `[[position]]`, `[[point_size]]`, `[[depth]]`, `[[sample_mask]]` or `[[user(...)]]` attribute.
* `return <value>;` is rewritten by `return_value` (~1023). Supported shapes: `return;` (outputs already
  written), `return output;`, `return (Struct){ .field = ..., ... };`, `return <local>;` where the local was
  declared as `Struct local = { a, b, ... };` (expanded field-by-field into the out assignments by the
  statement pass), and a plain value for non-struct returns.
* Uniform blocks come from `constant T& name [[buffer(n)]]`; samplers from `sampler s [[sampler(n)]]`;
  textures from `texture2d<float|half>` etc.
* `fixup()` (~1043) applies the GL Y flip (`gl_Position.y = -gl_Position.y`), and converts depth to the
  `[0,1]` range only when `GL_EXT_clip_control` is unavailable.
* Unity's HLSLcc output names both functions `xlatMtlMain` and writes `output.mtl_Position` members; hand
  written Metal (the title's own blit) uses named functions (`vprog`/`fshader`) and aggregate returns.

Debug injections in `make_pipeline`:

| Switch | Effect |
| :--- | :--- |
| `LOADER_GL_SOLID` | appends `<vec4 out> = vec4(1,0,1,1)` at the end of every fragment main and before each `return;` |
| `LOADER_GL_SOLID_VS` | overrides `gl_Position` from `gl_VertexID` to cover the whole target regardless of the transform |

Both are shader-source rewrites, so they only prove coverage and output colour; they say nothing about the
guest's own matrices.

## 5. Uniforms and vertex data as observed from Unity 2021.3 + Metal

* UBO block names in the translated shaders: `vs_UnityPerDraw_block` bound to `buffer(1)`,
  `vs_VGlobals_block` bound to `buffer(0)`. `bind_resources` maps each block to a ring-buffer range.
* Unity binds large buffers (multi-megabyte) at those indexes and selects a range by offset; `BoundBuf::data`
  therefore returns `avail = len - offset`, which is normally far larger than the block size.
* Vertex attribute formats seen: `29` = `Float2` (UV), `30` = `Float3` (position); a position+UV vertex is
  `stride 20`. `bind_vertices` copies only `max_index+1` vertices into the ring.
* Guest texture bindings that are unset resolve to `dummy2d` (1x1 opaque black) so a draw never samples an
  incomplete texture unit.

## 6. Practical invariants

1. `glIsTexture` on an attachment, `GL_FRAMEBUFFER_COMPLETE` (0x8cd5) and `glGetError() == 0` after a pass
   are cheap proofs that the attachment chain is sound.
2. Draws are counted, not guessed: `g_draws_frame` (incremented in `draw_indexed`) is drained by
   `dump_presented`/`commit`, so "N draws since last dump" is a real measurement per frame window.
3. A pipeline with `prog == 0` is invisible at draw time (`ready()`); the reasons are printed by
   `dump_failed_shader` ("vertex translation", "fragment translation", "vertex compile", "fragment compile",
   "link") and it also writes the offending MSL and GLSL next to `LOADER_FRAME_DIR`.
4. EGL contexts are per guest thread but share one root context (`gles_backend.cpp:ensure_context`), so GL
   objects created on one guest thread are valid on another; VAOs and FBO defaults are per thread.
5. No GLSL/GL state validation pass exists in the loader by design: correctness is proven by dumping frames
   and by the per-draw trace.
