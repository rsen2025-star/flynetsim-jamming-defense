# FlyNetSim jamming/defense testbed — ns-3 + bridge environment.
#
# Scope: this image builds the ns-3.27 network stack and the MAVLink-ZMQ bridge,
# which together produce every measured result in the paper (ns-3 FlowMonitor).
# ArduPilot SITL and QGroundControl are intentionally NOT in this image: they are
# heavy, GUI-bound, and do not affect the reported loss/delay/jitter numbers.
# Run ArduPilot separately (see README, "ArduPilot") pinned at Copter-4.0.0.
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
# python3 runs the bridge and plotting scripts.

# Bridge + plotting deps. pip 9 (bionic default) is too old for pyzmq 25, so bump it first.
COPY env/requirements.txt /tmp/requirements.txt
RUN python3 -m pip install --upgrade "pip<21" \
 && python3 -m pip install --no-cache-dir -r /tmp/requirements.txt

# Bring in the repo and build ns-3 (downloads ns-3.27, fetches FlyNetSim verbatim
# files, applies patches, drops in scratch/flynetsim, runs ./waf).
WORKDIR /opt/flynetsim
COPY . /opt/flynetsim/
RUN scripts/setup.sh

# ns-3 build tree lives here after setup.sh
WORKDIR /opt/flynetsim/ns-allinone-3.27/ns-3.27
CMD ["/bin/bash"]
