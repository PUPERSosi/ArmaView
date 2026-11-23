# Icons mode: BI's standards for UI pictures

Measured on the `scope = 2` classes of P:\a3 (1394 config.cpp with their includes, 11 613 classes): the effective value
of each UI picture field (through inheritance), the file's header size and format, and the opaque pixels of the picture
(alpha > 24): their bounding box as a fraction of the picture (fill) and the free border on each side. Medians per kind.
The presets of `src/core/IconStandards.cpp` use these numbers. This page holds numbers and config references only:
no BI picture is copied into the repository.

| Kind | Field | Size | Background | Fill (w x h) and border | View | Name, folder |
|---|---|---|---|---|---|---|
| Rifle / MG / DMR | picture | 1024 x 512 or 512 x 256 (2:1), DXT5 | transparent, colour render | ~90 x 60 %; L 5, R 3, T 21, B 19 % | side, muzzle left | `Data\UI\gear_<weapon>_X_CA` or `icon_<class>_X_CA` |
| Pistol | picture | 1024 x 512 / 512 x 256 | transparent | 41 x 56 % | side, muzzle left | `gear_<pistol>_X_ca` |
| Launcher | picture | 512 x 256 | transparent | 94 x 53 % | side | `gear_ / icon_..._ca` |
| Uniform | picture | 256 x 256 (some 512 / 1024) | transparent | 63 x 96 %, full height | front, no body | `Data\UI\icon_<class>_ca` |
| Vest | picture | 256 x 256 | transparent | 62 x 94 % | front | `icon_<class>_ca` |
| Helmet, headgear | picture | 256 x 256 | transparent | 78 x 81 %, border ~10 % | 3/4 front-left, a little above | `icon_<class>_ca` |
| Backpack | picture | 256 x 256 | transparent | 62 x 87 % | front | `icon_<class>_ca` |
| Glasses | picture | 256 x 256 | transparent | 64 x 70 % | front | `icon_g_*_ca` |
| Optic | picture | 256 x 256 | transparent | 85 x 45 % | side, objective left | `icon_<class>_ca` |
| Muzzle | picture | 256 x 256 | transparent | 85 x 26 % | side | `icon_<class>_ca` |
| Magazine | picture | 512 or 256 square | transparent | 64 x 74 % | side, tilted | `gear_ / m_ / icon_..._ca` |
| Vehicle | picture | 128 x 64 | white silhouette | ~97 x 80 %, on the bottom edge | side, nose left | `Data\UI\<vehicle>_ca` |
| Vehicle | icon (map) | 512 x 512 | white silhouette with lines | cars ~40 x 95 %, helicopters 73 x 89 % | from above, nose up | `Data\UI\map_<vehicle>_ca` |
| Weapon type | uiPicture | 32 x 32 | white | 69 %, border 16 % | symbol | `weapons_f\data\UI\icon_regular_CA` ... |
| Anything in Eden | editorPreview | JPG 455 x 256 (some DLC 410 x 256) | grey VR scene, ground and shadow | BI's script, below | below | `\A3\EditorPreviews_F[_DLC]\Data\CfgVehicles\<Class>.jpg` |

Examples (config file:line):
- MX `uiPicture`: `P:\a3\weapons_f\rifles\mx\config.cpp:292`
- Zafir: `P:\a3\weapons_f\machineguns\zafir\config.cpp:77`
- Pistol Heavy 01: `P:\a3\weapons_f\pistols\pistol_heavy_01\config.cpp:61`
- NLAW: `P:\a3\weapons_f\launchers\nlaw\config.cpp:33`
- Uniform: `P:\a3\characters_f\config.cpp:25896`, vest `:29016`, helmet `:30430`, glasses `:33288`
- Optic and muzzle: the accessories config of Weapons_F (`acc` folder), `config.cpp:345` and `:75`
- APC picture and map icon: `P:\a3\armor_f_beta\apc_tracked_01\config.cpp:3630-3631`
- Helicopter: `P:\a3\air_f\heli_light_01\config.cpp:936-937`
- Character preview: `P:\a3\characters_f\config.cpp:3484` (B_Soldier_F)

## BIS_fnc_exportEditorPreviews

`P:\a3\functions_f\diagnostic\fn_exporteditorpreviews.sqf`:
- parameters (27-32): delay, objects ("all" / "vehicles" / "props"), sides, mods, addons, classes
- classes (116-152): `scope 2`, not `Bag_Base` / `WeaponHolder` / `LaserTarget`
- camera: bearing 135 deg, 15 deg up (308-309); helipads 90 / 75 (311-315); distance `1.5 x sizeOf`, 0.2 .. 124 m (374);
  the narrowest field of view (0.1 .. 0.7, step 0.01) that keeps every bounding box corner in the picture (409-422);
  characters: the torso, target `[0,0,1.25]`, field of view 0.075 (384-388)
- the object: vehicles `setDir 90`, others 270 (319); poses with a rifle / pistol / unarmed (320-329)
- background: `Sphere_3DEN` with ground or `SphereNoGround_3DEN` when the object reaches far down, colour
  `color(0.93,1,0.98,0.1)` (216-227, 368); aperture 45 (229); date 2035-05-28 10:00 (230)
- output: `Screenshots\EditorPreviews\<DLC>\<Class>.png` (301, 427); the 455 x 256 JPG is made outside the game

The Icons window's "SQF for the game" gives the call for the mod's own CfgVehicles classes, e.g.
`[1, "all", [], [], [], ["My_Class_1", "My_Class_2"]] spawn BIS_fnc_exportEditorPreviews;` (it sleeps: `spawn`).

## The mode

- presets: editor preview, weapon, gear, headgear, attachment, vehicle silhouette, map icon, custom; the class of the
  open model (MLOD or glTF) from the config picks one (`SuggestIconPreset`)
- weapons / attachments: the view across the barrel with the muzzle on the left, from the memory points
  `muzzlePos` -> `muzzleEnd` of the weapon's config (else the longer horizontal axis)
- the item is rendered offscreen (supersampled, transparent), cut to its opaque bounds and fitted into the picture with
  the preset's border (`FitToFrame`); exposure, saturation, contrast, outline, white silhouette
- PNG / JPG by WIC, PAA by `ImageToPAA.exe` of the Arma 3 Tools (found in the Steam libraries, settings block `icons`)
- file name and config line by BI's rules; a batch over the mod's classes with a model on disk; characters: the class
  dressed by the Unit system (the Samples' ArmaMan with the unit's gear)
- ui-script: `icon <preset> <file.png|.jpg|.paa> [yaw pitch]` (an Error when the picture breaks the preset)

### Per class (Icons 1-6)

- a class of the mod shows its own model and re-texture (`IconSubjectOf`: uniformClass soldier / ItemInfo uniformModel
  / model; hiddenSelections* as a config skin, `""` hides the section)
- category (`IconCategory`, 15) by inheritance with the base class shown, or picked by hand; BI's examples of the
  category (`BiExamples`: read from the user's game configs on P: while the viewer runs, those of the standard's
  size first; no BI picture is part of the repository) with arrows
- camera: yaw / pitch / roll, zoom, target offset, mouse in the preview; kept per class with the category, fitting
  and the icon's own hiding of model parts (`ModelVisibility::Snapshot`, outliner window "Model parts (icon)")
- "BI standards" table; the file name in parts (`IconNameOf`); the pictures already in the mod's config checked
  (`CheckConfigPictures`: missing file, size, `_co` / no `_ca` / no alpha, folder, PAA power of two) in the panel and
  in Validation
- ui-script: `iconclass`, `iconcam`, `iconsave`, `iconstate`, `iconhide <selection>|reset`, `iconpics [class]`
