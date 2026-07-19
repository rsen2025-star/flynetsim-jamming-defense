import xml.etree.ElementTree as ET
import csv
import sys

def parse_comprehensive_xml():
    xml_file = 'flynetsim-results.xml'
    csv_file = input("Enter a name to save this CSV (e.g., final_stress_25mbps): ").strip() + ".csv"

    try:
        tree = ET.parse(xml_file)
        root = tree.getroot()

        # Phase 1: Read flow classifier for IP addresses and protocols
        flow_identities = {}
        for classifier in root.findall('.//Ipv4FlowClassifier/Flow'):
            flow_id = classifier.get('flowId')
            src_ip = classifier.get('sourceAddress')
            dst_ip = classifier.get('destinationAddress')
            protocol = classifier.get('protocol')
            if protocol == '6': protocol = 'TCP'
            elif protocol == '17': protocol = 'UDP'
            flow_identities[flow_id] = {'src': src_ip, 'dst': dst_ip, 'proto': protocol}

        # Phase 2: Read all flow stats — keep EVERY row
        rows = []
        for flow in root.findall('.//FlowStats/Flow'):
            flow_id = flow.get('flowId')
            tx_packets = int(flow.get('txPackets', 0))
            rx_packets = int(flow.get('rxPackets', 0))
            lost_packets = int(flow.get('lostPackets', 0))
            tx_bytes = int(flow.get('txBytes', 0))

            loss_ratio = (lost_packets / tx_packets * 100) if tx_packets > 0 else 0.0

            try:
                delay_ns = float(flow.get('delaySum', '+0.0ns').replace('+', '').replace('ns', ''))
                jitter_ns = float(flow.get('jitterSum', '+0.0ns').replace('+', '').replace('ns', ''))
                mean_delay_ms = (delay_ns / 1e6) / rx_packets if rx_packets > 0 else 0.0
                mean_jitter_ms = (jitter_ns / 1e6) / rx_packets if rx_packets > 0 else 0.0
            except (ValueError, ZeroDivisionError):
                mean_delay_ms, mean_jitter_ms = 0.0, 0.0

            src_ip = flow_identities.get(flow_id, {}).get('src', 'Unknown')
            dst_ip = flow_identities.get(flow_id, {}).get('dst', 'Unknown')
            proto = flow_identities.get(flow_id, {}).get('proto', 'Unknown')

            # Flag the row instead of deleting
            if tx_packets == 0:
                note = "GHOST (no traffic)"
            elif src_ip == '10.10.1.2' or dst_ip == '10.10.1.2':
                note = "DRONE"
            elif src_ip in ('10.10.1.3','10.10.1.4','10.10.1.5','10.10.1.6','10.10.1.7'):
                note = "JAMMER"
            else:
                note = ""

            rows.append([
                flow_id, src_ip, dst_ip, proto,
                tx_packets, rx_packets, lost_packets,
                round(loss_ratio, 2),
                round(mean_delay_ms, 2),
                round(mean_jitter_ms, 2),
                tx_bytes, note
            ])

        # Write CSV
        with open(csv_file, 'w', newline='') as file:
            writer = csv.writer(file)
            writer.writerow([
                'Flow ID', 'Source IP', 'Dest IP', 'Protocol',
                'Tx Packets', 'Rx Packets', 'Lost Packets', 'Loss (%)',
                'Mean Delay (ms)', 'Mean Jitter (ms)', 'Total Tx Bytes', 'Note'
            ])
            for r in rows:
                writer.writerow(r)

        # Print summary to terminal
        total = len(rows)
        active = sum(1 for r in rows if r[4] > 0)
        ghost = total - active
        drone = sum(1 for r in rows if r[11] == 'DRONE')
        jammer = sum(1 for r in rows if r[11] == 'JAMMER')

        print(f"\nExported {total} flows to {csv_file}")
        print(f"  Active flows: {active}  (Drone: {drone}, Jammer: {jammer})")
        print(f"  Ghost flows:  {ghost}  (kept in CSV, flagged as GHOST)")

    except FileNotFoundError:
        print(f"Error: Could not find {xml_file}.")

if __name__ == "__main__":
    parse_comprehensive_xml()
