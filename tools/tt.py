#!/usr/bin/env python3
"""tt: install Deki games on Tactility devices, over USB or Wi-Fi.

USB: on the device, Settings > USB > mass storage (SD card). It restarts as
a USB drive, and tt unpacks the game onto its SD card where Tactility keeps
installed apps (tactility/app/<id>/). Eject the drive and reset the device:
the game is in the Apps menu.

Wi-Fi: on the device, connect to Wi-Fi and switch on Settings > Development.
tt talks to its development service (port 6666), the same one Tactility's
own tactility.py uses.

Plain Python 3, nothing to pip install, so it can travel with the game.

    python tt.py find                       devices on USB and on this network
    python tt.py install [GAME.app] [--usb [--drive E:]] [--host IP] [--run]
    python tt.py uninstall APP_ID [--usb [--drive E:]] [--host IP]
    python tt.py run     APP_ID [--host IP]          (Wi-Fi only)
    python tt.py info    [--host IP]                 (Wi-Fi only)

install and uninstall use a device on USB when there is one, else the first
device on the network. --usb or --host chooses. Without GAME.app, install
takes the one .app file next to this script.
"""
import concurrent.futures
import glob
import ipaddress
import json
import os
import shutil
import socket
import string
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request
import uuid

PORT = 6666
TIMEOUT = 10
# The device answers an install only once the whole file is on its SD card,
# unpacked and registered; a game with its assets takes a while.
INSTALL_TIMEOUT = 300


def fail(message):
    print(f"tt: {message}", file=sys.stderr)
    sys.exit(1)


def url(host, path, query=None):
    if ":" not in host:
        host = f"{host}:{PORT}"
    q = "?" + urllib.parse.urlencode(query) if query else ""
    return f"http://{host}{path}{q}"


def request(method, host, path, query=None, body=None, headers=None, timeout=TIMEOUT):
    req = urllib.request.Request(url(host, path, query), data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except (urllib.error.URLError, OSError) as e:
        fail(f"{host} did not answer ({getattr(e, 'reason', e)}). Is the device on this network, "
             "with Settings > Development switched on?")


# ---- finding devices --------------------------------------------------------

def local_networks():
    """The /24 around each IPv4 address this machine has."""
    addresses = set()
    try:
        # A UDP "connect" sends nothing; it only picks the outgoing interface.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            addresses.add(s.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addresses.add(info[4][0])
    except OSError:
        pass
    nets = set()
    for a in addresses:
        if a.startswith("127.") or a.startswith("169.254."):
            continue
        nets.add(ipaddress.ip_network(f"{a}/24", strict=False))
    return sorted(nets)


# A device on Wi-Fi sleeps its radio between beacons, so it can take a few
# hundred ms to answer (a T-Deck measured 130 ms and dropped the first ping).
PROBE_TIMEOUT = 1.5


def probe(ip):
    try:
        with socket.create_connection((ip, PORT), timeout=PROBE_TIMEOUT):
            pass
    except OSError:
        return None
    try:
        with urllib.request.urlopen(url(ip, "/info"), timeout=4) as r:
            return ip, json.loads(r.read().decode("utf-8", "replace"))
    except (OSError, ValueError, urllib.error.URLError):
        return None


def find_devices(quiet=False):
    nets = local_networks()
    if not nets:
        fail("this machine has no IPv4 network to search")
    if not quiet:
        print("Searching " + ", ".join(str(n) for n in nets) + " for Tactility devices...")
    hosts = [str(h) for n in nets for h in n.hosts()]
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=128) as pool:
        for result in pool.map(probe, hosts):
            if result:
                found.append(result)
    return found


def describe(info):
    if not isinstance(info, dict):
        return str(info)
    keys = ("name", "model", "device", "vendor", "version", "firmware")
    parts = [str(info[k]) for k in keys if k in info]
    return " ".join(parts) if parts else json.dumps(info)


def pick_host(args):
    if args.get("host"):
        return args["host"]
    found = find_devices()
    if not found:
        fail("no Tactility device found. On the device: connect to Wi-Fi, then switch on "
             "Settings > Development. Or pass --host with the address it shows.")
    ip, info = found[0]
    print(f"Using {ip} ({describe(info)})")
    return ip


# ---- USB: the device's SD card as a drive -----------------------------------

def candidate_mounts():
    if sys.platform == "win32":
        # From C: on: A: and B: are floppy letters that can stall.
        return [f"{letter}:\\" for letter in string.ascii_uppercase[2:]]
    roots = ["/Volumes"] + glob.glob("/media/*") + glob.glob("/run/media/*") + ["/media", "/mnt"]
    mounts = []
    for root in roots:
        if os.path.isdir(root):
            mounts += [os.path.join(root, d) for d in sorted(os.listdir(root))]
    return mounts


def usb_drives():
    """Mounted drives that are a Tactility device's SD card: they have its
    tactility/ data folder at the root."""
    found = []
    for mount in candidate_mounts():
        try:
            if os.path.isdir(os.path.join(mount, "tactility")):
                found.append(mount)
        except OSError:
            pass
    return found


def pick_drive(args):
    if args.get("drive"):
        drive = args["drive"]
        if sys.platform == "win32" and len(drive) == 2 and drive.endswith(":"):
            drive += "\\"
        if not os.path.isdir(os.path.join(drive, "tactility")):
            fail(f"{drive} is not a Tactility SD card (no tactility/ folder at its root)")
        return drive
    drives = usb_drives()
    if not drives:
        fail("no Tactility device on USB. On the device: Settings > USB > mass storage (SD card); "
             "it restarts as a USB drive.")
    return drives[0]


def read_manifest(tar):
    for member in tar.getmembers():
        if member.name.lstrip("./") == "manifest.properties":
            text = tar.extractfile(member).read().decode("utf-8", "replace")
            return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    return None


def install_usb(path, drive):
    try:
        tar = tarfile.open(path)
    except (tarfile.TarError, OSError) as e:
        fail(f"{path} is not a Tactility app package: {e}")
    with tar:
        manifest = read_manifest(tar)
        package_id = manifest and manifest.get("id")
        if not package_id:
            fail(f"{path} has no manifest.properties with an id")
        apps = os.path.join(drive, "tactility", "app")
        final = os.path.join(apps, package_id)
        staging = final + ".tt-staging"
        shutil.rmtree(staging, ignore_errors=True)
        os.makedirs(staging)
        print(f"Installing {os.path.basename(path)} ({os.path.getsize(path) // 1024} KB) on the SD card at {drive}...")
        for member in tar.getmembers():
            name = member.name.lstrip("./")
            # What Tactility's own installer refuses: anything that climbs out.
            if not name or ".." in name.split("/") or os.path.isabs(name):
                if name:
                    fail(f"refusing unsafe path in the package: {member.name}")
                continue
            target = os.path.join(staging, *name.split("/"))
            if member.isdir():
                os.makedirs(target, exist_ok=True)
            elif member.isfile():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with tar.extractfile(member) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
        # As the device does: the new copy replaces the old one whole.
        shutil.rmtree(final, ignore_errors=True)
        os.replace(staging, final)
    print(f"Installed {package_id}. Eject the drive and reset the device; the game is in its Apps menu.")


def uninstall_usb(app_id, drive):
    target = os.path.join(drive, "tactility", "app", app_id)
    if not os.path.isdir(target):
        print(f"{app_id} is not installed on the SD card at {drive}.")
        return
    shutil.rmtree(target)
    print(f"Uninstalled {app_id}. Eject the drive and reset the device.")


def use_usb(args):
    """USB unless told otherwise: --usb, a --drive or a device on USB, and no --host."""
    if args.get("host"):
        return False
    return bool(args.get("usb") or args.get("drive") or usb_drives())


# ---- actions ----------------------------------------------------------------

def action_find(args):
    for drive in usb_drives():
        print(f"usb {drive}  Tactility SD card")
    found = [] if args.get("usb") else find_devices()
    for ip, info in found:
        print(f"{ip}  {describe(info)}")
    if not found and not usb_drives():
        print("No Tactility devices found. For USB: Settings > USB > mass storage (SD card). "
              "For Wi-Fi: connect to Wi-Fi, then switch on Settings > Development.")


def action_info(args):
    host = pick_host(args)
    status, body = request("GET", host, "/info")
    if status != 200:
        fail(f"{host} answered {status}")
    try:
        print(json.dumps(json.loads(body), indent=2))
    except ValueError:
        print(body.decode("utf-8", "replace"))


def app_id_of(path):
    """The id inside a .app (a tar with a manifest.properties)."""
    try:
        with tarfile.open(path) as tar:
            props = read_manifest(tar) or {}
            return props.get("app.0.id") or props.get("id")
    except (tarfile.TarError, OSError):
        return None


def default_app():
    here = os.path.dirname(os.path.abspath(__file__))
    apps = sorted(glob.glob(os.path.join(here, "*.app"))) or sorted(glob.glob("*.app"))
    if len(apps) == 1:
        return apps[0]
    if not apps:
        fail("no .app file given, and none next to tt.py")
    fail("several .app files here; name the one to install: " + ", ".join(os.path.basename(a) for a in apps))


def action_install(args):
    path = args["positional"][0] if args["positional"] else default_app()
    if not os.path.isfile(path):
        fail(f"{path} not found")
    if use_usb(args):
        if args.get("run"):
            print("(--run works over Wi-Fi only; over USB the device has to restart first)")
        install_usb(path, pick_drive(args))
        return
    host = pick_host(args)
    with open(path, "rb") as f:
        data = f.read()
    # Exactly one part, named "app": the device's parser accepts nothing else.
    boundary = uuid.uuid4().hex
    name = os.path.basename(path)
    body = (f"--{boundary}\r\n"
            f"Content-Disposition: form-data; name=\"app\"; filename=\"{name}\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{boundary}--\r\n".encode()
    print(f"Installing {name} ({len(data) // 1024} KB) on {host}...")
    status, reply = request("PUT", host, "/app/install", body=body,
                            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
                            timeout=INSTALL_TIMEOUT)
    if status != 200:
        fail(f"install refused ({status}): {reply.decode('utf-8', 'replace').strip()}. The device's log says more.")
    print("Installed. It is in the device's Apps menu.")
    if args.get("run"):
        app_id = app_id_of(path)
        if not app_id:
            fail("installed, but the app id could not be read from the package to start it")
        run(host, app_id)


def run(host, app_id):
    status, reply = request("POST", host, "/app/run", {"id": app_id})
    if status != 200:
        fail(f"run refused ({status}): {reply.decode('utf-8', 'replace').strip()}")
    print(f"Started {app_id}.")


def action_run(args):
    if not args["positional"]:
        fail("run needs the app id, e.g. com.example.mygame")
    run(pick_host(args), args["positional"][0])


def action_uninstall(args):
    if not args["positional"]:
        fail("uninstall needs the app id, e.g. com.example.mygame")
    app_id = args["positional"][0]
    if use_usb(args):
        uninstall_usb(app_id, pick_drive(args))
        return
    host = pick_host(args)
    status, reply = request("PUT", host, "/app/uninstall", {"id": app_id})
    if status != 200:
        fail(f"uninstall refused ({status}): {reply.decode('utf-8', 'replace').strip()}")
    print(f"Uninstalled {app_id}.")


ACTIONS = {
    "find": action_find,
    "info": action_info,
    "install": action_install,
    "run": action_run,
    "uninstall": action_uninstall,
}


def parse(argv):
    args = {"positional": []}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--host", "-h") and i + 1 < len(argv):
            args["host"] = argv[i + 1]
            i += 1
        elif a == "--run":
            args["run"] = True
        elif a == "--usb":
            args["usb"] = True
        elif a == "--drive" and i + 1 < len(argv):
            args["drive"] = argv[i + 1]
            i += 1
        elif a.startswith("-"):
            fail(f"unknown option {a}")
        else:
            args["positional"].append(a)
        i += 1
    return args


def main():
    argv = sys.argv[1:]
    if not argv or argv[0] in ("help", "--help", "-?") or argv[0] not in ACTIONS:
        print(__doc__.strip())
        sys.exit(0 if argv and argv[0] in ("help", "--help", "-?") else 1)
    ACTIONS[argv[0]](parse(argv[1:]))


if __name__ == "__main__":
    main()
