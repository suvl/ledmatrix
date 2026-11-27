# Build the firmware

Checked on macOS on 25 and 26 Nov 2026: what was run and what was not is in the table at the end of this page.

This page gets your computer ready and builds the [firmware](glossary.md#firmware), the program that runs on the display. It is for a beginner who can open a Terminal window and paste commands, but has not used Arduino tools before. To get a working display you need only this page for the software on your computer; the wiring and flashing are in [install.md](install.md) and the data service is in [hosting.md](hosting.md). **You do not build the service:** hosting.md uses the public image. If you want to run the tests, publish releases or build the service yourself, see [development.md](development.md).

The downloads and the first compile took about 3 minutes on a fast connection when this page was checked. Nobody has timed a first-time build by a beginner: allow about half an hour, most of it waiting for downloads.

## Terminal in 2 minutes

Every command in these pages is typed into **Terminal**, the program that lets you control the Mac with text. This box describes how macOS works; the clicks (Spotlight, TextEdit) are Not tested here.

- **Open it:** press Command+Space, type `Terminal`, press Enter. A window with a prompt (a line ending in `%`) opens.
- **Run a command:** click in the window, type or paste the command, press **Enter**. Paste with Command+V. A block with several lines can be pasted whole: the lines run one after another. If one fails, read its message before you go on.
- **Do not paste lines that start with `#`.** In the macOS Terminal (the `zsh` shell) a line that starts with `#` is not a comment; it prints `zsh: command not found: #` (Tested). It does no harm, but leave such lines out.
- **Folders.** `pwd` prints the folder you are in. `ls` lists what is in it. `cd name` goes into a folder, `cd ..` goes up one, and `cd ~` goes to your home folder. **`~` means your home folder** (`/Users/<your name>`), so `~/ledmatrix` is the folder `ledmatrix` inside it. The **repository root** is the top folder of the downloaded code, `~/ledmatrix` once you have done step 2 below: `cd ~/ledmatrix` takes you there, and these pages tell you when a command must be run from it.
- **`curl`** is a command that fetches a web address and prints what comes back. You will see it in later pages for asking the service a question.
- **Editing a text file.** Use `nano file-name` in Terminal: type, press Control+O then Enter to save, Control+X to leave (on macOS `nano` is a small editor called Pico; its key help is shown at the bottom of the screen). Or use TextEdit, but switch it to plain text first (Format menu, "Make Plain Text"): a rich-text file would corrupt a secret or a settings file. Not tested here: TextEdit.
- **Homebrew on an Apple silicon Mac (M1 or newer).** Homebrew is the program that installs the other tools. After you install it (step 1 below) the installer prints "Next steps" with two lines like these; run them, so that Terminal can find `brew` (Not tested here: the append; `brew shellenv` itself was run and prints the settings). If the installer's lines differ, use its lines.

  ```
  echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
  eval "$(/opt/homebrew/bin/brew shellenv)"
  ```

  On an Intel Mac Homebrew lives in `/usr/local` and needs no such lines.

## 1. What you need

| Tool | What it is for | Version | How to check it is installed |
| --- | --- | --- | --- |
| **Command Line Tools** (macOS) | `git`, which gets the code | any recent; checked: git 2.54.0 | `git --version` |
| **Homebrew** (macOS) | installs arduino-cli | any recent; checked: 7.0.16 | `brew --version` |
| **arduino-cli** | compiles the firmware and flashes it; downloads the board package and libraries | **1.6.0** (what the project's build uses) | `arduino-cli version` |
| **Python 3** | the scripts in `tools/` that later pages ask you to run | the Python that macOS ships (3.9.6) is enough; see the note | `python3 --version` |

You do **not** need Docker, Go, a C++ compiler or Chrome to build the firmware. Docker is for the computer that runs the service ([hosting.md](hosting.md)); the others are for changing the code ([development.md](development.md)).

Words you will meet: **arduino-cli** is a command-line program from Arduino that compiles a program and sends it to a board. A **sketch** is an Arduino program: the firmware is the folder `arduino/ledmatrix/`. A **board package** (arduino-cli calls it a **core**) is the compiler and the [ESP32](glossary.md#esp32) support code for one family of chips. A **library** is code by someone else that the sketch includes.

> **About Python.** The scripts run on the Python 3.9.6 that macOS gives you (checked: their tests pass on it, except one check in `tools/test_provision.py` that needs Python 3.10; see development.md). You only need a newer Python to run those tests.

### Steps (macOS)

1. **Install the Command Line Tools** (they give you `git`). Skip this if `git --version` already prints a version.

   ```
   xcode-select --install
   ```

   A window asks you to install. A good result: afterwards `git --version` prints something like `git version 2.54.0 (Apple Git-157)`. If it prints `xcode-select: note: Command line tools are already installed. Use "Software Update" ...` the tools are already there (Tested).

2. **Install Homebrew** by following the one command on <https://brew.sh>, then run the two "Next steps" lines from the Terminal box above if you have an Apple silicon Mac. A good result: `brew --version` prints `Homebrew` and a version. Not tested here: the install itself.

3. **Install arduino-cli:**

   ```
   brew install arduino-cli
   ```

   On 25 Nov 2026 `brew info arduino-cli` listed 1.6.0, the version this project uses. If a later Homebrew gives you a newer one, the commands below should still work, but only 1.6.0 was tested; the [arduino-cli releases page](https://github.com/arduino/arduino-cli/releases) has exactly 1.6.0. Not tested here: the `brew install` line.

4. **Check everything:**

   ```
   git --version
   python3 --version
   arduino-cli version
   ```

   A good result (the numbers may differ a little; this is what the checks printed):

   ```
   git version 2.54.0 (Apple Git-157)
   Python 3.9.6
   arduino-cli  Version: 1.6.0 Commit: Homebrew Date: 2026-11-10T15:27:00Z
   ```

   (`Python 3.9.6` is what `/usr/bin/python3` printed; a Homebrew Python prints its own version.) A line that says `command not found` means that tool is missing: go back to its step.

### Linux

Not tested here, except where noted. On Ubuntu or Debian:

```
sudo apt install git python3 curl ca-certificates
```

Install arduino-cli 1.6.0 with Arduino's install script (Tested in an Ubuntu 24.04.5 container, as a normal user and as root):

```
curl -fsSL -o install.sh https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh
mkdir -p "$HOME/.local/bin"
BINDIR="$HOME/.local/bin" sh install.sh 1.6.0
export PATH="$HOME/.local/bin:$PATH"
arduino-cli version
```

A good result: `arduino-cli  Version: 1.6.0 ... installed successfully in /home/<you>/.local/bin`, then the version line again. The `mkdir` matters: without the folder the script stops with `folder not found. Please create it before continuing.` The `export` line puts the folder on your `PATH` for this window only; add it to `~/.profile` or `~/.bashrc` to keep it. Reading a script before you run it is a good habit; this one is short.

## 2. Get the code

Tested on macOS (a fresh clone into an empty folder, then built as below).

The code lives in a **git repository**: a folder with all the files and their history. **Cloning** copies it to your computer:

```
cd ~
git clone https://github.com/suvl/ledmatrix.git
cd ~/ledmatrix
```

Use the address of your own fork instead if you made one. A good result: `Cloning into 'ledmatrix'...`, a second or two later it is done, and `ls` shows folders such as `arduino`, `portal`, `preview`, `service`, `shared` and `tools`. The clone was 2.3 MB and took 1.2 seconds.

From now on **run every command from inside `~/ledmatrix`** (the repository root) unless it says otherwise; `pwd` prints where you are. Keep the folder's path free of spaces (not tested with spaces). Not in the repository, on purpose: the airline logos (trademarks; see [hosting.md](hosting.md)), your master secret, and everything the build produces. Without git you can download the code as a ZIP file from GitHub; that was not tested.

## 3. Install the board package and the libraries

Tested on macOS and in an Ubuntu container, in a separate arduino-cli setup (see [development.md](development.md) if you want to keep this project apart from other Arduino work). Times are from a fast home connection.

arduino-cli's built-in list does not contain Espressif's own ESP32 package, so first add Espressif's list once. Create the settings file, then add the address:

```
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

A good result: the first command prints `Config file written to: ...` (on macOS the file is `~/Library/Arduino15/arduino-cli.yaml`, on Linux `~/.arduino15/arduino-cli.yaml`). If it says `Config file already exists, use --overwrite to discard the existing one.`, that is fine: do **not** add `--overwrite`, just run the second command. Running the second command twice does not add the address twice.

Download the lists (about 6 seconds):

```
arduino-cli core update-index
```

A good result:

```
Downloading index: package_index.tar.bz2 downloaded
Downloading index: package_esp32_index.json downloaded
```

Now install the board package at the **exact** version, then the four libraries at their exact versions:

```
arduino-cli core install esp32:esp32@3.3.13
arduino-cli lib update-index
arduino-cli lib install --no-deps "Adafruit BusIO@1.17.4"
arduino-cli lib install --no-deps "Adafruit GFX Library@1.13.0"
arduino-cli lib install --no-deps "Adafruit Protomatter@1.7.1"
arduino-cli lib install --no-deps "ArduinoJson@7.4.3"
```

- `core install esp32:esp32@3.3.13` downloads the compiler, the ESP32 support code and the tools. It is **big**: about 1.1 GB to download and 3.9 GB on disk. It took 68 seconds (70 in the Ubuntu container). **Always give the `@3.3.13`.** Without it arduino-cli installs the newest version, which on 25 Nov 2026 was 4.0.0, and this firmware has not been built with that.
- `lib install` downloads one library, 1 to 2 seconds each. **Adafruit Protomatter** drives the LED panel and needs **Adafruit GFX Library** (drawing and fonts) and **Adafruit BusIO**; **ArduinoJson** reads the service's answers.
- `--no-deps` means "install exactly this one and nothing it depends on". The project's build uses it "so nothing floats to a newer one": without it arduino-cli could fetch a newer GFX library than the one the firmware is developed with.

A good result: the core install ends with `Platform esp32:esp32@3.3.13 installed` and each library with `Installed <name>@<version>`. The whole set took about 85 seconds. Check what is installed:

```
arduino-cli core list
arduino-cli lib list
```

A good result (the "Latest" and "Available" columns show what is newest today and will change; **Installed** must match):

```
ID          Installed Latest Name
esp32:esp32 3.3.13    4.0.0  esp32

Name                 Installed Available    Location Description
Adafruit BusIO       1.17.4    -            user     -
Adafruit GFX Library 1.13.0    2.0.0        user     Adafruit GFX graphics core library, t...
Adafruit Protomatter 1.7.1     -            user     -
ArduinoJson          7.4.3     -            user     -
```

These versions are written in one place, the `env:` block at the top of `.github/workflows/service.yml` (`ESP32_CORE_VERSION` and `ARDUINO_LIBRARIES`). If this page and that file disagree, the file is right.

## 4. Compile the firmware

Tested on macOS from a fresh clone, and in the Ubuntu container.

**Compiling** turns the source code into a program file for the chip. From the repository root:

```
arduino-cli compile --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --jobs 0 --build-path ~/ledmatrix-build/firmware arduino/ledmatrix
```

- `--fqbn esp32:esp32:adafruit_matrixportal_esp32s3` is the **fully qualified board name**: the board package, then the board. It tells the compiler this is an Adafruit Matrix Portal S3.
- `--jobs 0` uses every processor core.
- `--build-path ~/ledmatrix-build/firmware` is where the results go: outside the repository on purpose, so nothing in the repository changes. [install.md](install.md) uses the same folder. **Keep this folder**: provisioning a board needs it again.
- `arduino/ledmatrix` is the sketch folder.

Do not use `--output-dir` instead of `--build-path`: Tested, it also creates a `build/` folder inside the sketch folder in your repository.

A good result, after about 76 to 92 seconds the first time (the first build also compiles the whole ESP32 support code; 145 seconds in the emulated Ubuntu container; **12 seconds** if you run the same command again with nothing changed):

```
Sketch uses 1505005 bytes (71%) of program storage space. Maximum is 2097152 bytes.
Global variables use 106280 bytes (32%) of dynamic memory, leaving 221400 bytes for local variables. Maximum is 327680 bytes.
```

The first line is the flash room the program takes: 71 percent of one 2 MB program slot (firmware 0.11.0 on 26 Nov 2026). The second is RAM: 32 percent. An update has to fit in one slot, so watch the first number if you add code. The numbers move a little with the code and with the machine (on 25 Nov the same code gave 1479637 bytes on the Mac, 1479301 in the Ubuntu container and 1479317 in the project's own build); a few hundred bytes of difference between machines is normal.

The file you will flash is `~/ledmatrix-build/firmware/ledmatrix.ino.bin` (about 1.5 MB, 1,505,152 bytes on 26 Nov 2026): the firmware image. The build folder holds other files too (the start-up program, the partition table, a debugging file, a single 8 MB file with everything joined); you do not need to touch them. [development.md](development.md), "Inside a build", says what each one is. If the compile fails, the last lines say why: see "If something goes wrong" below.

## 5. Check the partition table

Tested on macOS: built once with the file and once with a copy of the sketch folder that did not have it.

The board's 8 MB [flash](glossary.md#flash) is divided into named rooms called [partitions](glossary.md#partition). This project changes the stock layout in one way: it reserves 8 KB for a read-only partition called `ident`, which holds the board's identity record (see [install.md](install.md); [architecture.md](architecture.md) explains the whole table). The layout is the file `arduino/ledmatrix/partitions.csv`, and arduino-cli uses it only **if it sits next to the `.ino` file**. If it is missing, **the build still succeeds, with no error or warning**, and silently uses the board's stock layout, which has no `ident` room. Flashing that build would replace the layout on the board, and the board could no longer find its identity record. So always compile the whole folder `arduino/ledmatrix` (never a copy of the `.ino` file alone), and check the build:

```
grep '^ident' ~/ledmatrix-build/firmware/partitions.csv
```

A good result:

```
ident,    data, 0x40,    0x7FE000, 8K,     readonly
```

If it prints nothing, the build did not use the project's layout: compile the whole `arduino/ledmatrix` folder again and check again. **Do not flash a build that fails this check.** (In the checks, the build without the file had no `ident` line; the build with it had one.)

## 6. The version number

The firmware's version number is written in one place, `arduino/ledmatrix/version.h`:

```
#define FIRMWARE_VERSION "0.11.0"
```

The board shows it in the portal. **Leave it as it is** unless you publish releases of your own: a display offers an update only when the published version is strictly newer than its own, and the rules for changing it are in [development.md](development.md), "Releasing".

One more file next to it matters if you ever publish your own releases: `arduino/ledmatrix/update_keys.h`. It holds the **public keys** that a firmware update must be signed with before the display installs it. The file in the repository holds the maintainer's two public keys, so a firmware you build from it accepts the updates the maintainer signs and publishes in the public image, and **you do not change it**. If you build your own firmware *and* your own service image, a display will refuse your updates ("signed with a key this board does not know") until you put your own key in this file and flash every display once over USB: see [development.md](development.md), "Releasing". Flashing over USB, as in [install.md](install.md), never needs a signature. A build with no key at all in this file does not compile (checked in a copy of the repository: the compile stops with a plain message).

## 7. Find the serial port

Not tested here: the display was not touched while writing this page; [install.md](install.md), "First flash", has the checked procedure.

To flash a board you need its [serial port](glossary.md#serial-port), the name your computer gives the USB connection. With the board plugged in by USB:

```
arduino-cli board list
```

Look for a line whose port looks like `/dev/cu.usbmodem1234` on a Mac (`/dev/ttyACM0` on Linux) and says `Serial Port (USB)`. The number differs on every computer and can change after an upload; these pages write it as `/dev/cu.usbmodem1234`: use your own. Flash only with `arduino-cli upload`, as install.md explains; never run `esptool` by hand on this board.

## If something goes wrong

If a problem is not in this table, or a fix does not work, open an issue at <https://github.com/suvl/ledmatrix/issues> and say which step failed and paste the last lines the tool printed (no secrets).

| What you see | What it means | The fix |
| --- | --- | --- |
| `brew: command not found` after installing Homebrew | Terminal cannot find Homebrew yet. | On an Apple silicon Mac run the two lines from "Terminal in 2 minutes", then open a new Terminal window. |
| The core download is slow, stops, or ends with a network error | It is a 1.1 GB download from Espressif's servers and GitHub. | Run the same `arduino-cli core install esp32:esp32@3.3.13` again. Check that you have 8 GB of free disk space. A VPN or a company proxy can slow it or block it: try without, or set `arduino-cli config set network.proxy http://your-proxy:port`. Not tested here (it was fast on the connection used). |
| `Error during build: Platform 'esp32:esp32' not found: platform not installed`, then ``Try running `arduino-cli core install esp32:esp32` `` | The board package is not installed in the folders arduino-cli is using now. | Run the install of step 3. If you did install it, you may be using a different setup in this window (see "A separate arduino-cli setup" in [development.md](development.md)). |
| `fatal error: Adafruit_Protomatter.h: No such file or directory` (or another library's header), then `Error during build: exit status 1` | A library is not installed. | Install the four libraries of step 3 and check `arduino-cli lib list`. |
| `Error installing Adafruit Protomattr: Library 'Adafruit Protomattr@1.7.1' not found` | A misspelt library name (a version that does not exist gives the same sentence). | Check the spelling and the version, and keep the quotes (the names have spaces). |
| `Can't open sketch: main file missing from sketch: .../name/name.ino` | The sketch folder has a different name from the `.ino` file inside it, for example after copying only the `.ino`. | Compile the folder `arduino/ledmatrix` as it is. |
| The build works but `grep '^ident' ...` prints nothing | `partitions.csv` was not next to the `.ino` (step 5). There is **no** "partition table not found" error: the file being missing is silent. | Compile the whole `arduino/ledmatrix` folder and check again. Do not flash a build that fails the check. |
| `multiple access on port` while uploading (the wording can differ between tool versions) | Another program holds the serial port: a serial monitor (`arduino-cli monitor` in another window), the Arduino IDE, a `screen` session, or a tool such as `provision.py`. Not tested here (the board was not touched). | Close that program. `lsof /dev/cu.usbmodem1234` (with your port name) lists who has it open. Unplug and replug the cable if nothing holds it. See install.md, "First flash". |
| `git status` shows `arduino/ledmatrix/build/` | You used `--output-dir`, which writes a `build/` folder into the sketch folder. | Delete that folder and use `--build-path` (step 4). |
| The build seems out of date after you changed a file | arduino-cli reuses earlier work. | Add `--clean` to the compile command. |

## Where things are stored

| What | macOS | Linux | Size |
| --- | --- | --- | --- |
| arduino-cli's settings | `~/Library/Arduino15/arduino-cli.yaml` | `~/.arduino15/arduino-cli.yaml` | tiny |
| Board package and tools | `~/Library/Arduino15` | `~/.arduino15` | 3.9 GB |
| Libraries | `~/Documents/Arduino/libraries` | `~/Arduino/libraries` | 5 MB |
| Your build output | `~/ledmatrix-build/` | same | about 110 MB |

(Tested: the folder names were read from arduino-cli with a throw-away home folder.) `~/Documents/Arduino` is also where the Arduino IDE keeps your own sketches, so to remove this project's libraries delete only `libraries/Adafruit_*` and `libraries/ArduinoJson` there. [development.md](development.md), "Clean up", lists the rest (download archives, build caches, Docker images).

## What was run

On an Apple silicon Mac (macOS 27.0.1) with git 2.54.0, Python 3.15.2 (and the system Python 3.9.6), arduino-cli 1.6.0. The commit hashes below name the history before 27 Nov 2026, when every commit was signed again; the files are identical, so find a commit by its title and date (see [development.md](development.md), "What was run"). Rows dated 25 Nov 2026 were run on the repository as committed at `1f9b6a5` (firmware 0.10.1); rows dated 26 Nov 2026 were run on snapshots of `main` at `0789a98` (firmware 0.10.1) and at `1929bf2` (firmware 0.11.0). "Ubuntu" is an Ubuntu 24.04.5 x86_64 container, emulated on that Mac. Everything after the tools was run in a separate throw-away arduino-cli setup, so the result does not depend on what was already on the Mac.

| What | Where | Result |
| --- | --- | --- |
| `config init`, `config add`, `core update-index`, `core search esp32 --all` | Mac | worked; 3.3.13 is in the list, the newest was 4.0.0 |
| Install from an empty setup: `core update-index` 6 s, `core install esp32:esp32@3.3.13` 68 s, `lib update-index` 2 s, four `lib install --no-deps` 1 to 2 s each; `core list` and `lib list` as in step 3 | Mac | about 85 s in total; 3.9 GB data, 1.1 GB downloads, 5 MB libraries |
| The same install, in a container | Ubuntu | worked; core install 70 s |
| The arduino-cli install script as a normal user, with and without the `BINDIR` folder existing | Ubuntu | as in the Linux text |
| `arduino-cli compile` from the snapshot of `main` at `1929bf2`, as in step 4, 26 Nov | Mac | 86 s; `Sketch uses 1505005 bytes (71%) ...`, `Global variables use 106280 bytes (32%) ...`; `ledmatrix.ino.bin` 1,505,152 bytes; the `ident` line was present; the version marker said `0.11.0` and the signing marker `required` (the two markers are explained in [development.md](development.md), "Inside a build") |
| The same compile at `0789a98`, 26 Nov | Mac | 92 s (other jobs were running); `Sketch uses 1498117 bytes (71%) ...` |
| The compile in the project's own build (GitHub's run of the release commit), 26 Nov, read from its log | CI | `Sketch uses 1504701 bytes (71%) ...`, `Global variables use 106280 bytes (32%) ...` |
| A copy of the repository whose `update_keys.h` pins no key, 26 Nov | Mac | the compile stopped with `static assertion failed: a build that requires signed updates needs at least one public key in update_keys.h ...` |
| `arduino-cli compile` (25 Nov): cold 86 s (default build folder), cold with `--build-path` 76 s, again 12 s; from a fresh `git clone` 77 s (with `--output-dir`) and 79 s (as in step 4); in the container 145 s | Mac, Ubuntu | each ended `Sketch uses 1479637 bytes (70%) ...` on the Mac, `Global variables use 106064 bytes (32%) ...`; `ledmatrix.ino.bin` 1,479,792 bytes |
| Compile of a copy of the sketch without `partitions.csv`; of a copy in a renamed folder; with no board package; with no libraries; a misspelt library name | Mac | the messages quoted above (the first compiles silently with the stock layout) |
| `grep '^ident'` on both builds | Mac | one `ident` line with the project's layout; nothing without it |
| `git clone https://github.com/suvl/ledmatrix.git`, then steps 3 and 4 in that clone | Mac | 1.2 s, 2.3 MB; built as above |
| `xcode-select --install` with the tools already installed; `python3 -I` tool tests with Python 3.9.6 | Mac | the message quoted in step 1; all pass except one check in `test_provision.py` |
| A pasted `#` line in an interactive `zsh`; `brew shellenv` | Mac | `zsh: command not found: #`; prints the settings |

**Not tested here:** `brew install` of any tool and Homebrew itself; the Homebrew "Next steps" append; `arduino-cli board list` and every serial or board step (the board was not touched); `arduino-cli upload`; a slow or proxied network; the Linux steps beyond the install script in the container; TextEdit; Windows; an Intel Mac; the Arduino IDE.
