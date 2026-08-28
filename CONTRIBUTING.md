# Contributing to kvit-dictate

Thanks for considering a contribution. This page covers building, testing, what gets merged,
and — the part that is unusual and that you should read before writing code — the terms
contributions are accepted under.

Before changing anything structural, read `plan.md`. It is the design document, and it holds
several invariants that look arbitrary until you know what they defend: text that has been
committed into the user's document is never revised, audio is never silently dropped to relieve
backpressure, and the app never elevates. A change that crosses one of those is not necessarily
wrong, but it has to argue with the record rather than around it.

`release-plan.md` is the narrower, later document that covers what shipping to other people
requires. If your change is about installation, configuration, packaging or the public protocol,
that is the one to read.

---

## Licensing of contributions

The tree is split, deliberately:

| Path | Licence | Why |
|---|---|---|
| `src/`, `tests/`, `tools/` — the Windows client | **MPL-2.0** (`LICENSE`) | File-level copyleft. Modifications to these files come back; the files can still be combined into a larger work. |
| `docs/protocol.md`, `server/`, `bench/` — the protocol and the reference server | **Apache-2.0** (`LICENSE-APACHE`) | A protocol spec wants to spread without anyone reading a licence first, and the point of the network binding is to get other people writing servers. |

Copyleft where the value is, permissive where the adoption is.

Every source file carries its side's header. `tools/apply-license-headers.sh` adds them and
`tools/apply-license-headers.sh --check` verifies the tree; the check is expected to pass on
every commit.

### The contributor agreement, and why there is one

**By submitting a contribution you agree to the terms in `docs/cla.md`.** In short: you certify
the work is yours to give, and you grant the project's author a licence broad enough to
relicense your contribution — including under a proprietary licence — alongside the outbound
open-source licence above. Your contribution also stays available to everyone else under that
outbound licence; nothing here takes rights away from users.

This is more paperwork than a typo fix deserves and we know it. The honest reason it exists:
the option of a paid closed build later exists only while one party holds all the copyright, and
it ends quietly at the first merged pull request from somebody else. Retroactive relicensing is
a negotiation with everyone who ever contributed; agreeing up front is a checkbox. We would
rather state the trade plainly than discover it later.

If you do not want to agree, say so in the pull request. A bug report describing the fix is
always welcome and needs no agreement at all.

---

## Building

Requirements: Windows 10 1809+ or 11 (x64), Visual Studio 2022 with the C++ workload, and
CMake 3.24+. There are no third-party C++ dependencies and there must not be any — there is no
`find_package()` in `CMakeLists.txt` and adding one is a design change, not a build change.

```
win-build.bat Release          :: configure, build, run ctest
win-build.bat Debug clean      :: wipe the binary directory first
```

The reference server needs CPython 3.12 or newer and, for the `mock` backend, nothing else at
all. That is load-bearing: the entire client can be exercised end to end on a bare interpreter.

## Testing

```
ctest --test-dir build-windows-msvc-release -C Release --output-on-failure
py -3.12 -m unittest discover -s server/tests -t .
```

Both suites are merge gates. The conformance fixtures under `tests/fixtures/` are shared by the
two sides on purpose: a framing change that only satisfies one of them is not a framing change,
it is a fork.

## What gets merged

* A change that makes a failure louder rather than rarer.
* A change that comes with the test that would have caught the bug.
* Documentation that corrects something this repository currently claims and gets wrong. The
  house style is to record the correction next to the mistake rather than quietly deleting it.

## What does not

* A third-party C++ dependency (see above).
* Anything that lets the client revise text it has already committed into the user's document.
* Anything that drops audio to relieve backpressure. Overload is fatal and loud by design;
  `docs/protocol.md` §9 explains why the alternative produces mysteriously bad transcription.
* A silent fallback where a loud failure belongs. If the app cannot do the thing, it says so.

## Reporting a security issue

Do not open a public issue. Mail the address in `README.md` with what you found and how to
reproduce it. Note the security posture first: `release-plan.md` §2.4 states that audio crosses
the network unencrypted and that only LAN and loopback endpoints are supported, so "the audio is
readable on the wire" is documented, not a vulnerability.
