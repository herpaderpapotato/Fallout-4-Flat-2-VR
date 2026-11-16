"""Probe an .fxp: locate DXBC blobs and dump the gap bytes between them."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
n = int(sys.argv[2]) if len(sys.argv) > 2 else 12
pos = 0
prev_end = 0
blobs = []
while True:
    i = data.find(b'DXBC', pos)
    if i < 0:
        break
    size = struct.unpack_from('<I', data, i + 24)[0]
    blobs.append((i, size))
    pos = i + max(size, 4)
print('blobs', len(blobs), 'filesize', len(data))
for k, (i, size) in enumerate(blobs[:n]):
    gap = data[prev_end:i]
    print(f'#{k} at {i:#x} size {size} gaplen {len(gap)}')
    print('   ', gap.hex(' '))
    prev_end = i + size
print('tail after last blob:', len(data) - (blobs[-1][0] + blobs[-1][1]))
