#!/usr/bin/env python3

import argparse
import difflib
import functools
import io
import json
import multiprocessing
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import pexpect
import yaml


DEFAULT_TRAILER = ["C-x", "C-s", "C-x", "C-c"]
DEFAULT_DIMENSIONS = (24, 80)
DEFAULT_TIMEOUT = 5.0
# kg's own runs no longer pay this: they wait for the first painted frame
# (see wait_ready_pexpect) and charge each runner what it actually costs,
# from ~3 ms on a plain build to ~0.33 s under valgrind.  It is still the
# sleep the Emacs oracle takes, so it stays generous enough for emacs -nw.
DEFAULT_STARTUP_DELAY = 0.5
# Keys are buffered by the pty, so this is not "time for kg to keep up":
# kg needs well under a millisecond per key on a plain build, and
# multi-character tokens are already sent with no delay between bytes.  The
# binding constraint is semantic.  kg treats keys arriving less than 30 ms
# apart as a paste (editor.paste_mode in src/kbd.c), which suppresses
# auto-indent and autocompletion, so anything at or below 0.03 silently
# changes what the editor does.  kg also gives an escape sequence 100 ms to
# arrive, which is why the handful of cases that need a bare ESC to stay
# separate from the next key override this upward.
DEFAULT_KEY_DELAY = 0.05
# Draining gap between the individual bytes of one multi-byte token (a
# whole word like "version" typed as one YAML list entry) and between
# whole tokens while more are still queued.  Linux's pty output queue is
# generous enough that kg's per-keystroke full-screen redraw never backs
# up, so a burst of sends with nothing read in between still arrives
# intact; FreeBSD's is smaller, and with nothing draining it kg's write()
# blocks until this process finally reads (at end of test, via the
# trailer's EOF wait), which stalls kg's read loop for the run's whole
# duration and both times out cases whose replies alone exceed the queue
# and, once kg's write unblocks, delivers every queued key in one
# scheduler-timescale burst -- indistinguishable from a paste, so
# auto-indent and autocompletion silently turn off mid-case.  A
# non-blocking read after every byte drains that backlog continuously
# instead; SEND_DRAIN_TIMEOUT beyond zero additionally paces the send
# loop by a small, real gap, which a plain drain (timeout=0) does not,
# and which FreeBSD needed in order to hand kg a turn between two
# back-to-back byte writes with no other delay between them.  Its cost on
# Linux was measured at 5.9 s of summed per-case time over the whole
# suite (600.7 s across 443 cases against 594.8 s without it), which at
# PTY_JOBS=8 is under a second of wall.  The tmux backend deliberately
# has no counterpart: the tmux server owns the pane's pty and reads it
# continuously, so kg's output queue cannot fill there in the first
# place -- verified by the FreeBSD suite passing its tmux cases with no
# drain at all.
SEND_DRAIN_TIMEOUT = 0.005
DEFAULT_JOBS = 8
# rustup installs a proxy for rust-analyzer even when the component itself is
# absent.  A PATH lookup therefore says the tool exists while kg only gets an
# immediate rustup error.  Probe that one wrapper before admitting its cases;
# the other server names are ordinary executables and keep the cheap PATH
# check.
TOOL_PROBE_ARGS = {"rust-analyzer": ("--version",)}
TOOL_PROBE_TIMEOUT = 2.0
# The interpreter `requires_python_module:` asks.  It is a literal rather
# than $PYTHON because the predicate answers one question only -- can the
# interpreter kg itself spawns import this module -- and kg's Python debug
# adapter is spelled `python3 -m debugpy.adapter` in src/dap_config.c.
PYTHON_MODULE_INTERPRETER = "python3"
# kg's mode line: "----  name  All (1,0)  (Fundamental)" ("-**-" when dirty).
KG_READY_PATTERN = r"(?:----|-\*\*-)  "
KG_READY = re.compile(KG_READY_PATTERN)
KG_READY_BYTES = re.compile(KG_READY_PATTERN.encode())
READY_POLL = 0.005
# Only reached when the mode line never shows up, so it can be generous:
# polling normally returns in a few milliseconds on a plain build and about
# 0.35 s under valgrind.
READY_DEADLINE = 2.0
# A frame can be painted from inside init-file evaluation, so the mode line
# alone does not mean kg has reached its input loop.  Wait for output to
# stop as well.
READY_SETTLE = 0.05
# Last resort only: the developer box this suite grew up on keeps its Emacs
# outside PATH.  Anything with an `emacs` on PATH (a CI image, a distro
# install) is served by the search in resolve_emacs() long before this.
EMACS_FALLBACK = "/opt-3/emacs-31-lucid/bin/emacs"
# This checkout, for the `{REPO}` expansion in a case's `env:` values.  A
# case runs in a fresh temporary directory, so a path into the repository is
# otherwise unnameable from one.
ROOT = Path(__file__).resolve().parent.parent


@dataclass
class Case:
	name: str
	path: Path
	filename: str
	editor_args: list[str]
	initial: str
	file_mode: int | None
	keys: list[str]
	requires_feature: tuple[str, ...]
	requires_tool: tuple[str, ...]
	requires_python_module: str | None
	config_files: dict[str, str]
	workspace_files: dict[str, str]
	env: dict[str, str]
	expected_saved: str | None
	expected_saved_any: list[str] | None
	oracle: str | None
	expected_exit_code: int | None
	xfail: bool
	trailer_keys: list[str]
	backend: str
	oracle_backend: str | None
	startup_delay: float
	key_delay: float
	dimensions: tuple[int, int]
	expected_screen_contains: list[str] | None
	expected_screen_not_contains: list[str] | None


@dataclass
class RunResult:
	saved: bytes | None
	exit_code: int | None
	error: str | None
	transcript: bytes


def resolve_emacs(explicit: str | None) -> str | None:
	"""Find the Emacs the oracle cases run against, or None.

	An explicit --emacs is taken at its word (a wrong path should say so
	rather than fall back to something that happens to work); after that
	$KG_PTY_EMACS, then PATH, then the developer-box pin.
	"""
	if explicit:
		return explicit if os.access(explicit, os.X_OK) else None
	env = os.environ.get("KG_PTY_EMACS")
	if env:
		return env if os.access(env, os.X_OK) else None
	found = shutil.which("emacs")
	if found:
		return found
	if os.access(EMACS_FALLBACK, os.X_OK):
		return EMACS_FALLBACK
	return None


def python_module_unavailable(module: str) -> str | None:
	"""Return a skip reason when `python3` cannot import `module`.

	The interpreter is spelled `python3` on purpose, and is not $PYTHON:
	the only thing this predicate exists for is an adapter or server kg
	spawns as `python3 -m something` (debugpy is the one), and a probe run
	by a different interpreter would answer a question nobody asked.  The
	harness's own interpreter may well be another one.
	"""
	try:
		probe = subprocess.run(
			[PYTHON_MODULE_INTERPRETER, "-c", f"import {module}"],
			stdin=subprocess.DEVNULL,
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
			timeout=TOOL_PROBE_TIMEOUT,
			check=False,
		)
	except (OSError, subprocess.TimeoutExpired):
		return f"{PYTHON_MODULE_INTERPRETER} cannot import {module}"
	if probe.returncode != 0:
		return f"{PYTHON_MODULE_INTERPRETER} cannot import {module}"
	return None


def case_missing_tool(case: "Case") -> str | None:
	"""The executable this case needs and this box has not got, or None.

	`requires_tool:` is the general form of the tmux and Emacs rules below:
	one bare name, or a LIST of them for a case that needs more than one --
	kg's Go cases need both `dlv` and `go`, since `mode:"debug"` has delve
	build the package before it debugs it, and a box with only one of the
	two skips naming the one it lacks.
	a case that drives a real language server names the binary, and a box
	without a usable one skips with a reason rather than failing.  The normal
	case is still a plain PATH lookup -- the case says `clangd`, kg spawns
	`clangd`, and anything cleverer would let the two disagree about which one
	ran.  A small probe is reserved for wrappers such as rustup's
	`rust-analyzer` proxy, which can be present on PATH while its component is
	missing.
	"""
	if case.requires_python_module is not None:
		# A module is not an executable and cannot be spelled as one:
		# kg's Python adapter is `python3 -m debugpy.adapter`, so there
		# is nothing on PATH called debugpy for `requires_tool:` to
		# find.  Asked first, since it is the cheaper failure to
		# explain.
		missing = python_module_unavailable(case.requires_python_module)
		if missing:
			return missing
	for tool in case.requires_tool:
		missing = tool_unavailable(tool)
		if missing:
			return missing
	return None


def tool_unavailable(tool: str) -> str | None:
	"""Return a skip reason when a required executable cannot be used."""
	if shutil.which(tool) is None:
		return f"{tool} not found"
	probe_args = TOOL_PROBE_ARGS.get(tool)
	if probe_args is None:
		return None
	try:
		probe = subprocess.run(
			[tool, *probe_args],
			stdin=subprocess.DEVNULL,
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
			timeout=TOOL_PROBE_TIMEOUT,
			check=False,
		)
	except (OSError, subprocess.TimeoutExpired):
		return f"{tool} not runnable"
	if probe.returncode != 0:
		return f"{tool} not runnable"
	return None


def feature_mismatch(case: "Case", features: set[str]) -> bool:
	"""Whether this build is the wrong one for this case.

	`requires_feature: dap` needs the feature; `requires_feature: -dap`
	needs its ABSENCE, which is the only way to write a case about what a
	build without an optional subsystem does.  Such a case runs in the
	lane that makes that build (.ci/ci-16 for DAP) and skips in every
	other, which is the same bargain the positive form already makes.

	A LIST needs all of them, which is not a convenience: the Java
	debugger's adapter is a socket the LANGUAGE server announces, so its
	case is about a build that has both subsystems and must skip rather
	than fail in the lane that drops either one.
	"""
	for want in case.requires_feature:
		if want.startswith("-"):
			if want[1:] in features:
				return True
		elif want not in features:
			return True
	return False


def case_needs_tmux(case: "Case") -> bool:
	if case.backend == "tmux":
		return True
	return case.oracle == "emacs" and (case.oracle_backend or case.backend) == "tmux"


def ctrl_byte(ch: str) -> bytes:
	if len(ch) != 1:
		raise ValueError(f"invalid control key payload: {ch!r}")
	code = ord(ch.upper())
	if code == ord("?"):
		return b"\x7f"
	if code == ord("@"):
		return b"\x00"
	if not 0x40 <= code <= 0x5f and not 0x61 <= ord(ch) <= 0x7a:
		raise ValueError(f"unsupported control key: C-{ch}")
	return bytes([ord(ch.upper()) & 0x1f])


# Exact terminal bytes for named keys.  Both PTY backends consume this one
# table: pexpect writes the bytes, while tmux receives each byte through
# `send-keys -H`.  Keeping aliases here too prevents one backend from gaining
# a spelling whose other backend silently types the token's literal letters.
NAMED_KEY_BYTES = {
	"ESC": b"\x1b",
	"RET": b"\r",
	"ENTER": b"\r",
	"M-RET": b"\x1b\r",
	"M-ENTER": b"\x1b\r",
	"TAB": b"\t",
	"M-TAB": b"\x1b\t",
	"C-M-I": b"\x1b\t",
	"M-DEL": b"\x1b\x7f",
	"M-BACKSPACE": b"\x1b\x7f",
	"SPC": b" ",
	"SPACE": b" ",
	"M-SPC": b"\x1b ",
	"M-SPACE": b"\x1b ",
	"C-SPC": b"\x00",
	"C-SPACE": b"\x00",
	"C-@": b"\x00",
	"INSERT": b"\x1b[2~",
	"INS": b"\x1b[2~",
	"HOME": b"\x1b[1~",
	"END": b"\x1b[4~",
	"UP": b"\x1b[A",
	"DOWN": b"\x1b[B",
	"PAGEUP": b"\x1b[5~",
	"PAGEDOWN": b"\x1b[6~",
	"C-HOME": b"\x1b[1;5H",
	"C-END": b"\x1b[1;5F",
	"S-HOME": b"\x1b[1;2H",
	"S-END": b"\x1b[1;2F",
	"M-UP": b"\x1b[1;3A",
	"M-DOWN": b"\x1b[1;3B",
	"F1": b"\x1bOP",
	"F2": b"\x1bOQ",
	"F3": b"\x1bOR",
	"F4": b"\x1bOS",
	"F5": b"\x1b[15~",
	"F6": b"\x1b[17~",
	"F7": b"\x1b[18~",
	"F8": b"\x1b[19~",
	"F9": b"\x1b[20~",
	"F10": b"\x1b[21~",
	"F11": b"\x1b[23~",
	"F12": b"\x1b[24~",
	"C-F5": b"\x1b[15;5~",
	"C-F9": b"\x1b[20;5~",
	"M-F10": b"\x1b[21;3~",
	"M-F11": b"\x1b[23;3~",
}


# What a modifier token's payload looks like when it names a key rather
# than typing one: two or more characters, all letters and digits.
NAMED_KEY_SHAPE = re.compile(r"^[A-Za-z][A-Za-z0-9]+$")


def token_to_bytes(token: str) -> bytes:
	if not isinstance(token, str) or not token:
		raise ValueError(f"invalid key token: {token!r}")

	upper = token.upper()

	# One raw byte, named in hex.  Every other token is UTF-8 encoded on
	# the way out, so this is the only way to send a byte that is not
	# valid UTF-8 -- a malformed lead, a stray continuation byte, a C1
	# control -- which is exactly what the input decoder has to survive.
	if upper.startswith("BYTE="):
		digits = token[len("BYTE="):]
		if len(digits) != 2 or any(d not in "0123456789abcdefABCDEF" for d in digits):
			raise ValueError(f"BYTE= takes exactly two hex digits: {token!r}")
		return bytes([int(digits, 16)])

	if upper in NAMED_KEY_BYTES:
		return NAMED_KEY_BYTES[upper]

	# A modifier spelling whose payload NAMES a key rather than typing one
	# -- M-F12, S-Up, C-PageDown -- has no byte sequence unless the table
	# above holds it, and the fallthrough below would type the token's own
	# letters: ESC then "F12".  A case that asserts on a key it never sent
	# is worse than one that fails, so this is refused; the fix is the
	# key's exact bytes in NAMED_KEY_BYTES.
	#
	# The rule is deliberately "looks like a name": a multi-character
	# payload of letters and digits.  M-[<0;14;2M is not one, and must
	# keep working, because an SGR mouse report is parametrised by button
	# and cell and so cannot be a fixed table entry.
	if len(token) >= 3 and token[1] == "-":
		prefix = token[0].upper()
		payload = token[2:]
		if prefix in ("C", "M", "S") and NAMED_KEY_SHAPE.match(payload):
			raise ValueError(
				f"unknown named key token: {token!r} "
				f"(add it to NAMED_KEY_BYTES with its exact bytes)"
			)
		if prefix == "C":
			return ctrl_byte(payload)
		if prefix == "M":
			return b"\x1b" + payload.encode("utf-8")

	return token.encode("utf-8")


def drain_pexpect(child: pexpect.spawn) -> None:
	"""Read whatever kg has already written, so its next write() does not
	block on a full pty output queue -- see SEND_DRAIN_TIMEOUT."""
	try:
		child.read_nonblocking(size=1 << 20, timeout=SEND_DRAIN_TIMEOUT)
	except Exception:
		pass


def settle_token(token: str) -> tuple[float, str] | None:
	"""`SETTLE=<seconds>` or `SETTLE=<seconds>:<text>`: not a key, but one
	bounded wait.

	A key_delay is paid by every key of a case, so buying the time a real
	debug adapter takes to reach its first stop that way costs the case
	its own length times its number of keys -- and the measured figure is
	6.8 s to the first stop for a real Python program.  This spends it
	once, where it is owed: after the key that started the session.

	The budget is a MAXIMUM, never a sleep.  Without `<text>` the wait
	ends when the editor has painted something and gone quiet again; with
	it, when the pane SAYS the thing the case is waiting for -- a mode
	line reading the line the program stopped on -- which is what keeps a
	case that also has a slow step in it from paying its whole budget
	twice.  An expired wait is not itself a failure: what fails is the
	assertion the case then makes, and it says what was on screen.
	"""
	if not token.startswith("SETTLE="):
		return None
	budget, _, text = token.split("=", 1)[1].partition(":")
	return (float(budget), text)


def wait_change_pexpect(child: pexpect.spawn, budget: float) -> None:
	"""Wait for kg to write something and then stop, within `budget`."""
	deadline = time.monotonic() + budget
	painted = False
	while time.monotonic() < deadline:
		try:
			data = child.read_nonblocking(size=1 << 20,
						      timeout=READY_SETTLE)
		except Exception:
			data = b""
		if data:
			painted = True
		elif painted:
			return
		time.sleep(READY_POLL)


def wait_change_tmux(sock: str, pane: str, budget: float, text: str = "") -> None:
	"""tmux counterpart: wait for the pane to change -- or to say `text` --
	and then settle."""
	deadline = time.monotonic() + budget
	start = run_tmux_cmd(sock, "capture-pane", "-t", pane, "-p",
			     check=False).stdout
	prev = start
	quiet_since = time.monotonic()
	while time.monotonic() < deadline:
		now = run_tmux_cmd(sock, "capture-pane", "-t", pane, "-p",
				   check=False).stdout
		moment = time.monotonic()
		if now != prev:
			prev = now
			quiet_since = moment
		happened = (text in now) if text else (now != start)
		if happened and moment - quiet_since >= READY_SETTLE:
			return
		time.sleep(READY_POLL)


def send_token_pexpect(child: pexpect.spawn, token: str) -> None:
	if token.startswith("RESIZE="):
		r, c = map(int, token.split("=")[1].split(","))
		child.setwinsize(r, c)
		return

	payload = token_to_bytes(token)
	# Escape-prefixed and named sequences are one terminal key.  Keep their
	# bytes in one write so a loaded runner cannot stretch ESC past kg's
	# 100 ms completion window.  Ordinary multi-character YAML tokens keep
	# the per-byte drain that prevents PTY output backpressure on FreeBSD.
	if payload.startswith(b"\x1b") or token.upper() in NAMED_KEY_BYTES:
		child.send(payload)
		drain_pexpect(child)
		return
	for b in payload:
		child.send(bytes([b]))
		drain_pexpect(child)


def tmux_token_hex(token: str) -> tuple[str, ...]:
	# Preserve the documented backend restriction even though -H could send
	# arbitrary bytes: BYTE= fixtures intentionally exercise pexpect's raw
	# path and tmux remains a UTF-8 terminal backend for ordinary tokens.
	if token.upper().startswith("BYTE="):
		raise ValueError(f"{token!r}: BYTE= needs backend: pexpect")
	return tuple(f"{byte:02x}" for byte in token_to_bytes(token))


def send_token_tmux(sock: str, pane: str, token: str) -> None:
	run_tmux_cmd(sock, "send-keys", "-t", pane, "-H", *tmux_token_hex(token))


def decode_text(data: bytes) -> str:
	return data.decode("utf-8", "replace")


def diff_text(expected: bytes, actual: bytes, expected_name: str, actual_name: str) -> str:
	return "".join(difflib.unified_diff(
		decode_text(expected).splitlines(True),
		decode_text(actual).splitlines(True),
		fromfile=expected_name,
		tofile=actual_name,
	))


def relative_file_map(data: dict, path: Path, key: str) -> dict[str, str]:
	"""One of the `path: contents` maps a case can plant files with.

	The paths are relative and may not climb: a case writes inside its own
	throwaway directory or not at all.  `{REPO}` in a value expands here,
	as it does in `env:`; `{CWD}` cannot, because the directory only exists
	once the run starts (see write_workspace_files).
	"""
	value = data.get(key, {})
	if (not isinstance(value, dict) or
	    not all(isinstance(k, str) and isinstance(v, str) and k and
	            not k.startswith("/") and ".." not in k
	            for k, v in value.items())):
		raise ValueError(
			f"{path}: {key} must map relative paths to contents")
	return {k: v.replace("{REPO}", str(ROOT)) for k, v in value.items()}


def parse_file_mode(data: dict, path: Path) -> int | None:
	"""`file_mode:` -- the permission bits the file under test is created
	with, written the way chmod(1) takes them ("0444").

	It is the one property of the fixture that its *contents* cannot
	express, and a case that needs it needs it before kg starts: a buffer
	visiting a write-protected file comes up read-only, and there is no
	key kg could be sent to arrange that from inside.  A string, not an
	int, because YAML reads 0444 as decimal 444.
	"""
	value = data.get("file_mode")
	if value is None:
		return None
	if not isinstance(value, str):
		raise ValueError(f"{path}: file_mode must be an octal string, e.g. '0444'")
	try:
		return int(value, 8)
	except ValueError:
		raise ValueError(f"{path}: file_mode is not octal: {value!r}") from None


def load_case(path: Path) -> Case:
	data = yaml.safe_load(path.read_text())

	if not isinstance(data, dict):
		raise ValueError(f"{path}: YAML root must be a mapping")
	if "filename" not in data or "initial" not in data or "keys" not in data:
		raise ValueError(f"{path}: required keys are filename, initial, keys")
	editor_args = data.get("args", [])
	if not isinstance(editor_args, list) or not all(isinstance(v, str) for v in editor_args):
		raise ValueError(f"{path}: args must be a list of strings")
	# The word `kg -V` prints, and in `kg -V`'s own vocabulary: `dap` is
	# "this build has it", `-dap` is "this build has NOT got it".  The
	# second is what an absence case needs -- the one asserting that a
	# WITH_DAP=0 editor still answers every dap-* command with a sentence
	# -- and it can only run in a build the ordinary lanes do not make, so
	# it skips everywhere else exactly as a feature case does.
	requires_feature = data.get("requires_feature")
	if requires_feature is None:
		requires_feature = ()
	elif isinstance(requires_feature, str):
		requires_feature = (requires_feature,)
	elif isinstance(requires_feature, list):
		requires_feature = tuple(requires_feature)
	else:
		raise ValueError(
			f"{path}: requires_feature must be a string or a list of them"
		)
	if not all(
		isinstance(want, str) and want.lstrip("-") for want in requires_feature
	):
		raise ValueError(f"{path}: requires_feature must be non-empty strings")
	requires_tool = data.get("requires_tool")
	if requires_tool is None:
		requires_tool = ()
	elif isinstance(requires_tool, str):
		requires_tool = (requires_tool,)
	elif isinstance(requires_tool, list):
		requires_tool = tuple(requires_tool)
	else:
		raise ValueError(
			f"{path}: requires_tool must be a name or a list of them")
	if not all(
		isinstance(tool, str) and tool and "/" not in tool
		for tool in requires_tool
	):
		raise ValueError(
			f"{path}: requires_tool must be bare executable names")
	# A module the interpreter kg spawns must be able to import.  Distinct
	# from `requires_tool:` because it is not an executable: kg's Python
	# debug adapter is `python3 -m debugpy.adapter`, and no file called
	# debugpy is on PATH to look for.
	requires_python_module = data.get("requires_python_module")
	if requires_python_module is not None and (
		not isinstance(requires_python_module, str) or
		not requires_python_module or
		not all(part.isidentifier()
			for part in requires_python_module.split("."))
	):
		raise ValueError(
			f"{path}: requires_python_module must be a module name")
	# `config_files:` plants files under the case's throwaway HOME (an
	# init.el, a package); `workspace_files:` plants them beside the file
	# under test, which is the case's working directory, for the fixtures a
	# tool discovers by walking the tree -- a compile_commands.json, a
	# pyproject.toml, the second file of a two-file project.
	config_files = relative_file_map(data, path, "config_files")
	workspace_files = relative_file_map(data, path, "workspace_files")
	file_mode = parse_file_mode(data, path)
	# `env:` maps variable names to values merged into kg's environment for
	# this case only.  It exists for the run-time hooks kg reads from the
	# environment rather than from a file -- KG_LSP_SERVER_C, which is how
	# a case injects test/fake_lsp_server.py in place of clangd.
	#
	# `{REPO}` in a value expands to this checkout's root, because a case
	# runs with its working directory in a fresh temporary directory and
	# has no other way to name a file in the repository.  `{CWD}` expands
	# to that directory once the run starts (see expand_case_env), as it
	# does in `workspace_files:` and for the same reason: a debug
	# adapter's canned answers name the source file they stopped in, in
	# absolute terms, and the directory is a fresh mkdtemp nobody could
	# have written into the YAML.  Those two, and nothing else: more would
	# be a template language nobody asked for.
	#
	# The Emacs oracle deliberately does not get it.  A case that sets
	# KG_LSP_SERVER_C is a case about kg's own client, and handing Emacs a
	# variable it has never heard of would only make the oracle's run
	# differ from a plain one in a way the case did not intend.
	env = data.get("env", {})
	if (not isinstance(env, dict) or
	    not all(isinstance(k, str) and isinstance(v, str) and k and
	            "=" not in k for k, v in env.items())):
		raise ValueError(
			f"{path}: env must map variable names to string values")
	env = {k: v.replace("{REPO}", str(ROOT)) for k, v in env.items()}
	modes = sum(1 for key in ("expected_saved", "expected_saved_any", "oracle") if key in data)
	if modes != 1:
		raise ValueError(f"{path}: specify exactly one of expected_saved, expected_saved_any, or oracle")
	if not isinstance(data["keys"], list) or not all(isinstance(k, str) for k in data["keys"]):
		raise ValueError(f"{path}: keys must be a list of strings")
	if "expected_saved_any" in data:
		if (not isinstance(data["expected_saved_any"], list) or
		    not data["expected_saved_any"] or
		    not all(isinstance(v, str) for v in data["expected_saved_any"])):
			raise ValueError(f"{path}: expected_saved_any must be a non-empty list of strings")
	expected_exit_code = data.get("expected_exit_code")
	if expected_exit_code is not None and not isinstance(expected_exit_code, int):
		raise ValueError(f"{path}: expected_exit_code must be an int")
	backend = data.get("backend", "pexpect")
	if backend not in ("pexpect", "tmux"):
		raise ValueError(f"{path}: backend must be pexpect or tmux")
	if expected_exit_code is not None and backend == "tmux":
		raise ValueError(f"{path}: expected_exit_code is not supported with backend: tmux "
				 "(the tmux runner hardcodes exit code 0 since the process runs detached)")
	# `SETTLE=<seconds>:<text>` reads the screen, and only the tmux backend
	# has one to read: a pexpect transcript is raw output with escape
	# sequences in it, where "contains" would answer a different question.
	# Refused at load time rather than silently degraded.
	if backend != "tmux":
		for token in [*data["keys"], *data.get("trailer_keys", [])]:
			settle = settle_token(token)
			if settle is not None and settle[1]:
				raise ValueError(
					f"{path}: {token} needs backend: tmux "
					"(the text form reads the screen)")
	oracle_backend = data.get("oracle_backend")
	if oracle_backend is not None and oracle_backend not in ("pexpect", "tmux"):
		raise ValueError(f"{path}: oracle_backend must be pexpect or tmux")
	dimensions = tuple(data.get("dimensions", DEFAULT_DIMENSIONS))
	if (len(dimensions) != 2 or
	    not all(isinstance(v, int) and v > 0 for v in dimensions)):
		raise ValueError(f"{path}: dimensions must be [rows, cols] with positive integers")
	screen_contains = data.get("expected_screen_contains")
	screen_not_contains = data.get("expected_screen_not_contains")
	if screen_contains is not None and (
		not isinstance(screen_contains, list) or
		not all(isinstance(v, str) for v in screen_contains)
	):
		raise ValueError(f"{path}: expected_screen_contains must be a list of strings")
	if screen_not_contains is not None and (
		not isinstance(screen_not_contains, list) or
		not all(isinstance(v, str) for v in screen_not_contains)
	):
		raise ValueError(f"{path}: expected_screen_not_contains must be a list of strings")

	return Case(
		name=data.get("name", path.stem),
		path=path,
		filename=data["filename"],
		editor_args=editor_args,
		initial=data["initial"],
		file_mode=file_mode,
		keys=data["keys"],
		requires_feature=requires_feature,
		requires_tool=requires_tool,
		requires_python_module=requires_python_module,
		config_files=config_files,
		workspace_files=workspace_files,
		env=env,
		expected_saved=data.get("expected_saved"),
		expected_saved_any=data.get("expected_saved_any"),
		oracle=data.get("oracle"),
		expected_exit_code=expected_exit_code,
		xfail=bool(data.get("xfail", False)),
		trailer_keys=data.get("trailer_keys", DEFAULT_TRAILER),
		backend=backend,
		oracle_backend=oracle_backend,
		startup_delay=float(data.get("startup_delay", DEFAULT_STARTUP_DELAY)),
		key_delay=float(data.get("key_delay", DEFAULT_KEY_DELAY)),
		dimensions=(dimensions[0], dimensions[1]),
		expected_screen_contains=screen_contains,
		expected_screen_not_contains=screen_not_contains,
	)


def write_file_under_test(path: Path, initial: str, file_mode: int | None) -> None:
	"""Create the file the case runs on, and give it its mode.

	Written before the chmod for the obvious reason: a 0444 file cannot be
	written into afterwards, not even by the process that made it.  The
	temporary directory it sits in stays writable, so the run's own
	cleanup can still unlink it.
	"""
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_text(initial)
	if file_mode is not None:
		os.chmod(path, file_mode)


def write_config_files(home: Path, config_files: dict[str, str]) -> None:
	for relpath, content in config_files.items():
		target = home / relpath
		target.parent.mkdir(parents=True, exist_ok=True)
		target.write_text(content)


def expand_case_env(case_env: dict[str, str], cwd: Path) -> dict[str, str]:
	"""`{CWD}` in an `env:` value, once the directory exists.

	`{REPO}` was expanded at load time; this one cannot be, for the same
	reason `workspace_files:` expands its own at write time.
	"""
	return {k: v.replace("{CWD}", str(cwd)) for k, v in case_env.items()}


def write_workspace_files(cwd: Path, workspace_files: dict[str, str]) -> None:
	"""Plant `workspace_files:` beside the file under test.

	`{CWD}` in a value expands to that directory.  It exists because the
	fixtures this stages are the ones that have to name themselves in
	absolute terms -- a compile_commands.json entry's "directory" is the
	one field clangd will not accept relative -- and the directory is a
	fresh mkdtemp nobody could have written into the YAML.  Same spirit as
	`{REPO}` in `env:`, and the same limit: one substitution, not a
	template language.
	"""
	for relpath, content in workspace_files.items():
		target = cwd / relpath
		target.parent.mkdir(parents=True, exist_ok=True)
		target.write_text(content.replace("{CWD}", str(cwd)))


def wait_ready_pexpect(child: pexpect.spawn, ready: bool, budget: float) -> None:
	"""Wait for the editor's first painted frame, or sleep the budget.

	`budget` is a deadline rather than a cost: a plain build is ready in a
	few milliseconds where the same binary under valgrind needs ~0.3 s, and
	polling charges each runner only what it actually takes.  When the
	pattern never shows up (unknown editor, immediate exit) this degrades
	to the fixed sleep it replaced.
	"""
	if not ready:
		time.sleep(budget)
		return
	budget = max(budget, READY_DEADLINE)
	deadline = time.monotonic() + budget
	try:
		child.expect(KG_READY_BYTES, timeout=budget)
		last = time.monotonic()
		while time.monotonic() < deadline:
			try:
				if child.read_nonblocking(4096, READY_POLL):
					last = time.monotonic()
			except pexpect.TIMEOUT:
				pass
			if time.monotonic() - last >= READY_SETTLE:
				return
	except (pexpect.TIMEOUT, pexpect.EOF):
		pass


def run_editor_pexpect(argv: list[str], filename: str, initial: str, keys: list[str],
		       trailer_keys: list[str], startup_delay: float,
		       key_delay: float, dimensions: tuple[int, int],
		       timeout: float, config_files: dict[str, str],
		       ready: bool, case_env: dict[str, str],
		       workspace_files: dict[str, str] | None = None,
		       file_mode: int | None = None) -> RunResult:
	with tempfile.TemporaryDirectory(prefix="kg-pty-") as td:
		file_path = Path(td) / filename
		write_file_under_test(file_path, initial, file_mode)
		write_config_files(Path(td), config_files)
		write_workspace_files(Path(td), workspace_files or {})

		env = os.environ.copy()
		env["HOME"] = td
		env.pop("XDG_CONFIG_HOME", None)
		env["TERM"] = env.get("TERM", "xterm-256color")
		env.setdefault("LC_ALL", "C.UTF-8")
		env.update(expand_case_env(case_env, Path(td)))

		log = io.BytesIO()
		child = pexpect.spawn(
			argv[0],
			argv[1:] + [str(file_path)],
			cwd=td,
			env=env,
			encoding=None,
			echo=False,
			timeout=timeout,
			dimensions=dimensions,
		)
		child.delaybeforesend = 0
		child.logfile_read = log

		try:
			wait_ready_pexpect(child, ready, startup_delay)
			for token in [*keys, *trailer_keys]:
				settle = settle_token(token)
				if settle is not None:
					wait_change_pexpect(child, settle[0])
					continue
				send_token_pexpect(child, token)
				time.sleep(key_delay)
				drain_pexpect(child)
			child.expect(pexpect.EOF, timeout=timeout)
			child.close()
		except Exception as exc:
			child.close(force=True)
			return RunResult(None, None, str(exc), log.getvalue())

		exit_code = child.exitstatus
		if exit_code is None and child.signalstatus is not None:
			exit_code = 128 + child.signalstatus

		return RunResult(file_path.read_bytes(), exit_code, None, log.getvalue())


def run_tmux_cmd(sock: str, *args: str, check: bool = True) -> subprocess.CompletedProcess:
	return subprocess.run(["tmux", "-S", sock, *args], check=check, capture_output=True, text=True)


def wait_ready_tmux(sock: str, pane: str, ready: bool, budget: float) -> None:
	"""tmux counterpart of wait_ready_pexpect; see that docstring."""
	if not ready:
		time.sleep(budget)
		return
	budget = max(budget, READY_DEADLINE)
	deadline = time.monotonic() + budget
	prev = None
	last = time.monotonic()
	while time.monotonic() < deadline:
		cp = run_tmux_cmd(sock, "capture-pane", "-t", pane, "-p", check=False)
		now = time.monotonic()
		if cp.stdout != prev:
			prev = cp.stdout
			last = now
		elif KG_READY.search(cp.stdout) and now - last >= READY_SETTLE:
			return
		time.sleep(READY_POLL)


def settle_tmux(sock: str, pane: str, budget: float, floor: float = 0.0) -> None:
	"""Let the pane stop changing before a screen assertion reads it.

	Keys are queued in the pty, so at a small key_delay the capture can
	otherwise race ahead of a slow runner's redraw.

	`floor` is the answer to the failure mode "unchanged" cannot tell
	apart from "not painted yet".  A kg that is still running a
	compilation, a shell command or an arena-filling hook paints nothing
	while it does so, so it looks exactly like a finished one and the
	capture lands early -- which is how a case whose assertion is on a
	message painted *after* the last key fails only under lane
	contention.  A floor makes the quiet window mean something: nothing
	settles before `floor` seconds have passed since the last key,
	whatever the pane looks like.  It is paid once per case rather than
	once per key, which is what makes it cheaper than the key_delay every
	such case would otherwise have to carry.
	"""
	start = time.monotonic()
	deadline = start + max(budget, floor)
	prev = None
	quiet_since = start
	while time.monotonic() < deadline:
		cp = run_tmux_cmd(sock, "capture-pane", "-t", pane, "-p", check=False)
		now = time.monotonic()
		if cp.stdout != prev:
			prev = cp.stdout
			quiet_since = now
		elif now - quiet_since >= READY_SETTLE and now - start >= floor:
			return
		time.sleep(READY_POLL)


def wait_exit_tmux(sock: str, session: str, budget: float) -> None:
	"""Wait for the trailer's C-x C-c to actually land.

	The keys are only queued when send-keys returns, so the saved file is
	not on disk yet.  tmux tears the session down when the pane's command
	exits, which makes "session gone" the signal that the save completed.

	Some cases deliberately leave kg alive at the end (a running
	compilation prompts before quitting), so this must not be given the
	per-run timeout as its budget: the save has already happened by then
	and waiting the full timeout would make those cases the critical path.
	"""
	deadline = time.monotonic() + budget
	while time.monotonic() < deadline:
		cp = run_tmux_cmd(sock, "has-session", "-t", session, check=False)
		if cp.returncode != 0:
			return
		time.sleep(READY_POLL)


def run_editor_tmux(argv: list[str], filename: str, initial: str, keys: list[str],
		    trailer_keys: list[str], startup_delay: float,
		    key_delay: float, dimensions: tuple[int, int],
		    timeout: float, config_files: dict[str, str],
		    ready: bool, case_env: dict[str, str],
		    settle_floor: float = 0.0,
		    workspace_files: dict[str, str] | None = None,
		    file_mode: int | None = None) -> RunResult:
	if shutil.which("tmux") is None:
		return RunResult(None, None, "tmux not found", b"")

	with tempfile.TemporaryDirectory(prefix="kg-tmux-") as td:
		file_path = Path(td) / filename
		write_file_under_test(file_path, initial, file_mode)
		write_workspace_files(Path(td), workspace_files or {})

		home = Path(td) / "home"
		home.mkdir()
		write_config_files(home, config_files)
		sock = str(Path(td) / "tmux.sock")
		session = "ptyaccept"
		pane = f"{session}:0.0"
		rows, cols = dimensions
		cmd = "env -u XDG_CONFIG_HOME " + \
		      f"HOME={shlex.quote(str(home))} " + \
		      "TERM=xterm-256color LC_ALL=C.UTF-8 " + \
		      "".join(f"{k}={shlex.quote(v)} " for k, v in
			      expand_case_env(case_env, Path(td)).items()) + \
		      " ".join(shlex.quote(a) for a in argv + [str(file_path)])
		transcript = io.StringIO()

		try:
			run_tmux_cmd(sock, "new-session", "-d", "-s", session,
				     "-x", str(cols), "-y", str(rows), cmd)
			wait_ready_tmux(sock, pane, ready, startup_delay)
			for token in keys:
				if token.startswith("RESIZE="):
					r, c = map(int, token.split("=")[1].split(","))
					run_tmux_cmd(sock, "resize-window", "-t", session, "-x", str(c), "-y", str(r))
					time.sleep(key_delay)
					continue
				settle = settle_token(token)
				if settle is not None:
					wait_change_tmux(sock, pane, *settle)
					continue
				send_token_tmux(sock, pane, token)
				time.sleep(key_delay)
			settle_tmux(sock, pane, startup_delay, settle_floor)
			cp = run_tmux_cmd(sock, "capture-pane", "-t", pane, "-p", "-S", "-50",
					  check=False)
			transcript.write(cp.stdout)
			for token in trailer_keys:
				if token.startswith("RESIZE="):
					r, c = map(int, token.split("=")[1].split(","))
					run_tmux_cmd(sock, "resize-window", "-t", session, "-x", str(c), "-y", str(r))
					time.sleep(key_delay)
					continue
				settle = settle_token(token)
				if settle is not None:
					wait_change_tmux(sock, pane, *settle)
					continue
				send_token_tmux(sock, pane, token)
				time.sleep(key_delay)
			if trailer_keys:
				wait_exit_tmux(sock, session,
					       min(timeout, READY_DEADLINE))
			else:
				time.sleep(min(key_delay, timeout))
		except Exception as exc:
			try:
				cp = run_tmux_cmd(sock, "capture-pane", "-t", pane, "-p", "-S", "-50", check=False)
				transcript.write(cp.stdout)
			except Exception:
				pass
			return RunResult(None, None, str(exc), transcript.getvalue().encode())
		finally:
			try:
				cp = run_tmux_cmd(sock, "capture-pane", "-t", pane, "-p", "-S", "-50", check=False)
				transcript.write(cp.stdout)
			except Exception:
				pass
			run_tmux_cmd(sock, "kill-server", check=False)

		return RunResult(file_path.read_bytes(), 0, None, transcript.getvalue().encode())


def run_editor(argv: list[str], filename: str, initial: str, keys: list[str],
	       trailer_keys: list[str], backend: str, startup_delay: float,
	       key_delay: float, dimensions: tuple[int, int],
	       timeout: float, config_files: dict[str, str],
	       ready: bool = True, settle_floor: float = 0.0,
	       case_env: dict[str, str] | None = None,
	       workspace_files: dict[str, str] | None = None,
	       file_mode: int | None = None) -> RunResult:
	case_env = case_env or {}
	if backend == "tmux":
		return run_editor_tmux(argv, filename, initial, keys, trailer_keys,
				       startup_delay, key_delay, dimensions,
				       timeout, config_files, ready, case_env,
				       settle_floor, workspace_files, file_mode)
	return run_editor_pexpect(argv, filename, initial, keys, trailer_keys,
				  startup_delay, key_delay, dimensions,
				  timeout, config_files, ready, case_env,
				  workspace_files, file_mode)


def evaluate_case(case: Case, **kwargs) -> tuple[str, str | None, float]:
	"""Time one case.  The wall time is per case, not per key: it is what
	makes a slow lane visible in the results file rather than only as a
	timeout."""
	started = time.monotonic()
	status, details = evaluate_case_status(case, **kwargs)
	return (status, details, round(time.monotonic() - started, 3))


def evaluate_case_status(case: Case, kg_argv: list[str], features: set[str],
			 timeout: float, startup_delay_add: float,
			 key_delay_add: float, emacs: str | None,
			 have_tmux: bool,
			 settle_floor: float = 0.0) -> tuple[str, str | None]:
	if feature_mismatch(case, features):
		return ("SKIP", None)
	# A missing tool is a skip here and a hard failure in main() under
	# --require-tools; either way it is counted and named, never silent.
	if case_needs_tmux(case) and not have_tmux:
		return ("SKIP", f"{case.name}: skipped, tmux not found")
	if case.oracle == "emacs" and emacs is None:
		return ("SKIP", f"{case.name}: skipped, emacs oracle not found")
	missing_tool = case_missing_tool(case)
	if missing_tool:
		return ("SKIP", f"{case.name}: skipped, {missing_tool}")
	startup_delay = case.startup_delay + startup_delay_add
	key_delay = case.key_delay + key_delay_add
	kg_run = run_editor(kg_argv + case.editor_args, case.filename, case.initial, case.keys,
			    case.trailer_keys, case.backend, startup_delay,
			    key_delay, case.dimensions, timeout,
			    case.config_files, settle_floor=settle_floor,
			    case_env=case.env,
			    workspace_files=case.workspace_files,
			    file_mode=case.file_mode)
	if kg_run.error:
		return ("XFAIL" if case.xfail else "ERROR",
		        f"{case.name}: kg run error: {kg_run.error}")

	if case.oracle == "emacs":
		oracle_backend = case.oracle_backend or case.backend
		# The oracle keeps the fixed startup sleep: KG_READY describes
		# kg's mode line, not Emacs's.
		# The oracle gets the workspace fixture but not the HOME
		# config: `workspace_files:` describes the tree the file under
		# test lives in, which is as true for Emacs as for kg, while
		# `config_files:` (like `env:`) is kg's own configuration and
		# handing it over would only make the oracle's run differ from a
		# plain one in a way the case did not intend.
		emacs_run = run_editor([emacs, "-q", "-nw"], case.filename, case.initial,
				       case.keys, case.trailer_keys, oracle_backend,
				       startup_delay, key_delay, case.dimensions,
				       timeout, {}, ready=False,
				       workspace_files=case.workspace_files,
				       file_mode=case.file_mode)
		if emacs_run.error:
			return ("ERROR", f"{case.name}: emacs run error: {emacs_run.error}")
		passed = kg_run.saved == emacs_run.saved
		details = None if passed else diff_text(emacs_run.saved, kg_run.saved,
							"expected(emacs)", "actual(kg)")
	elif case.expected_saved_any is not None:
		expected_variants = [v.encode("utf-8") for v in case.expected_saved_any]
		passed = any(kg_run.saved == v for v in expected_variants)
		if passed:
			details = None
		else:
			details = diff_text(expected_variants[0], kg_run.saved, "expected[0]", "actual")
	else:
		expected = case.expected_saved.encode("utf-8")
		passed = kg_run.saved == expected
		details = None if passed else diff_text(expected, kg_run.saved,
							"expected", "actual")

	if passed and (case.expected_screen_contains or case.expected_screen_not_contains):
		screen = decode_text(kg_run.transcript)
		missing = []
		unexpected = []
		if case.expected_screen_contains is not None:
			missing = [s for s in case.expected_screen_contains if s not in screen]
		if case.expected_screen_not_contains is not None:
			unexpected = [s for s in case.expected_screen_not_contains if s in screen]
		if missing or unexpected:
			passed = 0
			msg = []
			if missing:
				msg.append("missing screen text: " + ", ".join(repr(s) for s in missing))
			if unexpected:
				msg.append("unexpected screen text: " + ", ".join(repr(s) for s in unexpected))
			details = "; ".join(msg)

	if passed and case.expected_exit_code is not None:
		if kg_run.exit_code != case.expected_exit_code:
			passed = 0
			details = (f"exit code {kg_run.exit_code}, "
				   f"expected {case.expected_exit_code}")
	elif passed and kg_run.exit_code not in (0, None):
		# A case that does not declare an expected status still
		# requires a clean one.  Only cases that opted in were checked
		# before, and a sanitizer reports through exactly this
		# channel: LeakSanitizer prints its report and exits 23 while
		# the saved file is perfectly correct, so ci-04 ran green over
		# a heap leak on the interactive error paths that ASan had
		# found and nothing was reading.
		#
		# The tmux backend cannot participate: it drives kg inside a
		# tmux pane and never waits on kg itself, so run_editor_tmux
		# reports 0 unconditionally -- which is why
		# expected_exit_code is refused for that backend too. Cases
		# that must observe a status use backend: pexpect.
		passed = 0
		details = (f"kg exited {kg_run.exit_code}; a sanitizer or "
			   "assertion failure reports through the exit "
			   "status even when the saved file is correct")

	if passed:
		return ("XPASS", None) if case.xfail else ("PASS", None)
	return ("XFAIL", details) if case.xfail else ("FAIL", details)


def main() -> int:
	parser = argparse.ArgumentParser(description="Run PTY-backed acceptance tests for kg.")
	parser.add_argument("--kg", required=True, help="Path to kg binary")
	parser.add_argument("--kg-runner", default="", help="Optional command prefix used to run kg")
	parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
	                    help="Per-run timeout in seconds")
	parser.add_argument("--startup-delay-add", type=float, default=0.0,
	                    help="Additional startup delay added to every case")
	parser.add_argument("--key-delay-add", type=float, default=0.0,
	                    help="Additional per-key delay added to every case")
	parser.add_argument("--settle-floor", type=float, default=0.0,
	                    help="Minimum seconds a tmux case waits after its "
	                         "last key before the pane may be declared "
	                         "settled (see settle_tmux)")
	parser.add_argument("--jobs", "-j", type=int, default=0,
	                    help="Cases to run concurrently (0 picks a default, "
	                         "1 runs them in this process)")
	parser.add_argument("--emacs", default="",
	                    help="Emacs binary the oracle cases compare against "
	                         "(default: $KG_PTY_EMACS, then PATH)")
	parser.add_argument("--require-tools", action="store_true",
	                    help="Fail instead of skipping when a tool some case "
	                         "needs (tmux, the Emacs oracle, a case's "
	                         "requires_tool or requires_python_module) is "
	                         "missing")
	parser.add_argument("--json", dest="json_path", default="",
	                    help="Write per-case results (status, wall time, "
	                         "backend, oracle) to this file")
	parser.add_argument("cases", nargs="+", help="YAML case files")
	args = parser.parse_args()
	args.kg = str(Path(args.kg).resolve())
	kg_argv = shlex.split(args.kg_runner) + [args.kg]
	version = subprocess.run(kg_argv + ["-V"], check=True, capture_output=True, text=True)
	features = {word[1:] for word in version.stdout.split() if word.startswith("+")}

	counts = {k: 0 for k in ("PASS", "SKIP", "FAIL", "XFAIL", "XPASS", "ERROR")}

	cases = [load_case(Path(p)) for p in args.cases]
	jobs = args.jobs if args.jobs > 0 else min(DEFAULT_JOBS, os.cpu_count() or 1)
	jobs = max(1, min(jobs, len(cases)))

	# What this run can actually exercise, decided once and reported, so a
	# green summary means the same thing everywhere it is read.
	emacs = resolve_emacs(args.emacs)
	have_tmux = shutil.which("tmux") is not None
	oracle_cases = sum(1 for c in cases if c.oracle == "emacs")
	tmux_cases = sum(1 for c in cases if case_needs_tmux(c))
	missing = []
	if oracle_cases and emacs is None:
		missing.append(f"emacs ({oracle_cases} oracle case(s); set --emacs "
			       "or $KG_PTY_EMACS, or put emacs on PATH)")
	if tmux_cases and not have_tmux:
		missing.append(f"tmux ({tmux_cases} case(s) need it)")
	tool_cases: dict[str, int] = {}
	for case in cases:
		for tool in case.requires_tool:
			tool_cases[tool] = tool_cases.get(tool, 0) + 1
	for tool in sorted(tool_cases):
		unavailable = tool_unavailable(tool)
		if unavailable:
			missing.append(f"{unavailable} ({tool_cases[tool]} case(s) need it)")
	module_cases: dict[str, int] = {}
	for case in cases:
		if case.requires_python_module:
			module_cases[case.requires_python_module] = \
				module_cases.get(case.requires_python_module, 0) + 1
	for module in sorted(module_cases):
		unavailable = python_module_unavailable(module)
		if unavailable:
			missing.append(
				f"{unavailable} ({module_cases[module]} case(s) need it)")
	for item in missing:
		print(f"{'FAIL' if args.require_tools else 'warning'}: missing tool: {item}",
		      file=sys.stderr)
	if missing and args.require_tools:
		print("FAIL: --require-tools was given and the suite cannot run in full",
		      file=sys.stderr)
		return 1
	if emacs and oracle_cases:
		print(f"# oracle: {emacs} ({oracle_cases} case(s))")

	run_one = functools.partial(evaluate_case, kg_argv=kg_argv,
				    features=features, timeout=args.timeout,
				    startup_delay_add=args.startup_delay_add,
				    key_delay_add=args.key_delay_add,
				    emacs=emacs, have_tmux=have_tmux,
				    settle_floor=args.settle_floor)

	records: list[dict] = []

	def report(results) -> None:
		for case, (status, details, seconds) in zip(cases, results):
			counts[status] += 1
			print(f"{status}: {case.name}")
			if details:
				print(details.rstrip())
			records.append({
				"name": case.name,
				"path": str(case.path),
				"status": status,
				"seconds": seconds,
				"backend": case.backend,
				"oracle": case.oracle,
				"requires_feature": list(case.requires_feature),
				"requires_tool": list(case.requires_tool),
				"requires_python_module": case.requires_python_module,
				"xfail": case.xfail,
			})

	# The suite is dominated by waiting on a child editor, so running cases
	# side by side is close to a linear speedup.  Cases are independent:
	# each gets its own temporary HOME and its own tmux socket.
	#
	# Processes rather than threads, and forkserver rather than plain fork,
	# because pexpect runs a good deal of Python (setsid, ioctls, fd
	# juggling) between fork() and execv().  Forking that from a
	# multi-threaded process can deadlock the child on a lock held by a
	# thread that does not exist in it -- a rare, unreproducible hang, and
	# the worst failure mode a test harness can have.  Each worker here is
	# single-threaded, so pexpect always forks from a single-threaded
	# process.  map() keeps the report in case order regardless of
	# completion order, so CI logs stay diffable.
	if jobs == 1:
		# Stay in-process so a debugging run keeps its tracebacks and
		# its C-c.
		report(map(run_one, cases))
	else:
		with ProcessPoolExecutor(max_workers=jobs,
					 mp_context=multiprocessing.get_context(
						 "forkserver")) as pool:
			report(pool.map(run_one, cases))

	if args.json_path:
		path = Path(args.json_path)
		path.parent.mkdir(parents=True, exist_ok=True)
		with path.open("w", encoding="utf-8") as fp:
			json.dump({
				"schema": "kg-pty-results/1",
				"kg": args.kg,
				"kg_runner": args.kg_runner,
				"jobs": jobs,
				"emacs": emacs,
				"tmux": have_tmux,
				"timeout": args.timeout,
				"startup_delay_add": args.startup_delay_add,
				"key_delay_add": args.key_delay_add,
				"settle_floor": args.settle_floor,
				"counts": counts,
				"cases": records,
			}, fp, indent=1)
			fp.write("\n")

	total = sum(counts.values())
	print()
	print("============================================================================")
	print("PTY acceptance summary for kg")
	print("============================================================================")
	print(f"# TOTAL: {total}")
	print(f"# PASS:  {counts['PASS']}")
	print(f"# SKIP:  {counts['SKIP']}")
	print(f"# XFAIL: {counts['XFAIL']}")
	print(f"# FAIL:  {counts['FAIL']}")
	print(f"# XPASS: {counts['XPASS']}")
	print(f"# ERROR: {counts['ERROR']}")
	print("============================================================================")

	return 0 if counts["FAIL"] == 0 and counts["ERROR"] == 0 and counts["XPASS"] == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
