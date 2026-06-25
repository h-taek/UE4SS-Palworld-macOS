#!/bin/bash
# Tail the spike log inside the Palworld sandbox container.
LOG="$HOME/Library/Containers/com.pocketpair.palworld.mac/Data/ue4ss-mac-spike.log"
echo "tailing $LOG"
touch "$LOG" 2>/dev/null || true
tail -f "$LOG"
