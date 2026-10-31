# Audio

How the engine plays sound on each target, and the command-line renderer.

## Audio output

`include/capsule/hal/audio.h` is what the audio core (`capsule/audio/core.h`) calls to play: on the GBA the
DirectSound FIFOs fed by DMA1 / DMA2 from the core's ring, timer 0 at the mix rate, the VCOUNT interrupt at line 150
(the DMA counter) and the core's step on every VBlank, lag frames included; on desktop an integer model of the sound
hardware (`src/hal/common/apu_model.h`: the four tone channels and the final mix, register writes applied at the
frame's start) sent to an SDL audio device (`CAPSULE_AUDIO=0`: none); headless the same model, every frame kept for
tests; Dreamcast silent (the core still steps, so timing is the same). Nothing of it is linked until a program calls
`audio::init()`.

`capsule_audio_render` (desktop and headless builds) renders a sound number (`--sound N`, `--song KEY`), a clip
(`--clip N` with `--clip-*` parameters) or the test apps' audio tour (`--tour`) of a build's audio table and writes
`segments.bin` (the ring segment mixed each frame), `tones.txt` (the ordered tone register writes), `state.jsonl`
(the core's state per frame), `out.wav` and `summary.json` (hashes of the four). `capsule_audio_render_hq` is the
same with the high-quality desktop mix (4x the rate; the ring and state are unchanged). `capsule_audio_render()` in
`cmake/parts/audio_hal.cmake` builds one for another manifest.

## See also

- `capsule_test_audio` and `capsule_sandbox_audio`: [Test apps](APPS.md#test-apps).
- The toolkit's audio editor (songs, instruments, samples, MIDI import, sound
  effects) edits a game's audio manifest, the `"audio"` key of its
  `reference/project.json`: see the toolkit's
  [user guide](https://github.com/TheWildDeadHero/CapsuleCreatures_Toolkit/blob/main/docs/USER_GUIDE.md).
