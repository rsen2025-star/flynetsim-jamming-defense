# Data

Per-run results from the ns-3 FlowMonitor, parsed to CSV by
`scripts/master_parser.py`.

Layout:

```
data/baseline/0.0mbps/               run0.csv, run1.csv   (no jammer)
data/jamming/{0.5,1.0,5.0,10}mbps/   run0.csv .. run5.csv defense OFF (threshold 101)
data/defense/{0.5,1.0,5.0,10}mbps/   run0.csv .. run5.csv defense ON  (threshold 30)
```

Each `runN.csv` is one genuine run's FlowMonitor summary. Raw pcaps are excluded.

## Flow mapping (pcap-verified)

Each run's FlowMonitor XML contains several flows. The paper uses:

| metric | flow | direction | port |
|---|---|---|---|
| telemetry data | Flow 1 | UAV -> AP (`.2 -> .1`) | 100 |
| command data   | Flow 7 | AP -> UAV (`.1 -> .2`) | 1000 |
| ACKs           | Flows 8/9 | — | — (excluded) |

For the 0.0 Mbps baseline the layout collapses to 4 flows: telemetry = Flow 1,
command = Flow 2.

## Genuine-run manifest (N = 6 per rate)

The paper reports the mean of six genuine runs per rate with 95% confidence intervals
(Student's t, 5 dof). Only these runs are used:

- **Defense (threshold 30):** runs 0-5 for each of 0.5, 1.0, 5.0, 10 Mbps.
- **Jamming (threshold 101):**
  - 0.5 Mbps: runs 0-5
  - 1.0 Mbps: runs 1-6 (run 0 is a duplicate)
  - 5.0 Mbps: runs 0, 1, 3, 4, 7, 8 (2, 5, 6 are duplicates/surplus)
  - 10 Mbps: runs 0, 1, 3, 4, 5, 6 (2 is surplus)

**Why the exclusions:** a killed or incomplete ns-3 run leaves a stale
`flynetsim-results.xml`, so the parser re-emits the previous run's CSV, producing
byte-identical duplicates. The excluded files are those duplicates/surplus.

### Collection ritual (to avoid stale duplicates)

Before each run: delete `flynetsim-results.xml` first, then confirm each new run has a
distinct Flow-1 Tx count (fingerprint) before accepting its CSV.

## Status

Populated: 50 genuine-run CSVs (2 baseline + 6 runs x 4 rates x {jamming, defense}),
copied from the `PAper_data` collection and renamed to `runN.csv` in the order listed in
the manifest above (e.g. jamming 1.0 Mbps `run0..run5` = source `J_1mbps` runs 1-6). All
50 were checksum-verified distinct; the manifest's excluded runs were confirmed to be
byte-identical cross-folder duplicates. Raw pcaps (~22 GB) are excluded.
