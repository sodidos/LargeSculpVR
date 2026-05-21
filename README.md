# LargeSculpVR

Application native de sculpture SDF pour Meta Quest 3.

Le projet vise une experience VR directe sur casque, sans Unity, avec un coeur
C++ portable et une interface OpenXR optimisee pour les manettes Touch et le
suivi des mains.

## Objectif

- sculpture volumique SDF en temps reel ;
- rendu stereo OpenGL ES par raymarching d'une texture 3D ;
- outils Add, Subtract, Smooth et Stretch ;
- manipulation de l'objet avec les grips ;
- HUD/menu spatial devant la manette gauche ;
- support experimental du suivi des mains Quest ;
- export OBJ de la sculpture.

## Structure

```text
quest/      Application Android native Quest 3
src/core/   Moteur SDF, historique, export OBJ, maillage de surface
src/tests/  Tests console du coeur SDF
src/tools/  Petit outil console pour generer des volumes de test
docs/       Notes d'interface VR
```

## Build Quest

Depuis `quest/` :

```powershell
.\check_quest_env.ps1
.\build_quest.ps1
.\install_quest.ps1
```

Le casque doit etre en mode developpeur, connecte en USB-C, avec le debogage USB
autorise.

## Interaction Actuelle

- gachette droite : appliquer l'outil actif ;
- A/B : outil suivant / precedent ;
- joystick gauche horizontal : intensite de l'outil actif ;
- joystick gauche vertical : taille de l'outil actif ;
- X/Y : undo / redo ;
- bouton menu gauche : menu tools / save / load / export / quit ;
- grip gauche ou droit : deplacer et tourner la sculpture ;
- deux grips : zoomer et tourner autour de l'axe entre les mains ;
- Stretch : poser une ancre avec la gachette droite, puis tirer la zone ;
- suivi des mains : poing gauche pour grab, pincement gauche pour zoom,
  pincement droit pour sculpter, clap des deux mains pour changer d'outil.

La scene VR inclut une piece vide fixe en arriere-plan pour donner un repere
stable pendant la manipulation de l'objet.

## Notes

Le coeur SDF reste testable separement afin de valider les operations de volume
avant leur integration dans le rendu VR. La cible produit reste la version
Meta Quest 3.
