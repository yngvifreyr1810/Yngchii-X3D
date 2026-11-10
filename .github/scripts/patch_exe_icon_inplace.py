# -*- coding: utf-8 -*-
import os, sys, pickle, struct, pefile

def patch_exe_icon(exe_path, pkl_path=None, rcc_ico_path=None):
    if pkl_path is None:
        cand = [
            r'D:\SB\pochita_icons.pkl',
            os.path.join(os.path.dirname(__file__), '..', 'assets', 'pochita_icons.pkl'),
            os.path.join(os.path.dirname(__file__), 'pochita_icons.pkl')
        ]
        for c in cand:
            if os.path.exists(c):
                pkl_path = c
                break

    if rcc_ico_path is None:
        cand_ico = [
            r'D:\SB\pochita_rcc.ico',
            os.path.join(os.path.dirname(__file__), '..', 'assets', 'pochita_rcc.ico'),
            os.path.join(os.path.dirname(__file__), 'pochita_rcc.ico')
        ]
        for c in cand_ico:
            if os.path.exists(c):
                rcc_ico_path = c
                break

    # 1. Patch Win32 PE Resources (RT_ICON & RT_GROUP_ICON)
    if pkl_path and os.path.exists(pkl_path):
        with open(pkl_path, 'rb') as f:
            data = pickle.load(f)
        icons_data = data['icons']
        grp_data = data['grp']

        pe = pefile.PE(exe_path)
        with open(exe_path, 'r+b') as f:
            for res_type in pe.DIRECTORY_ENTRY_RESOURCE.entries:
                if res_type.id == 3: # RT_ICON
                    for entry in res_type.directory.entries:
                        if entry.id in icons_data:
                            sub = entry.directory.entries[0]
                            offset = pe.get_offset_from_rva(sub.data.struct.OffsetToData)
                            new_icon = icons_data[entry.id]
                            max_size = sub.data.struct.Size
                            assert len(new_icon) <= max_size, f"Icon {entry.id} too large ({len(new_icon)} > {max_size})"
                            
                            f.seek(offset)
                            f.write(new_icon)
                            if len(new_icon) < max_size:
                                f.write(b'\x00' * (max_size - len(new_icon)))
                            
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
        print("Patched PE icon resources successfully!")

    # 2. Patch Qt RCC Resource (:/img/lichii.ico)
    if rcc_ico_path and os.path.exists(rcc_ico_path):
        with open(rcc_ico_path, 'rb') as f:
            rcc_ico = f.read()
        
        with open(exe_path, 'r+b') as f:
            exe_data = f.read()
            # ICO header signature: 00 00 01 00 07 00 10 10 (7 images, first is 16x16)
            ico_sig = b'\x00\x00\x01\x00\x07\x00\x10\x10'
            pos = exe_data.find(ico_sig)
            if pos != -1:
                # 4 bytes before pos is big-endian length in RCC
                len_offset = pos - 4
                old_len = struct.unpack('>I', exe_data[len_offset:pos])[0]
                if old_len >= len(rcc_ico):
                    f.seek(len_offset)
                    f.write(struct.pack('>I', len(rcc_ico)))
                    f.write(rcc_ico)
                    f.write(b'\x00' * (old_len - len(rcc_ico)))
                    print(f"Patched Qt RCC icon at {hex(pos)} (new size {len(rcc_ico)} <= old size {old_len})!")
                else:
                    print(f"Warning: Old RCC icon size {old_len} smaller than new {len(rcc_ico)}")
            else:
                print("Warning: RCC ICO signature not found in exe!")

    print(f"Successfully patched all icons into {exe_path}!")

if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else r'D:\SB\Yngchii-v5.6\Yngchii-X3D.exe'
    patch_exe_icon(target)
