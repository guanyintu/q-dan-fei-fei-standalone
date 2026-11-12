import hashlib
import json
import pathlib
import re
import struct
import sys

path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
pe = struct.unpack_from('<I', data, 0x3C)[0]
sections, optlen = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
image_base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
section_rows = []
for i in range(sections):
    start = pe + 24 + optlen + i * 40
    name = data[start:start + 8].rstrip(b'\0').decode()
    vsize, rva, size, raw = struct.unpack_from('<IIII', data, start + 8)
    section_rows.append((name, rva, size, raw))
text = next(s for s in section_rows if s[0] == '.text')

def raw_offset(rva):
    for _, base, size, raw in section_rows:
        if base <= rva < base + size:
            return raw + rva - base
    raise ValueError(hex(rva))

patterns = {
    'Objects': '48 8D 35 ?? ?? ?? ?? 81 FA',
    'Framework': '48 8B 1D ?? ?? ?? ?? 8B 7C 24',
    'FrameworkVtable': '48 8D 05 ?? ?? ?? ?? ?? ?? ?? 48 8B F1 49 89 6B',
    'Conditions': '48 8D 0D ?? ?? ?? ?? 66 2B D8',
    'GameMain': '48 8D 0D ?? ?? ?? ?? 0F 28 F2 48 89 44 24 ??',
    'GPose': 'E8 ?? ?? ?? ?? 41 83 7E ?? 00 4C 8D 3D',
    'AtkStage': '48 8B 05 ?? ?? ?? ?? 4C 8B 40 18 45 8B 40 18',
    'GameObjectVtable': '48 8D 05 ?? ?? ?? ?? ?? ?? ?? 33 C0 48 89 41 ?? 48 89 41 ?? 89 81 ?? ?? ?? ?? 48 89 81',
    'CharacterVtable': '48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 8F ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 87 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? 33 ED 48 8D 05 ?? ?? ?? ??',
    'BattleCharaVtable': '48 8D 05 ?? ?? ?? ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8D 05',
    'CompanionVtable': '48 8D 05 ?? ?? ?? ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 83 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 8B ?? ?? ?? ?? 48 C7 83',
}
exception_rva, exception_size = struct.unpack_from('<II', data, pe + 24 + 112 + 3 * 8)
exceptions = [struct.unpack_from('<III', data, raw_offset(exception_rva) + n) for n in range(0, exception_size, 12)]
def fn_info(rva):
    size = next((end - begin for begin, end, _ in exceptions if begin == rva), 16)
    value = 14695981039346656037
    for byte in data[raw_offset(rva):raw_offset(rva) + size]:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return {'Length': size, 'Hash': hex(value)}
result = {'Version': (path.parent / 'ffxivgame.ver').read_text().strip(), 'Sha256': hashlib.sha256(data).hexdigest(), 'Patterns': {}}
for key, pattern in patterns.items():
    regex = b''.join(b'.' if x == '??' else re.escape(bytes([int(x, 16)])) for x in pattern.split())
    matches = list(re.finditer(regex, data[text[3]:text[3]+text[2]], re.DOTALL))
    assert len(matches) == 1, (key, len(matches))
    offset = text[3] + matches[0].start()
    match_rva = text[1] + matches[0].start()
    displacement_offset = 1 if key == 'GPose' else 3
    target_rva = match_rva + displacement_offset + 4 + struct.unpack_from('<i', data, offset + displacement_offset)[0]
    row = {'MatchRva': hex(match_rva), 'TargetRva': hex(target_rva), 'Matches': 1}
    if key == 'FrameworkVtable':
        tick_va = struct.unpack_from('<Q', data, raw_offset(target_rva) + 4 * 8)[0]
        row['TickRva'] = hex(tick_va - image_base)
        row['TickPrefix'] = data[raw_offset(tick_va-image_base):raw_offset(tick_va-image_base)+16].hex()
        row['Function'] = fn_info(tick_va-image_base)
    elif key.endswith('Vtable'):
        update_rva = struct.unpack_from('<Q', data, raw_offset(target_rva) + 36 * 8)[0] - image_base
        row['UpdateRva'] = hex(update_rva)
        row['Function'] = fn_info(update_rva)
    elif key == 'GPose':
        row['Function'] = fn_info(target_rva)
    result['Patterns'][key] = row
print(json.dumps(result, indent=2))
if len(sys.argv) > 2:
    profiles = [result['Patterns'][key]['Function'] for key in ('GameObjectVtable', 'CharacterVtable', 'BattleCharaVtable', 'CompanionVtable')]
    tick, gpose = (result['Patterns'][key]['Function'] for key in ('FrameworkVtable', 'GPose'))
    header = '#pragma once\n#include <cstdint>\nnamespace qb {\nstruct FunctionReference { unsigned length; uint64_t hash; };\n'
    for name, value in (('TickReference', tick), ('GPoseReference', gpose)):
        header += f'inline constexpr FunctionReference {name}{{{value["Length"]}, {value["Hash"]}ULL}};\n'
    header += 'inline constexpr FunctionReference UpdateReferences[]{' + ','.join(f'{{{p["Length"]},{p["Hash"]}ULL}}' for p in profiles) + '};\n}\n'
    pathlib.Path(sys.argv[2]).write_text(header, encoding='utf-8')
