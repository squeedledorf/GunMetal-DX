# Avatar moderation (#1250)

Use an owner/manager account and an alt in-world. Do not use a production crowd
for destructive checks. No simulator operation should occur until its confirmation
button is pressed.

## Menu visibility

| Context | Expected |
| --- | --- |
| Friends, Groups, Recent, Blocked | No Manage submenu; single-target Block/Report remain available at the root. |
| Nearby People | Manage appears only when some moderation action is permitted for the whole selection. Only usable actions appear inside it. |
| Radar, single/multiple selection | Parcel Freeze/Eject and estate actions appear only when permitted for every selected resident. |
| Avatar/attachment context menu | Moderation actions appear in Manage only when permitted; Block/Report are unaffected. |
| Avatar/attachment pie | More retains Freeze and Parcel Eject, with Send Home and Estate Kick in previously unused slices. Unusable slices are hidden. |

Personal land ownership, estate authority, and exact group rights on the agent's
current parcel are available synchronously on the first open. For a remote group
parcel whose group UUID is not available from the ownership overlay, the viewer
shows a temporary permissions-check notification, resolves the actual owner, and
refreshes the **same open menu**. Closing/reopening must not be necessary. Membership
or moderation powers in another group must not expose actions on that parcel.

Check with both default and alternate skins, including near screen edges, UI
scaling, name hiding, and a multi-selection containing the user's own avatar.

## Permission and confirmation matrix

| Authority | Parcel Freeze/Eject | Eject and Ban choice | Estate removal |
| --- | --- | --- | --- |
| Parcel owner | Yes | Yes | Only with estate authority |
| Owning group, `GP_LAND_ADMIN` only | Yes | No | No |
| Owning group, admin and banned-list powers | Yes | Yes | No |
| Group member without admin power | No | No | No |
| Admin power in an unrelated group | No | No | No |
| Estate owner/manager on another resident's parcel | Yes | Only with independent parcel ban power | Yes |
| Unrelated resident | No | No | No |

1. Exercise first-open menus, both a current and a remote group parcel, and a
   neighbouring simulator. Repeat after an ownership-overlay update and after
   group powers change. Opening menus must not alter land selection or hover data.
2. Cancel and close both eject dialog forms. Cancel is button 1 on the no-ban form;
   on the full form button 1 is Eject and Ban, and button 2 is Cancel. Close selects
   no button. None of the cancel/close cases may send a removal message.
3. Plain Eject must send `EjectUser` flags 0, and explicit Eject and Ban flags 1.
   The ban choice requires current banned-list power on **every** target parcel.
   Revoking ban power before confirming must abort, without silently downgrading.
4. Freeze/Unfreeze use flags 0/1 respectively. Estate Teleport Home uses
   `teleporthomeuser` with agent and target parameters; Estate Kick uses `kickestate`
   with the target parameter. Confirmations describe distinct actions. No parcel
   or estate access-list change should accompany removal-only operations.
5. Change list selection after opening a menu/dialog. People callbacks retain the
   original immutable selection; radar already captures its selection. Avatar/pie
   actions resolve the selected avatar at click time, then bind the confirmation.
6. Move or remove a target, change regions, transfer land, revoke powers, or relog
   while a dialog or permission request is outstanding. The whole batch must abort
   if a target/session/permission changes. Walking within the same confirmed parcel
   is allowed after a fresh identity check; moving during that query may safely abort.
7. Mixed authorized/unauthorized selections must be rejected in either order;
   duplicates must not cause duplicate sends. Estate owners cannot be estate-kicked
   or sent home. Self/null/unknown targets must be rejected even in god mode.
8. A slow or missing simulator reply must show wait feedback followed by an abort;
   inspect reason-coded `Moderation`/`ParcelIdentity` logs without names or UUIDs.
9. Check every send goes to its target region, not the agent's current simulator.

## Shared parcel identity regression

Parcel speakers and moderation both use
`LLViewerParcelMgr::requestParcelIdentity`. With parcel speakers enabled, verify
same-owner speakers on another parcel still qualify, other-owner speakers do not,
and teleporting or disabling/re-enabling the registry invalidates old replies.
Keep a land selection open during these lookups; its region, parcel, and selection
sequence must remain unchanged. Expired or wrong-sender replies must not update
selection, hover, agent-parcel, speaker, or moderation state.

Estate Ban's separate pre-existing handler is outside this change's scope.
