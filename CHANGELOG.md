# Changelog

## v1.2

Fixes found with a real Wii U, which the emulator never showed:

- **Black blocks and striped pictures in games that draw into bitmaps** (Moto X3M's beach) and **Digger falling through its map**: the CPU's cache could still hold an old copy of a picture the GPU had just drawn into, and a partial update wrote that old copy back. Pictures now drop the cached lines after every GPU draw, and get a cleared stencil buffer when they first become a drawing target.
- **Pixel Bender shaders losing a colour channel**: the console's shader compiler kept a stale lane when a shader read a literal number out of its register file. The generated GLSL now carries the literals inline. Shader blend modes match Ruffle's output.

Faster:

- Bitmap fills and new-picture clears use `dcbz` (no read of the memory being overwritten), and fully written textures skip the clear.
- `f32/f64.nearest` no longer calls libm.
- The audio mixer resamples in integer arithmetic instead of f64 per sample.
- `BitmapData.perlinNoise` runs natively on all three cores.
- The garbage collector runs less often, and a full collection is forced when memory is nearly full.

New:

- Wii Remotes (and Nunchuk / Classic / Pro Controller) type on the system keyboard.
- Stand-ins for online services: Armor Games (AGI), Kongregate, CPMStar and gunblood.com ads. Games that waited for those files now start.
- Text engine: font metrics from the font file (`getFontMetrics`), right-to-left runs (Hebrew), mirror regions with real mouse events, fi/fl ligatures, joined Arabic letters (with an embedded Arabic font).
- Happy Wheels loads its levels (BlendMode.INVERT fix), Gunblood draws its shadows (invalid filter rectangle fix).
- H.264 video decodes through the console's decoder with up to two live decoders.

## v1.1

- Games saved together with their website (a folder with the web page) are found through that page and get their FlashVars.
- H.264 video, Stage3D mipmaps, ATF LZMA / JPEG-XR textures.
- Text engine: line wrapping, mixed formats, baselines, justification, inline graphics.
- Theme music: Cipher (Classic), Sneaky Snitch (Adobe Flash), Not As It Seems (Halloween).

## v1.0

- First release: Flash games on the Wii U, with the game grid, folders, three themes and the on-screen keyboard.
