#!/bin/sh
# Downloads the public test ROMs into tests/roms (not committed: Blargg's ROMs
# carry no explicit licence; mooneye's are MIT but kept out for the same
# layout). Every download is pinned and checksummed.
set -eu
cd "$(dirname "$0")"
mkdir -p roms
cd roms

fetch() { # url file sha256
    if [ -f "$2" ] && echo "$3  $2" | sha256sum -c --status; then return; fi
    # Release assets come through the API (the github.com download links
    # have been unreliable); GITHUB_TOKEN, when set, avoids its rate limit.
    set -- "$1" "$2" "$3" -H "Accept: application/octet-stream"
    if [ -n "${GITHUB_TOKEN:-}" ]; then set -- "$@" -H "Authorization: Bearer $GITHUB_TOKEN"; fi
    url=$1 file=$2 sum=$3
    shift 3
    case "$url" in https://api.github.com/*) ;; *) set -- ;; esac
    curl -fsSL --retry 6 --retry-delay 10 "$@" -o "$file.tmp" "$url"
    echo "$sum  $file.tmp" | sha256sum -c --quiet
    mv "$file.tmp" "$file"
}

BLARGG=c240dd7d700e5c0b00a7bbba52b53e4ee67b5f15
fetch "https://codeload.github.com/retrio/gb-test-roms/tar.gz/$BLARGG" blargg.tar.gz \
    1e121842bc380cc69ec4d5946335c0e6656926af8a794ccc732d1b293f29e411
if [ ! -d blargg ]; then
    mkdir blargg
    tar xzf blargg.tar.gz -C blargg --strip-components=1
fi

ACID=8a98ce731f96dde032ffb22ec36dc985d78fdb18
# dmg-acid2 v1.0 release asset
fetch https://api.github.com/repos/mattcurrie/dmg-acid2/releases/assets/19942984 dmg-acid2.gb \
    464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1
fetch "https://raw.githubusercontent.com/mattcurrie/dmg-acid2/$ACID/img/reference-dmg.png" dmg-acid2.png \
    ca966d50895c7efef05838590d148c2cbfd7fba57dab986f25b35b4da71abb57

MTS=mts-20260714-0944-31510e1
fetch "https://gekkio.fi/files/mooneye-test-suite/$MTS/$MTS.tar.gz" mooneye.tar.gz \
    5f0694e4e9e58441db23ff057a5ab681870d4cfaed91267e99885936db8e6f1d
if [ ! -d mooneye ]; then
    mkdir mooneye
    tar xzf mooneye.tar.gz -C mooneye --strip-components=1
fi
fetch "https://raw.githubusercontent.com/Gekkio/mooneye-test-suite/31510e12eea6286d36eea060a6adde755e1067aa/manual-only/sprite_priority-expected.png" \
    sprite_priority.png e08708fd0e5fc7df8650314707ec0ce0c929b969f2e19b6f2860c1e82828e9a0

MB=70e88fb90b59d19dfbb9c3ac36c64105202bb1f4
fetch "https://raw.githubusercontent.com/mattcurrie/mealybug-tearoom-tests/$MB/mealybug-tearoom-tests.zip" mealybug.zip \
    e2fa6ca96ad48bd64d7297fda042db04c15117a730da3bbd590f139e89b150bd
if [ ! -d mealybug ]; then
    mkdir mealybug
    python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" mealybug.zip mealybug
fi
for t in m2_win_en_toggle:b3125f1cfedaa0dd0ff8f02b9b44acf2343af9154d05dfd8788b98a0fc153935 \
         m3_bgp_change:c46133683a51f6eed3c79deb99cb0aa516f147918276c73f335c2b7d5d821f98 \
         m3_scx_low_3_bits:a05cd516b63ee547f6a8fa373bdecf46308baa5806be7f55f3eba207c3c03d21; do
    fetch "https://raw.githubusercontent.com/mattcurrie/mealybug-tearoom-tests/$MB/expected/DMG-blob/${t%%:*}.png" \
        "mealybug/${t%%:*}.png" "${t#*:}"
done
