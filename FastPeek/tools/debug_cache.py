import os, glob

def fnv16(path):
    h = 14695981039346656037
    for ch in path:
        c = ch.lower() if 'A' <= ch <= 'Z' else ch
        h ^= ord(c)
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

stress = r'C:\Users\dasde\Desktop\FastPeek\stress'
thumbs = r'C:\Users\dasde\AppData\Local\FastPeek\thumbs'
cached = set(os.path.basename(f) for f in glob.glob(thumbs + r'\*.tc'))

imgs = sorted(os.listdir(stress))
print(f'total stress imgs: {len(imgs)}, cached: {len(cached)}')
hit = 0
for im in imgs[:50]:
    full = os.path.join(stress, im)
    h = fnv16(full)
    exists = f'{h:016x}.tc' in cached
    if exists: hit += 1
    # show first few
    if im in imgs[:8]:
        print(f'{im}: hash={h:016x} cached={exists}')
print(f'first 50: {hit}/50 cached')

# now compare the cached file's stored mtime vs the actual file mtime
import struct
def fnv16_upper_aware(path):
    # exact C code: lowercase A-Z only
    h = 14695981039346656037
    for ch in path:
        c = ch
        if 'A' <= ch <= 'Z':
            c = ch.lower()
        h ^= ord(c)
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

# find a cached file and reverse-map to a source file
for f in glob.glob(thumbs + r'\*.tc')[:3]:
    data = open(f, 'rb').read()
    magic, ver, w, h, mtime, size = struct.unpack('<IIIIQQ', data[:32])
    print(f'\n{f.split(chr(92))[-1]}: mtime_u64={mtime} size={size}')
    # find which source file has this size
    for im in imgs:
        full = os.path.join(stress, im)
        if os.path.getsize(full) == size:
            ft = os.path.getmtime(full)
            # convert to FILETIME u64
            import datetime
            epoch = datetime.datetime(1601,1,1)
            dt = datetime.datetime.fromtimestamp(ft)
            delta = dt - epoch
            ft_u64 = int(delta.total_seconds() * 10_000_000)
            print(f'  matches source {im}: file_mtime_u64={ft_u64} stored={mtime} equal={ft_u64==mtime}')
            break
