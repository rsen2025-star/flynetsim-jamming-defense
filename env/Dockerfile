# ReCoST — UAV Wi-Fi jamming / channel-hopping defense testbed.
# ns-3 + bridge environment.
#
# This image holds ns-3.27 and the MAVLink-ZMQ bridge — together they produce
# every measured result in the paper (via ns-3 FlowMonitor).
#
# ArduPilot SITL and QGroundControl are deliberately left out: both are heavy and
# GUI-bound, and neither affects the reported loss, delay, or jitter. Install
# ArduPilot separately at Copter-4.0.0 — see the README.
#
# Distrobox can consume this same image:
#   distrobox create --image flynetsim-env:latest --name flynetsim-env
#
# Build (from the repository root):
#   docker build -t flynetsim-env:latest -f env/Dockerfile .

FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive

# Build toolchain + ns-3.27 link libraries (czmq/zmq/xml2/sqlite) — versions match
# the reference container: libczmq-dev 4.1.0, libzmq3-dev 4.2.5, libxml2-dev 2.9.4.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++-7 gcc-7 cmake ccache pkg-config \
      git curl wget unzip ca-certificates \
      libczmq-dev libzmq3-dev libxml2-dev libxslt1-dev \
      libsqlite3-dev libssl-dev zlib1g-dev \
      python python-dev \
      python3 python3-dev python3-pip python3-setuptools \
 && rm -rf /var/lib/apt/lists/*
# NOTE: python (2.7) is required by ns-3.27's ./waf (its shebang is `env python`);
# python3 runs the bridge and the results parser.

# Python deps. pip 9 (bionic default) is too old for pyzmq 25, so bump it first.
COPY env/requirements.txt /tmp/requirements.txt
RUN python3 -m pip install --upgrade "pip<21" \
 && python3 -m pip install --no-cache-dir -r /tmp/requirements.txt

# Copy the repo in and build. setup.sh downloads ns-3.27, fetches the FlyNetSim
# files we reuse, applies the patches, adds scratch/flynetsim, and runs ./waf.
# (.dockerignore keeps .git and any local ns-3 tree out of the build context.)
WORKDIR /opt/flynetsim
COPY . /opt/flynetsim/
RUN scripts/setup.sh

# ns-3 build tree lives here after setup.sh
WORKDIR /opt/flynetsim/ns-allinone-3.27/ns-3.27
CMD ["/bin/bash"]
