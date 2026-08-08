#!/usr/bin/env python3
"""A deterministic stand-in for a language server, for kg's LSP tests.

Encoding contract (LSP base protocol): a message is an ASCII header block,
one ``Name: value`` field per line with CRLF line endings, terminated by an
empty line, followed by exactly ``Content-Length`` bytes of body.  The
framing modes below treat bodies as opaque and never parse one; ``protocol``
mode parses them as JSON-RPC and answers.

Everything is read from stdin and written to stdout.  Nothing is ever
written to stdout that is not part of a frame -- diagnostics go to stderr,
which the transport routes to /dev/null -- except in the modes whose whole
purpose is to misbehave.  Every mode is deterministic apart from
``--delay-ms``, which is a duration a test asked for.

Usage::

    fake_lsp_server.py --mode MODE [mode-specific options]

``--mode`` names the behaviour; the remaining options are mode-specific and
ignored where they do not apply.

Framing modes (Stage 1 of doc/plans/2026-08-08-lsp.md; unchanged since, and
a test written against them stays written):

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

Protocol mode (Stage 3): ``protocol`` speaks just enough JSON-RPC to be a
language server -- initialize/initialized, definition, references,
shutdown/exit -- with every answer canned by argv, so a test asserts against
a value it chose rather than against whatever a real server happened to say.

Options, all of them optional:

``--position-encoding utf-8|utf-16``
    What the initialize result advertises (default ``utf-16``, the
    protocol's own default).
``--sync full|incremental|none``
    The ``textDocumentSync`` capability, sent in its object form with
    ``openClose`` true.
``--no-open-close``
    Send ``openClose`` false with it: a server that wants no documents
    pushed to it at all, which with ``--sync none`` is the whole of the
    "this server reads files from disk" configuration.
``--definition URI:LINE:CHAR`` / ``--definition-none``
    What ``textDocument/definition`` answers: one Location, or JSON null.
``--reference URI:LINE:CHAR`` (repeatable)
    What ``textDocument/references`` answers, as an array of Locations; an
    empty list answers ``[]``.
``--server-request METHOD``
    Before the first reply, send a server-to-client *request* named METHOD.
    The client is required to answer it with a MethodNotFound error;
    whether it did is reported by ``kg/state``.
``--notify METHOD``
    Before the first reply, send a notification named METHOD, which the
    client is required to ignore.
``--delay-ms N``
    Sleep N milliseconds before each reply, so a test can prove nothing
    blocks on an answer that has not arrived.
``--exit-after N``
    Exit as soon as N replies have been sent.
``--die-on METHOD``
    Exit, without replying, the moment a request for METHOD arrives: a
    server crashing with a request outstanding, which is the one death a
    client cannot notice by an answer failing to make sense.
``--reverse-pairs``
    Answer ``kg/echo`` requests in pairs, second one first, so a client
    that matched responses by arrival order instead of by id gets them
    swapped.
``--record FILE``
    Append one JSON object per line to FILE for every
    ``textDocument/did*`` notification received --
    ``{"method": ..., "params": ...}`` -- flushed and fsync'd before the
    reply that follows it, so a test polling the file sees a whole line or
    no line and never half of one.  This is how the document-sync tests
    (Stage 4) assert the exact payload kg sent rather than an effect of it.

Two methods exist only for the tests, and are named with kg's own prefix so
they cannot be confused with the protocol's:

``kg/echo``
    Answers with the request's own params, which is how a test proves a
    response reached the callback that asked for it.
``kg/state``
    Answers ``{"methodNotFound": bool, "handled": int}``: whether the
    client answered this server's request with error -32601, and how many
    requests have been handled so far.
"""

import argparse
import json
import os
import sys
import time

GARBAGE = b"\x01\x02 this is not a header block\r\n\r\n"

SYNC_KINDS = {"none": 0, "full": 1, "incremental": 2}

SERVER_REQUEST_ID = 424242


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


def parse_location(spec):
    """``URI:LINE:CHAR`` into a Location, splitting from the right so a
    ``file://`` URI's own colon stays where it is."""
    uri, line, char = spec.rsplit(":", 2)
    position = {"line": int(line), "character": int(char)}
    return {"uri": uri, "range": {"start": position, "end": position}}


class Protocol:
    """One session: canned answers, and the bookkeeping the tests read back."""

    def __init__(self, stdout, args):
        self.stdout = stdout
        self.args = args
        self.replies = 0
        self.handled = 0
        self.method_not_found = False
        self.greeted = False
        self.deferred = []

    def send(self, message):
        write_all(self.stdout, frame(json.dumps(message).encode("utf-8")))

    def reply(self, request_id, result):
        if self.args.delay_ms:
            time.sleep(self.args.delay_ms / 1000.0)
        self.send({"jsonrpc": "2.0", "id": request_id, "result": result})
        self.replies += 1
        if self.args.exit_after and self.replies >= self.args.exit_after:
            raise SystemExit(0)

    def greet(self):
        """The unsolicited traffic, sent once, before the first reply."""
        if self.greeted:
            return
        self.greeted = True
        if self.args.server_request:
            self.send({"jsonrpc": "2.0", "id": SERVER_REQUEST_ID,
                       "method": self.args.server_request, "params": {}})
        if self.args.notify:
            self.send({"jsonrpc": "2.0", "method": self.args.notify,
                       "params": {}})

    def initialize_result(self):
        return {
            "capabilities": {
                "positionEncoding": self.args.position_encoding,
                "textDocumentSync": {
                    "openClose": not self.args.no_open_close,
                    "change": SYNC_KINDS[self.args.sync],
                },
                "definitionProvider": True,
                "referencesProvider": True,
            },
            "serverInfo": {"name": "fake_lsp_server"},
        }

    def canned(self, method, params):
        if method == "initialize":
            return self.initialize_result()
        if method == "textDocument/definition":
            if self.args.definition_none or not self.args.definition:
                return None
            return parse_location(self.args.definition)
        if method == "textDocument/references":
            return [parse_location(r) for r in self.args.reference]
        if method == "kg/echo":
            return params
        if method == "kg/state":
            return {"methodNotFound": self.method_not_found,
                    "handled": self.handled}
        if method == "shutdown":
            return None
        return None

    def on_response(self, message):
        """The client answering the request this server sent it."""
        if message.get("id") != SERVER_REQUEST_ID:
            return
        error = message.get("error") or {}
        self.method_not_found = error.get("code") == -32601

    def record(self, message):
        """Append a document notification to --record, whole line or none.

        Opened, written and closed per line so the file is complete after
        every one of them: a test reading it concurrently must never see a
        line the server is still in the middle of writing."""
        method = message.get("method", "")
        if not self.args.record or not method.startswith("textDocument/did"):
            return
        line = json.dumps({"method": method,
                           "params": message.get("params")})
        with open(self.args.record, "a", encoding="utf-8") as handle:
            handle.write(line + "\n")
            handle.flush()
            os.fsync(handle.fileno())

    def on_request(self, message):
        method = message["method"]
        if method == "exit":
            raise SystemExit(0)
        self.record(message)
        self.greet()
        if self.args.die_on and method == self.args.die_on:
            raise SystemExit(1)
        if "id" not in message:
            return  # a notification: initialized, and whatever else
        self.handled += 1
        result = self.canned(method, message.get("params"))
        if self.args.reverse_pairs and method == "kg/echo":
            self.deferred.append((message["id"], result))
            if len(self.deferred) >= 2:
                for pending_id, pending in reversed(self.deferred):
                    self.reply(pending_id, pending)
                self.deferred = []
            return
        self.reply(message["id"], result)

    def run(self, stdin):
        while True:
            body = read_frame(stdin)
            if body is None:
                return
            message = json.loads(body.decode("utf-8"))
            if "method" in message:
                self.on_request(message)
            else:
                self.on_response(message)


def mode_protocol(stdin, stdout, args):
    Protocol(stdout, args).run(stdin)


MODES = {
    "echo": mode_echo,
    "split": mode_split,
    "batch": mode_batch,
    "garbage": mode_garbage,
    "huge-header": mode_huge_header,
    "die": mode_die,
    "protocol": mode_protocol,
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
    parser.add_argument("--position-encoding", default="utf-16",
                        choices=["utf-8", "utf-16"],
                        help="positionEncoding to advertise (protocol)")
    parser.add_argument("--sync", default="incremental",
                        choices=sorted(SYNC_KINDS),
                        help="textDocumentSync change kind (protocol)")
    parser.add_argument("--no-open-close", action="store_true",
                        help="advertise openClose false (protocol)")
    parser.add_argument("--definition", default=None,
                        help="URI:LINE:CHAR answered to definition requests")
    parser.add_argument("--definition-none", action="store_true",
                        help="answer definition requests with null")
    parser.add_argument("--reference", action="append", default=[],
                        help="URI:LINE:CHAR added to the references answer")
    parser.add_argument("--server-request", default=None,
                        help="method of a request sent to the client")
    parser.add_argument("--notify", default=None,
                        help="method of a notification sent to the client")
    parser.add_argument("--delay-ms", type=int, default=0,
                        help="milliseconds to wait before each reply")
    parser.add_argument("--exit-after", type=int, default=0,
                        help="exit once this many replies have been sent")
    parser.add_argument("--die-on", default=None,
                        help="exit without replying when this method arrives")
    parser.add_argument("--reverse-pairs", action="store_true",
                        help="answer kg/echo requests in pairs, reversed")
    parser.add_argument("--record", default=None,
                        help="file to append received didOpen/didChange/"
                             "didClose params to, one JSON object per line")
    args = parser.parse_args(argv[1:])
    MODES[args.mode](sys.stdin.buffer, sys.stdout.buffer, args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
