# nah2 — Suite de Diagnostic Spatial Temps Réel pour Roblox

`nah2` est un outil externe de visualisation spatiale sous Windows (Direct3D 11 + Direct2D 1.1 + DirectComposition) conçu pour afficher en surimpression transparente au-dessus de la fenêtre **Roblox** (`RobloxPlayerBeta.exe` ou `RobloxStudioBeta.exe`) des repères diagnostiques en temps réel :
- Rectangles englobants 2D avec étiquettes de distance (en *studs*)
- Boîtes englobantes 3D orientées filaires (OBB) avec découpage sur le plan proche (*near-plane clipping*)
- Axes locaux d'orientation (`RightVector`, `UpVector`, `LookVector`)
- Squelettes simplifiés R6 / R15
- Barre HUD de télémétrie (transport actif, fréquence IPC, FPS de rendu, latence)
- **Aim Assist humain réaliste (`nah2::aim`)** : simulation motrice Fitts + minimum-jerk, courbure naturelle du poignet, dérive organique dans la hitbox, délai de réaction au changement de cible, prédiction de vitesse relative, calibration automatique de sensibilité et synergie passive `WM_INPUT` (breakaway manuel instantané)

---

## Arborescence du projet

```text
nah2/
├── roblox/
│   └── client_telemetry.lua   # LocalScript autonome (exécuteur) — capture, visibilité, équipes & WS/HTTP
├── src/
│   ├── telemetry_types.h      # Structures Caméra, Entité, Squelette, État souris et Stats IPC
│   ├── math_utils.h           # Pipeline World-to-Screen, clipping 3D, OBB, lerp delta-time
│   ├── aim_assist.h/.cpp      # Moteur Aim Assist humain (Fitts, minimum-jerk, arc poignet, auto-sens)
│   ├── control_panel.h/.cpp   # Fenêtre de contrôle interactive ESP & Aim Assist (switches, profils, sliders)
│   ├── ipc_receiver.h/.cpp    # Serveur 127.0.0.1 (WS RFC 6455, HTTP Keep-Alive, UDP) + double buffer
│   ├── renderer.h/.cpp        # Fenêtre Win32 click-through + rendu D3D11/D2D1/DComp + WM_INPUT passif
│   └── nah2.cpp               # Point d'entrée, boucle haute fréquence, options CLI et raccourcis
├── tests/
│   └── test_nah2.cpp          # Tests unitaires (projection, lerp, JSON, boucle aim humaine, IPC)
├── CMakeLists.txt             # Configuration CMake (MSVC / MinGW-w64)
└── build.ps1                  # Script de compilation rapide et exécution des tests
```

---

## Utilisation côté Roblox (Exécuteur)

Aucun script serveur ni `RemoteEvent` n'est nécessaire : `roblox/client_telemetry.lua` fonctionne entièrement côté client via votre exécuteur.

1. **Lancer le programme externe :**
   ```powershell
   .\build\nah2.exe
   ```
   Le serveur IPC écoute sur `127.0.0.1:27015` (WebSocket `/ws`, HTTP POST `/telemetry`, UDP).
2. **Exécuter `roblox/client_telemetry.lua` dans votre exécuteur :**
   - Collez le contenu de `roblox/client_telemetry.lua` dans votre exécuteur (ou placez-le dans son dossier `autoexec`).
   - Le script capture `Workspace.CurrentCamera` et les entités (`Players`, NPCs avec `Humanoid`, instances taguées `nah2_track`) puis les transmet directement à `nah2.exe` :
     - **WebSocket (`ws://127.0.0.1:27015/ws`)** en priorité via `WebSocket.connect` (ou `syn.websocket.connect`, etc.) à 60 Hz.
     - **Repli HTTP POST (`http://127.0.0.1:27015/telemetry`)** via `request` / `http_request` / `syn.request` si WebSocket n'est pas disponible.
   - Ré-exécuter le script nettoie automatiquement l'instance précédente (connexions et socket) avant de redémarrer.

---

## Compilation et exécution (Windows)

### Méthode 1 : PowerShell + MSYS2 / MinGW-w64 (`g++`)

```powershell
.\build.ps1
```

Ce script génère `build\nah2.exe` et `build\test_nah2.exe`, puis lance la suite de tests.

### Méthode 2 : CMake (Visual Studio / Ninja / MinGW)

```powershell
cmake -B build -S .
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

### Options CLI & Raccourcis

- `--port <N>` : port d'écoute sur `127.0.0.1` (défaut : `27015`)
- `--target <texte>` : filtre de fenêtre cible (défaut : `Roblox`)
- `--smooth <vitesse>` : constante de lissage `lerp` des entités (`0.0` par défaut = latence minimale)
- `--cam-smooth <vitesse>` : lissage de la caméra (`0.0` par défaut ; utile à `18.0` en mode HTTP basse fréquence)
- `--always-show` : garde l'overlay affiché même si Roblox n'est pas au premier plan
- `--no-overlay` : démarre avec l'overlay visuel masqué (`F8` pour l'afficher)
- `--esp` / `--no-esp` : active / désactive l'ESP visuel au lancement (`F5` pour basculer)
- `--no-panel` : masque la fenêtre de contrôle graphique au démarrage (`INS` pour l'ouvrir)
- `--vsync` : active la VSync DXGI (désactivée par défaut pour une latence minimale)
- `--vp-offset <x,y>` : décalage manuel en pixels du viewport
- `--aim` / `--no-aim` : active / désactive l'Aim Assist humain (actif par défaut)
- `--aim-profile <legit|pro|tracking>` : profil moteur (`legit` par défaut, `pro` plus nerveux, `tracking` suivi continu)
- `--aim-bone <smart|head|neck|chest>` : zone visée (`smart` par défaut)
- `--aim-key <any|rmb|lmb|x1|x2|alt|shift|always>` : touche d'activation (`any` par défaut : RMB/X1/X2 ou LMB si caméra verrouillée)
- `--aim-fov <px>` : rayon FOV en pixels (référence 1080p)
- `--aim-strength <mult>` (ou `--aim-power <mult>`) : puissance non-linéaire (`0.10` à `2.00`, défaut `1.0` = `100%`)
- `--aim-smooth <mult>` : facteur de lissage (`< 1.0` plus rapide, `> 1.0` plus doux ; défaut : `1.0`)
- `--aim-deadzone <ratio>` : zone morte intérieure proportionnelle au rayon hitbox (`0.0` à `0.9`)
- `--aim-lead <mult>` : intensité de prédiction de vélocité (`0.0` à `1.5`)
- `--aim-sens <mult>` : sensibilité initiale (laisser `0` pour calibration automatique en ligne)
- `--no-team-check` / `--no-vis-check` : désactive le filtre d'équipe / de ligne de vue

Raccourcis clavier non-bloquants :
- **`F5`** : activer / désactiver l'ESP visuel
- **`F6`** : activer / désactiver l'Aim Assist
- **`F7`** : changer de profil Aim Assist (`LEGIT` -> `PRO` -> `TRACK`)
- **`F8`** : afficher / masquer tout l'overlay
- **`F9`** : activer / désactiver les squelettes
- **`F10`** : afficher / masquer la barre HUD
- **`INS`** : afficher / masquer le panneau de contrôle `nah2`
- **`END`** : quitter proprement `nah2`
