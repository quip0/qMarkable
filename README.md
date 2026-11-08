# qMarkable

UI mods for the reMarkable Paper Pure (firmware **3.28.0.172**), built on
[xovi](https://github.com/asivery/rm-xovi-extensions) + qt-resource-rebuilder.

## What's here

| Part | File | What it does |
|---|---|---|
| Homepage | `mods/qmarkableHome.qmd` | Full-screen **Home** page shown when xochitl starts (boot / `~/xovi/start`), not after sleep. "study →" closes it (to My Files); the calendar button in the My Files dock reopens it. Hides the stock Connect calendar dock button. Other mods add widgets through the `qmHomeWidgets` slot. |
| Study heatmap | `mods/studyHeatmap.qmd` | Today's study time; one calendar year (Jan–Jun over Jul–Dec) in 5 gray levels, swipe the grid sideways (or `<` `>`) to change year; tap a day for a collapsible view with an hourly bar chart and per-notebook totals. |
| Tracker | `tracker/qmarkable-trackerd.c` | Tiny background program that records time spent in notebooks and PDFs. |

### How study time is measured

- Time counts while a **notebook or PDF** is open on screen: the tablet is
  awake, the Home page isn't covering it, and it isn't locked. Ebooks don't count.
- The UI writes the active notebook (or "none") to `/run/qmarkable/current-doc`
  whenever that changes. This file is RAM-backed, so there are no flash writes.
- The tracker sleeps on an inotify watch of that file. It wakes only when it
  changes (a few times per session), plus one checkpoint every 5 minutes while
  a session runs. It doesn't read the pen or any input device.
- Session length uses a clock that stops during suspend, so sleep time is never
  counted, even if a sleep transition were missed.
- Data is saved when a session ends and at each checkpoint:
  - `/home/root/.local/share/qmarkable/index.txt`: daily totals
  - `/home/root/.local/share/qmarkable/days/YYYY-MM-DD.txt`: seconds per hour and per document
- Hours and days follow the tablet's system timezone (`/etc/localtime`), and
  the tracker notices when it changes. You can override it with `tz=` in the config.

### Timezone

The firmware has no timezone setting, and `/etc` is rebuilt from RAM on every
boot, so the tablet defaults to UTC. Put a zone name in
`~/.config/qmarkable/timezone` (e.g. `Asia/Taipei`). The xovi pre-start hook
then applies it every time xovi starts, before the UI loads. Leave the file
empty to keep the system default.

Heatmap levels: none, <30 m, 30 m–1 h, 1–2 h, 2 h+.

## Install / update

Requires xovi + qt-resource-rebuilder on the tablet, SSH access, and `zig`
(`brew install zig`) to cross-compile the tracker.

```sh
./install.sh            # default ssh host "remarkable"; restarts xovi
./install.sh myhost
NO_RESTART=1 ./install.sh
```

What it installs:
- the `.qmd` files go into `~/xovi/exthome/qt-resource-rebuilder/`
- the tracker goes into `~/qmarkable/`
- start/stop hooks go into `~/xovi/scripts/post-start` and `post-stock`
- the config goes to `~/.config/qmarkable/tracker.conf` (existing config is kept)

The tracker runs as a *transient* systemd unit (`qmarkable-tracker`) started
by `~/xovi/start`. Nothing is written to `/etc`. Like xovi itself, it doesn't
survive a reboot until xovi is started again, and `~/xovi/stock` stops it.

**Recovery:** if the UI ever misbehaves, run `~/xovi/stock` over SSH or reboot.
Both bring back the unmodified UI.

```sh
./uninstall.sh          # keeps recorded data
PURGE=1 ./uninstall.sh  # also deletes data + config
```

Useful on the tablet:
```sh
systemctl status qmarkable-tracker
journalctl -u qmarkable-tracker
cat ~/.local/share/qmarkable/index.txt
```

## Adding a homepage widget

Create a new `.qmd` (start it with `VERSION 3.28.0.172`) that fills the slot:

```
SLOT qmHomeWidgets
    INSERT {
        Item {
            width: parent.width
            height: 300
            // `root` = MainView, `qmHome` = the page (use qmHome.visible to refresh)
        }
    }
END SLOT
```

Widgets stack vertically in a scrollable column. `install.sh` installs every `mods/*.qmd`.

## Developing

`.qmd` files here use plain QML names. qt-resource-rebuilder accepts them
alongside hashed community mods. To read stock QML or other people's mods:

```sh
WORK=~/qmarkable-work; mkdir -p $WORK
scp remarkable:/usr/bin/xochitl $WORK/
scp remarkable:/home/root/xovi/exthome/qt-resource-rebuilder/hashtab $WORK/
python3 -m venv $WORK/venv && $WORK/venv/bin/pip install zstandard pyside6-essentials
$WORK/venv/bin/python -I tools/rccextract.py $WORK/xochitl $WORK/qml    # stock QML (do not commit)
python3 tools/qhash.py unhash $WORK/hashtab some-community-mod.qmd     # readable diff
```

Preview the homepage offscreen with sample data (needs a built
[qmldiff](https://github.com/asivery/qmldiff)):

```sh
WORK=~/qmarkable-work QMLDIFF=/path/to/qmldiff tools/preview/preview.sh
```

Tracker test mode, which replays synthetic sessions without touching real data:

```sh
printf '1794268200 open nb-1 Calculus\n1794269100 close\n' | \
  QMARKABLE_DATADIR=/tmp/t ~/qmarkable/qmarkable-trackerd --replay
```
