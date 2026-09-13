#!/usr/bin/env python3
import sys
import re
from collections import defaultdict

def convert_bonnmotion_to_dat(ns_file, output_dat, num_nodes=5):
    """
    Convert BonnMotion NS-2 format to Cooja .dat format
    """
    
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
                'node': node_id,
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
                            'node': node_id,
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
    if len(sys.argv) < 3:
        print("Usage: python3 bonnmotion_to_dat.py <input_ns2_file> <output_dat_file> [num_nodes]")
        print("Example: python3 bonnmotion_to_dat.py scenario.ns_movements positions.dat 5")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2]
    num_nodes = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    
    convert_bonnmotion_to_dat(input_file, output_file, num_nodes)