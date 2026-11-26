"""Small passive FTP server for isolated image archive integration tests."""
import pathlib
import socket
import socketserver
import threading


class _FtpHandler(socketserver.StreamRequestHandler):
    def response(self, code, text):
        self.wfile.write(f"{code} {text}\r\n".encode("ascii"))

    def local_path(self, name):
        base = self.server.root if name.startswith("/") else self.cwd
        path = (base / name.lstrip("/")).resolve()
        if not path.is_relative_to(self.server.root):
            raise ValueError("outside fixture")
        return path

    def handle(self):
        self.cwd = self.server.root
        self.response(220, "ready")
        passive = None
        rename_from = None
        try:
            while line := self.rfile.readline(8192):
                cmd, _, arg = line.rstrip(b"\r\n").decode("utf-8").partition(" ")
                try:
                    if cmd == "USER":
                        self.response(331, "password required")
                    elif cmd == "PASS":
                        accepted = self.server.password is None or arg == self.server.password
                        self.response(230 if accepted else 530, "login result")
                    elif cmd in ("OPTS", "TYPE", "NOOP"):
                        self.response(200, "ok")
                    elif cmd == "CWD":
                        path = self.local_path(arg)
                        if path.is_dir():
                            self.cwd = path
                            self.response(250, "directory changed")
                        else:
                            self.response(550, "no directory")
                    elif cmd == "MKD":
                        self.local_path(arg).mkdir()
                        self.response(257, "created")
                    elif cmd in ("EPSV", "PASV"):
                        if passive:
                            passive.close()
                        passive = socket.create_server(("127.0.0.1", 0))
                        passive.settimeout(5)
                        port = passive.getsockname()[1]
                        if cmd == "EPSV":
                            self.response(229, f"Entering Extended Passive Mode (|||{port}|)")
                        else:
                            self.response(227, f"(127,0,0,1,{port//256},{port%256})")
                    elif cmd == "STOR" and passive:
                        self.response(150, "opening data")
                        with passive.accept()[0] as channel, self.local_path(arg).open("wb") as target:
                            while data := channel.recv(65536):
                                target.write(data)
                        passive.close()
                        passive = None
                        self.response(226, "complete")
                    elif cmd == "SIZE" and self.local_path(arg).is_file():
                        self.response(213, str(self.local_path(arg).stat().st_size))
                    elif cmd == "RNFR" and self.local_path(arg).exists():
                        rename_from = self.local_path(arg)
                        self.response(350, "ready")
                    elif cmd == "RNTO" and rename_from is not None:
                        if self.server.reject_publish:
                            self.response(550, "publication unavailable")
                            continue
                        rename_from.replace(self.local_path(arg))
                        rename_from = None
                        self.response(250, "renamed")
                    elif cmd == "DELE":
                        if self.server.reject_delete:
                            self.response(550, "deletion unavailable")
                            continue
                        self.local_path(arg).unlink()
                        self.response(250, "removed")
                    elif cmd == "QUIT":
                        self.response(221, "bye")
                        return
                    else:
                        self.response(550, "unavailable")
                except (ValueError, OSError):
                    self.response(550, "operation failed")
        finally:
            if passive:
                passive.close()


class _TcpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class FtpServer:
    def __init__(self, remote_directory, port=0, reject_publish=False, password=None, reject_delete=False):
        self.server = _TcpServer(("127.0.0.1", port), _FtpHandler)
        self.server.reject_publish = reject_publish
        self.server.reject_delete = reject_delete
        self.server.password = password
        self.server.root = pathlib.Path(remote_directory).resolve()
        self.server.root.mkdir(parents=True, exist_ok=True)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
