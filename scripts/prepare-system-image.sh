#!/bin/sh
# Puts the real Play services, Play Store and Google Services Framework APKs
# where the guest's package ledger expects them: <data root>/system/product/priv-app/.
# Every app of this data root then sees the same, signed system packages (a game's
# "requires Google Play services" signature check reads them there).
#
# usage: prepare-system-image.sh DATA_ROOT GMS.apk VENDING.apk GSF.apk
set -eu
[ $# -eq 4 ] || { echo "usage: $0 DATA_ROOT GMS.apk VENDING.apk GSF.apk" >&2; exit 2; }
root=$1
place() {  # source dir-name file-name
   [ -s "$1" ] || { echo "$0: $1 is not a file" >&2; exit 1; }
   mkdir -p "$root/system/product/priv-app/$2"
   cp -f "$1" "$root/system/product/priv-app/$2/$3"
}
place "$2" PrebuiltGmsCore PrebuiltGmsCore.apk
place "$3" Phonesky Phonesky.apk
place "$4" GoogleServicesFramework GoogleServicesFramework.apk
echo "system image ready in $root/system"
