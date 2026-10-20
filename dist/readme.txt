totalcmd-git 1.0 for Total Commander
====================================

What posh-git is for PowerShell, totalcmd-git is for Total Commander: git
status in the command line prompt, the path shown to the left of the
command line above the F-key buttons:

    d:\projects\app [main ≡ +0 ~1 -0 !]>

The status is colored like in posh-git and refreshed in the background
whenever you change directory or switch back to Total Commander.


What is shown
-------------

    [main ≡ +1 ~2 -0 | +3 ~1 -0 !]
     |    | |          |          `-- ! working tree has changes,
     |    | |          |              ~ only the index has changes
     |    | |          `-- working tree: added (incl. untracked), modified, deleted
     |    | `-- index (staged): added, modified, deleted; !N = conflicts
     |    `-- ≡ in sync with upstream, ↑N ahead, ↓N behind, × upstream gone
     `-- branch, (abc1234...) when detached; |MERGING, |REBASE-i 2/5 etc.

Colors: branch cyan (in sync), green (ahead), red (behind), yellow
(diverged); index dark green; working tree dark red. Separate palettes are
used for light and dark panels.

Long paths are shortened in the middle (d:\…\src\app) so that the status
always fits.


Requirements
------------

- Total Commander 32-bit or 64-bit (tested with 11.56).
- Git for Windows (git.exe in PATH, or set GitPath in totalcmd-git.ini).


Installation
------------

1. Open the archive in Total Commander and confirm the plugin installation.
2. totalcmd-git asks whether to add a color filter that makes Total Commander
   load it at startup (see "How it works"). Answer Yes.
3. Restart Total Commander.

If you declined, or the filter could not be written, add it by hand:
Configuration > Options > Colors > Define colors by file type > Add >
Define..., on the Plugins tab choose plugin "totalcmd-git", property
"Autoload", operator "=", value "Yes". The color does not matter: the
filter never matches any file.


Settings
--------

totalcmd-git.ini is created on first start next to wincmd.ini (usually
%APPDATA%\GHISLER). If a totalcmd-git.ini exists next to the plugin, that one
is used instead, e.g. for portable setups. Changes are applied a second
after the file is saved, no restart needed.

[General]
  Enabled            0 turns the plugin off
  GitPath            full path to git.exe; empty = search PATH
  Timeout            time limit for one "git status" run, ms
  ShortenPath        shorten the middle of long paths
  RefreshOnActivate  re-run git status when TC becomes active again

[Format]
  BeforeStatus, DelimStatus, AfterStatus, BranchIdentical, BranchAhead,
  BranchBehind, BranchGone, LocalWorking, LocalStaged, Ellipsis
                     the symbols; quote values with leading or trailing
                     spaces, e.g. BeforeStatus=" ["
  ShowStatusWhenZero 0 hides zero counters: "+0 ~1 -0" becomes "~1"

[ColorsLight], [ColorsDark]
  Delimiters, BranchIdentical, BranchAhead, BranchBehind, BranchDiverged,
  BranchGone, Index, Working, Staged
                     colors as #RRGGBB; [ColorsLight] is used when TC
                     draws dark text, [ColorsDark] when it draws light text


How it works, and caveats
-------------------------

Total Commander has no plugin interface for its main window, so totalcmd-git
uses a few tricks. Please be aware of them:

- It is a content plugin (WDX) with a single dummy field, Autoload. TC
  loads content plugins on demand; a color filter that refers to the
  field makes TC load the plugin when it displays the first directory.
  The field is always "No", so the filter never colors anything.
- Once loaded, the plugin subclasses the prompt label and replaces TC's
  calls to DrawText/TextOut/ExtTextOut in TC's import table, so that the
  prompt text is drawn with the git status inserted. Nothing else is
  drawn differently.
- git status runs in a background thread (git --no-optional-locks status
  --porcelain=v2 --branch) with a time limit, so TC never waits for git.
  Directories outside a work tree do not start git at all.

This relies on undocumented details of how TC draws the prompt and may stop
working with a future TC version. If that happens, set Enabled=0 or remove
the plugin, and please report it.

Some antivirus products with machine-learning heuristics may flag the DLL
because it patches import tables of the process it is loaded into. This is
a false positive; the source code is available.

Errors are logged to %TEMP%\totalcmd-git.log.


Uninstallation
--------------

Remove the color filter ("Configuration > Options > Colors > Define colors
by file type"), then the plugin ("Configuration > Options > Plugins >
Content plugins"), and restart Total Commander.


License
-------

MIT, see LICENSE. Copyright (c) 2026 Dmitry Samodov.
