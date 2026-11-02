#!/usr/bin/env python3
# remote.py -- drives pico-386 on the RP2350 PicoCalc over its serial log port (the mainboard's
# USB-C, a CH340): types PC_KEYS scripts and saves screenshots. The firmware side is
# rp2350CheckSerialCmd (system_rp2350.cpp). Needs pyserial, and Pillow for .png.
#
#   remote.py keys "dir,enter,w2000,shot"   type a script; each "shot" in it saves a picture
#   remote.py shot [name.png]               one screenshot
#   remote.py raw <y>                       a panel row's raw RAMRD bytes (format check)
#   remote.py log [seconds]                 just print the log
#
# Options: --port COM6, --out <prefix> (shots go to <prefix>N.png, default sdcard/rshot).
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
    def __init__(self, port, out):
        self.p = serial.Serial(port, 115200, timeout=0.2)
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
    ap.add_argument('cmd', choices=['keys', 'shot', 'raw', 'log'])
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--out', default='sdcard/rshot')
    a = ap.parse_args()
    r = Remote(a.port, a.arg if a.cmd == 'shot' and a.arg else a.out)
    if a.cmd == 'keys':
        r.keys(a.arg or '')
    elif a.cmd == 'shot':
        r.send('@shot')
        if not r.wait('@shot end', 90):
            sys.exit('remote: the shot never finished')
    elif a.cmd == 'raw':
        r.send(f'@shotraw {int(a.arg or 0)}')
        r.wait('@raw ', 5)
    else:
        for ln in r.lines(time.time() + float(a.arg or 10)):
            r.handle(ln)


if __name__ == '__main__':
    main()
