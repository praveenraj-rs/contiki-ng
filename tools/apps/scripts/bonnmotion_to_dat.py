#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict

def convert_bonnmotion_to_dat(ns_file, output_dat, num_nodes=5, start_node=0):
    """
    Convert BonnMotion NS-2 format to Cooja .dat format

    start_node is the zero-based Cooja mote index assigned to BonnMotion
    node 0. Cooja motes with lower indices are not included in the trace and
    therefore remain at their configured static positions.
    """

    if start_node < 0:
        raise ValueError("start_node must be zero or greater")
    
    nodes = defaultdict(lambda: {'init_x': 0.0, 'init_y': 0.0, 'movements': []})
    
    print(f"Reading NS-2 file: {ns_file}")
    
    # Parse NS-2 movement file
    with open(ns_file, 'r') as f:
        for line in f:
            # Parse initial position: $node_(0) set X_ 50.0
            match = re.search(r'\$node_\((\d+)\)\s+set\s+([XY])_\s+([\d.]+)', line)
            if match:
                node_id = int(match.group(1))
                coord = match.group(2)
                value = float(match.group(3))
                
                if coord == 'X':
                    nodes[node_id]['init_x'] = value
                elif coord == 'Y':
                    nodes[node_id]['init_y'] = value
            
            # Parse movement: $ns_ at 10.0 "$node_(0) setdest 80.0 90.0 5.0"
            match = re.search(r'\$ns_\s+at\s+([\d.]+)\s+"\$node_\((\d+)\)\s+setdest\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)"', line)
            if match:
                time = float(match.group(1))
                node_id = int(match.group(2))
                x = float(match.group(3))
                y = float(match.group(4))
                speed = float(match.group(5))
                
                nodes[node_id]['movements'].append({
                    'time': time,
                    'x': x,
                    'y': y,
                    'speed': speed
                })
    
    print(f"Parsed {len(nodes)} nodes")
    
    # Create timeline with all position updates
    timeline = []
    
    # Add initial positions at time 0. Cooja's Mobility plugin uses zero-based
    # mote indices and expects the mote index before the timestamp.
    for node_id in range(num_nodes):
        if node_id in nodes:
            timeline.append({
                'time': 0.0,
                'node': node_id + start_node,
                'x': nodes[node_id]['init_x'],
                'y': nodes[node_id]['init_y']
            })
    
    # Add all movements
    for node_id in range(num_nodes):
        if node_id in nodes:
            current_x = nodes[node_id]['init_x']
            current_y = nodes[node_id]['init_y']
            
            for movement in nodes[node_id]['movements']:
                target_x = movement['x']
                target_y = movement['y']
                speed = movement['speed']
                start_time = movement['time']
                
                # Calculate distance and duration
                import math
                distance = math.sqrt((target_x - current_x)**2 + (target_y - current_y)**2)
                
                if speed > 0:
                    duration = distance / speed  # seconds
                    
                    # Create intermediate positions for smooth movement
                    num_steps = max(int(duration * 10), 1)  # 10 updates per second
                    
                    for step in range(num_steps + 1):
                        t = step / num_steps
                        time_s = start_time + duration * t
                        x = current_x + (target_x - current_x) * t
                        y = current_y + (target_y - current_y) * t
                        
                        timeline.append({
                            'time': time_s,
                            'node': node_id + start_node,
                            'x': x,
                            'y': y
                        })
                
                current_x = target_x
                current_y = target_y
    
    # Sort by time
    timeline.sort(key=lambda x: (x['time'], x['node']))
    
    # Write .dat file
    with open(output_dat, 'w') as f:
        f.write("# Cooja Mobility .dat file\n")
        f.write("# Generated from BonnMotion scenario\n")
        f.write("# Format: MoteIndex Time(s) X Y\n")
        f.write("# Nodes: {}\n".format(num_nodes))
        f.write("#\n")
        
        for entry in timeline:
            f.write("{} {:.3f} {:.2f} {:.2f}\n".format(
                entry['node'],
                entry['time'],
                entry['x'],
                entry['y']
            ))
    
    print(f"Successfully created {output_dat}")
    print(f"Total position entries: {len(timeline)}")
    
    # Statistics
    times = set(e['time'] for e in timeline)
    print(f"Time range: 0 - {max(times):.3f} seconds")
    print(f"Unique timestamps: {len(times)}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Convert BonnMotion NS-2 movement files to Cooja .dat format"
    )
    parser.add_argument("input_ns2_file")
    parser.add_argument("output_dat_file")
    parser.add_argument(
        "num_nodes", nargs="?", type=int, default=5,
        help="number of nodes in the BonnMotion file (default: 5)"
    )
    parser.add_argument(
        "start_node", nargs="?", type=int, default=0,
        help="zero-based Cooja mote index for BonnMotion node 0; lower indices "
             "remain static (default: 0)"
    )
    args = parser.parse_args()

    if args.start_node < 0:
        parser.error("start_node must be zero or greater")

    convert_bonnmotion_to_dat(
        args.input_ns2_file, args.output_dat_file, args.num_nodes, args.start_node
    )