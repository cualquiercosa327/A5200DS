/*
 * pokey.c - POKEY sound chip emulation
 *
 * Copyright (C) 1995-1998 David Firth
 * Copyright (C) 1998-2005 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"
#include "atari.h"
#include "cpu.h"
#include "pia.h"
#include "pokey.h"
#include "gtia.h"
#include "sio.h"
#include "input.h"
#include "statesav.h"
#include "pokeysnd.h"
#include "antic.h"
#include "cassette.h"

int pokeyBufIdx __attribute__((section(".dtcm")))= 0;
char pokey_buffer[SNDLENGTH] __attribute__((section(".dtcm")));

UBYTE KBCODE __attribute__((section(".dtcm")));
UBYTE SERIN __attribute__((section(".dtcm")));
UBYTE IRQST __attribute__((section(".dtcm")));
UBYTE IRQEN __attribute__((section(".dtcm")));
UBYTE SKSTAT __attribute__((section(".dtcm")));
UBYTE SKCTLS __attribute__((section(".dtcm")));
int DELAYED_SERIN_IRQ;
int DELAYED_SEROUT_IRQ;
int DELAYED_XMTDONE_IRQ;

/* structures to hold the 9 pokey control bytes */
UBYTE AUDF[4 * MAXPOKEYS];	/* AUDFx (D200, D202, D204, D206) */
UBYTE AUDC[4 * MAXPOKEYS];	/* AUDCx (D201, D203, D205, D207) */
UBYTE AUDCTL[MAXPOKEYS];	/* AUDCTL (D208) */
int DivNIRQ[4], DivNMax[4];
int Base_mult[MAXPOKEYS];		/* selects either 64Khz or 15Khz clock mult */

UBYTE POT_input[8] = {228, 228, 228, 228, 228, 228, 228, 228};
UBYTE PCPOT_input[8] = {112, 112, 112, 112, 112, 112,112, 112};
UBYTE POT_all;
UBYTE pot_scanline;

UBYTE poly9_lookup[511];
UBYTE poly17_lookup[16385];
static ULONG random_scanline_counter;

ULONG POKEY_GetRandomCounter(void)
{
	return random_scanline_counter;
}

void POKEY_SetRandomCounter(ULONG value)
{
	random_scanline_counter = value;
}

UBYTE POKEY_GetByte(UWORD addr)
{
	UBYTE byte = 0xff;

	addr &= 0x0f;
	switch (addr) {
	case _POT0:
	case _POT1:
	case _POT2:
	case _POT3:
	case _POT4:
	case _POT5:
	case _POT6:
	case _POT7:
    if (!POTENA)
      return 228;
		if (POT_input[addr] <= pot_scanline) {
			return POT_input[addr];
    }
    return pot_scanline;
    break;
	case _ALLPOT:
		{
			unsigned int i;
			for (i = 0; i < 8; i++)
				if (POT_input[i] <= pot_scanline)
					byte &= ~(1 << i);		// reset bit if pot value known 
		}
    return byte;
    //return POT_all;
		break;
	case _KBCODE:
		//byte = KBCODE;
				if ( SKCTLS & 0x01 )
					return 0xff;
				else
					return KBCODE | ((random_scanline_counter & 0x1)<<5);
		break;
	case _RANDOM:
		if ((SKCTLS & 0x03) != 0) {
			int i = random_scanline_counter + XPOS;
			if (AUDCTL[0] & POLY9)
				byte = poly9_lookup[i % POLY9_SIZE];
			else {
				const UBYTE *ptr;
				i %= POLY17_SIZE;
				ptr = poly17_lookup + (i >> 3);
				i &= 7;
				byte = (UBYTE) ((ptr[0] >> i) + (ptr[1] << (8 - i)));
			}
		}
		break;
	case _SERIN:
		byte = SERIN;
		break;
	case _IRQST:
		byte = IRQST;
		break;
	case _SKSTAT:
		byte = SKSTAT + (CASSETTE_IOLineStatus() << 4);
		break;
	}

	return byte;
}

void Update_Counter(int chan_mask);


#ifndef SOUND_GAIN /* sound gain can be pre-defined in the configure/Makefile */
#define SOUND_GAIN 4
#endif

void POKEY_PutByte(UWORD addr, UBYTE byte)
{
	addr &= 0x0f;
	switch (addr) {
	case _AUDC1:
        AUDC[CHAN1] = byte;
        Update_pokey_sound(_AUDC1, byte, 0, SOUND_GAIN);
		break;
	case _AUDC2:
        AUDC[CHAN2] = byte;
        Update_pokey_sound(_AUDC2, byte, 0, SOUND_GAIN);
		break;
	case _AUDC3:
        AUDC[CHAN3] = byte;
        Update_pokey_sound(_AUDC3, byte, 0, SOUND_GAIN);
		break;
	case _AUDC4:
        AUDC[CHAN4] = byte;
        Update_pokey_sound(_AUDC4, byte, 0, SOUND_GAIN);
		break;
	case _AUDCTL:
		AUDCTL[0] = byte;

		/* determine the base multiplier for the 'div by n' calculations */
		if (byte & CLOCK_15)
			Base_mult[0] = DIV_15;
		else
			Base_mult[0] = DIV_64;

		Update_Counter((1 << CHAN1) | (1 << CHAN2) | (1 << CHAN3) | (1 << CHAN4));
		Update_pokey_sound(_AUDCTL, byte, 0, SOUND_GAIN);
		break;
	case _AUDF1:
		AUDF[CHAN1] = byte;
		Update_Counter((AUDCTL[0] & CH1_CH2) ? ((1 << CHAN2) | (1 << CHAN1)) : (1 << CHAN1));
		Update_pokey_sound(_AUDF1, byte, 0, SOUND_GAIN);
		break;
	case _AUDF2:
		AUDF[CHAN2] = byte;
		Update_Counter(1 << CHAN2);
		Update_pokey_sound(_AUDF2, byte, 0, SOUND_GAIN);
		break;
	case _AUDF3:
		AUDF[CHAN3] = byte;
		Update_Counter((AUDCTL[0] & CH3_CH4) ? ((1 << CHAN4) | (1 << CHAN3)) : (1 << CHAN3));
		Update_pokey_sound(_AUDF3, byte, 0, SOUND_GAIN);
		break;
	case _AUDF4:
		AUDF[CHAN4] = byte;
		Update_Counter(1 << CHAN4);
		Update_pokey_sound(_AUDF4, byte, 0, SOUND_GAIN);
		break;
	case _IRQEN:
		IRQEN = byte;
		IRQST |= ~byte & 0xf7;	/* Reset disabled IRQs except XMTDONE */
		if (IRQEN & 0x20) {
			SLONG delay;
			delay = CASSETTE_GetInputIRQDelay();
			if (delay > 0)
				DELAYED_SERIN_IRQ = delay;
		}
		if ((~IRQST & IRQEN) == 0)
			IRQ = 0;
		break;
	case _SKRES:
		SKSTAT |= 0xe0;
		break;
	case _POTGO:
		if (!(SKCTLS & 4)) {
			pot_scanline = 0;	/* slow pot mode */
    }  
		break;
	case _SEROUT:
		break;
	case _STIMER:
		DivNIRQ[CHAN1] = DivNMax[CHAN1];
		DivNIRQ[CHAN2] = DivNMax[CHAN2];
		DivNIRQ[CHAN4] = DivNMax[CHAN4];
		Update_pokey_sound(_STIMER, byte, 0, SOUND_GAIN);
		break;
	case _SKCTLS:
		SKCTLS = byte;
        Update_pokey_sound(_SKCTLS, byte, 0, SOUND_GAIN);
		if (byte & 4)
			pot_scanline = 228;	/* fast pot mode - return results immediately */
		break;
	}
}

void POKEY_Initialise(void)
{
	int i;
	ULONG reg;

	/* Initialise Serial Port Interrupts */
	DELAYED_SERIN_IRQ = 0;
	DELAYED_SEROUT_IRQ = 0;
	DELAYED_XMTDONE_IRQ = 0;

	KBCODE = 0xff;
	SERIN = 0x00;	/* or 0xff ? */
	IRQST = 0xff;
	IRQEN = 0x00;
	SKSTAT = 0xef;
	SKCTLS = 0x00;

	for (i = 0; i < (MAXPOKEYS * 4); i++) {
		AUDC[i] = 0;
		AUDF[i] = 0;
	}

	for (i = 0; i < MAXPOKEYS; i++) {
		AUDCTL[i] = 0;
		Base_mult[i] = DIV_64;
	}

	for (i = 0; i < 4; i++)
		DivNIRQ[i] = DivNMax[i] = 0;

	pot_scanline = 0;

	/* initialise poly9_lookup */
	reg = 0x1ff;
	for (i = 0; i < 511; i++) {
		reg = ((((reg >> 5) ^ reg) & 1) << 8) + (reg >> 1);
		poly9_lookup[i] = (UBYTE) reg;
	}
	/* initialise poly17_lookup */
	reg = 0x1ffff;
	for (i = 0; i < 16385; i++) {
		reg = ((((reg >> 5) ^ reg) & 0xff) << 9) + (reg >> 8);
		poly17_lookup[i] = (UBYTE) (reg >> 1);
	}

	random_scanline_counter = 0;
}

void POKEY_Frame(void) 
{
	random_scanline_counter %= (AUDCTL[0] & POLY9) ? POLY9_SIZE : POLY17_SIZE;
}

/***************************************************************************
 ** Generate POKEY Timer IRQs if required                                 **
 ** called on a per-scanline basis, not very precise, but good enough     **
 ** for most applications                                                 **
 ***************************************************************************/
void POKEY_Scanline(void) 
{
    Pokey_process(&pokey_buffer[pokeyBufIdx], 1);	// Each scanline, compute 1 output samples. This corresponds to a 15720Khz output sample rate if running at 60FPS (good enough)
    pokeyBufIdx = (pokeyBufIdx+1) & (SNDLENGTH-1);

    if (pot_scanline < 228)
		pot_scanline++;
  
    POT_input[0] = PCPOT_input[0]; POT_input[1] = PCPOT_input[1]; POT_input[2] = PCPOT_input[2]; POT_input[3] = PCPOT_input[3];

	random_scanline_counter += LINE_C;

	if ((DivNIRQ[CHAN1] -= LINE_C) < 0 ) {
		DivNIRQ[CHAN1] += DivNMax[CHAN1];
		if (IRQEN & 0x01) {
			IRQST &= 0xfe;
			GenerateIRQ();
		}
	}

	if ((DivNIRQ[CHAN2] -= LINE_C) < 0 ) {
		DivNIRQ[CHAN2] += DivNMax[CHAN2];
		if (IRQEN & 0x02) {
			IRQST &= 0xfd;
			GenerateIRQ();
		}
	}

	if ((DivNIRQ[CHAN4] -= LINE_C) < 0 ) {
		DivNIRQ[CHAN4] += DivNMax[CHAN4];
		if (IRQEN & 0x04) {
			IRQST &= 0xfb;
			GenerateIRQ();
		}
	}
}

/*****************************************************************************/
/* Module:  Update_Counter()                                                 */
/* Purpose: To process the latest control values stored in the AUDF, AUDC,   */
/*          and AUDCTL registers.  It pre-calculates as much information as  */
/*          possible for better performance.  This routine has been added    */
/*          here again as I need the precise frequency for the pokey timers  */
/*          again. The pokey emulation is therefore somewhat sub-optimal     */
/*          since the actual pokey emulation should grab the frequency values */
/*          directly from here instead of calculating them again.            */
/*                                                                           */
/* Author:  Ron Fries,Thomas Richter                                         */
/* Date:    March 27, 1998                                                   */
/*                                                                           */
/* Inputs:  chan_mask: Channel mask, one bit per channel.                    */
/*          The channels that need to be updated                             */
/*                                                                           */
/* Outputs: Adjusts local globals - no return value                          */
/*                                                                           */
/*****************************************************************************/

void Update_Counter(int chan_mask)
{

/************************************************************/
/* As defined in the manual, the exact Div_n_cnt values are */
/* different depending on the frequency and resolution:     */
/*    64 kHz or 15 kHz - AUDF + 1                           */
/*    1 MHz, 8-bit -     AUDF + 4                           */
/*    1 MHz, 16-bit -    AUDF[CHAN1]+256*AUDF[CHAN2] + 7    */
/************************************************************/

	/* only reset the channels that have changed */

	if (chan_mask & (1 << CHAN1)) {
		/* process channel 1 frequency */
		if (AUDCTL[0] & CH1_179)
			DivNMax[CHAN1] = AUDF[CHAN1] + 4;
		else
			DivNMax[CHAN1] = (AUDF[CHAN1] + 1) * Base_mult[0];
		if (DivNMax[CHAN1] < LINE_C)
			DivNMax[CHAN1] = LINE_C;
	}

	if (chan_mask & (1 << CHAN2)) {
		/* process channel 2 frequency */
		if (AUDCTL[0] & CH1_CH2) {
			if (AUDCTL[0] & CH1_179)
				DivNMax[CHAN2] = AUDF[CHAN2] * 256 + AUDF[CHAN1] + 7;
			else
				DivNMax[CHAN2] = (AUDF[CHAN2] * 256 + AUDF[CHAN1] + 1) * Base_mult[0];
		}
		else
			DivNMax[CHAN2] = (AUDF[CHAN2] + 1) * Base_mult[0];
		if (DivNMax[CHAN2] < LINE_C)
			DivNMax[CHAN2] = LINE_C;
	}

	if (chan_mask & (1 << CHAN4)) {
		/* process channel 4 frequency */
		if (AUDCTL[0] & CH3_CH4) {
			if (AUDCTL[0] & CH3_179)
				DivNMax[CHAN4] = AUDF[CHAN4] * 256 + AUDF[CHAN3] + 7;
			else
				DivNMax[CHAN4] = (AUDF[CHAN4] * 256 + AUDF[CHAN3] + 1) * Base_mult[0];
		}
		else
			DivNMax[CHAN4] = (AUDF[CHAN4] + 1) * Base_mult[0];
		if (DivNMax[CHAN4] < LINE_C)
			DivNMax[CHAN4] = LINE_C;
	}
}

void POKEYStateSave(void)
{
}

void POKEYStateRead(void)
{
}
