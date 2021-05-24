/*!
 * Polyphonic synthesizer for microcontrollers.
 * (C) 2017 Stuart Longland
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
#ifndef _SYNTH_H
#define _SYNTH_H
#include "voice.h"
#include "debug.h"

#ifndef SYNTH_FREQ
/*!
 * Sample rate for the synthesizer: this needs to be declared in the
 * application.
 */
extern const uint16_t __attribute__((weak)) synth_freq;
#else
#define synth_freq		SYNTH_FREQ
#endif

/*!
 * Polyphonic synthesizer structure
 */
struct poly_synth_t {
	/*! Pointer to voices.  There may be up to 16 voices referenced. */
	struct voice_ch_t voice[VOICE_COUNT];
	/*!
	 * Bit-field enabling given voices.  This allows selective turning
	 * on and off of given voice channels.  If the corresponding bit is
	 * not a 1, then that channel is not computed.
	 *
	 * Note no bounds checking is done, if you have only defined 4
	 * channels, then only set bits 0-3, don't set bits 4 onwards here.
	 */
	CHANNEL_MASK_T enable;
};

extern struct poly_synth_t synth;

/*!
 * Compute the next synthesizer sample.
 */
static inline int8_t poly_synth_next() {
	int16_t sample = 0;
	CHANNEL_MASK_T mask = 1 << (VOICE_COUNT - 1);
    struct voice_ch_t* voice = &synth.voice[VOICE_COUNT - 1];

	do {
		if (synth.enable & mask) {
			/* Channel is enabled */
			sample += voice_ch_next(voice);
			if (voice->adsr.state_counter == ADSR_STATE_DONE) {
				//_DPRINTF("poly %p ch=%d done\n", synth, idx);
				synth.enable &= ~mask;
			}
		}
		mask >>= 1;
        voice--;
	} while (mask);

	/* Handle clipping */
	if (sample > INT8_MAX) {
		sample = INT8_MAX;
#ifdef CHECK_CLIPPING
		clip_count++;
#endif
	} else if (sample < INT8_MIN) {
		sample = INT8_MIN;
#ifdef CHECK_CLIPPING
		clip_count++;
#endif
	}
	return sample;
};
#endif
