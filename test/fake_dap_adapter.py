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

Protocol mode (stage 3): ``protocol`` speaks just enough of the Debug
Adapter Protocol to be an adapter -- it answers ``initialize`` with the
capabilities argv names and every other request with an echo -- and every
deviation from that is one measured adapter behaviour, switched on by one
option, so a test asserts against a value it chose.

Two properties hold in every configuration and are what the client's cases
read.  Every reply's body carries ``clientResponses``, the number of
RESPONSES kg has sent this adapter, which is how a case sees whether a
reverse request was answered without guessing at timing.  And every
response kg sends is reported back as a ``kgReverse`` event carrying its
``command``, ``success``, ``message``, ``request_seq`` and ``seq``, which is
how a case sees WHAT kg answered -- including that it echoed a zero
``request_seq`` verbatim and stamped a nonzero ``seq`` of its own.

Protocol options:

``--capabilities JSON``
    The body of the ``initialize`` response, which by the specification is
    the Capabilities object itself (default ``{}``).
``--capabilities-event JSON``, ``--capabilities-event-on CMD``
    Sent as a ``capabilities`` event right after the initialize response,
    or before answering CMD instead: lldb-dap adds two capabilities this
    way ~15 ms late, and an event may also turn one OFF.  Deferring it to a
    later command is what lets a case read the capability set both before
    and after the merge rather than racing one poll against the other.
``--seq-zero`` / ``--seq-string`` / ``--no-seq``
    How this adapter numbers what it sends: always zero (lldb-dap), as a
    decimal string (netcoredbg), or not at all.  None of the three changes
    ``request_seq``, which is kg's own number coming back.
``--request-seq-string``
    Send ``request_seq`` as a decimal string.
``--no-reply CMD`` (repeatable)
    Never answer CMD: the only thing that can end such a request is kg's
    own deadline, or the absence of one.
``--duplicate CMD``
    Answer CMD twice, byte for byte.
``--reorder CMD`` (repeatable)
    Hold CMD's replies and flush them in reverse order once two are held.
``--event-before CMD:EVENT``
    Emit EVENT immediately before answering CMD.
``--mismatch CMD``
    Answer CMD with a ``command`` naming something else, at CMD's own
    ``request_seq``.
``--stray-before CMD``, ``--stray-request-seq N``
    Before answering CMD, send a response whose ``request_seq`` is a number
    nobody is waiting for -- by default one that is merely unknown, and
    with N past 2**31 one that is not a protocol id at all.
``--bad-type-before CMD``
    Before answering CMD, send a message whose ``type`` is not one of the
    protocol's three.
``--error-message CMD:TEXT`` / ``--error-format CMD:TEXT``
    Refuse CMD with the failure in ``message`` (debugpy's shape) or in
    ``body.error.format`` with ``showUser``, ``url`` and ``urlLabel``
    (lldb-dap's, which uses no other).
``--reverse CMD``, ``--reverse-on TRIGGER``, ``--reverse-seq N``,
``--reverse-hold``
    Send CMD as a reverse request when TRIGGER (default ``launch``)
    arrives.  ``--reverse-seq`` forces the number stamped on it -- 0 for
    lldb-dap's shape, -1 to omit it entirely, so that kg has nothing to
    correlate an answer to -- and ``--reverse-hold`` leaves TRIGGER itself
    unanswered until kg has answered the reverse request, which is the
    reverse-request-mid-launch case.
``--pre-init NAME`` (repeatable: ``telemetry``, ``output``, ``initialized``)
    Events sent BEFORE the initialize response, which debugpy really does.
``--flood N``
    Emit N ``kgFlood`` events after the initialize response, for the
    per-poll work budget.
``--report-initialize``
    Report the ``initialize`` request's own arguments back as a
    ``kgInitializeArguments`` event, which is how a case asserts what kg
    promised -- and, more to the point, what it did not.

Session-choreography options (stage 4).  Each one is a measured ordering
that a real adapter has and the others do not, which is why the session
cannot be a linear state machine:

``--launch-order {early,between,late}``
    Where the ``launch``/``attach`` response falls [M-1].  ``early``
    answers it as it arrives (lldb-dap, which answers before the
    ``initialized`` event); ``between`` holds it until
    ``configurationDone`` arrives and answers it BEFORE that one
    (nbcode, delve); ``late`` answers it AFTER the ``configurationDone``
    response (debugpy).  A client that waited on the launch response
    before configuring deadlocks the third.
``--no-initialized``
    Never send the ``initialized`` event, so a case can assert that
    configuration is driven by the event and by nothing else.  The event is
    otherwise sent once, after the initialize response -- or before it, and
    then not again, with ``--pre-init initialized``.
``--stopped-before CMD`` (repeatable)
    A ``stopped`` event immediately before answering CMD: with
    ``configurationDone`` that is lldb-dap's stopOnEntry, whose stop lands
    while the session is nominally still configuring [M-10].
``--exited-after CMD``, ``--terminated-after CMD``
    An ``exited`` event (carrying ``--exit-code``) or a ``terminated``
    event after answering CMD.  They are separate on purpose: ``exited``
    is the debuggee's status and does not end the session, ``terminated``
    does [M-8].
``--terminated-restart JSON``
    The ``restart`` value that ``terminated`` carries, which a client
    retains for its restart policy.
``--terminated-twice``
    Send ``terminated`` twice, which delve really does: ending must be
    idempotent.
``--breakpoint-fail PATH`` (repeatable)
    Refuse ``setBreakpoints`` for that source alone, so a case can prove
    one failed source is reported without wedging the join.

Breakpoint choreography (stage 5).  ``setBreakpoints`` is answered like an
adapter rather than echoed: one element per requested breakpoint, verified,
at the line it asked for, carrying a FRESH id -- both measured adapters
re-key every id on every set, and debugpy does it even for a line that did
not move [M-9].

``--breakpoint-id-base N``
    The first id handed out (default 1).  ``0`` is debugpy's first id,
    which is why a client stores ``has_id`` beside it rather than treating
    0 as "none".
``--breakpoint-reply JSON`` (repeatable)
    The exact ``breakpoints`` array to answer the NEXT ``setBreakpoints``
    with, one option per round, in order; rounds past the last one get the
    generated answer again.  Everything a real adapter does to a set is
    spelled here rather than as an option each: a relocation
    (``[{"id":1,"verified":true,"line":3}]`` for a request that asked for
    line 999), an unverified breakpoint with no ``line`` at all, an array
    with fewer elements than the request had, an element that is not an
    object, or two rounds handing out the same id.
``--breakpoint-event JSON`` (repeatable), ``--breakpoint-event-on CMD``
    A ``breakpoint`` event body, emitted after answering CMD (default
    ``setBreakpoints``), one option per occurrence, in order.  lldb-dap
    fires ``changed`` right after every set response, which is what makes
    "updates are idempotent" and "an event never triggers a set" testable.
``--stopped-after CMD`` (repeatable), ``--stopped-body JSON`` (repeatable)
    A ``stopped`` event after answering CMD, with the given body -- with
    and without ``hitBreakpointIds``, which is the difference between
    lldb-dap and debugpy for a temporary breakpoint.
``--report-arguments CMD`` (repeatable)
    Report CMD's own ``arguments`` back as a ``kgArguments`` event before
    answering it, which is how a case asserts what kg ASKED for -- the
    exception filters it chose, the breakpoints it sent -- rather than
    inferring it from what came back.
``--die-after CMD``
    Exit with ``--exit-code`` once CMD has been answered: an adapter that
    crashed, whose session must end on end of stream alone.
``--linger-after-eof``
    Stay alive for ``--linger-seconds`` once the client has closed its
    end, which is debugpy's adapter after ``disconnect`` and what the
    grace/kill rungs of the shutdown ladder are for.

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
import json
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


def pairs(specs):
    """``CMD:TEXT`` options as a dict, splitting on the FIRST colon so the
    text may contain one."""
    out = {}
    for spec in specs:
        name, _, text = spec.partition(":")
        out[name] = text
    return out


class Protocol:
    """An adapter's half of the conversation, deviating only where argv
    says.  Nothing here is timed: every byte it writes is written from
    inside the handling of a message kg sent, so a test drives it by
    sending requests and never by waiting."""

    def __init__(self, stdout, args):
        self.stdout = stdout
        self.args = args
        self.seq = 0
        self.client_responses = 0
        self.reordered = []
        self.reverse_held = []
        self.held_launch = None
        self.breakpoint_id = args.breakpoint_id_base
        self.breakpoint_replies = list(args.breakpoint_reply)
        self.breakpoint_events = list(args.breakpoint_event)
        self.stopped_bodies = list(args.stopped_body)
        self.event_before = pairs(args.event_before)
        self.error_message = pairs(args.error_message)
        self.error_format = pairs(args.error_format)

    def next_seq(self):
        self.seq += 1
        if self.args.seq_zero:
            return 0
        if self.args.seq_string:
            return str(self.seq)
        return self.seq

    def write(self, message):
        write_all(self.stdout, frame(json.dumps(message).encode("utf-8")))

    def send(self, message):
        if not self.args.no_seq:
            message["seq"] = self.next_seq()
        self.write(message)

    def event(self, name, body=None):
        message = {"type": "event", "event": name}
        if body is not None:
            message["body"] = body
        self.send(message)

    def reply(self, request, body=None, success=True, message=None,
              command=None, request_seq=None):
        if request_seq is None:
            request_seq = request.get("seq", 0)
        if self.args.request_seq_string:
            request_seq = str(request_seq)
        out = {"type": "response", "request_seq": request_seq,
               "success": success,
               "command": command or request.get("command", "")}
        if message is not None:
            out["message"] = message
        if body is not None:
            out["body"] = body
        self.send(out)

    def pre_init_event(self, name):
        """The three shapes of pre-initialize traffic that were measured:
        telemetry to be dropped, user output to be kept, and an
        ``initialized`` that arrived before the response it must follow."""
        if name == "telemetry":
            self.event("output", {"category": "telemetry",
                                  "output": "adapter-telemetry"})
        elif name == "output":
            self.event("output", {"category": "stdout",
                                  "output": "early-output"})
        elif name == "initialized":
            self.event("initialized")

    def initialize(self, message):
        for name in self.args.pre_init:
            self.pre_init_event(name)
        if self.args.report_initialize:
            self.event("kgInitializeArguments",
                       {"arguments": message.get("arguments")})
        if "initialize" in self.error_message or "initialize" in self.error_format:
            # An adapter that refuses `initialize` is not an adapter, and
            # nothing that follows one happens: no capabilities, no
            # `initialized`.
            self.emit(message)
            return
        self.reply(message, body=json.loads(self.args.capabilities))
        if self.args.capabilities_event and not self.args.capabilities_event_on:
            self.send_capabilities_event()
        for n in range(self.args.flood):
            self.event("kgFlood", {"n": n})
        if not self.args.no_initialized and "initialized" not in self.args.pre_init:
            # Once, and only once: an adapter that already sent it early
            # (``--pre-init initialized``) does not send it again.
            self.event("initialized")

    def send_capabilities_event(self):
        self.event("capabilities", {
            "capabilities": json.loads(self.args.capabilities_event)})

    def send_reverse(self, trigger):
        """Ask kg something.  Returns whether the trigger's own answer is
        being held until kg replies."""
        message = {"type": "request", "command": self.args.reverse,
                   "arguments": {"kind": "integrated"}}
        if self.args.reverse_seq is None:
            message["seq"] = self.next_seq()
        elif self.args.reverse_seq >= 0:
            message["seq"] = self.args.reverse_seq
        self.write(message)
        if self.args.reverse_hold:
            self.reverse_held.append(trigger)
            return True
        return False

    def source_refused(self, message):
        """Whether this `setBreakpoints` names a source argv says to
        refuse.  One source of several, so that a test sees the join
        counting a failure rather than losing every breakpoint."""
        arguments = message.get("arguments") or {}
        source = arguments.get("source") or {}
        return source.get("path") in self.args.breakpoint_fail

    def breakpoints_body(self, message):
        """One element per requested breakpoint, each with an id nobody has
        had before: a set is a replacement, and an id is a fact about one
        set rather than an identity.  ``--breakpoint-reply`` replaces the
        whole array for one round, which is how a case scripts a
        relocation, an unverified breakpoint, a missing element or an id of
        its own choosing."""
        if self.breakpoint_replies:
            return {"breakpoints": json.loads(self.breakpoint_replies.pop(0))}
        arguments = message.get("arguments") or {}
        out = []
        for item in arguments.get("breakpoints") or []:
            out.append({"id": self.breakpoint_id, "verified": True,
                        "line": item.get("line")})
            self.breakpoint_id += 1
        return {"breakpoints": out}

    def emit(self, message):
        command = message.get("command", "")
        if command == "setBreakpoints" and self.source_refused(message):
            self.reply(message, success=False,
                       message="no such source")
            return
        if command in self.error_message:
            self.reply(message, success=False,
                       message=self.error_message[command])
            return
        if command in self.error_format:
            self.reply(message, success=False, body={"error": {
                "id": 42, "format": self.error_format[command],
                "showUser": True, "url": "https://example.invalid/why",
                "urlLabel": "Why this failed"}})
            return
        if command == "setBreakpoints":
            body = self.breakpoints_body(message)
        else:
            body = {"echo": message.get("arguments"),
                    "clientResponses": self.client_responses}
        wrong = "kgWrongCommand" if command in self.args.mismatch else None
        self.reply(message, body=body, command=wrong)
        if command in self.args.duplicate:
            self.reply(message, body=body, command=wrong)

    def answer(self, message):
        command = message.get("command", "")
        if command in self.args.report_arguments:
            self.event("kgArguments", {"command": command,
                                       "arguments": message.get("arguments")})
        if command in self.event_before:
            self.event(self.event_before[command], {"tag": command})
        if self.args.capabilities_event and command == self.args.capabilities_event_on:
            self.send_capabilities_event()
        if command in self.args.stray_before:
            self.write({"seq": self.next_seq(), "type": "response",
                        "request_seq": self.args.stray_request_seq,
                        "success": True, "command": "kgStray", "body": {}})
        if command in self.args.bad_type_before:
            self.write({"seq": self.next_seq(), "type": "kgNotAType",
                        "command": command})
        if self.args.reverse and command == self.args.reverse_on:
            if self.send_reverse(message):
                return
        if command in self.args.no_reply:
            return
        if command in ("launch", "attach") and self.args.launch_order != "early":
            self.held_launch = message
            return
        if command in self.args.stopped_before:
            self.event("stopped", {"reason": "breakpoint", "threadId": 1,
                                   "allThreadsStopped": True})
        if command == "configurationDone" and self.args.launch_order == "between":
            self.release_launch()
        if command in self.args.reorder:
            self.reordered.append(message)
            if len(self.reordered) >= 2:
                for held in reversed(self.reordered):
                    self.emit(held)
                self.reordered = []
            return
        self.emit(message)
        self.after(command)

    def release_launch(self):
        held = self.held_launch
        self.held_launch = None
        if held is not None:
            self.emit(held)

    def send_terminated(self):
        body = None
        if self.args.terminated_restart:
            body = {"restart": json.loads(self.args.terminated_restart)}
        self.event("terminated", body)
        if self.args.terminated_twice:
            self.event("terminated", body)

    def after(self, command):
        """What follows an answer: the debuggee's own news, and the launch
        response for the adapter that answers it last."""
        if command == "configurationDone" and self.args.launch_order == "late":
            self.release_launch()
        if command == self.args.breakpoint_event_on and self.breakpoint_events:
            self.event("breakpoint",
                       json.loads(self.breakpoint_events.pop(0)))
        if command in self.args.stopped_after:
            body = {"reason": "breakpoint", "threadId": 1}
            if self.stopped_bodies:
                body = json.loads(self.stopped_bodies.pop(0))
            self.event("stopped", body)
        if command in self.args.exited_after:
            self.event("exited", {"exitCode": self.args.exit_code})
        if command in self.args.terminated_after:
            self.send_terminated()
        if command in self.args.die_after:
            raise SystemExit(self.args.exit_code)

    def on_request(self, message):
        if message.get("command") == "initialize":
            self.initialize(message)
            return
        self.answer(message)

    def on_response(self, message):
        """kg answering a reverse request.  Reported straight back as an
        event, so a case reads what kg said rather than inferring it."""
        self.client_responses += 1
        self.event("kgReverse", {
            "command": message.get("command"),
            "success": message.get("success"),
            "message": message.get("message"),
            "request_seq": message.get("request_seq"),
            "seq": message.get("seq"),
            "count": self.client_responses})
        held = self.reverse_held
        self.reverse_held = []
        for trigger in held:
            self.emit(trigger)

    def run(self, stdin):
        while True:
            body = read_frame(stdin)
            if body is None:
                return
            message = json.loads(body.decode("utf-8"))
            if message.get("type") == "response":
                self.on_response(message)
            else:
                self.on_request(message)


def mode_protocol(stdin, stdout, args):
    Protocol(stdout, args).run(stdin)
    if args.linger_after_eof:
        note("protocol: input ended")
        time.sleep(args.linger_seconds)


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
    "protocol": mode_protocol,
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
    parser.add_argument("--capabilities", default="{}",
                        help="protocol: the initialize response body")
    parser.add_argument("--capabilities-event", default="",
                        help="protocol: a capabilities event after it")
    parser.add_argument("--capabilities-event-on", default="",
                        help="protocol: send it before this command instead")
    parser.add_argument("--seq-zero", action="store_true",
                        help="protocol: stamp seq 0 on everything")
    parser.add_argument("--seq-string", action="store_true",
                        help="protocol: stamp seq as a decimal string")
    parser.add_argument("--no-seq", action="store_true",
                        help="protocol: send no seq at all")
    parser.add_argument("--request-seq-string", action="store_true",
                        help="protocol: send request_seq as a string")
    parser.add_argument("--no-reply", action="append", default=[],
                        help="protocol: never answer this command")
    parser.add_argument("--duplicate", action="append", default=[],
                        help="protocol: answer this command twice")
    parser.add_argument("--reorder", action="append", default=[],
                        help="protocol: hold and reverse these replies")
    parser.add_argument("--event-before", action="append", default=[],
                        help="protocol: CMD:EVENT before answering CMD")
    parser.add_argument("--mismatch", action="append", default=[],
                        help="protocol: answer CMD naming another command")
    parser.add_argument("--stray-before", action="append", default=[],
                        help="protocol: a response nobody awaits, before CMD")
    parser.add_argument("--stray-request-seq", type=int, default=987654,
                        help="protocol: the request_seq that stray carries")
    parser.add_argument("--bad-type-before", action="append", default=[],
                        help="protocol: a message with no usable type")
    parser.add_argument("--error-message", action="append", default=[],
                        help="protocol: CMD:TEXT refused in `message`")
    parser.add_argument("--error-format", action="append", default=[],
                        help="protocol: CMD:TEXT refused in body.error")
    parser.add_argument("--reverse", default="",
                        help="protocol: a reverse request to send")
    parser.add_argument("--reverse-on", default="launch",
                        help="protocol: the request that triggers it")
    parser.add_argument("--reverse-seq", type=int, default=None,
                        help="protocol: its seq (-1 omits it)")
    parser.add_argument("--reverse-hold", action="store_true",
                        help="protocol: hold the trigger's own reply")
    parser.add_argument("--pre-init", action="append", default=[],
                        help="protocol: events before the initialize reply")
    parser.add_argument("--flood", type=int, default=0,
                        help="protocol: events after the initialize reply")
    parser.add_argument("--report-initialize", action="store_true",
                        help="protocol: report initialize's own arguments")
    parser.add_argument("--launch-order", default="early",
                        choices=("early", "between", "late"),
                        help="protocol: where the launch response falls")
    parser.add_argument("--no-initialized", action="store_true",
                        help="protocol: never send the initialized event")
    parser.add_argument("--stopped-before", action="append", default=[],
                        help="protocol: a stopped event before answering CMD")
    parser.add_argument("--exited-after", action="append", default=[],
                        help="protocol: an exited event after answering CMD")
    parser.add_argument("--terminated-after", action="append", default=[],
                        help="protocol: a terminated event after answering CMD")
    parser.add_argument("--terminated-restart", default="",
                        help="protocol: the restart value terminated carries")
    parser.add_argument("--terminated-twice", action="store_true",
                        help="protocol: send terminated twice, as delve does")
    parser.add_argument("--breakpoint-fail", action="append", default=[],
                        help="protocol: refuse setBreakpoints for this source")
    parser.add_argument("--breakpoint-id-base", type=int, default=1,
                        help="protocol: the first breakpoint id handed out")
    parser.add_argument("--breakpoint-reply", action="append", default=[],
                        help="protocol: one round's `breakpoints` array")
    parser.add_argument("--breakpoint-event", action="append", default=[],
                        help="protocol: a breakpoint event body to emit")
    parser.add_argument("--breakpoint-event-on", default="setBreakpoints",
                        help="protocol: the command those events follow")
    parser.add_argument("--stopped-after", action="append", default=[],
                        help="protocol: a stopped event after answering CMD")
    parser.add_argument("--stopped-body", action="append", default=[],
                        help="protocol: that stopped event's body")
    parser.add_argument("--die-after", action="append", default=[],
                        help="protocol: exit once CMD has been answered")
    parser.add_argument("--report-arguments", action="append", default=[],
                        help="protocol: report CMD's arguments back")
    parser.add_argument("--linger-after-eof", action="store_true",
                        help="protocol: outlive the client's half-close")
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
