# Changes

## 1.1.0

New:
- **Icons mode** (Ctrl+8): UI pictures of the mod's classes by Bohemia's standards. A class of the mod shows its own
  model and re-texture; its category comes from inheritance or is picked by hand; presets for editor previews,
  weapons, gear, headgear, attachments, vehicle silhouettes and map icons with BI's sizes, borders, views and file
  names; fitting (exposure, saturation, contrast, outline, white silhouette) and a camera kept per class; model parts
  hidden for the picture only; PNG / JPG / PAA (ImageToPAA of the Arma 3 Tools); the config line; a batch over the
  mod's classes; the SQF call of BIS_fnc_exportEditorPreviews for the mod's CfgVehicles classes.
- BI's own pictures of the category beside the preview, read from the game data on P: while the viewer runs (nothing
  of BI's is shipped); the "BI standards" window; docs/icons.md with the measured numbers and config references.
- The pictures a mod's config already names are checked against their category's standard (missing file, size,
  `_co` / `_ca`, alpha, folder, PAA power of two) in the Icons window and in Validation.
- Only source models are pictured: a binarized model or unit part gets the standard message, and the Icons window lists
  the unit parts it cannot show.
- `--no-game`: the viewer does not read the game configs on P: (Arma 3 Samples and own files only).
- ui-script: `icon`, `iconclass`, `iconcam`, `iconsave`, `iconstate`, `iconhide`, `iconpics`; selftest and coretest
  steps for the mode (Arma 3 Samples and files written by the test).

## 1.0.4

Fixes:
- An empty string in `hiddenSelectionsTextures` hides the faces of that hidden selection, as in the game (and as
  `setObjectTexture [i, ""]`); before, they were drawn with the model's own texture. An index past the array keeps the
  model's texture; an empty `hiddenSelectionsMaterials` entry changes nothing. Hidden faces are left out of drawing,
  shadows, framing, picking and the section counts; the Materials window marks such a selection "hidden by config".
- A class whose re-texture entries are all empty is listed as a config skin (it hides selections); the default skin
  is still the first class that sets a texture or material.
- Unit / outfit parts: the same rule for the hidden selections of the class wearing a part.

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
