#!/usr/bin/env bash
#
# setup.sh — fetch ns-3.27, overlay this project, and build.
#
# What it does:
#   1. Downloads ns-3.27 (nsnam release).
#   2. Clones the original FlyNetSim, whose files we reuse VERBATIM (myInput.cc/.h)
#      and whose ns-3 patches we apply. We do not re-host those files here; they are
#      fetched from the upstream repository at build time. See NOTICE.
#   3. Applies FlyNetSim's ns-3 patches (top-level wscript + packet-sink.h), matching
#      upstream net_init.sh, plus our mac-low hop-assertion patch.
#   4. Assembles scratch/flynetsim from our sources + the fetched FlyNetSim files.
#   5. Runs ./waf configure && ./waf.
#
# Run from the repository root (or let the Dockerfile call it):  scripts/setup.sh
#
# Requires: wget, git, a C++ toolchain, the ns-3 link libs (czmq/zmq/xml2/sqlite),
# and python2.7 (ns-3.27's ./waf runs under `env python`). See env/Dockerfile.

set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"   # repository root
BUILD="$HERE"                              # ns-3 is installed under the repo root
NS3VER="ns-allinone-3.27"
NS3_HOME="$BUILD/$NS3VER/ns-3.27"
FLYNETSIM_URL="https://github.com/saburhb/FlyNetSim.git"
CLONE="$BUILD/_flynetsim_upstream"

echo "==> [1/5] Download ns-3.27"
cd "$BUILD"
[ -f "$NS3VER.tar.bz2" ] || wget -q "https://www.nsnam.org/releases/$NS3VER.tar.bz2"
[ -d "$NS3VER" ]         || tar xf "$NS3VER.tar.bz2"

echo "==> [2/5] Clone original FlyNetSim (verbatim files + patches)"
[ -d "$CLONE" ] || git clone --depth 1 "$FLYNETSIM_URL" "$CLONE"
PATCHES="$CLONE/NetSim/patches"

echo "==> [3/5] Apply patches"
# FlyNetSim's ns-3 patches (same two applied by upstream net_init.sh)
cp -n "$NS3_HOME/wscript" "$NS3_HOME/wscript_original" || true
patch -N "$NS3_HOME/wscript" -i "$PATCHES/wscript.patch" || true
patch -N "$NS3_HOME/src/applications/model/packet-sink.h" -i "$PATCHES/packet-sink.h.patch" || true
# Our hop-assertion patch (clamps negative ACK duration during the channel hop)
patch -N -p1 -d "$NS3_HOME" < "$HERE/ns3/patches/mac-low-hop-assert.patch" || true

echo "==> [4/5] Assemble scratch/flynetsim"
SCRATCH="$NS3_HOME/scratch/flynetsim"
mkdir -p "$SCRATCH"
cp "$HERE/ns3/scratch/flynetsim/uav-net-sim.cc" "$SCRATCH/"
cp "$HERE/ns3/scratch/flynetsim/myApps.cc"      "$SCRATCH/"
cp "$HERE/ns3/scratch/flynetsim/myApps.h"       "$SCRATCH/"
cp "$HERE/ns3/scratch/flynetsim/wscript"        "$SCRATCH/"
# Verbatim from upstream FlyNetSim (not re-hosted in this repo):
cp "$CLONE/NetSim/uav-net-sim/myInput.cc"       "$SCRATCH/"
cp "$CLONE/NetSim/uav-net-sim/myInput.h"        "$SCRATCH/"
# Default run config
cp "$HERE/ns3/scratch/flynetsim/config.example.xml" "$NS3_HOME/config.xml"

echo "==> [5/5] Build ns-3 (this can take a while)"
cd "$NS3_HOME"
./waf configure
./waf

echo ""
echo "Done. Binary: $NS3_HOME/build/scratch/flynetsim/flynetsim"
echo "Edit $NS3_HOME/config.xml, then run from $NS3_HOME:  ./waf --run flynetsim"
