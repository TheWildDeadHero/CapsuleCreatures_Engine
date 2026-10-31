/// @file
/// audio_render (D-34 M4 B1b): renders a sound, a clip or the audio tour of a build's audio table headless, frame by
/// frame, through the real audio core and the headless backend, and writes what the format's test plan compares
/// (format §11): the ring segment of every frame (L2), the ordered tone writes (L3), the core's state (L1) and the
/// output model's samples as a WAV (L4).
///
///   capsule_audio_render (--sound N | --song KEY | --clip N | --tour | --list) [--out DIR]
///                        [--frames F | --tail T --max-frames M] [--mono] [--clip-volume V --clip-pan P
///                        --clip-pitch P --clip-hold T --clip-release R --clip-chorus C --clip-priority P
///                        --clip-offset S] [--then N\@F] [--no-wav] [--no-state] [--quiet]
///
/// Frame k (1-based) is the k-th vertical blank after the core's init(). As on the GBA (the backend's init() returns
/// at line 159), the first blank has no DMA-counter step before it, and the program's first calls come after it:
/// init, one empty blank, then the calls of frame 1 before blank 2 (apps/test_audio does the same, so the GBA run and
/// this one line up blank for blank).
///
/// Files in DIR (default "."):
///   segments.bin  per frame: the right side's samples of the segment mixed that frame, then the left side's
///   tones.txt     "frame offset value" (hex) per tone write, in order; wave RAM bytes as offsets 0x30-0x3F and
///                 SOUNDCNT_H as 0x22 / 0x23 (the GBA debug log's form)
///   state.jsonl   one JSON object per frame: the core's Snapshot (capsule/audio/state.h field names)
///   out.wav       the output model's signed 16-bit stereo (13,379 Hz at the reference rate; the HQ build,
///                 capsule_audio_render_hq: 4x the rate from the core's high-quality mix, never for parity)
///   summary.json  frames, the sound's end frames, FNV-1a 64 hashes of the four streams
///
/// End frames (C14a, S7), as render frames: "end_frame" is the frame whose ticks ran the sound's last END (the
/// frame midi_song.frames_to_end() counts, plus the empty first blank); "status_end_frame" the first frame whose
/// state shows the player ended (the tick after the last END, as the reference driver's status bits: what a game's
/// wait sees); 0 when it did not end. --then N\@F (with --sound / --song) plays sound N with frame F's calls (F >= 2:
/// after blank F - 1, before blank F) as a second call, so two sounds' priorities and channel stealing run in the
/// C++ core; "then_end_frame" / "then_status_end_frame" are its end frames; the run lasts until both ended (+ tail).
/// --list prints the table's sounds (number, player, key) and clips. Exit status 0, 1 on a failure, 2 on usage.
#include "capsule/asset/asset.h"
#include "capsule/audio/audio.h"
#include "capsule/audio/core.h"
#include "capsule/audio/rom.h"
#include "capsule/audio/state.h"
#include "capsule/core/endian.h"

#include "hal/headless/audio_headless.h"

#include "audio_tour.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace capsule::hal::system
{
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::fprintf(stderr, "ENGINE_ASSERT failed: %s\n  at %s:%d\n", expr, file, line);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

using namespace capsule;
namespace hal_audio = capsule::hal::audio;
namespace apu = capsule::hal::apu;

constexpr int kExitOk = 0;           ///< Success.
constexpr int kExitFailed = 1;       ///< A file could not be written.
constexpr int kExitUsage = 2;        ///< Bad command line.
constexpr uint32_t kDefaultTail = 30;           ///< Frames rendered after the sound's end.
constexpr uint32_t kDefaultMaxFrames = 3600;    ///< At most a minute.
constexpr uint32_t kTourMaxFrames = 100000;     ///< The tour ends by itself; this only stops a runaway.
constexpr int kPathBytes = 1024;                ///< Longest output path.
constexpr int kNumberBase = 0;                  ///< strtol: decimal, 0x hex or 0 octal.
constexpr char kThenSeparator = '@';            ///< --then N\@F.
constexpr uint32_t kFirstCallFrame = 2;         ///< The calls before blank 2 are the first (blank 1 is empty).

/// @name Clip defaults (the reference game's cry defaults: key 60, hold 140 ticks)
///@{
constexpr uint8_t kClipVolume = 127;        ///< Volume.
constexpr int16_t kClipPitch = 60 * 256;    ///< Key 60.
constexpr uint16_t kClipHold = 140;         ///< Ticks before the release.
constexpr uint8_t kClipPriority = 10;       ///< Priority.
///@}

/// @name WAV header
///@{
constexpr uint32_t kWavHeaderBytes = 44;     ///< RIFF + fmt + data headers.
constexpr uint32_t kRiffSizeOffset = 4;      ///< RIFF chunk size.
constexpr uint32_t kDataSizeOffset = 40;     ///< data chunk size.
constexpr uint32_t kRiffOverhead = 36;       ///< RIFF size = data bytes + this.
constexpr uint32_t kFmtBytes = 16;           ///< PCM fmt chunk size.
constexpr uint16_t kFormatPcm = 1;           ///< WAVE_FORMAT_PCM.
constexpr uint16_t kChannels = 2;            ///< Stereo.
constexpr uint16_t kBitsPerSample = 16;      ///< Signed 16-bit.
constexpr uint16_t kBytesPerFrame = 4;       ///< 2 x 16 bits.
constexpr uint32_t kOffsetFmtTag = 8;        ///< "WAVE".
constexpr uint32_t kOffsetFmt = 12;          ///< "fmt ".
constexpr uint32_t kOffsetFmtSize = 16;      ///< fmt size.
constexpr uint32_t kOffsetFormat = 20;       ///< format tag.
constexpr uint32_t kOffsetChannels = 22;     ///< channels.
constexpr uint32_t kOffsetRate = 24;         ///< sample rate.
constexpr uint32_t kOffsetByteRate = 28;     ///< byte rate.
constexpr uint32_t kOffsetAlign = 32;        ///< block align.
constexpr uint32_t kOffsetBits = 34;         ///< bits per sample.
constexpr uint32_t kOffsetData = 36;         ///< "data".
constexpr uint32_t kTagBytes = 4;            ///< A chunk tag.
///@}

/// @name Logs
///@{
constexpr uint8_t kLogWaveOffset = 0x30;     ///< Wave RAM byte i is logged as 0x30 + i.
constexpr uint8_t kLogSoundcntH = 0x22;      ///< SOUNDCNT_H's low byte (high: + 1).
constexpr int kByteBits = 8;                 ///< Bits in a byte.
constexpr uint32_t kWordBytes = 4;           ///< Bytes in a wave word.
constexpr uint64_t kFnv64Basis = 0xCBF29CE484222325ull;   ///< FNV-1a 64 offset basis.
constexpr uint64_t kFnv64Prime = 0x100000001B3ull;        ///< FNV-1a 64 prime.
constexpr int kLineBytes = 64;               ///< Room for one tones.txt line.
constexpr size_t kStateLineBytes = 65536;    ///< Room for one state line (a full snapshot is about 20 KB).
///@}

/// An open output file; closed when it goes out of scope.
struct File
{
   std::FILE* f = nullptr;   ///< The stream (nullptr: not open / not wanted).

   File() = default;
   File(const File&) = delete;
   File& operator=(const File&) = delete;
   /// Closes the stream.
   ~File()
   {
      if (f != nullptr)
      {
         std::fclose(f);
      }
   }
};

/// A running FNV-1a 64 hash of one stream.
struct Hash
{
   uint64_t h = kFnv64Basis;   ///< The hash so far.

   /// Adds bytes.
   /// @param p  first byte
   /// @param n  count
   void add(const void* p, size_t n)
   {
      const auto* b = static_cast<const uint8_t*>(p);
      for (size_t i = 0; i < n; ++i)
      {
         h = (h ^ b[i]) * kFnv64Prime;
      }
   }
};

/// What the command line asks for.
enum class Mode : uint8_t
{
   None,    ///< Nothing chosen yet.
   Sound,   ///< A sound number (--sound / --song).
   Clip,    ///< A clip voice.
   Tour,    ///< The audio tour (apps/common/audio_tour.h).
   List,    ///< Print the table.
};

/// The run's options and outputs.
struct Run
{
   Mode mode = Mode::None;                  ///< What to render.
   uint32_t sound = 0;                      ///< --sound / --song: the sound number.
   const char* song = nullptr;              ///< --song: the key's canonical name (resolved after init()).
   audio::ClipParams clip{};                ///< --clip and its parameters.
   uint32_t frames = 0;                     ///< --frames (0: until the end + tail).
   uint32_t tail = kDefaultTail;            ///< --tail.
   uint32_t max_frames = kDefaultMaxFrames; ///< --max-frames.
   bool mono = false;                       ///< --mono.
   bool wav = true;                         ///< Write out.wav.
   bool state = true;                       ///< Write state.jsonl.
   bool quiet = false;                      ///< No summary on stdout.
   bool then = false;                       ///< C14a: --then given.
   uint32_t then_sound = 0;                 ///< --then: the second sound.
   uint32_t then_frame = 0;                 ///< --then: the frame whose calls play it (>= 2).
   const char* out = ".";                   ///< --out.
   File segments;                           ///< segments.bin.
   File tones;                              ///< tones.txt.
   File states;                             ///< state.jsonl.
   File wave;                               ///< out.wav.
   Hash l1;                                 ///< state.jsonl's bytes.
   Hash l2;                                 ///< segments.bin's bytes.
   Hash l3;                                 ///< tones.txt's bytes.
   Hash l4;                                 ///< The WAV's samples.
   uint32_t wav_bytes = 0;                  ///< Sample bytes written.
   uint32_t rate = 0;                       ///< The WAV's rate.
   uint32_t output_samples = 0;             ///< Output samples per side of the latest frame.
   uint32_t frame = 0;                      ///< Frames (vertical blanks) so far.
   bool failed = false;                     ///< A write failed.
};

Run g_run;   ///< The run (the frame sink writes into it).

/// Opens DIR/name for writing (binary).
/// @param file  the file object
/// @param name  file name
/// @return false when it does not open
bool open_out(File& file, const char* name)
{
   char path[kPathBytes];
   const int n = std::snprintf(path, sizeof(path), "%s/%s", g_run.out, name);
   if (n < 0 || n >= kPathBytes)
   {
      return false;
   }
   file.f = std::fopen(path, "wb");
   return file.f != nullptr;
}

/// Writes bytes, hashing them; notes a failure.
/// @param file  the file (not open: only hashed)
/// @param hash  its hash
/// @param p     the bytes
/// @param n     their count
void put(File& file, Hash& hash, const void* p, size_t n)
{
   hash.add(p, n);
   if (file.f != nullptr && std::fwrite(p, 1, n, file.f) != n)
   {
      g_run.failed = true;
   }
}

/// One tones.txt line.
/// @param offset  register offset (the log's form)
/// @param value   the byte
void put_tone(uint32_t offset, uint32_t value)
{
   char line[kLineBytes];
   const int n = std::snprintf(line, sizeof(line), "%u %02x %02x\n", g_run.frame, offset, value);
   put(g_run.tones, g_run.l3, line, static_cast<size_t>(n));
}

/// A WAV header for the data written so far.
/// @param header  kWavHeaderBytes bytes
void wav_header(uint8_t* header)
{
   std::memcpy(header, "RIFF", kTagBytes);
   store_le32(header + kRiffSizeOffset, g_run.wav_bytes + kRiffOverhead);
   std::memcpy(header + kOffsetFmtTag, "WAVE", kTagBytes);
   std::memcpy(header + kOffsetFmt, "fmt ", kTagBytes);
   store_le32(header + kOffsetFmtSize, kFmtBytes);
   store_le16(header + kOffsetFormat, kFormatPcm);
   store_le16(header + kOffsetChannels, kChannels);
   store_le32(header + kOffsetRate, g_run.rate);
   store_le32(header + kOffsetByteRate, g_run.rate * kBytesPerFrame);
   store_le16(header + kOffsetAlign, kBytesPerFrame);
   store_le16(header + kOffsetBits, kBitsPerSample);
   std::memcpy(header + kOffsetData, "data", kTagBytes);
   store_le32(header + kDataSizeOffset, g_run.wav_bytes);
}

/// The headless backend's frame sink: L2, L3 and L4 of the frame just finished.
/// @param f  the frame
void on_frame(const apu::FrameOut& f)
{
   ++g_run.frame;
   put(g_run.segments, g_run.l2, f.right, f.segment_samples);
   put(g_run.segments, g_run.l2, f.left, f.segment_samples);
   for (uint16_t i = 0; i < f.write_count; ++i)
   {
      const apu::Write& w = f.writes[i];
      if (w.kind == apu::WriteKind::Tone)
      {
         put_tone(w.reg, w.value);
      }
      else if (w.kind == apu::WriteKind::Wave)
      {
         for (uint32_t b = 0; b < kWordBytes; ++b)
         {
            put_tone(kLogWaveOffset + w.reg * kWordBytes + b, (w.value >> (b * kByteBits)) & UINT8_MAX);
         }
      }
      else
      {
         put_tone(kLogSoundcntH, w.value & UINT8_MAX);
         put_tone(kLogSoundcntH + 1u, (w.value >> kByteBits) & UINT8_MAX);
      }
   }
   // little-endian samples, as a WAV stores them
   uint8_t bytes[apu::kOutputSides * apu::kMaxFrameOutput * sizeof(int16_t)];
   const uint32_t values = uint32_t{f.output_samples} * apu::kOutputSides;
   for (uint32_t i = 0; i < values; ++i)
   {
      store_le16(bytes + i * sizeof(int16_t), static_cast<uint16_t>(f.output[i]));
   }
   put(g_run.wave, g_run.l4, bytes, values * sizeof(int16_t));
   g_run.output_samples = f.output_samples;
   g_run.wav_bytes += values * static_cast<uint32_t>(sizeof(int16_t));
}

/// Appends `"name":value` to a JSON line.
/// @param line   the buffer
/// @param at     its length so far (updated)
/// @param size   its capacity
/// @param name   the field
/// @param value  the value
/// @param comma  a comma first
void json_field(char* line, size_t& at, size_t size, const char* name, long long value, bool comma = true)
{
   if (at < size)
   {
      const int n = std::snprintf(line + at, size - at, "%s\"%s\":%lld", comma ? "," : "", name, value);
      at += n > 0 ? static_cast<size_t>(n) : 0;
   }
}

/// Appends raw text.
/// @param line  the buffer
/// @param at    its length so far (updated)
/// @param size  its capacity
/// @param text  the text
void json_text(char* line, size_t& at, size_t size, const char* text)
{
   if (at < size)
   {
      const int n = std::snprintf(line + at, size - at, "%s", text);
      at += n > 0 ? static_cast<size_t>(n) : 0;
   }
}

// Snapshot fields (capsule/audio/state.h), listed once for the JSON writer.

/// PlayerState's fields: F(name) for each.
/// @param F  the macro applied to each field name
#define CAPSULE_PLAYER_FIELDS(F)                                                                                      \
   F(status) F(clock) F(sound) F(tempo_d) F(tempo_u) F(tempo_i) F(tempo_c) F(fade_oi) F(fade_oc) F(fade_ov)          \
   F(priority) F(track_count) F(first_track) F(start_check)
/// TrackState's scalar fields (stack is written apart).
/// @param F  the macro applied to each field name
#define CAPSULE_TRACK_FIELDS(F)                                                                                       \
   F(events) F(start_offset) F(offset) F(hold) F(flags) F(wait) F(call_depth) F(repeat) F(gate) F(key) F(velocity)    \
   F(running) F(program) F(voice_kind) F(voice_key) F(key_m) F(pit_m) F(key_shift) F(key_shift_x) F(tune) F(pit_x)   \
   F(bend) F(bend_range) F(vol_mr) F(vol_ml) F(vol) F(vol_x) F(pan) F(pan_x) F(mod_m) F(mod) F(mod_type)             \
   F(lfo_speed) F(lfo_phase) F(lfo_delay) F(lfo_delay_count) F(priority) F(echo_volume) F(echo_length) F(channel)
/// ChannelState's fields.
/// @param F  the macro applied to each field name
#define CAPSULE_CHANNEL_FIELDS(F)                                                                                     \
   F(frequency) F(position) F(count) F(fraction) F(status) F(kind) F(track) F(key) F(midi_key) F(velocity)            \
   F(priority) F(gate) F(rhythm_pan) F(right) F(left) F(env) F(env_right) F(env_left) F(attack) F(decay) F(sustain)   \
   F(release) F(echo_volume) F(echo_length) F(sustain_goal) F(pan) F(n4) F(length) F(sweep) F(next)
/// Snapshot's scalar fields.
/// @param F  the macro applied to each field name
#define CAPSULE_SNAPSHOT_FIELDS(F)                                                                                    \
   F(player_count) F(track_count) F(sampled_channels) F(reverb) F(dma_counter) F(segment) F(master_volume) F(mono)   \
   F(envelope_clock)

// One JSON field per name (put_state()'s locals: line, at, size and the struct s / p / t / c).

/// A Snapshot scalar.
/// @param name  the field
#define CAPSULE_JSON_SCALAR(name) json_field(line, at, size, #name, static_cast<long long>(s.name));
/// A PlayerState field.
/// @param name  the field
#define CAPSULE_JSON_PLAYER(name) json_field(line, at, size, #name, static_cast<long long>(p.name));
/// A TrackState field.
/// @param name  the field
#define CAPSULE_JSON_TRACK(name) json_field(line, at, size, #name, static_cast<long long>(t.name));
/// A ChannelState field.
/// @param name  the field
#define CAPSULE_JSON_CHANNEL(name) json_field(line, at, size, #name, static_cast<long long>(c.name));

/// The JSON line of the latest state: {"frame":k, scalars, "var":[...], "players":[{...}], "tracks":[...],
/// "channels":[...]} (only the players, tracks and channels in use; the tone channels always).
/// @param s  the snapshot
void put_state(const audio::Snapshot& s)
{
   static char line[kStateLineBytes];
   size_t at = 0;
   constexpr size_t size = sizeof(line);
   json_text(line, at, size, "{");
   json_field(line, at, size, "frame", g_run.frame, false);
   CAPSULE_SNAPSHOT_FIELDS(CAPSULE_JSON_SCALAR)
   json_text(line, at, size, ",\"var\":[");
   for (uint32_t i = 0; i < audio::kVarBytes; ++i)
   {
      char digits[kLineBytes];
      std::snprintf(digits, sizeof(digits), "%s%u", i == 0 ? "" : ",", s.var[i]);
      json_text(line, at, size, digits);
   }
   json_text(line, at, size, "],\"players\":[");
   const uint32_t players = uint32_t{s.player_count} + audio::kClipPlayers;
   for (uint32_t i = 0; i < players && i < audio::kMaxPlayers + audio::kClipPlayers; ++i)
   {
      const audio::PlayerState& p = s.players[i];
      json_text(line, at, size, i == 0 ? "{" : ",{");
      json_field(line, at, size, "index", i, false);
      CAPSULE_PLAYER_FIELDS(CAPSULE_JSON_PLAYER)
      json_text(line, at, size, "}");
   }
   json_text(line, at, size, "],\"tracks\":[");
   for (uint32_t i = 0; i < s.track_count && i < audio::kMaxTracks; ++i)
   {
      const audio::TrackState& t = s.tracks[i];
      json_text(line, at, size, i == 0 ? "{" : ",{");
      json_field(line, at, size, "rank", i + 1, false);
      CAPSULE_TRACK_FIELDS(CAPSULE_JSON_TRACK)
      json_text(line, at, size, ",\"stack\":[");
      for (uint32_t k = 0; k < sizeof(t.stack) / sizeof(t.stack[0]); ++k)
      {
         char digits[kLineBytes];
         std::snprintf(digits, sizeof(digits), "%s%u", k == 0 ? "" : ",", t.stack[k]);
         json_text(line, at, size, digits);
      }
      json_text(line, at, size, "]}");
   }
   json_text(line, at, size, "],\"channels\":[");
   bool first = true;
   for (uint32_t i = 0; i < audio::kMaxSampledChannels + audio::kToneChannels; ++i)
   {
      if (i >= s.sampled_channels && i < audio::kMaxSampledChannels)
      {
         continue;   // sampled slots past the table's count are never used
      }
      const audio::ChannelState& c = s.channels[i];
      json_text(line, at, size, first ? "{" : ",{");
      first = false;
      json_field(line, at, size, "index", i, false);
      CAPSULE_CHANNEL_FIELDS(CAPSULE_JSON_CHANNEL)
      json_text(line, at, size, "}");
   }
   json_text(line, at, size, "]}\n");
   put(g_run.states, g_run.l1, line, at < size ? at : size);
}

/// One vertical blank, then its state line.
void blank()
{
   hal_audio::capture::step();
   if (g_run.state)
   {
      static audio::Snapshot s;
      s = audio::Snapshot{};
      audio::snapshot(s);
      put_state(s);
   }
}

/// Parses an integer argument.
/// @param text   the text
/// @param value  the result
/// @return false when it is not a number
bool parse_number(const char* text, long& value)
{
   char* end = nullptr;
   value = std::strtol(text, &end, kNumberBase);
   return end != text && *end == '\0';
}

/// Reads --then's "N@F" into g_run (C14a, S7).
/// @param text  the argument
/// @return false when it is not two numbers N (a sound) and F >= 2 (a frame) joined by '@'
bool parse_then(const char* text)
{
   char* at = nullptr;
   const long sound = std::strtol(text, &at, kNumberBase);
   if (at == text || *at != kThenSeparator)
   {
      return false;
   }
   const char* frame_text = at + 1;
   char* end = nullptr;
   const long frame = std::strtol(frame_text, &end, kNumberBase);
   if (end == frame_text || *end != '\0' || sound < 0 || sound > UINT16_MAX || frame < kFirstCallFrame ||
       frame > INT32_MAX)
   {
      return false;
   }
   g_run.then_sound = static_cast<uint32_t>(sound);
   g_run.then_frame = static_cast<uint32_t>(frame);
   return true;
}

/// Prints the usage text.
void usage()
{
   std::fprintf(stderr,
                "usage: capsule_audio_render (--sound N | --song KEY | --clip N | --tour | --list) [--out DIR]\n"
                "       [--frames F | --tail T --max-frames M] [--mono] [--clip-volume V --clip-pan P --clip-pitch P\n"
                "       --clip-hold T --clip-release R --clip-chorus C --clip-priority P --clip-offset S]\n"
                "       [--then N@F] [--no-wav] [--no-state] [--quiet]\n");
}

/// Reads the command line into g_run.
/// @param argc  argument count
/// @param argv  arguments
/// @return false on a usage error
bool parse(int argc, char** argv)
{
   g_run.clip.volume = kClipVolume;
   g_run.clip.pitch = kClipPitch;
   g_run.clip.hold_ticks = kClipHold;
   g_run.clip.priority = kClipPriority;
   for (int i = 1; i < argc; ++i)
   {
      const char* a = argv[i];
      const bool has_value = i + 1 < argc;
      long v = 0;
      auto number = [&](long low, long high) -> bool
      {
         return has_value && parse_number(argv[++i], v) && v >= low && v <= high;
      };
      if (std::strcmp(a, "--sound") == 0 && number(0, UINT16_MAX))
      {
         g_run.mode = Mode::Sound;
         g_run.sound = static_cast<uint32_t>(v);
      }
      else if (std::strcmp(a, "--song") == 0 && has_value)
      {
         g_run.mode = Mode::Sound;
         g_run.song = argv[++i];
      }
      else if (std::strcmp(a, "--clip") == 0 && number(0, UINT16_MAX))
      {
         g_run.mode = Mode::Clip;
         g_run.clip.clip = static_cast<uint16_t>(v);
      }
      else if (std::strcmp(a, "--tour") == 0)
      {
         g_run.mode = Mode::Tour;
         g_run.max_frames = g_run.max_frames == kDefaultMaxFrames ? kTourMaxFrames : g_run.max_frames;
      }
      else if (std::strcmp(a, "--list") == 0)
      {
         g_run.mode = Mode::List;
      }
      else if (std::strcmp(a, "--out") == 0 && has_value)
      {
         g_run.out = argv[++i];
      }
      else if (std::strcmp(a, "--frames") == 0 && number(1, INT32_MAX))
      {
         g_run.frames = static_cast<uint32_t>(v);
      }
      else if (std::strcmp(a, "--tail") == 0 && number(0, INT32_MAX))
      {
         g_run.tail = static_cast<uint32_t>(v);
      }
      else if (std::strcmp(a, "--max-frames") == 0 && number(1, INT32_MAX))
      {
         g_run.max_frames = static_cast<uint32_t>(v);
      }
      else if (std::strcmp(a, "--mono") == 0)
      {
         g_run.mono = true;
      }
      else if (std::strcmp(a, "--clip-volume") == 0 && number(0, INT8_MAX))
      {
         g_run.clip.volume = static_cast<uint8_t>(v);
      }
      else if (std::strcmp(a, "--clip-pan") == 0 && number(INT8_MIN, INT8_MAX))
      {
         g_run.clip.pan = static_cast<int8_t>(v);
      }
      else if (std::strcmp(a, "--clip-pitch") == 0 && number(INT16_MIN, INT16_MAX))
      {
         g_run.clip.pitch = static_cast<int16_t>(v);
      }
      else if (std::strcmp(a, "--clip-hold") == 0 && number(0, UINT16_MAX))
      {
         g_run.clip.hold_ticks = static_cast<uint16_t>(v);
      }
      else if (std::strcmp(a, "--clip-release") == 0 && number(0, UINT8_MAX))
      {
         g_run.clip.release = static_cast<uint8_t>(v);
      }
      else if (std::strcmp(a, "--clip-chorus") == 0 && number(0, UINT8_MAX))
      {
         g_run.clip.chorus = static_cast<uint8_t>(v);
      }
      else if (std::strcmp(a, "--clip-priority") == 0 && number(0, UINT8_MAX))
      {
         g_run.clip.priority = static_cast<uint8_t>(v);
      }
      else if (std::strcmp(a, "--clip-offset") == 0 && number(0, INT32_MAX))
      {
         g_run.clip.start_offset = static_cast<uint32_t>(v);
      }
      else if (std::strcmp(a, "--then") == 0 && has_value && parse_then(argv[++i]))
      {
         g_run.then = true;
      }
      else if (std::strcmp(a, "--no-wav") == 0)
      {
         g_run.wav = false;
      }
      else if (std::strcmp(a, "--no-state") == 0)
      {
         g_run.state = false;
      }
      else if (std::strcmp(a, "--quiet") == 0)
      {
         g_run.quiet = true;
      }
      else
      {
         std::fprintf(stderr, "audio_render: bad argument %s\n", a);
         return false;
      }
   }
   return g_run.mode != Mode::None;
}

/// --list: the table.
void list()
{
   std::printf("{\"sounds\":[");
   for (uint16_t i = 0; i < audio::sound_count(); ++i)
   {
      const uint32_t key = audio::rom::kTable.keys != nullptr ? audio::rom::kTable.keys[i] : 0;
      std::printf("%s{\"sound\":%u,\"player\":%u,\"key\":%u}", i == 0 ? "" : ",", i, audio::player_of(i), key);
   }
   std::printf("],\"players\":%u,\"clips\":%u,\"samples_per_frame\":%u}\n", audio::player_count(),
               audio::rom::kTable.clip_count, audio::core::frame_samples());
}

/// When a sound ended, in render frames (0: not yet).
struct Ends
{
   uint32_t last_end = 0;     ///< The frame whose ticks ran its last END (no track of its player exists after it).
   uint32_t status_end = 0;   ///< The first frame whose status shows it ended (the tick after the last END).
};

/// The run's end frames (summary.json).
struct RunEnds
{
   Ends sound;   ///< The sound or clip.
   Ends then;    ///< --then's sound.
};

/// Whether any track of a player slot still exists (an END clears its track's exists bit at once; the player's
/// status bits follow at its next tick).
/// @param s     the state
/// @param slot  the player's slot (game players first, the clip players from audio::kMaxPlayers)
/// @return true while one exists
bool tracks_exist(const audio::Snapshot& s, uint8_t slot)
{
   const audio::PlayerState& p = s.players[slot];
   for (uint8_t i = 0; i < p.track_count; ++i)
   {
      if ((s.tracks[p.first_track + i].flags & audio::kTrackExists) != 0)
      {
         return true;
      }
   }
   return false;
}

/// Notes a sound's end frames after a blank.
/// @param e         its end frames
/// @param s         the state after the blank
/// @param slot      its player's slot (kNoPlayer: it has none, so it ends at once)
/// @param finished  its status shows it ended
void note_ends(Ends& e, const audio::Snapshot& s, uint8_t slot, bool finished)
{
   if (e.last_end == 0 && (slot == audio::kNoPlayer || !tracks_exist(s, slot)))
   {
      e.last_end = g_run.frame;
   }
   if (e.status_end == 0 && finished)
   {
      e.status_end = g_run.frame;
   }
}

/// Renders the chosen sound, clip or tour.
/// @return the end frames (all 0 for a tour)
RunEnds render()
{
   RunEnds ends;
   blank();   // the first blank comes before any call (as on the GBA after init())
   if (g_run.mode == Mode::Tour)
   {
      audio_tour::Tour tour{};
      audio_tour::start(tour);
      while (g_run.frame < g_run.max_frames && (g_run.frames == 0 || g_run.frame < g_run.frames))
      {
         const audio_tour::Blanks blanks = audio_tour::frame(tour);
         if (blanks == audio_tour::Blanks::None)
         {
            break;
         }
         if (blanks == audio_tour::Blanks::Lag)
         {
            // C14a: the modelled lag blanks, as rng::spend_blanks() steps them in a game (one state line each)
            for (uint16_t i = 0; i < audio_tour::kLagBlanks; ++i)
            {
               blank();
            }
         }
         blank();
      }
      return ends;
   }

   audio::ClipHandle handle = 0;
   if (g_run.mode == Mode::Sound)
   {
      audio::play(static_cast<audio::SoundId>(g_run.sound));
   }
   else
   {
      handle = audio::play_clip(g_run.clip);
   }
   const audio::PlayerId player = g_run.mode == Mode::Sound
                                     ? audio::player_of(static_cast<audio::SoundId>(g_run.sound))
                                     : static_cast<audio::PlayerId>(audio::kMaxPlayers + handle);
   const bool then = g_run.then && g_run.mode == Mode::Sound;
   audio::PlayerId then_player = audio::kNoPlayer;
   static audio::Snapshot state;   // static: the snapshot is large
   while (g_run.frame < g_run.max_frames)
   {
      if (then && g_run.frame + 1 == g_run.then_frame)
      {
         audio::play(static_cast<audio::SoundId>(g_run.then_sound));   // frame then_frame's calls
         then_player = audio::player_of(static_cast<audio::SoundId>(g_run.then_sound));
      }
      blank();
      if (g_run.frames != 0)
      {
         if (g_run.frame >= g_run.frames)
         {
            break;
         }
         continue;
      }
      state = audio::Snapshot{};
      audio::snapshot(state);
      const bool over = g_run.mode == Mode::Sound
                           ? (player == audio::kNoPlayer || audio::ended(player))
                           : !audio::clip_playing(handle);
      note_ends(ends.sound, state, player, over);
      const bool then_started = then && g_run.frame >= g_run.then_frame;
      if (then_started)
      {
         note_ends(ends.then, state, then_player, then_player == audio::kNoPlayer || audio::ended(then_player));
      }
      const uint32_t last = ends.then.status_end > ends.sound.status_end ? ends.then.status_end
                                                                         : ends.sound.status_end;
      const bool all = ends.sound.status_end != 0 && (!then || (then_started && ends.then.status_end != 0));
      if (all && g_run.frame >= last + g_run.tail)
      {
         break;
      }
   }
   return ends;
}

} // namespace

/// Renders what the command line asks for.
/// @param argc  argument count
/// @param argv  arguments
/// @return 0, 1 when a file could not be written, 2 on a usage error
int main(int argc, char** argv)
{
   if (!parse(argc, argv))
   {
      usage();
      return kExitUsage;
   }
   audio::init();
   if (g_run.song != nullptr)
   {
      const audio::SoundId id = audio::find_sound(asset::key_v1(g_run.song));
      if (id == audio::kNoSound)
      {
         std::fprintf(stderr, "audio_render: no sound %s in the table\n", g_run.song);
         return kExitUsage;
      }
      g_run.sound = id;
   }
   if (g_run.mode == Mode::List)
   {
      list();
      return kExitOk;
   }
   if (g_run.mono)
   {
      audio::set_output(audio::Output::Mono);
   }
   const bool opened = open_out(g_run.segments, "segments.bin") && open_out(g_run.tones, "tones.txt") &&
                       (!g_run.state || open_out(g_run.states, "state.jsonl")) &&
                       (!g_run.wav || open_out(g_run.wave, "out.wav"));
   if (!opened)
   {
      std::fprintf(stderr, "audio_render: cannot write in %s\n", g_run.out);
      return kExitFailed;
   }
#if defined(CAPSULE_AUDIO_HQ)
   g_run.rate = apu::output_rate(audio::core::frame_samples()) * hal_audio::kHqFactor;   // the HQ build's 4x mix
#else
   g_run.rate = apu::output_rate(audio::core::frame_samples());
#endif
   uint8_t header[kWavHeaderBytes] = {};
   if (g_run.wave.f != nullptr)
   {
      std::fwrite(header, 1, sizeof(header), g_run.wave.f);   // completed at the end
   }
   hal_audio::capture::set_sink(on_frame);

   const RunEnds ends = render();

   if (g_run.wave.f != nullptr)
   {
      wav_header(header);
      if (std::fseek(g_run.wave.f, 0, SEEK_SET) != 0 || std::fwrite(header, 1, sizeof(header), g_run.wave.f) != sizeof(header))
      {
         g_run.failed = true;
      }
   }
   File summary;
   if (open_out(summary, "summary.json"))
   {
      std::fprintf(summary.f,
                   "{\"frames\":%u,\"end_frame\":%u,\"status_end_frame\":%u,\"then_end_frame\":%u,"
                   "\"then_status_end_frame\":%u,\"rate\":%u,\"samples_per_frame\":%u,"
                   "\"output_samples_per_frame\":%u,"
                   "\"l1\":\"%016llx\",\"l2\":\"%016llx\",\"l3\":\"%016llx\",\"l4\":\"%016llx\"}\n",
                   g_run.frame, ends.sound.last_end, ends.sound.status_end, ends.then.last_end, ends.then.status_end,
                   g_run.rate, audio::core::frame_samples(), g_run.output_samples,
                   static_cast<unsigned long long>(g_run.l1.h), static_cast<unsigned long long>(g_run.l2.h),
                   static_cast<unsigned long long>(g_run.l3.h), static_cast<unsigned long long>(g_run.l4.h));
   }
   else
   {
      g_run.failed = true;
   }
   if (!g_run.quiet)
   {
      std::printf("{\"frames\":%u,\"end_frame\":%u,\"l1\":\"%016llx\",\"l2\":\"%016llx\",\"l3\":\"%016llx\","
                  "\"l4\":\"%016llx\"}\n",
                  g_run.frame, ends.sound.last_end, static_cast<unsigned long long>(g_run.l1.h),
                  static_cast<unsigned long long>(g_run.l2.h), static_cast<unsigned long long>(g_run.l3.h),
                  static_cast<unsigned long long>(g_run.l4.h));
   }
   return g_run.failed ? kExitFailed : kExitOk;
}
