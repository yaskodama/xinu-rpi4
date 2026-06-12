#!/bin/sh
# Usage: ./test.sh [host]   (default 192.168.3.100)
# Run after flashing kernel8.img to the Pi 4 and booting it.
H="${1:-192.168.3.100}"
sudo arp -s "$H" d8:3a:dd:a7:fd:bf 2>/dev/null
echo "== inline =="; curl -s --data-binary 'int main(){ puts("hi over HTTP"); return 7; }' "http://$H/compile"
echo "== fib ==";    curl -s --data-binary @"$(dirname "$0")/fib.c"    "http://$H/compile"
echo "== strlen =="; curl -s --data-binary @"$(dirname "$0")/strlen.c" "http://$H/compile"
echo "== actor ==";  curl -s --data-binary @"$(dirname "$0")/actor.c"  "http://$H/compile"
echo "== index ==";  curl -s "http://$H/"
