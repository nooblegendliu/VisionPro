import pathlib
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import time

from image_ftp_fixture import FtpServer


with tempfile.TemporaryDirectory(prefix="vp-ftp-") as temp:
    root = pathlib.Path(temp)
    local, remote = root / "previous", root / "server"
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    command = [sys.argv[1], "ftp", str(local), str(port)]
    password = secrets.token_urlsafe(24)
    environment = dict(os.environ, VISIONPRO_TEST_FTP_SECRET=password)
    subprocess.run([*command, "offline"], check=True, timeout=20, env=environment)
    subprocess.run([*command, "both-fail"], check=True, timeout=20, env=environment)
    cached, = local.glob("offline*.png")
    assert cached.read_bytes() == bytes([1]) * 512
    with FtpServer(remote, port, password=password, reject_publish=True):
        subprocess.run([*command, "reject"], check=True, timeout=20, env=environment)
    assert len(list(local.glob("*.png"))) == 2
    assert not list(remote.rglob("*.png"))
    assert not list(remote.rglob(".visionpro-upload-*"))
    current = root / "current"
    current.write_bytes(b"file instead of local directory")
    sentinel = remote / "质量/图像/existing.png"
    sentinel.write_bytes(b"pre-existing remote image")
    with FtpServer(remote, port, password=password):
        subprocess.run([sys.argv[1], "ftp", str(current), str(port), "online"],
                       check=True, timeout=20, env=environment)
        subprocess.run([*command, "no-local"], check=True, timeout=20, env=environment)
    assert current.read_bytes() == b"file instead of local directory"
    assert sentinel.read_bytes() == b"pre-existing remote image"
    assert not list(remote.rglob(".visionpro-*"))
    assert not list(local.glob("*.png"))
    saved = list((remote / "质量/图像").glob("*-样件-*.png"))
    assert sorted(f.read_bytes() for f in saved) == [
        bytes([value]) * 512 for value in (1, 2, 2, 3)
    ]
    assert all(password.encode() not in f.read_bytes() for f in local.rglob("*.json"))
    with FtpServer(remote, port, password=password, reject_delete=True):
        subprocess.run([*command, "probe-delete"], check=True, timeout=20, env=environment)
    probe, = remote.rglob(".visionpro-probe-*")
    assert probe.read_bytes() == b"VP" and sentinel.read_bytes() == b"pre-existing remote image"
    probe.unlink()

    idle = root / "idle"
    client = subprocess.Popen([sys.argv[1], "ftp", str(idle), str(port), "idle"], env=environment)
    try:
        deadline = time.monotonic() + 10
        while not (idle / "ready").exists() and client.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
        assert (idle / "ready").exists() and list(idle.glob("idle-*.png"))
        with FtpServer(remote, port, password=password):
            assert client.wait(timeout=20) == 0
        assert not list(idle.glob("*.png"))
        restored, = (remote / "质量/图像").glob("idle-样件-*.png")
        assert restored.read_bytes() == bytes([1]) * 512
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
