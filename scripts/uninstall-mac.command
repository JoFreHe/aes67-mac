#!/bin/bash
# User-invoked removal; preserve saved streams and installation backups.
set -euo pipefail
printf '%s\n' 'AES67-Treiber und Manager entfernen? Gespeicherte Konfiguration bleibt erhalten.'
read -r -p 'Zum Entfernen ENTFERNEN eingeben: ' reply
[ "$reply" = ENTFERNEN ] || exit 0
sudo /bin/launchctl bootout system/org.communityaudio.aes67.clock 2>/dev/null || true
sudo /bin/rm -f '/Library/LaunchDaemons/org.communityaudio.aes67.clock.plist' '/Library/Application Support/AES67Driver/AES67ClockService' '/Library/Application Support/AES67Driver/clock-status.txt'
sudo /bin/rm -rf /Library/Audio/Plug-Ins/HAL/AES67Driver.driver /Applications/AES67Manager.app
printf '%s\n' 'Entfernt. Bitte den Mac neu starten, damit Core Audio den Treiber entlädt.'
