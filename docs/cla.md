# Contributor agreement

**This is not legal advice, and it has not been reviewed by a lawyer.** `release-plan.md` §5
says a dual-licensing plan is worth twenty minutes of a real lawyer's time before the first
binary ships; this file is the draft that conversation should start from, not its conclusion.

By submitting a contribution to kvit-dictate — a pull request, a patch, a suggested edit, in
any form and through any channel — you agree to the following.

## 1. You have the right to give it

The contribution is your original work, or you have the right to submit it under these terms.
If your employer has rights in work you produce, you have their permission to contribute it. If
the contribution includes anything written by someone else, you say so in the pull request and
name its licence.

## 2. What you grant

You grant Kvit (the project's author, and their successors and assigns) a perpetual, worldwide,
non-exclusive, royalty-free, irrevocable licence to reproduce, prepare derivative works of,
publicly display, publicly perform, sublicense and distribute your contribution and derivative
works of it, **under any licence terms, including proprietary ones**.

You also grant every recipient of the project a patent licence on the same terms as the
outbound licence for the file you touched: Apache-2.0 §3 for `server/`, `bench/` and
`docs/protocol.md`; MPL-2.0 §2.1(b) for `src/`, `tests/` and `tools/`.

You keep your copyright. This is a licence, not an assignment: you can do anything you like with
your own contribution elsewhere, including relicensing it.

## 3. What you get, and what users get

Your contribution is published under the project's outbound licence for that path — MPL-2.0 for
the client, Apache-2.0 for the protocol and the reference server — and that publication is not
revocable. Nothing in §2 lets a future proprietary build take an already-published version away
from anyone.

## 4. Why §2 asks for more than the outbound licence

Stated plainly, because a contributor agreement that hides its purpose deserves the suspicion it
gets: §2 preserves the option of a paid closed-source build of the client. That option exists
only while one party holds a licence broad enough to relicense the whole work, and it ends at
the first merged contribution that does not grant one. Whether that build is ever made is
undecided. The agreement exists so that the decision stays open, not because it has been taken.

If that trade is not one you want to make, say so in the pull request. A bug report that
describes the fix precisely enough for someone else to write it is genuinely welcome and
requires no agreement.

## 5. No warranty

You provide your contribution "as is", without warranties or conditions of any kind, express or
implied.

## 6. Recording your agreement

Add a `Signed-off-by: Your Name <your@email>` line to each commit (`git commit -s`), and say in
the pull request that you agree to `docs/cla.md`. That is the whole process.
