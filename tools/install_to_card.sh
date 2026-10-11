#!/usr/bin/env bash
# install_to_card.sh - instaluje rdzen Butterscotch + assety Undertale na karcie GB300V2 / SF2000
#
# Uzycie:
#   ./tools/install_to_card.sh                    # domyslnie /media/Sajnaps/GB300
#   SD_ROOT=/mnt/sda1 ./tools/install_to_card.sh   # inna sciezka montowania
#
# Co kopiuje:
#   <karta>/system/Deimos/cores/butterscotch.sf2k      rdzen (sciezka Deimos, nowa)
#   <karta>/cores/butterscotch/core_87000000           rdzen (sciezka legacy)
#   <karta>/ROMS/butterscotch/game.unx                 WAD Undertale 1.08
#   <karta>/ROMS/butterscotch/ATLAS.BIN                indeks atlasu
#   <karta>/ROMS/butterscotch/ATLAS.RGBA4444           tekstury RGBA4444
#   <karta>/ROMS/butterscotch/undertale                stub (wpis w liscie gier)
#
# Skrypt jest idempotentny: pliki o identycznej zawartosci nie sa kopiowane ponownie.
# Nadpisuje tylko to, co sie roznia (dopisz --force, by kopiowac wlasciwie za kazdym razem).

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# SD_ROOT: mount point of the card. No default on purpose -- guessing a path risks
# writing to the wrong filesystem, and the script already refuses non-FAT mounts.
SD_ROOT="${SD_ROOT:-}"
FORCE="${FORCE:-0}"

# Every source is overridable so the checkout does not have to hold the WAD.
# The WAD is the game's copyrighted data and is never committed here.
CORE_SRC="${CORE_SRC:-$REPO/dist/butterscotch.sf2k}"
WAD_SRC="${WAD_SRC:-$REPO/assets/game.unx}"
ATLAS_BIN_SRC="${ATLAS_BIN_SRC:-$REPO/build/ATLAS.BIN}"
ATLAS_PX_SRC="${ATLAS_PX_SRC:-$REPO/build/ATLAS.RGBA4444}"

CORE_DIR_DEIMOS="system/Deimos/cores"
CORE_DIR_LEGACY="cores/butterscotch"
ROM_DIR="ROMS/butterscotch"

say()  { printf '%s\n' "$*"; }
fail() { printf 'BLAD: %s\n' "$*" >&2; exit 1; }

# --- 1. sprawdzenie, czy karta jest NAPRAWDĘ zamontowana -------------------------------
# Ścieżka karty potrafi zostać jako zwykły katalog na dysku głównym, gdy czytnik
# karty jest odłączony. Bez tego guardu skrypt nadpisałby pliki w $HOME zamiast na karcie.
if [ -z "$SD_ROOT" ]; then
    fail "nie podano SD_ROOT. Uzycie:
         SD_ROOT=/media/<twoja-karta> $0"
fi
if ! mountpoint -q "$SD_ROOT"; then
    fail "karta SD nie jest zamontowana w '$SD_ROOT' (to zwykly katalog, nie punkt montowania).
         Podlacz czytnik i zamontuj, np.:
           sudo mkdir -p '$SD_ROOT' && sudo mount /dev/mmcblk0p1 '$SD_ROOT'"
fi
# sama lokalizacja to za mało - sprawdz, że to faktycznie partycja FAT, a nie np. tmpfs
sd_fs=$(stat -f -c %T "$SD_ROOT" 2>/dev/null || echo unknown)
case "$sd_fs" in
    vfat|fat|msdos|exfat) ;;
    *) fail "'$SD_ROOT' jest zamontowane, ale jako '$sd_fs', a nie FAT.
          Wskaz inny punkt montowania karty, np.: SD_ROOT=/mnt/sda1 $0" ;;
esac
say "Karta SD: $SD_ROOT (${sd_fs})"

# --- 2. sprawdzenie zrodel ----------------------------------------------------------------
for f in "$CORE_SRC" "$WAD_SRC" "$ATLAS_BIN_SRC" "$ATLAS_PX_SRC"; do
    [ -f "$f" ] || fail "brak pliku zrodlowego: $f"
done

need_kb=$(( $(stat -c %s "$CORE_SRC") + $(stat -c %s "$WAD_SRC") \
          + $(stat -c %s "$ATLAS_BIN_SRC") + $(stat -c %s "$ATLAS_PX_SRC") ))
avail_kb=$(df -Pk "$SD_ROOT" | awk 'NR==2 {print $4}')
[ "$avail_kb" -ge $(( need_kb / 1024 + 10240 )) ] \
    || fail "za malo miejsca na karcie: potrzeba ~$(( need_kb / 1024 / 1024 )) MB, dostepne $(( avail_kb / 1024 / 1024 )) MB"

# --- 3. instalacja ------------------------------------------------------------------------

# copy_if_changed <src> <dst>
copy_if_changed() {
    local src="$1" dst="$2"
    mkdir -p "$(dirname "$dst")"
    if [ "$FORCE" = "1" ] || ! cmp -s "$src" "$dst"; then
        cp -f "$src" "$dst"
        printf '  -> %s (%s)\n' "$dst" "$(du -h "$dst" | cut -f1)"
    else
        printf '  OK  %s (bez zmian)\n' "$dst"
    fi
}

say ""
say "[1/2] Rdzen"
copy_if_changed "$CORE_SRC" "$SD_ROOT/$CORE_DIR_DEIMOS/butterscotch.sf2k"
copy_if_changed "$CORE_SRC" "$SD_ROOT/$CORE_DIR_LEGACY/core_87000000"

say ""
say "[2/2] Gra i assety -> $SD_ROOT/$ROM_DIR"
copy_if_changed "$WAD_SRC"       "$SD_ROOT/$ROM_DIR/game.unx"
copy_if_changed "$ATLAS_BIN_SRC" "$SD_ROOT/$ROM_DIR/ATLAS.BIN"
copy_if_changed "$ATLAS_PX_SRC"  "$SD_ROOT/$ROM_DIR/ATLAS.RGBA4444"

# Muzyka w tle BGM (*.ogg)
if ls "$REPO/assets/"*.ogg >/dev/null 2>&1; then
    for ogg in "$REPO/assets/"*.ogg; do
        copy_if_changed "$ogg" "$SD_ROOT/$ROM_DIR/$(basename "$ogg")"
    done
fi

# Stuby: multicore format: <core>;<rom_dir>;<target_file>
# Konsola GB300 wyswietla w glownym menu gry z ROMS/gba/*.gba
STUB_PAYLOAD="butterscotch;butterscotch;game.unx"
printf '%s' "$STUB_PAYLOAD" > "$SD_ROOT/$ROM_DIR/undertale"
printf '%s' "$STUB_PAYLOAD" > "$SD_ROOT/$ROM_DIR/Undertale.gba"
printf '%s' "$STUB_PAYLOAD" > "$SD_ROOT/$ROM_DIR/undertale.gba"
if [ -d "$SD_ROOT/ROMS/gba" ]; then
    printf '%s' "$STUB_PAYLOAD" > "$SD_ROOT/ROMS/gba/Undertale.gba"
fi
printf '%s' "$STUB_PAYLOAD" > "$SD_ROOT/ROMS/butterscotch;butterscotch;game.unx.gba"

sync

# --- 4. weryfikacja -----------------------------------------------------------------------
say ""
say "Weryfikacja:"
rc=0
check() {
    local dst="$1" src="$2"
    if cmp -s "$src" "$dst"; then
        printf '  OK      %s\n' "${dst#$SD_ROOT/}"
    else
        printf '  UHAJ    %s\n' "${dst#$SD_ROOT/}"
        rc=1
    fi
}
check "$SD_ROOT/$CORE_DIR_DEIMOS/butterscotch.sf2k" "$CORE_SRC"
check "$SD_ROOT/$CORE_DIR_LEGACY/core_87000000"      "$CORE_SRC"
check "$SD_ROOT/$ROM_DIR/game.unx"                   "$WAD_SRC"
check "$SD_ROOT/$ROM_DIR/ATLAS.BIN"                  "$ATLAS_BIN_SRC"
check "$SD_ROOT/$ROM_DIR/ATLAS.RGBA4444"             "$ATLAS_PX_SRC"

say ""
if [ "$rc" -eq 0 ]; then
    say "Gotowe. Rdzen i pliki gry sa na karcie."
    say "Uruchom 'Undertale' z menu; katalog ROMS jest przypisany do rdzenia butterscotch."
else
    say "UWAGA: rozjezdza sie wyzej - sprawdz nośnik." >&2
fi
exit "$rc"