# Isolated music qualification experiments — 2026-10-09

These folders are test hosts, not the production native library configuration.
`buildscripts/patches` and `music-b2-pc` retain production commit
`5863b1641db905fd1953eeb035ca7caadd3156de` behaviour. Do not copy an experimental
source into a shipping build merely because its safety checks passed.

- `music-band-budget-pc`: bass allowance floors at 25/50/75/100 percent.
  Peak and sanitizer checks passed; dynamic vocal modulation exceeded the
  required 0.5 dB. Rejected for deployment.
- `music-band-paired-pc`: allowance derived from simultaneous low/high samples,
  with 0/0.5/1 dB extra reserve. The first two passed synthetic vocal stability
  (0.184/0.446 dB); all passed sanitizer/runtime state checks. Private real-media
  tests found substantial loss of bass attack against current wideband playback.
  Rejected for deployment. No private audio or source media is in this repo.
- `music-hold-pc`: compile-time `LUMEN_PEAK_HOLD_MS`, default 25, trials 15/5/0.
  Existing lookahead, detector, ceiling and release remain unchanged. Local
  runtime state and all initial limiter/tail/chunk/peak gates passed, including
  additional 20/31 Hz distortion/ripple tests. Real-media improvements were small
  and attack contrast sometimes worsened; not promoted. No dedicated CI
  sanitizer claim is made for this last folder.

Production continues using the original wideband limiter and original extra
25 ms hold. Optional two-band protection remains off by default. Improving its
bass balance is unfinished; these experiments do not establish a quality cure.

Windows qualification used pinned official Zig 0.15.2 Clang, baseline CPU,
`-O2 -ffp-contract=off`, without fast-math. Unchanged wideband PCM matched the
previous reference below -269 dBFS. CI Linux GCC sanitizer tests are separate.
The compiler and all private waveform fixtures stay outside this Git repository.
