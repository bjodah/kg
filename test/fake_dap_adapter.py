#!/usr/bin/env python3
"""A deterministic stand-in for a debug adapter, for kg's DAP tests.

Encoding contract: the Debug Adapter Protocol's framing is byte for byte
the Language Server Protocol's -- an ASCII header block, ``Content-Length:
N`` among it, terminated by an empty line, then exactly N bytes of body --
so the framing modes below are ``test/fake_lsp_server.py``'s, nearly
verbatim, and a test written against either stays written.  They treat
bodies as opaque and never parse one.

What is NOT shared, and is why this file exists at all, is the LIFECYCLE.
An adapter's end of session is not a server's: measured against the real
ones, lldb-dap exits the moment it has answered ``disconnect`` while
debugpy's adapter answers it and stays alive indefinitely (30 s observed,
transport still healthy).  So kg may not end a session by waiting for end
of stream, and must still end one that arrives as end of stream and nothing
else.  The lifecycle modes below are those edges, one mode each.

Usage::

    fake_dap_adapter.py --mode MODE [options]

Framing modes (from fake_lsp_server.py):

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
``truncated``
    Write a header block claiming ten bytes of body and three bytes of it,
    then exit: a frame cut in half by an adapter that stopped.

Lifecycle modes (new here; each one is a measured adapter behaviour):

``linger``
    Echo like ``echo``, but do not exit at end of input: report the EOF on
    standard error and stay alive for ``--linger-seconds`` (default 5).
    This is debugpy's adapter after ``disconnect``, and it is the mode that
    proves kg ends a session on the response rather than on the stream.
``ignore-term``
    ``linger`` with SIGTERM, SIGINT and SIGHUP ignored, so only SIGKILL
    ends it: the kill backstop at the bottom of the ladder is the only
    thing that can collect this one.
``half-close``
    Read frames until end of input without answering any, then report
    ``frames=N bytes=M`` on standard error.  A client that closed the write
    side before its outbox had drained truncates the last body, and the
    count says so.
``crash``
    Read one frame, answer nothing, and exit with ``--exit-code`` (default
    1): an adapter that died with a request in flight, whose session must
    end on end of stream alone.
``respond-then-exit``
    Read one frame, answer it, and exit at once -- lldb-dap's shape, where
    the response and the EOF can arrive in the same read.  The response
    must be delivered before the end of stream is.

Options:

``--listen``
    Instead of stdin/stdout, bind 127.0.0.1 on an ephemeral port, print
    ``PORT <n>`` on standard output, accept one connection and run the mode
    over that socket.  This is the attach shape: the adapter is a process
    the client did not start and must never signal, and the caller reads
    the port off this process's own output.
``--stderr LINE`` (repeatable)
    Written to standard error before the mode runs.  The adapter's standard
    error is a channel of its own, never joined to the frame stream.
``--chunk N``, ``--count N``, ``--length N``, ``--linger-seconds S``,
``--exit-code N``
    Mode-specific, described with their modes above.
"""

import argparse
import signal
import socket
import sys
import time

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
        raise SystemExit("fake_dap_adapter: request had no Content-Length")
    return read_exactly(stream, length) if length else b""


def frame(body):
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def write_all(stream, data):
    stream.write(data)
    stream.flush()


def note(text):
    """One line on standard error: the side channel a test reads a verdict
    from without ever putting a byte of it near the frame stream."""
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


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


def mode_truncated(_stdin, stdout, _args):
    write_all(stdout, b"Content-Length: 10\r\n\r\nabc")


def mode_linger(stdin, stdout, args):
    """debugpy's adapter: answers, then outlives the conversation."""
    mode_echo(stdin, stdout, args)
    note("linger: input ended")
    time.sleep(args.linger_seconds)


def mode_ignore_term(stdin, stdout, args):
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, signal.SIG_IGN)
    note("ignore-term: signals ignored")
    mode_linger(stdin, stdout, args)


def mode_half_close(stdin, _stdout, _args):
    """Count what actually arrived before the write side closed.  A body cut
    short by an early half-close is a `read_frame` that returns None with
    bytes already consumed, and the totals are how a test sees it."""
    frames = 0
    total = 0
    while True:
        body = read_frame(stdin)
        if body is None:
            break
        frames += 1
        total += len(body)
    note("half-close: frames=%d bytes=%d" % (frames, total))


def mode_crash(stdin, _stdout, args):
    read_frame(stdin)
    raise SystemExit(args.exit_code)


def mode_respond_then_exit(stdin, stdout, _args):
    body = read_frame(stdin)
    if body is not None:
        write_all(stdout, frame(body))


MODES = {
    "echo": mode_echo,
    "split": mode_split,
    "batch": mode_batch,
    "garbage": mode_garbage,
    "huge-header": mode_huge_header,
    "die": mode_die,
    "truncated": mode_truncated,
    "linger": mode_linger,
    "ignore-term": mode_ignore_term,
    "half-close": mode_half_close,
    "crash": mode_crash,
    "respond-then-exit": mode_respond_then_exit,
}


def listen_for_client():
    """The attach shape: announce a port on this process's own output, take
    one connection, and speak the protocol on it.  Nothing about this
    process is the client's to reap."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    sys.stdout.write("PORT %d\n" % listener.getsockname()[1])
    sys.stdout.flush()
    connection, _ = listener.accept()
    listener.close()
    return connection.makefile("rb"), connection.makefile("wb")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", required=True, choices=sorted(MODES))
    parser.add_argument("--chunk", type=int, default=1,
                        help="split: bytes per write")
    parser.add_argument("--count", type=int, default=2,
                        help="batch: frames to read before answering")
    parser.add_argument("--length", type=int, default=999999999999,
                        help="huge-header: the Content-Length to claim")
    parser.add_argument("--linger-seconds", type=float, default=5.0,
                        help="linger/ignore-term: seconds to outlive input")
    parser.add_argument("--exit-code", type=int, default=1,
                        help="crash: the status to exit with")
    parser.add_argument("--listen", action="store_true",
                        help="speak the protocol on an accepted TCP socket")
    parser.add_argument("--stderr", dest="stderr_line", action="append",
                        default=[], help="a line to log before the mode runs")
    args = parser.parse_args(argv[1:])
    for line in args.stderr_line:
        note(line)
    if args.listen:
        stdin, stdout = listen_for_client()
    else:
        stdin, stdout = sys.stdin.buffer, sys.stdout.buffer
    MODES[args.mode](stdin, stdout, args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
