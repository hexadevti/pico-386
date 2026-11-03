#!/usr/bin/env python3
# deploy-p4.py -- puts the P4 build on the PicoCalc and boots it, with no cable but the module's
# USB-C (COM9, the console UART) and esp32loader's Wi-Fi:
#   1. "@uf2menu" on COM9, if pico-386 is running, reboots into esp32loader.
#   2. If its web server is not up (Wi-Fi autostart off), F4 (ESC O S) on the console starts it.
#   3. The loader's web API (esp32loader esp32p4/main/webserver.c) uploads the .bin over the old
#      one, keeping that as pico386-p4.prev.bin, and flashes it.
# COM9 is opened with DTR and RTS both high and never changed: the CH343's auto-reset pulls EN low
# when they differ, and opening with both low reset the board too (emu8 tools/deploy-p4-wifi.py).
# Usage: deploy-p4.py [--ip 192.168.0.100] [--port COM9] [--path /pico-386/pico386-p4.bin] [bin]
import argparse, json, os, sys, time, urllib.error, urllib.parse, urllib.request
import serial


def api(ip, name, query, data=None, timeout=10):
    url = f'http://{ip}/api/{name}?' + urllib.parse.urlencode(query)
    req = urllib.request.Request(url, data=data, method='GET' if data is None else 'POST',
                                 headers={'Content-Type': 'application/octet-stream'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


def loader_up(ip):
    try:
        api(ip, 'info', {}, timeout=3)
        return True
    except OSError:
        return False


def wait_up(ip, secs):
    end = time.time() + secs
    while time.time() < end:
        if loader_up(ip):
            return True
        time.sleep(2)
    return False


def open_port(name):
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = name, 115200, 0.2
    p.dtr = p.rts = True
    p.open()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bin', nargs='?', default=os.path.expanduser('~/.pico386-p4-build/pico386.ino.bin'))
    ap.add_argument('--ip', default='192.168.0.100')
    ap.add_argument('--port', default='COM9')
    ap.add_argument('--path', default='/pico-386/pico386-p4.bin')
    a = ap.parse_args()
    data = open(a.bin, 'rb').read()

    if not loader_up(a.ip):
        with open_port(a.port) as p:
            print('deploy: asking pico-386 for the loader', flush=True)
            p.write(b'\n@uf2menu\n')
            if not wait_up(a.ip, 20):
                print('deploy: starting the loader\'s web server (F4)', flush=True)
                p.write(b'\x1bOS')
                if not wait_up(a.ip, 60):
                    sys.exit(f'deploy: no loader at http://{a.ip}/')

    folder, name = a.path.rsplit('/', 1)
    try:
        api(a.ip, 'mkdir', {'path': folder}, b'')
    except urllib.error.HTTPError:
        pass                                 # already there
    names = [e['n'] for e in json.loads(api(a.ip, 'list', {'path': folder}))['entries']]
    stem, ext = os.path.splitext(name)
    prev = f'{stem}.prev{ext}'
    if name in names:
        if prev in names:
            api(a.ip, 'delete', {'path': f'{folder}/{prev}'}, b'')
        api(a.ip, 'rename', {'from': a.path, 'to': f'{folder}/{prev}'}, b'')
    t0 = time.time()
    print('deploy: upload', api(a.ip, 'upload', {'path': a.path, 'mtime': int(time.time())}, data, timeout=300).strip(),
          f'{len(data)} bytes, {time.time() - t0:.1f} s', flush=True)
    print('deploy: flash', api(a.ip, 'flash', {'path': a.path}, b'').strip(), flush=True)


if __name__ == '__main__':
    main()
