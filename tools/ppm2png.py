import sys, zlib, struct
def read_ppm(path):
    d = open(path,'rb').read()
    # P6 header: magic, width, height, maxval (whitespace separated, # comments)
    idx = 0; fields = []
    while len(fields) < 4:
        while d[idx:idx+1].isspace(): idx += 1
        if d[idx:idx+1] == b'#':
            while d[idx:idx+1] != b'\n': idx += 1
            continue
        start = idx
        while not d[idx:idx+1].isspace(): idx += 1
        fields.append(d[start:idx])
    idx += 1
    w, h = int(fields[1]), int(fields[2])
    return w, h, d[idx:idx+w*h*3]
def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    open(path,'wb').write(png)
w,h,rgb = read_ppm(sys.argv[1]); write_png(sys.argv[2], w, h, rgb)
print(f"{w}x{h} -> {sys.argv[2]}")
