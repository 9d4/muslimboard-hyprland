# MuslimBoard for Hyprland

A lightweight (C, GTK 3) desktop widget for [Hyprland](https://hyprland.org) that shows the clock, date, Hijri date,
prayer times with a countdown to the next one, and a rotating Quran verse or hadith, drawn
right on your wallpaper. It is a native port of the
[MuslimBoard browser extension](https://github.com/novalagung/muslimboard).

- Sits on the **bottom layer**: above your wallpaper (even when the wallpaper daemon redraws it), below every window, no bar or panel needed.
- **Configure on the go**: right-click for a menu, click the location line to change city. No file editing.
- City search and "detect from IP", with the calculation method suggested for your country.
- Curated verses from the original extension, in English or Bahasa Indonesia.
- Prayer times are cached, so it keeps working offline after the first fetch of the day.

## Build

A small C program: GTK 3, gtk-layer-shell, json-glib and libcurl, on a wlroots-style Wayland compositor (Hyprland, Sway, ...).
It needs glib 2.68 or newer.

| Distro | Packages |
|---|---|
| Arch | `sudo pacman -S base-devel gtk3 gtk-layer-shell json-glib curl` |
| Debian / Ubuntu | `sudo apt install build-essential pkg-config libgtk-3-dev libgtk-layer-shell-dev libjson-glib-dev libcurl4-openssl-dev` |
| Fedora | `sudo dnf install gcc make gtk3-devel gtk-layer-shell-devel json-glib-devel libcurl-devel` |

Optional but recommended fonts, which are what the original uses: **Lato** (text) and **Amiri** (Arabic).
Arch: `ttf-lato ttf-amiri`. Without them it falls back to Noto Sans / Noto Naskh Arabic.

You also need a wallpaper setter ([hyprpaper](https://github.com/hyprwm/hyprpaper), swww, ...). The widget is transparent
and only draws the text on top.

## Install

```sh
git clone https://github.com/<you>/muslimboard-hyprland
cd muslimboard-hyprland
make
make install PREFIX=~/.local    # installs ~/.local/bin/muslimboard; make sure it is on your PATH
```

The verses and stylesheet are embedded in the binary, so it is a single file.

Start it from `hyprland.conf`:

```ini
exec-once = muslimboard
```

On first launch a settings window opens so you can pick your city. That is all the setup there is.

## Using it

| Action | Result |
|---|---|
| Click the **location** line | Open settings |
| Click the **verse** | Show the next verse |
| **Right-click** the widget | Menu: Settings, Next verse, Refresh prayer times, Quit |

Only the widget's own area takes clicks. The rest of your desktop is untouched.

### Settings

- **Location**: search a city, or *Detect from IP*. The display name, coordinates and timezone can be edited by hand.
- **Calculation method**: any [Aladhan method](https://aladhan.com/calculation-methods). Picking a place pre-selects the usual one for its country
  (for example Kemenag for Indonesia, JAKIM for Malaysia, ISNA for the US and Canada); otherwise Muslim World League.
- **Verse language**, **12/24-hour clock**, **monitor**, and how often a **new verse** appears.

Changes apply immediately.

### Command line and key bindings

The widget is a single instance. Running the command again talks to the one that is already running:

```sh
muslimboard --settings     # open the settings window
muslimboard --next-verse   # show another verse now
muslimboard --refresh      # re-download prayer times
muslimboard --quit         # stop the widget
```

```ini
bind = SUPER, N, exec, muslimboard --next-verse
bind = SUPER SHIFT, N, exec, muslimboard --settings
```

The settings window is fixed-size, so Hyprland floats it automatically.

## Files

| Path | Purpose |
|---|---|
| `~/.config/muslimboard/config.json` | Your settings (written by the settings window) |
| `~/.config/muslimboard/style.css` | Optional GTK CSS overrides, loaded after the built-in style |
| `~/.cache/muslimboard/` | Cached prayer times |

Both locations follow `XDG_CONFIG_HOME` and `XDG_CACHE_HOME`.

`config.json` keys: `city`, `latitude`, `longitude`, `timezone` (IANA name, empty means system), `method`, `language` (`en` or `id`),
`clock_24h`, `monitor`, `verse_interval_minutes`.

### Restyling

Everything is plain GTK CSS. Put overrides in `style.css`, for example:

```css
.mb .clock    { font-size: 72px; }
.mb .arabic   { font-size: 40px; }
.mb .next     { background: rgba(80, 200, 120, 0.35); }
```

The class names (`clock`, `meta`, `small-title`, `prayer-name`, `prayer-time`, `prayer-tz`, `next`, `next-eta`, `arabic`,
`quote-en`, `quote-ref`) are defined in `data/style.css`.

### Adding verses

Edit `data/data-content-en.json` or `data/data-content-id.json` and add items with `matan` (Arabic), `translation` and `reference`, then rebuild.

## Network use

The widget talks to three services, and only for the reasons below:

- [Aladhan](https://aladhan.com/prayer-times-api) for prayer times, sending your coordinates, method and timezone.
- [Open-Meteo geocoding](https://open-meteo.com/en/docs/geocoding-api) when you search for a city.
- [ipwho.is](https://ipwho.is) only when you press *Detect from IP*.

No accounts, no keys, no analytics.

## Credits

The verse and hadith lists in `data/` come from [novalagung/muslimboard](https://github.com/novalagung/muslimboard)
(MIT, see `data/LICENSE-muslimboard`). The layout and look follow that project.
Prayer times by [Aladhan](https://aladhan.com).

## License

MIT, see `LICENSE`.
