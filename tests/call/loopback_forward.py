#!/usr/bin/env python3
"""Forwards 127.0.0.1:<port> to <host>:<port>, for run.sh's outside peer.

Runs inside the outside peer's network namespace, so that its browser loads the call page from
a loopback origin: getUserMedia needs a secure origin, loopback counts as one, and headless
Chrome ignores --unsafely-treat-insecure-origin-as-secure.
"""
import socket
import sys
import threading


def pipe(src: socket.socket, dst: socket.socket) -> None:
    try:
        while data := src.recv(65536):
            dst.sendall(data)
    except OSError:
        pass
    finally:
        src.close()
        dst.close()


def main() -> None:
    port, host = int(sys.argv[1]), sys.argv[2]
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen()
    while True:
        client, _ = listener.accept()
        upstream = socket.create_connection((host, port))
        for a, b in ((client, upstream), (upstream, client)):
            threading.Thread(target=pipe, args=(a, b), daemon=True).start()


if __name__ == "__main__":
    main()
