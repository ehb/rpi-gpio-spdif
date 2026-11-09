#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Selftest of the LibreELEC add-on service.rpi-gpio-spdif: runs bin/rpi-gpio-spdif against a
# fake root with a fake dtoverlay (no root, nothing of the real system is touched), lets
# alsa-lib parse and use the configuration with the native (x86_64) plugin, and checks
# build_addon.py with the aarch64 build. Needs Linux, alsa-utils (aplay), libasound with the
# system alsa.conf, python3, unzip. Prints "ALL OK" only if every check passes.
#
#   TS_SO=<native plugin .so> TS_SO_A64=<aarch64 plugin .so> sh test/test_addon.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
LE=$ROOT/libreelec
SCRIPT=$LE/addon/bin/rpi-gpio-spdif
: "${TS_SO:?set TS_SO}" "${TS_SO_A64:?set TS_SO_A64}"
TS_SO=$(readlink -f "$TS_SO"); TS_SO_A64=$(readlink -f "$TS_SO_A64")

T=$(mktemp -d /tmp/spdif-addon.XXXXXX) || exit 1
cleanup() { rm -rf "$T"; }
trap cleanup EXIT

FAILS=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
check() { # check DESCRIPTION COMMAND... (runs the command, passes when it succeeds)
  d=$1; shift
  if "$@" > /dev/null 2>&1; then ok "$d"; else bad "$d"; fi
}
nocheck() { # passes when the command fails
  d=$1; shift
  if "$@" > /dev/null 2>&1; then bad "$d"; else ok "$d"; fi
}

# ---- fake add-on root, fake /proc, fake dtoverlay, path overrides ------------------------
A=$T/addon; D=$T/data; CONFD=$T/var/lib/alsa/conf.d; RUN=$T/run; PROC=$T/proc
mkdir -p "$A/bin" "$A/lib" "$D" "$RUN" "$PROC/asound"
cp "$SCRIPT" "$A/bin/rpi-gpio-spdif"
cp "$TS_SO" "$A/lib/libasound_module_pcm_spdifbmc.so"
: > "$PROC/modules"; : > "$PROC/asound/cards"

# dtoverlay: NAME applies, -r NAME removes, -l lists; applying hifiberry-dac makes the card
# appear unless the file $T/nocard exists. Every call is logged in $T/dt.log.
printf '%s\n' \
  '#!/bin/sh' \
  'echo "$*" >> "$FAKE_DIR/dt.log"' \
  'OV=$FAKE_DIR/overlays; touch "$OV"' \
  'case "$1" in' \
  '  -l) if [ -s "$OV" ]; then echo "Overlays (in load order):"; n=0; while read -r o; do echo "$n:  $o"; n=$((n + 1)); done < "$OV"; else echo "No overlays loaded"; fi ;;' \
  '  -r) grep -vx "$2" "$OV" > "$OV.new"; mv "$OV.new" "$OV"; [ "$2" = hifiberry-dac ] && : > "$FAKE_PROC/asound/cards" ;;' \
  '  *) echo "$1" >> "$OV"; [ "$1" = hifiberry-dac ] && [ ! -e "$FAKE_DIR/nocard" ] && echo " 0 [sndrpihifiberry ]: RPi-simple - snd_rpi_hifiberry_dac" > "$FAKE_PROC/asound/cards" ;;' \
  'esac' \
  'exit 0' > "$T/dtoverlay"
printf '%s\n' '#!/bin/sh' '[ -e "$FAKE_DIR/raspdif-running" ]' > "$T/pidof"
chmod +x "$T/dtoverlay" "$T/pidof"
export FAKE_DIR=$T FAKE_PROC=$PROC
export SPDIF_ADDON=$A SPDIF_SETTINGS=$D/settings.xml SPDIF_CONFD=$CONFD SPDIF_RUN=$RUN \
       SPDIF_PROC=$PROC SPDIF_DTOVERLAY=$T/dtoverlay SPDIF_PIDOF=$T/pidof SPDIF_CARD_WAIT=1
S="sh $A/bin/rpi-gpio-spdif"
CONF=$CONFD/50-rpi-gpio-spdif.conf
STATUS=$RUN/service.rpi-gpio-spdif.status
APPLIED=$RUN/service.rpi-gpio-spdif.applied

# the same load hook as LibreELEC's alsa.conf (the line "/var/lib/alsa/conf.d" of the stock file)
sed "s|\"/var/lib/alsa/conf.d\"|\"$CONFD\"|" /usr/share/alsa/alsa.conf > "$T/alsa.conf"
grep -q "$CONFD" "$T/alsa.conf" || bad "system alsa.conf has no /var/lib/alsa/conf.d entry"
export ALSA_CONFIG_PATH=$T/alsa.conf HOME=$T/home
mkdir -p "$HOME"

has() { grep -q -- "$1" "$2"; }
settings() { # settings 'xml lines'
  printf '<settings version="2">\n%s\n</settings>\n' "$1" > "$D/settings.xml"
}
dtcount() { grep -c -x -- "$1" "$T/dt.log" 2> /dev/null || true; }
reset_world() { # nothing applied, nothing loaded, no files
  rm -f "$CONF" "$CONF.tmp" "$STATUS" "$APPLIED" "$T/dt.log" "$T/overlays" "$T/nocard" "$T/raspdif-running"
  : > "$PROC/modules"; : > "$PROC/asound/cards"
}

# ---- static checks ----------------------------------------------------------------------
check "sh -n bin/rpi-gpio-spdif" sh -n "$SCRIPT"
check "sh -n test_addon.sh" sh -n "$HERE/test_addon.sh"
check "default.py compiles" python3 -c "import sys; compile(open(sys.argv[1]).read(), 'default.py', 'exec')" "$LE/addon/default.py"
for x in addon/addon.xml.in addon/resources/settings.xml; do
  check "xml well-formed: $x" python3 -c "import sys, xml.etree.ElementTree as E; E.parse(sys.argv[1])" "$LE/$x"
done
check "unit: Before=kodi.service" grep -q '^Before=kodi.service' "$LE/addon/system.d/service.rpi-gpio-spdif.service"
check "settings: only extra_ms, default 80, range 0..500" python3 -c "
import sys, xml.etree.ElementTree as E
r = E.parse(sys.argv[1]).getroot()
ids = [s.get('id') for s in r.iter('setting')]
s = next(r.iter('setting'))
ok = (ids == ['extra_ms'] and s.findtext('default') == '80'
      and s.findtext('constraints/minimum') == '0' and s.findtext('constraints/maximum') == '500')
sys.exit(0 if ok else 1)" "$LE/addon/resources/settings.xml"
check "settings labels exist in strings.po" python3 -c "
import sys, re
po = open(sys.argv[2]).read()
xml = open(sys.argv[1]).read()
need = set(re.findall(r'(?:label|help)=\"(\d+)\"', xml)) | set(re.findall(r'<heading>(\d+)</heading>', xml))
have = set(re.findall(r'msgctxt \"#(\d+)\"', po))
sys.exit(0 if need <= have else 1)" "$LE/addon/resources/settings.xml" "$LE/addon/resources/language/resource.language.en_gb/strings.po"
check "add-on files are ASCII" python3 -c "
import sys, os
for d, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        open(os.path.join(d, f), encoding='ascii').read()" "$LE/addon"

# ---- defaults (no settings file) --------------------------------------------------------
reset_world
$S start > "$T/out" 2>&1; rc=$?
[ $rc = 0 ] && ok "start with no settings file" || { bad "start with no settings file (rc $rc)"; cat "$T/out"; }
check "conf.d file written (directory created)" test -f "$CONF"
check "first line is the marker" sh -c "[ \"\$(head -n 1 '$CONF')\" = '# written by service.rpi-gpio-spdif' ]"
check "no tmp file left" test ! -e "$CONF.tmp"
check "plugin path points into the add-on" has "lib \"$A/lib/libasound_module_pcm_spdifbmc.so\"" "$CONF"
check "slave is the card" has 'slave.pcm "hw:CARD=sndrpihifiberry"' "$CONF"
check "default extra_ms 80" has '^  extra_ms 80$' "$CONF"
check "AES arguments with default -1" sh -c "[ \$(grep -c '^  @args.AES[0-3] { type integer default -1 }\$' '$CONF') = 4 ]"
check "aes0..3 passed on" sh -c "grep -qx '  aes0 \$AES0' '$CONF' && grep -qx '  aes3 \$AES3' '$CONF'"
check "hint description" has 'description "S/PDIF on GPIO21"' "$CONF"
check "name begins with hdmi" has '^pcm.hdmi_bmc {' "$CONF"
check "overlay hifiberry-dac applied once" sh -c "[ \"\$(grep -cx hifiberry-dac '$T/dt.log')\" = 1 ]"
check "applied settings stored" sh -c "[ \"\$(cat '$APPLIED')\" = extra_ms=80 ]"
nocheck "no status after a good start" test -e "$STATUS"
$S start > "$T/out" 2>&1
check "second start applies the overlay only once" sh -c "[ \"\$(grep -cx hifiberry-dac '$T/dt.log')\" = 1 ]"

# ---- the configuration parses and the device shows up -----------------------------------
check "aplay -L lists hdmi_bmc" sh -c "aplay -L 2>&1 | grep -qx hdmi_bmc"
check "aplay -L shows the hint text" sh -c "aplay -L 2>&1 | grep -q 'S/PDIF on GPIO21'"
printf 'pcm.junk_tmp {\n type null\n}\n' > "$CONFD/99-junk.conf.tmp"
nocheck "conf.d: a .tmp file is not read" sh -c "aplay -L 2>&1 | grep -qx junk_tmp"
rm -f "$CONFD/99-junk.conf.tmp"

# Play through the generated device with a file instead of the card: the slave line is the
# only thing replaced, so plugin path, arguments and extra_ms are those of the real file.
PLAY=$T/play; mkdir -p "$PLAY"
sed 's|slave.pcm "hw:CARD=sndrpihifiberry"|slave.pcm "sink"|' "$CONF" > "$PLAY/50-play.conf"
printf '</usr/share/alsa/alsa.conf>\n' > "$PLAY/asound.conf"
cat "$PLAY/50-play.conf" >> "$PLAY/asound.conf"
printf 'pcm.sink {\n type file\n slave.pcm "null"\n file "%s"\n format "raw"\n}\n' "$PLAY/out.raw" >> "$PLAY/asound.conf"
play() { # play DEVICE: one second of silence at 48 kHz into the file slave
  rm -f "$PLAY/out.raw"
  ALSA_CONFIG_PATH=$PLAY/asound.conf timeout 20 aplay -q -D "$1" -t raw -f S16_LE -r 48000 -c 2 -d 1 /dev/zero > "$PLAY/aplay.out" 2>&1
}
play hdmi_bmc; rc=$?
[ $rc = 0 ] && ok "aplay plays through hdmi_bmc" || { bad "aplay plays through hdmi_bmc (rc $rc)"; cat "$PLAY/aplay.out"; }
check "output holds the second plus the 80 ms lead, 16 bytes per frame" python3 -c "
import sys
n = len(open(sys.argv[1], 'rb').read())
sys.exit(0 if n >= (48000 + 3840) * 16 and n % 16 == 0 else 1)" "$PLAY/out.raw"
play hdmi_bmc:6,130,0,2
check "AES0 from the player ends up in the channel status" python3 -c "
import sys
sys.path.insert(0, sys.argv[1])
import bmc_decode as bmc
got, csa, csb, errors = bmc.decode(open(sys.argv[2], 'rb').read())
b0 = sum(csa[j] << j for j in range(8))
sys.exit(0 if not errors and b0 == 6 else 1)" "$HERE" "$PLAY/out.raw"

# ---- settings ---------------------------------------------------------------------------
settings '    <setting id="extra_ms" default="false">200</setting>'
$S start > "$T/out" 2>&1
check "extra_ms setting applied" has '^  extra_ms 200$' "$CONF"
check "applied settings follow" sh -c "[ \"\$(cat '$APPLIED')\" = extra_ms=200 ]"
settings '    <setting id="extra_ms">  0  </setting>'
$S start > "$T/out" 2>&1
check "extra_ms 0 is valid, whitespace trimmed" has '^  extra_ms 0$' "$CONF"
settings '    <setting id="extra_ms">500</setting>'
$S start > "$T/out" 2>&1
check "extra_ms 500 is valid" has '^  extra_ms 500$' "$CONF"
settings '    <setting id="extra_ms">070</setting>'
$S start > "$T/out" 2>&1
check "extra_ms 070 is 70" has '^  extra_ms 70$' "$CONF"
for bad_val in 501 abc -5 1.5 "1 2" 99999 "8;x" "\$x"; do
  settings "    <setting id=\"extra_ms\">$bad_val</setting>"
  $S start > "$T/out" 2>&1
  check "extra_ms '$bad_val' -> default" has '^  extra_ms 80$' "$CONF"
  check "extra_ms '$bad_val' is said in the log" has 'using 80' "$T/out"
done
settings '    <setting id="extra_ms" />
    <setting id="other">9</setting>'
$S start > "$T/out" 2>&1
check "empty entry -> default" has '^  extra_ms 80$' "$CONF"
printf 'garbage, not xml' > "$D/settings.xml"
check "garbage settings file -> default, start succeeds" $S start
rm -f "$D/settings.xml"

# applied vs current: default.py restarts the unit only for a real change
settings '    <setting id="extra_ms">120</setting>'
$S start > "$T/out" 2>&1
check "current equals applied after start" sh -c "[ \"\$(sh '$A/bin/rpi-gpio-spdif' current)\" = \"\$(cat '$APPLIED')\" ]"
settings '    <setting id="extra_ms">120</setting>
    <setting id="unrelated">x</setting>'
check "an irrelevant settings change is no change" sh -c "[ \"\$(sh '$A/bin/rpi-gpio-spdif' current)\" = \"\$(cat '$APPLIED')\" ]"
settings '    <setting id="extra_ms">130</setting>'
nocheck "a changed value differs from the applied settings" sh -c "[ \"\$(sh '$A/bin/rpi-gpio-spdif' current)\" = \"\$(cat '$APPLIED')\" ]"
rm -f "$D/settings.xml"

# ---- refusals leave a reason ------------------------------------------------------------
reset_world
printf 'rpi_gpio_spdif 20480 0 - Live 0x0000000000000000\nsnd 98304 3 snd_pcm, Live 0x0\n' > "$PROC/modules"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start refuses when the kernel module is loaded" || bad "start refuses when the kernel module is loaded"
check "reason: kernel driver add-on is active" grep -q 'kernel driver add-on is active: remove it first' "$STATUS"
nocheck "no configuration written" test -e "$CONF"
nocheck "no overlay applied" test -s "$T/overlays"
reset_world
printf 'snd_rpi_gpio_spdif_other 1 0 - Live 0x0\n' > "$PROC/modules"
$S start > "$T/out" 2>&1; rc=$?
[ $rc = 0 ] && ok "a module with a similar name does not count" || bad "a module with a similar name does not count"

reset_world
echo rpi-gpio-spdif > "$T/overlays"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start refuses when the overlay rpi-gpio-spdif is active" || bad "start refuses when the overlay rpi-gpio-spdif is active"
check "reason: kernel driver add-on is active (overlay)" grep -q 'kernel driver add-on is active' "$STATUS"
check "the driver's overlay is left alone" grep -qx rpi-gpio-spdif "$T/overlays"
SERVICE_RESULT=exit-code $S stop
check "stop after the refusal keeps the status file" test -f "$STATUS"
check "stop after the refusal leaves the driver's overlay" grep -qx rpi-gpio-spdif "$T/overlays"
SERVICE_RESULT=success $S stop
nocheck "clean stop removes the status file" test -e "$STATUS"

reset_world
touch "$T/raspdif-running"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start refuses when raspdif runs" || bad "start refuses when raspdif runs"
check "reason names raspdif" grep -q 'raspdif is running' "$STATUS"
nocheck "raspdif: no overlay applied" test -s "$T/overlays"

reset_world
touch "$T/nocard"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start fails when the card does not appear" || bad "start fails when the card does not appear"
check "reason names the card" grep -q 'sndrpihifiberry did not appear' "$STATUS"
nocheck "no configuration without the card" test -e "$CONF"
SERVICE_RESULT=exit-code $S stop
check "stop after the failure removes the overlay" sh -c "! grep -qx hifiberry-dac '$T/overlays'"
check "stop after the failure keeps the reason" test -f "$STATUS"
rm -f "$STATUS"

reset_world
mv "$A/lib/libasound_module_pcm_spdifbmc.so" "$T/so.keep"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start fails without the plugin" || bad "start fails without the plugin"
check "reason: plugin missing" grep -q 'plugin missing' "$STATUS"
mv "$T/so.keep" "$A/lib/libasound_module_pcm_spdifbmc.so"
$S start > "$T/out" 2>&1
nocheck "status removed by a good start" test -e "$STATUS"

# a file under our name that we did not write: refused, left alone, also by stop
reset_world
mkdir -p "$CONFD"
printf '# mine, not yours\n' > "$CONF"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start refuses to overwrite a foreign file" || bad "start refuses to overwrite a foreign file"
check "the foreign file is untouched" sh -c "[ \"\$(cat '$CONF')\" = '# mine, not yours' ]"
check "reason: refusing to overwrite" grep -q 'refusing to overwrite' "$STATUS"
nocheck "nothing applied before the refusal" test -s "$T/overlays"
SERVICE_RESULT=exit-code $S stop
check "stop leaves the foreign file" test -f "$CONF"
rm -f "$CONF" "$STATUS"
# the file the kernel driver add-on writes under the same name is not ours either
printf '# written by driver.audio.rpi-gpio-spdif\npcm.SPDIFout { type hw card SPDIFout }\n' > "$CONF"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start refuses the kernel driver add-on's file" || bad "start refuses the kernel driver add-on's file"
check "the driver add-on's file is untouched" has 'driver.audio.rpi-gpio-spdif' "$CONF"
$S stop
check "stop leaves the driver add-on's file" test -f "$CONF"
rm -f "$CONF" "$STATUS"

# ---- stop removes everything ------------------------------------------------------------
reset_world
rm -rf "$CONFD"
$S start > "$T/out" 2>&1
check "overlay is applied" grep -qx hifiberry-dac "$T/overlays"
$S stop
nocheck "stop removes the conf.d file" test -e "$CONF"
nocheck "stop removes the applied file" test -e "$APPLIED"
nocheck "stop removes the empty conf.d directory" test -d "$CONFD"
nocheck "stop removes the overlay" grep -qx hifiberry-dac "$T/overlays"
nocheck "device is gone" sh -c "aplay -L 2>&1 | grep -qx hdmi_bmc"
check "stop twice is fine" $S stop
n1=$(dtcount "-r hifiberry-dac")
$S stop
[ "$(dtcount '-r hifiberry-dac')" = "$n1" ] && ok "stop does not remove an overlay that is not applied" || bad "stop does not remove an overlay that is not applied"
# another add-on's file in the directory
mkdir -p "$CONFD"; printf '# other\n' > "$CONFD/60-alsa-leadtap.conf"
$S start > "$T/out" 2>&1
$S stop
check "stop leaves other add-ons' files" test -f "$CONFD/60-alsa-leadtap.conf"
check "stop leaves the directory when it is not empty" test -d "$CONFD"
rm -f "$CONFD/60-alsa-leadtap.conf"
printf 'x' > "$STATUS"
$S stop
nocheck "clean stop removes the status file" test -e "$STATUS"
check "stop exits 0 even without a dtoverlay" env SPDIF_DTOVERLAY=/nonexistent $S stop
check "unknown command is a usage error" sh -c "! $S bogus"

# ---- build_addon.py ---------------------------------------------------------------------
B="python3 $LE/build_addon.py --out $T/out-zip"
$B --so "$TS_SO" > "$T/b.out" 2>&1; rc=$?
[ $rc != 0 ] && grep -q AArch64 "$T/b.out" && ok "wrong architecture refused" || bad "wrong architecture refused"
printf 'not an elf' > "$T/junk.bin"
$B --so "$T/junk.bin" > "$T/b.out" 2>&1; rc=$?
[ $rc != 0 ] && grep -q 'not an ELF' "$T/b.out" && ok "non-ELF refused" || bad "non-ELF refused"
nocheck "no zip after refusals" test -e "$T/out-zip"
$B --so "$TS_SO_A64" --version 1.x > "$T/b.out" 2>&1; rc=$?
[ $rc != 0 ] && ok "bad version refused" || bad "bad version refused"
$B --so "$TS_SO_A64" --version 9.8.7 > "$T/b.out" 2>&1; rc=$?
[ $rc = 0 ] && ok "aarch64 build packs" || { bad "aarch64 build packs"; cat "$T/b.out"; }
Z=$T/out-zip/service.rpi-gpio-spdif-9.8.7.zip
check "zip named after id and version" test -f "$Z"
mkdir -p "$T/unz"; unzip -q -o "$Z" -d "$T/unz" 2> /dev/null
P=$T/unz/service.rpi-gpio-spdif
for f in addon.xml default.py LICENSE.txt bin/rpi-gpio-spdif lib/libasound_module_pcm_spdifbmc.so \
         system.d/service.rpi-gpio-spdif.service resources/settings.xml \
         resources/language/resource.language.en_gb/strings.po; do
  check "zip has $f" test -f "$P/$f"
done
check "zip addon.xml: id, version, license" sh -c "grep -q 'id=\"service.rpi-gpio-spdif\"' '$P/addon.xml' && grep -q 'version=\"9.8.7\"' '$P/addon.xml' && grep -q 'GPL-2.0-only' '$P/addon.xml' && ! grep -q @VERSION@ '$P/addon.xml'"
check "zip addon.xml well-formed" python3 -c "import sys, xml.etree.ElementTree as E; E.parse(sys.argv[1])" "$P/addon.xml"
check "zip: script is executable, others not" python3 -c "
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
m = {i.filename.split('/', 1)[1]: (i.external_attr >> 16) & 0o111 for i in z.infolist()}
sys.exit(0 if m['bin/rpi-gpio-spdif'] and not m['default.py'] and not m['lib/libasound_module_pcm_spdifbmc.so'] else 1)" "$Z"
check "zip: plugin is the aarch64 one" cmp "$P/lib/libasound_module_pcm_spdifbmc.so" "$TS_SO_A64"
check "zip: script is the one under test" cmp "$P/bin/rpi-gpio-spdif" "$SCRIPT"
$B --so "$TS_SO_A64" > "$T/b.out" 2>&1
CLV=$(sed -n 's/^## \([0-9][0-9.]*\)$/\1/p' "$ROOT/CHANGELOG.md" | head -n 1)
check "default version is the newest CHANGELOG entry ($CLV)" test -f "$T/out-zip/service.rpi-gpio-spdif-$CLV.zip"

echo
if [ "$FAILS" = 0 ]; then echo "ALL OK"; else echo "$FAILS check(s) FAILED"; exit 1; fi
