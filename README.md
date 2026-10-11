# Bard's Score 🎵

**Music notation on the Flipper Zero** — created by **Lachlan The Bard**.

Bard's Score is an experimental but working external application (FAP) for writing treble-clef music on the Flipper Zero. It supports notes, rests, chords, two independent voices, key and time signatures, articulations, basic speaker playback, native score files and MusicXML export.

> **Version:** v1.6 (documentation and in-app About/Help added to v1.5). The v1.6 source is supplied for testing; a successful Momentum build and on-device test have not yet been confirmed.

## Features

- Treble-clef score display, note entry and selection; pitch range MIDI 48–95
- Whole, half, quarter, eighth and sixteenth notes and rests
- Beamed short notes, barlines, time signatures (2/4, 3/4, 4/4, 6/8)
- Major/minor key signatures (up to seven sharps or flats)
- Slurs, ties, staccato, accent and fermata marks
- Two independent voices; combined or solo score view
- Up to four simultaneous pitches per chord
- Playback of a voice or both voices; adjustable tempo (40–240 BPM)
- Save/load BSC4; backward loading for BSC1/BSC2/BSC3
- MusicXML export, with one-staff or two-staff layout selection
- In-app About / Help pages

## Quick controls

| Where | Input | Action |
| --- | --- | --- |
| Score | OK | Choose pitch, then duration, then insert note |
| Score | Left / Right | Select previous / next note |
| Score | Up / Down | Raise / lower selected pitch by one semitone |
| Score | Short Back | Delete selected note/rest |
| Score | Hold Back | Exit app |
| Score | Hold OK | Open Score tools |
| Score tools | Up / Down | Navigate; wraps around |
| Score tools | OK | Select tool |
| Score tools | Back | Return to score |
| Pitch/duration/settings | Up / Down | Change value |
| Pitch/duration/settings | OK | Confirm |
| Pitch/duration/settings | Back | Cancel/return |
| Playback | Back | Stop playback |
| About / Help | Up / Down or OK | Change help page |
| About / Help | Back | Return to Score tools |

## Writing music

1. On the score, press **OK** to choose a pitch. Adjust with Up/Down and confirm with OK.
2. Choose a duration with Up/Down and press OK to insert.
3. Use Left/Right to select a note, Up/Down to transpose by semitone, or short Back to delete it.
4. Hold OK for **Score tools**. Add rests, change time/key signatures, apply articulations and choose playback or file operations there.

### Chords

Select an existing pitched note, then **Score tools → Add chord tone**. Adjust the added pitch and press OK. Repeat for up to **four total pitches** per chord. **Remove chord tone** removes the most recently added chord tone. Chord tones share the selected note's duration and onset.

### Two voices

Use **Switch voice** to edit Voice 1 or Voice 2. **Combined/solo view** changes how voices are displayed on the Flipper. **Play both voices** attempts simultaneous playback by rapid alternation; the built-in speaker is not a true polyphonic synthesizer.

### MusicXML

Choose **XML staff layout** to toggle between one and two staves in exported MusicXML. Use **Export XML** to write the file. Import it into a notation program for further editing and fuller polyphonic playback. Complex notation may need cleanup after import.

## Files and backups

- `apps_data/bards_score/melody.bsc` — native editable score (BSC4 when saved by this version)
- `apps_data/bards_score/melody.musicxml` — exported MusicXML

**Important:** The app uses a single fixed save slot. Saving overwrites `melody.bsc`; exporting overwrites `melody.musicxml`. Back up your files before saving or loading experiments. Loading replaces the current in-memory score.

## Install / build

This repository includes source code for an external Flipper Zero application. The GitHub Actions workflow builds a Momentum-compatible `.fap` using the pinned firmware revision `d3f89dfe`. Download the successful workflow's `Bards-Score-Momentum` artifact, extract the FAP and copy it to your Flipper's SD-card apps directory using qFlipper or your preferred file manager. Use firmware compatible with the build's API.

To build manually, copy `bards_score.c`, `application.fam`, and `bards_score_icon.png` together into `applications_user/bards_score` in a compatible Momentum firmware checkout, then run:

```sh
./fbt fap_bards_score
```

## Limitations

- Flipper display is 128 × 64 pixels; dense polyphonic passages can be hard to read.
- Speaker playback is basic; two-voice playback approximates polyphony.
- Only one native score filename is currently supported; no composition titles or score browser.
- MusicXML engraving/import may require adjustment in desktop notation software.
- BSC4 is not guaranteed readable by older Bard's Score releases; keep backups.

## Help, bugs and contributions

See [QUICK_REFERENCE.md](QUICK_REFERENCE.md) for an abbreviated control guide and [CONTRIBUTING.md](CONTRIBUTING.md) for pull requests and bug reports. When reporting a problem, include firmware revision, Bard's Score version, steps to reproduce, and build logs if applicable.

## Author and license

Created by **Lachlan The Bard**. Contributions welcome via pull requests, subject to maintainer review. **No license has been selected yet:** do not assume the project is open-source licensed until a LICENSE file is added by the copyright holder.
