# Glossary

This page explains, in plain English, the technical words used in the other pages of this documentation. Each word is explained once, here. The other pages link to it the first time they use the word, and each entry names the page and the section that explain the idea in depth. The entries are in alphabetical order.

How this page was checked: every entry was written from the other pages of this documentation and the code and contracts they cite, and checked against them on 25 Nov 2026. Nothing on this page was run on a board. Links to the pages are by file name, and the section names are the titles of sections in those pages.

## ADS-B

The radio signal that aircraft broadcast with their position, height and speed. Receivers on the ground pick it up.
In this project the open data service adsb.fi publishes what the receivers hear, and your [service](#service) reads it to find the planes near you.
Read [architecture.md](architecture.md), "How data gets to the panel".

## amd64 and ARM

Two kinds of computer processor. Intel and AMD processors (most PCs, mini PCs and rented servers) are amd64. Apple Silicon Macs and Raspberry Pis are ARM.
In this project the [public image](#public-image) is built for amd64 only. On an ARM machine it runs under emulation, which is fine for trying things; on an ARM server you build your own image, which carries no firmware.
Read [hosting.md](hosting.md), "The image".

## API token

A secret text that lets a program use an online service on your behalf. Treat it like a password.
In this project there is no token between the display and your service. The one token you may need is your DNS provider's, which lets [Caddy](#caddy) prove that you own your domain name.
Read [hosting.md](hosting.md), "Cloudflare, click by click".

## App slot

One of the two 2 MB rooms in the board's [flash](#flash), called `ota_0` and `ota_1`, where a firmware image lives. The running firmware stays in one slot while a new one is written to the other; only a successful check switches the boot choice.
See [partition](#partition).
Read [architecture.md](architecture.md), "The flash layout" and [architecture.md](architecture.md), "Firmware updates".

## arduino-cli

A command-line program from Arduino that compiles the firmware and sends it to the board. It does what the Arduino IDE does, but from the [Terminal](#terminal), so every step can be written down and repeated.
This project uses version 1.6.0.
Read [build.md](build.md), "What you need".

## A record and AAAA record

A DNS record that says "this name is at this address". An A record holds an [IPv4](#ipv4) address and an AAAA record holds an [IPv6](#ipv6) address.
In this project you give the name of your service an A record. Do not add an AAAA record unless the server really answers over IPv6: the display tries IPv6 first when it has a global IPv6 address, and a dead one makes it wait 5 seconds before it falls back to IPv4.
Read [hosting.md](hosting.md), "What the display needs".

## Bootloader

The small program a chip runs first when it gets power. It decides which program to start and can accept a new one.
On this board Adafruit's [TinyUF2](#tinyuf2) plays this role. It does not roll back a new image that fails, so the firmware does the [rollback](#rollback) itself.
Read [architecture.md](architecture.md), "Firmware updates".

## Branch and fork

A branch is one line of a repository's history; `main` is the main one. A fork is your own copy of someone else's repository on GitHub.
In this project a push to `main` that changes the right files starts the [CI](#ci) workflow, which is how a [release](#release) is built.
Read [development.md](development.md), "Releasing".

## Burn

To copy a display's key permanently into the chip's [eFuse](#efuse) memory. After the burn the chip can sign with the key, but no program, tool or person can read it back, and nothing can undo it.
In this project the burn is optional and comes last. The separate sketch `tools/burn_key` does it, and it runs as a [dry run](#dry-run) unless you ask for more. Afterwards `tools/provision.py --burned` removes the software key from flash. Do not confuse it with [burn-in](#burn-in).
Read [install.md](install.md), "Optional, days later: the permanent burn" and [security.md](security.md), "How the system is protected".

## Burn-in

Uneven wear of an LED panel where the same picture stays lit for hours.
In this project [panel protection](#panel-protection) fights it.
Read [install.md](install.md), "First look at the portal".

## CA bundle

The list of trusted [certificate authorities](#certificate-authority) that is built into the firmware. The display checks every server's [certificate](#certificate) against it and never skips the check.
So your service needs a certificate from a normal public authority; Let's Encrypt works.
Read [hosting.md](hosting.md), "What the display needs".

## Cache

A place where a program keeps a recent answer so that it does not have to ask again.
In this project the service keeps a cache in memory: positions for 15 seconds, a route for 12 hours, a forecast for 30 minutes, headlines for 15 minutes. Many requests share one call to the data source, and when a source fails the last good answer is served for a while. The cache is lost when the service restarts.
Read [architecture.md](architecture.md), "The service".

## Caddy

A small web server that gets and renews [certificates](#certificate) by itself.
In this project Caddy is the [reverse proxy](#reverse-proxy) of the main hosting path: it holds the certificate, speaks HTTPS to the display and passes plain HTTP to your service.
Read [hosting.md](hosting.md), "Path 1: a home server".

## Call sign

The short code that an aircraft broadcasts to identify its flight, often an airline's code and a number.
In this project the service looks up each call sign at adsbdb to find the airline and the route. Routes are missing for some flights, such as charter, cargo, general aviation and military ones.
Read [architecture.md](architecture.md), "How data gets to the panel".

## Captive page

A web page that opens by itself when you join a network.
In [setup mode](#setup-mode) the board answers every web name with its own address, so the setup page opens on its own. If it does not, open `http://192.168.4.1` yourself.
Read [install.md](install.md), "Wi-Fi setup (setup mode)".

## Certificate

A digital document that proves who a server is: "this key belongs to `apis.example.com`". A [certificate authority](#certificate-authority) issues it, and it is valid only for a limited time.
In this project the display checks your service's certificate against its [CA bundle](#ca-bundle) and refuses it if the name, the chain or the date is wrong, so its clock must be right.
Read [hosting.md](hosting.md), "What the display needs".

## Certificate authority

An organisation that issues [certificates](#certificate) and that devices trust. Often shortened to CA.
In this project the main hosting path uses [Let's Encrypt](#lets-encrypt). The display trusts only the authorities in its [CA bundle](#ca-bundle).

## Certificate Transparency

Public logs in which certificate authorities must record every certificate they issue.
So the name of your service can be found by anyone, for good. Choose a name that does not identify you.
Read [hosting.md](hosting.md), "A domain name" and [security.md](security.md), "How the system is protected".

## CI

Continuous integration: a computer that runs the tests and builds the release for you every time code is pushed.
Here it is a GitHub workflow, `.github/workflows/service.yml`. It tests everything, compiles the firmware, writes the [manifest](#manifest) and builds the service [image](#docker-image).
Read [development.md](development.md), "Releasing".

## Clock skew

The difference between two clocks.
In this project the service refuses a signed request when the display's clock and its own differ by more than 2 minutes (the reason is `clock_skew`). It answers with its own time, and the display corrects its signing time once and tries again.
Read [architecture.md](architecture.md), "Identity and trust".

## Cloudflare

A company that offers DNS hosting and registers domain names.
It is the hosting guide's worked example: a domain registered there uses Cloudflare's DNS from the start, and you can create an [API token](#api-token) there for [Caddy](#caddy). The guide's Cloudflare steps are not tested here.
Read [hosting.md](hosting.md), "Cloudflare, click by click".

## Cloud Run

Google's service that runs a [container](#container) for you and gives it an HTTPS address of its own, so you need no server and no [domain name](#domain-name). It needs a Google billing account with a payment card.
It is one of the four ways to run the service.
Read [hosting.md](hosting.md), "Path 3: Google Cloud Run".

## Commit hash

A long code (40 characters, [hexadecimal](#hexadecimal)) that names one exact commit of a repository.
"Pinned by commit hash" means a workflow step or an image is tied to that exact commit instead of a version label that someone could move. In this project the workflow's GitHub Actions are pinned this way, with the release name in a comment, and the service [image tags](#image-tag) include the commit hash.
Read [security.md](security.md), "What you must keep up to date" and [hosting.md](hosting.md), "The image".

## Container

A running copy of a [Docker image](#docker-image): a program started by [Docker](#docker) in its own sealed space, with its own files.
In this project the service runs in one container. The home-server path runs two with [Docker Compose](#docker-compose): Caddy and `ledmatrix-api`.
Read [hosting.md](hosting.md), "Path 1: a home server".

## Core

A word with two meanings here.
A processor core is one of the two processors inside the ESP32-S3 chip: the firmware draws on core 1 and does its network work on core 0. A core, in [arduino-cli](#arduino-cli), is the board package for one family of chips (compiler, board definitions and support code); this project pins `esp32:esp32` at version 3.3.13.
Read [architecture.md](architecture.md), "The tasks" and [build.md](build.md), "Install the board package and the libraries".

## curl

A command that fetches a web address and prints what comes back.
In these pages you use it to ask the service a question, for example `curl -i https://apis.example.com/healthz`.
Read [build.md](build.md), "Terminal in 2 minutes".

## Dependabot

A GitHub service that proposes updates to the software your project depends on.
In this project it runs weekly (on Mondays) for the workflow's GitHub Actions, the service's Go modules and its Docker base images (`.github/dependabot.yml`). It cannot see the tool versions pinned in the workflow's `env` block (arduino-cli, the board package and the libraries): check those by hand.
Read [security.md](security.md), "What you must keep up to date" and [development.md](development.md), "Releasing".

## Device ID

The board's factory [MAC address](#mac-address), written as 12 lowercase [hexadecimal](#hexadecimal) characters, for example `aabbccddeeff`. It is not secret.
The portal shows it as "This board's ID", `/status` shows it as `device`, and the [serial monitor](#serial-monitor) prints it. The service works out the [device key](#device-key) from it, and the [revoked list](#revoked-list) holds device IDs.
Read [install.md](install.md), "Make the master secret and back it up".

## Device key

The secret of one display, 32 bytes long. It is computed as [HMAC](#hmac)-SHA-256 of the [device ID](#device-id), with the [master secret](#master-secret) as the key. The display signs every request to your service with it.
The service stores no device keys: it works the key out again for each request. On the board the key is first an [interim software key](#interim-software-key) in the [ident partition](#ident-partition) and, after the [burn](#burn), only inside the chip.
Read [architecture.md](architecture.md), "Identity and trust" and [security.md](security.md), "How the system is protected".

## DHCP

The service in your router that hands out network addresses to the devices on your home network.
A DHCP reservation (also called a static lease) makes the router always give the same address to one device. Give your home server one.
Read [hosting.md](hosting.md), "Path 1: a home server".

## Display name

The name of one display, set in the [portal](#portal) under "Wi-Fi and device". The default is `ledmatrix`. It is the display's [mDNS](#mdns) name (`<name>.local`), its name on your router and the name of its own setup Wi-Fi network once you have set one; the Connected screen and the Problem message show it too.
A name is 1 to 24 characters: letters, digits and hyphens, with no hyphen at either end, and capitals become lower case. Saving a new name restarts the display (about 20 seconds) and the portal follows it to the new address. Two displays on one network need different names; with the same name the second one is answered as `<name>-2.local`, which was not seen on a board yet. A board that has never been set up always starts its setup network as `ledmatrix`.
Read [install.md](install.md), "A second display" and [operations.md](operations.md), "Change a setting".

## DNS

The internet's phone book. It turns a name such as `apis.example.com` into an address. A DNS record is one line of that book, and your DNS provider keeps your lines. Often that is the company where you rent the [domain name](#domain-name).
In this project the display looks up your service's name and needs an [A record](#a-record-and-aaaa-record).
Read [hosting.md](hosting.md), "A domain name".

## DNS challenge

A way to prove to a [certificate authority](#certificate-authority) that you control a name: you add a temporary record at your DNS provider, and the authority looks it up.
In this project [Caddy](#caddy) does this with a token for your DNS provider. A home server needs no open port for it, but renewal repeats it every few weeks, so the machine and the DNS access must keep working.
Read [hosting.md](hosting.md), "A domain name" and [hosting.md](hosting.md), "Path 1: a home server".

## DNS rebinding protection

A feature of many routers and DNS filters that refuses a public name which answers with a private address.
If yours has it, the display cannot look up your home server's name. Add your domain to its exceptions.
Read [hosting.md](hosting.md), "Background".

## Docker

A program that runs [containers](#container). On a Mac you install Docker Desktop or OrbStack and start it; on Linux you install Docker Engine.
You need it only on the computer that runs the service, and for development. You do not need it to build the firmware.
Read [build.md](build.md), "What you need" and [hosting.md](hosting.md), "Path 1: a home server".

## Docker Compose

A tool that starts several [containers](#container) together from one file, `docker-compose.yml`.
It is the main hosting path: one file runs [Caddy](#caddy) and the service. The example files are in `deploy/compose/`.
Read [hosting.md](hosting.md), "Path 1: a home server".

## Docker image

A sealed, ready-made copy of a program and everything it needs to run. [Docker](#docker) starts an image as a [container](#container).
The service ships as one image, built for `linux/amd64`, with the firmware inside it. See [public image](#public-image) and [your own build](#your-own-build).
Read [hosting.md](hosting.md), "The image" and [development.md](development.md), "The service image and running it on your computer".

## Domain name

A name such as `example.com` that you rent for a year at a time from a company called a registrar. It lets you create names under it, such as `apis.example.com`, and decide what each points to.
In this project the display connects to your service by a host name on port 443, and a [certificate](#certificate) from a public authority needs a real name that is yours. The "service address" in the [portal](#portal) is the host name only. Cloud Run is the one path that needs no domain name.
Read [hosting.md](hosting.md), "A domain name".

## Dry run

A run that does every check and shows what it would do, but changes nothing.
In this project the key [burn](#burn) sketch is a dry run by default, and a dry-run build does not even contain the code that writes.
Read [install.md](install.md), "Optional, days later: the permanent burn".

## eFuse

A tiny one-time switch inside a chip. It can be set once and never cleared.
In this project the burned key lives in the chip's eFuse block `BLOCK_KEY5`, marked so that it cannot be read or changed.
Read [architecture.md](architecture.md), "Identity and trust" and [security.md](security.md), "How the system is protected".

## Environment variable

A named setting that is handed to a program when it starts.
The service is set up this way: `MASTER_SECRET_FILE` (the recommended way to give it the [master secret](#master-secret)) or `MASTER_SECRET`, `LISTEN`, `PUBLIC_HOST`, `REVOKED_DEVICES`, `LOGO_DIR`, `FIRMWARE_DIR` and `AUTH_OFF` (which is for your own computer only).
Read [hosting.md](hosting.md), "Settings and revoking a display".

## ESP32

A family of Wi-Fi chips from Espressif. The Adafruit Matrix Portal S3 holds an ESP32-S3, which has two processor cores and, on this board, 8 MB of [flash](#flash).
It runs the [firmware](#firmware).
Read [install.md](install.md), "Parts, wiring and power".

## ESP-IDF

The chip maker Espressif's own software kit for ESP32 chips. The Arduino support code for these chips builds on it.
In this project the firmware update runs ESP-IDF's check of an image before it switches to it.
Read [architecture.md](architecture.md), "Firmware updates".

## Firmware

The program that lives on the board and runs the display: it joins your Wi-Fi, fetches data, draws the screens and serves the [portal](#portal).
In this project it is the Arduino C++ code in `arduino/ledmatrix/`, and its version number is in `version.h`.
Read [architecture.md](architecture.md), "The firmware from the inside".

## Flash

The memory chip that keeps its contents when the power is off. The board has 8 MB of it, holding the firmware, the saved settings and the identity record. It is divided into [partitions](#partition).
To flash a board means to write a new program into it over USB. In this project you do that only with `arduino-cli upload`.
Read [install.md](install.md), "First flash".

## Folder and path

A folder (also called a directory) holds files. A path says where a file or folder is, for example `~/ledmatrix/arduino`. The sign `~` means your home folder.
In the [Terminal](#terminal), `pwd` prints the folder you are in, `ls` lists what is in it, `cd name` goes into a folder and `cd ..` goes up one. See also [repository root](#repository-root).
Read [build.md](build.md), "Terminal in 2 minutes".

## FQBN

Fully qualified board name: the exact name [arduino-cli](#arduino-cli) uses for a board. It is the board package, then the board.
For this project it is `esp32:esp32:adafruit_matrixportal_esp32s3`.
Read [build.md](build.md), "Compile the firmware".

## Frame

One complete picture drawn on the panel.
In this project the render [task](#task) draws about 30 frames a second, and the board's `/status` page counts them and the longest gap between two. See the [one-second rule](#one-second-rule).
Read [architecture.md](architecture.md), "The one-second rule".

## GitHub issue

A numbered page in the project's issue tracker on GitHub. It is also where you ask for help.
In this project the decisions and plans live in issues, not in the repository. Start at the map, issue 35: it lists every decision in one line and links to the issue that holds the detail.
Read [README.md](../README.md), "Getting help".

## Git repository and clone

A git repository is a folder that holds all the project's files and their whole history. To clone it is to copy it to your computer.
After you clone this one, the top folder is your [repository root](#repository-root).
Read [build.md](build.md), "Get the code".

## Hexadecimal

A way of writing numbers with the digits 0 to 9 and the letters a to f.
In this project the [device ID](#device-id) (12 characters) and the [SHA-256](#sha-256) fingerprint (64 characters) are written this way, and `openssl rand -hex 32` makes a 64-character hexadecimal master secret.
Read [install.md](install.md), "Make the master secret and back it up".

## HMAC

A way to mix a secret key into a message and get a short fingerprint that nobody can make without the key. HMAC-SHA-256 uses [SHA-256](#sha-256) for the mixing.
In this project it makes the [device key](#device-key) and the signature of every request.
Read [architecture.md](architecture.md), "Identity and trust".

## Homebrew

A package manager for macOS: a program that installs other programs with one command, such as `brew install arduino-cli`.
On an Apple silicon Mac its installer prints two lines that you must run so that the [Terminal](#terminal) can find `brew` (see [PATH](#path)).
Read [build.md](build.md), "Terminal in 2 minutes" and [build.md](build.md), "What you need".

## Host test

A test that compiles part of the firmware's own code as an ordinary program for your computer and runs it, so you need no board.
In this project they cover request signing, the [problem list](#problem-list), plane choice, [panel protection](#panel-protection), the update logic, the display name, the settings save and the portal's request guard. You need them only if you change the code.
Read [development.md](development.md), "The tests".

## HTTP and HTTPS

HTTP is the language that web browsers and web servers speak. HTTPS is HTTP carried inside an encrypted [TLS](#tls) connection.
In this project the [portal](#portal) uses plain HTTP on your home network with no password. The service must be reachable over HTTPS on port 443.
Read [security.md](security.md), "How the system is protected" and [hosting.md](hosting.md), "What the display needs".

## HUB75

The common connector and signalling standard for RGB LED matrix panels. It carries the picture signals, not the panel's power.
The Waveshare panel has two HUB75 sockets: IN, for data coming from a controller, and OUT, for data going on to a second panel. The board's [ribbon cable](#ribbon-cable) goes into IN.
Read [install.md](install.md), "Parts, wiring and power".

## IATA code

An airline's short code, two characters such as `TP`.
In this project the service looks for logo files named `<IATA>.bin`.
Read [hosting.md](hosting.md), "The logo set".

## Ident partition

An 8 KB read-only [partition](#partition) at address 0x7FE000 that holds the board's identity record: the [device ID](#device-id) and, until the [burn](#burn), the [interim software key](#interim-software-key).
A normal upload never writes it, so the identity survives every upload and update. `tools/provision.py` writes it over USB. "Read-only" is a rule for the firmware's own code; a flash tool can still write it.
Read [architecture.md](architecture.md), "The flash layout".

## Image tag

A label on a [Docker image](#docker-image).
In this project each image is tagged `latest` and with the full commit hash. To pin a version, deploy the commit tag. Take the commit from the package page on GitHub: a commit that changed only documents has no image.
Read [hosting.md](hosting.md), "The image".

## Interim software key

The [device key](#device-key) as it is first saved in the [ident partition](#ident-partition), before the [burn](#burn). It lets you test everything end to end.
A flash tool connected to the USB port could read it back. After the burn `provision.py --burned` removes it.
Read [install.md](install.md), "Optional, days later: the permanent burn".

## IPv4

The older and still most common kind of network address, four numbers such as `192.0.2.10`.
In this project the board works on IPv4 and IPv6 networks, and the service host uses IPv4 today.
Read [architecture.md](architecture.md), "Networks and time".

## IPv6

The newer kind of network address, longer and written in hexadecimal.
In this project the display tries IPv6 first when it has a global IPv6 address, and falls back to IPv4 if that fails.
Read [architecture.md](architecture.md), "Networks and time".

## JSON

A plain-text format for structured data, such as `{"version":"0.10.1"}`.
In this project the service answers the display in small JSON, and the board's `/status` page is JSON too.
Read [architecture.md](architecture.md), "The service".

## Kubernetes

A system for running [containers](#container) on a cluster of computers.
It is an appendix in the hosting guide, for people who already run a cluster; its tool is `kubectl`. The example manifest is `service/deploy/ledmatrix-api.yaml`.
Read [hosting.md](hosting.md), "Path 4: Kubernetes (appendix)".

## LAN

Local area network: your home network, the devices on your router.
In this project the [portal](#portal) is meant for your LAN only: it has no password, so anyone on your Wi-Fi can use it.
Read [security.md](security.md), "How the system is protected".

## Let's Encrypt

A free [certificate authority](#certificate-authority).
In this project it is the authority used on the main hosting path; its certificates last 90 days today, and [Caddy](#caddy) renews them.
Read [hosting.md](hosting.md), "Background".

## Library

Code written by someone else that a program includes.
In this project the firmware uses four, at exact versions: Adafruit [Protomatter](#protomatter), Adafruit GFX Library, Adafruit BusIO and ArduinoJson.
Read [build.md](build.md), "Install the board package and the libraries".

## Lock

A flag that lets only one [task](#task) at a time touch some shared data. Programmers also call it a mutex.
In this project locks are held for only microseconds and never across a network request or a [flash](#flash) write, so the drawing task never waits long.
Read [architecture.md](architecture.md), "The one-second rule".

## MAC address

A hardware address that a network chip gets at the factory.
In this project the board's MAC address, written as 12 lowercase [hexadecimal](#hexadecimal) characters, is its [device ID](#device-id).
Read [install.md](install.md), "First flash".

## Maintainer

The person who publishes firmware releases and the public service image. Today that is the project's author.
If you run the [public image](#public-image), updates reach your display when the maintainer publishes a release. If you build your own, you are the maintainer of your copy (see [your own build](#your-own-build)).
Read [README.md](../README.md), "Words used in these guides" and [development.md](development.md), "Which case are you?".

## Manifest

A small [JSON](#json) file, `manifest.json`, that says which firmware is the newest: its version, file name, size, [SHA-256](#sha-256) and notes.
The display fetches it from `/v1/firmware/manifest` once a day and when you press "Check now", and offers an update only if the version is strictly newer than its own. It is not signed.
Read [architecture.md](architecture.md), "Firmware updates" and [development.md](development.md), "Releasing".

## Master secret

One long random text, at least 32 characters, that only you know. Every display's [device key](#device-key) comes from it.
You keep it in a file outside the repository, back it up in a [password manager](#password-manager), and give it to the service as `MASTER_SECRET_FILE` (recommended) or `MASTER_SECRET`. After the [burn](#burn), a lost master secret locks the display out of your service for good.
Read [install.md](install.md), "Make the master secret and back it up" and [security.md](security.md), "Secrets".

## mDNS

A way for devices on the same local network to find each other by name without a server.
It is why `http://ledmatrix.local` works (`ledmatrix` is the default [display name](#display-name)). macOS supports it. On Linux you may need the Avahi service and the `nss-mdns` package (not tested here).
Read [install.md](install.md), "Wi-Fi setup (setup mode)".

## Mermaid

A text format for diagrams. GitHub draws it for you.
The documentation's diagrams are Mermaid code blocks.

## Multimeter

A tool that measures electrical values. Any one that measures DC volts will do for the polarity check in the wiring step.
Between the + and - terminals of the power adapter it should read close to 5 V; the author's notes expect +5.0 to 5.2 V.
Read [install.md](install.md), "Parts, wiring and power".

## nano

A small text editor that runs inside the [Terminal](#terminal). `nano file-name` opens a file; press Control+O then Enter to save, and Control+X to leave.
You can use TextEdit instead if you switch it to plain text first (Format menu, "Make Plain Text"): a rich-text file would corrupt a secret or a settings file.
Read [build.md](build.md), "Terminal in 2 minutes".

## Network port

A number that says which program on a computer a network connection is for.
In this project the display always connects to port 443 (HTTPS) on your service, the [portal](#portal) answers on port 80, and the service itself listens on port 8080 behind the [reverse proxy](#reverse-proxy).
Read [hosting.md](hosting.md), "What the display needs".

## Night blanking

A [panel protection](#panel-protection) feature: the panel goes dark between two local times, by default 00:00 to 06:00. A short press of UP or DOWN lights it for 30 seconds.
So a dark panel is usually normal.
Read [install.md](install.md), "First look at the portal".

## NTP

Network Time Protocol: how devices ask the internet for the time.
In this project the board asks `pool.ntp.org` and `time.cloudflare.com`. The clock matters: [certificates](#certificate) have dates, signed requests carry the time, and the clock and night blanking need it. NTP itself is not authenticated.
Read [architecture.md](architecture.md), "Networks and time".

## NVS

Non-volatile storage: a small key-value store in the `nvs` [partition](#partition) of the board's [flash](#flash).
In this project it holds the saved settings and the Wi-Fi name and password, and, only while a new firmware is on [probation](#probation), that firmware's boot counter. It survives restarts and updates, and it is not encrypted.
Read [architecture.md](architecture.md), "What the board remembers".

## One-second rule

The project's hard rule: once the board is online, the panel never goes more than 1 second without drawing a new picture.
It works because slow work (network, [flash](#flash), name lookups) never runs on the drawing [task](#task).
Read [architecture.md](architecture.md), "The one-second rule".

## Origin and Host check

The portal's guard against other web pages. It refuses a request that changes something (every POST) when its `Origin` (or `Referer`) names another website, with `403`. It also refuses, with `421 Misdirected Request`, any request that does not name the display itself (its `<name>.local`, its bare name or its IP address) in the `Host` header, which blunts DNS rebinding.
So a web page in your browser cannot change the display's settings. Programs that send no `Origin` or `Referer`, such as [curl](#curl), still can: there is still no password. In setup mode the Host check is off.
Read [security.md](security.md), "The portal: plain HTTP, no password".

## OTA

Over the air: sending new firmware through the network instead of over a USB cable.
In this project the board pulls an update from your service, or you upload a `.bin` file in the [portal](#portal). Both need the [update code](#update-code).
Read [operations.md](operations.md), "Firmware updates".

## Panel protection

The settings that keep an LED panel from wearing unevenly (see [burn-in](#burn-in)): [pixel drift](#pixel-drift), dimming after a while without a new plane, [night blanking](#night-blanking) and optional swapping of the weather and clock screens.
They are in the portal's "Panel protection" section.
Read [architecture.md](architecture.md), "Panel protection" and [operations.md](operations.md), "Panel protection: dimming, pixel drift, night blanking".

## Partition

One named room of the board's [flash](#flash). A small table near the start of the flash says where each room begins.
This project's table is `arduino/ledmatrix/partitions.csv`: `nvs`, `otadata`, two program slots `ota_0` and `ota_1`, `uf2`, `ffat` (unused) and `ident`. Every build needs this file.
Read [build.md](build.md), "Check the partition table" and [architecture.md](architecture.md), "The flash layout".

## Password manager

A program that stores your passwords and other secrets safely.
In this project it is the default place to back up the [master secret](#master-secret).
Read [install.md](install.md), "Make the master secret and back it up".

## PATH

The list of folders in which the [Terminal](#terminal) looks for the programs you type. "command not found" can mean the program is not installed, or that its folder is not on the PATH.
On an Apple silicon Mac, [Homebrew](#homebrew)'s installer prints two lines that put `brew` on it. On Linux an `export PATH=...` line does the same for [arduino-cli](#arduino-cli), for the current window.
Read [build.md](build.md), "Terminal in 2 minutes".

## Pin 1

The first pin of a connector. A cable's red stripe marks its pin 1, and it must meet pin 1 of the socket.
In this project the red stripe of the [ribbon cable](#ribbon-cable) goes to pin 1 of the panel's IN socket. The sources do not say in words which end is pin 1, so use the markings on your panel and board.
Read [install.md](install.md), "Parts, wiring and power".

## Pixel drift

A [panel protection](#panel-protection) feature: still pictures move one pixel every 3 minutes, in a four-step loop. Screens keep column 63 and row 31 black so that nothing falls off the edge.
Read [architecture.md](architecture.md), "Panel protection".

## Polarity

Which of two power wires is plus (+, usually red) and which is minus (-, usually black). Connected the wrong way round, a supply can damage the panel.
In this project you check the polarity with a [multimeter](#multimeter) before the panel's power cable goes into the panel.
Read [install.md](install.md), "Parts, wiring and power".

## Portal

The settings web page that the display serves itself on your home network, at `http://ledmatrix.local`. You use it to choose what to show, set your location, check problems and allow updates.
It uses plain HTTP and has no password, so anyone on your Wi-Fi can open it; a firmware update still needs the [update code](#update-code) from the panel. A web page in your browser cannot change its settings (see [Origin and Host check](#origin-and-host-check)), but a program on your network that sends the commands directly can.
Read [install.md](install.md), "First look at the portal" and [security.md](security.md), "How the system is protected".

## POSIX time zone rule

A compact text, such as `WET0WEST,M3.5.0/1,M10.5.0/2`, that tells a device a time zone's offset from UTC and when daylight saving starts and ends.
In this project the [portal](#portal) turns your map pin into a zone name such as `Europe/Lisbon`, looks up its rule in a table, and saves both on the board.
Read [architecture.md](architecture.md), "Networks and time".

## Preview page

`preview/index.html`: a web page that draws every screen of the display in your browser with example data, using the same drawing code as the panel. You need no board, so you can see every screen before you buy anything.
Read [architecture.md](architecture.md), "The big picture".

## Probation

The trial period of a freshly installed firmware. Within 3 minutes it must join Wi-Fi, serve the [portal](#portal), draw 100 frames with none more than 1 second apart, and get one answer from your service. If it passes and 60 seconds have gone by, it is kept. If it fails, or the board has started 3 times without passing, it goes back to the old image ([rollback](#rollback)) on the 4th start. The count is kept in flash and every start counts, so a power cut counts like a crash.
A firmware flashed over USB is never on probation.
Read [architecture.md](architecture.md), "Firmware updates".

## Problem list

The display's own list of its last 10 problems and notable events, in plain English, newest first. It is in the portal's Problems section and at `/api/problems`, and it is empty after a restart. A rollback of a new firmware appears here too, under the kind "update check".
When one kind of data has failed for a full hour, the panel also shows "PROBLEM" for 5 seconds once a minute; events such as a rollback show only in the list.
Read [operations.md](operations.md), "Reading the Problems section".

## Protomatter

Adafruit's library that drives a [HUB75](#hub75) panel. It refreshes the panel by itself and is double buffered.
It has no brightness control, so the firmware scales each finished frame instead.
Read [architecture.md](architecture.md), "The firmware from the inside".

## Provisioning

Giving a board its identity: `tools/provision.py` makes the board's [device key](#device-key) from your [master secret](#master-secret) and writes an identity record into the [ident partition](#ident-partition).
It happens at home over USB, never over the air.
Read [install.md](install.md), "Provision the display".

## Public image

The service as a ready-made [Docker image](#docker-image), `ghcr.io/suvl/ledmatrix-api`, as the [maintainer](#maintainer) publishes it. It holds the service and the firmware that matches it.
You run it; you do not build it. Firmware updates reach your display when the maintainer publishes a new image and you run it.
Read [hosting.md](hosting.md), "The image" and [README.md](../README.md), "Words used in these guides".

## RAM

The board's fast working memory. It loses its contents at a restart.
In this project the live data, the cached logos, the problem list and the plane log live in RAM. `heap` in `/status` is the free part of it.
Read [architecture.md](architecture.md), "Privacy and trust, in plain words".

## Registry

A server that stores [Docker images](#docker-image) for others to pull.
In this project the public image is at `ghcr.io` (GitHub's registry). If you add your own logos, keep that image in a private registry.
Read [hosting.md](hosting.md), "The image" and [hosting.md](hosting.md), "The logo set".

## Release

A new firmware version that boards will offer to install.
A release is a push to the `main` branch with a higher number in `version.h`; the [CI](#ci) workflow builds it. Only the [maintainer](#maintainer) does this for the [public image](#public-image).
Read [development.md](development.md), "Releasing".

## Replay

Sending again a copy of a request that someone captured.
A signed request can be replayed for 2 minutes, while its timestamp is still valid. All a replay gets is public data.
Read [security.md](security.md), "Known limits".

## Repository root

The top folder of the downloaded code: `~/ledmatrix` once you have cloned the repository. `cd ~/ledmatrix` takes you there.
Run every command from it unless the page tells you otherwise.
Read [build.md](build.md), "Terminal in 2 minutes" and [build.md](build.md), "Get the code".

## Reverse proxy

A program that sits in front of another one. It receives the requests from outside and passes them on.
In this project it holds the [certificate](#certificate), speaks HTTPS to the display and passes plain HTTP to the service on port 8080. [Caddy](#caddy) is the one the hosting guide uses.
Read [hosting.md](hosting.md), "Path 1: a home server".

## Revoked list

The service's own list of device IDs that it refuses with the reason `revoked`, even when the signature is valid. You keep it in `REVOKED_DEVICES`.
Remove an ID to let the display back in: the service never bans a display for good.
Read [operations.md](operations.md), "Revoke a display, and allow it again".

## RGB565

A way to store one colour in 2 bytes: 5 bits of red, 6 of green and 5 of blue.
In this project a 16 by 16 logo is 512 bytes of RGB565.
Read [hosting.md](hosting.md), "The logo set".

## Ribbon cable

A flat cable with many wires side by side. The one in the Waveshare box has 16 pins and is about 30 cm long.
In this project it carries the picture signals between the board and the panel's IN socket. It carries no power. Its red stripe marks [pin 1](#pin-1).
Read [install.md](install.md), "Parts, wiring and power".

## Rollback

Going back to the old firmware when a new one fails its [probation](#probation).
The board's [bootloader](#bootloader) does not do this, so the firmware does it itself, using a state record and a boot counter that is kept in flash. A version that was rolled back is not offered again until a newer one exists. A rollback is listed in the [problem list](#problem-list) but does not put the PROBLEM message on the panel.
Read [operations.md](operations.md), "Rollback: when the new version does not pass" and [architecture.md](architecture.md), "Firmware updates".

## ROM bootloader

The chip's own built-in [bootloader](#bootloader). It cannot be erased, so you can always get back to it.
If an upload cannot connect, hold BOOT, tap RESET, then release BOOT and upload again (Adafruit's procedure; not tested here).
Read [install.md](install.md), "First flash".

## RSA and ECDSA

Two kinds of key that a [certificate](#certificate) can hold.
The display accepts both, but a certificate with an RSA key (2048 bits) lets it make its first secure connection much faster: about 0.4 seconds against about 2.3 in the project's own measurements. So ask for RSA.
Read [hosting.md](hosting.md), "What the display needs" and [hosting.md](hosting.md), "Background".

## Screw terminal

A connector where you fix a bare wire under a screw.
In this project the DC jack adapter has two, marked + and -: the panel's power cable goes under them, red under + and black under -. The board's own screw terminals stay unused.
Read [install.md](install.md), "Parts, wiring and power".

## Secure boot and flash encryption

Two protections that the [ESP32](#esp32) chip offers and this project does not use. Secure boot makes the chip start only programs that you signed. Flash encryption scrambles the contents of the flash so that a cable cannot read them.
Without them, someone who holds the display can read the saved Wi-Fi password and location and can run a program of their own. That is why you should revoke a lost display.
Read [security.md](security.md), "How the system is protected" and [security.md](security.md), "Known limits".

## Serial monitor

A tool that shows the text the board prints about itself over USB.
Start it with `arduino-cli monitor --port /dev/cu.usbmodem1234 --config baudrate=115200`.
Read [install.md](install.md), "First flash".

## Serial port

The name your computer gives the USB connection to the board, such as `/dev/cu.usbmodem1234` on macOS or `/dev/ttyACM0` on Linux.
You need it to flash the board.
Read [install.md](install.md), "First flash" and [build.md](build.md), "Find the serial port".

## Service

The small program `ledmatrix-api` that you run. It fetches flights, weather and headlines from public sources, shrinks them to a few hundred bytes of [JSON](#json) for the display, and also serves firmware images.
It runs in one [container](#container), keeps nothing on disk and checks the signature of every data request.
Read [architecture.md](architecture.md), "The service" and [hosting.md](hosting.md), "What the display needs".

## Setup mode

The state in which the board makes its own Wi-Fi network, named `ledmatrix` (or your [display name](#display-name), once you have set one), so that you can tell it your home Wi-Fi. A random 8-character password is shown on the panel. You then open `http://192.168.4.1`.
A board with no saved Wi-Fi starts in setup mode. To start it again, hold the UP button for 5 seconds.
Read [install.md](install.md), "Wi-Fi setup (setup mode)".

## SHA-256

A way to turn any file into a 64-character fingerprint. A different file gives a different fingerprint.
In this project the [manifest](#manifest) carries the SHA-256 of the firmware image, and the display checks it before it switches to the new image.
Read [architecture.md](architecture.md), "Firmware updates".

## Signed request

A request that carries three headers: `X-Device` (the [device ID](#device-id)), `X-Timestamp` and `X-Signature`. The signature is an [HMAC](#hmac) of the method, the host, the address and the time, made with the [device key](#device-key).
The service answers `401` unless the signature is valid. The firmware routes are the one exception: they are open on purpose.
Read [architecture.md](architecture.md), "Identity and trust".

## Sketch

An Arduino program. The firmware is one sketch: the folder `arduino/ledmatrix/`, with a main file `ledmatrix.ino` and the files next to it.
Read [build.md](build.md), "What you need".

## Soft refusal

What happens when the service refuses a display's key: the display keeps its last data, tries again every 5 minutes, never wipes anything and never restarts. It says why in its [problem list](#problem-list).
The service never bans a display for good.
Read [architecture.md](architecture.md), "Identity and trust".

## SSH and scp

`ssh` logs you in to another computer over the network and gives you its [Terminal](#terminal). `scp` copies files to or from it.
In this project you meet them when the service runs on another computer, such as a rented server.
Read [hosting.md](hosting.md), "Path 2: a small rented server".

## Stale data dot

A small blinking orange square in the top right corner of the panel. It means the data is old: flights were last fetched more than 5 minutes ago, or weather more than 2 hours ago (or nothing has arrived a few minutes after start-up).
It goes away by itself when fresh data arrives.
Read [operations.md](operations.md), "The screens and when each appears".

## sudo

A word you put in front of a Linux command to run it with administrator rights.
You will see it in the Linux notes, for example `sudo apt install git`.
Read [hosting.md](hosting.md), "Path 1: a home server".

## Task

A program that runs alongside others on the same chip. A small operating system called FreeRTOS schedules them.
In this project the firmware has a render task that draws, a net task for Wi-Fi and the [portal](#portal), a data task for everything that talks to the service, and a short-lived [probation](#probation) task.
Read [architecture.md](architecture.md), "The tasks".

## Terminal

The program on your computer where you control it by typing commands. On a Mac, press Command+Space, type `Terminal` and press Enter. You paste a command, press Enter and read what it prints. The program that reads your commands is called the shell; on a Mac it is `zsh`.
Leave out any line that starts with `#`: in `zsh` it is not a comment and prints an error. See [folder and path](#folder-and-path), [curl](#curl) and [nano](#nano).
Read [build.md](build.md), "Terminal in 2 minutes".

## TinyUF2

Adafruit's small USB updater that ships with the board. It is the board's [bootloader](#bootloader) and sits in the `uf2` [partition](#partition).
Nothing protects it from a tool that does not know about it, so you flash only with `arduino-cli upload`. It does not roll back a failing update.
Read [install.md](install.md), "Background".

## TLS

Transport Layer Security: the encryption and identity check behind HTTPS. People also call it a secure connection.
In this project the display uses TLS to reach your service, checks the [certificate](#certificate) against its [CA bundle](#ca-bundle) and never skips the check. Because certificates have dates, the clock must be right.
Read [architecture.md](architecture.md), "Networks and time".

## Two-factor sign-in

A second proof besides your password, such as a code from an authenticator app or a security key.
Turn it on for your GitHub, domain registrar and DNS provider accounts.
Read [security.md](security.md), "Before you start".

## u.FL connector

A tiny socket for an external antenna.
Adafruit sells a version of the board with one (product 6475) and says you must connect an antenna to it. The version with a PCB antenna (product 5778) needs none.
Read [install.md](install.md), "Parts, wiring and power".

## Update code

A random 6-digit code that appears only on the panel. You type it into the [portal](#portal) to allow a firmware update or an uploaded `.bin` file.
It lives 3 minutes and allows 3 wrong tries. After 3 wrong tries a new start is refused for 60 seconds, then 120, doubling up to an hour. It proves that someone is standing in front of the display.
Read [operations.md](operations.md), "Firmware updates" and [security.md](security.md), "How the system is protected".

## Variants

A short list, at the end of a step, of other ways to do it, for people who have a reason.
The Variants box in the wiring step covers powering panel and board from one supply (the author's way), feeding everything through the board's screw terminals (Adafruit's way), plugging the board straight into the panel, and a bigger supply.
Read [install.md](install.md), "Parts, wiring and power".

## Version marker

A few bytes inside the firmware image, `LEDMATRIX-FW-VERSION` followed by the version, that say which version the image is.
The board reads the marker back after writing an image and refuses it if it differs from the [manifest](#manifest). `tools/make_manifest.py` checks it too.
Read [development.md](development.md), "Inside a build" and [development.md](development.md), "Releasing".

## Volt and amp

Volts (V) measure the electrical push of a power supply, and amps (A) measure how much current it can deliver.
The panel needs a 5 V supply that can deliver at least the current Waveshare asks for. Do not guess either number, and never use another voltage.
Read [install.md](install.md), "Parts, wiring and power".

## WPA2 and WPA3

The two current security standards for Wi-Fi passwords.
In this project the display's own setup network uses WPA2, and the display will not join an open or older network. Use a long password.
Read [security.md](security.md), "How the system is protected".

## Your own build

An image or firmware that you build yourself, instead of running the [public image](#public-image).
Then no update reaches your display unless you build and publish a [release](#release) yourself, and your own image carries no firmware unless you put one in. You are the [maintainer](#maintainer) of your copy.
Read [development.md](development.md), "Which case are you?" and [hosting.md](hosting.md), "The image".
