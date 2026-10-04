/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA.
 *
 * =======================================================================
 *
 * The header file for the WAV music playback via SDL_LoadWAV.
 *
 * =======================================================================
 */

#ifndef CL_SOUND_WAV_MUSIC_H
#define CL_SOUND_WAV_MUSIC_H

typedef enum
{
	WAVMUS_PLAY,
	WAVMUS_PAUSE,
	WAVMUS_STOP
} wav_music_status_t;

void WAV_InitTrackList(void);
void WAV_Init(void);
void WAV_PlayTrack(int track);
void WAV_RecoverState(void);
void WAV_SaveState(void);
void WAV_Shutdown(void);
void WAV_Stop(void);
void WAV_Stream(void);

#endif /* CL_SOUND_WAV_MUSIC_H */
