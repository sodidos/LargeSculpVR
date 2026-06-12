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
  droite, avec manche court et une tete 3D dont la forme identifie l'outil
  (raymarching SDF dans la passe UI, ombrage reel) : boule pleine = Add,
  cuillere creuse = Subtract, galet plat = Smooth, crochet = Stretch, taloche
  carree = Flatten, burin pointu = Groove, pince = Crease, pinceau avec touffe
  de la couleur choisie = Paint. Un anneau discret indique le rayon reel du
  pinceau. Les memes silhouettes servent de mini-icones dans le menu et la
  barre d'en-tete ; la couleur reste un indice secondaire.
- Prototype actuel : pincement de la main droite pres de la surface lance aussi
  Stretch, avec la zone pincee comme ancre.
- Prototype actuel : clap des deux mains ouvertes = outil suivant ; pincement
  droit = application de l'outil actif a la surface.
- Prototype actuel : les mains suivies sont dessinees en capsules gris pale
  translucides (style mains systeme Meta) dans la passe UI separee, avec une
  pastille couleur outil au bout de l'index droit.
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

- Add : ajoute de la matiere ; fonctionne aussi en espace vide pour creer la
  premiere matiere (la scene demarre vierge). Les stamps successifs d'un trait
  sont relies par des capsules pour rester continus.
- Subtract : retire de la matiere avec le meme falloff, traits continus aussi.
- Smooth : lissage local prioritaire pour rendre la surface sculptable.
- Flatten : au debut du trait, le plan tangent est verrouille sur le point de
  contact ; tout le trait aplatit vers ce plan (rase les bosses, comble les
  creux). C'est l'outil hard surface principal, inspire de Medium.
- Groove : creuse une rainure fine (capsule de rayon ~1/3 du pinceau) le long
  du mouvement, pour les lignes de panneaux et details hard surface.
- Crease : re-echantillonne le champ depuis des positions ecartees du centre
  du pinceau, ce qui contracte la matiere vers le trait et affute les aretes
  (pinch de Medium).
- Paint : depose la couleur de la palette dans un volume couleur RGBA8
  echantillonne par le shader comme albedo ; la peinture est limitee a une
  bande etroite autour de la surface. L'undo/redo capture aussi les couleurs,
  et l'export OBJ ecrit les couleurs de sommets (v x y z r g b).
- Stretch manette : gachette droite pose l'ancre sur la surface, puis le
  mouvement de la manette droite tire la zone. L'influence suit tout le trajet
  de traction (capsule ancre vers main, rampe axiale) : la base reste attachee
  au corps et un cou effile se forme, meme en tirant au-dela du rayon. Une
  passe de relaxation ponderee par le warp re-regularise le champ pour eviter
  les surfaces dechirees. Relacher la gachette libere l'ancre pour choisir une
  autre zone au prochain appui.
- Outils mains : le pincement droit applique l'outil actif a la position
  pincee. Add cree de la matiere, Stretch tire directement la zone pincee,
  Subtract efface, Smooth lisse.
- La taille de zone d'influence vient du rayon courant et de l'echelle/zoom de
  l'objet ; elle sera ajustee apres test.
- Chaque outil conserve ses propres reglages de rayon et d'intensite.

## Interface minimale

- HUD actuel : panneau spatial devant la manette gauche avec nom de l'outil en
  toutes lettres, barres SIZE et POWER horizontales avec valeurs numeriques
  (cm et %), et rappels des raccourcis (X:UNDO Y:REDO A/B:TOOL).
- Menu actuel : deux colonnes — 8 outils a gauche, actions SAVE / LOAD /
  EXPORT / QUIT / AR / MIRROR a droite — et une palette de 8 couleurs
  (grille 4x2) en bas du panneau. Le panneau reste ainsi compact et toutes
  les lignes sont faciles a viser.
- MIRROR : symetrie sur le plan local X=0 visualisee par un disque bleu
  transparent qui suit l'objet ; chaque stamp est applique des deux cotes,
  Stretch compris (ancre, delta et rotation miroites, application en couche
  sans reinitialiser la source).
- Architecture texte : le panneau entier est peint cote CPU dans une texture
  RGBA8 264x528 (`HudPainter.h`, police bitmap 5x7, rectangles, contours),
  re-uploadee uniquement quand l'etat affiche change ; le shader UI ne fait
  plus qu'echantillonner cette texture sur le plan du panneau. Le layout en
  pixels est partage entre le dessin et le hit-test C++ du rayon, ce qui
  garantit que la zone cliquee correspond a la zone affichee (l'ancien code
  avait un panneau dessine en 0,34 m mais teste en 0,17 m).
- Le HUD, le menu, la ligne de selection, l'outil main droite et le squelette
  des mains restent rendus dans une passe UI transparente separee du shader
  SDF.
- Actions fichier : SAVE/LOAD ecrivent et relisent un fichier local v2
  (SDF + volume couleur, retrocompatible v1), EXPORT genere un OBJ local,
  QUIT demande la sortie de la session.

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
