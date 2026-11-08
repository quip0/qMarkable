# One-shot batch rename

Renames documents and folders on the tablet through the reMarkable app itself
(`LibraryController.setVisibleName`, the call the stock Rename dialog uses), so
cloud sync treats every change as a normal rename.

1. Write `rename.json`: `[{"id": "<uuid>", "from": "<current name>", "to": "<new name>"}, ...]`
   (kept out of git: it contains your document titles).
2. Back up metadata on the tablet:
   `cd ~/.local/share/remarkable/xochitl && tar -cf ~/rename-backup-$(date +%F).tar *.metadata`
3. Copy `rename.json` to `~/.config/qmarkable/rename.json`, and `renameBatch.qmd` to
   `~/xovi/exthome/qt-resource-rebuilder/zz-renameBatch.qmd`, then delete any old `~/.config/qmarkable/rename.done`.
4. `systemctl reset-failed xochitl; ~/xovi/start`, then wake and unlock the tablet.
5. Read `~/.config/qmarkable/rename.log`. Entries are only renamed if their current
   name equals `from`, and the rest are logged as MISMATCH or MISSING. `rename.done` stops a second run.
6. Remove `zz-renameBatch.qmd` and restart xovi again.

Undo: rename back the same way with `from`/`to` swapped, or restore the backup's
`.metadata` files with xochitl stopped.
