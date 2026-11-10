# -*- coding: utf-8 -*-
import os, sys, pickle, pefile, ctypes, ctypes.wintypes as wt

def patch_exe_icon(exe_path, pkl_path=r'D:\SB\pochita_icons.pkl'):
    with open(pkl_path, 'rb') as f:
        data = pickle.load(f)
    icons_data = data['icons']
    grp_data = data['grp']

    pe = pefile.PE(exe_path)
    
    with open(exe_path, 'r+b') as f:
        # Patch RT_ICON
        for res_type in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            if res_type.id == 3: # RT_ICON
                for entry in res_type.directory.entries:
                    if entry.id in icons_data:
                        sub = entry.directory.entries[0]
                        offset = pe.get_offset_from_rva(sub.data.struct.OffsetToData)
                        new_icon = icons_data[entry.id]
                        max_size = sub.data.struct.Size
                        assert len(new_icon) <= max_size, f"Icon {entry.id} too large ({len(new_icon)} > {max_size})"
                        
                        # Write icon bytes
                        f.seek(offset)
                        f.write(new_icon)
                        if len(new_icon) < max_size:
                            f.write(b'\x00' * (max_size - len(new_icon)))
                        
                        # Update Size in resource data entry
                        # struct IMAGE_RESOURCE_DATA_ENTRY: OffsetToData (4), Size (4), CodePage (4), Reserved (4)
                        size_offset = sub.data.struct.get_file_offset() + 4
                        f.seek(size_offset)
                        f.write(len(new_icon).to_bytes(4, 'little'))
                        print(f"Patched RT_ICON {entry.id} (size {len(new_icon)} at {hex(offset)})")
                        
            elif res_type.id == 14: # RT_GROUP_ICON
                sub = res_type.directory.entries[0].directory.entries[0]
                offset = pe.get_offset_from_rva(sub.data.struct.OffsetToData)
                assert len(grp_data) == sub.data.struct.Size, f"Group icon size mismatch ({len(grp_data)} vs {sub.data.struct.Size})"
                f.seek(offset)
                f.write(grp_data)
                print(f"Patched RT_GROUP_ICON (size {len(grp_data)} at {hex(offset)})")
                
    pe.close()
    print(f"Successfully patched Pochita icon into {exe_path}!")

if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else r'D:\SB\Yngchii-v5.6-Build\Yngchii-X3D-v5.6\Yngchii-X3D.exe'
    patch_exe_icon(target)
