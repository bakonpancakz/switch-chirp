# Reads test/out/grey.h264 (crypto_test writes it) as an H.264 decoder's parser would: the PPS's fields,
# then the IDR slice's header and every macroblock, and checks the slice ends exactly at its stop bit.
import sys

data = open(sys.argv[1] if len(sys.argv) > 1 else 'out/grey.h264', 'rb').read()
nals = [n for n in data.split(b'\x00\x00\x00\x01') if n]
assert [n[0] & 0x1f for n in nals] == [7, 8, 5], [n[0] for n in nals]

def rbsp(n):
    out, zeros = bytearray(), 0
    for b in n[1:]:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return out

class R:
    def __init__(self, b):
        self.bits = ''.join(f'{x:08b}' for x in b)
        self.i = 0
    def u(self, n):
        v = int(self.bits[self.i:self.i + n] or '0', 2)
        self.i += n
        return v
    def ue(self):
        z = 0
        while self.bits[self.i] == '0':
            z += 1
            self.i += 1
        self.i += 1
        return (1 << z) - 1 + (self.u(z) if z else 0)
    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)
    def trailing(self):
        rest = self.bits[self.i:]
        return rest.startswith('1') and set(rest[1:]) <= {'0'} and len(rest) <= 8

p = R(rbsp(nals[1]))
pps = dict(id=p.ue(), sps=p.ue(), cabac=p.u(1), bottom=p.u(1), groups=p.ue(), l0=p.ue(), l1=p.ue(), wp=p.u(1), wb=p.u(2),
           qp=p.se(), qs=p.se(), cqp=p.se(), dbk=p.u(1), cip=p.u(1), rpc=p.u(1))
assert p.trailing(), p.bits[p.i:]
assert pps['id'] == 1 and pps['cabac'] == 0 and pps['dbk'] == 1, pps
print('PPS', pps)

s = R(rbsp(nals[2]))
hdr = dict(first=s.ue(), type=s.ue(), pps=s.ue(), frame_num=s.u(8), idr_id=s.ue(), no_out=s.u(1), lt=s.u(1), qpd=s.se(), dbk=s.ue())
assert hdr['first'] == 0 and hdr['type'] == 7 and hdr['pps'] == 1 and hdr['dbk'] == 1, hdr
print('slice', hdr)
for mb in range(80 * 45):
    t, chroma, qpd, token = s.ue(), s.ue(), s.se(), s.u(1)
    assert (t, chroma, qpd, token) == (3, 0, 0, 1), (mb, t, chroma, qpd, token)
assert s.trailing(), s.bits[s.i:s.i + 32]
print('3600 macroblocks, ends at its stop bit: ok')
