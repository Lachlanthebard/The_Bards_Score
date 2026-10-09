# Bard's Score — experimental source prototype

A Flipper Zero external app for entering monophonic treble-clef melodies, saving a native score, and exporting MusicXML.

## Controls
- Score: OK -> choose pitch -> OK -> choose duration -> OK inserts note after current note.
- Score: Left/Right selects a note; Up/Down adjusts selected pitch by semitone.
- Score: Hold OK opens Save, Load, Export and Clear menu.
- Back returns to previous screen or exits from score view.

## Building with Momentum
This is **source code**, not a compiled FAP. Add this folder to `applications_user/bards_score` in a Momentum firmware checkout, then build with `./fbt fap_bards_score` (or the corresponding uFBT command). Build against the same Momentum revision as the device. Compilation and on-device behaviour have **not** been verified.

## Files
- `apps_data/bards_score/melody.bsc` (editable native score)
- `apps_data/bards_score/melody.musicxml` (MusicXML export)

## Current prototype limitations
- Single melody and fixed C major, 4/4; no rests, dotted notes, playback, multiple compositions or editable titles yet.
- Pitch menu steps chromatically; accidental display and engraving are simplified.
- Staff glyph is a placeholder 'G', not a rendered treble clef.
- MusicXML export assumes 4/4 and does not split notes crossing barlines; keep each bar's durations adding to four quarter-note beats for reliable import.
- File actions have no success/error popup yet. Load overwrites the current in-memory melody.
