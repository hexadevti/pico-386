#!/usr/bin/env python3
# remote.py -- drives pico-386 on the PicoCalc over its serial log port: types PC_KEYS scripts and
# saves screenshots. RP2350: the mainboard's USB-C (a CH340, COM6), rp2350CheckSerialCmd
# (system_rp2350.cpp). P4 (--p4): the module's USB-C (a CH343, COM9), p4CheckSerialCmd
# (system_p4.cpp). Needs pyserial, and Pillow for .png.
#
#   remote.py keys "dir,enter,w2000,shot"   type a script; each "shot" in it saves a picture
#   remote.py shot [name.png]               one screenshot
#   remote.py raw <y>                       a panel row's raw RAMRD bytes (format check)
#   remote.py log [seconds]                 just print the log
#   remote.py --p4 prof [seconds]           sample core 1 (prof_p4.cpp) and print the hot functions
#                                           and lines (addr2line on --elf)
#
# Options: --port COM6, --p4 (COM9 unless --port), --out <prefix> (shots go to <prefix>N.png, default sdcard/rshot).
# Everything else the device prints is passed through, so the log stays visible.
import argparse, base64, os, sys, time
import serial

CHUNK = 150          # the firmware's line buffer is 160 bytes, "@keys " included


def chunks(script):
    """Splits a script at commas into pieces that fit one @keys line."""
    out, cur = [], ''
    for tok in script.split(','):
        if cur and len(cur) + 1 + len(tok) > CHUNK:
            out.append(cur)
            cur = tok
        else:
            cur = cur + ',' + tok if cur else tok
    if cur:
        out.append(cur)
    return out


class Remote:
    def __init__(self, port, out, p4=False):
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, 115200, 0.2
        if p4:
            # the P4's CH343 resets the board when DTR and RTS differ, and opening with both low
            # reset it too: open with both high and never change them (deploy-p4.py)
            self.p.dtr = self.p.rts = True
        self.p.open()
        self.out = out
        self.buf = b''
        self.shot = None          # rows while a shot is coming in
        self.nshot = 0

    def lines(self, until):
        """Yields the device's lines until the time `until`."""
        while time.time() < until:
            self.buf += self.p.read(4096)
            while b'\n' in self.buf:
                ln, self.buf = self.buf.split(b'\n', 1)
                yield ln.rstrip(b'\r').decode('latin-1')

    def handle(self, ln):
        """Collects shot rows; prints everything else. Returns the line for the caller."""
        if self.shot is not None and '@S ' in ln:
            # a log line can only land in front of a row (the firmware holds the log lock per row)
            parts = ln[ln.index('@S '):].split(' ')
            try:
                y, data, ck = int(parts[1]), base64.b64decode(parts[2]), int(parts[3], 16)
                if sum(data) & 0xFFFF == ck:
                    self.shot[y] = data
            except (IndexError, ValueError):
                pass
            return ln
        if ln.startswith('@shot ') and ln != '@shot end':
            self.shot = {}
            return ln
        if ln == '@shot end' and self.shot is not None:
            self.save(self.shot)
            self.shot = None
            return ln
        print(ln, flush=True)
        return ln

    def save(self, rows, w=320, h=320):
        px = bytearray(w * h * 3)
        bad = []
        for y in range(h):
            data = rows.get(y)
            if data is None:
                bad.append(y)
                continue
            x = 0
            for i in range(0, len(data) - 2, 3):
                c = data[i + 1] << 8 | data[i + 2]
                r, g, b = (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3
                for _ in range(data[i] + 1):
                    if x < w:
                        o = (y * w + x) * 3
                        px[o:o + 3] = bytes((r | r >> 5, g | g >> 6, b | b >> 5))
                    x += 1
        self.nshot += 1
        name = self.out if self.out.lower().endswith(('.png', '.bmp')) else f'{self.out}{self.nshot}.png'
        os.makedirs(os.path.dirname(name) or '.', exist_ok=True)
        from PIL import Image
        Image.frombytes('RGB', (w, h), bytes(px)).save(name)
        print(f'remote: saved {name}' + (f', {len(bad)} rows lost: {bad[:10]}' if bad else ''), flush=True)

    def send(self, line):
        # UART0's receive FIFO is 32 bytes and the keyboard task reads it every 10 ms, so a whole
        # line at 115200 baud overflows it and loses its middle: send it in paced pieces.
        data = (line + '\n').encode('latin-1')
        for i in range(0, len(data), 16):
            self.p.write(data[i:i + 16])
            self.p.flush()
            time.sleep(0.03)

    def wait(self, what, secs):
        for ln in self.lines(time.time() + secs):
            if self.handle(ln).startswith(what):
                return ln
        return None

    def keys(self, script):
        for part in chunks(script):
            self.send('@keys ' + part)
            # a shot takes ~30 s at 115200 baud; waits in the script count as well
            budget = 30 + 40 * part.split(',').count('shot') + sum(
                int(t[1:]) / 1000 for t in part.split(',') if t[:1] == 'w' and t[1:].isdigit())
            res = self.wait('@keys ', budget + len(part))
            if res != '@keys done':
                sys.exit(f'remote: {res or "no answer"} while typing {part!r}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['keys', 'shot', 'raw', 'log', 'prof'])
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--port')
    ap.add_argument('--p4', action='store_true')
    ap.add_argument('--out', default='sdcard/rshot')
    ap.add_argument('--hz', type=int, default=2000)
    ap.add_argument('--elf', default=os.path.expanduser('~/.pico386-p4-build/pico386.ino.elf'))
    a = ap.parse_args()
    port = a.port or ('COM9' if a.p4 else 'COM6')
    r = Remote(port, a.arg if a.cmd == 'shot' and a.arg else a.out, a.p4)
    if a.cmd == 'keys':
        r.keys(a.arg or '')
    elif a.cmd == 'shot':
        r.send('@shot')
        if not r.wait('@shot end', 90):
            sys.exit('remote: the shot never finished')
    elif a.cmd == 'raw':
        r.send(f'@shotraw {int(a.arg or 0)}')
        r.wait('@raw ', 5)
    elif a.cmd == 'prof':
        prof(r, a)
    else:
        for ln in r.lines(time.time() + float(a.arg or 10)):
            r.handle(ln)


def prof(r, a):
    secs = float(a.arg or 5)
    n = min(16384, int(secs * a.hz))
    r.send(f'@prof {a.hz} {n}')
    if not r.wait('@prof started', 5):
        sys.exit('remote: the profiler did not start')
    time.sleep(secs + 0.5)
    r.send('@profdump')
    pcs = []
    for ln in r.lines(time.time() + 60):
        if ln.startswith('@P '):
            pcs += [int(x, 16) for x in ln.split()[1:]]
        elif ln.startswith('@prof end'):
            break
        else:
            r.handle(ln)
    if not pcs:
        sys.exit('remote: no samples')
    import collections, glob, subprocess
    tool = glob.glob(os.path.expanduser('~/.emu6502-p4/data/packages/esp32/tools/esp-rv32/*/bin/riscv32-esp-elf-addr2line*'))[0]
    uniq = sorted(set(pcs))
    out = subprocess.run([tool, '-f', '-C', '-e', a.elf], input='\n'.join(f'{x:x}' for x in uniq),
                         capture_output=True, text=True).stdout.splitlines()
    where = {pc: (out[2 * i], os.path.basename(out[2 * i + 1].split(' ')[0])) for i, pc in enumerate(uniq)}
    funcs, lines = collections.Counter(), collections.Counter()
    for pc in pcs:
        f, l = where[pc]
        funcs[f] += 1
        lines[f'{l} ({f})'] += 1
    total = len(pcs)
    print(f'{total} samples')
    print('-- functions')
    for f, c in funcs.most_common(30):
        print(f'{100 * c / total:5.1f}%  {f}')
    print('-- lines')
    for l, c in lines.most_common(40):
        print(f'{100 * c / total:5.1f}%  {l}')


if __name__ == '__main__':
    main()
