# eFuse HMAC and partition layout on this board (ticket 38), 25 Nov 2026

Question: can the Matrix Portal S3 hold a per-device HMAC key in an eFuse and a small read-only `ident` partition, using only the installed Arduino core and `arduino-cli`? Feeds ticket 37 (decision) and 39 (build slice).

Setup: arduino-esp32 **3.3.13** (core on ESP-IDF **5.5.6**, libs commit `a45de8ccca1`), FQBN `esp32:esp32:adafruit_matrixportal_esp32s3`, arduino-cli 1.6.0, esptool/espefuse **5.3.1** (bundled in the core's `esptool_py` tool), mbedTLS 3.6.7. Everything below was **compiled or run on this laptop only**. The board on the wall was not contacted, nothing was flashed, no serial port was opened, no `arduino-cli upload` was run.

## Answers in one line each
1. **Yes.** `esp_hmac.h`, the eFuse key API and the purpose enums are in the installed core, and a sketch that calls `esp_hmac_calculate(HMAC_KEY0, ...)` and reads every key block's purpose **compiles and links with no extra flags and no hand-written prototype**.
2. **Yes, without moving anything:** shrink `ffat` by 8 KB at its tail and put `ident` (type `data`, custom subtype `0x40`, flag `readonly`) at **0x7FE000, 8 KB**. NVS, otadata, both OTA slots and the TinyUF2 app keep their offsets. A `partitions.csv` in the sketch folder selects it (proved by a compile).
3. **Procedure found and rehearsed** with espefuse's virtual mode: `espefuse --chip esp32s3 --port <port> burn-key BLOCK_KEY5 hmac.key HMAC_UP` burns key, purpose, read protection and write protection in one go (observed). Six key blocks; one burn per block, forever.
4. **Yes, standard HMAC-SHA256** (TRM: "standard HMAC algorithm as described in RFC 2104", key exactly 256 bits). Go `crypto/hmac`, Python and OpenSSL agree on the shared test vector below. Software equivalent: `mbedtls_md_hmac()` from `mbedtls/md.h`, which a sketch can include and link. Latency was **not measured**.

Ranked list of things that change the plan is at the end.

## 1. The HMAC unit and eFuse key API in the core

**Facts (read from the installed files, then proved by compile):**
- Chip support flags, `tools/esp32s3-libs/3.3.13/include/soc/esp32s3/include/soc/soc_caps.h`: `SOC_HMAC_SUPPORTED 1`, `SOC_EFUSE_KEY_PURPOSE_FIELD 1`, `SOC_DIG_SIGN_SUPPORTED 1`, `SOC_EFUSE_BLOCK9_KEY_PURPOSE_QUIRK 1` (comment: XTS-AES key purpose not supported for block 9), `SOC_EFUSE_SECURE_BOOT_KEY_DIGESTS 3`, `SOC_SECURE_BOOT_V2_RSA`, `SOC_FLASH_ENCRYPTION_XTS_AES(_128/_256)`.
- Headers (all under `~/Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.13/include/`): `esp_hw_support/include/esp_hmac.h` (declares `esp_hmac_calculate`, `esp_hmac_jtag_enable/disable`), `hal/include/hal/hmac_types.h` (`hmac_key_id_t`: `HMAC_KEY0`..`HMAC_KEY5` = 0..5), `efuse/include/esp_efuse.h` (read: `esp_efuse_get_key_purpose`, `esp_efuse_get_key_dis_read/write`, `esp_efuse_key_block_unused`, `esp_efuse_find_purpose`, `esp_efuse_count_unused_key_blocks`; write: `esp_efuse_write_key`, `esp_efuse_set_key_dis_read/write`, batch API), `efuse/esp32s3/include/esp_efuse_chip.h` (`EFUSE_BLK_KEY0`=4 .. `EFUSE_BLK_KEY5`=9, `ESP_EFUSE_KEY_PURPOSE_HMAC_UP`=8, `HMAC_DOWN_ALL`=5, `HMAC_DOWN_JTAG`=6, `HMAC_DOWN_DIGITAL_SIGNATURE`=7, XTS 2/3/4, `SECURE_BOOT_DIGEST0..2`=9..11).
- Where the code is: `lib/libesp_security.a` (`esp_hmac.c.obj`: `esp_hmac_calculate`), `lib/libhal.a` (`hmac_hal.c.obj`), `lib/libefuse.a` (`esp_efuse_api_key.c.obj`). `libesp_hw_support.a` has **no** HMAC symbols although the header lives in `esp_hw_support/include` (in IDF 5.5 the implementation is in the `esp_security` component).
- The core's `esp_hmac.c` source equals ESP-IDF at the libs' commit `a45de8ccca1` (diffed against the raw file at that commit: identical).
- The prebuilt libs have `CONFIG_EFUSE_VIRTUAL` **not set**, `CONFIG_MBEDTLS_HARDWARE_SHA=y`, `CONFIG_MBEDTLS_HARDWARE_AES=y`, `CONFIG_MBEDTLS_HARDWARE_MPI=y`, no flash encryption, no secure boot (`sdkconfig` in the libs folder).

**Proof (compile only):**
```
arduino-cli compile --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --build-path <scratchpad>/build_hmac <scratchpad>/hmac_probe
Sketch uses 366038 bytes (17%) of program storage space. Maximum is 2097152 bytes.
```
The probe sketch includes `esp_hmac.h`, `esp_efuse.h`, `esp_efuse_chip.h`, `mbedtls/md.h`; has `static_assert`s on the enum values above; prints for each of `EFUSE_BLK_KEY0..KEY5` the purpose, read-protected, write-protected and unused flags (`esp_efuse_get_key_purpose`, `esp_efuse_get_key_dis_read/write`, `esp_efuse_key_block_unused`), plus `esp_efuse_find_purpose(HMAC_UP)` and the unused-block count; and has a hardware signer `esp_hmac_calculate(HMAC_KEY0, msg, n, out)` next to a software one `mbedtls_md_hmac(...)`. The linked ELF contains `esp_hmac_calculate`, `hmac_hal_*`, `esp_efuse_get_key_purpose`, `esp_efuse_find_purpose`, `esp_efuse_get_key_dis_read`, `mbedtls_md_hmac`. **Nothing extra was needed**: no prototype, no build flag, no library. (The header was present, so the fallback of declaring the prototype myself was not tried.)

A second compile-only probe took the address of the **write** APIs (`esp_efuse_write_key`, `esp_efuse_set_key_dis_read/write`, `esp_efuse_batch_write_begin/commit`, `esp_efuse_find_unused_key_block`) and linked them too (`burn_probe`, 365574 bytes). So a firmware-side burn is possible in this core. The read-only part of the probe (about 15 lines) is enough to inspect a real board's key blocks from firmware.

**Not proven:** that the probe's output is right on the real chip, and that `esp_hmac_calculate` returns `ESP_OK` with a real key. Needs a board.

## 2. Partition table of this board's scheme

**Facts:** `boards.txt` entry `adafruit_matrixportal_esp32s3`, option `PartitionScheme.tinyuf2` ("TinyUF2 8MB (2MB APP/3.7MB FATFS)"): `build.partitions=tinyuf2-partitions-8MB`, `build.custom_bootloader=bootloader-tinyuf2`, `build.bootloader_addr=0x0`, `upload.maximum_size=2097152`, `upload.extra_flags=0x410000 "<core>/variants/adafruit_matrixportal_esp32s3/tinyuf2.bin"`. The CSV is `tools/partitions/tinyuf2-partitions-8MB.csv` in the core, identical to the one in Adafruit's TinyUF2 repo.

| Name | Type, subtype | Offset | Size | End |
| --- | --- | --- | --- | --- |
| 2nd stage bootloader (TinyUF2's, 22.8 KB file) | n/a | 0x0 | up to 0x8000 | |
| partition table (+MD5) | n/a | 0x8000 | 4 KB | 0x9000 |
| nvs | data, nvs | 0x9000 | 20 KB | 0xE000 |
| otadata | data, ota | 0xE000 | 8 KB | 0x10000 |
| ota_0 | app, ota_0 | 0x10000 | 2048 KB | 0x210000 |
| ota_1 | app, ota_1 | 0x210000 | 2048 KB | 0x410000 |
| uf2 (TinyUF2 app) | app, factory | 0x410000 | 256 KB | 0x450000 |
| ffat | data, fat | 0x450000 | 3776 KB | 0x800000 (end of 8 MB) |

- A plain `arduino-cli upload` writes (from `tools.esptool_py.upload.pattern_args`, expanded with `--show-properties`): `0x0` bootloader, `0x8000` partition table, `0xe000` `boot_app0.bin` (resets otadata), `0x10000` app, `0x410000` `tinyuf2.bin`. Erase command is empty, so NVS is not erased: this matches the project note that flashing keeps the saved settings.
- The bootloader is **Adafruit's prebuilt TinyUF2 2nd stage** (`variants/adafruit_matrixportal_esp32s3/bootloader-tinyuf2.bin`, strings show ESP-IDF v5.3.2 and TinyUF2 0.35.0), not the Arduino core's own. TinyUF2 finds its partitions **by type and subtype** (`esp_partition_find_first(APP, OTA_0)`, `(APP, FACTORY)`), not by fixed offsets, and writes UF2 files into ota_0 (TinyUF2 `board_flash.c`, README). It does not reference `ffat`. The repo's firmware uses only `Preferences` (NVS): no FFat, LittleFS or SPIFFS (grep of `arduino/ledmatrix`), so `ffat` is unused today.

**Adding `ident` without moving anything (compiled, table decoded):** sketch-folder `partitions.csv`:
```
nvs,      data, nvs,      0x9000,   20K,
otadata,  data, ota,      0xe000,   8K,
ota_0,    app,  ota_0,    0x10000,  2048K,
ota_1,    app,  ota_1,    0x210000, 2048K,
uf2,      app,  factory,  0x410000, 256K,
ffat,     data, fat,      0x450000, 3768K,
ident,    data, 0x40,     0x7FE000, 8K,    readonly
```
Decoded with the core's `gen_esp32part.py` from the built `partitions.bin` and compared byte by byte with the stock build: nvs, otadata, ota_0, ota_1 and uf2 entries are **identical**; only `ffat` changes (3776K to 3768K, `0x450000+0x3AE000 = 0x7FE000`) and `ident` is new. Two 4 KB sectors, 4 KB aligned.
- **Settings (NVS):** same offset and size, so the saved settings stay. They would only be lost if `nvs` were moved or resized.
- **Why not the 8 KB gap at 0x6000-0x8000** (free between the 22.8 KB bootloader and the table): `gen_esp32part.py` rejects a partition whose offset is "below" the end of the partition table (0x9000), so the table cannot describe it, and a newer TinyUF2 release may grow the bootloader into it.
- **Custom subtype in `data`:** `readonly` is documented only for data-type partitions (ESP-IDF partition tables guide). Custom *types* 0x40-0xFE are reserved for apps, but a custom *subtype* under `data` compiled and decoded fine (`ident,data,64,0x7fe000,8K,readonly`). In C++ the subtype needs a cast to `esp_partition_subtype_t`.
- `readonly` effect: `esp_partition_write/erase` return `ESP_ERR_NOT_ALLOWED` (`esp_partition.h`); the guide says lower-level `spi_flash` writes also fail. The libs set `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y`, so a raw flash write may abort instead of returning an error. Enforced in software only, not by hardware. Not run.

**Selecting the table with arduino-cli.** `platform.txt` prebuild hooks copy, in increasing priority: `tools/partitions/{build.partitions}.csv`, then `variants/<variant>/{build.custom_partitions}.csv`, then **`<sketch folder>/partitions.csv`** (comment in platform.txt: "build.partitions < variant < source"). So: put `arduino/ledmatrix/partitions.csv` next to the `.ino`. Proved: the build used it (`build/partitions.csv` is a copy of mine, `partitions.bin` decodes to it). `--build-property build.partitions=<name>` only selects a CSV inside the core install, and the board's menu has no entry with `ident`, so the sketch-folder file is the only repository-local way. A bench copy of the sketch folder carries the file along.

**Reading it from firmware (compiled):**
```cpp
const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "ident");
esp_partition_read(p, sector * 4096, &rec, sizeof(rec));   // sector 0 and 1; take the first copy whose CRC is good
```
CRC32: `esp_crc32_le` from `esp_crc.h` compiles. The probe checks magic, length and CRC of both copies and attempts a write to show `ESP_ERR_NOT_ALLOWED`.

**Writing it at flash time.**
- esptool (5.3.1) one-off, for reference only: `esptool --chip esp32s3 --port <port> write-flash 0x7FE000 ident.bin` where `ident.bin` is 8192 bytes (sector 0 and sector 1 holding the two copies, padded with 0xFF). **Project rule: flash only with `arduino-cli upload`; esptool once left the board in download mode.** So the esptool command is not the recommended path.
- Through `arduino-cli upload` (flag exists in 1.6.0, `--upload-property`): override `upload.extra_flags` so the esptool line gets one more offset/file pair, for example `--upload-property 'upload.extra_flags=0x410000 "<core>/variants/adafruit_matrixportal_esp32s3/tinyuf2.bin" 0x7FE000 "<dir>/ident.bin"'`. I checked with `--show-properties` that the property expands into the `write-flash` line as `... 0x410000 "<core>/.../tinyuf2.bin" 0x7FE000 "/path/to/ident.bin"`. **I did not run an upload**, so that the upload-time override behaves the same is inferred, not proven.
- The core's `tools/flasher.py` (wrapper used by `upload`) adds `--diff-with` for fast reflashing. esptool's help says unchanged files are still MD5-checked unless `--trust-flash-content` is given (not given), so a stale reference should not skip a needed write. Not exercised.
- A later plain `arduino-cli upload` does not list `0x7FE000`, and esptool erases only the sectors it writes (inference from how write-flash works), so `ident` should survive normal uploads. It must still stay in the **partition table**, which every upload rewrites: every build needs the sketch-folder `partitions.csv`.

## 3. The eFuse procedure

### Facts about the chip (TRM v1.8 chapters 5 and 21; ESP-IDF 5.5 eFuse and HMAC guides)
- 11 blocks. **Six key blocks: BLOCK4..BLOCK9 = KEY0..KEY5**, each 256 bits, each with a 4-bit purpose field in BLOCK0 (`KEY_PURPOSE_n`).
- A programmed bit never returns to 0. BLOCK1-10 use RS(44,32) coding, "each block can only be written once" (TRM 5.3.1.3, 5.3.2).
- Read protection (`RD_DIS` bit n) hides the block from software and espefuse, but the hardware crypto units can still use it (TRM 5.3.1.2). Write protection is `WR_DIS` bits (block and purpose field each have one).
- HMAC upstream needs purpose **8** (`HMAC_UP`); the unit refuses (`HMAC_QUERY_ERROR`) if the selected block's purpose differs (TRM 21.2.5). The TRM tells you to read-protect the key and "a copy of this key should be kept by any party who needs to verify this device".
- What else wants key blocks: flash encryption (XTS_AES_128 = 1 block, XTS_AES_256 = 2 blocks, **not usable in BLOCK9** per the IDF guide and `SOC_EFUSE_BLOCK9_KEY_PURPOSE_QUIRK`), Secure Boot v2 digests (1 block each, up to 3), Digital Signature key (`HMAC_DOWN_DIGITAL_SIGNATURE`), soft-JTAG key (`HMAC_DOWN_JTAG`). Secure boot and flash encryption are not planned, and nothing in the Arduino core or TinyUF2 uses a key block by default.

### Read-only inspection of a real board (do NOT run during this ticket)
```
espefuse --chip esp32s3 --port <port> summary                   # read-only
espefuse --chip esp32s3 --port <port> summary --format json --file efuses.json
espefuse --chip esp32s3 --port <port> dump                      # raw words
espefuse --chip esp32s3 --port <port> check-error
```
These need the chip in ROM download mode and default to `--after hard-reset`, which is exactly the esptool path that once left this board in download mode. **Safer: read the same fields from firmware** (the compiled probe in section 1), for example into `/status` or the serial log of a normal build flashed with `arduino-cli upload`. The summary line to look at: `KEY_PURPOSE_0..5`, `BLOCK_KEY0..5` (`Purpose:` and the bytes), `RD_DIS`, `WR_DIS`.

### What a factory-fresh Adafruit board is likely to show
- **Likely:** `KEY_PURPOSE_0..5 = USER (0x0)`, all six `BLOCK_KEY*` zero and `R/W`, `RD_DIS = 0`, `WR_DIS = 0`, `SECURE_BOOT_EN = False`, `SPI_BOOT_CRYPT_CNT = Disable`, `BLOCK_USR_DATA` zero; BLOCK1 holds the MAC.
- **Evidence:** the esptool docs show exactly this for an ESP32-S3 (`espefuse summary` page); TinyUF2 (Adafruit's bootloader for this board, checked at upstream commit `5684ecf`, 25 Nov 2026) only **reads** eFuses (factory MAC, ADC calibration), has no eFuse write call and no secure boot or flash encryption setting; CircuitPython/Arduino flows on this board do not burn eFuses either (not exhaustively checked for CircuitPython). The ESP-IDF definition of an unused block is zero content, no read/write protection, purpose USER (`esp_efuse_key_block_unused`).
- **Not found:** an `espefuse summary` from this Adafruit board or from an ESP32-S3-WROOM-1 module (searched; one forum thread was behind a bot challenge and could not be read). So this is an expectation, to be confirmed by the firmware read. The sample in the docs is not Adafruit's.

### The burn command and what it does
```
espefuse --chip esp32s3 --port <port> burn-key BLOCK_KEY5 hmac.key HMAC_UP
```
- `hmac.key` is a raw 32-byte file. The tool prompts and needs `BURN` typed in capitals. Do not add `--do-not-confirm` or `--show-sensitive-info`. Delete the key file afterwards; never commit it.
- In 5.3.1 the command is `burn-key` (the underscore form `burn_key` still works but prints a deprecation warning).
- **Read protection:** observed in the rehearsal below: for `HMAC_UP` the tool plans and applies "Disabling read to key block" and "Disabling write to key block" by default, and always write-protects the purpose field. `--no-read-protect` would leave the key readable and must not be used. Note that the esptool docs page says "Only flash encryption key is read protected if --no-read-protect is not used" in one paragraph and "read-protect by default" for the other key purposes in another; the installed 5.3.1 behaved as the second. Check the summary after every real burn.
- ESP-IDF's firmware API does the same in one batch: the header note on `esp_efuse_write_key` says it "also enables the read protection efuse bit for certain key blocks like XTS-AES, HMAC, ECDSA".

### What is irreversible
Everything the command touches: the 256 key bits (no rewrite, no rotation of that block), the purpose field (write-protected), `RD_DIS` for that block, `WR_DIS` for the block and the purpose. The key can never be read, changed or removed, by anyone, including the owner. A second HMAC key can be burned into another free block, and firmware can be pointed at it with a different `HMAC_KEYn`, but the first block stays used.

### What could brick the board, and what only wastes a block
- **Bricks or cuts the repair path (never part of this procedure):** burning `SECURE_BOOT_EN`, `SPI_BOOT_CRYPT_CNT` (flash encryption), `DIS_DOWNLOAD_MODE`, `DIS_USB_OTG_DOWNLOAD_MODE`, `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE`, `DIS_USB_OTG`, USB pin or PHY bits, `VDD_SPI_*` and `SPI_PAD_CONFIG_*` (flash voltage and pins; the tool has a separate `set-flash-voltage` command). The rehearsal shows `burn-key ... HMAC_UP` changes only the key block, `KEY_PURPOSE_n`, `RD_DIS` and `WR_DIS` (`WR_DIS` 0x00800100, `RD_DIS` 1 for KEY0; 0x10002000 and 0x20 for KEY5, matching TRM tables 5.3-1 and 5.3-3). From this I infer it cannot affect boot or download mode.
- **Wastes a block, does not brick (inference from the TRM layout):** a wrong purpose in the same command (for example `XTS_AES_128_KEY` or `SECURE_BOOT_DIGEST0`; the enabling bits are separate eFuses), a key made from the wrong ID, a lost or wrong master secret. A torn burn (power loss mid-write) can leave the block unusable. `HMAC_UP` is the single bit value 8, so a half-programmed purpose can only read as USER or HMAC_UP (my inference from the 4-bit field).
- **Locks the board out of the data service for good:** wrong key bytes, or a lost master secret (already in ticket 37). Not a chip brick: the design lets unsigned requests fetch firmware.
- **Tooling:** the espefuse docs page and the installed 5.3.1 differ (docs list `summary --active`, 5.3.1 has only `--format`, `--file`). Read `espefuse <command> --help` of the installed version before any real run.

### Complete rehearsal with espefuse's virtual eFuse mode (actually run, 25 Nov 2026)
Binary: `~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/espefuse` (no pip install needed). `--virt` works without a chip; `--path-efuse-file` keeps the virtual eFuses between commands (delete the file to reset). No port was given. The key is 32 bytes derived from a **placeholder** master secret and a placeholder ID.

Fresh virtual chip, summary: `KEY_PURPOSE_0..5 = USER R/W`, all `BLOCK_KEY0..5 = 00 .. 00 R/W`, `WR_DIS = 0 R/W`, `RD_DIS = 0 R/W`. Then, answering nothing at the prompt (nothing burned, file unchanged):
```
$ espefuse --chip esp32s3 --virt --path-efuse-file efuse.bin burn-key BLOCK_KEY0 key.bin HMAC_UP < /dev/null
=== Run "burn-key" command ===
Sensitive data will be hidden (see --show-sensitive-info)
Burn keys to blocks:
 - BLOCK_KEY0 -> [?? ?? ?? ?? ... ?? ??]
	'KEY_PURPOSE_0': 'USER' -> 'HMAC_UP'.
	Disabling write to 'KEY_PURPOSE_0'...
	Disabling read to key block...
	Disabling write to key block...
Check all blocks for burn...
[00] BLOCK0               is empty, will burn the new value
[04] BLOCK_KEY0           is empty, will burn the new value
This is an irreversible operation!
Type 'BURN' (all capitals) to continue.
Aborted.
```
Then with `BURN` typed:
```
BURN BLOCK4  - OK (write block == read block)
BURN BLOCK0  - OK (write block == read block)
Burn postponed eFuses from BLOCK0.
BURN BLOCK0  - OK (all write block bits are set)
Reading updated eFuses...
Successful.
```
Summary afterwards (read-only):
```
WR_DIS (BLOCK0)  = 8388864 R/W (0x00800100)
RD_DIS (BLOCK0)  = 1 R/W (0b0000001)
KEY_PURPOSE_0 (BLOCK0)  Purpose of Key0  = HMAC_UP R/- (0x8)
KEY_PURPOSE_1..5                         = USER R/W (0x0)
BLOCK_KEY0 (BLOCK4)  Purpose: HMAC_UP
   = ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??  -/-
```
`summary --format value_only KEY_PURPOSE_0` printed `HMAC_UP`; `check-error` printed "No errors detected."; `dump` shows `BLOCK_KEY0` as zeros (read-protected) and BLOCK0 words `00800100 00000001 08000000 ...`. A second burn into the same block was **refused**:
```
A fatal error occurred: 	BLOCK_KEY0 is read-protected. The written value can not be read, the eFuse/block looks as all 0.
	Burn in this case may damage an already written value.(use '--force-write-always' option to ignore it)
```
Same procedure into **BLOCK_KEY5** on a fresh virtual chip: `KEY_PURPOSE_5 = HMAC_UP R/-`, `WR_DIS = 0x10002000`, `RD_DIS = 0b0100000`, key shows `??`, `Successful`.

**What the rehearsal does not cover:** the virtual mode emulates only the eFuse controller. It cannot run the HMAC unit, cannot test `esp_hmac_calculate` against the burned key, and cannot rehearse the **firmware** burn path (`esp_efuse_write_key`), because the prebuilt Arduino libs have `CONFIG_EFUSE_VIRTUAL` off. The firmware path can only be tested on a real chip.

## 4. Is it standard HMAC-SHA256, limits, latency, software equivalent

**Facts:**
- TRM chapter 21: "Standard HMAC-SHA-256 algorithm"; structure "is the standard HMAC algorithm as described in RFC 2104" (ipad 0x36, opad 0x5c); "the 256-bit HMAC key". The key is 256 bits, zero-extended to the 512-bit block. For RFC 2104 a key shorter than the block is zero-padded, so a **32-byte key** gives identical output to any standard implementation. The planned key `HMAC-SHA256(master, id)` is exactly 32 bytes.
- `esp_hmac_calculate` (source above) does the SHA padding in software and writes 64-byte blocks to the unit; **no message length limit** in the code beyond `size_t` and the 64-bit bit-length field; time grows linearly. It takes `s_crypto_hmac_lock` **and** the shared SHA/AES lock for the whole call (`esp_crypto_lock.c`), so hardware-accelerated TLS hashing on the other core waits meanwhile. Needs the key block purpose to be 8; wrong purpose returns `ESP_FAIL`.
- Latency: **no figure in the TRM or the ESP-IDF guide, and I did not measure** (no hardware allowed). Expectation only: a request signature is a few SHA-256 blocks of hardware work (the vector below is 135 bytes, three message blocks plus the key blocks), far below the 2.3 s cold TLS handshake in `arduino-milestone.md`; measure in slice 39 with `esp_timer_get_time()` around a loop of calls.
- **Alignment (inference from reading the HAL, not tested):** `hmac_ll_write_block_512(const uint32_t *block)` reads `block[i]` as 32-bit words, and `esp_hmac_calculate` passes the caller's `message` pointer straight through for the full blocks of a message of 56 bytes or more; `hmac_ll_read_result_256(uint32_t *)` writes the output the same way. On Xtensa an unaligned 32-bit access faults, so copy the canonical string into a `alignas(4)` buffer and give an `alignas(4)` 32-byte output buffer.
- Software equivalent for the interim signer: `mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, msg, len, out)` from `mbedtls/md.h` (3.6.7). **Available to a sketch:** the probe includes it and links `mbedtls_md_hmac`; `mbedtls_md_hmac_starts/update/finish` are linked as well for streaming. mbedTLS SHA here uses the hardware accelerator (`CONFIG_MBEDTLS_HARDWARE_SHA=y`).
- Go side: `hmac.New(sha256.New, key)` and **`hmac.Equal`** (constant time), after hex-decoding the received signature.

### Known-answer test vector (placeholders only; computed with Python `hmac`)
All values below were computed with Python 3 `hmac`/`hashlib`, then checked with OpenSSL (`dgst -sha256 -hmac`/`-mac HMAC`) and with Go `crypto/hmac` (Go 1.25 in an offline container, all 14 checks OK).

1. **Primitive anchor (RFC 4231 test case 2):** key `4a656665` ("Jefe"), message `what do ya want for nothing?` -> `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`.
2. **Key derivation:** master `example-master-secret` (ASCII, placeholder), device ID `aabbccddeeff` (lowercase hex **text**, placeholder), key = HMAC-SHA256(master, id):
   `daca07112685934c42e92f75ba827c66b1fca25bb7304c5f856fffc2051fb604`.
   If the slice defines the ID as the **6 raw bytes** instead, the key is `855fc0dd8fe2521f1be20a95ec04cbf4202653f0193c1c1af41da9edfd8dfc94`. The slice must fix which one (and the case) in one place.
3. **Request signature** (illustrative canonical layout: lines joined with `\n`, no trailing newline: scheme version, method, host, path and query, timestamp, hex SHA-256 of the body). Body is empty, so its hash is `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
   ```
   v1
   GET
   ledmatrix.example.com
   /api/flights?lat=38.7&lon=-9.1
   1796000000
   e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
   ```
   Message length 135 bytes, hex:
   `76310a4745540a6c65646d61747269782e6578616d706c652e636f6d0a2f6170692f666c69676874733f6c61743d33382e37266c6f6e3d2d392e310a313739363030303030300a65336230633434323938666331633134396166626634633839393666623932343237616534316534363439623933346361343935393931623738353262383535`
   Signature (key from item 2): **`32ab07d3dba934874efe58d1afb1201c753114b7d5e7a98c8f6c8c17a9d08562`**
4. **Length boundaries** for the hardware signer (key from item 2; message byte i = `(i*7+3) & 0xff`; these hit every branch of `esp_hmac_calculate`: one block up to 55 bytes, exact multiples of 64, remainder of 56 or more):

| Length | HMAC-SHA256 |
| --- | --- |
| 0 | `9d5d660b5242de1b978a39ee07f542822cc89e568d3d42465befadbd1250789a` |
| 1 | `47181c81ef314c472267c9ea59e0781df517256c4465c1ecde972b33f2b3f4d6` |
| 55 | `97177663ef3cfea61e36c462eb05c5d7f1cf870a772ae698a71b47cf1159940d` |
| 56 | `429ab55e5a986916cbc3ad64f185e10ca9a5e04c010c889fecd06cc44dd51114` |
| 63 | `02dc50f13a16e72f090afd0e29ba55258ad7a4d6ff5cbf7d9cd2950be33fcbbb` |
| 64 | `2b0ceaeecf02b59f1b51ec7893fb4561073668e2836ceda100f5a95f2c0d6e5b` |
| 65 | `704d2c81e4ada1200b044a74e720cb4e807ff5211d386850ed063ffcbf8650d2` |
| 119 | `ce39af2fc0bd1358ddb8e9d23633d058efbde99c1cb04e439284c21eea8835e3` |
| 120 | `49da4234f519348487b165451202969c29d7753c4f7d7f76462800cfab4b1d3d` |
| 128 | `d2415477b6fcdab002b6d6aded7984ae948f25481b2c2e9e2b00848343105190` |
| 200 | `43cecb168cf10a462e062849b5efbac8963abc717b09d7e6b9974cbe754247d8` |

The C++ unit test can run the software signer on a host or the board, and later the hardware signer on the board, against the same table: one key, same bytes, same answer.

## What changes the plan in tickets 37 and 39 (ranked)

1. **Trap, affects 37 point 6 and ticket 21: rollback is unproven on this board.** The bootloader that runs is TinyUF2's prebuilt one (ESP-IDF v5.3.2), not the Arduino core's. Upstream TinyUF2's sdkconfig sets no `BOOTLOADER_APP_ROLLBACK_ENABLE` (ESP-IDF default is off; searched the whole `ports/espressif` tree for "rollback": nothing) and the installed binary has no rollback strings. Separately, the core's `initArduino()` marks a pending image valid **before `setup()`** unless the sketch defines `verifyRollbackLater()` returning `true` (`esp32-hal-misc.c`). So "the new version needs one good signed fetch or the board falls back" will not happen by itself. Needs either a bench test of the real bootloader or an application-level fallback (a boot counter in NVS and `esp_ota_set_boot_partition` back to the previous slot). The floor that does exist for repair: TinyUF2 UF2 mode and ROM download mode with the USB cable. Evidence is negative (absence), so this is "likely not available", not proven.
2. **Trap, 39: provisioning `ident` and the flash rule collide.** A normal `arduino-cli upload` never writes 0x7FE000. Either use `--upload-property upload.extra_flags=... 0x7FE000 ident.bin` (flag exists, expansion checked, **not run**) or break the esptool rule. The first real step of 39 should be one bench upload that writes the new table and `ident` together and is then checked by reading `ident` back from firmware. Every later build also needs `partitions.csv` in the sketch folder or `ident` falls out of the table (the data would stay in flash).
3. **Decision for the owner: how to burn.** Host `espefuse burn-key` can be rehearsed (done above) but needs ROM download mode and a reset afterwards, where esptool already misbehaved once on this board; recover with an `arduino-cli upload` and have the cable ready. Firmware `esp_efuse_write_key` links in this core, but **cannot be rehearsed** (`CONFIG_EFUSE_VIRTUAL` is off) and puts burning code in a firmware image. A one-off burn sketch flashed with `arduino-cli upload`, reading the key from `ident` and requiring a button press, fits the project rule better, but only a spare chip could test it first.
4. **Better option: burn into BLOCK_KEY5 (`HMAC_KEY5`) rather than KEY0.** KEY5 cannot hold a flash-encryption key (IDF guide and `SOC_EFUSE_BLOCK9_KEY_PURPOSE_QUIRK`), so it is the cheapest block to give away; KEY0-4 stay free for XTS-AES-256 (two blocks) plus secure boot digests if the owner ever wants them. The virtual rehearsal into KEY5 worked.
5. **Trap, 39: alignment of buffers for the hardware signer** (copy the message into an `alignas(4)` buffer, `alignas(4)` output). Inference from the HAL source, to be proven by the length table above on the board.
6. **Inspect the real board from firmware, not espefuse.** The compiled probe reads all key blocks read-only. Do this before the burn to confirm KEY5 is unused (factory state is expected, not verified).
7. **Confirmations (no change):** the 32-byte derived key fits the unit exactly; software and hardware produce the same bytes, so the server needs no change at the burn step; no existing partition moves; NVS settings survive; `ffat` is not used by the firmware or by TinyUF2.

## Not verified

- Anything on the real chip: eFuse state, `esp_hmac_calculate` return value and timing, the `readonly` flag at run time, unaligned-pointer behaviour.
- `arduino-cli upload --upload-property` with the extra `ident` pair (property expansion only).
- Whether the TinyUF2 binary shipped in the core has rollback (inferred from source defaults and the absence of strings in the binary).
- Factory eFuse contents of this specific board.
- TinyUF2 source was read at upstream commit `5684ecf` (25 Nov 2026), the installed binary is release 0.35.0.
- Web documentation of espefuse is newer than the installed 5.3.1 (an option differed); behaviour above was taken from the installed binary's help and virtual runs.

## Sources

Core and tools on this machine (all under `~/Library/Arduino15/packages/esp32/`):
- `hardware/esp32/3.3.13/boards.txt` (entry `adafruit_matrixportal_esp32s3`), `platform.txt` (partition hooks, upload patterns), `tools/partitions/tinyuf2-partitions-8MB.csv`, `tools/gen_esp32part.py`, `tools/flasher.py`, `cores/esp32/esp32-hal-misc.c` (rollback hooks), `variants/adafruit_matrixportal_esp32s3/` (`bootloader-tinyuf2.bin`, `tinyuf2.bin`).
- `tools/esp32s3-libs/3.3.13/`: `versions.txt`, `sdkconfig`, `include/soc/esp32s3/include/soc/soc_caps.h`, `include/esp_hw_support/include/esp_hmac.h`, `include/hal/include/hal/hmac_types.h`, `include/hal/esp32s3/include/hal/hmac_ll.h`, `include/efuse/include/esp_efuse.h`, `include/efuse/esp32s3/include/esp_efuse_chip.h`, `include/esp_partition/include/esp_partition.h`, `include/mbedtls/mbedtls/include/mbedtls/md.h`, `lib/libesp_security.a`, `lib/libhal.a`, `lib/libefuse.a`.
- `tools/esptool_py/5.3.1/espefuse` and `esptool` (`--help` texts, virtual runs).

Espressif and Adafruit (read 25 Nov 2026):
- ESP32-S3 Technical Reference Manual v1.8, chapter 5 eFuse Controller and chapter 21 HMAC Accelerator: https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf
- ESP-IDF v5.5 HMAC guide: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/hmac.html
- ESP-IDF v5.5 eFuse Manager: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/efuse.html
- ESP-IDF v5.5 partition tables guide: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/partition-tables.html
- esptool docs, espefuse index, burn-key and summary (latest, ahead of 5.3.1): https://docs.espressif.com/projects/esptool/en/latest/esp32s3/espefuse/index.html
- ESP-IDF source at the libs' commit: `components/esp_security/src/esp_hmac.c`, `esp_crypto_lock.c`, `components/hal/hmac_hal.c` (https://github.com/espressif/esp-idf, commit `a45de8ccca1`)
- TinyUF2 (Adafruit): https://github.com/adafruit/tinyuf2, files `ports/espressif/README.md`, `boards/board_flash.c`, `boards/boards.c`, `bootloader_components/main/double_reset_adc.c`, `partitions-8MB.csv`, `sdkconfig.defaults*`, `boards/adafruit_matrixportal_s3/`
- RFC 2104 and RFC 4231 for the HMAC definition and the anchor vector.
