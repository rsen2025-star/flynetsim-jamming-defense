# A Reproducible Co-Simulation Testbed for UAV Wi-Fi Jamming and Channel-Hopping Defense

Artifact for the IEEE CCNC 2027 (Testbed Track) paper. It couples **ArduPilot SITL**,
**ns-3.27**, and **QGroundControl** through a four-thread **MAVLink-to-ZeroMQ bridge**,
re-purposes five congesting Wi-Fi stations as a **jammer**, and adds a **reactive
channel-hopping defense** inside the ns-3 binary. The whole network + bridge stack runs
in a single container for end-to-end reproduction.

This work **extends FlyNetSim** (Baidya et al., ACM MSWiM 2018). See [`NOTICE`](NOTICE)
for exactly which files are new, modified, or reused.

## Architecture

![architecture](figures/System.pdf)

- **Flight plane:** ArduPilot SITL (Copter firmware) + QGroundControl on the host.
- **Network plane:** ns-3.27, real-time mode, 802.11g. One AP, one UAV, five jammer
  stations on `10.10.1.0/24`. Defense module hops the UAV from channel 1 to channel 6
  when application-layer loss crosses a threshold.
- **Control plane:** `bridge/mavlink_zmq_bridge.py`, four worker threads carrying MAVLink
  between ArduPilot (UDP) and ns-3 (ZeroMQ).

## Repository layout

```
ns3/scratch/flynetsim/   ns-3 program: uav-net-sim.cc, myApps.*, wscript, config.example.xml
ns3/patches/             mac-low-hop-assert.patch (applied by setup.sh)
bridge/                  mavlink_zmq_bridge.py
scripts/                 setup.sh, master_parser.py
env/                     Dockerfile, requirements.txt
data/                    per-run CSV results (see data/README.md)
figures/                 System.pdf (architecture diagram)
```

## Requirements

The **ns-3 + bridge** stack (which produces every measured result) runs in the container
built from [`env/Dockerfile`](env/Dockerfile): Ubuntu 18.04, Python 3.6, gcc 7,
`libczmq/libzmq/libxml2/libsqlite3`, `pymavlink` and `pyzmq`.

**ArduPilot** is *not* in the container (it is heavy and does not affect the reported
numbers). Install it separately, pinned to the version used in the paper:

```
git clone https://github.com/ArduPilot/ardupilot
cd ardupilot && git checkout Copter-4.0.0   # commit 49693540
git submodule update --init --recursive
```

**QGroundControl** runs on the host and is only needed for manual flight.

## Build

```
docker build -t flynetsim-env:latest -f env/Dockerfile .
```

This downloads ns-3.27, fetches the verbatim FlyNetSim files, applies the patches, drops
in `scratch/flynetsim`, and runs `./waf`. To build without Docker (e.g. inside a Distrobox
container on the same base), run `scripts/setup.sh` from the repo root. Distrobox can also
consume the image directly: `distrobox create --image flynetsim-env:latest --name flynetsim-env`.

## Reproducing the measurements

Results come from the ns-3 FlowMonitor, independent of ArduPilot/QGC. One `config.xml`
drives one operating point; its fields are documented in
[`ns3/scratch/flynetsim/config.example.xml`](ns3/scratch/flynetsim/config.example.xml):

| field | meaning |
|---|---|
| `count` | number of jammer stations (paper: 5) |
| `rate`  | per-jammer offered load in Mbps (sweep: 0, 0.5, 1.0, 5.0, 10.0) |
| `size`  | jammer packet size in bytes (paper: 800) |
| `threshold` | loss % that triggers the hop: **30 = defense ON**, **101 = defense OFF** |

Each run is 300 simulated seconds. ns-3 uses the default RNG seed and run number, so the
discrete-event substrate is fixed; run-to-run variation comes from real-time MAVLink
arrival timing. **Repeat each point (the paper uses N = 6) and average.**

Run each component by hand, one per terminal, in this order (ArduPilot before the bridge,
the bridge before ns-3):

**Terminal 1 — ArduPilot SITL** (from the ArduPilot checkout). Wait for "Ready to fly":

```
cd ardupilot/ArduCopter
sim_vehicle.py -v ArduCopter -f quad --no-extra-ports --out 127.0.0.1:14560
```

**Terminal 2 — MAVLink-ZMQ bridge** (from the repo root):

```
python3 bridge/mavlink_zmq_bridge.py
```

**Terminal 3 — ns-3** (from `ns-allinone-3.27/ns-3.27`). Edit `config.xml` first to set the
per-jammer `rate` and the `threshold` (30 = defense on, 101 = off):

```
PATH=/usr/bin:$PATH ./waf --run flynetsim
```

ns-3 prints per-window loss every 5 s, writes `flynetsim-results.xml` (FlowMonitor) after
300 s, and exits.

**Terminal 4 — QGroundControl** (host, optional): only for manual flight; it listens on
UDP 14550. The measured results do not depend on it.

**Parse the results to CSV.** From the `ns-3.27` folder (where `flynetsim-results.xml` was
written), run the parser and enter a name when prompted:

```
python3 /path/to/repo/scripts/master_parser.py
```

It writes `<name>.csv` with per-flow loss, mean delay, and jitter; telemetry is the
`DRONE`-flagged row. Repeat per rate and threshold. The CSVs behind the paper's tables are
in [`data/`](data/README.md).

## Citation and license

Please cite the paper (see [`CITATION.cff`](CITATION.cff)). This repository is licensed
under **GPL v2** ([`LICENSE`](LICENSE)), consistent with ns-3. It extends FlyNetSim; see
[`NOTICE`](NOTICE) for full attribution.
