#!/usr/bin/env python3
"""tt: install Deki games on Tactility devices, over USB or Wi-Fi.

USB: on the device, Settings > USB > mass storage (SD card). It restarts as
a USB drive, and tt unpacks the game onto its SD card where Tactility keeps
installed apps (tactility/app/<id>/). Eject the drive and reset the device:
the game is in the Apps menu.

Wi-Fi: on the device, connect to Wi-Fi and switch on Settings > Development.
tt talks to its development server (port 6667, over TLS), the same one
Tactility's own tt uses, and shares its files in ~/.tactility: the
certificate that names this computer and the devices it has met. The first
time, the device asks on its screen whether to accept this computer.

Plain Python 3, nothing to pip install, so it can travel with the game. The
certificate is made with the cryptography package when it is installed, else
with the openssl command (Git for Windows, macOS and Linux have it).

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
import datetime
import getpass
import glob
import hashlib
import ipaddress
import json
import os
import shutil
import socket
import ssl
import string
import struct
import subprocess
import sys
import tarfile
import tempfile

PORT = 6667
PROTOCOL_VERSION = 1
TIMEOUT = 10
# The device answers an install only once the whole file is on its SD card,
# unpacked and registered; a game with its assets takes a while.
INSTALL_TIMEOUT = 300
# How long the person at the device has to accept this computer.
ACCEPT_TIMEOUT = 90

CONFIG_DIRECTORY = os.path.join(os.path.expanduser("~"), ".tactility")
CLIENT_KEY_PATH = os.path.join(CONFIG_DIRECTORY, "client.key")
CLIENT_CERTIFICATE_PATH = os.path.join(CONFIG_DIRECTORY, "client.crt")
KNOWN_HOSTS_PATH = os.path.join(CONFIG_DIRECTORY, "known_hosts")

FRAME_DATA = 0x01
FRAME_DONE = 0x14
FRAME_OK = 0x15
FRAME_FAIL = 0x16
FRAME_APP_INSTALL = 0x20
FRAME_APP_UNINSTALL = 0x21
FRAME_APP_RUN = 0x22
FRAME_PAYLOAD_MAX = 1024


def fail(message):
    print(f"tt: {message}", file=sys.stderr)
    sys.exit(1)


def split_host(host):
    if host.count(":") == 1:
        name, port = host.split(":")
        return name, int(port)
    return host, PORT


# ---- this computer's certificate --------------------------------------------

def client_name():
    return f"{getpass.getuser()}@{socket.gethostname()}"


def make_identity_with_cryptography():
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.x509.oid import NameOID
    except ImportError:
        return False
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, client_name())])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                   .serial_number(x509.random_serial_number())
                   .not_valid_before(now - datetime.timedelta(days=1))
                   .not_valid_after(now + datetime.timedelta(days=365 * 50))
                   .sign(key, hashes.SHA256()))
    key_bytes = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption())
    descriptor = os.open(CLIENT_KEY_PATH, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "wb") as f:
        f.write(key_bytes)
    with open(CLIENT_CERTIFICATE_PATH, "wb") as f:
        f.write(certificate.public_bytes(serialization.Encoding.PEM))
    return True


def find_openssl():
    found = shutil.which("openssl")
    if found:
        return found
    if sys.platform == "win32":
        for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                     os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
            for candidate in (os.path.join(base, "Git", "usr", "bin", "openssl.exe"),
                              os.path.join(base, "Git", "mingw64", "bin", "openssl.exe")):
                if os.path.isfile(candidate):
                    return candidate
    return None


def make_identity_with_openssl():
    openssl = find_openssl()
    if not openssl:
        return False
    # The same certificate the cryptography package makes: P-256, self-signed,
    # named after this computer's user, valid for 50 years.
    with tempfile.TemporaryDirectory() as work:
        key = os.path.join(work, "client.key")
        certificate = os.path.join(work, "client.crt")
        result = subprocess.run([openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                                 "-nodes", "-keyout", key, "-out", certificate, "-days", str(365 * 50),
                                 "-subj", "/CN=" + client_name()],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode != 0 or not os.path.isfile(key) or not os.path.isfile(certificate):
            print(result.stdout.decode("utf-8", "replace"), file=sys.stderr)
            return False
        with open(key, "rb") as f:
            key_bytes = f.read()
        descriptor = os.open(CLIENT_KEY_PATH, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "wb") as f:
            f.write(key_bytes)
        shutil.copyfile(certificate, CLIENT_CERTIFICATE_PATH)
    return True


def ensure_client_identity():
    if os.path.isfile(CLIENT_KEY_PATH) and os.path.isfile(CLIENT_CERTIFICATE_PATH):
        return
    os.makedirs(CONFIG_DIRECTORY, exist_ok=True)
    if not make_identity_with_cryptography() and not make_identity_with_openssl():
        fail("could not make this computer's certificate: install the openssl command, or the cryptography "
             "package (pip install cryptography). Or install over USB.")
    print(f"Made this computer's certificate in {CONFIG_DIRECTORY}")


def client_fingerprint():
    with open(CLIENT_CERTIFICATE_PATH, "rb") as f:
        der = ssl.PEM_cert_to_DER_cert(f.read().decode("ascii"))
    return hashlib.sha256(der).hexdigest()


def grouped(fingerprint):
    return " ".join(fingerprint[i:i + 4] for i in range(0, len(fingerprint), 4))


# ---- devices it has met -----------------------------------------------------

def read_known_hosts():
    hosts = {}
    if os.path.isfile(KNOWN_HOSTS_PATH):
        with open(KNOWN_HOSTS_PATH) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    hosts[parts[0]] = parts[1]
    return hosts


def check_device(host, port, fingerprint):
    """Remembers a device the first time, as Tactility's tt does, and refuses one
    whose certificate changed since."""
    address = f"{host}:{port}"
    known = read_known_hosts().get(address)
    if known == fingerprint:
        return
    if known is not None:
        fail(f"the certificate of {address} changed since tt last met it. If the device was reset, remove its "
             f"line from {KNOWN_HOSTS_PATH}; otherwise someone may be in between.")
    hosts = read_known_hosts()
    hosts[address] = fingerprint
    os.makedirs(CONFIG_DIRECTORY, exist_ok=True)
    with open(KNOWN_HOSTS_PATH, "w") as f:
        for known_address, known_fingerprint in hosts.items():
            f.write(f"{known_address} {known_fingerprint}\n")
    print(f"New device {address}, fingerprint {grouped(fingerprint)}: it should match the Development app.")


# ---- the development server -------------------------------------------------

class Connection:
    def __init__(self, host, mode, timeout=TIMEOUT, certificate=True):
        name, port = split_host(host)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        # A device's certificate is its own: check_device() pins it instead.
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        if certificate:
            context.load_cert_chain(CLIENT_CERTIFICATE_PATH, CLIENT_KEY_PATH)
        try:
            raw = socket.create_connection((name, port), timeout=timeout)
            self.socket = context.wrap_socket(raw)
        except (OSError, ssl.SSLError) as e:
            raise ConnectionError(f"{name}:{port} did not answer ({e}). Is the device on this network, with "
                                  "Settings > Development switched on?")
        self.buffer = b""
        self.fingerprint = hashlib.sha256(self.socket.getpeercert(binary_form=True)).hexdigest()
        self.host, self.port = name, port
        hello = {"protocol": PROTOCOL_VERSION, "mode": mode, "client": client_name()}
        self.send((json.dumps(hello, separators=(",", ":")) + "\n").encode("utf-8"))

    def send(self, data):
        try:
            self.socket.sendall(data)
        except (OSError, ssl.SSLError):
            fail("the device closed the connection")

    def receive(self, timeout):
        self.socket.settimeout(timeout)
        try:
            data = self.socket.recv(4096)
        except socket.timeout:
            return None
        except (OSError, ssl.SSLError):
            return False
        if not data:
            return False
        self.buffer += data
        return True

    def read_line(self, timeout):
        while b"\n" not in self.buffer:
            got = self.receive(timeout)
            if not got:
                return None
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode("utf-8", "replace")

    def send_frame(self, kind, payload=b""):
        for offset in range(0, max(len(payload), 1), FRAME_PAYLOAD_MAX):
            chunk = payload[offset:offset + FRAME_PAYLOAD_MAX]
            self.send(struct.pack(">BH", kind, len(chunk)) + chunk)

    def read_frame(self, timeout):
        while True:
            if len(self.buffer) >= 3:
                kind, length = struct.unpack(">BH", self.buffer[:3])
                if len(self.buffer) >= 3 + length:
                    payload = self.buffer[3:3 + length]
                    self.buffer = self.buffer[3 + length:]
                    return kind, payload
            got = self.receive(timeout)
            if got is None:
                fail(f"the device did not answer within {timeout} seconds")
            if got is False:
                fail("the device closed the connection")

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass


def device_info(host, timeout=4):
    """What a device says about itself. Needs no certificate and asks nothing on
    the device."""
    connection = Connection(host, "info", timeout=timeout, certificate=False)
    try:
        line = connection.read_line(timeout)
    finally:
        connection.close()
    status = json.loads(line or "{}")
    if status.get("status") != "accepted":
        raise ConnectionError(f"{host} refused: {status.get('message', line)}")
    return status.get("info", {})


def open_app_session(host):
    ensure_client_identity()
    try:
        connection = Connection(host, "app")
    except ConnectionError as e:
        fail(str(e))
    check_device(connection.host, connection.port, connection.fingerprint)
    # A computer the device trusts is answered at once; else the device asks.
    line = connection.read_line(1)
    if line is None:
        print(f"Accept this computer on the device (fingerprint {grouped(client_fingerprint())[:9]}...)", flush=True)
        line = connection.read_line(ACCEPT_TIMEOUT)
    if line is None:
        fail("the device did not accept the connection")
    try:
        status = json.loads(line)
    except ValueError:
        fail(f"unexpected answer from the device: {line}")
    if status.get("status") == "rejected":
        fail("the device rejected this computer")
    if status.get("status") != "accepted":
        fail(f"the device refused: {status.get('message', line)}")
    return connection


def expect_ok(connection, subject, timeout):
    kind, payload = connection.read_frame(timeout)
    if kind == FRAME_FAIL:
        fail(f"{subject}: {payload.decode('utf-8', 'replace')}. The device's log says more.")
    if kind != FRAME_OK:
        fail(f"{subject}: unexpected answer from the device")


def app_request(host, kind, app_id):
    connection = open_app_session(host)
    try:
        connection.send_frame(kind, app_id.encode("utf-8"))
        expect_ok(connection, app_id, TIMEOUT)
    finally:
        connection.close()


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
# A device this computer has not talked to yet also needs an ARP lookup first,
# which took more than 1.5 s with the whole /24 being probed at once.
PROBE_TIMEOUT = 3


def probe(ip):
    try:
        with socket.create_connection((ip, PORT), timeout=PROBE_TIMEOUT):
            pass
    except OSError:
        return None
    try:
        return ip, device_info(ip)
    except (OSError, ValueError, ConnectionError, ssl.SSLError):
        return None


def find_devices(quiet=False):
    nets = local_networks()
    if not nets:
        fail("this machine has no IPv4 network to search")
    if not quiet:
        print("Searching " + ", ".join(str(n) for n in nets) + " for Tactility devices...")
    hosts = [str(h) for n in nets for h in n.hosts()]
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=256) as pool:
        for result in pool.map(probe, hosts):
            if result:
                found.append(result)
    return found


def describe(info):
    if not isinstance(info, dict):
        return str(info)
    keys = ("vendor", "name", "id", "version")
    parts = [str(info[k]) for k in keys if info.get(k)]
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
    try:
        print(json.dumps(device_info(host, TIMEOUT), indent=2))
    except (ConnectionError, ValueError, ssl.SSLError) as e:
        fail(str(e))


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
    name = os.path.basename(path)
    connection = open_app_session(host)
    try:
        print(f"Installing {name} ({os.path.getsize(path) // 1024} KB) on {host}...")
        connection.send_frame(FRAME_APP_INSTALL, name.encode("utf-8"))
        with open(path, "rb") as f:
            while data := f.read(64 * 1024):
                connection.send_frame(FRAME_DATA, data)
        connection.send_frame(FRAME_DONE)
        expect_ok(connection, name, INSTALL_TIMEOUT)
    finally:
        connection.close()
    print("Installed. It is in the device's Apps menu.")
    if args.get("run"):
        app_id = app_id_of(path)
        if not app_id:
            fail("installed, but the app id could not be read from the package to start it")
        run(host, app_id)


def run(host, app_id):
    app_request(host, FRAME_APP_RUN, app_id)
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
    app_request(pick_host(args), FRAME_APP_UNINSTALL, app_id)
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
