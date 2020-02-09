/*
    snes2psx: SNES controller to Playstation adapter
    Copyright (C) 2012-2014 Raphael Assenat <raph@raphnet.net>
    Modified 2020 Jeff Stenhouse

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#define CMD_BEGIN_01		0x01
#define CMD_GET_DATA_42		0x42
#define REP_DATA_START_5A	0x5a

#define DEVICE_ID_DIGITAL_PS1 0x41
#define DEVICE_ID_DUALSHOCK2  0x79

enum {
  ST_IDLE = 0,
  ST_READY,
  ST_SEND_BUF0,
  ST_SEND_BUF1,
  ST_ANALOGSTICKS,
  ST_ANALOGBUTTONS,
  ST_DONE
};

enum {
  DS2_ANALOG_R = 0,
  DS2_ANALOG_L,
  DS2_ANALOG_U,
  DS2_ANALOG_D,
  DS2_ANALOG_TRIANGLE,
  DS2_ANALOG_O,
  DS2_ANALOG_X,
  DS2_ANALOG_SQUARE,
  DS2_ANALOG_L1,
  DS2_ANALOG_R1,
  DS2_ANALOG_L2,
  DS2_ANALOG_R2,
  MAX_DS2_ANALOG_BUTTONS // Sometimes is used as a throw-away value
};

/******** IO port definitions **************/
#define SNES_LATCH_DDR  DDRC
#define SNES_LATCH_PORT PORTC
#define SNES_LATCH_BIT  (1<<4)

#define SNES_CLOCK_DDR  DDRC
#define SNES_CLOCK_PORT PORTC
#define SNES_CLOCK_BIT  (1<<5)

#define SNES_DATA_PORT  PORTC
#define SNES_DATA_DDR   DDRC
#define SNES_DATA_PIN   PINC
#define SNES_DATA_BIT   (1<<3)

#define PSX_ACK_PORT	PORTC
#define PSX_ACK_DDR		DDRC
#define PSX_ACK_PIN		PINC
#define PSX_ACK_BIT		(1<<0)

/********* IO pins manipulation macros **********/
#define SNES_LATCH_LOW()    do { SNES_LATCH_PORT &= ~(SNES_LATCH_BIT); } while(0)
#define SNES_LATCH_HIGH()   do { SNES_LATCH_PORT |= SNES_LATCH_BIT; } while(0)
#define SNES_CLOCK_LOW()    do { SNES_CLOCK_PORT &= ~(SNES_CLOCK_BIT); } while(0)
#define SNES_CLOCK_HIGH()   do { SNES_CLOCK_PORT |= SNES_CLOCK_BIT; } while(0)

#define SNES_GET_DATA() (SNES_DATA_PIN & SNES_DATA_BIT)

/*	PSX data : (MSb first)
		Left    Down  Right  Up    Start  R3  L3  Select
		Square  X     O      Tri.  R1     L1  R2  L2
*/
#define PSX_LEFT		0x8000
#define PSX_DOWN		0x4000
#define PSX_RIGHT		0x2000
#define PSX_UP			0x1000
#define PSX_START		0x0800
#define PSX_R3			0x0400
#define PSX_L3			0x0200
#define PSX_SELECT		0x0100
#define PSX_SQUARE		0x0080
#define PSX_X			0x0040
#define PSX_O			0x0020
#define PSX_TRIANGLE	0x0010
#define PSX_R1			0x0008
#define PSX_L1			0x0004
#define PSX_R2			0x0002
#define PSX_L2			0x0001

/*	SNES data, in the received order.
		B Y Select Start
		Up Down Left Right
		A X L R
		1 1 1 1
 */
#define SNES_B		0x8000
#define SNES_Y		0x4000
#define SNES_SELECT	0x2000
#define SNES_START	0x1000
#define SNES_UP		0x0800
#define SNES_DOWN	0x0400
#define SNES_LEFT	0x0200
#define SNES_RIGHT	0x0100
#define SNES_A		0x0080
#define SNES_X		0x0040
#define SNES_L		0x0020
#define SNES_R		0x0010

#define MAPPING_MASK (SNES_START | SNES_SELECT | SNES_A | SNES_B | SNES_X | SNES_Y | SNES_L)

struct map_ent {
	unsigned short s; // Snes bit
	unsigned short p; // PSX bit
  unsigned char analogByte;
};

static struct map_ent type1_mapping[] = {
		{ SNES_B, 		PSX_X,        DS2_ANALOG_X },
		{ SNES_Y, 		PSX_SQUARE,   DS2_ANALOG_SQUARE },
		{ SNES_SELECT,	PSX_SELECT, MAX_DS2_ANALOG_BUTTONS },
		{ SNES_START,	PSX_START,    MAX_DS2_ANALOG_BUTTONS },
		{ SNES_UP,		PSX_UP,       DS2_ANALOG_U },
		{ SNES_DOWN,	PSX_DOWN,     DS2_ANALOG_D },
		{ SNES_LEFT,	PSX_LEFT,     DS2_ANALOG_L },
		{ SNES_RIGHT,	PSX_RIGHT,    DS2_ANALOG_R },
		{ SNES_A,		PSX_O, DS2_ANALOG_O },
		{ SNES_X,		PSX_TRIANGLE, DS2_ANALOG_TRIANGLE },
		{ SNES_R,		PSX_R1, DS2_ANALOG_R1 },
		{ SNES_L,		PSX_L1, DS2_ANALOG_L1 },
		{ 0, 0, MAX_DS2_ANALOG_BUTTONS },
};

static struct map_ent type2_mapping[] = {
		{ SNES_B, 		PSX_O, DS2_ANALOG_O },
		{ SNES_Y, 		PSX_X, DS2_ANALOG_X },
    { SNES_SELECT,	PSX_SELECT, MAX_DS2_ANALOG_BUTTONS },
		{ SNES_START,	PSX_START,    MAX_DS2_ANALOG_BUTTONS },
    { SNES_UP,		PSX_UP,       DS2_ANALOG_U },
		{ SNES_DOWN,	PSX_DOWN,     DS2_ANALOG_D },
		{ SNES_LEFT,	PSX_LEFT,     DS2_ANALOG_L },
		{ SNES_RIGHT,	PSX_RIGHT,    DS2_ANALOG_R },
		{ SNES_A,		PSX_R2, DS2_ANALOG_R2 },
		{ SNES_X,		PSX_TRIANGLE, DS2_ANALOG_TRIANGLE },
		{ SNES_R,		PSX_R1, DS2_ANALOG_R1 },
		{ SNES_L,		PSX_SQUARE,   DS2_ANALOG_SQUARE },
		{ 0, 0, MAX_DS2_ANALOG_BUTTONS },
};

static struct map_ent type3_mapping[] = {
		{ SNES_B, 		PSX_TRIANGLE, DS2_ANALOG_TRIANGLE },
		{ SNES_Y, 		PSX_O, DS2_ANALOG_O },
    { SNES_SELECT,	PSX_SELECT, MAX_DS2_ANALOG_BUTTONS },
		{ SNES_START,	PSX_START,    MAX_DS2_ANALOG_BUTTONS },
    { SNES_UP,		PSX_UP,       DS2_ANALOG_U },
		{ SNES_DOWN,	PSX_DOWN,     DS2_ANALOG_D },
		{ SNES_LEFT,	PSX_LEFT,     DS2_ANALOG_L },
		{ SNES_RIGHT,	PSX_RIGHT,    DS2_ANALOG_R },
		{ SNES_A,		PSX_X,   DS2_ANALOG_X },
		{ SNES_X,		PSX_SQUARE,   DS2_ANALOG_SQUARE },
		{ SNES_R,		PSX_R1, DS2_ANALOG_R1 },
		{ SNES_L,		PSX_L1, DS2_ANALOG_L1 },
		{ 0, 0, MAX_DS2_ANALOG_BUTTONS },
};

static struct map_ent type4_mapping[] = {
		{ SNES_B, 		PSX_SQUARE,   DS2_ANALOG_SQUARE },
		{ SNES_Y, 		PSX_X,   DS2_ANALOG_X },
    { SNES_SELECT,	PSX_SELECT, MAX_DS2_ANALOG_BUTTONS },
		{ SNES_START,	PSX_START,    MAX_DS2_ANALOG_BUTTONS },
    { SNES_UP,		PSX_UP,       DS2_ANALOG_U },
		{ SNES_DOWN,	PSX_DOWN,     DS2_ANALOG_D },
		{ SNES_LEFT,	PSX_LEFT,     DS2_ANALOG_L },
		{ SNES_RIGHT,	PSX_RIGHT,    DS2_ANALOG_R },
		{ SNES_A,		PSX_TRIANGLE, DS2_ANALOG_TRIANGLE },
		{ SNES_X,		PSX_O, DS2_ANALOG_O },
		{ SNES_R,		PSX_R1, DS2_ANALOG_R1 },
		{ SNES_L,		PSX_L1, DS2_ANALOG_L1 },
		{ 0, 0, MAX_DS2_ANALOG_BUTTONS },
};

static struct map_ent type5_mapping[] = {
		{ SNES_B, 		PSX_O, DS2_ANALOG_O },
		{ SNES_Y, 		PSX_TRIANGLE, DS2_ANALOG_TRIANGLE },
    { SNES_SELECT,	PSX_SELECT, MAX_DS2_ANALOG_BUTTONS },
		{ SNES_START,	PSX_START,    MAX_DS2_ANALOG_BUTTONS },
    { SNES_UP,		PSX_UP,       DS2_ANALOG_U },
		{ SNES_DOWN,	PSX_DOWN,     DS2_ANALOG_D },
		{ SNES_LEFT,	PSX_LEFT,     DS2_ANALOG_L },
		{ SNES_RIGHT,	PSX_RIGHT,    DS2_ANALOG_R },
		{ SNES_A,		PSX_SQUARE,   DS2_ANALOG_SQUARE },
		{ SNES_X,		PSX_X,   DS2_ANALOG_X },
		{ SNES_R,		PSX_L1, DS2_ANALOG_L1 }, // L/R swapped
		{ SNES_L,		PSX_R1, DS2_ANALOG_R1 },
		{ 0, 0, MAX_DS2_ANALOG_BUTTONS },
};

static struct map_ent type6_mapping[] = { // Type 1 with L2/R2
		{ SNES_B, 		PSX_X,   DS2_ANALOG_X },
		{ SNES_Y, 		PSX_SQUARE,   DS2_ANALOG_SQUARE },
    { SNES_SELECT,	PSX_SELECT, MAX_DS2_ANALOG_BUTTONS },
		{ SNES_START,	PSX_START,    MAX_DS2_ANALOG_BUTTONS },
    { SNES_UP,		PSX_UP,       DS2_ANALOG_U },
		{ SNES_DOWN,	PSX_DOWN,     DS2_ANALOG_D },
		{ SNES_LEFT,	PSX_LEFT,     DS2_ANALOG_L },
		{ SNES_RIGHT,	PSX_RIGHT,    DS2_ANALOG_R },
		{ SNES_A,		PSX_O, DS2_ANALOG_O },
		{ SNES_X,		PSX_TRIANGLE, DS2_ANALOG_TRIANGLE },
		{ SNES_R,		PSX_R2, DS2_ANALOG_R2 },
		{ SNES_L,		PSX_L2, DS2_ANALOG_L2 },
		{ 0, 0, MAX_DS2_ANALOG_BUTTONS },
};

static struct map_ent type7_mapping[] = { // Type 1 with rotated directions for right-hand arcade stick steering
		{ SNES_B, 		PSX_X,   DS2_ANALOG_X },
		{ SNES_Y, 		PSX_SQUARE,   DS2_ANALOG_SQUARE },
    { SNES_SELECT,	PSX_SELECT, MAX_DS2_ANALOG_BUTTONS },
		{ SNES_START,	PSX_START,    MAX_DS2_ANALOG_BUTTONS },
		{ SNES_UP,		PSX_DOWN,     DS2_ANALOG_D },
		{ SNES_DOWN,	PSX_UP,       DS2_ANALOG_U },
		{ SNES_LEFT,	PSX_RIGHT,    DS2_ANALOG_R },
		{ SNES_RIGHT,	PSX_LEFT,     DS2_ANALOG_L },
		{ SNES_A,		PSX_O, DS2_ANALOG_O },
		{ SNES_X,		PSX_TRIANGLE, DS2_ANALOG_TRIANGLE },
		{ SNES_R,		PSX_L1, DS2_ANALOG_L1 },
		{ SNES_L,		PSX_R1, DS2_ANALOG_R1 },
		{ 0, 0, MAX_DS2_ANALOG_BUTTONS },
};

static struct map_ent *g_cur_map = type1_mapping;
static unsigned char state = ST_IDLE;
static volatile unsigned char psxbuf[2];
static unsigned char snesbuf[2];
static unsigned char deviceID = DEVICE_ID_DIGITAL_PS1;
static unsigned char numStickBytes = 0;
static unsigned char numButtonBytes = 0;
static unsigned char psxAnalogButtons[13];

#define CHIP_SELECT_ACTIVE()	(0 == (PINB & (1<<2)))

static void ack()
{
	_delay_us(1);

	// pull acknowledge
	PSX_ACK_PORT &= ~PSX_ACK_BIT;
	PSX_ACK_DDR	|= PSX_ACK_BIT;

	_delay_us(3);

	// release acknowledge
	PSX_ACK_DDR &= ~PSX_ACK_BIT;
}

ISR(SPI_STC_vect)
{
	unsigned char cmd;

	cmd = SPDR;

	switch(state)
	{
		case ST_IDLE: // Expecting 0x01
			if (cmd != CMD_BEGIN_01) {
				/* First byte is no 0x01? This is not a message for us (probably memory card)
				 *
				 * Ignore all other bytes until Slave Select is deasserted.
				 */
				while (CHIP_SELECT_ACTIVE()) {
					// Make sure we dont pull the bus low.

					if (SPSR & (1<<SPIF)) {
						cmd = SPDR;
						SPDR = 0x00; // dont pull the bus low (sends 0xff)
					}
				}
			}
			else {
				// Prepare the Device ID (default is 0x41)
				SPDR = 0xff ^ deviceID;
				state = ST_READY;
				ack();
			}
			break;

		case ST_READY: // Expecting 0x42
			if (cmd == CMD_GET_DATA_42) {
				SPDR = 0xff ^ REP_DATA_START_5A;
				state = ST_SEND_BUF0;
				ack();

			}
			break;

			// Based on Playstation.txt, I initially understood that the Playstation
			// would always send 0x00 when reading the button status and wrote code
			// that checked the received values.
			//
			// However, Einhander sends 0x40:
			//  Game...: 0x01, 0x42, 0x00, 0x40,   0x00
			//  Adapter: 0xFF, 0x41, 0x5a, dat[0], dat[1]
			//
			// And rollcage sends a 0x01:
			//  Game...: 0x01, 0x42, 0x01, 0x00,   0x00
			//  Adapter: 0xFF, 0x41, 0x5a, dat[0], dat[1]
			//
			// The unexpected values above prevented the games from working, so
			// I now treat the transmitted values during button status as "don't care"
			// rather than "expecting 0".
			//
			// This seems to be working well.
			//
		case ST_SEND_BUF0: // start of data 0x5a sent
				SPDR = 0xff ^ psxbuf[0];
				state = ST_SEND_BUF1;
				ack();
				break;

		case ST_SEND_BUF1: // psxbuf[0] sent
				SPDR = 0xff ^ psxbuf[1];
        if (deviceID == DEVICE_ID_DUALSHOCK2) state = ST_ANALOGSTICKS;
        else state = ST_DONE;
				ack();
				break;

    case ST_ANALOGSTICKS: // psxbuf[1] sent, faking DualShock 2 sticks
				SPDR = 0x80; // Sends 0x7F (default value for DS2 sticks)
        numStickBytes--;
        ack();
        while (numStickBytes) {
          if (SPSR & (1<<SPIF)) {
            numStickBytes--;
            SPDR = 0x80; // Send another 0x7F
            ack();
          }
        }
        state = ST_ANALOGBUTTONS;
				break;

    case ST_ANALOGBUTTONS: // Fake stick data sent, faking DualShock 2 analog buttons by sending either 0x00 or 0xFF
				SPDR = psxAnalogButtons[0];
        numButtonBytes++;
        ack();
        while (numButtonBytes < 12) {
          if (SPSR & (1<<SPIF)) {
            SPDR = psxAnalogButtons[numButtonBytes];
            numButtonBytes++;
            ack();
          }
        }
        state = ST_DONE;
				break;

		case ST_DONE: // All data sent
				SPDR = 0x00; // dont pull the bus low (send 0xff)
				state = ST_IDLE;
				break;
	}

}

/* update snesbuf[] */
static void snesUpdate(void)
{
	int i,j;
	unsigned char tmp=0;

	SNES_LATCH_HIGH();
	_delay_us(12);
	SNES_LATCH_LOW();

	for (j=0; j<2; j++)
	{
		for (i=0; i<8; i++)
		{
			_delay_us(6);
			SNES_CLOCK_LOW();

			tmp <<= 1;
			if (SNES_GET_DATA())
				tmp |= 1;

			_delay_us(6);

			SNES_CLOCK_HIGH();
		}
		snesbuf[j] = tmp;
	}

}

unsigned short snes2psx(unsigned short snesbits)
{
	unsigned short psxval;
	int i;
	struct map_ent *map = g_cur_map;

	/* Start with a ALL ones message and
	 * clear the bits when needed. */
	psxval = 0xffff;

	for (i=0; map[i].s; i++) {
		if (!(snesbits & map[i].s)) {
			psxval &= ~(map[i].p);
      psxAnalogButtons[map[i].analogByte] = 0x00; // Will send 0xFF
    }
    else
    {
      psxAnalogButtons[map[i].analogByte] = 0xFF; // Will send 0x00, or unpressed
    }
	}

	return psxval;
}

int main(void)
{
	/* PORT C
	 *    Name          Type
	 * 0: PSX ACT       Emulated OC
	 * 1: NC            OUT 0
	 * 2: NC            OUT 0
	 * 3: SNES DATA     IN - PU
	 *
	 * 4: SNES LATCH    OUT 0
	 * 5: SNES CLK      OUT 0
	 * 6: reset
	 */
	DDRC = 0xF6;
	PORTC = 0x08;

	/* PORT B
	 *
	 *          Name                    Type
	 * 0, 1, 2: Attention               Input   (The 3 pins are shorted together)
	 * 3      : CMD (MOSI) from PSX     Input
	 * 4      : DATA (MISO) to PSX      Output 0
	 * 5      : PSX CLK (SCK) from PSX  Input
	 * 6      : XTAL
	 * 7      : XTAL
	 */
	PORTB = 0;
	DDRB = 0x10;

	/* PORTD
	 *
	 *    Name         Type
	 * 0: USB          OUT 0
	 * 1: USB          OUT 0
	 * 2: USB          OUT 0
	 * 3: NC           OUT 0
	 * 4: VCC          OUT 1
	 * 5: NC           OUT 1
	 * 6: NC           OUT 1
	 * 7: NC           OUT 1
	 *
	 */
	PORTD = 0xFF;
	DDRD  = 0;



	/* Enable interrupt (SPIE)
	 * Enable SPI (SPE)
	 * Use LSB first transmission (DORD)
	 * Slave mode (MSTR not set)
	 * Clock normally high (CPOL)
	 * Data setup on leading edge (falling in this case) (CPHA)
	 * */
	SPCR = (1<<SPIE) | (1<<SPE) | (1<<DORD) | (1<<CPOL) | (1<<CPHA);
	SPDR = 0xff ^ 0xff;

	/* configure acknowledge pin. Simulate an open-collector
	 * by changing it's direction. */
	PSX_ACK_PORT &= ~PSX_ACK_BIT;
	PSX_ACK_DDR &= ~PSX_ACK_BIT;

	// buttons are active low and reserved bits stay high.
	psxbuf[0] = 0xff;
	psxbuf[1] = 0xff;

	// TODO: Snes stuff
	//
	// clock and latch as output
	SNES_LATCH_DDR |= SNES_LATCH_BIT;
	SNES_CLOCK_DDR |= SNES_CLOCK_BIT;

	// data as input
	SNES_DATA_DDR &= ~(SNES_DATA_BIT);
	// enable pullup. This should prevent random toggling of pins
	// when no controller is connected.
	SNES_DATA_PORT |= SNES_DATA_BIT;

	// clock is normally high
	SNES_CLOCK_PORT |= SNES_CLOCK_BIT;

	// LATCH is Active HIGH
	SNES_LATCH_PORT &= ~(SNES_LATCH_BIT);

	snesUpdate();

  unsigned short snesbits = 0xFFFF ^ (snesbuf[0]<<8 | snesbuf[1]);
	switch (snesbits & MAPPING_MASK)
	{
		case SNES_START:
			g_cur_map = type1_mapping;
			break;
		case SNES_SELECT:
			g_cur_map = type2_mapping;
			break;
		case SNES_A:
			g_cur_map = type3_mapping;
			break;
		case SNES_B:
			g_cur_map = type4_mapping;
			break;
		case SNES_X:
			g_cur_map = type5_mapping;
			break;
		case SNES_Y:
			g_cur_map = type6_mapping;
			break;
		case SNES_L:
			g_cur_map = type7_mapping;
			break;
	}
  if (snesbits & SNES_UP)
  {
    deviceID = DEVICE_ID_DUALSHOCK2;
  }
  memset(psxAnalogButtons, 0, 12);

	sei();
	while(1)
	{
		unsigned short psxbits;

		if (!CHIP_SELECT_ACTIVE()) {
			SPDR = 0x00;
			state = ST_IDLE;
      numStickBytes = 4;
      numButtonBytes = 0;
		}

		snesUpdate();

		psxbits = snes2psx((snesbuf[0]<<8) | snesbuf[1]);

		psxbuf[0] = psxbits >> 8;
		psxbuf[1] = psxbits & 0xff;
	}
}
