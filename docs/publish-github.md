# Publishing to GitHub, and the author rewrite before it

2026-11-19. Public at <https://github.com/kvit-s/kvit-dictate>, licence MPL-2.0.

## What was rewritten

Before publishing, a former work address was taken out of the history. It had been the author
and committer of the 93 oldest commits (2026-08-16 to 08-22). Dates, messages and files were
kept, with one exception: the audit row in `docs/release-audit.md` §3 that named the address.

| Author and committer | Commits on `main` after |
|---|---|
| `Kvit <info@kvit.app>` | 179 (the 93 rewritten plus the 86 that already used it) |
| `Kvit Agent <agent@kvit.local>` | 5 |

The tool was `git filter-repo`, run from a temporary Python environment, with a mailmap that
maps both old author identities to `Kvit <info@kvit.app>`, a `--replace-text` list for file
contents, and `--refs main`. The two input files held the old address, so they were kept
outside the project and are not reproduced here.

## Checked

* A search of every name, address, message and diff on `main` finds the old address 0 times,
  both in this repository and in a fresh clone from GitHub.
* `main` has 184 commits before and after. The final files differ in the two documents that
  described the address, and nowhere else.
* GitHub's `main` is the same commit as the local one (`9d5f23e`), and GitHub answers "No commit
  found" for the old tip.

## What it changed

* **Every commit ID on `main` changed.** The old ones are kept on this machine: in the track
  branch, the reflog, and the backup
  `D:/projects/kvit-dictate-backup/kvit-dictate-before-rewrite-2026-11-19.bundle`.
* **Six IDs quoted in files now point at old commits**: `41ef07e`, `4805a1d`, `89d012e`,
  `ea8d22f` in `progress.md` (`ea8d22f` is also in `src/injector.cpp`), `59b0a57` in
  `openai-translator.md` and `release-plan.md`, and `c6526cc` in `openai-translator.md`.
  Commit messages were updated automatically. Quotes in files were not.
* **The track branch `track/this-project-is-intended-58855e` still has the old history.** Its
  work is already in `main`. Archive it, and never push it or merge it again: either would bring
  the old author lines back.

## GitHub

The first private repository, which held the old history, was renamed `kvit-s/kvit-dictate-old`
and is still private. A fresh `kvit-s/kvit-dictate` got the rewritten `main`, because a
force-push leaves the replaced commits reachable by ID. Delete `kvit-dictate-old` in the browser
(Settings → Delete repository): the current GitHub login lacks the `delete_repo` permission.

No remote is stored in the local git settings. Pushes go to
`git@github.com:kvit-s/kvit-dictate.git` by address.
