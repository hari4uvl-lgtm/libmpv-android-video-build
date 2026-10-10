# Complete music graph PC renderer

B4 diagnostic tooling only. No Android library/APK is built or installed. The
isolated workflow produces Linux and Windows FFmpeg 6.0 executables with pinned
SoX 0.1.3 and the exact shipping `lumendsp` / `lumenout` C sources. The source
manifest rejects stale or experimental limiter implementations.

The old `b2-pc.yml` artifact had no SoX or `lumendsp`, so it could not qualify the
whole production chain. The desktop FFmpeg used by earlier partial tests also
has no custom filters. A combination of those tools is useful evidence but is
not the same as running negotiated native filters in one graph.

## Build and smoke check

The workflow must be reviewed before pushing its dedicated branch, because that
narrow branch/path push trigger starts the initial build. Manual reruns are also
available once the workflow is registered. Sources are
FFmpeg `n6.0` / `ea3d24bbe3c58b171e55fe2151fc7ffaca3ab3d2` and the SoX archive
whose SHA-256 is recorded in `build.sh`. Compiler/package/config versions and
all shipping-source checksums are included in the artifact. MinGW cross-build
follows the existing PC workflow. SoX OpenMP is disabled as in Android. No
fast-math flags are added; no Android or native algorithm source is changed.
The source manifest uses Git's LF normalization: the Windows checkout's raw
limiter-core SHA-256 `312c7a4724f83a615443bd90c796030358013144ed2da3072da85d2c8e0b5df2`
is the same source as LF hash
`4420b41161af6131b0dbb2b224db6df87e5ea8438641f48b53ce2540d439ec7b`.
No source file is rewritten to obtain that normalized manifest. Host C checks
cannot establish bit-identical ARM64 machine output by themselves.

On a fresh Ubuntu checkout run `TARGET=linux bash music-full-chain-pc/build.sh`
or `TARGET=windows ...`. Do not run both into the same artifact directory.
On Windows, after downloading the Windows artifact, run:

```text
python smoke.py --ffmpeg path/to/ffmpeg.exe --output path/to/new-smoke-folder
```

`smoke.py` uses generated samples only and the real stereo and output filters.
It checks the neutral impulse's position/flush, finite complete output and basic
sample-peak protection on full/f32 graphs. It records the full/light residual
without treating it as a quality pass. It does not change any app preference.

## Next qualification gate

Use the actual app's `MusicEqualizer.liveChain` arguments as fixture metadata;
keep the graph definitions checked against that source. Run paired full/light
44.1/48/96 kHz synthetic fixtures, asymmetric stereo, shelves and active wideband
limiting. Include startup and flush. Require finite equal-size outputs and an
absolute maximum residual at or below -100 dBFS, or document a separately
justified audibility exception. Use an independent 8x true-peak measurement.
Separate SoX20/f64 and SoX28/f32 comparisons can attribute any failure.

The existing partial-chain result failed eight of nine strict null cases.
Automatic light remains disabled. Passing this tool's smoke tests does not
qualify light, CPU, battery, acoustic output or Phase C. Do not enable light on
either owner's phone. No private media belongs in this repository or artifacts.
