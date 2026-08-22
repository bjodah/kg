import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from utils import pty_accept


class TokenBytesTest(unittest.TestCase):
	def test_function_keys_are_terminal_sequences_not_literal_names(self) -> None:
		expected = {
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
		}
		for token, wire in expected.items():
			with self.subTest(token=token):
				self.assertEqual(pty_accept.token_to_bytes(token), wire)
				self.assertNotEqual(wire, token.encode())

	def test_dap_modifier_and_page_tokens_are_exact(self) -> None:
		expected = {
			"C-F5": b"\x1b[15;5~",
			"C-F9": b"\x1b[20;5~",
			"M-F10": b"\x1b[21;3~",
			"M-F11": b"\x1b[23;3~",
			"M-Up": b"\x1b[1;3A",
			"M-Down": b"\x1b[1;3B",
			"PageUp": b"\x1b[5~",
			"PageDown": b"\x1b[6~",
		}
		for token, wire in expected.items():
			with self.subTest(token=token):
				self.assertEqual(pty_accept.token_to_bytes(token), wire)

	def test_existing_named_aliases_keep_their_bytes(self) -> None:
		expected = {
			"RET": b"\r",
			"M-RET": b"\x1b\r",
			"M-TAB": b"\x1b\t",
			"M-DEL": b"\x1b\x7f",
			"C-SPC": b"\x00",
			"Insert": b"\x1b[2~",
			"Home": b"\x1b[1~",
			"End": b"\x1b[4~",
			"Up": b"\x1b[A",
			"Down": b"\x1b[B",
			"C-Home": b"\x1b[1;5H",
			"S-End": b"\x1b[1;2F",
		}
		for token, wire in expected.items():
			with self.subTest(token=token):
				self.assertEqual(pty_accept.token_to_bytes(token), wire)

	def test_both_backends_use_the_same_byte_source(self) -> None:
		child = mock.Mock()
		with mock.patch.object(pty_accept, "drain_pexpect"):
			pty_accept.send_token_pexpect(child, "F5")
		child.send.assert_called_once_with(pty_accept.token_to_bytes("F5"))

		with mock.patch.object(pty_accept, "run_tmux_cmd") as run:
			pty_accept.send_token_tmux("sock", "pane", "F5")
		run.assert_called_once_with(
			"sock", "send-keys", "-t", "pane", "-H", "1b", "5b", "31",
			"35", "7e"
		)

	def test_unknown_named_modifier_tokens_are_refused(self) -> None:
		# Without the table entry these used to be typed as their own
		# letters -- M-F12 as ESC then "F12" -- so a case asserted on a
		# key it never sent.  Refusing is the only honest answer.
		for token in ("M-F12", "S-Up", "C-PageDown", "M-Home", "S-F5"):
			with self.subTest(token=token):
				with self.assertRaisesRegex(ValueError, "NAMED_KEY_BYTES"):
					pty_accept.token_to_bytes(token)

	def test_single_character_and_escape_payloads_still_pass(self) -> None:
		# One character after the modifier keeps its old meaning, and an
		# SGR mouse report is parametrised by button and cell, so it can
		# never be a table entry and must stay spellable as ESC + body.
		expected = {
			"M-x": b"\x1bx",
			"M-\\": b"\x1b\\",
			"M-[": b"\x1b[",
			"C-x": b"\x18",
			"M-SPC": b"\x1b ",
			"M-[<0;14;2M": b"\x1b[<0;14;2M",
		}
		for token, wire in expected.items():
			with self.subTest(token=token):
				self.assertEqual(pty_accept.token_to_bytes(token), wire)

	def test_raw_byte_stays_pexpect_only(self) -> None:
		self.assertEqual(pty_accept.token_to_bytes("BYTE=e2"), b"\xe2")
		with self.assertRaisesRegex(ValueError, "backend: pexpect"):
			pty_accept.tmux_token_hex("BYTE=e2")


class ReadinessTest(unittest.TestCase):
	"""What the harness does when the editor never paints a first frame.

	Wire-level for the same reason the token tests are: the answer is a
	property of the harness, not of any one case, and it decides which
	line discipline the case's keys are interpreted by.
	"""

	def test_editor_that_never_paints_is_refused_not_typed_into(self) -> None:
		# Sending anyway types into a pty still in its default cooked
		# mode, where the keys mean what termios says and not what kg
		# says: C-c is VINTR (kg dies of SIGINT, which the harness
		# reports as its own 128+2), C-u is VKILL and erases every key
		# queued before it.  The failures that come out name the
		# editor, so they have to not happen.
		child = mock.Mock()
		child.expect.side_effect = pty_accept.pexpect.TIMEOUT("no frame")
		with self.assertRaisesRegex(pty_accept.EditorNotReady, "cooked"):
			pty_accept.wait_ready_pexpect(child, True, 0.5, 0.1)
		child.send.assert_not_called()

	def test_tmux_readiness_expiry_is_refused_too(self) -> None:
		# READY_DEADLINE is a floor under the budget, so it has to come
		# down too or this test waits it out rather than the 0.1 asked
		# for.
		with mock.patch.object(pty_accept, "run_tmux_cmd") as run, \
		     mock.patch.object(pty_accept, "READY_DEADLINE", 0.01):
			run.return_value = mock.Mock(stdout="no mode line here")
			with self.assertRaisesRegex(pty_accept.EditorNotReady,
						    "cooked"):
				pty_accept.wait_ready_tmux("sock", "pane", True,
							   0.5, 0.1)

	def test_typeahead_waits_for_the_editors_raw_mode_signature(self) -> None:
		# The mouse-report request is written from inside
		# enable_raw_mode(), so it is the harness's proof that the line
		# discipline is kg's.  An editor that never sends it never gets
		# typed into: sending anyway would put C-c and C-u in front of
		# termios.
		child = mock.Mock()
		child.expect.side_effect = pty_accept.pexpect.TIMEOUT("no output")
		with self.assertRaisesRegex(pty_accept.EditorNotReady, "cooked"):
			pty_accept.send_typeahead_pexpect(child, ["C-c"], 0.1)
		child.send.assert_not_called()

	def test_typeahead_trigger_is_the_exact_escape_not_any_output(self) -> None:
		# argv[0] need not be kg: --kg-runner puts a wrapper on the same
		# pty, and `valgrind` without --quiet greets it.  Matching "the
		# first thing that arrives" would take that banner for raw mode
		# and type into a cooked terminal, which is measured: bare
		# valgrind turns this case's C-c back into SIGINT.
		self.assertEqual(pty_accept.KG_RAW_BYTES.pattern,
				 rb"\x1b\[\?1000;1002;1006h")
		self.assertIsNone(pty_accept.KG_RAW_BYTES.search(
			b"==12345== Memcheck, a memory error detector"))

	def test_typeahead_goes_out_as_one_write_after_that(self) -> None:
		# One write and no key_delay: type-ahead is what arrives faster
		# than the editor reads it, and the pty queues the burst whole.
		child = mock.Mock()
		pty_accept.send_typeahead_pexpect(child, ["C-c", "C-u", "3"], 0.1)
		child.expect.assert_called_once()
		child.send.assert_called_once_with(b"\x03\x15" b"3")

	def test_no_typeahead_waits_for_nothing_at_all(self) -> None:
		# The match would otherwise be consumed out from under the
		# ordinary readiness wait for every case that has no type-ahead.
		child = mock.Mock()
		pty_accept.send_typeahead_pexpect(child, [], 0.1)
		child.expect.assert_not_called()
		child.send.assert_not_called()

	def test_unrecognised_editor_reports_startup_failure(self) -> None:
		# Emacs readiness is recognised from its mode line now.  A process
		# that never paints one must be reported as infrastructure failure,
		# rather than receiving keys in the pty's cooked mode.
		with self.assertRaisesRegex(pty_accept.EditorNotReady,
							   "Emacs startup settle"):
			pty_accept.wait_ready_pexpect(mock.Mock(), False,
							      0.5, 20.0)


if __name__ == "__main__":
	unittest.main()
