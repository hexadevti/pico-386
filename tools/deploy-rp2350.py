#!/usr/bin/env python3
# deploy-rp2350.py -- puts build-rp2350/pico386.ino.uf2 on the PicoCalc and boots it, with no cable
# but the mainboard's USB-C (COM6) and PicoMultiLoader's Wi-Fi:
#   1. "@uf2menu" over the serial port, if pico-386 is running, opens the loader.
#   2. The loader's web API (PicoMultiLoader rp2xxx/src/webserver.c) uploads the .uf2 over the
#      old one, keeping that as pico386.prev.uf2, and flashes it.
# Usage: deploy-rp2350.py [--ip 192.168.0.70] [--port COM6] [--path /pico-386/pico386.ino.uf2] [uf2]
import argparse, json, os, sys, time, urllib.parse, urllib.request
import serial


def api(ip, name, query, data=None, timeout=10):
    url = f'http://{ip}/api/{name}?' + urllib.parse.urlencode(query)
    req = urllib.request.Request(url, data=data, method='POST' if data is not None or name != 'list' else 'GET')
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


def loader_up(ip):
    try:
        api(ip, 'list', {'path': '/'}, timeout=3)
        return True
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('uf2', nargs='?', default=os.path.join(os.path.dirname(__file__), '..', 'build-rp2350', 'pico386.ino.uf2'))
    ap.add_argument('--ip', default='192.168.0.70')
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--path', default='/pico-386/pico386.ino.uf2')
    a = ap.parse_args()
    data = open(a.uf2, 'rb').read()

    if not loader_up(a.ip):
        print('deploy: asking pico-386 for the loader', flush=True)
        with serial.Serial(a.port, 115200, timeout=0.2) as p:
            p.write(b'@uf2menu\n')
        end = time.time() + 60
        while not loader_up(a.ip):
            if time.time() > end:
                sys.exit(f'deploy: no loader at http://{a.ip}/')
            time.sleep(2)

    folder, name = a.path.rsplit('/', 1)
    names = [e['n'] for e in json.loads(api(a.ip, 'list', {'path': folder or '/'}))['entries']]
    prev = name.split('.')[0] + '.prev.uf2'
    if name in names:
        if prev in names:
            api(a.ip, 'delete', {'path': f'{folder}/{prev}'}, b'')
        api(a.ip, 'rename', {'from': a.path, 'to': f'{folder}/{prev}'}, b'')
    t0 = time.time()
    print('deploy: upload', api(a.ip, 'upload', {'path': a.path, 'mtime': int(time.time())}, data, timeout=300),
          f'{len(data)} bytes, {time.time() - t0:.1f} s', flush=True)
    print('deploy: flash', api(a.ip, 'flash', {'path': a.path}, b''), flush=True)


if __name__ == '__main__':
    main()
