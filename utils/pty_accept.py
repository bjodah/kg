#!/usr/bin/env python3

import argparse
import difflib
import io
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import pexpect
import yaml


DEFAULT_TRAILER = ["C-x", "C-s", "C-x", "C-c"]
DEFAULT_DIMENSIONS = (24, 80)
DEFAULT_TIMEOUT = 5.0
DEFAULT_STARTUP_DELAY = 0.5
DEFAULT_KEY_DELAY = 0.05
EMACS = "/opt-3/emacs-31-lucid/bin/emacs"


@dataclass
class Case:
	name: str
	path: Path
	filename: str
	initial: str
	keys: list[str]
	expected_saved: str | None
	expected_saved_any: list[str] | None
	oracle: str | None
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


def token_to_bytes(token: str) -> bytes:
	if not isinstance(token, str) or not token:
		raise ValueError(f"invalid key token: {token!r}")

	upper = token.upper()

	if upper in ("ESC",):
		return b"\x1b"
	if upper in ("RET", "ENTER"):
		return b"\r"
	if upper == "TAB":
		return b"\t"
	if upper in ("SPC", "SPACE"):
		return b" "
	if upper in ("C-SPC", "C-SPACE", "C-@"):
		return b"\x00"

	if len(token) >= 3 and token[1] == "-":
		prefix = token[0].upper()
		payload = token[2:]
		if prefix == "C":
			return ctrl_byte(payload)
		if prefix == "M":
			return b"\x1b" + payload.encode("utf-8")

	return token.encode("utf-8")


def send_token_pexpect(child: pexpect.spawn, token: str) -> None:
	upper = token.upper()

	if upper in ("C-SPC", "C-SPACE", "C-@"):
		child.sendcontrol("@")
		return

	if len(token) >= 3 and token[1] == "-":
		prefix = token[0].upper()
		payload = token[2:]
		if prefix == "C" and len(payload) == 1:
			child.sendcontrol(payload)
			return
		if prefix == "M" and len(payload) == 1:
			child.send("\x1b")
			child.send(payload)
			return

	for b in token_to_bytes(token):
		child.send(bytes([b]))


def tmux_key_name(token: str) -> tuple[str, str]:
	upper = token.upper()

	if upper == "ESC":
		return ("key", "Escape")
	if upper in ("RET", "ENTER"):
		return ("key", "Enter")
	if upper == "TAB":
		return ("key", "Tab")
	if upper in ("SPC", "SPACE"):
		return ("key", "Space")
	if upper in ("C-SPC", "C-SPACE", "C-@"):
		return ("key", "C-Space")

	if len(token) >= 3 and token[1] == "-":
		prefix = token[0].upper()
		payload = token[2:]
		if len(payload) == 1:
			if prefix == "C":
				return ("key", f"C-{payload}")
			if prefix == "M":
				return ("key", f"M-{payload}")

	return ("literal", token)


def decode_text(data: bytes) -> str:
	return data.decode("utf-8", "replace")


def diff_text(expected: bytes, actual: bytes, expected_name: str, actual_name: str) -> str:
	return "".join(difflib.unified_diff(
		decode_text(expected).splitlines(True),
		decode_text(actual).splitlines(True),
		fromfile=expected_name,
		tofile=actual_name,
	))


def load_case(path: Path) -> Case:
	data = yaml.safe_load(path.read_text())

	if not isinstance(data, dict):
		raise ValueError(f"{path}: YAML root must be a mapping")
	if "filename" not in data or "initial" not in data or "keys" not in data:
		raise ValueError(f"{path}: required keys are filename, initial, keys")
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
	backend = data.get("backend", "pexpect")
	if backend not in ("pexpect", "tmux"):
		raise ValueError(f"{path}: backend must be pexpect or tmux")
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
		initial=data["initial"],
		keys=data["keys"],
		expected_saved=data.get("expected_saved"),
		expected_saved_any=data.get("expected_saved_any"),
		oracle=data.get("oracle"),
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


def run_editor_pexpect(argv: list[str], filename: str, initial: str, keys: list[str],
		       trailer_keys: list[str], startup_delay: float,
		       key_delay: float, dimensions: tuple[int, int]) -> RunResult:
	with tempfile.TemporaryDirectory(prefix="kg-pty-") as td:
		file_path = Path(td) / filename
		file_path.parent.mkdir(parents=True, exist_ok=True)
		file_path.write_text(initial)

		env = os.environ.copy()
		env["HOME"] = td
		env["TERM"] = env.get("TERM", "xterm-256color")
		env.setdefault("LC_ALL", "C.UTF-8")

		log = io.BytesIO()
		child = pexpect.spawn(
			argv[0],
			argv[1:] + [str(file_path)],
			cwd=td,
			env=env,
			encoding=None,
			echo=False,
			timeout=DEFAULT_TIMEOUT,
			dimensions=dimensions,
		)
		child.delaybeforesend = 0
		child.logfile_read = log

		try:
			time.sleep(startup_delay)
			for token in [*keys, *trailer_keys]:
				send_token_pexpect(child, token)
				time.sleep(key_delay)
			child.expect(pexpect.EOF, timeout=DEFAULT_TIMEOUT)
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


def run_editor_tmux(argv: list[str], filename: str, initial: str, keys: list[str],
		    trailer_keys: list[str], startup_delay: float,
		    key_delay: float, dimensions: tuple[int, int]) -> RunResult:
	if shutil.which("tmux") is None:
		return RunResult(None, None, "tmux not found", b"")

	with tempfile.TemporaryDirectory(prefix="kg-tmux-") as td:
		file_path = Path(td) / filename
		file_path.parent.mkdir(parents=True, exist_ok=True)
		file_path.write_text(initial)

		home = Path(td) / "home"
		home.mkdir()
		sock = str(Path(td) / "tmux.sock")
		session = "ptyaccept"
		pane = f"{session}:0.0"
		rows, cols = dimensions
		cmd = "env " + \
		      f"HOME={shlex.quote(str(home))} " + \
		      "TERM=xterm-256color LC_ALL=C.UTF-8 " + \
		      " ".join(shlex.quote(a) for a in argv + [str(file_path)])
		transcript = io.StringIO()

		try:
			run_tmux_cmd(sock, "new-session", "-d", "-s", session,
				     "-x", str(cols), "-y", str(rows), cmd)
			time.sleep(startup_delay)
			for token in [*keys, *trailer_keys]:
				mode, value = tmux_key_name(token)
				if mode == "key":
					run_tmux_cmd(sock, "send-keys", "-t", pane, value)
				else:
					run_tmux_cmd(sock, "send-keys", "-t", pane, "-l", value)
				time.sleep(key_delay)
			time.sleep(key_delay)
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
	       key_delay: float, dimensions: tuple[int, int]) -> RunResult:
	if backend == "tmux":
		return run_editor_tmux(argv, filename, initial, keys, trailer_keys,
				       startup_delay, key_delay, dimensions)
	return run_editor_pexpect(argv, filename, initial, keys, trailer_keys,
				  startup_delay, key_delay, dimensions)


def evaluate_case(case: Case, kg_path: str) -> tuple[str, str | None]:
	kg_run = run_editor([kg_path], case.filename, case.initial, case.keys,
			    case.trailer_keys, case.backend, case.startup_delay,
			    case.key_delay, case.dimensions)
	if kg_run.error:
		return ("XFAIL" if case.xfail else "ERROR",
		        f"{case.name}: kg run error: {kg_run.error}")

	if case.oracle == "emacs":
		oracle_backend = case.oracle_backend or case.backend
		emacs_run = run_editor([EMACS, "-q", "-nw"], case.filename, case.initial,
				       case.keys, case.trailer_keys, oracle_backend,
				       case.startup_delay, case.key_delay, case.dimensions)
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

	if passed:
		return ("XPASS", None) if case.xfail else ("PASS", None)
	return ("XFAIL", details) if case.xfail else ("FAIL", details)


def main() -> int:
	parser = argparse.ArgumentParser(description="Run PTY-backed acceptance tests for kg.")
	parser.add_argument("--kg", required=True, help="Path to kg binary")
	parser.add_argument("cases", nargs="+", help="YAML case files")
	args = parser.parse_args()
	args.kg = str(Path(args.kg).resolve())

	counts = {k: 0 for k in ("PASS", "FAIL", "XFAIL", "XPASS", "ERROR")}

	for case_path in args.cases:
		case = load_case(Path(case_path))
		status, details = evaluate_case(case, args.kg)
		counts[status] += 1
		print(f"{status}: {case.name}")
		if details:
			print(details.rstrip())

	total = sum(counts.values())
	print()
	print("============================================================================")
	print("PTY acceptance summary for kg")
	print("============================================================================")
	print(f"# TOTAL: {total}")
	print(f"# PASS:  {counts['PASS']}")
	print("# SKIP:  0")
	print(f"# XFAIL: {counts['XFAIL']}")
	print(f"# FAIL:  {counts['FAIL']}")
	print(f"# XPASS: {counts['XPASS']}")
	print(f"# ERROR: {counts['ERROR']}")
	print("============================================================================")

	return 0 if counts["FAIL"] == 0 and counts["ERROR"] == 0 and counts["XPASS"] == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
