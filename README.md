# Large SDF

Prototype C++ natif pour une application de sculpture VR type Medium, ciblee a
terme pour Meta Quest 3 via Android NDK + OpenXR.

Le projet commence volontairement par le noyau de modelisation :

- volume SDF sur grille 3D ;
- pinceau sphere add/subtract ;
- export OBJ blocky pour verifier la forme sans moteur graphique ;
- tests console simples.

Unity n'est pas utilise. Le but est de garder un coeur portable et performant,
testable sur PC avant integration OpenXR/Vulkan ou OpenGL ES sur Quest.

## Compiler sous Windows

Visual Studio Build Tools 2022 est deja detecte sur cette machine. Lancer :

```bat
build_windows.bat
```

Ou manuellement :

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
cmake -S . -B build-vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build-vs2022 --config Debug
build-vs2022\Debug\large_sdf_tests.exe
build-vs2022\Debug\large_sdf_demo.exe
```

La demo ecrit un mesh OBJ dans :

```text
out/sdf_demo_blocky.obj
out/sdf_demo_smooth.obj
```

## Preview interactive

Apres compilation, lancer :

```bat
build-vs2022\Debug\large_sdf_gpu_preview.exe
```

Cette preview est la version principale pour sculpter. Elle utilise Direct3D 11 :
le volume SDF est envoye au GPU comme texture 3D et la surface est rendue par
raymarching dans un pixel shader. Le resultat se met a jour pendant le geste.

Commandes GPU :

- souris gauche sur la surface : sculpter en 3D ;
- clic droit + mouvement souris : tourner la vue ;
- `Ctrl` + clic droit + mouvement souris : zoomer la vue ;
- molette : changer la taille du pinceau ;
- `1` : outil add ;
- `2` : outil subtract ;
- `3` : outil smooth ;
- `6` : outil stretch/deplacement proportionnel ;
- `4` / `5` : baisser/augmenter la precision voxel ;
- `+` / `-` du pave numerique : force de l'outil actif ;
- `Z` : undo ;
- `Y` : redo ;
- `P` : exporter `out/gpu_interactive_sculpt.obj` ;
- `R` : recommencer depuis une sphere ;
- `Echap` : quitter.

L'ancienne preview CPU/GDI existe encore pour debug :

```bat
build-vs2022\Debug\large_sdf_preview.exe
```

Commandes CPU/GDI :

- souris gauche dans la vue 3D : sculpter directement sur la surface ;
- souris gauche dans la coupe 2D : sculpter sur la profondeur active ;
- clic droit + mouvement souris : tourner la vue 3D ;
- `Ctrl` + clic droit + mouvement souris : zoomer la vue 3D ;
- molette : changer la taille du pinceau ;
- `Page Up` / `Page Down` : changer la profondeur de coupe ;
- `1` : outil add ;
- `2` : outil subtract ;
- `3` : outil smooth ;
- `6` : outil stretch/deplacement proportionnel ;
- `4` / `5` : baisser/augmenter la precision voxel ;
- `+` / `-` du pave numerique : force du smooth ;
- `Z` : undo ;
- `Y` : redo ;
- `W` : afficher/masquer le filaire de surface ;
- `P` : exporter `out/interactive_sculpt.obj` ;
- `R` : recommencer depuis une sphere ;
- `Echap` : quitter.

Cette preview ne remplace pas la VR. Elle sert a caler vite les outils, le
comportement du SDF et la logique undo/redo avant de brancher OpenXR.

## Cible Quest VR

Un premier squelette natif Quest est disponible dans :

```text
quest/
```

Il utilise Android NDK + OpenXR, sans Unity, et reutilise le coeur SDF du dossier
`src/core`. Sur cette machine, l'outillage Android/Quest n'est pas encore
installe dans le PATH ; lancer le diagnostic :

```powershell
cd quest
.\check_quest_env.ps1
```

Puis, une fois Android Studio, le SDK/NDK, `adb` et Gradle installes :

```powershell
.\build_quest.ps1
.\install_quest.ps1
```

Le mapping VR prevu est documente dans `docs/interface_vr_v0.md`.
