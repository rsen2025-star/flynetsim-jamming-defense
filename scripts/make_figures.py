#!/usr/bin/env python3
"""
make_figures.py — regenerate the telemetry-throughput figures (Figs 2 and 3 in the
paper: io_jam_5mbps.png and io_def_5mbps.png).

STATUS: PLACEHOLDER.

The two figures plot telemetry throughput (packets/second) over time for the 5.0 Mbps
per-jammer run, without defense (Fig 2) and with the channel-hop defense (Fig 3). The
throughput-over-time series is NOT in the per-flow FlowMonitor CSVs under data/ (those
hold summary loss/delay/jitter). It is derived from a time series of received packets.

TODO — wire this to the actual source used to produce figures/io_*_5mbps.png, one of:
  (a) the ns-3 pcap (uav-flight-*.pcap): bin telemetry packets (UAV->AP, port 100) into
      1-second windows and plot the per-window count; or
  (b) an existing throughput log, if the run recorded one.

Once the source is known, this script should read it, bin into per-second throughput,
and write figures/io_jam_5mbps.png and figures/io_def_5mbps.png with matplotlib.
"""

import sys


def main():
    print(__doc__)
    print("make_figures.py is a placeholder; see the TODO above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
