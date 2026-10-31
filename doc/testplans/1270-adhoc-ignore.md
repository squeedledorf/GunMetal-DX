# Conference ignore preferences

The existing per-account `SlipstreamIgnoreAdHocSessions` integer remains authoritative:
0 disables rejection, 1 allows friends through, 2 rejects all non-exempt conferences.
The three checkboxes do not migrate or overwrite saved values on opening Preferences.
The rejection panel binds the integer so the ordinary Preferences snapshot/Cancel path
continues to restore it; the reporting checkbox retains its existing account setting.

## Settings and search

- Start with each saved integer value, open Preferences > Chat > Reject, and verify the
  master and friends-exception checkboxes reflect it. Reporting retains its saved value.
- Toggle the master and friends exception, Apply, relog and verify persistence. Check
  Cancel restores both mode and reporting, including after Apply and account changes.
- Both indented options are disabled while the master is off. Disabling and enabling
  within the same dialog preserves the selected friends exception.
- Search Preferences for conference, ad-hoc and ignore. Each must find the master option.
- Check layout at 100% and 150% UI scale and the translated fallback without old combo entries.

## Incoming paths

Using another account, send first-message text invitations, immediate conference invites,
and conference voice invites. Test rejection with voice disabled as well as enabled.
Cover friends and non-friends for each mode and report-checkbox combination.

- Off: ordinary handling, no rejection report.
- Friends exception enabled: live friends pass; non-friends are declined.
- Friends exception disabled: both are declined. Adding/removing a friend affects the next
  invitation without reopening Preferences or relogging.
- Ignored text invitations must not subsequently issue an acceptance request or create an
  IM session, history fetch, sound, unread count or message-content notification.
- When reporting is enabled, nearby history gets a system line with the inviter's avatar
  link only; no invitation contents or dedicated notification toast. Reporting off is silent.
- Check name-link anonymization under name-hiding restrictions.
- Group sessions, direct IMs/P2P calls, self-originated sessions and existing joined sessions
  retain their ordinary behavior. Linden-originated invitations are exempt from this policy.
- Check capability decline success/failure and the no-capability leave-message fallback.

Source/static checks do not establish server decline behavior. Aggregate compilation and
the account/simulator/UI scenarios above remain required; no full viewer build or launch
was performed for this change.
