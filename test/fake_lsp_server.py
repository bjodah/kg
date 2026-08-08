#!/usr/bin/env python3
"""A deterministic stand-in for a language server, for kg's LSP tests.

Encoding contract (LSP base protocol, and nothing above it at this stage):
a message is an ASCII header block, one ``Name: value`` field per line with
CRLF line endings, terminated by an empty line, followed by exactly
``Content-Length`` bytes of body.  Bodies are opaque here: this server never
parses one, so the tests can send whatever they find convenient.

Everything is read from stdin and written to stdout.  Nothing is ever
written to stdout that is not part of a frame -- diagnostics go to stderr,
which the transport routes to /dev/null -- except in the modes whose whole
purpose is to misbehave.  Every mode is deterministic: no timing, no
randomness, no environment.

Usage::

    fake_lsp_server.py --mode MODE [--count N] [--chunk N] [--length N]

``--mode`` names the behaviour; the remaining options are mode-specific and
ignored where they do not apply.  Stage 1 of doc/plans/2026-08-08-lsp.md
needs framing behaviour only, so every mode here is about bytes on the
wire.  Later stages add protocol modes (initialize/definition/references)
as new ``--mode`` values; the framing modes below keep their names and
their meaning, so a test written against them stays written.

Framing modes:

``echo``
    Read frames until end of input, echoing each body back framed, one
    response per request.
``split``
    Like ``echo``, but each response leaves in ``--chunk`` byte pieces
    (default 1) with a flush after each, so the reader sees the header
    split mid-token and the body split mid-message.
``batch``
    Read ``--count`` frames (default 2), then write all of their bodies
    back as that many frames in a single write.
``garbage``
    Write bytes that are not a header block at all, then exit.
``huge-header``
    Write a header block whose ``Content-Length`` is ``--length`` (default
    999999999999, past any bound a client should accept), then exit
    without a body.
``die``
    Exit immediately, having written nothing.
"""

import argparse
import sys

GARBAGE = b"\x01\x02 this is not a header block\r\n\r\n"


def read_exactly(stream, count):
    """Read exactly `count` bytes, or return None at end of input."""
    chunks = []
    got = 0
    while got < count:
        chunk = stream.read(count - got)
        if not chunk:
            return None
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def read_frame(stream):
    """Read one framed message and return its body, or None at end of input."""
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        byte = stream.read(1)
        if not byte:
            return None
        header += byte
    length = None
    for line in header.split(b"\r\n"):
        if not line:
            continue
        name, _, value = line.partition(b":")
        if name.strip().lower() == b"content-length":
            length = int(value.strip())
    if length is None:
        raise SystemExit("fake_lsp_server: request had no Content-Length")
    return read_exactly(stream, length) if length else b""


def frame(body):
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def write_all(stream, data):
    stream.write(data)
    stream.flush()


def mode_echo(stdin, stdout, _args):
    while True:
        body = read_frame(stdin)
        if body is None:
            return
        write_all(stdout, frame(body))


def mode_split(stdin, stdout, args):
    step = max(1, args.chunk)
    while True:
        body = read_frame(stdin)
        if body is None:
            return
        data = frame(body)
        for start in range(0, len(data), step):
            write_all(stdout, data[start:start + step])


def mode_batch(stdin, stdout, args):
    bodies = []
    for _ in range(args.count):
        body = read_frame(stdin)
        if body is None:
            break
        bodies.append(body)
    write_all(stdout, b"".join(frame(b) for b in bodies))


def mode_garbage(_stdin, stdout, _args):
    write_all(stdout, GARBAGE)


def mode_huge_header(_stdin, stdout, args):
    write_all(stdout, b"Content-Length: %d\r\n\r\n" % args.length)


def mode_die(_stdin, _stdout, _args):
    return


MODES = {
    "echo": mode_echo,
    "split": mode_split,
    "batch": mode_batch,
    "garbage": mode_garbage,
    "huge-header": mode_huge_header,
    "die": mode_die,
}


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", required=True, choices=sorted(MODES))
    parser.add_argument("--count", type=int, default=2,
                        help="frames to collect before replying (batch)")
    parser.add_argument("--chunk", type=int, default=1,
                        help="bytes per write when replying (split)")
    parser.add_argument("--length", type=int, default=999999999999,
                        help="Content-Length to claim (huge-header)")
    args = parser.parse_args(argv[1:])
    MODES[args.mode](sys.stdin.buffer, sys.stdout.buffer, args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
