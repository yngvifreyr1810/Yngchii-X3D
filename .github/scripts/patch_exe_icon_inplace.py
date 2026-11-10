# -*- coding: utf-8 -*-
import os, sys, pickle, struct, pefile

def patch_exe_icon(exe_path, pkl_path=None, rcc_ico_path=None, *, strict=False, stock_rcc=None):
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

    if strict and (not os.path.isfile(pkl_path or "") or not os.path.isfile(rcc_ico_path or "")):
        raise FileNotFoundError("Missing Pochita icon assets")
    if strict and not stock_rcc:
        raise ValueError("The upstream Qt icon is required for strict patching")
    patched_icons = set()
    patched_group = False
    patched_rcc = False

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
                            patched_icons.add(entry.id)
                            print(f"Patched RT_ICON {entry.id} (size {len(new_icon)} at {hex(offset)})")
                            
                elif res_type.id == 14: # RT_GROUP_ICON
                    sub = res_type.directory.entries[0].directory.entries[0]
                    offset = pe.get_offset_from_rva(sub.data.struct.OffsetToData)
                    assert len(grp_data) == sub.data.struct.Size, f"Group icon size mismatch ({len(grp_data)} vs {sub.data.struct.Size})"
                    f.seek(offset)
                    f.write(grp_data)
                    patched_group = True
                    print(f"Patched RT_GROUP_ICON (size {len(grp_data)} at {hex(offset)})")
        pe.close()
        print("Patched PE icon resources successfully!")

    # 2. Patch Qt RCC Resource (:/img/lichii.ico)
    if rcc_ico_path and os.path.exists(rcc_ico_path):
        with open(rcc_ico_path, 'rb') as f:
            rcc_ico = f.read()
        
        with open(exe_path, 'r+b') as f:
            exe_data = f.read()
            if stock_rcc:
                stock_resource = struct.pack('>I', len(stock_rcc)) + stock_rcc
                if exe_data.count(stock_resource) != 1:
                    raise RuntimeError("Could not uniquely identify the upstream Qt icon")
                pos = exe_data.index(stock_resource) + 4
            else:
                pos = exe_data.find(b'\x00\x00\x01\x00\x07\x00\x10\x10')
            if pos != -1:
                # 4 bytes before pos is big-endian length in RCC
                len_offset = pos - 4
                old_len = struct.unpack('>I', exe_data[len_offset:pos])[0]
                if old_len >= len(rcc_ico):
                    f.seek(len_offset)
                    f.write(struct.pack('>I', len(rcc_ico)))
                    f.write(rcc_ico)
                    f.write(b'\x00' * (old_len - len(rcc_ico)))
                    patched_rcc = True
                    print(f"Patched Qt RCC icon at {hex(pos)} (new size {len(rcc_ico)} <= old size {old_len})!")
                else:
                    print(f"Warning: Old RCC icon size {old_len} smaller than new {len(rcc_ico)}")
            else:
                print("Warning: RCC ICO signature not found in exe!")

    if strict and (not patched_group or patched_icons != set(icons_data) or not patched_rcc):
        raise RuntimeError("Incomplete Pochita icon patch for this upstream executable")

    # 3. Patch Home List View in Yngchii-X3D.exe
    with open(exe_path, 'r+b') as f:
        exe_data = f.read()
        # Patch 1: Loop condition at 0x15c397b (jne 0x1415c462a -> 6 NOPs)
        if exe_data[0x15c397b:0x15c397b+6] == bytes.fromhex('0f85a9000000'):
            f.seek(0x15c397b)
            f.write(b'\x90' * 6)
            print("Patched Home::Refresh loop to display all games without skipping active game!")
        # Patch 2: setToolButtonStyle (mov edx, 3 -> mov edx, 2)
        if exe_data[0x15c4acf:0x15c4acf+5] == bytes.fromhex('ba03000000'):
            f.seek(0x15c4acf)
            f.write(bytes.fromhex('ba02000000'))
            print("Patched setToolButtonStyle to Qt::ToolButtonTextBesideIcon!")
        # Patch 3: icon size (72x72 -> 56x56)
        if exe_data[0x15c4ae6:0x15c4ae6+10] == bytes.fromhex('48b84800000048000000'):
            f.seek(0x15c4ae6)
            f.write(bytes.fromhex('48b83800000038000000'))
            print("Patched tile icon size to 56x56!")
        # Patch 4: setFixedWidth(112) -> 14 NOPs
        if exe_data[0x15c4b00:0x15c4b00+14] == bytes.fromhex('4c89f1ba70000000ff1532423601'):
            f.seek(0x15c4b00)
            f.write(b'\x90' * 14)
            print("Patched tile setFixedWidth(112) out (14 NOPs)!")
        # Patch 5: elide width (96 -> 1000)
        if exe_data[0x15c4b61:0x15c4b61+8] == bytes.fromhex('c744242060000000'):
            f.seek(0x15c4b61)
            f.write(bytes.fromhex('c7442420e8030000'))
            print("Patched tile title elide width to 1000px!")

    print(f"Finished icon patching and checked the V5.6 list-view offsets in {exe_path}")

if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else r'D:\SB\Yngchii-v5.6\Yngchii-X3D.exe'
    patch_exe_icon(target)
