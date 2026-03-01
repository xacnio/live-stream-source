import sys
import os

def file_to_c_array(filepath, array_name):
    with open(filepath, 'rb') as f:
        data = f.read()
    
    # Null terminate 
    data += b'\0'
    
    out = [f"constexpr const unsigned char {array_name}[] = {{"]
    
    line = "    "
    for i, byte in enumerate(data):
        line += f"0x{byte:02x}, "
        if (i + 1) % 12 == 0:
            out.append(line)
            line = "    "
            
    if line != "    ":
        out.append(line)
        
    out.append("};")
    return "\n".join(out)

if __name__ == "__main__":
    src_dir = sys.argv[1]
    bin_dir = sys.argv[2]
    
    dashboard_path = os.path.join(src_dir, 'docs', 'dashboard.html')
    overlay_path = os.path.join(src_dir, 'docs', 'stats-overlay.html')
    
    dashboard_c = file_to_c_array(dashboard_path, 'DASHBOARD_HTML_DATA')
    overlay_c = file_to_c_array(overlay_path, 'OVERLAY_HTML_DATA')
    
    header = f"""#pragma once

{dashboard_c}

{overlay_c}

// Macros to pretend it's a string. Cast it.
#define DASHBOARD_HTML reinterpret_cast<const char*>(DASHBOARD_HTML_DATA)
#define OVERLAY_HTML reinterpret_cast<const char*>(OVERLAY_HTML_DATA)
"""
    
    out_path = os.path.join(bin_dir, 'html-assets.h')
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(header)

    print("Success")
