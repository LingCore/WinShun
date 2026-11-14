# Plugin settings synchronization: draft review

## Problem

Cursorwrap's search commands persisted private overrides while the settings
card edited the host's plugin configuration. Reapplying private overrides
after `settingsChanged` made the displayed and effective values disagree.
Comparing consecutive card values cannot provide bidirectional synchronization.

## Proposed implementation

`winshun/settings/update` carries a `settings` object. Session forwards it to
Host; Host derives the plugin identity from the session rather than the
payload. PluginSettings accepts declared, type-compatible, non-password keys,
stores them through the existing settings store, commits, and refreshes the
card and running plugin.

Companion cursorwrap changes are in its separate repository (0.10.0). They
emit the notification and stop replaying local overrides over host updates.
Old hosts ignore the notification; installing the plugin alone is insufficient.

The branch also preserves the related launcher fixes (drill hints and stable
number-key gutters) and separately commits the build/test issues encountered
during verification: `/bigobj`, interning disabled before test compaction,
and discovery of the installed VC runtime directory.

## Evidence already obtained

- Host Debug and Release builds completed in the original workspace.
- Release deployment and dependency audit completed after the CRT fallback.
- Host Release `tst_plugin`: passed, 45.58 seconds.
- Companion plugin protocol fixture: 147 checks passed, including host-side
  persistence simulation and disabling acceleration through the card.
- Earlier Debug `tst_core` passed after the test-contract fix (316 seconds).

These results precede extraction onto this branch. The complete combined
branch has not been rebuilt in this worktree; no full-suite green claim is made.

## Not tested / review requirements

- Real settings-card/search-bar round trip and restart using the deployed host.
- Focused tests of the new Session notification, host persistence and UI refresh.
- Echo ordering, rapid successive edits, unrelated-key preservation and errors.
- Capability negotiation / acknowledgement for older hosts.
- Schema constraints beyond the current basic type and declared-key checks.
- The companion currently sends a full snapshot: assess stale snapshot races.
- Review inherited launcher menu changes for duplicate drill entries and action
  index semantics; existing plugin tests alone do not prove this interaction.

The broader Debug run had failures and timeouts, and a subsequent run was
interrupted by a server restart. Those results do not establish that every
failure is pre-existing or environmental. This PR must remain a draft until
the relevant integration coverage and UI acceptance are complete.
