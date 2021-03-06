/*
 * Copyright (C) 2003-2015 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "UsfDecoderPlugin.hxx"
#include "CheckAudioFormat.hxx"
#include "../DecoderAPI.hxx"
#include "tag/Handler.hxx"
#include "fs/Path.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringView.hxx"
#include "tag/Table.hxx"

#include <usf.h>
#include <psflib.h>

static constexpr Domain usf_domain("usf");
static constexpr unsigned USF_BUFFER_FRAMES = 2048;
static constexpr unsigned USF_CHANNELS = 2;
static constexpr unsigned USF_BUFFER_SAMPLES = USF_BUFFER_FRAMES*USF_CHANNELS;

static void *
stdio_fopen(const char *path)
{
    return fopen(path, "rb");
}

static size_t
stdio_fread(void *p, size_t size, size_t count, void *f)
{
    return fread(p, size, count, (FILE *) f);
}

static int
stdio_fseek(void *f, int64_t offset, int whence)
{
    return fseek((FILE *) f, offset, whence);
}

static int
stdio_fclose(void *f)
{
    return fclose((FILE *) f);
}

static long
stdio_ftell(void *f)
{
    return ftell((FILE *) f);
}

static constexpr psf_file_callbacks stdio_callbacks =
{
    "\\/:",
    stdio_fopen,
    stdio_fread,
    stdio_fseek,
    stdio_fclose,
    stdio_ftell
};

static constexpr struct tag_table usf_tags[] = {
    {"title", TAG_TITLE},
    {"artist", TAG_ARTIST},
    {"composer", TAG_COMPOSER},
    {"game", TAG_ALBUM},
    {"year", TAG_DATE},
    {"genre", TAG_GENRE},
    {"track", TAG_TRACK},
    {nullptr, TAG_NUM_OF_ITEM_TYPES}
};

struct UsfLength {
    unsigned long total = 0; // Track duration.
    unsigned long fade = 0;   // Fade out duration
};

struct UsfLoaderState
{
    bool enable_compare; // The _enablecompare tag is present in the file; passed to usf_set_compare
    bool enable_fifo_full; // The _enableFIFOfull tag is present in the file; passed to usf_set_fifo_full
    UsfLength length;
    void *emu; // The emulator state
};

struct UsfTags {
    UsfLength length;                  // Song length needs to be stored for further calculation
    TagHandler &tag_handler; // These two are needed for setting tags when this struct is used
    UsfTags(TagHandler &handler) : tag_handler(handler) {} 
};

/**
 * Parse a time (in ms) from a tag on the format mm:SS.sss
 */
static int
get_length_from_string(const char *string)
{
    size_t len = strlen(string);
    int total = 0;         // Total time in milliseconds
    int final_mult = 1000; // Multiplier for the final unit. If no delimiter is used, use seconds.
    int local_mult = 1;    // Multiplier in the currently parsed unit (millisecond, second, minute).
    int acc = 0;           // The accumulating value of the current unit
    for (int i = len - 1; i >= 0; i--) {
        char c = string[i];
        if (std::isdigit(c)) {
            acc += (c - '0') * local_mult;
            local_mult *= 10;
        } else {
            local_mult = 1;
            int mult = 0;
            if (c == '.') {
                mult = 1;
            } else if (c == ':') {
                mult = 1000;
                final_mult = 60000;
            } else {
                return -1; // Error in parsing duration. Return -1 (looping).
            }
            total += acc * mult;
            acc = 0;
        }
    }
    total += final_mult*acc;

    return total;
}

/**
 * Store track length by tag
 */
static void
set_length_from_tags(UsfLength &length, const char *name, const char *value) {
    if (strcmp("length", name) == 0) {
        length.total = get_length_from_string(value);
    } else if (strcmp("fade", name) == 0) {
        length.fade = get_length_from_string(value);
    }
}

static int
usf_loader(void *context, const uint8_t *exe, size_t exe_size,
               const uint8_t *reserved, size_t reserved_size)
{
    auto &state = *static_cast<UsfLoaderState *>(context);

    if (exe && exe_size > 0) return -1;

    return usf_upload_section(state.emu, reserved, reserved_size);
}

static int
usf_info(void *context, const char *name, const char *value)
{
    UsfLoaderState &state = *static_cast<UsfLoaderState *>(context);

    if (strcmp(name, "_enablecompare") == 0)
        state.enable_compare = 1;
    else if (strcmp(name, "_enableFIFOfull") == 0)
        state.enable_fifo_full = 1;
    else {
        set_length_from_tags(state.length, name, value);
    }
    return 0;
}

/**
 * Callback for getting and setting tags
 */
static int
usf_tags_target(void *context, const char *name, const char *value)
{
    UsfTags &tags = *static_cast<UsfTags *>(context);

    TagType type = tag_table_lookup(usf_tags, name);

    if (type != TAG_NUM_OF_ITEM_TYPES) {
        tags.tag_handler.OnTag(type, StringView(value));
    } else {
        set_length_from_tags(tags.length, name, value);
    }
    return 0;
}

static void
usf_file_decode(DecoderClient &client, Path path_fs)
{
    /* Load the file */

    UsfLoaderState state;
    state.emu = malloc(usf_get_state_size());
    AtScopeExit(&state) {
        free(state.emu);
    };
    usf_clear(state.emu);

    // 0x21 represents the miniusf file format
    const int psf_version = psf_load(
            path_fs.c_str(), 
            &stdio_callbacks, 
            0x21, 
            usf_loader, 
            &state, 
            usf_info, 
            &state, 
            0, 
            nullptr, 
            nullptr);

    // If psf_version < 0  an error occured while loading the file.
    if (psf_version < 0) {
        LogWarning(usf_domain, "Error loading usf file");
        return;
    }

    usf_set_compare(state.emu, state.enable_compare);
    usf_set_fifo_full(state.emu, state.enable_fifo_full);

    int32_t sample_rate;
    usf_render(state.emu, 0, 0, &sample_rate);

    /* initialize the MPD decoder */

    const auto audio_format = CheckAudioFormat(sample_rate, SampleFormat::S16, USF_CHANNELS);
    assert(audio_format.IsValid());

    // Duration
    client.Ready(audio_format, true, SongTime(state.length.total));

    /* .. and play */
    DecoderCommand cmd;

    bool loop = state.length.total == 0; // When song has no length, enable looping
    unsigned long decoded_frames = 0;
    unsigned long total_frames = 0;
    unsigned long fade_frames = 0;
    if (!loop) {
        total_frames = (state.length.total*sample_rate)/1000;
        fade_frames = (state.length.fade*sample_rate)/1000;
    }
    unsigned long fade_start_time = total_frames - fade_frames;
    bool fade = state.length.fade > 0;

    do {
        int16_t buf[USF_BUFFER_SAMPLES];
        const char* result = usf_render(state.emu, buf, USF_BUFFER_FRAMES, nullptr);
        if (result != 0) {
            FormatWarning(usf_domain, "usf_render failed: %s", result);
            break;
        }
        decoded_frames += USF_BUFFER_FRAMES; 

        // Linear fade out
        if (!loop && fade && decoded_frames > fade_start_time) {
            const double vol = 1.0 - ((decoded_frames + fade_frames - total_frames) / double(fade_frames));
            const double normalized_vol = vol < 0 ? 0 : vol;
            for (unsigned int i = 0; i < USF_BUFFER_SAMPLES; i++) {
                buf[i] *= normalized_vol;
            }
        }

        cmd = client.SubmitData(nullptr, buf, sizeof(buf), 0);

        // Stop the song when the total samples have been decoded
        // or loop
        if (!loop && decoded_frames > total_frames)
            break;

        if (cmd == DecoderCommand::SEEK) {
            // If user seeks during the fade period. Disable fading and play forever.
            // Hacky way to give user posibility to enable looping on the fly
            if (decoded_frames > fade_start_time) {
                loop = true;
            }

            // Seek manually by restarting emulator and discarding samples.
            const int target_time = client.GetSeekTime().ToS();
            const int frames_to_throw = target_time*sample_rate;
            usf_restart(state.emu);
            usf_render(state.emu, nullptr, frames_to_throw, nullptr);

            client.CommandFinished();
            client.SubmitTimestamp(FloatDuration(target_time));
            decoded_frames = frames_to_throw;
        }

    } while (cmd != DecoderCommand::STOP);
    usf_shutdown(state.emu);
}

static bool
usf_scan_file(Path path_fs, TagHandler &handler) noexcept
{
    UsfTags tags(handler);
    const int psf_version = psf_load(
            path_fs.c_str(), 
            &stdio_callbacks, 
            0, 
            nullptr, 
            nullptr, 
            usf_tags_target, 
            &tags, 
            false, 
            nullptr, 
            nullptr);
    if (psf_version < 0) {
        return false;
    }

    // Duration
    handler.OnDuration(SongTime::FromMS(tags.length.total));
    return true;
}

static const char *const usf_suffixes[] = {
	"usf",
	"miniusf",
	nullptr
};

extern const struct DecoderPlugin usf_decoder_plugin;
constexpr DecoderPlugin usf_decoder_plugin = 
	DecoderPlugin("usf", usf_file_decode, usf_scan_file)
    .WithSuffixes(usf_suffixes);
