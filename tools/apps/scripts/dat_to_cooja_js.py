#!/usr/bin/env python3
import sys

def dat_to_cooja_js(dat_file, js_file):
    """Convert .dat file to Cooja JavaScript"""
    
    movements = {}
    
    with open(dat_file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split()
            if len(parts) >= 4:
                node_id = int(parts[0])
                time_s = float(parts[1])
                x = float(parts[2])
                y = float(parts[3])
                
                time_ms = round(time_s * 1000)
                if time_ms not in movements:
                    movements[time_ms] = []
                
                movements[time_ms].append({
                    'node': node_id,
                    'x': x,
                    'y': y,
                    'z': 0.0
                })
    
    with open(js_file, 'w') as f:
        f.write("// Cooja Mobility Script from .dat file\n")
        f.write("TIMEOUT(100, log.log(\"Loading mobility from .dat file\\n\"));\n\n")
        f.write("var motes = sim.getMotes();\n\n")
        
        for time_ms in sorted(movements.keys()):
            f.write(f"TIMEOUT({time_ms}, function() {{\n")
            for move in movements[time_ms]:
                f.write(f"    motes[{move['node']}].getInterfaces().getPosition()")
                f.write(f".setCoordinates({move['x']}, {move['y']}, {move['z']});\n")
            f.write("});\n\n")
        
        f.write("log.log(\"Mobility script loaded\\n\");\n")
    
    print(f"Converted {len(movements)} time points to JavaScript")
    print(f"Output: {js_file}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 dat_to_cooja_js.py <input.dat> <output.js>")
        sys.exit(1)
    
    dat_to_cooja_js(sys.argv[1], sys.argv[2])