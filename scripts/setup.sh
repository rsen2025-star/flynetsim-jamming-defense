#!/usr/bin/env bash
#
# setup.sh — build ReCoST: fetch ns-3.27, overlay this project, compile.
#
# Steps:
#   1. Download ns-3.27 from nsnam.
#   2. Clone FlyNetSim, for the two files we reuse unchanged (myInput.cc/.h) and
#      its wscript patch. We fetch rather than re-host them — see NOTICE.
#   3. Apply FlyNetSim's wscript patch and our mac-low hop-assertion patch.
#      (Upstream's packet-sink.h patch is skipped on purpose — see step [3/5].)
#   4. Assemble scratch/flynetsim from our sources plus the fetched files.
#   5. Run ./waf configure && ./waf.
#
# Run from the repository root:  scripts/setup.sh
# (The Dockerfile calls it for you.)
#
# Needs: wget, git, a C++ toolchain, the ns-3 link libs (czmq/zmq/xml2/sqlite),
# and python2.7 — ns-3.27's ./waf runs under `env python`. See env/Dockerfile.

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
# FlyNetSim's top-level wscript patch (as in upstream net_init.sh).
cp -n "$NS3_HOME/wscript" "$NS3_HOME/wscript_original" || true
patch -N "$NS3_HOME/wscript" -i "$PATCHES/wscript.patch" || true
# Upstream also patches packet-sink.h, retyping its Rx trace to
# TracedCallback<Ptr<Packet>, Address&>. We skip that on purpose: our RcvPacket()
# takes the stock Ptr<const Packet>, const Address& — the mismatch would make ns-3
# abort at startup. The paper's results all use the stock packet-sink.h.

# Our patch: clamps a negative ACK duration that ns-3 asserts on during the hop.
patch -N -p1 -d "$NS3_HOME" < "$HERE/ns3/patches/mac-low-hop-assert.patch" || true

echo "==> [4/5] Assemble scratch/flynetsim"
SCRATCH="$NS3_HOME/scratch/flynetsim"
mkdir -p "$SCRATCH"
cp "$HERE/ns3/scratch/flynetsim/uav-net-sim.cc" "$SCRATCH/"
cp "$HERE/ns3/scratch/flynetsim/myApps.cc"      "$SCRATCH/"
cp "$HERE/ns3/scratch/flynetsim/myApps.h"       "$SCRATCH/"
cp "$HERE/ns3/scratch/flynetsim/wscript"        "$SCRATCH/"
# Unchanged from FlyNetSim — fetched, not stored in this repo:
cp "$CLONE/NetSim/uav-net-sim/myInput.cc"       "$SCRATCH/"
cp "$CLONE/NetSim/uav-net-sim/myInput.h"        "$SCRATCH/"
# Starting config — edit this before each run (rate, threshold).
cp "$HERE/ns3/scratch/flynetsim/config.example.xml" "$NS3_HOME/config.xml"

echo "==> [5/5] Build ns-3 (this can take a while)"
cd "$NS3_HOME"
./waf configure
./waf

echo ""
echo "Done. Binary: $NS3_HOME/build/scratch/flynetsim/flynetsim"
echo "Edit $NS3_HOME/config.xml, then run from $NS3_HOME:  ./waf --run flynetsim"
