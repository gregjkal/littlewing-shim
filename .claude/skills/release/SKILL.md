---
name: release
description: Cut a LittleWing release on GitHub with tools/release.sh. Use when asked to cut, make or ship a release.
---

`tools/release.sh` does the build, tests, notarization, tag and draft release. Its header says what it checks and needs. These steps cover the judgment around it.

1. **Version.** List what's new: `git log --oneline $(git describe --tags --abbrev=0 origin/main)..origin/main`. A new game or a feature players notice bumps the minor version (0.3.0 to 0.4.0). Only fixes bump the patch. Tell the user the version and the reason, then go on.

2. **Notes.** Read `tools/release_notes.md` alongside the README intro and the pull requests from step 1. It's current when each sentence is true of the app this release ships: the games and versions it plays, and what the bundle carries. When a sentence is stale, fix the template in a pull request and wait for the user to merge it, since the script releases only a `main` that matches `origin/main`.

3. **Run.** Run the script from the main checkout, on `main`. It can't run from a worktree, since `main` is checked out in the main checkout. The signing identity is the `Developer ID Application` line of `security find-identity -v -p codesigning`. The build compiles Unicorn and SDL3 from source and waits on Apple's notary, so run it in the background with its output in a log:
   ```sh
   LOONY_SIGN_ID="<identity>" tools/release.sh <x.y.z> > <log> 2>&1; echo "exit=$?" >> <log>
   ```

4. **Check.** The release is good when all of these hold:
   - the log ends with `exit=0`, and its test line says `0 failed`;
   - `git ls-remote --tags origin 'refs/tags/v<x.y.z>^{}'` is the `origin/main` commit from step 1. Other sessions switch branches in this checkout, and the script tags `HEAD`;
   - `gh release view v<x.y.z>` is a draft with `LittleWing-<x.y.z>.zip` attached, and its notes open with the template.

5. **Hand off.** Give the user the draft and the command to publish it after they've looked: `gh release edit v<x.y.z> --draft=false --latest`.
