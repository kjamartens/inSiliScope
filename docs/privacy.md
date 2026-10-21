# Privacy

The viewer and these docs count anonymous usage with [Umami](https://umami.is) (Umami Cloud), so we can see which
parts of inSiliScope people use. No cookies are set, nothing that identifies you is stored, and nothing is shared
with advertisers. The Micro-Manager adapter, the command-line tool and the webSMLM block send nothing at all.

## What is counted

- **Page views:** the page, the referring site (for example a paper or a course page), the approximate location
  (country, region, city, looked up from the IP address), the browser, operating system, device type, screen size and
  language. Umami tells visits apart by a hash of the IP address, the browser and a secret that changes every month,
  so a returning visitor is recognised within a month at most, and never across other sites. The IP address itself is
  not stored. The search and `#` parts of page addresses are left out.
- **Viewer events**, all from fixed lists, never text you type or file names:

| Event | What it carries |
|---|---|
| `pick` | which control (specimen, modality, Look, settings level, a Configuration group such as Mode, Objective or Camera) and the option picked |
| `live`, `snap` | that live view was started, that a snap was taken |
| `record` | that a recording started; its length in a range (up to 10, 100, 1000 ... frames); whether it continued the live sample |
| `save-tiff` | that a TIFF was saved, from a recording or a movie; its length in a range |
| `make-movie` | that a movie was made; fluorescence or brightfield |
| `anim-export` | that an animation was exported; its format (MP4, WebM, GIF) |
| `link` | that the Docs, Micro-Manager or releases link was followed |
| `summary` | once, when the page closes: the specimen, mode, Look and level in use, whether live, record, save or movie were used, and the time on the page in a range (up to 1, 5, 15, 30, 60 minutes) |

- **Docs:** page views, and a click on a download button (which file).

A choice counts only when you make it: settings the viewer restores from your last visit, and the settings a preset
changes, are not counted.

## Why there is no cookie banner

The EU rules behind cookie banners cover storing or reading information on your device. Umami stores nothing on it,
and what it keeps cannot be traced back to a person, which is why no consent is asked. The viewer does keep your settings and a cache of packed
cells in your browser's local storage; that is for you (the next visit starts where you left off), stays on your
device and is never sent anywhere. **Reset all to defaults** in the viewer's ☰ menu forgets the settings.

## Turning it off

Nothing is sent when your browser has **Do Not Track** or **Global Privacy Control** on, or when an ad or tracker
blocker blocks Umami. To switch it off on this site only, run `localStorage.setItem('umami.disabled', 1)` in the
browser's developer console on the viewer and on a docs page.

## Downloads

Downloads of the release files (the Micro-Manager adapter, its configurations) are counted by GitHub, per file, as
for every GitHub release; the docs' download buttons count a click as above.

Questions: open an issue on [GitHub](https://github.com/kjamartens/inSiliScope/issues).
