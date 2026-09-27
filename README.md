# CrossRender

A cross-platform 2D/3D game engine written from scratch in C++17 with CMake and
OpenGL. No GLFW, no SDL, no NanoVG, no external 3D framework: windows, input,
the vector renderer, the font rasteriser, the animation system and the Lottie
player are all implemented in this repository.

```
┌──────────────────────────────────────────────────────────────────────────┐
│  targets                                                                 │
│    WebGL 2 (WASM)   macOS (GL 3.3 core)   Windows (GL 3.3 core)          │
│    Linux (GL 3.3 core)   iOS (GLES 3)   Android (GLES 3)                 │
└──────────────────────────────────────────────────────────────────────────┘
```

## Quick start

```bash
./build.sh              # build the engine, the example and the tests (host OS)
./build.sh run          # build and launch the feature showcase
./build.sh test         # build and run the whole test suite
./build.sh wasm         # Emscripten / WebGL 2 build
./build.sh ios          # iOS (Xcode project, arm64, OpenGL ES 3)
./build.sh android      # Android (arm64-v8a, OpenGL ES 3)
./build.sh all          # every platform available on this machine
./build.sh --help       # all options
```

Plain CMake also works:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/crossrender_example
```

### The example app

`crossrender_example` is one binary that demonstrates every feature. It opens on a
main menu built from a scene registry: **tap a card to open a scene with one
example**, press `Esc` to come back, `F1` for the debug overlay.

```bash
./build/bin/crossrender_example --list          # print every scene
./build/bin/crossrender_example --scene voxel   # jump straight to a scene
./build/bin/crossrender_example --headless --frames 120 --screenshot shot.png
./build/bin/crossrender_example --retro pixel --retro-palette nes --retro-res 320x180
./build/bin/crossrender_example --retro ascii --scene game-pong
./build/bin/crossrender_example --post --scene 3d-postfx
./build/bin/crossrender_example --filters --scene 3d-lighting
```

## Feature map

| # | Feature | Where |
|---|---|---|
| 1 | 2D **and** 3D | `gfx/Renderer2D.*`, `gfx/Renderer3D.*`, scenes `2d-vector`, `3d-lighting` |
| 2 | Animation **and blending** | `anim/Anim.*` (crossfade, additive, masked, blend spaces, state, easing), scene `3d-animation` |
| 3 | Mixing 2D and 3D | `Engine::Step` composites a 3D pass under the 2D pass; `Camera::WorldToScreen`; scene `3d-mix-2d` |
| 4 | NanoVG-class 2D API (original code) | `gfx/Renderer2D.*` — paths, bezier/quad/arc, fills, strokes, gradients, images, clipping, transforms |
| 4a | Sprite atlases loaded from file | `gfx/SpriteAtlas.*` — TexturePacker (hash + array), Aseprite, Sparrow XML, libGDX and the engine's own JSON; multi-page, rotated/trimmed regions, pivots, animations, nine-patch and tiled drawing, plus a runtime shelf packer |
| 4b | Rich text (inner/outer colour, shadow, twist, curves) | `TextStyle` + `DrawTextStyled`/`DrawTextOnPath`/`DrawTextOnArc`/`DrawTextBoxStyled`/`DrawTextTwisted`/`DrawTextShadow`/`DrawTextGradient` |
| 4c | Slug-style GPU vector text (Eric Lengyel reference) | `gfx/SlugText.*` — quadratic outline curves in textures, banded per-pixel ray/quadratic solve, analytic coverage |
| 5 | Native windows, no GLFW/SDL | `src/platform/<os>/` — Cocoa, Win32+WGL, X11+GLX, EGL, EAGL, Emscripten |
| 6 | Lottie animations | `anim/Lottie.*` (own parser, player, renderer + procedural animations), scene `lottie` |
| 7a | Multiple TTF/OTF faces staged for testing | `examples/assets/fonts/` stages seven system faces (San Francisco, Georgia, Verdana, Andale Mono, Impact, Arial Black and a real CFF/OpenType face) and validates each with the engine's own parser; scene `fonts` shows specimens, metrics, kerning, a glyph grid and a comparison table |
| 7 | TTF/OTF text with SDF | `text/Font.*` — own `glyf` + CFF/Type2 interpreter, own rasteriser, own SDF generator, plus `GetGlyphOutline` for vector text |
| 8 | Voxels (textured and not) + LowPoly | `voxel/Voxel.*` (greedy meshing, AO, lighting, 3D-texture raymarch); `assets/Model.*` (OBJ, glTF/GLB, PLY, STL, .vox, procedural) |
| 9 | Platform code grouped by OS folder | `engine/src/platform/{macos,windows,linux,ios,android,wasm}/` |
| 10 | Music (mp3, ogg) + sounds (ogg, wav) | `audio/Audio.*` — own RIFF/WAVE parser, stb_vorbis for OGG, minimp3 for MP3, 64-voice mixer, buses, 3D audio |
| 10b | 8-bit and 16-bit chiptune music | `audio/Chiptune.*` — NES/Game Boy APU channels (pulse duty+sweep+envelope, stepped triangle, LFSR noise, wave), SPC-style sample voices with ADSR + FIR echo, a tracker format with 13 effects, five generated songs, text serialisation |
| 10c | Pixel-art and ASCII rendering | `gfx/Retro.*` — low-resolution virtual framebuffer, 12 hardware palettes, ordered dithering, scanlines, CRT curvature + aperture mask, GPU ASCII shader with 5 charsets |
| 11 | Scene system (one scene = one screen) | `scene/Scene.*`, `Engine` |
| 12 | Resolution-independent 2D layout | `ui/Ui.h` anchors, `SafeArea`, flex/grid containers; scenes `ui-layout`, `ui-widgets` |
| 13 | Build any platform from a root script | `build.sh` |
| 14 | stb allowed | `engine/third_party/` (stb_image, stb_image_write, stb_rect_pack, stb_vorbis, minimp3) |
| 15 | Full 2D widget set + 9-patch buttons | `ui/Ui.*` — button, checkbox, radio, slider, drag, text field, text area, list view, scrollbar, dropdown, combo, tabs, collapsing header, color picker, progress, spinner, tooltip, modal, toast; 9-patch in `Renderer2D::Image9` |
| 16 | Static library + giant example | target `crossrender`, app `examples/sources/` (~20 scenes) |
| 17 | Full game of **Дурак** with animation | `examples/sources/games/DurakRules.h` + `scenes/SceneDurak.cpp`, tests in `tests/test_durak.cpp` |
| 18 | WebSocket (WASM) + TCP/UDP sockets | `net/Net.*`, `src/platform/wasm/WebSocketGlue.cpp`, scene `network` |
| 19 | Tests for every feature | `tests/` (one file per module) — 30 files, **637 cases, 0 failed** (1 skipped) |
| 20 | 3D lighting and shadows | cascaded directional shadow maps, spot/point casters, PBR-lite forward shader; scene `3d-lighting` |
| 21 | 3D particle system with blending | `fx/Particles.*` — 8 presets, curves, attractors, vortex, collision, trails, sub-emitters; scene `particles` |
| 21b | Post-processing filters (blur, distort, CRT, glitch, ...) | `gfx/FilterChain.*` — 31 stackable filters with a parameter block, 10 presets, a preview grid, plus the existing bloom/tonemap chain |
| 22 | Example builds for every supported platform | `build.sh <platform>` |

## Layout

```
CMakeLists.txt              single build definition for every platform
build.sh                    root build script (all platforms + assets + run)
cmake/shell.html            WebGL shell page used by the WASM build
engine/
  include/crossrender/              public headers, one folder per module
  src/                      implementations (same folder names)
  src/platform/<os>/        native window / GL context / platform services
  third_party/              vendored single-file libraries
examples/sources/              the showcase application
  Mega.h, Mega.cpp          scene framework, asset helpers, scene registry
  main.cpp                  boot, CLI, theme wiring
  scenes/                   one file per feature scene
  games/DurakRules.h        pure Durak rules (unit tested)
tests/                      test suite (one file per module) + runner
examples/assets/          runtime assets (fonts, textures, audio, models)
```

### Modules

| Module | Header | Notes |
|---|---|---|
| Core | `crossrender/core/{Base,Math,Log,File,Json,Time}.h` | math (vec/mat/quat/colour/rect), logging, virtual file system, JSON DOM, frame clock |
| Platform | `crossrender/platform/{Window,Platform}.h` | native windows, input state machine, services, headless GL context |
| Graphics | `crossrender/gfx/{GL,Texture,Shader,Mesh,Renderer2D,Renderer3D,RenderTarget}.h` | own GL loader (no glad/GLEW), 2D vector renderer, PBR-lite 3D renderer, HDR targets, post FX |
| Text | `crossrender/text/Font.h` | TrueType `glyf` and CFF/Type2 outlines, analytic rasteriser, SDF generator, dynamic atlas, UTF-8 |
| Animation | `crossrender/anim/{Anim,Lottie}.h` | skeletons, clips, blending, state, easing, springs, tween; Lottie player |
| Audio | `crossrender/audio/Audio.h` | WAV/OGG/MP3 decoders, mixer, buses, 3D positional audio, per-OS output |
| Assets | `crossrender/assets/Model.h` | OBJ+MTL, glTF/GLB, PLY, STL, MagicaVoxel `.vox`, procedural low-poly |
| Voxel | `crossrender/voxel/Voxel.h` | chunked world, greedy mesher, AO, flood-fill light, DDA raycast, 3D-texture raymarch |
| FX | `crossrender/fx/Particles.h` | CPU simulation, instanced GPU rendering, curves, sub-emitters |
| UI | `crossrender/ui/Ui.h` | theme, anchors, flex layout, full widget set, 9-patch |
| Scene | `crossrender/scene/Scene.h`, `crossrender/Engine.h` | scene stack, transitions, engine facade |
| Net | `crossrender/net/Net.h` | TCP, UDP, RFC 6455 WebSocket, HTTP GET |
| Test | `crossrender/test/Test.h` | tiny test framework used by `tests/` |

## Documentation

Full Russian-language API documentation for every public header lives in
`docs/` — one file per header, with a description and a code example for every
public member (about 39 000 lines across 35 files).

```bash
# entry point and reading order
less docs/README.md

# check that the docs still match the headers
python3 tools/doccheck.py -v
```

`tools/doccheck.py` verifies that the text is Russian, that every member section
carries a ```cpp example, that every identifier named in a heading exists in
`engine/include`, and that every public class is documented somewhere.

## Fonts

The example's interface is set in **Ubuntu**: the theme, every scene header and
every scene control use `Assets::FontRegular()` (Ubuntu Regular at 16 px) and
`Assets::FontLarge()` (Ubuntu Bold at 48 px for headlines). **Ubuntu Mono**
remains available for monospace uses through `Assets::FontMono()`.

The example's assets are pre-generated and kept in `examples/assets/`; the
procedural generator tool has been removed from the project. Ubuntu is not a
system font on any target platform, so the family is carried in the repository
(`tools/fonts/Ubuntu-{Regular,Bold,Light}.ttf` and
`tools/fonts/UbuntuMono-{Regular,Bold}.ttf`, all under the Ubuntu Font Licence
1.0 - see `tools/fonts/README.md`); the staged copies live in
`examples/assets/fonts/`.

If an asset file is missing, the engine falls back to its built-in procedural
font and placeholders, so text always renders.

## Adding a scene

```cpp
// examples/sources/scenes/SceneMine.cpp
#include "Mega.h"

namespace mega {
namespace {
class MineScene : public MegaScene {
public:
    explicit MineScene(const char* id) : MegaScene(id, /*wants3D=*/true, "3D") {}
    void On3D(SceneContext& ctx) override { /* draw with ctx.r3d */ }
    void OnUI(SceneContext& ctx, const Rect& content) override { /* draw with ctx.r2d / ctx.ui */ }
};
}  // namespace
MEGA_SCENE(MineScene, "3d-mine", "My Scene", "3D", "What it demonstrates")
}  // namespace mega
```

The build globs `examples/sources/scenes/*.cpp`, and the main menu is generated from
the registry — the new card appears automatically.

## Tests

```bash
./build.sh test                      # everything
./build/bin/crossrender_tests ui      # filter by suite or suite.case
./build/bin/crossrender_tests --list
```

Each module has its own test file. GPU tests create an offscreen OpenGL context
and read pixels back; when no context is available they skip instead of failing,
so the suite runs in CI without a display.

## Platform notes

| Platform | Window | GL | Audio | Status |
|---|---|---|---|---|
| macOS | Cocoa `NSWindow` + `NSOpenGLContext` | 3.3 core (4.1 capable) | CoreAudio AudioUnit | built and run on the host |
| Windows | Win32 + WGL (`wglCreateContextAttribsARB`) | 3.3 core | WASAPI | source complete |
| Linux | X11 + GLX (`glXCreateContextAttribsARB`) | 3.3 core | ALSA via `dlopen` | source complete |
| iOS | UIKit + `CAEAGLLayer` + `EAGLContext` | OpenGL ES 3 | AudioUnit (RemoteIO) | source complete |
| Android | `ANativeActivity` + EGL | OpenGL ES 3 | OpenSL ES / AAudio | source complete |
| Web | Emscripten HTML5 + WebGL 2 | OpenGL ES 3 | WebAudio | source complete |

Only the platform folder matching the host OS is compiled; the others stay in the
tree, guarded by `ENG_PLATFORM_*` macros.

## Design decisions

* **One GL surface.** `crossrender::gl` declares its own types, enums and function
  pointers, so no system GL header is included anywhere and the same code targets
  GLSL 330 core and GLSL ES 3.00. `builtin::Preamble()` supplies the version and
  precision qualifiers.
* **CPU tessellation, GPU batching.** `Renderer2D` flattens paths, triangulates
  fills (ear clipping with hole bridging), expands strokes (miter/round/bevel
  joins, butt/round/square caps), emits an anti-aliasing fringe, and coalesces
  everything into draw calls keyed by (texture, paint, blend, scissor, program).
* **Original font stack.** `text/Font.cpp` parses `glyf` outlines and CFF Type2
  charstrings itself and rasterises coverage and signed distance fields without
  stb_truetype.
* **Deferred 3D submission.** `Renderer3D` records draws, then runs shadow
  cascades, sky, opaque (front-to-back), transparent (back-to-front) and debug
  lines. Custom passes such as the particle system register through
  `Renderer3D::AddPostDraw` so they composite with correct depth.
* **No exceptions, no RTTI dependency**, pimpl everywhere the platform or GL
  state is involved, and every failure path logs and degrades instead of
  crashing.

## The showcase scenes

`crossrender_example` opens on a generated main menu; every card below is a
self-registering scene in `examples/sources/scenes/`.

| Scene id | Category | Demonstrates |
|---|---|---|
| `menu` | Game | Generated launcher, animated background, category tabs, live stats |
| `2d-vector` | 2D | Paths, splines, gradients (linear/radial/box), all caps/joins, all blend modes, nested transforms, text-on-path, batching stats |
| `2d-sprites` | 2D | `Image`, `Image9`, `ImageQuad`, `ImageTinted4`, a card-atlas walk, bouncing sprites with trails and additive crossrender |
| `text` | Text | Bitmap vs **SDF** ladder 8→96 px, align/baseline grid, wrapping, ellipsis, UTF-8 (Cyrillic/Greek/CJK/emoji), outline, rotation, kinetic typography |
| `ui-layout` | UI | Phone/tablet/desktop previews, anchors, `SafeArea`, flex/grid layout, live DPI switching |
| `ui-widgets` | UI | Every widget in `Ui.h`, three themes, modal/popup/toast, keyboard focus, event log |
| `ui-ninepatch` | UI | 9-patch buttons stretched across 14 sizes, patch guides, runtime 9-patch designer |
| `ui-forms` | UI | Validated form, inline errors, JSON save/load round-trip |
| `3d-lighting` | 3D | Material grid, directional/point/spot lights, cascaded shadow maps, fog, cascade debug |
| `3d-models` | 3D | OBJ+MTL, glTF/GLB, PLY, STL, MagicaVoxel `.vox`, every primitive builder, instancing, picking |
| `3d-animation` | Animation | Skeleton, clips, crossfade, additive layers, masked blending, blend space, skinning, easing/springs |
| `3d-mix-2d` | 3D | 2D UI tracked to 3D world positions, ray picking, a 2D minimap over a 3D island |
| `3d-postfx` | FX | HDR target, bloom, four tonemap modes, FXAA, vignette, grain, chromatic aberration, presets |
| `voxel` | Voxel | Island generation, greedy vs per-face meshing, AO, sky light, textured/vertex-coloured, 3D-texture raymarch, block editing |
| `particles` | FX | All eight presets, curves, attractors, vortex, mesh emitters, ground collision, sub-emitters, blend modes |
| `fx-showcase` | FX | Particles + 3D + 2D compositing, spell state machine, projected crossrender sprites, shockwaves |
| `lottie` | Animation | Bodymovin playback, gallery, scrubbing, segments, speed, "Lottie as UI" |
| `audio` | Audio | Sound pads with waveforms, 4×8 step sequencer, mixer/buses, 3D positional audio |
| `network` | Network | TCP loopback chat, UDP ping/pong, WebSocket client, background HTTP GET, interpolated remote players |
| `durak` | Game | Complete **Дурак** card game: rules, AI, animated cards, sounds, Lottie game-over |
| `retro-display` | FX | Pixel-art and ASCII modes: mode/resolution/palette switching, dithering, scanlines, CRT curvature, aperture mask, live stats |
| `post-filters` | FX | Filter chain editor: add/remove/reorder/enable 31 filters, live parameters, 10 presets, preview grid |
| `text-fx` | Text | Inner/outer gradients, outlines, soft shadows, crossrender, twisting, text on arcs and paths, combinations |
| `chiptune` | Audio | 8-bit vs 16-bit chip playback, a live tracker view, channel mute/solo, waveform scope, song switching |
| `game-snake` | Game | Playable Snake in pixel-art style with particles, shake and a persisted high score |
| `game-breakout` | Game | Playable Breakout with power-ups, ball trails, screen shake and a "juice" toggle |
| `game-pong` | Game | Pong drawn entirely in ASCII, two-player and three AI levels |
| `game-roguelike` | Game | Turn-based ASCII dungeon: shadowcast FOV, monsters, items, inventory, stairs |
| `game-platformer` | Game | Side-scrolling platformer: coyote time, wall jumps, moving platforms, enemies, HUD |
| `game-space-shooter` | Game | ASCII starfield shooter with waves, a boss and power-ups |
| `game-mathlogic` | Game | **Number Logic (KenKen)**: 105 levels across 3x3..9x9 and five tiers, every one generated and proved to have exactly one solution; cage arithmetic with + - x /, cage outlines, pencil marks, hints with explanations and a solver-backed attract mode |
| `game-flight` | Game | Low-poly 3D dogfight: arcade flight model with stall, guns/missiles with lock-on, four enemy types and a boss ace, low-poly island terrain, ring gates, 10 wave missions, full cockpit HUD with radar and target boxes |
| `game-match3` | Game | **Hex** match-3: 61-cell axial board, rotating a triangle of three mutually-adjacent hexes as the headline move, matches along all three hex axes, beam/bomb/prism specials with hex-specific combos, 14 levels plus a timed endless mode |
| `game-crossword` | Game | Crossword: six derived puzzles (5x5 to 13x13) with clue lists, on-screen keyboard, checks/hints/reveal, timer and auto-solving attract mode |
| `earth` | 3D | Rotating Earth: procedurally generated 2048x1024 continents with height-driven normal mapping, biome colouring, polar ice, a specular sea, a separately rotating cloud shell, a fresnel atmosphere, night-side city lights, a bloomed sun, a starfield and a cratered moon |
| `atlas` | Graphics | Loads the same sheet from five descriptor formats, draws regions/animations/nine-patches/tiling, and packs an atlas at runtime |
| `fonts` | Text | Every staged TTF/OTF face: specimens at 7 sizes, kerning, metrics, a glyph grid and a cross-font comparison table |
| `game-minecraft` | Game | Voxel sandbox: generated terrain, break/place blocks, day/night, hotbar, minimap, saved worlds |
| `game-doom` | Game | Doom-2-style raycaster: textured walls, floor/ceiling, depth-sorted sprites, doors, keys, weapons, HUD |
| `game-ascii` | Game | ASCII action RPG in the Steam-ASCII tradition: character-grid world, animated ASCII sprites, biomes, boss |
| `3d-robot` | 3D | Low-poly dancing robot: primitive-built body on a real skeleton, three dances, beat-synced additive layers, disco stage |

## Verification status

Last full run on the host (macOS arm64, Apple M4 Pro):

```
./build.sh --clean          # from-scratch build: 0 errors, 0 warnings
./build/host/bin/crossrender_tests
                            # 637 passed, 0 failed, 1 skipped  (52 suites)
./build/host/bin/crossrender_example --list
                            # 44 scenes registered
```

All 44 scenes were also rendered headlessly (`--headless --frames 240 --screenshot`)
and inspected: every one produces a non-empty, correct image, in pixel and ASCII
modes too (`--retro pixel|ascii`).

## Known limitations

Honest status of the parts that are implemented but not exercised on this host,
or that are deliberately simplified:

* **Platform coverage.** Only the macOS backend is built and run here. The
  Windows (Win32/WGL), Linux (X11/GLX), iOS (UIKit/EAGL), Android (EGL) and Web
  (Emscripten/WebGL 2) backends are complete and were syntax-checked with
  stub headers; iOS also compiles against the real iPhoneOS SDK. None of them
  has been linked or launched on a real device in this environment.
* **`--screenshot` in windowed mode** reads the back buffer after present, which
  some drivers return stale; headless capture (`--headless`) is exact and is what
  the example's own screenshots use. Post-processing works with `--post`.
* **MSAA.** The window requests 4x MSAA and falls back automatically when the
  driver refuses it. `Renderer2D` adds its own analytic anti-aliasing fringe on
  fills, so 2D stays smooth either way.
* **Post-processing** needs `--post`; scenes that would like bloom report this in
  their UI instead of faking it.
* **Slug-style text** renders glyphs analytically from their outlines with no
  atlas, but it is a faithful re-implementation of the *technique*, not of the
  Slug library: glyphs are batched in runs of eight with per-instance uniforms
  instead of Slug's packed vertex records, the drop shadow is a small ring of
  jittered copies rather than a real blur, and there is no depth pre-pass for
  overlapping glyphs. The curve/band data lives in two `RGBA32F` textures
  (`R32F` samples as zero on some GL 3.2 drivers).
* **ASCII mode** converts the *whole* frame, UI included, which is faithful to how
  a character-grid display works but makes dense widget panels hard to read. The
  ASCII games (Pong, roguelike, space shooter) draw their own character grids and
  stay legible.
* **Lottie** parsing and rendering are complete: shapes, gradients, trim paths,
  masks, mattes, text, precomps, easing and spatial tangents, with After Effects
  semantics for transform order, front-to-back layer stacking, style/geometry
  ordering, solid layers and per-property defaults. The six generated sample
  animations render correctly and are covered by a pixel-level test.
* **Voxel raymarch** step count is fixed at 256 (`uMaxSteps`); the scene exposes
  volume resolution instead.
* **Renderer3D shadows** are cascaded directional maps; spot/point casters are
  rendered into the same maps, and a point light gets a single-face
  approximation (documented in `Renderer3D.cpp`).
* **`WebSocket` on native** supports `ws://`; `wss://` needs a TLS backend that
  is not vendored. WASM uses the browser's implementation and supports both.
* **MP3/OGG assets** are decoded by the engine, but no encoder is available here,
  so the generated audio is WAV; drop `.ogg`/`.mp3` files into `assets/audio/`
  and the same code paths load them.

## Licence

The engine code in this repository is original. Vendored third-party files live in
`engine/third_party/` and keep their own licences (stb: public domain / MIT,
minimp3: CC0).
