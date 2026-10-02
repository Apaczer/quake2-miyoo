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
 * The header file for the MP3 playback via libmpg123.
 *
 * =======================================================================
 */

#ifndef CL_SOUND_MP3_H
#define CL_SOUND_MP3_H

typedef enum
{
	MP3_PLAY,
	MP3_PAUSE,
	MP3_STOP
} mp3_status_t;

void MP3_InitTrackList(void);
void MP3_Init(void);
void MP3_PlayTrack(int track);
void MP3_RecoverState(void);
void MP3_SaveState(void);
void MP3_Shutdown(void);
void MP3_Stop(void);
void MP3_Stream(void);

#endif /* CL_SOUND_MP3_H */
