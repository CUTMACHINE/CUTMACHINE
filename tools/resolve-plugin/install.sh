#!/bin/bash
# Installe le plugin CUTMACHINE dans le menu Scripts de DaVinci Resolve.
set -e

SCRIPTS="$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Scripts/Utility"
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "→ Dossier Scripts de Resolve..."
mkdir -p "$SCRIPTS"

echo "→ Copie du plugin et de son cœur..."
cp "$HERE/CUTMACHINE.lua" "$SCRIPTS/"
cp "$HERE/cutmachine_resolve_lib.lua" "$SCRIPTS/"

# PRD36 : le plugin copié ne sait plus où est le dépôt. On lui laisse le chemin
# du moteur compilé dans ce checkout ; s'il n'est pas encore compilé, rien
# n'est écrit et un ancien fichier est retiré, pour ne pas désigner un binaire
# qui n'existe plus.
REPO="$(cd "$HERE/../.." && pwd)"
BINARY="$REPO/build/cutmachine"
HINT="$SCRIPTS/cutmachine_binary_path.txt"
if [ -x "$BINARY" ]; then
    printf '%s\n' "$BINARY" > "$HINT"
    echo "→ Moteur noté : $BINARY"
else
    rm -f "$HINT"
    echo "→ Moteur non compilé dans ce dépôt ($BINARY absent) :"
    echo "  le plugin le cherchera dans CUTMACHINE_BIN, puis dans le PATH."
fi

echo ""
echo "✓ Plugin installé."
echo "  1. Ouvre DaVinci Resolve et ton projet"
echo "  2. Workspace → Scripts → Utility → CUTMACHINE"
echo "  3. Renseigne le chemin du binaire et celui du projet CUTMACHINE"
echo ""
echo "  Le binaire est détecté seul, dans cet ordre : le dernier chemin"
echo "  enregistré, la variable CUTMACHINE_BIN, build/cutmachine du dépôt"
echo "  depuis lequel ce script a été lancé, ~/CUTMACHINE/build/cutmachine,"
echo "  puis le PATH. Les logs vont dans :"
echo "  ~/Desktop/CUTMACHINE_Resolve.log"
