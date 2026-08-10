#!/usr/bin/env python3
"""A deterministic stand-in for a language server, for kg's LSP tests.

Encoding contract (LSP base protocol): a message is an ASCII header block,
one ``Name: value`` field per line with CRLF line endings, terminated by an
empty line, followed by exactly ``Content-Length`` bytes of body.  The
framing modes below treat bodies as opaque and never parse one; ``protocol``
mode parses them as JSON-RPC and answers.

Everything is read from stdin and written to stdout.  Nothing is ever
written to stdout that is not part of a frame -- diagnostics go to stderr,
which the transport reads on a pipe of its own and hands to kg's
``*lsp-log*`` buffer (``--stderr`` below) -- except in the modes whose whole
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
``--definition-self LINE:CHAR``
    Answer ``textDocument/definition`` with a Location in the document the
    client most recently opened or changed, at LINE:CHAR.  It exists
    because a PTY case edits a file in a temporary directory whose name
    neither the case nor this server knows: pointing the answer at the URI
    the client itself sent removes the absolute path from the test's
    vocabulary entirely.  Takes precedence over ``--definition``; if no
    document has been opened, the answer is null.
``--definition-echo``
    Answer ``textDocument/definition`` with a Location at the very position
    the request asked about, in the document it named.  It is the only way
    a test outside this process can see which column the client encoded:
    the answer comes back in the same units it was asked in, so point lands
    where it started only if the client's encoding and its decoding agree
    with the negotiated one.  Takes precedence over ``--definition-self``
    and ``--definition``, and yields to ``--definition-none``.
``--reference URI:LINE:CHAR`` (repeatable)
    What ``textDocument/references`` answers, as an array of Locations; an
    empty list answers ``[]``.
``--references-self LINE:CHAR,LINE:CHAR,...``
    Answer ``textDocument/references`` with one Location per pair, all of
    them in the document the client most recently opened or changed --
    ``--definition-self`` for the references request, and for the same
    reason: a PTY case whose file lives in a temporary directory nobody
    named can still assert exact targets.  Takes precedence over
    ``--reference``; if no document has been opened, the answer is ``[]``.
``--publish-diagnostic LINE:CHAR:SEVERITY:MESSAGE`` (repeatable)
    Publish these as ``textDocument/publishDiagnostics`` for a document as
    soon as the client opens it (``textDocument/didOpen``), which is the
    unsolicited traffic the protocol's diagnostics are.  The range is
    LINE:CHAR to the end of that line as the server sees it -- CHAR + the
    length of MESSAGE is not a range anybody could predict, so the end is
    CHAR + ``--publish-width`` (default 1).  SEVERITY is the protocol's
    number (1 error, 2 warning, 3 information, 4 hint).  The split is from
    the left and stops after three, so MESSAGE may contain colons.
    The publish names the document the client just opened, so a case whose
    file lives in a temporary directory nobody named still gets
    diagnostics about its own file -- ``--definition-self``'s reason.
``--publish-version N``
    Send ``version: N`` with the publish.  Absent, no version is sent at
    all, which is what a server that does not track them does.
``--publish-empty-after N``
    Publish the diagnostics above on the first didOpen, then publish an
    empty list on the Nth ``textDocument/did*`` notification after it: a
    server taking back what it said, which is the only way a client's
    "replace, do not merge" semantics is observable from outside.
``--references-sibling NAME:LINE:CHAR`` (repeatable)
    One more Location per option, in the file NAME *beside* the document
    the client last sent, appended after ``--references-self``'s.  Same
    reason as that one -- the case cannot spell the temporary directory it
    runs in -- for the answers that must name a file the editor has not
    opened, which is the only way to ask for text it has to go to disk
    for.  Ignored before a document has been opened.
``--hover TEXT``
    What ``textDocument/hover`` answers, as a MarkupContent whose ``kind``
    is ``--hover-kind`` (default ``markdown``).  A literal ``\n`` in TEXT
    becomes a newline, which is how a case asks for the multi-line answer.
``--hover-kind markdown|plaintext``
    The MarkupContent kind sent with ``--hover``.
``--hover-plain``
    Answer with a bare string instead of a MarkupContent: the protocol's
    oldest of the three shapes, and the one a client is likeliest to
    render as its JSON.
``--hover-marked``
    Answer with an array of MarkedString -- ``{language, value}`` and a
    bare string -- which is the third shape.
``--hover-none``
    Answer ``textDocument/hover`` with null.
``--rename-self LINE:START:END`` (repeatable)
    One TextEdit per option, in the document the client last sent,
    replacing the bytes [START, END) of LINE with the ``newName`` the
    request itself carried -- so a case asserts the name it typed made the
    round trip, and, as with ``--definition-self``, never has to spell the
    temporary directory it runs in.  Without any of these (and without
    ``--rename-sibling``) a rename request is answered with ``null``,
    which is how a server says it will not rename that position.
``--rename-sibling NAME:LINE:START:END`` (repeatable)
    The same, in the file NAME *beside* the last document: the multi-file
    half of a rename, which is the only part that proves kg opens a file
    nobody was editing and leaves it modified.
``--rename-shape changes|documentChanges``
    Which of the WorkspaceEdit's two shapes to answer with (default
    ``changes``).  ``documentChanges`` sends TextDocumentEdit objects with
    versioned identifiers, which is what a modern server does.
``--rename-resource-op KIND``
    Add a resource operation of that kind (``create``, ``rename``,
    ``delete``) to the ``documentChanges`` array.  kg performs none of
    them and says how many it skipped; this is how a test sees that.
``--completion LABEL`` (repeatable)
    A CompletionItem with nothing but a label: the plainest shape a
    server can answer ``textDocument/completion`` with.
``--completion-insert LABEL:TEXT`` (repeatable)
    An item whose ``insertText`` differs from its label.
``--completion-edit LABEL:LINE:START:END`` (repeatable)
    An item carrying a ``textEdit`` that replaces [START, END) of LINE
    with the label -- the shape that tells the client where the
    completion begins instead of leaving it to guess.
``--completion-list``
    Answer with a CompletionList (``{isIncomplete, items}``) rather than
    the bare array.  Both are legal and a client has to read both.
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
``--delay-method METHOD``
    Restrict ``--delay-ms`` to replies to METHOD.  Without it the delay is
    paid on every reply including ``initialize``, which a test about one
    slow request does not want -- a handshake delayed past the client's
    own deadline is a different case.
``--no-reply METHOD`` (repeatable)
    Read requests for METHOD, count them, and answer none of them, ever:
    a server that is alive and stuck, which is the one failure a client
    cannot notice by watching the child die.  Everything else is answered
    as usual, so a test can prove the server still works afterwards.
``--stderr TEXT`` (repeatable)
    Write TEXT, and a newline, to standard error before reading anything:
    a server explaining itself on the channel the protocol does not use.
    Written at startup so a test never has to guess when it appears.
``--exit-after N``
    Exit as soon as N replies have been sent.
``--die-on METHOD``
    Exit, without replying, the moment a request for METHOD arrives: a
    server crashing with a request outstanding, which is the one death a
    client cannot notice by an answer failing to make sense.
``--garbage-reply METHOD``
    Answer a request for METHOD with a correctly framed message whose body
    is not JSON, and send nothing else.  The framing modes above corrupt
    the transport; this corrupts what the transport delivered, which is a
    protocol violation the client is required to treat as fatal rather
    than skip past -- a client that guessed would navigate somewhere the
    server never named.
``--huge-after N``
    After the Nth reply, write a header block claiming ``--length`` bytes
    and exit: an oversized frame arriving mid-session, once the handshake
    has settled and requests are outstanding, rather than as the first
    thing on the pipe (which is what ``huge-header`` mode covers).
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

# A body that is framed correctly and is not JSON: --garbage-reply's whole
# payload.  Deliberately not "almost JSON" -- the client's contract is that
# an unparseable body is fatal, and a truncated object would test the
# parser's recovery instead of that contract.
GARBAGE_BODY = b"\x01\x02 this is framed, and it is not JSON"

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
        # The URI of the document the client last told this server about,
        # which is what --definition-self answers in.
        self.last_uri = None
        # How many textDocument/did* notifications have arrived, which is
        # what --publish-empty-after counts.
        self.did_count = 0
        self.published = False

    def send(self, message):
        write_all(self.stdout, frame(json.dumps(message).encode("utf-8")))

    def reply(self, request_id, result, method=None):
        if self.args.delay_ms and (not self.args.delay_method
                                   or method == self.args.delay_method):
            time.sleep(self.args.delay_ms / 1000.0)
        self.send({"jsonrpc": "2.0", "id": request_id, "result": result})
        self.replies += 1
        if self.args.huge_after and self.replies >= self.args.huge_after:
            write_all(self.stdout,
                      b"Content-Length: %d\r\n\r\n" % self.args.length)
            raise SystemExit(0)
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
                "hoverProvider": True,
                "renameProvider": True,
                "completionProvider": {"triggerCharacters": ["."]},
            },
            "serverInfo": {"name": "fake_lsp_server"},
        }

    def canned(self, method, params):
        if method == "initialize":
            return self.initialize_result()
        if method == "textDocument/definition":
            if self.args.definition_none:
                return None
            if self.args.definition_echo:
                return self.echo_location(params)
            if self.args.definition_self:
                return self.self_location()
            if not self.args.definition:
                return None
            return parse_location(self.args.definition)
        if method == "textDocument/references":
            if self.args.references_self or self.args.references_sibling:
                return (self.self_locations(self.args.references_self)
                        + self.sibling_locations(
                            self.args.references_sibling))
            return [parse_location(r) for r in self.args.reference]
        if method == "textDocument/hover":
            return self.hover_result()
        if method == "textDocument/rename":
            return self.rename_result(params)
        if method == "textDocument/completion":
            return self.completion_result()
        if method == "kg/echo":
            return params
        if method == "kg/state":
            return {"methodNotFound": self.method_not_found,
                    "handled": self.handled}
        if method == "shutdown":
            return None
        return None

    def hover_result(self):
        """Hover.contents in whichever of the three shapes was asked for."""
        if self.args.hover_none:
            return None
        text = (self.args.hover or "").replace("\\n", "\n")
        if self.args.hover_marked:
            return {"contents": [{"language": "c", "value": text},
                                 "and a bare string"]}
        if self.args.hover_plain:
            return {"contents": text}
        return {"contents": {"kind": self.args.hover_kind, "value": text}}

    def diagnostics(self):
        """--publish-diagnostic as protocol Diagnostic objects."""
        out = []
        for spec in self.args.publish_diagnostic:
            line, char, severity, message = spec.split(":", 3)
            start = {"line": int(line), "character": int(char)}
            end = {"line": int(line),
                   "character": int(char) + self.args.publish_width}
            out.append({"range": {"start": start, "end": end},
                        "severity": int(severity),
                        "source": "fake_lsp_server",
                        "message": message})
        return out

    def publish(self, diagnostics):
        """One textDocument/publishDiagnostics notification."""
        params = {"uri": self.last_uri, "diagnostics": diagnostics}
        if self.args.publish_version is not None:
            params["version"] = self.args.publish_version
        self.send({"jsonrpc": "2.0",
                   "method": "textDocument/publishDiagnostics",
                   "params": params})

    def maybe_publish(self, method):
        """The unsolicited diagnostics traffic, driven by the client's own
        document notifications: the first didOpen publishes what argv
        asked for, and --publish-empty-after takes it back later."""
        if not self.args.publish_diagnostic or not self.last_uri:
            return
        if not method.startswith("textDocument/did"):
            return
        self.did_count += 1
        if not self.published:
            self.published = True
            self.publish(self.diagnostics())
            return
        if (self.args.publish_empty_after
                and self.did_count > self.args.publish_empty_after):
            self.publish([])

    def text_edits(self, specs, new_text):
        """One TextEdit per LINE:START:END spec."""
        out = []
        for spec in specs:
            line, start, end = (int(n) for n in spec.split(":"))
            out.append({"range": {"start": {"line": line,
                                            "character": start},
                                  "end": {"line": line, "character": end}},
                        "newText": new_text})
        return out

    def sibling_uri(self, name):
        return self.last_uri.rsplit("/", 1)[0] + "/" + name

    def rename_edits(self, new_text):
        """(uri, edits) pairs for every --rename-* option, in order."""
        pairs = []
        if self.args.rename_self:
            pairs.append((self.last_uri,
                          self.text_edits(self.args.rename_self, new_text)))
        grouped = {}
        for spec in self.args.rename_sibling:
            name, rest = spec.split(":", 1)
            grouped.setdefault(name, []).append(rest)
        for name, specs in grouped.items():
            pairs.append((self.sibling_uri(name),
                          self.text_edits(specs, new_text)))
        return pairs

    def rename_result(self, params):
        """A WorkspaceEdit in whichever shape --rename-shape asked for."""
        if not self.last_uri:
            return None
        new_text = (params or {}).get("newName", "renamed")
        pairs = self.rename_edits(new_text)
        if not pairs and not self.args.rename_resource_op:
            return None
        if self.args.rename_shape == "changes":
            return {"changes": {uri: edits for uri, edits in pairs}}
        changes = [{"textDocument": {"uri": uri, "version": 1},
                    "edits": edits} for uri, edits in pairs]
        if self.args.rename_resource_op:
            changes.append({"kind": self.args.rename_resource_op,
                            "uri": self.sibling_uri("created.c")})
        return {"documentChanges": changes}

    def completion_result(self):
        """The canned candidate set, as an array or a CompletionList."""
        items = []
        for spec in self.args.completion_edit:
            label, line, start, end = spec.split(":")
            position = {"line": int(line), "character": int(start)}
            items.append({"label": label,
                          "textEdit": {
                              "range": {"start": position,
                                        "end": {"line": int(line),
                                                "character": int(end)}},
                              "newText": label}})
        for spec in self.args.completion_insert:
            label, text = spec.split(":", 1)
            items.append({"label": label, "insertText": text})
        for label in self.args.completion:
            items.append({"label": label})
        if self.args.completion_list:
            return {"isIncomplete": False, "items": items}
        return items

    def echo_location(self, params):
        """A Location at exactly the position the request asked about.

        The mirror of --definition-self: that one answers where a test told
        it to, this one answers *where the client pointed*, which is the
        only way a test outside this process can see the position kg
        encoded.  A client that encoded the column in the wrong unit gets
        that same wrong column back and lands point on it, so a round trip
        through here is an assertion about the encoding rather than about
        the answer.  Null when the request named no document or no
        position."""
        params = params or {}
        document = params.get("textDocument") or {}
        uri = document.get("uri")
        position = params.get("position")
        if not uri or not position:
            return None
        return {"uri": uri,
                "range": {"start": position, "end": position}}

    def self_location(self):
        """A Location in the document the client last sent, or null."""
        if not self.last_uri:
            return None
        line, char = self.args.definition_self.split(":")
        position = {"line": int(line), "character": int(char)}
        return {"uri": self.last_uri,
                "range": {"start": position, "end": position}}

    def sibling_locations(self, specs):
        """Locations in files beside the last document the client sent,
        one per NAME:LINE:CHAR spec, or [] before there is one.

        The URI is built from the last document's own, so the temporary
        directory a case runs in stays out of the case."""
        if not self.last_uri or not specs:
            return []
        directory = self.last_uri.rsplit("/", 1)[0]
        out = []
        for spec in specs:
            name, line, char = spec.split(":")
            position = {"line": int(line), "character": int(char)}
            out.append({"uri": f"{directory}/{name}",
                        "range": {"start": position, "end": position}})
        return out

    def self_locations(self, spec):
        """Locations in the last document the client sent, one per
        comma-separated LINE:CHAR pair, or [] before there is one."""
        if not self.last_uri or not spec:
            return []
        out = []
        for pair in spec.split(","):
            line, char = pair.split(":")
            position = {"line": int(line), "character": int(char)}
            out.append({"uri": self.last_uri,
                        "range": {"start": position, "end": position}})
        return out

    def note_document(self, message):
        """Remember the URI of any textDocument notification's document."""
        params = message.get("params") or {}
        document = params.get("textDocument") or {}
        uri = document.get("uri")
        if uri:
            self.last_uri = uri

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
        if method.startswith("textDocument/"):
            self.note_document(message)
        self.record(message)
        self.maybe_publish(method)
        self.greet()
        if self.args.die_on and method == self.args.die_on:
            raise SystemExit(1)
        if "id" not in message:
            return  # a notification: initialized, and whatever else
        self.handled += 1
        if method in self.args.no_reply:
            # Received, counted, and left unanswered forever: the client's
            # own deadline is the only thing that can end this request.
            return
        if self.args.garbage_reply and method == self.args.garbage_reply:
            write_all(self.stdout, frame(GARBAGE_BODY))
            return
        result = self.canned(method, message.get("params"))
        if self.args.reverse_pairs and method == "kg/echo":
            self.deferred.append((message["id"], result))
            if len(self.deferred) >= 2:
                for pending_id, pending in reversed(self.deferred):
                    self.reply(pending_id, pending, method)
                self.deferred = []
            return
        self.reply(message["id"], result, method)

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
    for line in args.stderr_line:
        sys.stderr.write(line + "\n")
    sys.stderr.flush()
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
    parser.add_argument("--definition-self", default=None,
                        help="LINE:CHAR answered to definition requests, in "
                             "the document the client last sent")
    parser.add_argument("--definition-echo", action="store_true",
                        help="answer definition requests with a Location at "
                             "the position they asked about")
    parser.add_argument("--reference", action="append", default=[],
                        help="URI:LINE:CHAR added to the references answer")
    parser.add_argument("--references-self", default=None,
                        help="comma-separated LINE:CHAR pairs answered to "
                             "references requests, in the document the "
                             "client itself opened")
    parser.add_argument("--references-sibling", action="append", default=[],
                        help="NAME:LINE:CHAR beside the last document, "
                             "appended to the references answer")
    parser.add_argument("--publish-diagnostic", action="append", default=[],
                        help="LINE:CHAR:SEVERITY:MESSAGE published for the "
                             "document the client opens")
    parser.add_argument("--publish-width", type=int, default=1,
                        help="characters a published diagnostic's range "
                             "covers")
    parser.add_argument("--publish-version", type=int, default=None,
                        help="version sent with the publish")
    parser.add_argument("--publish-empty-after", type=int, default=0,
                        help="publish an empty list after this many "
                             "textDocument/did* notifications")
    parser.add_argument("--hover", default=None,
                        help="text answered to hover requests; \\n is a "
                             "newline")
    parser.add_argument("--hover-kind", default="markdown",
                        choices=["markdown", "plaintext"],
                        help="MarkupContent kind sent with --hover")
    parser.add_argument("--hover-plain", action="store_true",
                        help="answer hover with a bare string")
    parser.add_argument("--hover-marked", action="store_true",
                        help="answer hover with an array of MarkedString")
    parser.add_argument("--hover-none", action="store_true",
                        help="answer hover requests with null")
    parser.add_argument("--rename-self", action="append", default=[],
                        help="LINE:START:END replaced in the last document "
                             "by a rename request's newName")
    parser.add_argument("--rename-sibling", action="append", default=[],
                        help="NAME:LINE:START:END replaced in a file beside "
                             "the last document")
    parser.add_argument("--rename-shape", default="changes",
                        choices=["changes", "documentChanges"],
                        help="which WorkspaceEdit shape to answer with")
    parser.add_argument("--rename-resource-op", default=None,
                        choices=["create", "rename", "delete"],
                        help="add a resource operation to documentChanges")
    parser.add_argument("--completion", action="append", default=[],
                        help="label of a bare CompletionItem")
    parser.add_argument("--completion-insert", action="append", default=[],
                        help="LABEL:TEXT of an item with an insertText")
    parser.add_argument("--completion-edit", action="append", default=[],
                        help="LABEL:LINE:START:END of an item with a "
                             "textEdit")
    parser.add_argument("--completion-list", action="store_true",
                        help="answer completion with a CompletionList")
    parser.add_argument("--server-request", default=None,
                        help="method of a request sent to the client")
    parser.add_argument("--notify", default=None,
                        help="method of a notification sent to the client")
    parser.add_argument("--delay-ms", type=int, default=0,
                        help="milliseconds to wait before each reply")
    parser.add_argument("--delay-method", default=None,
                        help="restrict --delay-ms to replies to this method")
    parser.add_argument("--no-reply", action="append", default=[],
                        help="read requests for this method and never answer "
                             "one")
    parser.add_argument("--stderr", dest="stderr_line", action="append",
                        default=[],
                        help="line written to standard error at startup")
    parser.add_argument("--exit-after", type=int, default=0,
                        help="exit once this many replies have been sent")
    parser.add_argument("--die-on", default=None,
                        help="exit without replying when this method arrives")
    parser.add_argument("--garbage-reply", default=None,
                        help="answer this method with a framed non-JSON body")
    parser.add_argument("--huge-after", type=int, default=0,
                        help="write an oversized header block and exit once "
                             "this many replies have been sent")
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
