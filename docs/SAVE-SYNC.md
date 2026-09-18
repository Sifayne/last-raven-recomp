# Playing the same saves on more than one computer

Saves live in one folder per title:

```
~/.local/share/last-raven/saves/<slug>/ms/PSP/SAVEDATA/
```

`<slug>` is `aclr` (Last Raven), `ac3p` (Armored Core 3 Portable) or `acsl`
(Silent Line). The AppImage and a source build started through
`scripts/15-settings.sh` both write there, so the layout is the same on every
machine. `XDG_DATA_HOME` is honoured, the AppImage prints the path with
`--print-paths`, and the launcher prints it on each launch. To play from two
computers, keep `~/.local/share/last-raven/saves` the same on both with any
folder-sync tool. Nothing in the game needs configuring.

## Three names to leave out

A save is written by staging the new one in `.pending-<name>`, moving the old
one to `.backup-<name>` and holding `.savedata-lock` meanwhile, so a half-written
save is never in place. Those three are transient; tell the sync tool to
ignore them:

```
.pending-*
.backup-*
.savedata-lock
```

Frame captures (`*.ppm`) that diagnostics write into the same folder are safe
to ignore or delete.

## Syncthing

No account, no cloud, works on the local network, and the Deck can run it in
Gaming Mode.

1. **Desktop:** install your distribution's `syncthing` package and start it
   at login ([Syncthing autostart](https://docs.syncthing.net/users/autostart.html)):

   ```bash
   systemctl --user enable --now syncthing
   ```

   The web interface is at <http://localhost:8384>.
2. **Steam Deck:** in Desktop Mode install *Syncthing GTK*
   (`me.kozec.syncthingtk`) from Discover. To keep it running in Gaming Mode,
   install the [Decky Syncthing](https://github.com/theCapypara/steamdeck-decky-syncthing)
   plugin, choose *Installed via the "Syncthing GTK" Flatpak* in its wizard
   and enable starting with Gamescope.
3. **Pair** the two devices: *Add Remote Device* on each, using the ID the
   other one shows.
4. **Share the folder:** on one machine *Add Folder* with the path
   `~/.local/share/last-raven/saves`, share it with the other device, and
   accept it there with the same path. Keep the default *Send & Receive*.
5. **Ignore the transient names:** in the folder's *Ignore Patterns* (or a
   `.stignore` file in that folder, on each machine) put the three lines
   above. A pattern without a leading slash matches at any depth
   ([ignoring files](https://docs.syncthing.net/users/ignoring.html)).

Play on one machine at a time and let it catch up before switching; both
machines must be running for a sync to happen. If both were played before
they next met, Syncthing keeps both copies: the newer one keeps the name and
the other becomes a `.sync-conflict-…` file the game never reads. Pick the
one you want by hand.

## Dropbox, Nextcloud or a network share

Move the folder into the synced location and leave a symlink in its place.
On the first machine:

```bash
mv ~/.local/share/last-raven/saves ~/Dropbox/last-raven-saves && ln -s ~/Dropbox/last-raven-saves ~/.local/share/last-raven/saves
```

On the others, once the folder has arrived, only the `ln -s` (move any saves
already in their `saves` folder into the synced one first). The same
one-machine-at-a-time rule applies.

## Moving saves out of a source checkout

Before 17 September 2026 a source build wrote its saves to `<repo>/ms/`, all
titles together. Copy each save into its title's folder by the ID it starts
with: `NPUH10024` is `aclr`, `NPUH10023` is `ac3p`, `NPUH10025` is `acsl`.
`-n` never overwrites a save already there.

```bash
mkdir -p ~/.local/share/last-raven/saves/{aclr,ac3p,acsl}/ms/PSP/SAVEDATA
```

```bash
cp -a -n ms/PSP/SAVEDATA/NPUH10024* ~/.local/share/last-raven/saves/aclr/ms/PSP/SAVEDATA/
```

Repeat for the other two IDs. Check the game sees the save before deleting
the copy in `ms/`.

Direct runs (`scripts/06-boot.sh`, replays, the autotests) still use their
working directory as the memory stick and are unaffected.
