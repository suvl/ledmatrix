# Security policy

LedMatrix is a personal project, built and looked after by one person in their spare time. Security reports are
welcome and are taken seriously, but everything below is best effort: there is no team, no on-call and no promised
response time.

This page is about reporting a problem. What the project protects, what it deliberately does not, and what you must
keep up to date on your own display and service is explained in [docs/security.md](docs/security.md).

## How to report a vulnerability

Please report it **privately**, so that nobody can use it before it is fixed:

1. Open the **Security** tab of this repository on GitHub.
2. Choose **Report a vulnerability** (GitHub calls this private vulnerability reporting).
3. Fill in the form. Only you and the maintainer can see it.

If the Security tab has no **Report a vulnerability** button, do not put details in a public issue. Open an issue that
says only that you have a security report and ask how to send it privately, with no details, no secrets and no
screenshots of your settings, and wait for an answer.

## What to include

The more of this you can give, the faster the problem can be understood:

- **What is affected**: the firmware (and its version, which the display's settings page shows), the service (and the
  image tag or the commit it was built from), the settings page (the portal), a tool in `tools/`, or the build
  workflow.
- **What you did**, step by step, so that it can be repeated. Say whether you used a real display or only read the code.
- **What happened** and **what you expected**, and what an attacker gains from it.
- **Any idea for a fix**, if you have one.

Do not send a real master secret, a real device key, a real Wi-Fi password or the update code of a display. Use made-up
values; if a real one is the only way to show the problem, say so and ask how to share it. Test only against your own
display and your own service, never against other people's.

## What to expect

- This is a hobby project: an answer may take a while, and a fix takes as long as it takes. Nothing here is a promise or
  a service level.
- You will get a reply that says whether the report is understood, whether it is considered a vulnerability, and what
  happens next. If it is, the fix comes as a new release (a new firmware version and a new service image), and a
  security advisory is published on GitHub once the fix is available.
- You are welcome to be named in the advisory. Say so in your report if you want that, or if you want to stay unnamed.
- There is no bug bounty.
- Please keep the details private until the fix is released. If you have heard nothing for a long time, a polite
  reminder on the same report is fine. A common default is to wait up to 90 days before publishing anything.

## Supported versions

Only the **latest release** is supported: the newest firmware version, and the newest service image published from the
`main` branch. Older versions get no fixes of their own; the answer to a problem in one of them is to update. Both the
display and the service are updated from the same releases (see "Firmware updates" in
[docs/operations.md](docs/operations.md)).

## What is out of scope

These are not treated as vulnerabilities, and a report about one of them will usually be answered with a link to the
documentation:

- **Anything that needs physical access to a display.** Someone who can plug a cable into the board can read its flash
  memory, including the saved Wi-Fi password and location. Secure boot and flash encryption are not used. See
  [docs/security.md](docs/security.md).
- **The documented limits of the portal on your home network.** The settings page is plain HTTP on the local network,
  has no password, and anyone on that Wi-Fi can use it; the setup mode is open to anyone in Wi-Fi range; the clock comes
  from unauthenticated NTP. These are listed, with the reasons, under "Known limits" in
  [docs/security.md](docs/security.md). A way to do more than what is listed there, such as reaching the portal from
  outside the local network or running code on the display, is in scope.
- **Someone who already holds the master secret, or the maintainer's GitHub account.** The design trusts both.
- **How you host the service**: your reverse proxy, router, DNS provider, certificate and computer. Weaknesses in
  third-party software or services the project uses (an upstream data source, a library, GitHub itself) belong with
  their authors, unless this project uses them in a way that makes them worse.
- **Overwhelming a service with traffic**, and reports from automated scanners without a way to make use of what they
  found.
- **Social engineering** of the maintainer or of people who run a display.
