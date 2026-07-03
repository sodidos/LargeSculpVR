# Conception : résolution adaptative par briques éparses (R2b)

Objectif : sculpter un personnage détaillé, voire un diorama complet, avec des
crêtes et sillons nets (hard surface). Cela demande deux choses que le volume
dense actuel ne peut pas offrir en même temps : une grande étendue de travail
et des voxels fins là où le détail compte. Ce document propose l'architecture,
à valider avant implémentation.

## État actuel (après R1/R2a)

- Volume dense 256³ sur 4,8 m → voxels de 1,875 cm partout, 64 Mo SDF +
  64 Mo couleur.
- Undo par journal de pages 32³ (budget mémoire), brosses à coût local.
- Un doublement supplémentaire (512³ dense) coûterait 512 Mo + 512 Mo :
  exclu sur casque.

## Architecture proposée

### Découpage en briques

- Le volume devient **virtuel** : une grille de briques de 32³ voxels.
- Une brique n'existe en mémoire que si de la matière (bande étroite autour
  de la surface) la traverse. Le vide et l'intérieur profond ne coûtent rien
  (valeur par défaut : « loin dehors » ou « loin dedans » par brique).
- Étendue virtuelle cible : 16 × 16 × 16 briques de base (~512³ voxels
  logiques à 1,875 cm → même finesse qu'aujourd'hui mais sur **9,6 m**
  d'atelier : le diorama).

### Deux niveaux de détail

- **L0** : voxels de 1,875 cm (briques 32³ → 60 cm de côté).
- **L1** : raffinement 4× → voxels de ~4,7 mm. Une brique L0 peut être
  « promue » : elle est remplacée par 4×4×4 = 64 sous-briques L1 (créées
  paresseusement, seules celles traversées par la surface existent).
- La promotion est déclenchée automatiquement quand un outil fin travaille
  la zone (rayon de pinceau sous un seuil, outils Groove/Crease/Flatten), ou
  explicitement par un bouton « DETAIL » du menu.

### Côté CPU

- `SparseSdfVolume` : table de hachage clé de brique → brique
  (`std::vector<float>` 32³ + niveau + drapeaux). API identique aux brosses
  actuelles (mêmes signatures), qui itèrent les briques intersectées.
- Les brosses écrivent au niveau le plus fin présent dans la zone ; une
  écriture fine dans une brique L0 non promue la promeut d'abord
  (rééchantillonnage L0 → L1 à la volée, trilinéaire).
- L'undo actuel (journal de pages) s'adapte directement : une page = une
  brique ; le journal mémorise aussi les promotions/créations.

### Côté GPU

- **Atlas de briques** : une grande texture 3D (ex. 1024×1024×512 = illimité
  en pratique, on vise ~256 Mo) contenant les briques actives (33³ avec
  bordure de 1 voxel pour le filtrage trilinéaire sans couture).
- **Table d'indirection** : petite texture 3D (16³ pour L0, entrées des
  briques promues pointant vers une sous-table ou un second niveau
  d'indirection) donnant pour chaque brique : absente (valeur constante),
  adresse atlas L0, ou adresse(s) L1.
- **Raymarch deux niveaux** : DDA sur la grille de briques ; brique absente →
  saut direct à la brique suivante (grands pas dans le vide, plus rapide
  qu'aujourd'hui) ; brique présente → échantillonnage atlas avec le pas
  adapté au niveau.
- La couleur suit le même schéma (atlas RGBA8 parallèle).

### Coutures entre niveaux

- Bordure de 1 voxel dupliquée dans chaque brique (33³ stockés) : le
  filtrage trilinéaire ne traverse jamais une frontière de brique.
- À la frontière L0/L1, la bordure L1 est remplie par échantillonnage du
  voisin L0 (et inversement) à chaque édition de la zone frontalière ;
  visuellement la transition est continue (le champ est le même, seule la
  bande passante spatiale change).

### Fichiers

- Format **v3** : en-tête + liste de briques (clé, niveau, données SDF +
  couleur compressées RLE simple). Les v1/v2 denses se rééchantillonnent au
  chargement (déjà en place).
- Export OBJ : itération par brique au niveau le plus fin local (le maillage
  suit le détail réel).

## Estimation mémoire (cas typique)

Un personnage détaillé ~1,2 m dans l'atelier :
- ~300 briques L0 traversées par la surface : 300 × 33³ × 4 o ≈ 43 Mo (+ 43 Mo
  couleur) ;
- ~400 sous-briques L1 sur les zones détaillées (visage, mains, gravures) :
  ≈ 57 Mo (+ 57 Mo) ;
- Total ≈ 200 Mo, dans le budget du Quest 3, pour un détail effectif 4× plus
  fin qu'aujourd'hui et un atelier 2× plus grand.

## Plan d'implémentation par étapes (chacune testable)

1. **SparseSdfVolume CPU** derrière l'API actuelle + tests (les brosses et
   l'undo fonctionnent sans changement d'appelant) — le rendu reçoit
   provisoirement une texture dense reconstruite (lent mais correct).
2. **Atlas + indirection GPU L0** : le raymarcheur passe au DDA par briques ;
   suppression de la texture dense. Gains de perf immédiats dans le vide.
3. **Promotion L1** : écriture fine, bordures, affichage deux niveaux.
4. **Format v3 + export par briques.**
5. Extension de l'atelier à 9,6 m (constante).

Chaque étape laisse l'application dans un état fonctionnel installable.

## Points ouverts (à trancher ensemble)

- Faut-il un indicateur visuel des zones promues (léger liseré au survol) ?
- La promotion automatique : seuil de rayon proposé = pinceau < 6 cm
  (objet) ; OK ?
- Budget briques dur (ex. 256 Mo) avec refus de promotion au-delà + message
  HUD, ou dépromotion automatique des zones les moins récemment éditées ?
