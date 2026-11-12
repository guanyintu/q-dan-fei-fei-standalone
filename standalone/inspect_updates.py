"""Read-only diagnosis of loaded actor update entries; never writes to the game."""
import collections
import ctypes as c
import json
import pathlib
import struct
import sys

pid = int(sys.argv[1])
exe = pathlib.Path(sys.argv[2])
profile = json.loads(pathlib.Path(sys.argv[3]).read_text(encoding='utf-8-sig'))
k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [c.c_uint32, c.c_int, c.c_uint32]
k.OpenProcess.restype = c.c_void_p
k.ReadProcessMemory.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k.ReadProcessMemory.restype = c.c_int
k.K32EnumProcessModules.argtypes = [c.c_void_p, c.POINTER(c.c_void_p), c.c_uint32, c.POINTER(c.c_uint32)]
k.CloseHandle.argtypes = [c.c_void_p]
handle = k.OpenProcess(0x410, 0, pid)
if not handle:
    raise c.WinError(c.get_last_error())
def read(address, size):
    buffer = c.create_string_buffer(size)
    received = c.c_size_t()
    if not k.ReadProcessMemory(handle, address, buffer, size, c.byref(received)) or received.value != size:
        raise OSError('Read failed')
    return buffer.raw
def u64(address): return struct.unpack('<Q', read(address, 8))[0]
data = exe.read_bytes()
pe = struct.unpack_from('<I', data, 0x3C)[0]
section_count = struct.unpack_from('<H', data, pe + 6)[0]
optlen = struct.unpack_from('<H', data, pe + 20)[0]
sections = [struct.unpack_from('<IIII', data, pe + 24 + optlen + i * 40 + 8) for i in range(section_count)]
def raw(rva):
    for _, base, length, offset in sections:
        if base <= rva < base + length: return offset + rva - base
    raise ValueError('RVA')
exception_rva, exception_size = struct.unpack_from('<II', data, pe + 24 + 112 + 3 * 8)
functions = {begin: end - begin for begin, end, _ in [struct.unpack_from('<III', data, raw(exception_rva) + n) for n in range(0, exception_size, 12)]}
try:
    modules = (c.c_void_p * 1024)(); needed = c.c_uint32()
    if not k.K32EnumProcessModules(handle, modules, c.sizeof(modules), c.byref(needed)): raise c.WinError(c.get_last_error())
    base = modules[0]
    manager = base + int(profile['Patterns']['Objects']['TargetRva'], 16)
    pointers = struct.unpack('<819Q', read(manager + 0x20, 819 * 8))
    entries = collections.Counter()
    for index, actor in enumerate(pointers):
        if not actor: continue
        try:
            header = read(actor, 0x110)
            kind = header[0x90]
            if kind not in (1, 2, 3, 9, 10) or struct.unpack_from('<H', header, 0x8C)[0] != index: continue
            draw = struct.unpack_from('<Q', header, 0x100)[0]
            if not draw or read(draw + 0x89, 1)[0] & 15 != 3: continue
            vtable = struct.unpack_from('<Q', header)[0]
            update = u64(vtable + 36 * 8) - base
            entries[(kind, update)] += 1
        except OSError: pass
    rows = []
    for (kind, rva), count in entries.items():
        length = functions.get(rva, 16); value = 14695981039346656037
        for byte in data[raw(rva):raw(rva) + length]: value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        rows.append({'Kind': kind, 'Rva': hex(rva), 'Length': length, 'Hash': hex(value), 'Count': count})
    print(json.dumps(rows, indent=2))
finally:
    k.CloseHandle(handle)
