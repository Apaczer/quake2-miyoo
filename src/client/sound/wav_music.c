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
 * This file implements background music playback from uncompressed WAV
 * files.  It uses SDL_LoadWAV to decode the file into a heap buffer and
 * then streams the PCM data into the engine's raw-sample queue (identical
 * to how OGG and MP3 backends work).  No additional libraries are needed
 * beyond SDL itself.
 *
 * Usage: compile with -DWAV_MUSIC (no extra link flags required).
 *
 * =======================================================================
 */

#ifdef WAV_MUSIC

#ifndef _WIN32
#include <sys/time.h>
#endif

#include <stdlib.h>
#include <string.h>

#ifdef SDL2
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif

#include "../header/client.h"
#include "header/local.h"
#include "header/wav_music.h"

/* ------------------------------------------------------------------ */
/* Statics                                                              */
/* ------------------------------------------------------------------ */

static cvar_t *wav_shuffle;        /* Shuffle playback */
static cvar_t *wav_ignoretrack0;   /* Toggle track 0 playing */
static cvar_t *wav_volume;         /* Music volume */
static int     wav_curfile;        /* Index of currently played file */
static int     wav_numbufs;        /* Number of buffers for OpenAL */
static int     wav_numsamples;     /* Number of samples fed so far */
static wav_music_status_t wav_status; /* Status indicator */
static qboolean wav_started;       /* Initialization flag */

/* Decoded PCM for the current track */
static Uint8  *wav_buf;           /* SDL-allocated PCM buffer */
static Uint32  wav_buf_len;       /* Total bytes in wav_buf */
static Uint32  wav_buf_pos;       /* Current read position (bytes) */
static SDL_AudioSpec wav_spec;    /* WAV format info */

enum { MAX_NUM_WAVTRACKS = 32 };
static char *wav_tracks[MAX_NUM_WAVTRACKS];
static int   wav_maxfileindex;

/* Saved state for save/restore across level transitions */
struct {
	qboolean saved;
	int curfile;
	int numsamples;
} wav_saved_state;

/* ------------------------------------------------------------------ */
/* GOG track mapping (identical logic to OGG/MP3 subsystems)          */
/* ------------------------------------------------------------------ */

enum GameType_wav {
	wav_other,   /* incl. baseq2 */
	wav_xatrix,
	wav_rogue
};

/*
 * The GOG version of Quake2 has music tracks in music/TrackXX.wav
 * Map CD track numbers to GOG file numbers.
 */
static int
getMappedGOGtrack_wav(int track, enum GameType_wav gameType)
{
	if (track <= 0)
		return 0;

	if (track == 1)
		return 0; /* data track on CD */

	if (gameType == wav_other)
		return track;
	if (gameType == wav_rogue)
		return track + 10;

	/* xatrix */
	switch (track)
	{
		case  2: return 9;
		case  3: return 13;
		case  4: return 14;
		case  5: return 7;
		case  6: return 16;
		case  7: return 2;
		case  8: return 15;
		case  9: return 3;
		case 10: return 4;
		case 11: return 18;
		default: return track;
	}
}

/* ------------------------------------------------------------------ */
/* Track list                                                           */
/* ------------------------------------------------------------------ */

/*
 * Load the list of WAV files found under the "music/" directory.
 * Mirrors OGG_InitTrackList()/MP3_InitTrackList() but searches for *.wav.
 */
void
WAV_InitTrackList(void)
{
	for (int i = 0; i < MAX_NUM_WAVTRACKS; ++i)
	{
		if (wav_tracks[i] != NULL)
		{
			free(wav_tracks[i]);
			wav_tracks[i] = NULL;
		}
	}

	wav_maxfileindex = 0;

	const char *potMusicDirs[3] = {0};
	char gameMusicDir[MAX_QPATH] = {0};
	cvar_t *gameCvar = Cvar_Get("game", "", CVAR_LATCH | CVAR_SERVERINFO);

	if (gameCvar == NULL || gameCvar->string[0] == '\0' ||
	    strcmp(BASEDIRNAME, gameCvar->string) == 0)
	{
		potMusicDirs[0] = BASEDIRNAME "/music/";
		potMusicDirs[1] = "music/";
		potMusicDirs[2] = NULL;
	}
	else
	{
		snprintf(gameMusicDir, MAX_QPATH, "%s/music/", gameCvar->string);
		potMusicDirs[0] = gameMusicDir;
		potMusicDirs[1] = "music/";
		potMusicDirs[2] = BASEDIRNAME "/music/";
	}

	enum GameType_wav gameType = wav_other;

	if (strcmp("xatrix", gameCvar->string) == 0)
		gameType = wav_xatrix;
	else if (strcmp("rogue", gameCvar->string) == 0)
		gameType = wav_rogue;

	for (int potIdx = 0;
	     potIdx < (int)(sizeof(potMusicDirs) / sizeof(potMusicDirs[0]));
	     ++potIdx)
	{
		const char *musicDir = potMusicDirs[potIdx];

		if (musicDir == NULL)
			break;

		for (const char *rawPath = FS_GetNextRawPath(NULL);
		     rawPath != NULL;
		     rawPath = FS_GetNextRawPath(rawPath))
		{
			char fullMusicPath[MAX_OSPATH] = {0};
			snprintf(fullMusicPath, MAX_OSPATH, "%s/%s", rawPath, musicDir);

			if (!Sys_IsDir(fullMusicPath))
				continue;

			char testFileName[MAX_OSPATH];

			/* Simple case: 02.wav … */
			snprintf(testFileName, MAX_OSPATH, "%s02.wav", fullMusicPath);

			if (Sys_IsFile(testFileName))
			{
				wav_tracks[2] = strdup(testFileName);

				for (int i = 3; i < MAX_NUM_WAVTRACKS; ++i)
				{
					snprintf(testFileName, MAX_OSPATH, "%s%02i.wav",
					         fullMusicPath, i);

					if (Sys_IsFile(testFileName))
					{
						wav_tracks[i] = strdup(testFileName);
						wav_maxfileindex = i;
					}
				}

				return;
			}

			/* GOG case: Track02.wav … */
			int gogTrack = getMappedGOGtrack_wav(8, gameType);
			snprintf(testFileName, MAX_OSPATH, "%sTrack%02i.wav",
			         fullMusicPath, gogTrack);

			if (Sys_IsFile(testFileName))
			{
				for (int i = 2; i < MAX_NUM_WAVTRACKS; ++i)
				{
					int gt = getMappedGOGtrack_wav(i, gameType);
					snprintf(testFileName, MAX_OSPATH, "%sTrack%02i.wav",
					         fullMusicPath, gt);

					if (Sys_IsFile(testFileName))
					{
						wav_tracks[i] = strdup(testFileName);
						wav_maxfileindex = i;
					}
				}

				return;
			}
		}
	}

	Com_Printf("No WAV music tracks have been found, so there will be no music.\n");
}

/* ------------------------------------------------------------------ */
/* Playback helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Stream a chunk of the pre-decoded WAV buffer into the raw-sample queue.
 */
static void
WAV_Read(void)
{
	if (wav_buf == NULL || wav_buf_pos >= wav_buf_len)
	{
		/* End of track — loop it. */
		wav_buf_pos    = 0;
		wav_numsamples = 0;
		wav_numbufs    = 0;
		return;
	}

	/* Determine how many bytes to push this call (up to 4096 * channels * 2) */
	int channels   = wav_spec.channels;
	int rate       = wav_spec.freq;

	/* We only handle 16-bit signed PCM here (guaranteed by SDL conversion) */
	Uint32 chunk = 4096 * (Uint32)(channels * sizeof(short));

	if (wav_buf_pos + chunk > wav_buf_len)
		chunk = wav_buf_len - wav_buf_pos;

	if (chunk == 0)
		return;

	int frames = (int)(chunk / (sizeof(short) * (Uint32)channels));

	wav_numsamples += frames;
	S_RawSamples(frames, rate, sizeof(short), channels,
	             wav_buf + wav_buf_pos, wav_volume->value);

	wav_buf_pos += chunk;
}

/* ------------------------------------------------------------------ */

/*
 * Called every frame to keep the audio buffers filled.
 */
void
WAV_Stream(void)
{
	if (!wav_started)
		return;

	if (wav_status == WAVMUS_PLAY)
	{
#ifdef USE_OPENAL
		if (sound_started == SS_OAL)
		{
			if (wav_numbufs == 0 || active_buffers < wav_numbufs - 256)
				wav_numbufs = active_buffers + 256;

			while (active_buffers <= wav_numbufs)
				WAV_Read();
		}
		else /* SDL */
#endif
		{
			if (sound_started == SS_SDL)
			{
				while (paintedtime + MAX_RAW_SAMPLES - 2048 > s_rawend)
					WAV_Read();
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Track control                                                        */
/* ------------------------------------------------------------------ */

/*
 * Start playing the WAV track that corresponds to the given CD track number.
 */
void
WAV_PlayTrack(int trackNo)
{
	/* Track 0 means "stop music". */
	if (trackNo == 0)
	{
		if (wav_ignoretrack0->value == 0)
			WAV_Stop();

		if (wav_curfile > 0)
			return;
	}

	/* Shuffle mode or track 0 with ignoretrack0=1 → pick a random track. */
	if ((trackNo == 0) || wav_shuffle->value)
	{
		if (wav_maxfileindex >= 0)
		{
			trackNo = randk() % (wav_maxfileindex + 1);
			int retries = 100;
			while (wav_tracks[trackNo] == NULL && retries-- > 0)
				trackNo = randk() % (wav_maxfileindex + 1);
		}
	}

	if (wav_maxfileindex == -1)
		return; /* no wav files; stay silent */

	if ((trackNo < 2) || (trackNo > wav_maxfileindex))
	{
		Com_Printf("WAV_PlayTrack: %d out of range.\n", trackNo);
		return;
	}

	if (wav_tracks[trackNo] == NULL)
	{
		Com_Printf("WAV_PlayTrack: Don't have a .wav file for track %d\n",
		           trackNo);
		return;
	}

	/* If already playing this track, do nothing. */
	if (wav_status == WAVMUS_PLAY)
	{
		if (wav_curfile == trackNo)
			return;
		else
			WAV_Stop();
	}

	/* Free any previously loaded buffer. */
	if (wav_buf != NULL)
	{
		SDL_FreeWAV(wav_buf);
		wav_buf     = NULL;
		wav_buf_len = 0;
		wav_buf_pos = 0;
	}

	/* Load the WAV file via SDL. */
	SDL_AudioSpec obtained;
	Uint8  *raw_buf = NULL;
	Uint32  raw_len = 0;

	if (SDL_LoadWAV(wav_tracks[trackNo], &obtained, &raw_buf, &raw_len) == NULL)
	{
		Com_Printf("WAV_PlayTrack: SDL_LoadWAV('%s') failed: %s\n",
		           wav_tracks[trackNo], SDL_GetError());
		return;
	}

	/* Build an SDL audio conversion if the file format differs from
	 * what the engine expects (signed 16-bit, stereo, 44100 Hz). */
	SDL_AudioCVT cvt;
	int need_cvt = SDL_BuildAudioCVT(&cvt,
	                                  obtained.format, obtained.channels, obtained.freq,
	                                  AUDIO_S16SYS, 2, 44100);

	if (need_cvt < 0)
	{
		Com_Printf("WAV_PlayTrack: SDL_BuildAudioCVT failed: %s\n", SDL_GetError());
		SDL_FreeWAV(raw_buf);
		return;
	}

	if (need_cvt > 0)
	{
		cvt.len = (int)raw_len;
		cvt.buf = (Uint8 *)malloc((size_t)(cvt.len * cvt.len_mult));

		if (cvt.buf == NULL)
		{
			Com_Printf("WAV_PlayTrack: out of memory during format conversion\n");
			SDL_FreeWAV(raw_buf);
			return;
		}

		memcpy(cvt.buf, raw_buf, raw_len);
		SDL_FreeWAV(raw_buf);

		if (SDL_ConvertAudio(&cvt) < 0)
		{
			Com_Printf("WAV_PlayTrack: SDL_ConvertAudio failed: %s\n", SDL_GetError());
			free(cvt.buf);
			return;
		}

		wav_buf     = cvt.buf;
		wav_buf_len = (Uint32)cvt.len_cvt;

		/* Update spec to reflect the converted format. */
		wav_spec.freq     = 44100;
		wav_spec.format   = AUDIO_S16SYS;
		wav_spec.channels = 2;
	}
	else
	{
		/* No conversion needed; copy out of the SDL-allocated buffer so
		 * we can manage its lifetime uniformly via free(). */
		wav_buf = (Uint8 *)malloc(raw_len);

		if (wav_buf == NULL)
		{
			Com_Printf("WAV_PlayTrack: out of memory\n");
			SDL_FreeWAV(raw_buf);
			return;
		}

		memcpy(wav_buf, raw_buf, raw_len);
		SDL_FreeWAV(raw_buf);

		wav_buf_len = raw_len;
		wav_spec    = obtained;
	}

	wav_buf_pos    = 0;
	wav_curfile    = trackNo;
	wav_numsamples = 0;
	wav_status     = WAVMUS_PLAY;
}

/* ------------------------------------------------------------------ */
/* Stop / pause                                                         */
/* ------------------------------------------------------------------ */

/*
 * Stop WAV playback.
 */
void
WAV_Stop(void)
{
	if (wav_status == WAVMUS_STOP)
		return;

#ifdef USE_OPENAL
	if (sound_started == SS_OAL)
		AL_UnqueueRawSamples();
#endif

	if (wav_buf != NULL)
	{
		free(wav_buf);
		wav_buf     = NULL;
		wav_buf_len = 0;
		wav_buf_pos = 0;
	}

	wav_status  = WAVMUS_STOP;
	wav_numbufs = 0;
}

/*
 * Toggle pause / resume.
 */
static void
WAV_TogglePlayback(void)
{
	if (wav_status == WAVMUS_PLAY)
	{
		wav_status  = WAVMUS_PAUSE;
		wav_numbufs = 0;

#ifdef USE_OPENAL
		if (sound_started == SS_OAL)
			AL_UnqueueRawSamples();
#endif
	}
	else if (wav_status == WAVMUS_PAUSE)
	{
		wav_status = WAVMUS_PLAY;
	}
}

/* ------------------------------------------------------------------ */
/* Console commands                                                     */
/* ------------------------------------------------------------------ */

/*
 * Print information about the WAV subsystem state.
 */
static void
WAV_Info(void)
{
	Com_Printf("Tracks:\n");

	for (int i = 2; i <= wav_maxfileindex; i++)
	{
		if (wav_tracks[i])
			Com_Printf(" - %02d %s\n", i, wav_tracks[i]);
		else
			Com_Printf(" - %02d <none>\n", i);
	}

	Com_Printf("Total: %d WAV files.\n", wav_maxfileindex + 1);

	switch (wav_status)
	{
		case WAVMUS_PLAY:
			Com_Printf("State: Playing track %d (%s) at %d samples.\n",
			           wav_curfile, wav_tracks[wav_curfile], wav_numsamples);
			break;

		case WAVMUS_PAUSE:
			Com_Printf("State: Paused track %d (%s) at %d samples.\n",
			           wav_curfile, wav_tracks[wav_curfile], wav_numsamples);
			break;

		case WAVMUS_STOP:
			if (wav_curfile == -1)
				Com_Printf("State: Stopped.\n");
			else
				Com_Printf("State: Stopped track %d (%s).\n",
				           wav_curfile, wav_tracks[wav_curfile]);
			break;
	}
}

/*
 * Print a short help message for the 'wav' console command.
 */
static void
WAV_HelpMsg(void)
{
	Com_Printf("Unknown sub command %s\n\n", Cmd_Argv(1));
	Com_Printf("Commands:\n");
	Com_Printf(" - info: Print information about playback state and tracks\n");
	Com_Printf(" - play <track>: Play track number <track>\n");
	Com_Printf(" - stop: Stop playback\n");
	Com_Printf(" - toggle: Toggle pause\n");
}

/*
 * The 'wav' console command.
 */
static void
WAV_Cmd(void)
{
	if (Cmd_Argc() < 2)
	{
		WAV_HelpMsg();
		return;
	}

	if (Q_stricmp(Cmd_Argv(1), "info") == 0)
	{
		WAV_Info();
	}
	else if (Q_stricmp(Cmd_Argv(1), "play") == 0)
	{
		if (Cmd_Argc() != 3)
		{
			Com_Printf("wav play <track> : Play <track>\n");
			return;
		}

		int track = (int)strtol(Cmd_Argv(2), NULL, 10);

		if (track < 2 || track > wav_maxfileindex)
		{
			Com_Printf("invalid track %s, must be a number between 2 and %d\n",
			           Cmd_Argv(2), wav_maxfileindex);
			return;
		}

		WAV_PlayTrack(track);
	}
	else if (Q_stricmp(Cmd_Argv(1), "stop") == 0)
	{
		WAV_Stop();
	}
	else if (Q_stricmp(Cmd_Argv(1), "toggle") == 0)
	{
		WAV_TogglePlayback();
	}
	else
	{
		WAV_HelpMsg();
	}
}

/* ------------------------------------------------------------------ */
/* State save / restore (for level transitions)                         */
/* ------------------------------------------------------------------ */

void
WAV_SaveState(void)
{
	if (wav_status != WAVMUS_PLAY)
	{
		wav_saved_state.saved = false;
		return;
	}

	wav_saved_state.saved      = true;
	wav_saved_state.curfile    = wav_curfile;
	wav_saved_state.numsamples = wav_numsamples;
}

void
WAV_RecoverState(void)
{
	if (!wav_saved_state.saved)
		return;

	/* Disable shuffle temporarily so the exact track is restored. */
	int shuffle_state = (int)wav_shuffle->value;
	Cvar_SetValue("wav_shuffle", 0);

	WAV_PlayTrack(wav_saved_state.curfile);

	/* Seek to the saved position by fast-forwarding the byte offset. */
	if (wav_status == WAVMUS_PLAY && wav_buf != NULL)
	{
		/* bytes = samples * channels * sizeof(short) */
		Uint32 byte_pos = (Uint32)(wav_saved_state.numsamples *
		                           wav_spec.channels * (int)sizeof(short));

		if (byte_pos < wav_buf_len)
		{
			wav_buf_pos    = byte_pos;
			wav_numsamples = wav_saved_state.numsamples;
		}
	}

	Cvar_SetValue("wav_shuffle", shuffle_state);
}

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                      */
/* ------------------------------------------------------------------ */

/*
 * Initialize the WAV music subsystem.
 */
void
WAV_Init(void)
{
	/* CVars */
	wav_shuffle      = Cvar_Get("wav_shuffle",      "0",   CVAR_ARCHIVE);
	wav_ignoretrack0 = Cvar_Get("wav_ignoretrack0", "0",   CVAR_ARCHIVE);
	wav_volume       = Cvar_Get("wav_volume",       "0.7", CVAR_ARCHIVE);

	cvar_t *wav_enabled = Cvar_Get("wav_enable", "1", CVAR_ARCHIVE);

	if (wav_enabled->value != 1)
		return;

	/* Console command */
	Cmd_AddCommand("wav", WAV_Cmd);

	/* Global state */
	wav_curfile    = -1;
	wav_numsamples = 0;
	wav_status     = WAVMUS_STOP;
	wav_buf        = NULL;
	wav_buf_len    = 0;
	wav_buf_pos    = 0;

	wav_started = true;
}

/*
 * Shut down the WAV music subsystem and free all resources.
 */
void
WAV_Shutdown(void)
{
	if (!wav_started)
		return;

	WAV_Stop();

	/* Free track list. */
	for (int i = 0; i < MAX_NUM_WAVTRACKS; ++i)
	{
		if (wav_tracks[i] != NULL)
		{
			free(wav_tracks[i]);
			wav_tracks[i] = NULL;
		}
	}
	wav_maxfileindex = 0;

	/* Remove console command. */
	Cmd_RemoveCommand("wav");

	wav_started = false;
}

#endif /* WAV_MUSIC */
