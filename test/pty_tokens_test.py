#!/usr/bin/env python3
"""Wire-level tests for PTY acceptance key tokens."""

import unittest
from unittest import mock

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


if __name__ == "__main__":
	unittest.main()
