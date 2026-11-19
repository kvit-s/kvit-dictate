# Taking the work address out of the history, then publishing

2026-11-19. GitHub: `kvit-s/kvit-dictate`, private, holding `main` as of `568bac2`.

## What changes

Every author and committer line is rewritten. Dates, messages and files stay as they are.

| Now | After |
|---|---|
| the former author line (92 commits, 2026-08-16 to 08-22) | `Kvit <info@kvit.app>` |
| the former author line (1 commit) | `Kvit <info@kvit.app>` |
| `Kvit <info@kvit.app>`, `Kvit Agent <agent@kvit.local>` | unchanged |

The address also appears in one file: the row in `docs/release-audit.md` §3 that names it. In
every commit that has this file, the row reads "a former address" instead.

The tool is `git filter-repo`, installed in a temporary Python environment, not in the project:

```
mailmap:       Kvit <info@kvit.app> the former author line
               Kvit <info@kvit.app> the former author line
replace.txt:   a former address==>a former address
               a former address==>a former address

git filter-repo --mailmap mailmap --replace-text replace.txt --refs main --force
```

## Dry run, in throwaway copies of the repository

* Afterwards 179 commits are `Kvit <info@kvit.app>` and 7 are `Kvit Agent`. Searching every
  name, address, message and diff finds `former` 0 times.
* There are 183 commits on `main`, as before. Compared with the old `main`, the final files
  differ in one line only, the audit row.
* Running with `--refs main` rewrites only `main`. The old commits stay in this machine's
  repository, so the app's records of past turns still point at commits that exist. The
  working copy comes out clean.

## What it costs

* **Every commit ID on `main` changes**, starting with the first commit (`51db576` →
  something new). In the dry run, `568bac2` became `78b9c6c`.
* **Six IDs quoted in files will point at old commits**: `41ef07e`, `4805a1d`, `89d012e`,
  `ea8d22f` in `progress.md` (`ea8d22f` is also in `src/injector.cpp`), `59b0a57` in
  `openai-translator.md` and `release-plan.md`, and `c6526cc` in `openai-translator.md`.
  Commit messages are updated automatically. These quotes in files are not.
* **The track branch `track/this-project-is-intended-58855e` keeps the old history.** Its work
  is already in `main`, and it has never been pushed. Archive it rather than push or merge it
  again; merging it would bring the old commits back into `main`.

## Steps

1. Save a backup of the whole repository first:
   `git bundle create <outside the project>/kvit-dictate-before-rewrite.bundle --all`.
2. Run the command above in `D:/projects/kvit-dictate`.
3. Rename the GitHub repository that holds the old commits to `kvit-dictate-old`, and create a
   new, empty `kvit-s/kvit-dictate`. A force-push alone would leave the old commits on GitHub,
   still reachable by ID. Push the rewritten `main` to the new repository.
4. Check what GitHub holds: the same ID as local `main`, and no `former` in its authors.
5. Make it public: `gh repo edit kvit-s/kvit-dictate --visibility public
   --accept-visibility-change-consequences`.
6. Delete `kvit-dictate-old` in the browser (Settings → Delete). The current GitHub login does
   not have permission to delete repositories.
