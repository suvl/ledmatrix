#!/usr/bin/env python3
"""Checks on tools/burn_key (the one-off eFuse burn sketch) that need no board and no compiler. Python 3, standard library only:
    python3 -I tools/test_burn_key.py
- the sketch folder's partitions.csv is byte for byte the firmware's (arduino-cli picks the table from the sketch folder, and a
  different table would move or drop the `ident` partition the burn reads its key from),
- the default is a dry run, and every call that writes an eFuse sits inside a "#if BURN_KEY5_FOR_REAL" block,
- nothing prints a key.
The compile checks (dry run, and a real-burn build that is only compiled) are in the notes of the ticket, not here."""
import os
import re
import sys
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
SKETCH_DIR = os.path.join(HERE, "burn_key")
INO = os.path.join(SKETCH_DIR, "burn_key.ino")
FIRMWARE_TABLE = os.path.join(REPO, "arduino", "ledmatrix", "partitions.csv")
SKETCH_TABLE = os.path.join(SKETCH_DIR, "partitions.csv")

# An ALLOWLIST: every esp_efuse_* / efuse_hal_* name that is not in the read-only list below counts as a writer, whatever it is
# called (a new or misspelled name cannot slip past a list of known bad names). Types (esp_efuse_block_t) are not calls.
READ_ONLY = re.compile(r"esp_efuse_(get_\w+|key_block_unused|find_purpose|count_unused_key_blocks|block_is_empty|mac_get_default|\w+_t)")
EFUSE_NAMES = re.compile(r"\b(esp_efuse_\w+|efuse_hal_\w+|efuse_ll_\w+|esp_rom_efuse\w*|ets_efuse\w*)")
# Other ways to change the chip or the flash behind the API's back
OTHER_WRITERS = re.compile(r"\b(esp_partition_(write|erase)\w*|spi_flash_(write|erase)\w*|esp_flash_(write|erase)\w*|REG_WRITE|REG_SET_BIT|REG_SET_FIELD|"
                           r"WRITE_PERI_REG|SET_PERI_REG_\w+|CLEAR_PERI_REG_MASK|REG_CLR_BIT)\b")


def writer_calls(code, flash_too=True):
    """The names in one line of code that could change an eFuse (or, with flash_too, the flash or a register)."""
    hits = [m.group(0) for m in EFUSE_NAMES.finditer(code) if READ_ONLY.fullmatch(m.group(0)) is None]
    if flash_too:
        hits += [m.group(0) for m in OTHER_WRITERS.finditer(code)]
    return hits


def read(path):
    with open(path, "rb") as f:
        return f.read()


def code_lines(text):
    """The source as (line number, code) pairs with comments removed and the inside of string and character literals blanked
    (so a word in a message is not taken for code, and a // inside a string is not taken for a comment)."""
    out, in_block = [], False
    for n, raw in enumerate(text.splitlines(), 1):
        res, i = [], 0
        while i < len(raw):
            c = raw[i]
            if in_block:
                if raw.startswith("*/", i):
                    in_block = False
                    i += 2
                else:
                    i += 1
            elif raw.startswith("/*", i):
                in_block = True
                i += 2
            elif raw.startswith("//", i):
                break
            elif c in "\"'":
                q = c
                res.append(q)
                i += 1
                while i < len(raw) and raw[i] != q:
                    i += 2 if raw[i] == "\\" else 1
                res.append(q)
                i += 1
            else:
                res.append(c)
                i += 1
        out.append((n, "".join(res)))
    return out


def guarded_lines(text):
    """Yields (line number, code, inside a '#if BURN_KEY5_FOR_REAL' branch) for every line. An #else or #elif branch of such a block
    is NOT guarded (it is what a dry run compiles)."""
    stack = []  # one entry per open #if: True when the branch being read is the one taken only by a real-burn build
    for n, code in code_lines(text):
        d = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", code)
        if d:
            kind, rest = d.group(1), d.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(kind == "if" and re.fullmatch(r"BURN_KEY5_FOR_REAL", rest) is not None)
            elif kind in ("elif", "else"):
                if stack:
                    stack[-1] = False
            elif kind == "endif" and stack:
                stack.pop()
            yield n, code, any(stack)
            continue
        yield n, code, any(stack)


class PartitionTable(unittest.TestCase):
    def test_the_two_copies_are_identical(self):
        self.assertTrue(os.path.exists(SKETCH_TABLE), "tools/burn_key/partitions.csv is missing")
        self.assertEqual(read(SKETCH_TABLE), read(FIRMWARE_TABLE),
                         "tools/burn_key/partitions.csv differs from arduino/ledmatrix/partitions.csv: copy the firmware's file over it")

    def test_the_table_keeps_the_ident_partition(self):
        rows = [re.split(r"\s*,\s*", l.strip()) for l in read(SKETCH_TABLE).decode().splitlines() if l.strip() and not l.lstrip().startswith("#")]
        ident = [r for r in rows if r[0] == "ident"]
        self.assertEqual(len(ident), 1)
        self.assertEqual(ident[0][1:5], ["data", "0x40", "0x7FE000", "8K"])
        self.assertIn("readonly", ident[0][5] if len(ident[0]) > 5 else "")


class Allowlist(unittest.TestCase):
    def test_what_counts_as_a_writer(self):
        for name in ("esp_efuse_write_key(1)", "esp_efuse_set_key_dis_read(2)", "esp_efuse_batch_write_commit()", "esp_efuse_burn_something(3)",
                     "esp_efuse_totally_new_call(4)", "efuse_hal_program(5)", "efuse_ll_set_pgm_cmd(6)", "esp_efuse_reset()", "esp_efuse_disable_basic_rom_console()",
                     "esp_partition_write(p, 0, b, 1)", "esp_flash_erase_region(c, 0, 1)", "REG_WRITE(EFUSE_CMD_REG, 2)"):
            self.assertTrue(writer_calls(name), name)
        for name in ("esp_efuse_get_key_purpose(b)", "esp_efuse_get_key_dis_read(b)", "esp_efuse_get_keypurpose_dis_write(b)", "esp_efuse_key_block_unused(b)",
                     "esp_efuse_find_purpose(p, &b)", "esp_efuse_count_unused_key_blocks()", "esp_efuse_block_is_empty(b)",
                     "esp_efuse_mac_get_default(mac)", "esp_efuse_block_t b;", "esp_efuse_purpose_t p;", "ESP_EFUSE_KEY_PURPOSE_HMAC_UP", "EFUSE_BLK_KEY5",
                     "esp_partition_read(p, 0, b, 1)", "esp_hmac_calculate(k, m, 1, o)"):
            self.assertEqual(writer_calls(name), [], name)

    def test_the_firmware_sources_contain_no_efuse_writer_at_all(self):
        fw = os.path.join(REPO, "arduino", "ledmatrix")
        files = sorted(f for f in os.listdir(fw) if f.endswith((".cpp", ".h", ".ino", ".c")) and f != "portal_page.h")
        self.assertIn("identity.cpp", files)
        self.assertIn("signers.h", files)
        for f in files:
            for n, code in code_lines(read(os.path.join(fw, f)).decode()):
                self.assertEqual(writer_calls(code, flash_too=False), [], "%s:%d" % (f, n))

    def test_one_constant_for_the_key_block(self):
        signers_h = read(os.path.join(REPO, "arduino", "ledmatrix", "signers.h")).decode()
        self.assertRegex(signers_h, r"KEY_BLOCK\s*=\s*EFUSE_BLK_KEY5;")
        self.assertRegex(signers_h, r"KEY_ID\s*=\s*HMAC_KEY5;")
        self.assertRegex(signers_h, r"static_assert\(\(int\)KEY_ID == \(int\)KEY_BLOCK - \(int\)EFUSE_BLK_KEY0")
        # nowhere else names the block or the key id: everything goes through signers::KEY_BLOCK and signers::KEY_ID
        fw = os.path.join(REPO, "arduino", "ledmatrix")
        files = [os.path.join(fw, f) for f in sorted(os.listdir(fw)) if f.endswith((".cpp", ".h", ".ino")) and f != "portal_page.h"] + [INO]
        pattern = re.compile(r"\b(HMAC_KEY5|EFUSE_BLK_KEY5|EFUSE_BLK5|EFUSE_BLK9)\b")
        counts = {os.path.basename(f): sum(len(pattern.findall(c)) for _, c in code_lines(read(f).decode())) for f in files}
        self.assertEqual({k: v for k, v in counts.items() if v}, {"signers.h": 2, "burn_key.ino": 1})  # the two definitions, and the sketch's static_assert
        self.assertRegex(self.sketch_text(), r"BLOCK\s*=\s*signers::KEY_BLOCK")
        self.assertIn("signers::KEY_ID", self.sketch_text())
        self.assertIn("signers::KEY_BLOCK", read(os.path.join(fw, "identity.cpp")).decode())

    def sketch_text(self):
        return read(INO).decode()


class Sketch(unittest.TestCase):
    def setUp(self):
        self.text = read(INO).decode()

    def test_the_default_is_a_dry_run(self):
        self.assertRegex(self.text, r"(?m)^#ifndef BURN_KEY5_FOR_REAL\s*\n#define BURN_KEY5_FOR_REAL 0\s*\n#endif")
        self.assertRegex(self.text, r"#if BURN_KEY5_FOR_REAL != 0 && BURN_KEY5_FOR_REAL != 1\s*\n#error")
        # a real burn without a board ID does not compile
        self.assertRegex(self.text, r"#elif BURN_KEY5_FOR_REAL\s*\n#error[^\n]*BURN_DEVICE_ID")
        self.assertIn("static_assert(burnIdIsValid(BURN_DEVICE_ID)", self.text)

    def test_every_eFuse_write_is_inside_the_real_burn_guard(self):
        outside, inside = [], []
        for n, code, guarded in guarded_lines(self.text):
            for name in writer_calls(code):
                (inside if guarded else outside).append((n, name))
        self.assertEqual(outside, [], "writing calls outside '#if BURN_KEY5_FOR_REAL': %s" % outside)
        self.assertEqual([w for _, w in inside].count("esp_efuse_write_key"), 1, "exactly one write site")
        self.assertTrue(inside)  # the guard really holds the write (the check above is not passing for lack of anything to find)

    def test_the_guard_finder_itself(self):
        src = "a\n#if BURN_KEY5_FOR_REAL\nesp_efuse_write_key(1);\n#else\nesp_efuse_write_key(2);\n#endif\nesp_efuse_set_key_dis_read(3);\n" \
              "#ifdef BURN_DEVICE_ID\nesp_efuse_write_key(4);\n#endif\n// esp_efuse_write_key(5)\nx = \"esp_efuse_write_key(6)\";\n"
        found = [(n, bool(writer_calls(c)), g) for n, c, g in guarded_lines(src)]
        by_line = {n: (w, g) for n, w, g in found}
        self.assertEqual(by_line[3], (True, True))    # inside #if BURN_KEY5_FOR_REAL
        self.assertEqual(by_line[5], (True, False))   # the #else branch is what a dry run builds
        self.assertEqual(by_line[7], (True, False))   # after the #endif
        self.assertEqual(by_line[9], (True, False))   # another #ifdef is not the guard
        self.assertEqual(by_line[11], (False, False))  # a comment
        self.assertEqual(by_line[12], (False, False))  # inside a string

    def test_the_real_burn_needs_a_typed_confirmation(self):
        raw = self.text.splitlines()
        guarded = "\n".join(raw[n - 1] for n, _, g in guarded_lines(self.text) if g)  # the text of the guarded lines, strings included
        self.assertIn('snprintf(want, sizeof(want), "BURN %s", chipId)', guarded)
        self.assertIn("strcmp(line, want) == 0", guarded)
        self.assertIn("CONFIRM_WITHIN_MS", guarded)
        # the burn is only reachable from the confirmation branch
        self.assertEqual(guarded.count("burnAndVerify()"), 2)  # its definition and its one call

    def test_no_key_is_printed(self):
        for n, code, _ in guarded_lines(self.text):
            if re.search(r"Serial\.(print|printf|println|write)\b", code):
                self.assertNotRegex(code, r"\bkey\b|\brec\b|\.key\b", "line %d prints something that could be a key" % n)
        # the only places that hand the record's key on are the signers, the writing call and the self-test
        keys = [n for n, code, _ in guarded_lines(self.text) if re.search(r"\brec\.key\b|\bkey\[", code)]
        self.assertTrue(keys)

    def test_the_key_is_read_from_the_ident_partition_only(self):
        self.assertIn("identstore::load(", self.text)
        for banned in ("WiFi", "HTTPClient", "Serial.read", "argv", "getenv"):
            if banned == "Serial.read":
                continue  # the confirmation line is read from the serial monitor; it is not a key
            self.assertNotIn(banned, "\n".join(c for _, c in code_lines(self.text)))

    def test_the_readme_exists_and_has_no_secret_looking_values(self):
        readme = os.path.join(SKETCH_DIR, "README.md")
        self.assertTrue(os.path.exists(readme))
        text = read(readme).decode()
        self.assertNotRegex(text, r"\b[0-9a-f]{64}\b")  # no key or signature written out
        self.assertIn("BURN_KEY5_FOR_REAL", text)
        self.assertIn("irreversible", text.lower())


if __name__ == "__main__":
    unittest.main(verbosity=1)
