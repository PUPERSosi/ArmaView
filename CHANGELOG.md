# Changes

## 1.0.3

Fixes:
- Texture streaming no longer stops for good when its worker threads all finish at once: missing workers are started
  again, a model opened right after another one gets its textures (the viewer could wait for them forever).
- Command-line options (`--lod`, `--select`, `--highlight`, `--isolate`, `--focus-bone`, `--skin`, `--section`,
  `--compare-rtm` and others) are applied to the last model given, after it has loaded; `--screenshot` waits until the
  model, its textures and the options are in place. The comparison animation follows a model change.
- Translated labels with an id (`Stop##rec`) find their own translation first; `override` / `blend` translated.
- Cancelling a recording removes the unfinished MP4 / GIF and the ffmpeg log; a failed frame stops the recording as
  failed, not as finished.
- Mod prefixes come from one place for every check: `$PBOPREFIX$`, then a prefix set by hand (settings block
  `modprefix`: `<mod folder> = <prefix>`), then the paths in the mod's config, then the folder name.
- The RTM folder check runs in the background with progress and Cancel; reloading one RTM re-checks only that file.
- A model still being written while it is hot-reloaded logs no errors until the last attempt.

Also in this version (from the unreleased 1.0.2 work): the view logic (input, camera, picking, control mode, point
overlay settings) is separate from the ImGui windows, so another UI can embed the viewer - see `docs/embedding.md`.

Checks added: a texture streaming stress test (viewer coretest), `tools/tests/cli_options.py` (command-line options),
a recording cancel step and a 120-file RTM folder check in the selftest.
