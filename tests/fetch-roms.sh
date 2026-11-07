#!/bin/sh
# Downloads the public test ROMs into tests/roms (not committed: Blargg's ROMs
# carry no explicit licence). Every download is pinned and checksummed.
set -eu
cd "$(dirname "$0")"
mkdir -p roms
cd roms

fetch() { # url file sha256
    if [ -f "$2" ] && echo "$3  $2" | sha256sum -c --status; then return; fi
    curl -fsSL --retry 3 -o "$2.tmp" "$1"
    echo "$3  $2.tmp" | sha256sum -c --quiet
    mv "$2.tmp" "$2"
}

BLARGG=c240dd7d700e5c0b00a7bbba52b53e4ee67b5f15
fetch "https://codeload.github.com/retrio/gb-test-roms/tar.gz/$BLARGG" blargg.tar.gz \
    1e121842bc380cc69ec4d5946335c0e6656926af8a794ccc732d1b293f29e411
if [ ! -d blargg ]; then
    mkdir blargg
    tar xzf blargg.tar.gz -C blargg --strip-components=1
fi

ACID=8a98ce731f96dde032ffb22ec36dc985d78fdb18
fetch https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb dmg-acid2.gb \
    464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1
fetch "https://raw.githubusercontent.com/mattcurrie/dmg-acid2/$ACID/img/reference-dmg.png" dmg-acid2.png \
    ca966d50895c7efef05838590d148c2cbfd7fba57dab986f25b35b4da71abb57
