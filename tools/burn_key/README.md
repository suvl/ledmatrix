# Burning the board's key into the chip (one-off)

Today the board signs its requests with a key saved in the read-only `ident` partition. This step puts the same key into the chip's eFuse, where the chip's HMAC unit can use it but no program, no flash tool and no person can ever read it back. After it, the software key can be removed from flash.

**This is irreversible.** Once the key is burned it cannot be changed, read, or removed, by anyone. Read this page to the end before you start. Every name, ID and path below is a placeholder.

## What the sketch does

`burn_key.ino` is a separate sketch. It reads the board's key **from the `ident` partition** (so no key ever touches a file on your computer, a command line or the network) and burns it into `BLOCK_KEY5` with the purpose `HMAC_UP`, with read protection and write protection on. It never prints a key.

- **The default is a dry run.** It runs every check, prints what it would do, and writes nothing. A dry-run build does not even contain the code that writes.
- **A real burn needs three things together:**
  1. a build with `-DBURN_KEY5_FOR_REAL=1` **and** `-DBURN_DEVICE_ID="<12 hex>"`, where the ID is this board's own address (the sketch refuses to run if it is not; one build is for one board),
  2. every check passing,
  3. you typing `BURN <the 12 hex characters>` in the serial monitor within 3 minutes after the checks passed.

  A reset, a wrong word or waiting too long burns nothing.

## Before you start

- **The board runs this firmware version and works with its current `ident` record.** Flash the firmware from this repository first (it has the hardware signer, which the burn step needs) and confirm that data still flows: `/status` says `"identity":"ok"`, `"signer":"software"` and `"signer_check":"not_run"`, `/api/problems` is empty, and `/api/selftest` shows the service line OK. **This is the only protection against burning a wrong key.** The burn sketch cannot tell whether the key in the record is the one the service derives from the master secret (it has no master secret). If the record was made with the wrong secret, the service refuses the board today, and burning that key would make the mistake permanent. Do not start unless the board is accepted by the service now.
- **The master secret has an offline backup.** After the burn the board's key exists only in the chip and in what the master secret can derive. If the master secret is lost, the board is locked out of the data service for good.
- The board is on USB, you know its serial port (`<port>`, for example `/dev/cu.usbmodem1234`), and the cable is solid. Do not touch the cable during the burn.
- You have the normal firmware's build directory ready (`<build dir>`), or can compile it.

## The steps

All commands run from the repository root. Use a build directory outside the repository (`<scratch>`). Flash only with `arduino-cli`.

**1. Dry run.** Compile and upload the sketch without any extra flag, and read its output.

```
arduino-cli compile --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --build-path <scratch>/build_burn tools/burn_key
arduino-cli upload  --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --port <port> --input-dir <scratch>/build_burn tools/burn_key
arduino-cli monitor --port <port> --config baudrate=115200
```

This replaces the running firmware with the burn sketch (the display shows nothing and the board is offline while it runs). Settings and the `ident` partition are kept: the sketch folder has its own copy of `partitions.csv`, identical to the firmware's, and a plain upload never writes `0x7FE000`.

**2. Read the dry-run output.** It looks like this:

```
BURN burn_key: DRY RUN. Every check runs, nothing is ever written.
BURN key block KEY0: purpose USER      unused yes read-protected no  write-protected no
   ... one line for each of KEY0 to KEY5 ...
BURN check the chip's own address can be read           ok
BURN chip address aabbccddeeff
BURN check the ident partition holds a record           ok
BURN check the record is for this chip                  ok
BURN check the record is valid (magic, version, CRC, ID) ok
BURN check the record does not say "burned" yet         ok
   ... then: a real key, software HMAC, BLOCK_KEY5 unused, purpose can be set, no HMAC key yet, an unused block ...
BURN DRY RUN done: every check passed. Nothing was written.
```

- The key block lines are the state of the six eFuse key blocks: their purpose, whether each is **unused** (all zero, no protection, purpose `USER`) and whether it is read or write protected. On a factory-fresh board all six are expected to say `purpose USER` and `unused yes` (the research expects this; it was not confirmed on this board before the dry run).
- Every check is `ok` or `FAILED`. The first failure stops the sketch with one plain sentence that says why, and `Nothing was written.` Fix that and run again. Nothing is ever half done at this stage.
- The last lines repeat every 15 seconds, so a monitor opened late still shows how it ended.
- If anything is `FAILED`, or any of KEY0 to KEY5 is not `unused yes` when you expected it to be, **stop and ask**. Do not go on to the real burn.

**3. The real burn.** Compile with both flags (the quoting below is the one that works with `arduino-cli`; put the board's own ID from the dry run's `chip address` line), upload, and open the monitor:

```
arduino-cli compile --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --build-path <scratch>/build_burn_real \
  --build-property "compiler.cpp.extra_flags=-DBURN_KEY5_FOR_REAL=1 '-DBURN_DEVICE_ID=\"aabbccddeeff\"'" tools/burn_key
arduino-cli upload  --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --port <port> --input-dir <scratch>/build_burn_real tools/burn_key
arduino-cli monitor --port <port> --config baudrate=115200
```

The sketch runs the same checks. When all pass it prints `all checks passed` and asks for `BURN aabbccddeeff`. Type exactly that and press Enter. Then it writes, reads everything back and compares the chip's answers with the saved key's, and prints either `BURN verified` or one plain failure sentence. Do not remove power until it says so.

**3b. If the sketch says the chip's answers differ.** The burn is done and cannot change, but the sketch could not prove that the chip signs like the saved key (`BURN FAILED: the chip's HMAC gives different answers ...`). Do not run step 5. Reset the board (unplug and plug it in) and go to step 4: the firmware repeats the comparison at boot, with a fresh start of the chip's HMAC unit, and keeps signing with the saved key if the answers still differ. Read `signer_check` on `/status` afterwards:
- `match`: it was a start-up problem of the unit right after the burn. Go on with step 4's checks, then step 5.
- `mismatch`: the chip's key is not the saved key (for example, the key bytes reached the unit in another byte order). The board keeps working with the saved key and says so in the problem list. **Stop and ask.** Do not run `provision.py --burned`.
- `unprotected`: see "If the power fails during the burn".

**4. Put the normal firmware back and check it.** The sketch prints these steps too.

```
arduino-cli compile --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --build-path <scratch>/build arduino/ledmatrix
arduino-cli upload  --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --port <port> --input-dir <scratch>/build arduino/ledmatrix
```

Open `http://<the board>/status`. You want `"identity":"ok"`, `"signer":"hardware"` and `"signer_check":"match"`: the firmware signed the same test messages with the chip's key and with the saved key, got the same answers, and now signs with the chip. Check that data arrives (`/api/problems` is empty). If `signer_check` says `mismatch`, the firmware keeps signing with the saved key, says so in the problem list, and never uses the chip's key: stop and ask.

**5. Remove the software key from flash.** Only after step 4 shows `hardware` and `match`:

```
python3 -I tools/provision.py --burned --from-board <the board> --out <file outside the repository> \
  --flash --port <port> --input-dir <scratch>/build
```

This writes the record with flag bit 0 set and the key all zero (no master secret needed), flashes it with the normal firmware, and deletes the image file. With `--from-board` it asks `/status` first and refuses unless the board already signs with the chip (`signer: hardware`) and the comparison matched (`signer_check: match`); that gate is what stops a keyless record from reaching a board whose chip key does not work. Afterwards `/status` should say `"signer":"hardware"` and `"signer_check":"not_run"` (there is no software key left to compare with), and data still arrives.

`--device-id <id>` in place of `--from-board` **skips that gate**. Use it only if the board cannot be reached over the network, and only after you have checked `/status` by hand (`signer: hardware`, `signer_check: match`) while it could.

## What is irreversible, and what is not

- **Irreversible:** the key bits in `BLOCK_KEY5`; its purpose; read protection; write protection. Nothing can change them again. `BLOCK_KEY5` is used up for good.
- **A "wasted block"** is a key block that was written wrongly (the wrong key, the wrong purpose, or interrupted halfway) and so can never be used or rewritten. There are six key blocks on the chip (`KEY0` to `KEY5`); nothing else in this project uses one. A wasted `BLOCK_KEY5` does not break the board: the software key in the `ident` partition keeps working, because the firmware only switches to the chip when the key is there, fully protected, and its answers match. Using another block later means changing `KEY_BLOCK` and `KEY_ID` in `arduino/ledmatrix/signers.h`, the one place that names them (a `static_assert` there keeps the pair pointing at the same key, and another one in `burn_key.ino` stops the build until its `BLOCK_KEY5` texts are updated).
- **Not irreversible:** the sketches and firmware (reflash any time with `arduino-cli`), the `ident` record (provision again, with or without `--burned`), the settings. The master secret recreates the same software key for the same board, so a board whose chip has no key can always be provisioned again with `provision.py` and no flag.
- **Not possible after the burn:** changing the board's key (a new master secret would not reach this board's chip), or reading the key out. The service keeps working because it derives the same key from the master secret.

## If the power fails during the burn

The write is one eFuse batch (key, purpose and protections together), a fraction of a second. If power is lost during it, the chip can be left in a state nobody has seen on this board. The reasoning, not a measurement: eFuse bits only ever go from 0 to 1, so the result is some of the bits set.

- **Nothing was written** (the usual case if it happens before the write line): `BLOCK_KEY5` is still unused; run the burn again.
- **Partly written:** `BLOCK_KEY5` is then no longer unused, so the sketch refuses to write again (it never writes twice), and a dry run shows the block's state on its `KEY5` line. The normal firmware uses the chip's key only when all of this is true: the block holds an HMAC key, read protection, write protection and the purpose's write protection are all set, and the chip's answers equal the saved key's for every test message. Otherwise it keeps signing with the saved key: `signer_check` on `/status` says `unprotected` (a protection is missing) or `mismatch` (the answers differ), and the problem list says so in plain words. A burned record (no software key) on such a chip makes the board unable to sign (`identity` says `hardware_key` or `hardware_unprotected`): provision it again with `provision.py` and no flag. The block is wasted; the owner decides what to do next. Do not run `provision.py --burned` in this state.

The record is never changed by this sketch, so the board is never left without a way to sign.

## What the first real burn showed

The first (and, for this project, only so far) burn was run on the owner's board on 25 Nov 2026. It worked the first time:

- All twelve checks passed, the confirmation was typed, and the sketch printed `BURN verified`: the block read back as an HMAC_UP key with read protection, write protection and purpose write protection all set. The sketch did not need to add any missing protection (it printed no "adding the missing" line), so `esp_efuse_write_key` in this core sets all three itself.
- **Byte order:** the chip's answers equalled the saved key's for all 11 test lengths (0 to 200 bytes) straight away, so the key bytes need no reversal for the HMAC unit.
- **Right after the burn, without a reset:** the comparison already passed in the sketch, so the unit sees the key immediately.
- After flashing the normal firmware, `/status` said `signer: hardware`, `signer_check: match`, the owner's real service accepted the chip-signed requests, and after `provision.py --burned` the board kept signing with the chip alone (`signer: hardware`, `signer_check: not_run`).

What stays unverified is only what a second board or a power cut could show: the state after a power loss in the middle of the write (see above), and `BLOCK_KEY5` on any other board (each board is checked by its own dry run first).

