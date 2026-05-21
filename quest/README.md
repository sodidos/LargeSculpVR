# Large SDF Quest

Squelette Android natif pour porter le prototype SDF sur Meta Quest 3 sans
Unity. Cette cible reutilise le coeur C++ dans `src/core` et demarre par une
`NativeActivity` OpenXR.

Etat actuel :

- projet Android/Gradle minimal ;
- manifest VR Quest 3 / Quest 3S ;
- suivi des mains Quest declare en option dans le manifeste ;
- entree native C++ chargeant le volume SDF initial ;
- initialisation OpenXR, session VR, espace local et swapchains stereo ;
- rendu OpenGL ES par oeil avec raymarching direct d'une texture SDF 3D ;
- loader OpenXR Android recupere par Gradle via Prefab ;
- scripts PowerShell pour verifier l'environnement, builder et installer.

Ce palier valide la chaine Android + NDK + OpenXR sur le casque, avec une
surface SDF visible en stereo, les outils de sculpture de base et les premiers
controles manettes/mains.

## Outils a installer

Sur cette machine, la chaine suivante a ete installee :

- Android Studio `2025.3.4.7` ;
- JDK 17 : `C:\Program Files\Eclipse Adoptium\jdk-17.0.19.10-hotspot` ;
- Android SDK : `E:\Android\Sdk` ;
- Android NDK : `E:\Android\Sdk\ndk\27.0.12077973` ;
- CMake Android : `E:\Android\Sdk\cmake\3.22.1` ;
- Gradle : `E:\Tools\gradle-8.10.2` ;
- `adb` : `E:\Android\Sdk\platform-tools\adb.exe`.

Ces chemins sont aussi configures dans les variables utilisateur
`JAVA_HOME`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `ANDROID_NDK_HOME` et dans le
`PATH`. Si un terminal deja ouvert ne les voit pas, le fermer puis le rouvrir.

## Installation manuelle equivalente

Installer :

1. Android Studio.
2. Dans le SDK Manager : Android SDK Platform 32, Android SDK Platform-Tools,
   Android SDK Build-Tools, NDK (Side by side) et CMake.
3. Les pilotes Oculus ADB si Windows ne detecte pas le Quest.

Le premier build n'a pas besoin d'un SDK OpenXR local : Gradle recupere
`org.khronos.openxr:openxr_loader_for_android:1.1.53`. Le SDK Meta OpenXR reste
utile comme reference et pour les extensions Meta plus avancees.

Variables utiles :

```powershell
[Environment]::SetEnvironmentVariable("ANDROID_HOME", "$env:LOCALAPPDATA\Android\Sdk", "User")
[Environment]::SetEnvironmentVariable("ANDROID_NDK_HOME", "$env:LOCALAPPDATA\Android\Sdk\ndk\<version>", "User")
```

Ensuite, rouvrir PowerShell pour recharger le PATH.

## Verification

Depuis ce dossier :

```powershell
.\check_quest_env.ps1
```

Si Windows bloque l'execution des scripts PowerShell :

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\check_quest_env.ps1
```

Le script verifie Java, Gradle, Android SDK, Android API 32, NDK, CMake et
`adb`. La dependance OpenXR est resolue par Gradle.

## Build

```powershell
.\build_quest.ps1
```

Ou ouvrir `quest/` dans Android Studio et lancer `app`.

## Installation sur Quest

Activer le mode developpeur dans l'application Meta Horizon, brancher le Quest
3 en USB-C, accepter le debogage USB dans le casque, puis :

```powershell
.\install_quest.ps1
```

L'APK debug attendu est :

```text
quest/app/build/outputs/apk/debug/app-debug.apk
```

Si le Quest apparait en etat `unauthorized`, accepter la demande "USB
debugging" dans le casque, puis relancer :

```powershell
adb devices
.\install_quest.ps1
```

Quand cette version se lance correctement, le casque doit afficher une forme SDF
couleur argile devant l'utilisateur, sur un fond sombre identique dans les deux
yeux. La visualisation ne passe plus par un mesh triangule, afin d'eviter les
trous visuels pendant les premiers tests VR.

Interaction actuelle :

- main droite : viser la surface SDF ;
- zone coloree : position du pinceau, avec une couleur par outil ;
- gachette droite : appliquer l'outil actif ;
- A/B : outil suivant / precedent ;
- outils actuels : Add, Subtract, Smooth, Stretch ;
- Stretch : en mode Stretch, la gachette droite pose la zone attrapee, puis le
  mouvement de la manette droite deplace/etire cette zone jusqu'au relachement ;
  relacher la gachette libere l'accroche, le prochain appui choisit une
  nouvelle zone ;
- joystick gauche : gauche/droite regle l'intensite, haut/bas regle la taille
  du pinceau de l'outil actif ; chaque outil conserve ses propres reglages ;
- X/Y : undo / redo ;
- bouton menu gauche : affiche le panneau minimal outils + SAVE / LOAD /
  EXPORT / QUIT devant la manette gauche ;
- HUD spatial devant la manette gauche : nom du pinceau actif, taille verticale
  et puissance ; le rendu actuel a ete remis dans une passe separee du shader
  SDF afin de ne plus perturber l'objet ;
- representation de l'outil pres de la manette droite : manche court + tete
  coloree selon l'outil actif, rendue separement de la surface SDF ;
- quand le menu est ouvert, le rayon/gachette droite reste l'interaction de
  selection avec les manettes ;
- grip gauche ou droit : deplacer et tourner l'objet dans l'espace de travail ;
- grip gauche + grip droit : zoomer/changer l'echelle de l'objet autour des
  deux mains, avec rotation selon l'axe entre les mains ;
- grip gauche + gachette droite : tenir l'objet avec la main gauche tout en
  sculptant avec la main droite ;
- poing gauche, si le suivi des mains est disponible : equivalent au grip pour
  deplacer et tourner l'objet ;
- pincement gauche + deplacement horizontal : zoom/echelle de l'objet, vers la
  gauche agrandit et vers la droite reduit ;
- clap des deux mains ouvertes : passe a l'outil suivant dans l'ordre Add /
  Stretch / Subtract / Smooth ;
- pincement main droite pres de la surface : applique l'outil actif a la
  position pincee ; Stretch tire directement la zone pincee ;
- les mains suivies sont visualisees dans la passe UI separee sous forme de
  squelette/capsules sans glow ; la main droite prend la couleur de l'outil
  actif ;
- la texture SDF 3D est re-uploadee au GPU apres chaque coup de pinceau.

## Mapping VR prevu

- Main droite : rayon/pinceau de sculpture.
- Gachette droite : appliquer l'outil actif.
- Boutons A/B : outil suivant/precedent.
- Main gauche : manipuler la sculpture ou la vue.
- Joystick gauche horizontal : intensite du pinceau de l'outil actif.
- Joystick gauche vertical : taille du pinceau de l'outil actif.
- Chaque outil conserve sa taille et son intensite quand on change d'outil.
- Grip gauche ou droit : translation + rotation de la sculpture.
- Deux grips : zoom/echelle de travail + rotation autour de l'axe des mains.
- Zoom autorise : 0.05x a 10.00x.
- Grip droit : manipulation de l'objet, y compris en mode Stretch.
- Gachette droite en stretch : poser l'ancre puis tirer la zone avec influence
  proportionnelle ; relacher libere l'ancre pour pouvoir en choisir une autre.
- X/Y : undo/redo.
- Bouton menu gauche : ouvrir/fermer le menu outils/save/load/export/quit,
  affiche avec le HUD devant la manette gauche.
- Menu : rayon droit et gachette droite avec les manettes.
- Le HUD, le menu et la representation d'outil sont rendus dans une passe UI
  transparente separee du raymarching SDF, pour eviter de casser la surface.
- Main droite : representation spatiale de l'outil a la position de la manette.
- Poing gauche : grab de l'objet via suivi des mains.
- Pincement gauche + deplacement horizontal : zoom/echelle de l'objet.
- Clap des deux mains ouvertes : outil suivant.
- Pincement droit sur la surface : applique l'outil actif via suivi des mains.

Ce mapping est documente plus largement dans `docs/interface_vr_v0.md`.
