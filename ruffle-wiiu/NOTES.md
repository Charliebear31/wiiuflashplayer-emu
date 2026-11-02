# ruffle-wiiu: Ruffle (Flash player) on the Wii U

Route: Ruffle (Rust) -> `wasm32-wasip1` -> **wasm2c** -> C -> devkitPPC/wut. Same C also builds a PC test host
(llvm-mingw + SDL2 + OpenGL) so everything except the GX2 layer is tested on PC first.

## Layout
- `ruffle/` upstream clone (shallow, 2026-10-28, 36bd357f). Our crate: `ruffle/wiiu/` (added to the workspace).
  - `src/lib.rs` exports `rw_*` (start/tick/render/mouse/key/text/audio_mix). `rw_start` url may be
    `"<movie url>\n<spoofed url>"` for sitelocked games. Forces ShowAll scale + centered align.
  - `src/render.rs` RenderBackend: shapes tessellated by `ruffle_render::tessellator`, sent to the host as
    meshes; one u32-word command list per frame / offscreen draw (`host::op`). Gradients become 256x1 ramp
    textures; masks = stencil (WebGL backend's scheme); blend modes via fixed-function; cacheAsBitmap and
    BitmapData.draw render into textures (`host_frame(target)`), readback via `host_tex_read`.
  - `src/navigator.rs` local files only (WASI paths, "/" = host root). `src/audio.rs` AudioMixer, pulled by host.
- Patches to upstream (keep when updating): `target_family = "wasm"` -> `all(target_family = "wasm",
  not(target_os = "wasi"))` in core/Cargo.toml, navigator.rs (executor), loader.rs, avm1/globals/sound.rs;
  WASI `url_to_file_path`/`url_from_file_path` in core/src/backend/navigator.rs; workspace member "wiiu".
- `host/rw_core.c` wasm2c instance, WASI subset (14 calls), endian-safe memory access. **WABT_BIG_ENDIAN
  stores wasm memory byte-reversed** (`data_end - addr - n`): words read natively, byte strings reversed.
- `host/gfx_gl.c` + `host/main_pc.c` PC host. `host/wiiu/` Wii U host: `gfx_gx2.c`, `main_wiiu.c`,
  `shaders/ruffle.vert|frag` (CafeGLSL, register uniform mode).
- `host/wasm-rt/` wasm2c runtime copied from wabt 1.0.42, patched for `__WIIU__` (no sigsetjmp/mman,
  `WASM_RT_NO_THREAD_LOCAL`).
- `gen/` generated C (~130 MB, 16 files), `build-pc/`, `build-wiiu/`, `out/<name>/` PC test shots.

## Build
- `tools/env.ps1` (dot-source): Rust in buildtools\rustup+cargo (host `x86_64-pc-windows-gnullvm` so
  llvm-mingw links build scripts), Java (playerglobal build needs it), wabt built from source in
  buildtools\wabt-build (the release wasm2c.exe needs a missing MSVC DLL).
- `tools/build-pc.ps1` -> `build-pc\rwpc.exe`. `tools/build-wiiu.ps1 [-Configure]` -> `build-wiiu\ruffle.wuhb`
  (generated C at -O1, ~10 min). `tools/check-wiiu.sh` compile-only check of the host files.
- Shaders: `buildtools\cafeglsl\...\bin\glslcompiler.exe` (CafeGLSL 1.0.0-rc1) compiles ruffle.vert/.frag to
  ruffle.gsh -> `shaders/ruffle_gsh.c`.

## Testing
- PC: `tools/run-pc.ps1 -Movie swf/x.swf -Name x [-Frames N -Every N -Script tests\x.txt -Spoof url]`;
  games in `testroot/swf/`; PNGs land in `out/<Name>/`. Hidden window, fixed 60 fps steps.
  Script lines: `<frame> move|down|up|click x y`, `key|keydown|keyup <code>`, `type <text>`, `shot`
  (window pixels; the stage is letterboxed into the window).
- Wii U: SD `wiiu/apps/ruffle/` = app folder (`swf/`, `log.txt`, `shots/`, `autotest.txt`). autotest adds
  `0 movie /wiiu/apps/ruffle/swf/x.swf`, `touch x y`, `press|release <button>`, `quit`. Use
  `tools/cemu-test.ps1 -AppFolder ruffle -App ruffle-wiiu\build-wiiu\ruffle.wuhb -Script ...`.
- Wii U controls: GamePad touch = mouse; D-pad/left stick = arrows; A space, B z, X x, Y c, + Enter,
  - Escape, L Shift, R Ctrl. Optional `<game>.swf.url` next to a game = spoofed URL (sitelocks).

## Naming (user rule: no file named "ruffle" in what ships)
- Output `build-wiiu\flashplayeremu.wuhb/.rpx`, SD folder `sd:/wiiu/apps/flashplayeremu/`, shaders
  `host/wiiu/shaders/flashplayer.*`, generated C `gen/flashplayer_*.c` (wasm2c `-n flashplayer`).
  The upstream clone and our crate (`ruffle/`, `ruffle_wiiu`) keep their names; credit text stays.
- Cemu tests: `tools/cemu-test.ps1 -AppFolder flashplayeremu -App ruffle-wiiu\build-wiiu\flashplayeremu.wuhb`.

## Wii U menu ("Flashplayer Emu")
- Settings (+ or the SETTINGS tab): Menu music Off..Max, Side borders On/Off, Retake game pictures,
  Credits, Back; saved in `settings.txt` (music=, borders=). One tap on a game starts it.
- Borders only when side bars >= 60 px; 16:9 (or wider) games fill the screen with none
  (`tests/wiiu-wide.txt`, `tools/make-test-swf.ps1` makes plain-color SWFs of any size).
- Shown name is **Flashplayer Emu** (menu title + .wuhb NAME). Footer credits: the theme's music,
  **Port by Charliebear31** (user's wording; keep exactly). Ruffle/minimp3/CafeGLSL/libnds font credited in
  `host/wiiu/content/CREDITS.txt` (packed in the .wuhb).
- Look: old Flash-game-site homepage (black page, rainbow letters, bright tabs, colored tiles) - styled after
  the user's "old Coolmath Games" request without their name/logo. `host/wiiu/ui.c` draws it on the CPU into
  the screen texture; 4x2 tile grid, tabs double as a controls legend.
- Music: **only** the bundled `content/music/*.mp3` (/vol/content in the .wuhb), decoded by minimp3 in
  `music.c`, looped, menu only. User rule: no custom/SD music.
- Game pictures: first time a game is seen, `make_thumb` runs it at 400x300 until Ruffle reports the root
  movie loaded (`rw_load_permille`) plus 150 ticks (cap 1800), halves the last frame to 200x150 and caches
  `thumbs/<file>.thumb` (header "RWT2" + SWF size; a changed file or old RWT1 picture is retaken).
  `rw_core_reset()` throws the wasm instance and all GPU objects away between games (also recovers from a
  trapped game). bloons2xmas.swf is a MochiAds wrapper whose game came from mochiads.com (dead): it never
  gets past its loading bar, on PC too.
- Menu <-> game: `run_picker` / `run_game` in main_wiiu.c. Holding Minus 1 s in a game (or script `leave`)
  returns to the menu (short press = Escape). Music fades out over 0.8 s after a game is chosen
  (`music_fade_to`, then `music_pause` frees the audio device for the game) and fades back in over 1.5 s
  from the same spot (`music_resume`).
- Side borders: when a game's stage leaves >= 60 px bars at the sides (4:3 etc.), `ui_draw_border` art (stars,
  rainbow frame, stacked title, control pills) is shown around it; `gfx_gx2_set_border` draws the art, then
  the stage rect (`rw_stage_size`) from the screen target. Game screenshots (`game_*.bmp`) include it.
- Autotest: each phase (menu, game, menu again...) counts frames from 0; `movie <path>` and `leave` end a
  phase, later lines belong to the next. Menu: `press|release <button>`, `shot`, `quit`, `movie`. Game: the
  PC-host commands plus `touch`, `press|release`, `leave`, `quit`. `tests/wiiu-menu.txt`,
  `tests/wiiu-mario.txt`, `tests/wiiu-border-leave.txt`.
- Cemu: `OSSwitchFiber` returns at once without running the entry; the 16 MB stack switch is
  `stack_switch.S` (thread stackStart/End updated around it).

## Filters, saves, video (2026-10-29)
- Filters: `ruffle/wiiu/src/filters.rs` = CPU versions of blur, glow, drop shadow, bevel, gradient glow/bevel,
  color matrix, convolution (premultiplied RGBA). Used for cacheAsBitmap entries (render, read back, filter,
  upload) and BitmapData.applyFilter. Not done: displacement map, Pixel Bender shader filters.
  Checked against Ruffle's visual tests (tests/tests/swfs/avm2/bitmapdata_applyfilter_blur matches Flash
  except edge falloff; bitmapdata_draw_filters matches Ruffle's own known-wrong output). Slow for big
  filtered objects on the console (GPU readback + CPU work) - a GPU path would be the next step.
- Saves: `storage.rs`, one `.sol` per SharedObject key in `/wiiu/apps/flashplayeremu/saves/` (WASI
  path_create_directory / path_unlink_file / path_filestat_get added to rw_core.c).
- Video: Ruffle's software decoders (H.263, VP6, Screen Video) are on by default. H.264 is not possible:
  Ruffle only plays it through Cisco's OpenH264 binary downloaded at runtime.
- Bug fixed: rw_core_audio kept a wasm address across rw_core_reset, so the second game started in a
  session wrote audio into random memory of the new instance (AS3 "VerifyError Class <garbage>", or an
  out-of-bounds trap). PC host script `movie <path>` starts another game in-process to test this.
- Wii U objects are assembled through sa/tools/as-crfix (Cemu stale-cr0 guard) - harmless, kept.

## Code size limit (white screen before main)
- The Wii U app's .text must stay under 32 MB: at -O1 it reached 34 MB once Stage3D (naga GLSL writer)
  was added, and Cemu hung on a white screen before `main` ran. -Os fit but miscompiled the generated C
  (AS3 regex searches ran away allocating and trapped). Generated C is now `-O1 -fno-inline` (.text ~28 MB
  with the CafeGLSL library linked; most of -O1's size was wasm2c's load/store helpers inlined everywhere).
  Check with `tools/sections.sh`.

## Stage3D (2026-10-29)
- `ruffle/wiiu/src/context3d.rs`: Context3D. AGAL -> naga-agal -> naga GLSL 420 (cached per vertex
  attribute layout); Flash state kept in Rust, one self-contained packet per drawTriangles
  (`host_c3d_draw`), clears via `host_c3d_clear`, buffers/programs via `host_c3d_*`. Back/front buffers are
  normal host textures; `render_stage3d` draws the front buffer opaquely with kind 3 (raw RGB, alpha 1).
- PC (gfx_gl.c): done. `tools/s3d-tests.ps1` runs Ruffle's 22 Stage3D tests against their reference images:
  17 match (triangle variants, cube, fractal, stencil, samplers, blend, constants, cross product...).
  stage3d_texture differs only because the test clicks a button (input.json not replayed).
  stage3d_bitmap (31%) only differs in frame count (random sprites, "134 created" vs 49): drawing is right.
  stage3d_raytrace (86%): the demo's camera follows the mouse and the reference was taken with another
  mouse position; the scene and cube-map sky draw correctly.
- Cube textures: a 2D texture with the 6 faces stacked vertically (+X,-X,+Y,-Y,+Z,-Z); context3d.rs turns
  `samplerCube` into `sampler2D` and `texture(s, dir)` into `cube_atlas(s, dir)` (CUBE_ATLAS_GLSL), so
  neither host needs cube maps. Mip levels > 0 are ignored (core stubs them).
- ATF / compressed textures: all three ATF tests match their references (PC 0%, Cemu by eye). Core's
  `jpegxr` feature is on: JPEG-XR ATF decodes to RGBA, CompressedAlpha/RawCompressedAlpha give DXT5 blocks
  that context3d.rs decodes to RGBA (dxt5_to_rgba; C3dTexture.dxt5 for CompressedAlpha textures).
  The jpegxr crate (jxrlib C + bindgen) is vendored in ruffle-wiiu/vendor/jpegxr (workspace Cargo.toml
  points there): on wasm32-wasip1 it uses its fake libc's headers only and links WASI libc (its Rust
  malloc glue is for wasm32-unknown-unknown only); wasi_extra.c stubs tmpnam. Built with the official
  LLVM release in buildtools\clang+llvm-23.1.3-x86_64-pc-windows-msvc (clang with the wasm target +
  libclang; env.ps1 sets CC_/AR_wasm32_wasip1 and LIBCLANG_PATH). buildtools\llvm-dl holds the download.
  rw_core.c gained fd_seek, fd_fdstat_set_flags and path_remove_directory for it.
  Code size: .text is now 29.65 of 32 MiB.
- sa/tools/as-crfix (shared with the GTA SA build): its added `mcrf 0,0` lines pushed a conditional branch
  in a huge generated function out of its 32 KB reach once jxrlib was linked. It now rewrites conditional
  branches to local labels in functions over ~7000 instructions into the long form (inverted bc over a b).
- Wii U (gfx_gx2.c): works in Cemu for triangle, rotating cube, fractal, blend and texture
  (`tests/wiiu-stage3d.txt`, all five in one run; `tests/wiiu-s3d-tri.txt` / `wiiu-s3d-cube.txt` for one).
  CafeGLSL's Wii U library (libcafeglsl.a) compiles the GLSL at runtime in register mode (flags bit 1:
  context3d.rs rewrites uniform blocks, one-line or multi-line, to plain `uniform vec4 name[N]`).
  Things that cost time, all needed:
  - Vertex buffers are stored as big-endian 32-bit words at upload and read with GX2_ENDIAN_SWAP_DEFAULT;
    index buffers are byte-swapped to big-endian u16 and drawn as GX2_INDEX_TYPE_U16. Cemu drew nothing
    with ENDIAN_SWAP_NONE / U16_LE.
  - Constant arrays: CafeGLSL lists the array and every element ("name[3]"); use the lowest offset.
  - Culling with GX2SetCullOnlyControl made Cemu drop every triangle; GX2SetPolygonControl works.
  - gfx_reset frees Stage3D buffers/programs; compiled shader pairs are cached for the whole run.
  - In Cemu a compile sometimes came out empty (16-byte vertex shader, no attributes/uniforms) for a few
    hundred ms after starting a game, then right with the same input. Not compiler state (restart didn't
    help), not heap/stack garbage, not the mfcr bug (guarding all 1963 mfcr in prebuilt code didn't help).
    compiled_shaders() retries every 30 ms while the result lacks what the source declares. Probably
    Cemu-only; check on a console. `tools/unguarded-mfcr.sh` lists prebuilt functions with bare mfcr.
    Note: an 8-byte pixel shader is valid (pass-through = one export); don't treat it as empty.
  - Shaders compile on their own thread (core 2, CompileJob queue); a program draws nothing until ready,
    except its first draw waits up to 2 s (games that draw one still frame). In Cemu the raytrace demo's
    116 KB fragment shader hangs CafeGLSL (sometimes; bisected cuts hang or not run to run, PC compiles it
    in 0.1 s); the app keeps running but later compiles queue behind the hung one for that session.
  - Cube textures and raw DXT5 ATF work in Cemu (tests/wiiu-s3d-dxt5.txt; raytrace in
    tests/wiiu-s3d-textures.txt only draws if its compile finishes).
  ATF: tests/wiiu-s3d-atf.txt (all three kinds). Not tested on a console.

## Menu extras
- Launch: tile pops + flashes, synthesized chime (music.c `music_sfx_launch`, not affected by music volume),
  GPU fade to black (`gfx_gx2_set_fade`), music fades out; menu fades in on open; music fades in at boot.

## Console speed and stability (2026-10-30, from the user's console logs)
- Log: plat_log only queues lines; a writer thread on core 0 writes / flushes (a flush took ~1 s on the
  console and stalled games that logged every frame). Repeated lines are counted. Every 300 game frames:
  fps, logic / render / present ms, then a "drawing:" line (GPU / CPU filters, shapes built, uploads).
  Menu: fps + drawing / upload ms every 120 frames.
- Wasm memory: one block reserved at app start (largest malloc under 1 GB, minus 240 MB for GPU memory)
  and reused by every game (rw_mem_reserve / rw_mem_release). malloc says yes to >= 1 GB requests that
  then can't be had, so they're never tried; re-reserving per game failed on the console.
- Frames: WHBGfxFinishRender's GX2DrawDone is skipped (gfx_gx2_present): the GPU draws frame N while
  the CPU runs N+1; memory freed in frame N is released after frame N+1 (timestamps). Ruffle draws only
  when the game has a new picture (rw_core_tick's result).
- Menu: only changed areas are restored, redrawn and uploaded (ui.c frame_begin / drawn_over,
  gfx_tex_update_screen_rects); flushing whole lines was ~9 ms a frame on the console.
- Filters: blur, glow and drop shadow run on the GPU (shaders/filter.frag, gfx_filter); others on the CPU
  (filters.rs, 8-bit). Digger: 25-30 ms -> 0.2 ms drawing in Cemu. Test script "cpufilters" compares.
  The CPU versions read GPU-drawn pictures back, which in Cemu gave stale data (Digger's title lost its
  outline); the GPU path has no read-back.
- System keyboard (kbd.cpp, nn::swkbd) opens when a text field gets focus (HostUi in ui.rs); "settext"
  in test scripts types into the focused field. tests/kbtest.swf is made by tools/make-kbtest-swf.ps1.
- GPU memory (2026-10-30): shares the default heap; the wasm reservation leaves 384 MB (MEM_HEADROOM) and
  the keyboard loads before it. On failure gpu_alloc waits for the GPU, frees everything pending and
  retries; NULL images are never flushed (that was a DSI at address 0 = the console freeze in Zuma).
  Sizes are tracked (block table) and logged with the per-300-frame "drawing:" line. Tried and dropped:
  a separate expheap inside a default-heap block (crashed Cemu); top-of-heap (negative alignment)
  allocations are refused by Cemu (falls back to normal). Zuma in Cemu holds ~35 MB of GPU memory.
- Filters on the GPU also cover the colour matrix (mode 3). "filter on the CPU: <kind>" is logged once
  per kind still done on the CPU. The menu waits only for the TV/GamePad pictures (g_blit_ts), not the
  frame's swap, before writing its picture; settings rows redraw just the panel over a cached darkened menu.
- Cemu reads back GPU-drawn pictures unreliably (screenshots early in a game show stale data); the game
  is drawing (checked by counting commands). Games start on a black screen now.
- Menu folders (2026-10-30): menu.txt on the SD card keeps the order and folders ("g <path>",
  "f <name>" + "  <path>" lines; main_wiiu.c load_menu / save_menu). A tap starts a game on release;
  moving a held tile 24 px or holding it 0.5 s picks it up. Held 1/3 s over the middle of another tile:
  folder (or into that folder); otherwise it goes where the red bar is. Inside a folder: B / "back",
  X / "rename" (system keyboard); dropping a game on "back" takes it out (an emptied folder goes).
  Test scripts: "down x y", "move x y", "up" in the menu; "settext" renames the open folder.
  tests/wiiu-folders.txt (delete the Cemu SD's menu.txt first).
- Ads and links: games can't open anything. navigate_to_url only logs, fetch is local files only and
  sockets always fail (navigator.rs); keep it that way. Ads built into a SWF can still be shown.
- Menu per frame (ui.c): the chosen tile stays on the canvas until the choice changes (g_sel_drawn); only
  its opaque top edge and the equalizer change. Scrolling redraws just the grid. The highlight jumps, it
  doesn't slide. gfx_tex_update_screen_rects doesn't wait for the GPU.
- User's game set (60 SWFs, 2026-10-30): none crash in Cemu, but Mochi-packed games and timed preloaders
  (Bloons, BTD3/5, Happy Wheels, Duck Life 2-4, most Papa's) stay stuck in Cemu; the PC host passes them only
  with -realtime, and the console passed several in older builds. Cause in Cemu not found. Bloons 2 holds
  itself to 30 fps (33 ms a frame on PC too). Bevel is GPU mode 4; filter clears cover only the area in use.
- Big SWFs: decompress_swf reserves the header's size up front (try_reserve_exact, the 128 MiB cap only as
  fallback) and load_root_movie drops the file's bytes after from_data. btd5_no_banner.swf (165 MB) now
  plays in Cemu, peaking near 611 MB of the 624 MB reservation with 215 MB of GPU memory after 30 s.
- CPU filter results go back with put_pixels (a draw): the temp picture must outlive the flush, or the draw finds it freed and skips silently.
- Test scripts: `click` presses in the same frame as the move, so a button only sees it once already hovered. Use `move`, then `down`/`up` some frames later. Super Mario 63 story level: move/down/up at 640,600 (START), 320,460 (STORY), 255,240 (File A), 945,225 (CREATE), then B (Z) dismisses text.
- tools/fp-mem-context.sh must report 0 before shipping; it ignores constant-table loads (lis) and code
  after unconditional branches. LTO was tried: no faster, so it stays off.

## Memory, picture updates, sound (2026-10-30, later)
- Memory: big GPU blocks fill the reserved wasm block from the bottom while wasm grows down from the top
  (gfx_gx2.c pool_alloc, main_wiiu.c rw_mem_can_grow). 752 MB shared, 256 MB left for malloc and small GPU blocks.
- Host mallocs in rw_core.c are null-checked. Each 300-frame log has a "memory:" line.
- gfx_tex_update waits only if the GPU may still read that texture (epochs), and copy_in/copy_out swap words.
  The doll game no longer slows down in Cemu.
- Sound fills the SDL queue by level (120-400 ms ahead), not by frame time. The old way capped at 33 ms a frame,
  so anything under 30 fps ran short. Each log has "audio:" and "game speed:" lines.
- The FPS counter now shows game pictures a second, so a 30 fps game shows 30.
- Device fonts: Liberation 2.1.5 in host/wiiu/content/fonts (OFL), read on demand through the ".content/" path
  in rw_core.c path_open; names mapped in wiiu/src/ui.rs font_family. _sans/_serif/_typewriter use them too, with
  Noto Sans for missing characters. The PC host has no content path, so it stays on Noto Sans.
- Black blocks in pictures on the console: gfx_tex_update skipped its wait for a picture just drawn into, and the
  GPU's colour cache overwrote the update. gfx_frame now marks its target's epoch.
- Controllers: pads.c merges the GamePad and four KPAD channels (Wii Remote + pointer, Nunchuk, Classic, Pro).
  In games the left stick moves a mouse arrow (gfx_gx2_set_cursor), L/R click, the right stick is a second d-pad,
  ZL/ZR are Shift/Ctrl, Minus leaves at once. Scripts: "stick x y", "press l". Remotes and Pro are untested:
  Cemu's test setup has none connected.
- Themes (ui.c THEMES, settings.txt theme=): Classic, Adobe Flash, Halloween. theme_animate redraws only the two
  header ends each frame. Classic's shapes aren't in the cached base, so the dimmed menu behind settings lacks them.
- Theme music, chosen by the user (main_wiiu.c THEME_MUSIC): Classic "Cipher", Flash "Sneaky Snitch",
  Halloween "Not As It Seems", all Kevin MacLeod (CC BY). The tracks are in content/music; music_switch reloads on a theme change.
- GPU speed: games draw into tiled g_game_screen and filters use tiled scratch textures (linear targets are slow on
  the console). Readback draws the tiled picture into linear g_screen (GX2CopySurface left it unchanged in Cemu).
- Log "slow spots" appear after 300-frame windows with logic over 20 ms: PC buckets with counts from the console.
  Subtract 0x0A000000 and run addr2line on the shipped ELF. In Cemu, saved srr0 is byte-swapped and addresses
  match the ELF directly (no subtraction).
- Moto X3M driving, Cemu profile (logic 44-70 ms): AS3 interpreter run_actions 27%, Value::abstract_lt 24%, wasm
  memory loads/stores about 20%, audio mixing about 12%. Cemu sampling may be biased; confirm with console slow spots.
- Moto X3M level from a script: "520 click 640 620", "800 click 960 235".
- Graphics.drawTriangles / GraphicsTrianglePath: uvt data works (one bitmap fill per triangle; perspective t ignored)
  and solid triangles use non-zero winding. render_alpha_mask uses two layers and blend 11 (alpha channel fixed too).
  displace() does DisplacementMapFilter on the CPU. These two have no test game: only compiled and regression-run.
- Blend modes: render.rs blend() draws groups through layers (MAX_LAYERS 8 a target, then per draw; one lone draw
  skips the layer for modes the blender can do). gfx_blend_layer does difference, overlay and hard light, plus
  multiply, lighten and darken on non-screen targets, in filter.frag mode 5.
- Conformance: ruffle/tests/tests/swfs has about 4,900 SWFs, many with output.expected.png. Copy a test folder to
  sdcard/swftest and shoot it in Cemu, or use build-pc\rwpc.exe. Cemu doesn't show CPU writes to a texture that was a
  render target (CPU filters on cached objects look unfiltered there): check those on the PC host.
- Pixel Bender: wiiu/src/pixel_bender.rs (naga-pixelbender -> GLSL -> CafeGLSL), gfx_pb_run. flatten_shader removes
  all `if`s and the sampler helper because CafeGLSL hung in Cemu on them, and writes cross() out (it gave green from zero
  inputs). The PC host doesn't run these (gfx_pb_run returns 0): test in Cemu. Look right there: tintype, smudge, twirl,
  dithering, images, glassDisplace and its applyFilter. Not matching: BlurredFocus (halo still green; a vector pow() written
  out changed nothing). Float targets for ShaderJob output are unverified.
  CafeGLSL bug found with shader_as_mask (2026-11-02): naga parks literals in register 0's z/w (`reg0.z = float(vec4(0.5).x)`)
  and reads them through a copy (`_e9 = reg0; ... * _e9.zzzz`); CafeGLSL then kept a stale x lane, so `(a + b) * 0.5`
  came out as `(b.x, (a+b).y/2, ...)`. Writing the literal in place of the read fixes it, so flatten_shader's
  propagate_immediates does that for every such read (merge_component_writes also folds naga's four per-component
  copies into one vector write; harmless, not the fix). Bisected with scratchpad pbtry.ps1: a pb_override.glsl in the
  app folder replaces the generated fragment shader, and pb_last.glsl is what was generated. shader_as_mask's shader
  quadrant now equals Ruffle's own output.ruffle.png; the rest of that test is Ruffle's known failure (shader masks).
- Anything drawn with render_bitmap must stay alive until the flush: the last handle's drop frees the picture and the host
  skips the draw silently (put_pixels and apply_shader_filter both did this).
- Text engine (2026-10-31): text_block.rs breaks lines itself (width, breakOpportunity, fitSomething) and keeps every span's format; a TextLine's origin is its baseline. 22 of 28 tests match real Flash; compare by trace with output.txt. Not done: bidi, vertical text and per-span rotation, ligatures, real getFontMetrics, mirror regions, tab alignment other than start; children of a TextLine.
- Stage3D mipmaps: gfx_tex_mip adds a linear mip chain on first upload; not for cube textures. ATF: atf_jpegxr.rs decodes every mip; the lossy record layout is a guess (no public description, no test file) and only DXT data is used.
- Test scripts: in a game `down`/`up` take their own `x y` (without them the press lands at 0,0); `click x y` does all three.
  In the menu `move`/`down` are touches. `realtime` must come after `movie` (before it, the menu loop consumes it). Without it, audio takes the fixed-step path, which never updates the peak meter and sounds choppy.
- Digger (Diggy, Box2D, GroundMap read with getPixel, holes drawn with BlendMode.ERASE): falls through the ground on the console with no tunnel drawn; in Cemu it digs normally. Possibly the same console-only problem as Moto's black blocks (unproven). swf/Readback Test logs "RB" lines with wrong pixels; all are 0 in Cemu.
- 2026-11-02 additions: the system keyboard gets the Wii Remote channels too (pads_kpad -> swkbd ControllerInfo.kpad;
  untested, Cemu has no Remote here). Online stand-ins in content/stubs: AGI (Armor Games), Kongregate (API_AS3_Local.swf:
  a dynamic Sprite whose services are NullService, a Proxy that answers anything), Blank (AVM2) and BlankAS2 (18-byte AVM1)
  for CPMStar/Gunblood ads; the table is STAND_INS in navigator.rs. Text engine: ElementFormat.getFontMetrics uses the
  font file's OS/2 tables (FontMetrics.extra, only for FontFace fonts; SWF/Memory fonts keep the old guesses);
  TextLineMirrorRegion is real and TextLine.initMirrors (called from text_block.rs) forwards mouse events to the
  mirrors, TextLine now takes mouse picks; right-to-left runs are reversed (visual_order in text_block.rs: a simplified
  bidi, no embeddings; the line keeps a logical->visual map for atoms). Liberation Sans draws Hebrew (checked in Cemu); Arabic has
  no joining forms (no shaper). TextLine::filter_clip_event accepts mouse events, as EditText does, or none arrive. Not done: per-span rotation (lineRotation
  turns the whole line and works), tab alignments other than start.
- Wii Remote typing on the system keyboard (kbd.cpp feeds pads_kpad(0..3) to nn::swkbd::Calc) confirmed working on a
  console (2026-11-02).
- engine/shaping.rs (2026-11-02): before a line's EditText gets its text, Arabic letters become their joined presentation
  forms (U+FE70.. / FB50.. by joining context, lam-alef as one glyph) and fi/fl/ffi/ffl become the font's ligature glyphs
  (U+FB01..FB04) when the format's ligatureLevel allows. One unit per character stays: a ligature's other letters become
  U+200B, so atoms, mirror regions and char positions still index the original text. Only forms the font has are used
  (element_format::resolve_font, shared with getFontMetrics); Liberation Sans has fi/fl but no Arabic, so joining only
  shows with a game's own embedded Arabic font. The line breaker still measures the unshaped text (a hair too wide).
- Audio: the mixer's dasp linear resampler (f64 per sample) is replaced by FixedResampler in mixer.rs (16.16 integer
  phase, i16 lerp); the console spent 5-13% of each frame mixing. Output stays 48 kHz (AX).
- Console speed work (2026-11-02, from the console's slow spots): BitmapData.perlinNoise runs natively in host/perlin.c
  (a line-for-line port of turbulence.rs, same pixels) over all three cores on the Wii U, via host_perlin_noise; Happy
  Wheels' menu clouds took 15 s of noise on one core as wasm. The garbage collector's sleep factor is 1.5 (default 0.5,
  Player::set_gc_sleep_factor): Gunblood spent about a fifth of its fight in do_collection/sweep. Filter::calculate_dest_rect
  leaves an invalid rectangle alone: a filtered empty clip grew the INVALID marker (0x7ffffff twips = 6710886 px) into a
  "real" rectangle, so Gunblood's shadows got 6.7-million-pixel bounds and lost cacheAsBitmap.
- Happy Wheels' black level: its FPS text is BlendMode.INVERT, which went through a full-size layer, and the invert blend zeroed everything see-through. Fixed in set_blend (both hosts). Coloured or black sources still aren't exact: Flash inverts by coverage alone. main_wiiu.c also frees unused memory at once when the block is within 112 MB of full (rw_collect); that path hasn't been seen to run yet.
- content/stubs/AGI.swf stands in for Armor Games' online AGI.swf (navigator.rs), which games wait for with no error handler (Johnny Upgrade). Readback Test on a console: 0 wrong, so Digger's fall and Moto's blocks aren't pixel readback.
- Saved websites as game folders (Angry Birds Coke / McDonald's): a folder with no SWF at its top is listed under the
  folder's name, using the SWF its page (.html/.jsp/.php/...) embeds, else the biggest SWF below. start_url passes that page
  as a third line to rw_start; site.rs reads FlashVars from it. navigator.rs on_site finds "/x", "http://site/x" and
  page-relative links inside swf/<folder>. Those Angry Birds are slow: ~70% in the AS3 interpreter (Cemu, about 240 ms a frame).
- H.264: wiiu/src/video.rs + host/wiiu/video_h264.c, using h264.rpl on a worker thread with a 2 s limit. It decodes in
  Cemu and matches visual/video/h264 (the earlier "Cemu freezes in H264DECExecute" was the callback bug below).
  Its memory comes from malloc at level 4.2 (35 MB per stream at 256x160): the system heap gave "out of memory" on the
  console at level 5.1. The 100 ms pause after opening saves the log line in case the decoder never comes back.
  The output callback's userMemory is the value itself, not a pointer to it. The engine never ends a stream, so video.rs
  keeps 2 decoders at most, closing the least recently used. Confirmed on a console (2026-10-31): 39 MB per decoder, and
  a second one doesn't fit in the heap, so in practice one is live and the "out of memory" warning before a reopen is
  normal. visual/video/h264 slows down when left running because its timeline adds a new Video every 6 s: not a leak. A single framebuffer is passed to Execute: if a video with
  B-frames shows out of order on a console, give it a ring of them.
- Stage3D DXT1: raw ATF only (RawCompressed, dxt1_to_rgba); no test file exists, so it's untested. Not done: LZMA/JPEG-XR
  ATF "Compressed" and lossy formats, mipmaps (level 0 is always sampled).
- Console logs (2026-10-31): Moto X3M 4-5 fps with black blocks in the beach. It draws pictures on the GPU, reads them back and
  updates 1.3 MB of pictures a frame. settle_target flushes the colour cache before the CPU touches a drawn picture: a best
  guess from the block shape, Cemu never showed it, confirm on a console. Host fills (host_fill_words) and direct uploads
  (gfx_tex_update_begin) replace a wasm loop and a double copy. gfx_reset now frees at once: LTF3 left the pool top at 360 MB.
- Watchdog stacks stop in system calls such as the log's mutex: they don't show where time goes. Use the alarm "slow spots",
  and look addresses up with nm (addr2line gave misleading names). EBF5 at 520 + 225 MB is the real limit, not a leak.
- Console log of 2026-11-02 (build 1505), mapped with addr2line -f against the shipped ELF: the names it gives are the
  innermost inlined frame, so "rw_copy" / "i32_load_unchecked" / "i32_store_unchecked" are the inlined wasm memory
  accesses of whatever function holds them (no such symbols exist in the ELF: the accessors are inlined as intended).
  Shares: AVM2 run_actions + its memory accesses 50-70% in Zuma / Happy Wheels / Angry Birds McDonald's (interpreter);
  audio mixing (AudioMixerProxy::mix + mp3 synthesis) 5-13%; set_tex_surface's memset of every new texture 4-10%
  (Digger, Angry Birds); fill_words 21% in Moto X3M; libm rintf 10% in Digger (f32.nearest); copy_out 7-11% (already
  lwbrx). Memory growth doesn't move anything (expand_data_allocation only zeroes the new pages). Fixed in the next
  build: host/wiiu/fastmem.h fills with dcbz (no read-allocate of the lines being overwritten) for texture clears and
  fill_words, textures that get every pixel written skip the clear, and gen.ps1 replaces wasm_nearbyint(f) with the
  add/subtract 2^23 (2^52) trick. Not fixed: the interpreter and the mixer (the mixer can't leave the wasm thread).
  Speeds that log: Johnny Upgrade 59 fps, Angry Birds McDonald's 58 in play (17 while loading), Moto X3M 14, Digger
  18-51, Happy Wheels 21-34, Zuma 25-58 (56+ once past its intro). Script: scratchpad conslog.ps1 (reads Desktop\log.txt).
- Second console log the same day (build 1544, with those fills): fill_words fell from 21% to ~1% of Moto X3M's samples
  and rintf is gone, but Moto's race is still ~5 fps (interpreter + copy_out + memmove: its picture copies) and Angry
  Birds McDonald's in a level is 1 frame every 3-4 s, 56% interpreter + memory accesses, 27% memory copies
  (memory_copy/copy_out/write_pixels/BitmapData init): a Box2D game in AS3, not fixable without a JIT. Digger 30-59.
  Moto's black blocks (video): the sea's horizontal wave lines come out as 1-px vertical stripes and the sand's wavy top
  edge as 8/16/32-px square steps, console only. Two console-only holes fixed blind (Cemu has no CPU cache and zeroes
  memory): a picture's depth/stencil buffer is now cleared when it becomes a target (BitmapData.draw never clears, so
  masks inside a drawn clip started from junk stencil on the console), and settle_target drops the CPU's cached lines of
  a picture after the GPU drew into it (a partial tex_update merged new pixels into stale cached lines, zero from the
  picture's fill, and wrote them back over the GPU's work 8 pixels at a time). A junk-filled stencil in Cemu changed
  nothing (Cemu doesn't take CPU-written depth data). Console video (build 1626): Moto's blocks and stripes are gone
  and Digger digs again, so the two together were it (not separated; the cache one is the likelier cause of both).
  Lesson: a console-only picture bug is CPU cache coherence or uninitialised memory until proven otherwise; Cemu has
  neither. Moto's race is still ~5 fps (interpreter + 38 MB of picture updates per 300 frames).
- Stuck games: core/src/timer.rs update_timers' 64-bit `tick_time < cur_time` came out false in Cemu, so overdue timers
  never fired. Now tested by the difference's top bit. That unstuck Cat Mario's level, Duck Life 2, Papa's, Bloons and
  Learn To Fly 2 at once. Suspect other u64 comparisons if something "can't" happen. Test in levels, not just menus.
- Memory: pictures and sounds in the root movie point into its data (movie_clip.rs share_with_movie,
  audio SHARE_NEXT_SOUND). Session-long GPU surfaces use the heap (gfx_gx2.c g_heap_next): one high in the shared block
  pinned it at 252 MB and EBF5 (137 MB SWF) ran out at 511 MB of game memory. EBF5 story scene: 555 MB game, 137 MB GPU.
- Slow spots use a periodic alarm on the game's core (real pc, lr). OSSuspendThread sampling only saw system calls.
- On-screen keyboard: tab at the far left in games (ui.c OSK table, main_wiiu.c osk_*). Script touches: "tdown x y", "tup".
  GamePad only (user rule): draw_screen_to skips it on the TV, and only touch presses it. Shots still show it.
- All 63 games in Desktop\swf ran 20 s each in Cemu with clicks and no crashes (scan in the session scratchpad).
- Still missing: network, sockets, camera/mic, about 120 engine stubs (grep avm2_stub_ in core/src, mostly text
  engine, display3D, globalization, 3D display transforms).
- Cursor shots are composed in shot_full; the TV window can't be captured while Cemu is hidden.
- Not done: CompressedBitmap and Sound still copy their bytes out of the movie (a big game's memory).
- Open: in Cemu, Duck Life 2 stays at 38% and the Snoring start button doesn't react; both work on the PC host.
  Check the log's peak over the whole run; a peak of 0 usually means a menu with no music yet.

## Status (2026-10-29)
- PC host: Unfair Mario (AS2), World's Hardest Game (AS2), Papa's Pizzeria (AS2, big), Awesome Tanks (AS3)
  load and render correctly; ~0.5 ms/frame Ruffle time on PC for Mario.
- Wii U (Cemu): menu, music, game pictures and Unfair Mario run; GX2 output matches the PC host. Ruffle
  ~10 ms/frame in Cemu for Mario (console speed unknown). Not tried on a real Wii U yet.
- Open: Unfair Mario's Start button does nothing (only the sponsor getURL fires; spoofing didn't help) -
  compare with desktop Ruffle. Not done: filters (blur/glow), alpha masks, Stage3D, non-builtin blend
  shaders, H.264, network URLs, saving SharedObjects to SD (MemoryStorageBackend now), on-screen keyboard,
  right-stick mouse for TV play, returning to the picker without restarting.
