#!/bin/bash
# hib_install.sh <apk> [package]: installs an update over a running game without losing the player's place (hibernation, dm/host/hle_hibsnap.c).
#   1. the running game (its session started with DM_HIBERNATE=1) is asked through <external>/hib_ctl.txt to "hib freeze": a snapshot, then the game stays stopped;
#   2. once the log says it is frozen, adb install -r replaces the app (the process ends with it);
#   3. the menu activity is started: it finds the snapshot and resumes the game there ("MELANJUTKAN..." while it is read). A snapshot the new build cannot take is removed
#      and the menu shows: Continue from the game's own save works as before (the snapshot never touches it).
# No input is sent to the device. ADB_SERIAL picks the device; FORCE=1 installs even when the game could not be frozen (it then starts from the menu as usual).
# Without a running game (or one started without DM_HIBERNATE) it only installs.
set -u
APK=${1:?usage: hib_install.sh <apk> [package]}
PKG=${2:-com.nxnative.t0100606026f12000}
A=(adb); [ -n "${ADB_SERIAL:-}" ] && A=(adb -s "$ADB_SERIAL")
EXT=/sdcard/Android/data/$PKG/files
LOG=$EXT/log.txt
sh_() { "${A[@]}" shell "$@" 2>/dev/null | tr -d '\r'; }
log_from() { sh_ "tail -n +$1 $LOG"; }   # the log's lines from line $1 on
wait_for() {   # wait_for <from line> <seconds> <ok pattern> <fail pattern>: prints the matching line; 0 ok, 1 failed, 2 timed out
    local from=$1 secs=$2 ok=$3 bad=$4 t=0 l
    while [ $t -lt $secs ]; do
        l=$(log_from "$from" | grep -E "$ok|$bad" | head -1)
        if [ -n "$l" ]; then echo "$l"; echo "$l" | grep -qE "$ok" && return 0; return 1; fi
        sleep 1; t=$((t + 1))
    done
    return 2
}

frozen=0
if sh_ "pidof $PKG:game" | grep -q .; then
    if sh_ "grep -c 'hibernate: control file' $LOG" | grep -qv '^0*$'; then
        n=$(($(sh_ "wc -l < $LOG") + 1))
        echo "the game is running with hibernation: freezing it (snapshot)..."
        sh_ "echo 'hib freeze' >> $EXT/hib_ctl.txt"
        if l=$(wait_for "$n" 90 'hibernate: frozen after the snapshot' 'not frozen|no snapshot'); then echo "  $l"; frozen=1
        else echo "  not frozen: ${l:-no answer in 90 s}"; log_from "$n" | grep -E 'hibernate|hib ' | tail -3 | sed 's/^/  /'; fi
    else echo "the game is running without hibernation (DM_HIBERNATE was off when it started): no snapshot can be made"; fi
else echo "the game is not running: install only"; fi
if [ $frozen = 0 ] && [ -z "${FORCE:-}" ] && sh_ "pidof $PKG:game" | grep -q .; then echo "stopping here (FORCE=1 installs anyway; the game then starts from its menu)"; exit 1; fi

echo "installing $APK..."
"${A[@]}" install -r "$APK" || { echo "install failed"; [ $frozen = 1 ] && echo "the game is still frozen: echo 'hib thaw' >> $EXT/hib_ctl.txt lets it go on"; exit 1; }
[ $frozen = 1 ] || exit 0

echo "starting the game..."
ino=$(sh_ "stat -c %i $LOG")
sh_ "am start -n $PKG/com.nxnative.MenuActivity" >/dev/null
for t in $(seq 1 60); do [ "$(sh_ "stat -c %i $LOG")" != "$ino" ] && break; sleep 1; done   # the new process's log (the old one becomes log.prev.txt)
if l=$(wait_for 1 120 'hibernate: restored|the main thread resumes' 'not restored|no snapshot at'); then echo "resumed: $l"
else echo "not resumed: ${l:-no answer in 120 s} (the menu shows; Continue loads the game's own save)"; exit 1; fi
