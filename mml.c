/*!
 * Polyphonic synthesizer for microcontrollers.  MML parser.
 * (C) 2021 Luciano Martorella
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */

#include "mml.h"
#include "synth.h"
#include "sequencer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*!
 * Not optimized for microcontroller usage.
 * Requires dynamic memory allocation support (heap), especially `malloc` and `realloc`.
 */

/*! Manage parser errors */
static void (*error_handler)(const char* err, int line, int column);
static int line = 1;
static int pos = 1;

#define ARTICULATION_STACCATO (2.5 / 4.0)
#define ARTICULATION_NORMAL (7.0 / 8.0)
#define ARTICULATION_LEGATO (1.0)

/*! Temporary list of sequencer stream frames, per channel */
static struct seq_frame_map_t frame_map;

static void init_stream_channel(int channel) {
	// Init new channels
	frame_map.channels[channel].count = 0;
	frame_map.channels[channel].frames = malloc(sizeof(struct seq_frame_t) * 16);
}

static int add_channel_frame(int channel, int frequency, int time_scale, int volume, double articulation, int edit_last_duration) {
	// New channel?
	if (channel >= frame_map.channel_count) {
		int old_count = frame_map.channel_count;
		frame_map.channel_count = channel + 1;
		frame_map.channels = realloc(frame_map.channels, sizeof(struct seq_frame_list_t) * frame_map.channel_count);
		for (int i = old_count; i < frame_map.channel_count; i++) {
			// Init new channels
			init_stream_channel(i);
		}
	}

	struct seq_frame_list_t* list = &frame_map.channels[channel];
	if (!edit_last_duration && list->count > 0 && (list->count % 16) == 0) {
		list->frames = realloc(list->frames, sizeof(struct seq_frame_t) * (list->count + 16));
	}

	if (edit_last_duration && list->count == 0) {
		error_handler("Can't join, no note before", line, pos);
		return 0;
	}

	int i = edit_last_duration ? (list->count - 1) : (list->count++);
	struct seq_frame_t* frame = &list->frames[i];

    if (!frequency) {
		if (!voice_wf_setup_def(frame, 0, 0)) {
			error_handler("Can't pack frame: pause", line, pos);
			return 0;
		}
    } else {
		if (!voice_wf_setup_def(frame, frequency, volume)) {
			error_handler("Can't pack frame: waveform", line, pos);
			return 0;
		}
    }

	// Calc duration and scale
	if (edit_last_duration) {
		time_scale += (frame->adsr_time_scale_1 + 1);
	}
	if (time_scale > UINT16_MAX) {
		error_handler("Can't pack frame: adsr time_scale", line, pos);
		return 0;
	}
	frame->adsr_time_scale_1 = time_scale - 1;
	frame->adsr_release_start = (uint8_t)round(ADSR_TIME_UNITS * articulation) - 1;
	return 1;
}

void mml_set_error_handler(void (*handler)(const char* err, int line, int column)) {
	error_handler = handler;
}

/*! Read a single digit from the stream and advance */
static int read_digit(const char** str, int* pos) {
	const char code = **str;
	*str += 1;
	*pos += 1;
	if (code < '0' || code > '9') {
		return 255;
	} else {
		return code - '0';
	}
}

/*! Read a number from the stream and advance */
static int read_number(const char** str, int* pos) {
	char* end;
	int ret = strtol(*str, &end, 10);
	if (!ret || end == *str) {
		return -1;
	}
	*pos += (end - *str);
	*str = end;
	return ret;
}

/*! Convert a node 0-84 to frequency. 0 is "C" at octave 0, so octave 2 (fourth-octave in scientific pitch) c2 = note 24, and a2 (Helmholtz 440Hz) = note 33 */
static int get_freq_from_code(int noteCode) {
	return (int)(440.0 * pow(2, ((noteCode - 33) / 12.0)));
}

/*! Convert a a-g code chromatic scale to frequency. Octave 2 is the fourth-octave in scientific pitch */
static int get_freq_from_note(char note, int sharp, int octave) {
	int semitone = ((note - 'a' + 5) % 7) * 2;
	if (semitone > 4) {
		semitone--;
	}
	if (sharp) {
		semitone++;
	}
	// semitone is 0 for c
	return get_freq_from_code(semitone + octave * 12);
}

/*! Parser state, per channel */
struct mml_channel_state_t {
	int octave;
	int default_length;
	int default_length_dot;
	int tempo;
	int volume;
	double articulation;
	// Active in current MML parsing line
	int isActive;
	// Running time in seconds and time units. Used to round note duration and not accumulate errors (skew between channels).
	struct {
		double seconds;
		int time_units;
	} running_time;
};
static struct mml_channel_state_t* mml_channel_states;
static int mml_channel_count;

/*! 
 * Get duration in ADSR time scale units. 
 * Tempo is in channel state. 
 * Length is fraction of whole note. Dots are number of dots (1 dot = 3/2, 2 dots = 9/4, etc..) 
 */
static int get_adsr_time_scale(struct mml_channel_state_t* state, int length, int dots) {
	double l = length;
	for (; dots > 0; dots--) {
		l /= 1.5;
	}
	double seconds = 60.0 * 4 / state->tempo / l;
	state->running_time.seconds += seconds;

	// Round note duration to not accumulate errors (skew between channels)
	double time_units_total = (int)round(state->running_time.seconds * synth_freq);
	double delta_units = time_units_total - state->running_time.time_units;
	int time_scale = (int)round(delta_units / ADSR_TIME_UNITS);

	state->running_time.time_units += time_scale * ADSR_TIME_UNITS;
	return time_scale;
}

static void enable_channel(int channel) {
	if (channel >= mml_channel_count) {
		mml_channel_count = channel + 1;
		mml_channel_states = realloc(mml_channel_states, sizeof(struct mml_channel_state_t) * mml_channel_count);
		// Init new channel
		mml_channel_states[channel].octave = 4;
		mml_channel_states[channel].default_length = 4;
		mml_channel_states[channel].default_length_dot = 0;
		mml_channel_states[channel].tempo = 120;
		mml_channel_states[channel].volume = 63;
		mml_channel_states[channel].articulation = ARTICULATION_NORMAL;
		mml_channel_states[channel].running_time.seconds = 0;
		mml_channel_states[channel].running_time.time_units = 0;
	}

	mml_channel_states[channel].isActive = 1;
}

// By default, if no channel identifier at the beginning of a MML line, it is referring to A channel only
static void reset_active_state() {
	for (int i = 1; i < mml_channel_count; i++) {
		mml_channel_states[i].isActive = 0;
	}
	enable_channel(0);
}

/*! 
 * Parse the MML file and produce sequencer stream of frames in `stream_channel` array.
 */
static int mml_parse(const char* content) {
	line = 1;
	pos = 0;

	// Starts with 1 voice
	mml_channel_states = malloc(0);
	frame_map.channels = malloc(0);
	frame_map.channel_count = 0;

	// Read the string until end
	reset_active_state();
	while(1) {
		pos++;
		char code = content[0];
		content++;
		if (!code) {
			break;
		}

		if (code <= 32 || code == '|') {
			// Skip blanks and partitures
			if (code == '\n') {
				line++;
				reset_active_state();
				pos = 0;
			}
			if (code == '\r') {
				pos--;
			}
			continue;
		}

		if (code == '#' || code == ';') {
			// Skip line comment
			while (*content != '\n') {
				content++;
			}
			content++;
			line++;
			reset_active_state();
			pos = 0;
			continue;
		}

		// Join last note?
		int join = 0;
		if (code == '&') {
			join = 1;
			continue;
		}

		if (code >= 'A' && code <= 'Z') {
			if (pos == 1) {
				// Decode active channels
				mml_channel_states[0].isActive = 0;
				enable_channel(code - 'A');
				while (*content >= 'A' && *content <= 'Z') {
					enable_channel(*content - 'A');
					content++;
					pos++;
				}
				continue;
			} else {
				error_handler("Misplaced channel selector", line, pos);
			}
		}

		int isPause;
		int isNoteCode;
		if (code == 'o') {
			int octave = read_digit(&content, &pos);
			if (octave == 255 || octave > 6) {
				error_handler("Invalid octave", line, pos);
				return 1;
			}
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					mml_channel_states[i].octave = octave;
				}
			}
		} else if (code == 'l') {
			int length = read_number(&content, &pos);
			if (length < 0) {
				error_handler("Invalid length", line, pos);
				return 1;
			}
			int dot = 0;
			while (*content == '.') {
				dot++;
				content++;
				pos++;
			}
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					mml_channel_states[i].default_length = length;
					mml_channel_states[i].default_length_dot = dot;
				}
			}
		} else if (code == 't') {
			int tempo = read_number(&content, &pos);
			if (tempo < 0) {
				error_handler("Invalid tempo", line, pos);
				return 1;
			}
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					mml_channel_states[i].tempo = tempo;
				}
			}
		} else if (code == 'v') {
			int volume = read_number(&content, &pos);
			if (volume < 0 || volume > 128) {
				error_handler("Invalid volume", line, pos);
				return 1;
			}
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					mml_channel_states[i].volume = volume;
				}
			}
		} else if (code == '<') {
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					if (mml_channel_states[i].octave == 0) {
						error_handler("Invalid octave step down", line, pos);
						return 1;
					}
					mml_channel_states[i].octave--;
				}
			}
		} else if (code == '>') {
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					if (mml_channel_states[i].octave == 9) {
						error_handler("Invalid octave step up", line, pos);
						return 1;
					}
					mml_channel_states[i].octave++;
				}
			}
		} else if (code == 'm') {
			// Music articulation
			double articulation;
			switch (*content) {
				case 'l': 
					articulation = ARTICULATION_LEGATO;
					break;
				case 'n': 
					articulation = ARTICULATION_NORMAL;
					break;
				case 's': 
					articulation = ARTICULATION_STACCATO;
					break;
				default:
					error_handler("Invalid music articulation", line, pos);
					return 1;
			}
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					mml_channel_states[i].articulation = articulation;
				}
			}
			pos++;
			content++;
		} else if ((isPause = (code == 'p' || code == 'r')) || (isNoteCode = code == 'n') || (code >= 'a' && code <= 'g')) {
			// Note or pause
			int length = -1;
			int dot = 0;
			int sharp = 0;
			int customLength = 0;
			int noteCode = -1;

			while (1) {
				char next = content[0];
				if (!isPause && !isNoteCode) {
					// Sharp/flat?
					if (next == '-' || next == '+' || next == '#') {
						// variation
						if (next == '-') {
							code--;
						}
						if (code == 'e' || code == 'b') {
							error_handler("Invalid sharp", line, pos);
							return 1;
						}
						sharp = 1;
						content++;
						pos++;
						continue;
					}
				}
				if (next >= '0' && next <= '9') {
					if (isNoteCode) {
						if (noteCode != -1) {
							error_handler("Invalid note code", line, pos);
							return 1;
						}
						noteCode = read_number(&content, &pos);
						if (noteCode < 0 || noteCode > 84) {
							error_handler("Invalid note code", line, pos);
							return 1;
						}
					} else {
						if (customLength) {
							error_handler("Invalid length", line, pos);
							return 1;
						}
						// Length
						length = read_number(&content, &pos);
						if (length < 0) {
							error_handler("Invalid length", line, pos);
							return 1;
						}
						customLength = 1;
					}
					continue;
				}
				if (next == '.') {
					// Half length
					dot++;
					content++;
					pos++;
					continue;
				}
				break;
			}

			// Set note
			for (int i = 0; i < mml_channel_count; i++) {
				if (mml_channel_states[i].isActive) {
					if (isNoteCode && noteCode == 0) {
						isPause = 1;
					}
					int frequency = isPause ? 0 : (isNoteCode ? get_freq_from_code(noteCode) : get_freq_from_note(code, sharp, mml_channel_states[i].octave));
					int time_scale = get_adsr_time_scale(&mml_channel_states[i], length < 0 ? mml_channel_states[i].default_length : length, (length < 0 && !dot) ? mml_channel_states[i].default_length_dot : dot);
					
					if (!add_channel_frame(i, frequency, time_scale, mml_channel_states[i].volume, mml_channel_states[i].articulation, join)) {
						return 1;
					}
				}
			}
		} else {
			error_handler("Unknown command", line, pos);
			return 1;
		}
	}

	printf("MML stats:\n");
	for (int i = 0; i < mml_channel_count; i++) {
		printf("\tchannel %d time %fs (%d samples)\n", i, (float)mml_channel_states[i].running_time.seconds, mml_channel_states[i].running_time.time_units);
	}

	free(mml_channel_states);
}

/*! 
 * Parse the MML file and produce sequencer frames map.
 */
int mml_compile(const char* content, struct seq_frame_map_t* map) {
	int ret = mml_parse(content);
	if (ret) {
		return ret;
	}
	*map = frame_map;
	return 0;
}

void mml_free(struct seq_frame_map_t* map) {
	for (int i = 0; i < map->channel_count; i++) {
		free(map->channels[i].frames);
	}
	free(map->channels);
}
