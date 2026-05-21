# Interface VR v0

Objectif : definir une interface de sculpture native Quest 3 avec les deux
manettes et le suivi des mains comme controles principaux.

## Principes

- Le volume SDF reste centre dans un espace de travail devant l'utilisateur.
- La main droite est l'outil principal : add, subtract, smooth, stretch.
- La main gauche sert a manipuler le modele et a acceder aux actions rapides.
- Les outils doivent rester continus : pas de pause perceptible entre mouvement
  de main, edition SDF et rendu.

## Main droite

- Gachette : appliquer l'outil actif.
- Prototype actuel : la gachette droite applique l'outil actif.
- Prototype actuel : A/B change l'outil actif entre Add, Subtract, Smooth et
  Stretch.
- Prototype actuel : viser la surface avec la main droite colore la zone du
  pinceau selon l'outil actif.
- Prototype actuel : en mode Stretch, la gachette droite definit la zone
  attrapee et le mouvement de la manette deplace cette zone.
- Prototype actuel : un outil spatial apparait a la position de la manette
  droite, avec manche court et tete coloree selon l'outil actif.
- Prototype actuel : pincement de la main droite pres de la surface lance aussi
  Stretch, avec la zone pincee comme ancre.
- Prototype actuel : clap des deux mains ouvertes = outil suivant ; pincement
  droit = application de l'outil actif a la surface.
- Prototype actuel : les mains suivies sont dessinees en squelette/capsules
  dans la passe UI separee.
- Grip : manipulation de l'objet, meme en mode Stretch.
- A/B : outil suivant / precedent.

## Main gauche

- Prototype actuel : grip gauche ou droit + mouvement de main deplace et
  tourne la sculpture avec l'orientation de la manette.
- Prototype actuel : grip gauche + grip droit + ecartement des mains zoome /
  change l'echelle de travail, avec rotation selon l'axe entre les mains.
- Prototype actuel : grip gauche seul laisse la main droite sculpter pendant
  que la sculpture est tenue.
- Prototype actuel : poing gauche ferme = grab de l'objet.
- Prototype actuel : la main gauche suivie est visible meme sans manette, avec
  la meme representation squelette/capsules.
- Prototype actuel : bornes de zoom 0.05x a 10.00x.
- Prototype actuel : joystick gauche horizontal = intensite de l'outil actif.
- Prototype actuel : joystick gauche vertical = taille du pinceau de l'outil
  actif.
- X : undo.
- Y : redo.
- Bouton menu : ouvrir/fermer un panneau outils + SAVE / LOAD / EXPORT / QUIT
  devant la manette gauche.
- Prototype actuel : le menu reste pilote par le rayon/gachette droite avec les
  manettes. En suivi des mains, le changement d'outil passe par un clap.
- Prototype actuel : poing gauche = grab de l'objet ; pincement gauche +
  deplacement horizontal = zoom/echelle ; clap des deux mains ouvertes = outil
  suivant ; pincement droit pres de la surface = applique l'outil actif.

## Outils

- Add : ajoute de la matiere avec une intensite faible par defaut.
- Subtract : retire de la matiere avec le meme falloff.
- Smooth : lissage local prioritaire pour rendre la surface sculptable.
- Stretch manette : gachette droite pose l'ancre sur la surface, puis le
  mouvement de la manette droite tire la zone ; les voxels autour suivent avec
  une influence proportionnelle au rayon et a l'intensite. Relacher la gachette
  libere l'ancre pour choisir une autre zone au prochain appui.
- Outils mains : le pincement droit applique l'outil actif a la position
  pincee. Add cree de la matiere, Stretch tire directement la zone pincee,
  Subtract efface, Smooth lisse.
- La taille de zone d'influence vient du rayon courant et de l'echelle/zoom de
  l'objet ; elle sera ajustee apres test.
- Chaque outil conserve ses propres reglages de rayon et d'intensite.

## Interface minimale

- HUD actuel : panneau spatial devant la manette gauche avec nom du pinceau,
  barre SIZE verticale et barre POWER horizontale.
- Menu actuel : panneau spatial outils + SAVE / LOAD / EXPORT / QUIT au meme
  endroit que le HUD.
- Le HUD, le menu, la ligne de selection et l'outil main droite sont rendus
  dans une passe UI transparente separee du shader SDF pour ne plus perturber
  l'affichage de l'objet.
- Actions fichier : SAVE/LOAD ecrivent et relisent un fichier SDF local,
  EXPORT genere un OBJ local, QUIT demande la sortie de la session.

## Etapes techniques

1. Demarrer une activite Quest native et valider OpenXR sur le casque.
2. Creer session OpenXR + swapchains stereo.
3. Stabiliser le raymarching SDF OpenGL ES 3.2 sur Quest 3.
4. Ajouter les actions OpenXR des manettes Touch.
5. Brancher add/sub/smooth.
6. Brancher stretch avec ancre, delta et rayon d'influence.
7. Optimiser les editions SDF GPU pour tenir le framerate VR.
8. Remettre HUD/menu/outils dans une passe separee du raymarching SDF.
9. Remplacer la representation squelette des mains par `XR_FB_hand_tracking_mesh`
   si l'extension est fiable sur Quest 3.
