/*
 * main.c
 *
 * Example of reading from the SD card
 * This will read the size of the SD card
 * And then read the first block of data.
 */
#include <msp430.h>
#include <string.h> //For mem copy
#include "MMC.h"
#include "hal_SPI.h"
#include "hal_MMC_hardware_board.h"
#include "ringbuffer.h"

//Red onboard LED
const unsigned char LED = BIT0;
unsigned int blockIndex = 0;
unsigned int stereoBlockIndex = 0;

//This buffer will be written into TACCR1 on update
volatile float buffer = 0;
unsigned int value = 0;

RingBuffer bufferStack;
RingBuffer otherBufferStack;
#pragma vector=TIMER0_A0_VECTOR
__interrupt void TACCR0_INT(void) {
	++blockIndex;
	value = bufferStack.pop();
	TACCR1 = (int) value + 128;
	LPM1_EXIT;
}

#pragma vector=TIMER1_A0_VECTOR
__interrupt void timerA2(void) {
	++stereoBlockIndex;
	value = otherBufferStack.pop();
	TA1CCR1 = (int) value + 128;
	LPM1_EXIT;
}

unsigned long int pointerToWord(unsigned char* p) {
	return ((unsigned long int) p[0]) << 24 | ((unsigned long int) p[1]) << 16
			| ((unsigned long int) p[2]) << 8 | ((unsigned long int) p[3]);
}

int main(void) {
//Turn off the watchdog timer
	WDTCTL = WDTPW + WDTHOLD;

	P1DIR |= LED;
	P1OUT &= ~LED;

//Set the green LED to output
	P1DIR |= BIT6;
//Set up output from Timer A0.1
	P1SEL |= BIT6;

	P2DIR |= BIT3;
	P2SEL |= BIT3;


//Set DCO to 16 MHz calibrated
	DCOCTL = CALDCO_16MHZ;
	BCSCTL1 = CALBC1_16MHZ;

	/*Set up the Timer A module
	 //Set up TACCR0 to 1000
	 //Sourced from a clock running at 16MHz this
	 will give us 16,000 samples/second */
	TACCR0 = 1000;
	TA1CCR0 = 1000;

//TACCR1 will hold the bin width for each sample
	TACCR1 = 0;
	TA1CCR1 = 0;
//Set up the Timer A module
	TACTL = MC_1 | ID_0 | TASSEL_2;
	TA1CTL = MC_1 | ID_0 | TASSEL_2;

//Enable an interrupt when TAR == TACCR0
	TACCTL0 = CCIE;
	TA1CCTL0 = CCIE;
//Turn on internal PWM system that outputs on P1.6
//Set as we count up to TACCR1, reset up to TACCR0
	TA0CCTL1 = OUTMOD_7;
	TA1CCTL1 = OUTMOD_7;

	P2DIR |= BIT2;
	P2OUT &= ~BIT2;
	__delay_cycles(1600000); // 100ms @ 16MHz
	P2OUT |= BIT2;
	__delay_cycles(1600000); // 100ms @ 16MHz

	while (MMC_SUCCESS != mmcInit())
		;
	while (MMC_SUCCESS != mmcPing())
		;
	volatile unsigned long size = mmcReadCardSize(); //Check the memory card size
	P1OUT ^= LED; //Toggle the LED to indicate that reading was successful

	unsigned char block[64] = { 0 };
	volatile char result = mmcMountBlock(0x02fb400, 512);

	//Read in the block 64 bytes at a time
	if (result == MMC_SUCCESS) {

		//Read the 24 byte header
		spiReadFrame(block, 24);
		unsigned long int* words = (unsigned long int*) block;
		//These are stored in big-endian format
		volatile unsigned long int snd_offset = pointerToWord(block + 4);
		volatile long int snd_size = pointerToWord(block + 8);
		//We will only play 8bit PCM (decimal 2)
		volatile unsigned char data_encoding = pointerToWord(block + 12);
		//We will only play rates up to 16kHz
		volatile unsigned int sample_rate = pointerToWord(block + 16);
		volatile unsigned char channels = pointerToWord(block + 20);

		spiReadFrame(/*(void*)*/block, 40);

		unsigned int i; //loop variable
		unsigned int j;
		for (i = 0; i < 7; ++i) {

			spiReadFrame(/*(void*)*/block, 64); //64 bytes

		}

		mmcUnmountBlock();

		//Set up 128 byte buffer for songs
		unsigned char blockSong[128] = { 0 };
		//unsigned char SecondblockSong[128] = { 0 };

		unsigned long int startAddress = 0x02fb400 + 0x200;
		volatile char result = mmcMountBlock(startAddress, 512);
		if (result != MMC_SUCCESS) {
			return 0;
		}

		//Pre Load song data from
		for (i = 0; i <= 1; i++) {
			spiReadFrame((unsigned char*) blockSong, 128);

			for (j = 1; j <= 128; j = j + 2) {
				bufferStack.push(blockSong[j]);
				otherBufferStack.push(blockSong[j - 1]);
			}
		}
		volatile int checkBufferFull = bufferStack.isFull();
		volatile int checkBufferTwoFull = otherBufferStack.isFull();
		unsigned int OffsetToNextBlock = 2;
		unsigned int sizeReadCounter = 0;
		unsigned int endSong = snd_size / 512 ;
		__enable_interrupt();

		while (1) {
			LPM1;

			if (bufferStack.isEmpty()) {
				volatile int checkBufferEmpty = bufferStack.isEmpty();
				volatile int t = 0;
			}
			if (otherBufferStack.isEmpty()) {
				volatile int checkBufferEmpty = otherBufferStack.isEmpty();
				volatile int t = 0;
			}
			if (blockIndex >= 32) {
				spiReadFrame((unsigned char*)blockSong, 128);

				for (i = 1; i < 128; i = i + 2) {
					bufferStack.push(blockSong[i]);
				}
				volatile checkBufferFull = bufferStack.isFull();

				for (i = 0; i < 128; i = i + 2) {
					otherBufferStack.push(blockSong[i]);
				}
				volatile checkBufferSecondFull = otherBufferStack.isFull();

				blockIndex = 0;
				stereoBlockIndex = 0;
				OffsetToNextBlock++;
			}

			if (OffsetToNextBlock == 4) {
				sizeReadCounter++;
				mmcUnmountBlock();
				//End the song if over
				if (endSong >= sizeReadCounter){
					return 0;
				}
				startAddress = startAddress + 0x200;
				volatile char result = mmcMountBlock(startAddress, 512);
				if (result != MMC_SUCCESS) {
					int i = 1;
					return 0;
				}
				OffsetToNextBlock = 0;
			}

		}
	}

	return 0;

}
