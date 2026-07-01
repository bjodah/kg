#!/usr/bin/env python3

import argparse
import difflib
import io
import os
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
	)


def run_editor(argv: list[str], filename: str, initial: str, keys: list[str], trailer_keys: list[str]) -> RunResult:
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
			dimensions=DEFAULT_DIMENSIONS,
		)
		child.delaybeforesend = 0.03
		child.logfile_read = log

		try:
			time.sleep(0.2)
			for token in [*keys, *trailer_keys]:
				child.send(token_to_bytes(token))
			child.expect(pexpect.EOF, timeout=DEFAULT_TIMEOUT)
			child.close()
		except Exception as exc:
			child.close(force=True)
			return RunResult(None, None, str(exc), log.getvalue())

		exit_code = child.exitstatus
		if exit_code is None and child.signalstatus is not None:
			exit_code = 128 + child.signalstatus

		return RunResult(file_path.read_bytes(), exit_code, None, log.getvalue())


def evaluate_case(case: Case, kg_path: str) -> tuple[str, str | None]:
	kg_run = run_editor([kg_path], case.filename, case.initial, case.keys, case.trailer_keys)
	if kg_run.error:
		return ("XFAIL" if case.xfail else "ERROR",
		        f"{case.name}: kg run error: {kg_run.error}")

	if case.oracle == "emacs":
		emacs_run = run_editor([EMACS, "-q", "-nw"], case.filename, case.initial,
				       case.keys, case.trailer_keys)
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
