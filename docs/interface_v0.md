# Interface VR v0

Objectif : definir une interface simple avant le rendu final. La sculpture doit
rester utilisable avec les controleurs Quest 3, puis compatible main tracking
si le geste reste clair.

## Main dominante

- Gachette index : appliquer l'outil actif.
- Grip : tenir/attraper une zone quand l'outil stretch sera actif.
- Joystick haut/bas : changer la taille du pinceau.
- Joystick gauche/droite : changer la force du pinceau.
- Bouton A : alterner add/subtract.
- Bouton B : menu radial des outils.

## Main secondaire

- Gachette index : saisir/deplacer la sculpture.
- Grip : rotation/echelle de la sculpture avec les deux mains.
- Joystick gauche/droite : undo/redo.
- Bouton X : centrer la sculpture.
- Bouton Y : afficher/masquer la grille et les aides visuelles.

## Outils v0

1. Add : union SDF avec une sphere.
2. Subtract : difference SDF avec une sphere.
3. Smooth : moyenne locale du champ SDF avec attenuation douce au bord du
   pinceau.
4. Stretch : deplacement proportionnel d'une zone attrapee. Le centre suit le
   geste, les voxels proches suivent selon un falloff doux.

## Preview desktop v0

La preview PC simule les gestes principaux avant l'integration OpenXR.

| Preview PC | Controleur Quest |
| --- | --- |
| Souris gauche sur surface 3D | Gachette main dominante |
| Souris gauche sur coupe 2D | outil de debug/profondeur |
| Clic droit + mouvement souris | rotation de vue / manipulation objet |
| `Ctrl` + clic droit + mouvement souris | zoom debug de la vue desktop |
| Molette | Joystick haut/bas main dominante |
| `1` / `2` | Bouton A ou menu radial |
| `3` | menu radial vers Smooth |
| `6` | menu radial vers Stretch |
| `4` / `5` | precision voxel moins/plus |
| `+` / `-` pave numerique | force de l'outil actif |
| `Page Up` / `Page Down` | profondeur du pointeur VR |
| `Z` / `Y` | joystick gauche/droite main secondaire |
| `W` | afficher/masquer filaire debug |
| `P` | export OBJ triangule de travail |
| `R` | reset scene |

## Outil Stretch

Stretch : selectionner une zone de l'objet, tirer la main, et deformer les
voxels proches avec une attenuation proportionnelle a la distance.

Formule cible pour stretch :

```text
offset(p) = drag_delta * falloff(distance(p, handle_center) / radius)
```

Le falloff devra etre fort au centre, doux vers le bord, et nul au-dela du
rayon. Pour que le geste soit naturel en VR, l'utilisateur doit voir :

- la sphere d'influence ;
- le point d'ancrage ;
- la direction de tirage ;
- la previsualisation pendant le mouvement.
