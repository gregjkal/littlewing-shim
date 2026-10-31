---
name: release
description: Cut a LittleWing release on GitHub with tools/release.sh. Use when asked to cut, make or ship a release.
---

`tools/release.sh` does the build, tests, notarization, tag and draft release. Its header says what it checks and needs. These steps cover the judgment around it.

1. **Version.** List what's new: `git log --oneline $(git describe --tags --abbrev=0 origin/main)..origin/main`. A new game or a feature players notice bumps the minor version (0.3.0 to 0.4.0). Only fixes bump the patch. Tell the user the version and the reason, then go on.

2. **Notes.** The draft's notes are `tools/release_notes.md` followed by GitHub's list of pull requests. The user edits a draft on GitHub before publishing it, so the latest published release is the reference: `gh release view --json body -q .body`. The template is current when, with `@VERSION@` set to that release's version (its tag without the `v`), it matches that release's text above GitHub's generated part, which starts at `## What's Changed` (GitHub's editor saves CRLF line endings, so compare without them), and each sentence is true of the app this release ships, checked against the README intro and the pull requests from step 1. Carry the user's lasting edits into the template, but leave out lines about that release alone, and ask the user when you can't tell which a line is. When the template isn't current, fix it in a pull request and wait for the user to merge it, since the script releases only a `main` that matches `origin/main`.

3. **Summary.** Under `## What's Changed`, the user wants one sentence, above the list, that sums up the release in terms a player notices. Propose it from the pull requests in step 1, leaving out internal fixes the list already names, and wait for the user to agree. The script takes it as `LOONY_RELEASE_SUMMARY` and puts it in both the draft and `appcast.xml`, whose notes are what Sparkle shows in the app's update dialog, so it has to be settled before the run: editing the draft afterwards doesn't reach the feed. It belongs to this release alone, so it stays out of `tools/release_notes.md`, and it sits in GitHub's part, which step 2 doesn't compare.

4. **Run.** Run the script from the main checkout, on `main`. It can't run from a worktree, since `main` is checked out in the main checkout. The signing identity is the `Developer ID Application` line of `security find-identity -v -p codesigning`. The build compiles Unicorn and SDL3 from source and waits on Apple's notary, so run it in the background with its output in a log:
   ```sh
   LOONY_SIGN_ID="<identity>" LOONY_RELEASE_SUMMARY="<sentence>" tools/release.sh <x.y.z> > <log> 2>&1; echo "exit=$?" >> <log>
   ```
   Single-quote the sentence if it has `"`, `$` or a backtick, writing an apostrophe as `'\''`.
   Reading the Sparkle key, it can stop at a macOS dialog asking to let `generate_keys` or `generate_appcast` use the keychain. When the log ends at a line saying so, tell the user to click Always Allow.

5. **Check.** The release is good when all of these hold:
   - the log ends with `exit=0`, and its test line says `0 failed`;
   - `git ls-remote --tags origin 'refs/tags/v<x.y.z>^{}'` is the `origin/main` commit from step 1. Other sessions switch branches in this checkout, and the script tags `HEAD`;
   - `gh release view v<x.y.z>` is a draft with `LittleWing-<x.y.z>.zip`, `LittleWing.zip` and `appcast.xml` attached, and its notes open with the template and have the summary under `## What's Changed`;
   - `build-dist/updates/appcast.xml` offers version `<x.y.z>` at `releases/download/v<x.y.z>/LittleWing-<x.y.z>.zip`, with a `sparkle:edSignature`, and its notes have the summary.

6. **Hand off.** Give the user the draft and the command to publish it after they've looked: `gh release edit v<x.y.z> --draft=false --latest`. Publishing is what makes released apps offer the update, since they read the latest release's `appcast.xml`.
