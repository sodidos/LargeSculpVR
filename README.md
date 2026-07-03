# LargeSculpVR

Application native de sculpture SDF pour Meta Quest 3.

Le projet vise une experience VR directe sur casque, sans Unity, avec un coeur
C++ portable et une interface OpenXR optimisee pour les manettes Touch et le
suivi des mains.

## Objectif

- sculpture volumique SDF en temps reel, scene vierge au demarrage ;
- rendu stereo OpenGL ES par raymarching d'une texture 3D ;
- outils Add, Subtract, Smooth, Stretch, Flatten (aplatir), Groove (rainure),
  Crease (pincement d'arete) et Paint (peinture volumique avec palette) ;
- traits continus : les coups de pinceau rapides sont relies par des capsules ;
- manipulation de l'objet avec les grips ;
- HUD/menu spatial devant la manette gauche, avec vrai texte rendu CPU
  (police bitmap) dans une texture echantillonnee par le shader UI ;
- suivi des mains Quest avec hysteresis et lissage des gestes ;
- undo/redo couvrant la sculpture et la peinture ;
- export OBJ avec couleurs de sommets, sauvegarde locale v2 (SDF + couleurs).

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

- gachette droite : appliquer l'outil actif (Add fonctionne en espace vide,
  ce qui permet de creer la premiere matiere dans la scene vierge) ;
- A/B : outil suivant / precedent (Add, Subtract, Smooth, Stretch, Flatten,
  Groove, Crease, Paint) ;
- joystick droit horizontal : intensite de l'outil actif ;
- joystick droit vertical : taille de l'outil actif ; la taille est relative
  a l'objet (elle suit le zoom : meme fraction de la sculpture quel que soit
  l'agrandissement) ; deux arcs (cyan = taille, orange = force) apparaissent
  autour de la pointe de l'outil pendant le reglage ;
- joystick gauche : deplacement dans le decor ;
- X/Y : undo / redo (sculpture et peinture) ;
- bouton menu gauche : menu en deux colonnes (outils a gauche, actions SAVE /
  LOAD / EXPORT / QUIT / AR / MIRROR a droite) + palette de 8 couleurs de
  peinture (grille 4x2 de grandes cases) ;
- MIRROR : mode symetrie, chaque coup de pinceau est duplique en miroir sur
  le plan local X=0 (Stretch compris), visualise par un disque bleu
  transparent ;
- LOCK : fige la position, la rotation et l'echelle de l'objet (les grips et
  le pincement-zoom sont ignores tant que le verrou est actif) ;
- en mode AR seul l'objet semble se deplacer pendant la locomotion ;
- EXPORT publie l'OBJ dans `Documents/LargeSculpVR/sculpt_<date>.obj`,
  visible dans le gestionnaire de fichiers du Quest et par USB ;
- AR : passthrough compose sous la scene, le fond devient transparent et la
  sculpture reste opaque ;
- grip gauche ou droit : deplacer et tourner la sculpture ;
- deux grips : zoomer et tourner autour de l'axe entre les mains ;
- Stretch : poser une ancre avec la gachette droite, puis tirer la zone ;
- Flatten : un disque transparent previsualise le plan tangent sous le pinceau
  avant d'appuyer ; le plan est verrouille au premier contact du trait, les
  bosses sont rasees et les creux combles vers ce plan ;
- Groove : creuse une rainure fine (capsule) le long du mouvement ;
- Crease : pince la matiere vers le centre du pinceau pour affuter les aretes ;
- Paint : depose la couleur choisie dans la palette pres de la surface ;
- suivi des mains : poing gauche pour grab, pincement gauche pour zoom,
  pincement droit pour sculpter/peindre, clap des deux mains pour changer
  d'outil ; les gestes ont une hysteresis et les positions sont lissees ;
- menu aux mains : bouton MENU dans l'en-tete du panneau du poignet gauche,
  et appui direct de l'index droit sur le panneau pour survoler et cliquer
  (la sculpture est suspendue quand le doigt est dans la zone du panneau) ;
- anti-declenchement : les gestes mains (pincement sculpteur, poing/main
  ouverte, poing gauche de grab) doivent etre tenus ~0,2 s avant d'agir ;
  l'armement est annonce par un bourdonnement grave continu termine par un
  pop aigu de validation ;
- mains : la droite n'affiche que pouce et index (fins, noir opaque, pastille
  couleur outil au bout de l'index) ; la gauche n'apparait que lorsqu'elle
  agit (poing ou pincement) ;
- menu ferme : un simple bouton rond MENU au-dessus du poignet gauche ;
- Add depose la matiere dans la couleur de peinture choisie ;
- retour sonore synthetise (AAudio) : clic menu, changement d'outil, debut de
  trait, grab, undo/redo, save/load/export.

La scene VR inclut une piece vide fixe en arriere-plan pour donner un repere
stable pendant la manipulation de l'objet.

## Notes

Le coeur SDF reste testable separement afin de valider les operations de volume
avant leur integration dans le rendu VR. La cible produit reste la version
Meta Quest 3.
