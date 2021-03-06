//*****************************************************************************
//
//      "cs4281.c" --  Cirrus Logic-Crystal CS4281 linux audio driver.
//
//      Copyright (C) 2000  Cirrus Logic Corp.  
//            -- adapted from drivers by Thomas Sailer, 
//            -- but don't bug him; Problems should go to:
//            -- gw boynton (wesb@crystal.cirrus.com) or
//            -- tom woller (twoller@crystal.cirrus.com).
//
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; either version 2 of the License, or
//      (at your option) any later version.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// Module command line parameters:
//   none
//
//  Supported devices:
//  /dev/dsp    standard /dev/dsp device, (mostly) OSS compatible
//  /dev/mixer  standard /dev/mixer device, (mostly) OSS compatible
//  /dev/midi   simple MIDI UART interface, no ioctl
//
//
//

// *****************************************************************************

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/sound.h>
#include <linux/malloc.h>
#include <linux/soundcard.h>
#include <linux/pci.h>
#include <linux/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <asm/spinlock.h>
#include <asm/uaccess.h>
#include <asm/hardirq.h>
#include <linux/vmalloc.h>
#include "dm.h"
#include "cs4281_hwdefs.h"

EXPORT_NO_SYMBOLS;

#undef OSS_DOCUMENTED_MIXER_SEMANTICS

// --------------------------------------------------------------------- 

#ifndef PCI_VENDOR_ID_CIRRUS
#define PCI_VENDOR_ID_CIRRUS          0x1013
#endif
#ifndef PCI_DEVICE_ID_CRYSTAL_CS4281
#define PCI_DEVICE_ID_CRYSTAL_CS4281  0x6005
#endif

#define CS4281_MAGIC  ((PCI_DEVICE_ID_CRYSTAL_CS4281<<16) | PCI_VENDOR_ID_CIRRUS)

#include <linux/version.h>

#if LINUX_VERSION_CODE < 0x02030d
#ifdef MODULE
#define __exit
#define module_exit(x) void cleanup_module(void) { x(); }
#define module_init(x) int init_module(void) { return x(); }
#else
#define __exit __attribute__ ((unused, __section__ (".text.init")))
#define module_exit(x) // nothing 
#define module_init(x) // nothing 
#endif

#define DECLARE_WAIT_QUEUE_HEAD(w) struct wait_queue *w = NULL
#define DECLARE_WAITQUEUE(w,c) struct wait_queue w = {(c), NULL}
#define wait_queue_head_t struct wait_queue *
#define init_waitqueue_head(w) *(w) = 0
#define init_MUTEX(m) *(m) = MUTEX
#endif

// MIDI buffer sizes 
#define MIDIINBUF  500
#define MIDIOUTBUF 500

#define FMODE_MIDI_SHIFT 3
#define FMODE_MIDI_READ  (FMODE_READ << FMODE_MIDI_SHIFT)
#define FMODE_MIDI_WRITE (FMODE_WRITE << FMODE_MIDI_SHIFT)


struct cs4281_state {
        // magic 
        unsigned int magic;

        // we keep the cards in a linked list 
        struct cs4281_state *next;

        // pcidev is needed to turn off the DDMA controller at driver shutdown 
        struct pci_dev *pcidev;

        // soundcore stuff 
        int dev_audio;
        int dev_mixer;
        int dev_midi;
// *wb*    int dev_dmfm;

        // hardware resources 
// *wb*    unsigned long iobase, sbbase, vcbase, ddmabase, mpubase, gpbase; // long for SPARC   
        unsigned int pBA0phys, pBA1phys;
        char *pBA0, *pBA1; 
        unsigned int irq;
	int endofbuffer;

        // mixer registers 
        struct {
                unsigned short vol[10];
                unsigned int recsrc;
                unsigned int modcnt;
                unsigned short micpreamp;
        } mix;

        // wave stuff   // Note that play & record formats must be the same *wb.
        unsigned fmt;
        unsigned channels;
        unsigned rate;
        unsigned char clkdiv;
        unsigned ena;

        spinlock_t lock;
        struct semaphore open_sem;
        mode_t open_mode;
        wait_queue_head_t open_wait;

        struct dmabuf {
                void *rawbuf;            // Physical address of  
                unsigned buforder;       // Log base 2 of 'rawbuf' size in bytes..
                unsigned numfrag;        // # of 'fragments' in the buffer.
                unsigned fragshift;      // Log base 2 of fragment size.
                unsigned hwptr, swptr;
                unsigned total_bytes;    // # bytes process since open.
                int count;
                unsigned error; // over/underrun 
                wait_queue_head_t wait;
                // redundant, but makes calculations easier 
                unsigned fragsize;       // 2**fragshift..
                unsigned dmasize;        // 2**buforder.
                unsigned fragsamples;
                // OSS stuff 
                unsigned mapped:1;       // Buffer mapped in cs4281_mmap()?
                unsigned ready:1;        // prog_dmabuf_dac()/adc() successful?
                unsigned endcleared:1;
                unsigned ossfragshift;
                int ossmaxfrags;
                unsigned subdivision;
        } dma_dac, dma_adc;

        // midi stuff 
        struct {
                unsigned ird, iwr, icnt;
                unsigned ord, owr, ocnt;
                wait_queue_head_t iwait;
                wait_queue_head_t owait;
                struct timer_list timer;
                unsigned char ibuf[MIDIINBUF];
                unsigned char obuf[MIDIOUTBUF];
        } midi;

};


struct cs4281_state *devs = NULL;
// --------------------------------------------------------------------- 
//
//		Hardware Interfaces For the CS4281
//


//******************************************************************************
// "delayus()-- Delay for the specified # of microseconds.
//******************************************************************************
static void delayus(u32 delay)
{
	u32 j;   
	if(delay > 9999)
	{
		j = (delay * HZ)/1000000;   /* calculate delay in jiffies  */
		if(j<1) 
			j=1;               /* minimum one jiffy. */
		current->state = TASK_UNINTERRUPTIBLE;  
		schedule_timeout(j);       
	}
	else
		udelay(delay);
	return;                      
}


//******************************************************************************
// "cs4281_read_ac97" -- Reads a word from the specified location in the
//               CS4281's address space(based on the BA0 register).
//
// 1. Write ACCAD = Command Address Register = 46Ch for AC97 register address
// 2. Write ACCDA = Command Data Register = 470h for data to write to AC97 register,
//                                            0h for reads.
// 3. Write ACCTL = Control Register = 460h for initiating the write
// 4. Read ACCTL = 460h, DCV should be reset by now and 460h = 17h
// 5. if DCV not cleared, break and return error
// 6. Read ACSTS = Status Register = 464h, check VSTS bit
//****************************************************************************
static int cs4281_read_ac97(struct cs4281_state *card, u32 offset, u32 *value)
{
	u32 count, status;

        // Make sure that there is not data sitting
        // around from a previous uncompleted access.
        // ACSDA = Status Data Register = 47Ch
	status = readl(card->pBA0+BA0_ACSDA);

        // Setup the AC97 control registers on the CS4281 to send the
        // appropriate command to the AC97 to perform the read.
        // ACCAD = Command Address Register = 46Ch
        // ACCDA = Command Data Register = 470h
        // ACCTL = Control Register = 460h
        // bit DCV - will clear when process completed
        // bit CRW - Read command
        // bit VFRM - valid frame enabled
        // bit ESYN - ASYNC generation enabled

        // Get the actual AC97 register from the offset
	writel(offset - BA0_AC97_RESET, card->pBA0+BA0_ACCAD);
	writel(0, card->pBA0+BA0_ACCDA);
	writel(ACCTL_DCV | ACCTL_CRW | ACCTL_VFRM | ACCTL_ESYN, card->pBA0+BA0_ACCTL);

         // Wait for the read to occur.
	for(count = 0; count < 10; count++)
	{
		// First, we want to wait for a short time.
		udelay(25);

		// Now, check to see if the read has completed.
        	// ACCTL = 460h, DCV should be reset by now and 460h = 17h
        	if( !(readl(card->pBA0+BA0_ACCTL) & ACCTL_DCV))
			break;
	}

         // Make sure the read completed.
	if(readl(card->pBA0+BA0_ACCTL) & ACCTL_DCV)
 	       return 1;

         // Wait for the valid status bit to go active.
	for(count = 0; count < 10; count++)
	{
        	// Read the AC97 status register.
        	// ACSTS = Status Register = 464h
        	status = readl(card->pBA0+BA0_ACSTS);

		// See if we have valid status.
        	// VSTS - Valid Status
        	if(status & ACSTS_VSTS)
        		break;
		// Wait for a short while.
        	udelay(25);
	}

         // Make sure we got valid status.
	if(!(status & ACSTS_VSTS))
        	return 1;

	// Read the data returned from the AC97 register.
	// ACSDA = Status Data Register = 474h
	*value = readl(card->pBA0+BA0_ACSDA);

	// Success.
	return(0);
}


//****************************************************************************
//
// "cs4281_write_ac97()"-- writes a word to the specified location in the
// CS461x's address space (based on the part's base address zero register).
//
// 1. Write ACCAD = Command Address Register = 46Ch for AC97 register address
// 2. Write ACCDA = Command Data Register = 470h for data to write to AC97 reg.
// 3. Write ACCTL = Control Register = 460h for initiating the write
// 4. Read ACCTL = 460h, DCV should be reset by now and 460h = 07h
// 5. if DCV not cleared, break and return error
//
//****************************************************************************
static int cs4281_write_ac97(struct cs4281_state *card, u32 offset, u32 value)
{
	u32 count, status;

         // Setup the AC97 control registers on the CS4281 to send the
         // appropriate command to the AC97 to perform the read.
         // ACCAD = Command Address Register = 46Ch
         // ACCDA = Command Data Register = 470h
         // ACCTL = Control Register = 460h
         // set DCV - will clear when process completed
         // reset CRW - Write command
         // set VFRM - valid frame enabled
         // set ESYN - ASYNC generation enabled
         // set RSTN - ARST# inactive, AC97 codec not reset

         // Get the actual AC97 register from the offset

	writel(offset - BA0_AC97_RESET, card->pBA0+BA0_ACCAD);
	writel(value, card->pBA0+BA0_ACCDA);
	writel(ACCTL_DCV | ACCTL_VFRM | ACCTL_ESYN, card->pBA0+BA0_ACCTL);

         // Wait for the write to occur.
	for(count = 0; count < 10; count++)
	{
		// First, we want to wait for a short time.
        	udelay(25);
		// Now, check to see if the write has completed.
		// ACCTL = 460h, DCV should be reset by now and 460h = 07h
		status = readl(card->pBA0+BA0_ACCTL);
		if(!(status & ACCTL_DCV))
			break;
	}

        // Make sure the write completed.
	if(status & ACCTL_DCV)
	        return 1;
	// Success.
	return 0;
}


//******************************************************************************
// "Init4281()" -- Bring up the part.
//******************************************************************************
static int cs4281_hw_init(struct cs4281_state *card)
{
	u32 ac97_slotid;
	u32 temp1, temp2;

	//***************************************
	//  Set up the Sound System Configuration
	//***************************************

         // Set the 'Configuration Write Protect' register
         // to 4281h.  Allows vendor-defined configuration
         // space between 0e4h and 0ffh to be written.

	writel(0x4281, card->pBA0+BA0_CWPR);                       // (3e0h)

         // (0), Blast the clock control register to zero so that the
         // PLL starts out in a known state, and blast the master serial
         // port control register to zero so that the serial ports also
         // start out in a known state.

	writel(0, card->pBA0+BA0_CLKCR1);                          // (400h)
	writel(0, card->pBA0+BA0_SERMC);                           // (420h)


         // (1), Make ESYN go to zero to turn off
         // the Sync pulse on the AC97 link.

	writel(0, card->pBA0+BA0_ACCTL);
	udelay(50);


         // (2) Drive the ARST# pin low for a minimum of 1uS (as defined in
         // the AC97 spec) and then drive it high.  This is done for non
         // AC97 modes since there might be logic external to the CS461x
         // that uses the ARST# line for a reset.

	writel(0, card->pBA0+BA0_SPMC);                            // (3ech)
	udelay(100);
	writel(SPMC_RSTN, card->pBA0+BA0_SPMC);
	delayus(50000);     // Wait 50 ms for ABITCLK to become stable.

        // (3) Turn on the Sound System Clocks.
	writel(CLKCR1_PLLP, card->pBA0+BA0_CLKCR1);                // (400h)
	delayus(50000);     // Wait for the PLL to stabilize.
	// Turn on clocking of the core (CLKCR1(400h) = 0x00000030)
	writel(CLKCR1_PLLP | CLKCR1_SWCE, card->pBA0+BA0_CLKCR1);

        // (4) Power on everything for now..
	writel(0x7E, card->pBA0 + BA0_SSPM);                       // (740h)

        // (5) Wait for clock stabilization.
	for(temp1=0; temp1<1000;  temp1++)
	{
		udelay(1000);
		if(readl(card->pBA0+BA0_CLKCR1) & CLKCR1_DLLRDY)
			break;
	}
	if(!(readl(card->pBA0+BA0_CLKCR1) & CLKCR1_DLLRDY))
	{
		printk(KERN_ERR "cs4281: DLLRDY failed!\n");
		return -EIO;
	}

	// (6) Enable ASYNC generation.
	writel(ACCTL_ESYN, card->pBA0+BA0_ACCTL);                  // (460h)

	// Now wait 'for a short while' to allow the  AC97
	// part to start generating bit clock. (so we don't
	// Try to start the PLL without an input clock.)
	delayus(50000);

        // Set the serial port timing configuration, so that the
        // clock control circuit gets its clock from the right place.
	writel(SERMC_PTC_AC97, card->pBA0+BA0_SERMC);              // (420h)=2.

        // (7) Wait for the codec ready signal from the AC97 codec.

	for(temp1=0; temp1<1000; temp1++)
	{
		// Delay a mil to let things settle out and
		// to prevent retrying the read too quickly.
		udelay(1000);
		if( readl(card->pBA0+BA0_ACSTS) & ACSTS_CRDY )	// If ready,  (464h)
			break;                                  //   exit the 'for' loop.
	}
	if( !(readl(card->pBA0+BA0_ACSTS) & ACSTS_CRDY) )       // If never came ready,
	{
        	printk(KERN_ERR "cs4281: ACSTS never came ready!\n");
        	return -EIO;                                //   exit initialization.
	}

         // (8) Assert the 'valid frame' signal so we can
         // begin sending commands to the AC97 codec.
	writel(ACCTL_VFRM | ACCTL_ESYN, card->pBA0+BA0_ACCTL);   // (460h)

         // (9), Wait until CODEC calibration is finished.
         // Print an error message if it doesn't.
	for(temp1 = 0; temp1 < 1000; temp1++)
	{
		delayus(10000);
		// Read the AC97 Powerdown Control/Status Register.
		cs4281_read_ac97(card, BA0_AC97_POWERDOWN, &temp2);
		if( (temp2 & 0x0000000F) == 0x0000000F )
			break;
	}
	if ( (temp2 & 0x0000000F) != 0x0000000F )
	{
		printk(KERN_ERR "cs4281: Codec failed to calibrate.  Status = %.8x.\n", temp2);
		return -EIO;
	}

         // (10), Set the serial port timing configuration, so that the
         // clock control circuit gets its clock from the right place.
	writel(SERMC_PTC_AC97, card->pBA0+BA0_SERMC);              // (420h)=2.


         // (11) Wait until we've sampled input slots 3 & 4 as valid, meaning
         // that the codec is pumping ADC data across the AC link.
	for(temp1=0; temp1<1000; temp1++)
	{
        	// Delay a mil to let things settle out and
        	// to prevent retrying the read too quickly.
        	delayus(1000);    //(test)

        	// Read the input slot valid register;  See
        	// if input slots 3 and 4 are valid yet.
        	if( (readl(card->pBA0+BA0_ACISV) & (ACISV_ISV3 | ACISV_ISV4) ) ==  (ACISV_ISV3 | ACISV_ISV4))
			break;    // Exit the 'for' if slots are valid.
	}
       	// If we never got valid data, exit initialization.
	if( (readl(card->pBA0+BA0_ACISV) & (ACISV_ISV3 | ACISV_ISV4) ) != (ACISV_ISV3 | ACISV_ISV4))
	{
       		printk(KERN_ERR "cs4281: Never got valid data!\n");
       		return -EIO;     // If no valid data, exit initialization.
	}

	// (12), Start digital data transfer of audio data to the codec.
	writel(ACOSV_SLV3 | ACOSV_SLV4, card->pBA0+BA0_ACOSV);             // (468h)


        //**************************************
        // Unmute the Master and Alternate
        // (headphone) volumes.  Set to max.
        //**************************************
	cs4281_write_ac97(card,BA0_AC97_HEADPHONE_VOLUME, 0);
	cs4281_write_ac97(card,BA0_AC97_MASTER_VOLUME, 0);

        //******************************************
        // Power on the DAC(AddDACUser()from main())
        //******************************************
	cs4281_read_ac97(card,BA0_AC97_POWERDOWN, &temp1);
	cs4281_write_ac97(card,BA0_AC97_POWERDOWN, temp1 &= 0xfdff);

        // Wait until we sample a DAC ready state.
	for(temp2=0; temp2<32; temp2++)
	{
		// Let's wait a mil to let things settle.
		delayus(1000);
		// Read the current state of the power control reg.
        	cs4281_read_ac97(card, BA0_AC97_POWERDOWN, &temp1);
		// If the DAC ready state bit is set, stop waiting.
		if(temp1 & 0x2)
        		break;
	}

        //******************************************
        // Power on the ADC(AddADCUser()from main())
        //******************************************
	cs4281_read_ac97(card, BA0_AC97_POWERDOWN, &temp1);
	cs4281_write_ac97(card, BA0_AC97_POWERDOWN, temp1 &= 0xfeff);

        // Wait until we sample ADC ready state.
	for(temp2=0; temp2<32; temp2++)
	{
		// Let's wait a mil to let things settle.
		delayus(1000);
        	// Read the current state of the power control reg.
        	cs4281_read_ac97(card, BA0_AC97_POWERDOWN, &temp1);
		// If the ADC ready state bit is set, stop waiting.
		if(temp1 & 0x1)
        		break;
	}
	// Set up 4281 Register contents that
	// don't change for boot duration.

	// For playback, we map AC97 slot 3 and 4(Left
	// & Right PCM playback) to DMA Channel 0.
	// Set the fifo to be 15 bytes at offset zero.

	ac97_slotid = 0x01000f00;	// FCR0.RS[4:0]=1(=>slot4, right PCM playback).
        				// FCR0.LS[4:0]=0(=>slot3, left PCM playback).
                                 	// FCR0.SZ[6-0]=15; FCR0.OF[6-0]=0.
	writel(ac97_slotid, card->pBA0 + BA0_FCR0);                 // (180h)
	writel(ac97_slotid | FCRn_FEN, card->pBA0 + BA0_FCR0);      // Turn on FIFO Enable.

        // For capture, we map AC97 slot 10 and 11(Left
        // and Right PCM Record) to DMA Channel 1.
        // Set the fifo to be 15 bytes at offset sixteen.
	ac97_slotid = 0x0B0A0f10;	// FCR1.RS[4:0]=11(=>slot11, right PCM record).
               				// FCR1.LS[4:0]=10(=>slot10, left PCM record).
					// FCR1.SZ[6-0]=15; FCR1.OF[6-0]=16.
	writel(ac97_slotid | FCRn_PSH, card->pBA0 + BA0_FCR1);      // (184h)
	writel(ac97_slotid | FCRn_FEN, card->pBA0 + BA0_FCR1);      // Turn on FIFO Enable.

        // Map the Playback SRC to the same AC97 slots(3 & 4--
        // --Playback left & right)as DMA channel 0.
        // Map the record SRC to the same AC97 slots(10 & 11--
        // -- Record left & right) as DMA channel 1.

	ac97_slotid = 0x0b0a0100;	// SCRSA.PRSS[4:0]=1(=>slot4, right PCM playback).
					// SCRSA.PLSS[4:0]=0(=>slot3, left PCM playback).
					// SCRSA.CRSS[4:0]=11(=>slot11, right PCM record)
					// SCRSA.CLSS[4:0]=10(=>slot10, left PCM record).
	writel(ac97_slotid, card->pBA0 + BA0_SRCSA);                // (75ch)

        // Set 'Half Terminal Count Interrupt Enable' and 'Terminal
        // Count Interrupt Enable' in DMA Control Registers 0 & 1.
        // Set 'MSK' flag to 1 to keep the DMA engines paused.
	temp1 = (DCRn_HTCIE | DCRn_TCIE | DCRn_MSK);	 // (00030001h)
	writel(temp1, card->pBA0 + BA0_DCR0);            // (154h
	writel(temp1, card->pBA0 + BA0_DCR1);            // (15ch)

        // Set 'Auto-Initialize Control' to 'enabled'; For playback,
        // set 'Transfer Type Control'(TR[1:0]) to 'read transfer',
        // for record, set Transfer Type Control to 'write transfer'.
        // All other bits set to zero;  Some will be changed @ transfer start.
	temp1 = (DMRn_DMA | DMRn_AUTO | DMRn_TR_READ);   // (20000018h)
	writel(temp1, card->pBA0 + BA0_DMR0);            // (150h)
	temp1 = (DMRn_DMA | DMRn_AUTO | DMRn_TR_WRITE);  // (20000014h)
	writel(temp1, card->pBA0 + BA0_DMR1);            // (158h)

        // Enable DMA interrupts generally, and
        // DMA0 & DMA1 interrupts specifically.
	temp1 = readl(card->pBA0 + BA0_HIMR) &  0xfffbfcff;
	writel(temp1, card->pBA0+BA0_HIMR);
	return 0;
}


//******************************************************************************
// "cs4281_play_rate()" --
//******************************************************************************
static void cs4281_play_rate(struct cs4281_state *card, u32 playrate)
{
	u32 DACSRvalue = 1;

         // Based on the sample rate, program the DACSR register.
	if(playrate == 8000)
        	DACSRvalue = 5;
	if(playrate == 11025)
		DACSRvalue = 4;
	else if(playrate == 22050)
        	DACSRvalue = 2;
	else if(playrate == 44100)
	        DACSRvalue = 1;
	else if((playrate <= 48000) && (playrate >= 6023))
        	DACSRvalue = 24576000/(playrate*16);
	else if(playrate < 6023)
		// Not allowed by open.
		return;
	else if(playrate > 48000)
		// Not allowed by open.
		return;
         //  Write the 'sample rate select code'
         //  to the 'DAC Sample Rate' register.
	writel(DACSRvalue, card->pBA0 + BA0_DACSR);           // (744h)
}

//******************************************************************************
// "cs481_record_rate()" -- Initialize the record sample rate converter.
//******************************************************************************
static void cs481_record_rate(struct cs4281_state *card, u32 outrate)
{
	u32 ADCSRvalue = 1;

         //
         // Based on the sample rate, program the ADCSR register
         //
	if(outrate == 8000)
        	ADCSRvalue = 5;
	if(outrate == 11025)
	        ADCSRvalue = 4;
	else if(outrate == 22050)
		ADCSRvalue = 2;
	else if(outrate == 44100)
		ADCSRvalue = 1;
	else if((outrate <= 48000) && (outrate >= 6023))
		ADCSRvalue = 24576000/(outrate*16);
	else if(outrate < 6023)
	{
		// Not allowed by open.
		return;
	}
	else if(outrate > 48000)
	{
	        // Not allowed by open.
	        return;
	}
        //  Write the 'sample rate select code
	//  to the 'ADC Sample Rate' register.
	writel(ADCSRvalue, card->pBA0 + BA0_ADCSR);           // (748h)
}



static void stop_dac(struct cs4281_state *s)
{
        unsigned long flags;
        unsigned temp1;

        spin_lock_irqsave(&s->lock, flags);
        s->ena &= ~FMODE_WRITE;
        temp1 = readl(s->pBA0+ BA0_DCR0) | DCRn_MSK;     
        writel(temp1, s->pBA0+BA0_DCR0);         

        spin_unlock_irqrestore(&s->lock, flags);
}


static void start_dac(struct cs4281_state *s)
{
        unsigned long flags;
        unsigned temp1;

        spin_lock_irqsave(&s->lock, flags);
        if (!(s->ena & FMODE_WRITE) && (s->dma_dac.mapped ||
              s->dma_dac.count > 0) && s->dma_dac.ready)      {
                s->ena |= FMODE_WRITE;
                temp1 = readl(s->pBA0+BA0_DCR0) & ~DCRn_MSK;     // Clear DMA0 channel mask.
                writel(temp1, s->pBA0+BA0_DCR0);                 // Start DMA'ing.
                writel(HICR_IEV | HICR_CHGM, s->pBA0+BA0_HICR);     // Enable interrupts.              
        
                writel(7, s->pBA0+BA0_PPRVC);
                writel(7, s->pBA0+BA0_PPLVC);
        
        }
        spin_unlock_irqrestore(&s->lock, flags);
}


static void stop_adc(struct cs4281_state *s)
{
        unsigned long flags;
        unsigned temp1;

        spin_lock_irqsave(&s->lock, flags);
        s->ena &= ~FMODE_READ;
        temp1 = readl(s->pBA0+ BA0_DCR1) | DCRn_MSK;     
        writel(temp1, s->pBA0+BA0_DCR1);
        spin_unlock_irqrestore(&s->lock, flags);
}


static void start_adc(struct cs4281_state *s)
{
        unsigned long flags;
        unsigned temp1;

        spin_lock_irqsave(&s->lock, flags);
        if (!(s->ena & FMODE_READ) && (s->dma_adc.mapped
              || s->dma_adc.count <= 
                 (signed)(s->dma_adc.dmasize - 2*s->dma_adc.fragsize))
              && s->dma_adc.ready)
        {
                s->ena |= FMODE_READ;
                temp1 = readl(s->pBA0+BA0_DCR1) & ~ DCRn_MSK;         // Clear DMA1 channel mask bit.
                writel(temp1, s->pBA0+BA0_DCR1);                      // Start recording
                writel(HICR_IEV | HICR_CHGM, s->pBA0+BA0_HICR);       // Enable interrupts.
        }
        spin_unlock_irqrestore(&s->lock, flags);

}


// --------------------------------------------------------------------- 
#define DMABUF_DEFAULTORDER (15-PAGE_SHIFT)     // == 3(for PC), = log base 2( buff sz = 32k).
#define DMABUF_MINORDER 1                       // ==> min buffer size = 8K.


static void dealloc_dmabuf(struct dmabuf *db)
{
        unsigned long map, mapend;

        if (db->rawbuf) {
                // Undo prog_dmabuf()'s marking the pages as reserved 
                mapend = MAP_NR(db->rawbuf + (PAGE_SIZE << db->buforder) - 1);
                for (map = MAP_NR(db->rawbuf); map <= mapend; map++)
                        clear_bit(PG_reserved, &mem_map[map].flags);
                free_pages((unsigned long)db->rawbuf, db->buforder);
        }
        db->rawbuf = NULL;
        db->mapped = db->ready = 0;
}


static int prog_dmabuf(struct cs4281_state *s, struct dmabuf *db, int gfp_mask)
{
        int order;
        unsigned bytespersec, temp1;
        unsigned bufs, sample_shift = 0;
        unsigned long map, mapend;

        db->hwptr = db->swptr = db->total_bytes = db->count = db->error = db->endcleared = 0;
        if (!db->rawbuf) {
                db->ready = db->mapped = 0;
                for (order = DMABUF_DEFAULTORDER; order >= DMABUF_MINORDER; order--)
                        if ((db->rawbuf = (void *)__get_free_pages(gfp_mask, order)))
                                break;
                if (!db->rawbuf)
                        return -ENOMEM;
                db->buforder = order;
                // Now mark the pages as reserved; otherwise the 
                // remap_page_range() in cs4281_mmap doesn't work.
                // 1. get index to last page in mem_map array for rawbuf.
                mapend = MAP_NR(db->rawbuf + (PAGE_SIZE << db->buforder) - 1);
                
                     // 2. mark each physical page in range as 'reserved'.
                for (map = MAP_NR(db->rawbuf); map <= mapend; map++)
                        set_bit(PG_reserved, &mem_map[map].flags);
        }
        if (s->fmt & (AFMT_S16_LE | AFMT_U16_LE))
                sample_shift++;
        if (s->channels > 1)
                sample_shift++;
        bytespersec = s->rate << sample_shift;
        bufs = PAGE_SIZE << db->buforder;


#define INTERRUPT_RATE_MS       100                      // Interrupt rate in milliseconds.
        db->numfrag = 2;
        temp1 = bytespersec/(1000/INTERRUPT_RATE_MS);    // Nominal frag size(bytes/interrupt)
        db->fragshift = 8;                               // Min 256 bytes.
        while( 1 << db->fragshift  < temp1)              // Calc power of 2 frag size.
                db->fragshift +=1;
        db->fragsize = 1 << db->fragshift;               
        db->dmasize = db->fragsize * 2;
 
                // If the calculated size is larger than the allocated
                //  buffer, divide the allocated buffer into 2 fragments.
        if(db->dmasize > bufs) {
                db->numfrag = 2;                                 // Two fragments.
                db->fragsize = bufs >> 1;                        // Each 1/2 the alloc'ed buffer.
                db->fragsamples = db->fragsize >> sample_shift;  // # samples/fragment.
                db->dmasize =  bufs;                             // Use all the alloc'ed buffer.
                
                db->fragshift = 0;                               // Calculate 'fragshift'.
                temp1 = db->fragsize;                            // update_ptr() uses it 
                while( (temp1 >>=1) > 1)                         // to calc 'total-bytes'
                     db->fragshift +=1;                          // returned in DSP_GETI/OPTR. 
        }
        return 0;
}    


static int prog_dmabuf_adc(struct cs4281_state *s)
{
        unsigned long va;
        unsigned count;      
        int c;
        stop_adc(s);
        if ((c = prog_dmabuf(s, &s->dma_adc, GFP_KERNEL | GFP_DMA)))
                return c;
             
        va = virt_to_bus(s->dma_adc.rawbuf);
        
        count = s->dma_adc.dmasize;       
       
        if(s->fmt & (AFMT_S16_LE | AFMT_U16_LE | AFMT_S16_BE | AFMT_U16_BE))
                count /= 2;                      // 16-bit.
                        
        if(s->channels > 1)
                count /= 2;                      // Assume stereo.
          
        writel(va, s->pBA0+BA0_DBA1);            // Set buffer start address.
        writel(count-1, s->pBA0+BA0_DBC1);       // Set count. 
        s->dma_adc.ready = 1;
        return 0;
}


static int prog_dmabuf_dac(struct cs4281_state *s)
{
        unsigned long va;
        unsigned count;
        int c;
        stop_dac(s);
        if ((c = prog_dmabuf(s, &s->dma_dac, GFP_KERNEL)))
                return c;
        memset(s->dma_dac.rawbuf, (s->fmt & (AFMT_U8 | AFMT_U16_LE))
                                   ? 0x80 : 0, s->dma_dac.dmasize);      

        va = virt_to_bus(s->dma_dac.rawbuf);

        count = s->dma_dac.dmasize;       
        if(s->fmt & (AFMT_S16_LE | AFMT_U16_LE | AFMT_S16_BE | AFMT_U16_BE))
                count /= 2;                      // 16-bit.
      
        if(s->channels > 1)
                count /= 2;                      // Assume stereo.
           
        writel(va, s->pBA0+BA0_DBA0);               // Set buffer start address.
        writel(count-1, s->pBA0+BA0_DBC0);       // Set count.             

        s->dma_dac.ready = 1;
        return 0;
}


static void clear_advance(void *buf, unsigned bsize, unsigned bptr, unsigned len, unsigned char c)
{
        if (bptr + len > bsize) {
                unsigned x = bsize - bptr;
                memset(((char *)buf) + bptr, c, x);
                bptr = 0;
                len -= x;
        }
        memset(((char *)buf) + bptr, c, len);
}



// call with spinlock held! 
static void cs4281_update_ptr(struct cs4281_state *s)
{
        int diff;
        unsigned hwptr, va, temp1;

        // update ADC pointer 
        if (s->ena & FMODE_READ) {
                hwptr = readl(s->pBA0+BA0_DCA1);          // Read capture DMA address.
                va = virt_to_bus(s->dma_adc.rawbuf);
                //trw added fix  hwptr -= (unsigned)s->dma_adc.rawbuf;
                hwptr -= (unsigned)va;                 
                diff = (s->dma_adc.dmasize + hwptr - s->dma_adc.hwptr) % s->dma_adc.dmasize;
                s->dma_adc.hwptr = hwptr;
                s->dma_adc.total_bytes += diff;
                s->dma_adc.count += diff;
                if (s->dma_adc.mapped) {
                        if (s->dma_adc.count >= (signed)s->dma_adc.fragsize)
                                wake_up(&s->dma_adc.wait);
                } else {
                        if (s->dma_adc.count > 0)
                                wake_up(&s->dma_adc.wait);
                }
        }
        // update DAC pointer 
	//
	// check for end of buffer, means that we are going to wait for another interrupt
	// to allow silence to fill the fifos on the part, to keep pops down to a minimum.
	//
        if ( (s->ena & FMODE_WRITE) && (!s->endofbuffer) )
	{
                hwptr = readl(s->pBA0+BA0_DCA0);          // Read play DMA address.
                va = virt_to_bus(s->dma_dac.rawbuf);
                hwptr -= (unsigned)va;
                diff = (s->dma_dac.dmasize + hwptr - s->dma_dac.hwptr) % s->dma_dac.dmasize;
                s->dma_dac.hwptr = hwptr;
                s->dma_dac.total_bytes += diff;
                if (s->dma_dac.mapped) {
                        s->dma_dac.count += diff;
                        if (s->dma_dac.count >= (signed)s->dma_dac.fragsize)
                                wake_up(&s->dma_dac.wait);
                } else {
                        s->dma_dac.count -= diff;
                        if (s->dma_dac.count <= 0) {
                                s->ena &= ~FMODE_WRITE;
                                temp1 = readl(s->pBA0+BA0_DCR0);
			//
			// fill with silence, and wait on turning off the DAC until interrupt routine.
			// wait on "Poke(pBA0+BA0_DCR0, temp1 | DCRn_MSK);    // Stop Play DMA"
			//
				memset(s->dma_dac.rawbuf, (s->fmt & (AFMT_U8 | AFMT_U16_LE)) ? 0x80 : 0, 
					s->dma_dac.dmasize); 
				s->endofbuffer = 1;
                        } else if (s->dma_dac.count <= (signed)s->dma_dac.fragsize
                                                         && !s->dma_dac.endcleared) {
                                clear_advance(s->dma_dac.rawbuf, 
                                              s->dma_dac.dmasize, s->dma_dac.swptr,
                                              s->dma_dac.fragsize,
                                              (s->fmt & (AFMT_U8 | AFMT_U16_LE)) ? 0x80 : 0);
                                s->dma_dac.endcleared = 1;
                        }
                        if (s->dma_dac.count < (signed)s->dma_dac.dmasize)
                                wake_up(&s->dma_dac.wait);
                }
        }
}


// --------------------------------------------------------------------- 

static void prog_codec(struct cs4281_state *s)
{
        unsigned long flags;
        unsigned temp1, format;

        spin_lock_irqsave(&s->lock, flags);
        temp1 = readl(s->pBA0+BA0_DCR0);
        writel(temp1 | DCRn_MSK, s->pBA0+BA0_DCR0);   // Stop play DMA, if active.
        temp1 = readl(s->pBA0+BA0_DCR1);
        writel(temp1 | DCRn_MSK, s->pBA0+BA0_DCR1);   // Stop capture DMA, if active.
 
        // program sampling rates  
        // Note, for CS4281, capture & play rates can be set independently.
        cs481_record_rate(s, s->rate); 
               
        // program ADC parameters 
        format = DMRn_DMA | DMRn_AUTO | DMRn_TR_WRITE;
        if(s->fmt & (AFMT_S16_LE | AFMT_U16_LE | AFMT_S16_BE | AFMT_U16_BE)) { // 16-bit
        if(s->fmt & (AFMT_S16_BE | AFMT_U16_BE))  // Big-endian?
                format |= DMRn_BEND;  
                if(s->fmt & (AFMT_U16_LE  | AFMT_U16_BE)) 
                        format |= DMRn_USIGN;         // Unsigned.      
        }          
        else
                format |= DMRn_SIZE8 | DMRn_USIGN;    // 8-bit, unsigned
        if(s->channels < 2)
                format |= DMRn_MONO;

        writel(format, s->pBA0+BA0_DMR1);       
       
  
        // program DAC parameters 
        format = DMRn_DMA | DMRn_AUTO | DMRn_TR_READ;
        if(s->fmt & (AFMT_S16_LE | AFMT_U16_LE | AFMT_S16_BE | AFMT_U16_BE)) { // 16-bit
                if(s->fmt & (AFMT_S16_BE | AFMT_U16_BE))  
                        format |= DMRn_BEND;          // Big Endian.
                if(s->fmt & (AFMT_U16_LE  | AFMT_U16_BE)) 
                        format |= DMRn_USIGN;         // Unsigned.      
        }          
        else
                format |= DMRn_SIZE8 | DMRn_USIGN;    // 8-bit, unsigned
        
        if(s->channels < 2)
                format |= DMRn_MONO;

        writel(format, s->pBA0+BA0_DMR0);       
        cs4281_play_rate(s, s->rate);

        s->ena = 0;     // Neither writing or reading.
        spin_unlock_irqrestore(&s->lock, flags);
}


// --------------------------------------------------------------------- 

static const char invalid_magic[] = KERN_CRIT "cs4281: invalid magic value\n";

#define VALIDATE_STATE(s)                         \
({                                                \
        if (!(s) || (s)->magic != CS4281_MAGIC) { \
                printk(invalid_magic);            \
                return -ENXIO;                    \
        }                                         \
})

// --------------------------------------------------------------------- 


static int mixer_ioctl(struct cs4281_state *s, unsigned int cmd, unsigned long arg)
{
	// Index to mixer_src[] is value of AC97 Input Mux Select Reg.
	// Value of array member is recording source Device ID Mask.
        static const unsigned int mixer_src[8] = {
                SOUND_MASK_MIC, SOUND_MASK_CD, 0, SOUND_MASK_LINE1,
                SOUND_MASK_LINE, SOUND_MASK_VOLUME, 0, 0
        };
             
        // Index of mixtable1[] member is Device ID 
        // and must be <= SOUND_MIXER_NRDEVICES.
        // Value of array member is index into s->mix.vol[]
        static const unsigned char mixtable1[SOUND_MIXER_NRDEVICES] = {
                [SOUND_MIXER_PCM]     = 1,   // voice 
                [SOUND_MIXER_LINE1]   = 2,   // AUX
                [SOUND_MIXER_CD]      = 3,   // CD 
                [SOUND_MIXER_LINE]    = 4,   // Line 
                [SOUND_MIXER_SYNTH]   = 5,   // FM
                [SOUND_MIXER_MIC]     = 6,   // Mic 
                [SOUND_MIXER_SPEAKER] = 7,   // Speaker 
                [SOUND_MIXER_RECLEV]  = 8,   // Recording level 
                [SOUND_MIXER_VOLUME]  = 9    // Master Volume 
        };
        
        
        static const unsigned mixreg[] = {
                BA0_AC97_PCM_OUT_VOLUME,
                BA0_AC97_AUX_VOLUME, 
                BA0_AC97_CD_VOLUME, 
                BA0_AC97_LINE_IN_VOLUME
        };
        unsigned char l, r, rl, rr, vidx;
        unsigned char attentbl[11] = {63,42,26,17,14,11,8,6,4,2,0};
        unsigned temp1;
        int i, val;

        VALIDATE_STATE(s);
 
        if (cmd == SOUND_MIXER_PRIVATE1) {
                // enable/disable/query mixer preamp 
                get_user_ret(val, (int *)arg, -EFAULT);
                if (val != -1) {
                        cs4281_read_ac97(s, BA0_AC97_MIC_VOLUME, &temp1);
                        temp1 = val ? (temp1 | 0x40) : (temp1 & 0xffbf);
                        cs4281_write_ac97(s, BA0_AC97_MIC_VOLUME, temp1);
                }
                cs4281_read_ac97(s, BA0_AC97_MIC_VOLUME, &temp1);
                val = (temp1 & 0x40) ? 1 : 0;
                return put_user(val, (int *)arg);
        }
        if (cmd == SOUND_MIXER_PRIVATE2) {
                // enable/disable/query spatializer 
                get_user_ret(val, (int *)arg, -EFAULT);
                if (val != -1) {
                        temp1 = (val & 0x3f) >> 2;
                        cs4281_write_ac97(s, BA0_AC97_3D_CONTROL, temp1);
                        cs4281_read_ac97(s, BA0_AC97_GENERAL_PURPOSE, &temp1);
                        cs4281_write_ac97(s, BA0_AC97_GENERAL_PURPOSE,temp1 | 0x2000);
                }
                cs4281_read_ac97(s, BA0_AC97_3D_CONTROL, &temp1);
                return put_user((temp1 << 2) | 3, (int *)arg);
        }
        if (cmd == SOUND_MIXER_INFO) {
                mixer_info info;
                strncpy(info.id, "CS4281", sizeof(info.id));
                strncpy(info.name, "Crystal CS4281", sizeof(info.name));
                info.modify_counter = s->mix.modcnt;
                if (copy_to_user((void *)arg, &info, sizeof(info)))
                        return -EFAULT;
                return 0;
        }
        if (cmd == SOUND_OLD_MIXER_INFO) {
                _old_mixer_info info;
                strncpy(info.id, "CS4281", sizeof(info.id));
                strncpy(info.name, "Crystal CS4281", sizeof(info.name));
                if (copy_to_user((void *)arg, &info, sizeof(info)))
                        return -EFAULT;
                return 0;
        }
        if (cmd == OSS_GETVERSION)
                return put_user(SOUND_VERSION, (int *)arg);
        
        if (_IOC_TYPE(cmd) != 'M' || _IOC_SIZE(cmd) != sizeof(int))
                return -EINVAL;
        
             // If ioctl has only the IOC_READ bit(bit 31)
             // on, process the only-read commands. 
        if (_IOC_DIR(cmd) == _IOC_READ) {
                switch (_IOC_NR(cmd)) {
                case SOUND_MIXER_RECSRC: // Arg contains a bit for each recording source 
                    cs4281_read_ac97(s, BA0_AC97_RECORD_SELECT, &temp1);
                    return put_user(mixer_src[temp1 & 7], (int *)arg);

                case SOUND_MIXER_DEVMASK: // Arg contains a bit for each supported device 
                        return put_user(SOUND_MASK_PCM | SOUND_MASK_SYNTH | SOUND_MASK_CD |
                                        SOUND_MASK_LINE | SOUND_MASK_LINE1 | SOUND_MASK_MIC |
                                        SOUND_MASK_VOLUME | SOUND_MASK_RECLEV |
                                        SOUND_MASK_SPEAKER, (int *)arg);

                case SOUND_MIXER_RECMASK: // Arg contains a bit for each supported recording source 
                        return put_user(SOUND_MASK_LINE | SOUND_MASK_MIC | SOUND_MASK_CD 
                                      | SOUND_MASK_VOLUME | SOUND_MASK_LINE1, (int *)arg);

                case SOUND_MIXER_STEREODEVS: // Mixer channels supporting stereo 
                        return put_user(SOUND_MASK_PCM | SOUND_MASK_SYNTH | SOUND_MASK_CD |
                                        SOUND_MASK_LINE | SOUND_MASK_LINE1 | SOUND_MASK_MIC |
                                        SOUND_MASK_VOLUME | SOUND_MASK_RECLEV, (int *)arg);

                case SOUND_MIXER_CAPS:
                        return put_user(SOUND_CAP_EXCL_INPUT, (int *)arg);

                default:
                        i = _IOC_NR(cmd);
                        if (i >= SOUND_MIXER_NRDEVICES || !(vidx = mixtable1[i]))
                                return -EINVAL;
                        return put_user(s->mix.vol[vidx-1], (int *)arg);
                }
        }
        
             // If ioctl doesn't have both the IOC_READ and 
             // the IOC_WRITE bit set, return invalid.
        if (_IOC_DIR(cmd) != (_IOC_READ|_IOC_WRITE))
                return -EINVAL;
        
             // Increment the count of volume writes.
        s->mix.modcnt++;
             
             // Isolate the command; it must be a write.
        switch (_IOC_NR(cmd)) {
        
        case SOUND_MIXER_RECSRC: // Arg contains a bit for each recording source 
                get_user_ret(val, (int *)arg, -EFAULT);
                i = hweight32(val);                 // i = # bits on in val.
                if (i != 1)                         // One & only 1 bit must be on.
                        return 0;
                for(i=0; i<sizeof(mixer_src)/sizeof(int); i++) {
                        if(val == mixer_src[i]) {
                                temp1 = (i << 8) | i;  
                                cs4281_write_ac97(s, BA0_AC97_RECORD_SELECT, temp1);
                                return 0;
                        }
                }
                return 0;

        case SOUND_MIXER_VOLUME:
                get_user_ret(val, (int *)arg, -EFAULT);
                l = val & 0xff;
                if(l > 100)
                        l = 100;                    // Max soundcard.h vol is 100.
                if(l < 6) {
                        rl = 63;
                        l  = 0;
                }
                else 
                        rl = attentbl[(10*l)/100];  // Convert 0-100 vol to 63-0 atten.
                        
                        r = (val >> 8) & 0xff;
                        if (r > 100)
                                r = 100;            // Max right volume is 100, too
                        if(r < 6) {
                                rr = 63;
                                r  = 0;
                        }
                        else 
                               rr = attentbl[(10*r)/100];   // Convert volume to attenuation.
                
                if ((rl > 60 ) && (rr > 60))        // If both l & r are 'low',          
                        temp1 = 0x8000;             //  turn on the mute bit.
                else
                        temp1 = 0;
                
                temp1 |= (rl << 8) | rr;
                
                cs4281_write_ac97(s, BA0_AC97_MASTER_VOLUME, temp1);
                cs4281_write_ac97(s, BA0_AC97_HEADPHONE_VOLUME, temp1);

#ifdef OSS_DOCUMENTED_MIXER_SEMANTICS
                s->mix.vol[8] = ((unsigned int)r << 8) | l;
#else
                s->mix.vol[8] = val;
#endif
                return put_user(s->mix.vol[8], (int *)arg);

        case SOUND_MIXER_SPEAKER:
                get_user_ret(val, (int *)arg, -EFAULT);
                l = val & 0xff;
                if (l > 100)
                        l = 100;
                if(l < 3 ) {
                        rl = 0;
                        l = 0;
                }
                else {
                        rl = (l*2 - 5)/13;          // Convert 0-100 range to 0-15.
                        l = (rl*13 +5)/2;
                }

                if (rl < 3){
                       temp1 = 0x8000;
                       rl     = 0;
                }
                else
                       temp1 = 0;
                rl = 15 - rl;                       // Convert volume to attenuation.
                temp1 |= rl << 1;
                cs4281_write_ac97(s, BA0_AC97_PC_BEEP_VOLUME, temp1);

#ifdef OSS_DOCUMENTED_MIXER_SEMANTICS
                s->mix.vol[6] = l << 8;
#else
                s->mix.vol[6] = val;
#endif
                return put_user(s->mix.vol[6], (int *)arg);

        case SOUND_MIXER_RECLEV:
                get_user_ret(val, (int *)arg, -EFAULT);
                l = val & 0xff;
                if (l > 100)
                        l = 100;
                r = (val >> 8) & 0xff;
                if (r > 100)
                        r = 100;
                rl = (l*2 - 5) / 13;                // Convert 0-100 scale to 0-15.
                rr = (r*2 - 5) / 13;
                if (rl <3 && rr <3)
                        temp1 = 0x8000;
                else
                        temp1 = 0;

                temp1 = temp1 | (rl << 8) | rr;
                cs4281_write_ac97(s, BA0_AC97_RECORD_GAIN, temp1); 

#ifdef OSS_DOCUMENTED_MIXER_SEMANTICS
                s->mix.vol[7] = ((unsigned int)r << 8) | l;
#else
                s->mix.vol[7] = val;
#endif
                return put_user(s->mix.vol[7], (int *)arg);

        case SOUND_MIXER_MIC:
                get_user_ret(val, (int *)arg, -EFAULT);
                l = val & 0xff;
                if (l > 100)
                        l = 100;
                if (l < 1) {
                        l = 0;
                        rl = 0;
                }
                else {
                        rl = ((unsigned)l*5 - 4)/16; // Convert 0-100 range to 0-31.
                        l  = (rl*16 +4)/5;
                }
                cs4281_read_ac97(s, BA0_AC97_MIC_VOLUME, &temp1);
                temp1 &= 0x40;                      // Isolate 20db gain bit.
                if (rl < 3){
                       temp1 |= 0x8000;
                       rl     = 0;
                }
                rl = 31 - rl;                       // Convert volume to attenuation.
                temp1 |= rl; 
                cs4281_write_ac97(s, BA0_AC97_MIC_VOLUME, temp1);

#ifdef OSS_DOCUMENTED_MIXER_SEMANTICS
                s->mix.vol[5] = val << 8;
#else
                s->mix.vol[5] = val;
#endif
                return put_user(s->mix.vol[5], (int *)arg);
        
        
        case SOUND_MIXER_SYNTH:
                get_user_ret(val, (int *)arg, -EFAULT);
                l = val & 0xff;
                if (l > 100)
                        l = 100;
                get_user_ret(val, (int *)arg, -EFAULT);
                r = (val >> 8) & 0xff;
                if (r > 100)
                        r = 100;
                rl = (l * 2 - 11)/3;        // Convert 0-100 range to 0-63.
                rr = (r * 2 - 11)/3;
                if (rl < 3)                 // If l is low, turn on
                        temp1 = 0x0080;     //  the mute bit.
                else
                        temp1 = 0;

                rl = 63 - rl;               // Convert vol to attenuation.
                writel(temp1|rl, s->pBA0+BA0_FMLVC);
                if (rr < 3)                 //  If rr is low, turn on
                        temp1 = 0x0080;     //   the mute bit.
                else
                        temp1 = 0;
                rr = 63 - rr;               // Convert vol to attenuation.
                writel(temp1 | rr, s->pBA0+BA0_FMRVC);

#ifdef OSS_DOCUMENTED_MIXER_SEMANTICS
                s->mix.vol[4] = (r << 8) | l;
#else
                s->mix.vol[4] = val;
#endif
                return put_user(s->mix.vol[4], (int *)arg);

                
        default:
                i = _IOC_NR(cmd);
                if (i >= SOUND_MIXER_NRDEVICES || !(vidx = mixtable1[i]))
                        return -EINVAL;
                get_user_ret(val, (int *)arg, -EFAULT);
                l = val & 0xff;
                if (l > 100)
                        l = 100;
                if (l < 1) {
                        l = 0;
                        rl = 31;
                }
                else 
                        rl = (attentbl[(l*10)/100])>>1;
                
                r = (val >> 8) & 0xff;
                if (r > 100)
                        r = 100;
                if (r < 1) {
                        r = 0;
                        rr = 31;
                }
                else 
                        rr = (attentbl[(r*10)/100])>>1;                        
                if ((rl > 30) && (rr > 30))
                        temp1 = 0x8000;
                else
                        temp1 = 0;
                temp1 = temp1 | (rl << 8) | rr;              
                cs4281_write_ac97(s, mixreg[vidx-1], temp1);
                
#ifdef OSS_DOCUMENTED_MIXER_SEMANTICS
                s->mix.vol[vidx-1] = ((unsigned int)r << 8) | l;
#else
                s->mix.vol[vidx-1] = val;
#endif
                return put_user(s->mix.vol[vidx-1], (int *)arg);
        }
}


// --------------------------------------------------------------------- 

static loff_t cs4281_llseek(struct file *file, loff_t offset, int origin)
{
        return -ESPIPE;
}


// --------------------------------------------------------------------- 

static int cs4281_open_mixdev(struct inode *inode, struct file *file)
{
        int minor = MINOR(inode->i_rdev);
        struct cs4281_state *s = devs;

        while (s && s->dev_mixer != minor)
                s = s->next;
        if (!s)
                return -ENODEV;
        VALIDATE_STATE(s);
        file->private_data = s;
        MOD_INC_USE_COUNT;
        return 0;
}


static int cs4281_release_mixdev(struct inode *inode, struct file *file)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;

        VALIDATE_STATE(s);
        MOD_DEC_USE_COUNT;
        return 0;
}


static int cs4281_ioctl_mixdev(struct inode *inode, struct file *file,
                               unsigned int cmd, unsigned long arg)
{
        return mixer_ioctl((struct cs4281_state *)file->private_data, cmd, arg);
}


// ******************************************************************************************
//   Mixer file operations struct.
// ******************************************************************************************
static /*const*/ struct file_operations cs4281_mixer_fops = {
        &cs4281_llseek,
        NULL,  // read 
        NULL,  // write 
        NULL,  // readdir 
        NULL,  // poll 
        &cs4281_ioctl_mixdev,
        NULL,  // mmap 
        &cs4281_open_mixdev,
        NULL,  // flush 
        &cs4281_release_mixdev,
        NULL,  // fsync 
        NULL,  // fasync 
        NULL,  // check_media_change 
        NULL,  // revalidate 
        NULL,  // lock 
};


// --------------------------------------------------------------------- 

static int drain_dac(struct cs4281_state *s, int nonblock)
{
        DECLARE_WAITQUEUE(wait, current);
        unsigned long flags;
        int count;
        unsigned tmo;

        if (s->dma_dac.mapped)
                return 0;
        current->state = TASK_INTERRUPTIBLE;
        add_wait_queue(&s->dma_dac.wait, &wait);
        for (;;) {
                spin_lock_irqsave(&s->lock, flags);
                count = s->dma_dac.count;
                spin_unlock_irqrestore(&s->lock, flags);
                if (count <= 0)
                        break;
                if (signal_pending(current))
                        break;
                if (nonblock) {
                        remove_wait_queue(&s->dma_dac.wait, &wait);
                        current->state = TASK_RUNNING;
                        return -EBUSY;
                }
                tmo = 3 * HZ * (count + s->dma_dac.fragsize) / 2 / s->rate;
                if (s->fmt & (AFMT_S16_LE | AFMT_U16_LE))
                        tmo >>= 1;
                if (s->channels > 1)
                        tmo >>= 1;
                if (!schedule_timeout(tmo + 1))
                        printk(KERN_DEBUG "cs4281: dma timed out??\n");
        }
        remove_wait_queue(&s->dma_dac.wait, &wait);
        current->state = TASK_RUNNING;
        if (signal_pending(current))
                return -ERESTARTSYS;
        return 0;
}


// --------------------------------------------------------------------- 

static ssize_t cs4281_read(struct file *file, char *buffer, size_t count, loff_t *ppos)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        ssize_t ret;
        unsigned long flags;
        unsigned swptr;
        int cnt;

        VALIDATE_STATE(s);
        if (ppos != &file->f_pos)
                return -ESPIPE;
        if (s->dma_adc.mapped)
                return -ENXIO;
        if (!s->dma_adc.ready && (ret = prog_dmabuf_adc(s)))
                return ret;
        if (!access_ok(VERIFY_WRITE, buffer, count))
                return -EFAULT;
        ret = 0;
        while (count > 0) {
                spin_lock_irqsave(&s->lock, flags);
                swptr = s->dma_adc.swptr;
                cnt = s->dma_adc.dmasize-swptr;
                if (s->dma_adc.count < cnt)
                        cnt = s->dma_adc.count;
                spin_unlock_irqrestore(&s->lock, flags);
                if (cnt > count)
                        cnt = count;
                if (cnt <= 0) {
                        start_adc(s);
                         if (file->f_flags & O_NONBLOCK)
                                return ret ? ret : -EAGAIN;
                        interruptible_sleep_on(&s->dma_adc.wait);
                        if (signal_pending(current))
                                return ret ? ret : -ERESTARTSYS;
                        continue;
                }
                if (copy_to_user(buffer, s->dma_adc.rawbuf + swptr, cnt))
                        return ret ? ret : -EFAULT;
                swptr = (swptr + cnt) % s->dma_adc.dmasize;
                spin_lock_irqsave(&s->lock, flags);
                s->dma_adc.swptr = swptr;
                s->dma_adc.count -= cnt;
                spin_unlock_irqrestore(&s->lock, flags);
                count -= cnt;
                buffer += cnt;
                ret += cnt;
                start_adc(s);
        }
        return ret;
}


static ssize_t cs4281_write(struct file *file, const char *buffer, size_t count, loff_t *ppos)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        ssize_t ret;
        unsigned long flags;
        unsigned swptr;
        int cnt;

        VALIDATE_STATE(s);

        if (ppos != &file->f_pos)
                return -ESPIPE;
        if (s->dma_dac.mapped)
                return -ENXIO;
        if (!s->dma_dac.ready && (ret = prog_dmabuf_dac(s)))
                return ret;
        if (!access_ok(VERIFY_READ, buffer, count))
                return -EFAULT;
        ret = 0;
        while (count > 0) {
                spin_lock_irqsave(&s->lock, flags);
                if (s->dma_dac.count < 0) {
                        s->dma_dac.count = 0;
                        s->dma_dac.swptr = s->dma_dac.hwptr;
                }
                swptr = s->dma_dac.swptr;
                cnt = s->dma_dac.dmasize-swptr;
                if (s->dma_dac.count + cnt > s->dma_dac.dmasize)
                        cnt = s->dma_dac.dmasize - s->dma_dac.count;
                spin_unlock_irqrestore(&s->lock, flags);
                if (cnt > count)
                        cnt = count;
                if (cnt <= 0) {
                        start_dac(s);
                        if (file->f_flags & O_NONBLOCK)
                                return ret ? ret : -EAGAIN;
                        interruptible_sleep_on(&s->dma_dac.wait);
                        if (signal_pending(current))
                                return ret ? ret : -ERESTARTSYS;
                        continue;
                }
                if (copy_from_user(s->dma_dac.rawbuf + swptr, buffer, cnt))
                        return ret ? ret : -EFAULT;
                swptr = (swptr + cnt) % s->dma_dac.dmasize;
                spin_lock_irqsave(&s->lock, flags);
                s->dma_dac.swptr = swptr;
                s->dma_dac.count += cnt;
                s->dma_dac.endcleared = 0;
                spin_unlock_irqrestore(&s->lock, flags);
                count -= cnt;
                buffer += cnt;
                ret += cnt;
                start_dac(s);
        }
        return ret;
}


static unsigned int cs4281_poll(struct file *file, struct poll_table_struct *wait)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        unsigned long flags;
        unsigned int mask = 0;

        VALIDATE_STATE(s);
        if (file->f_mode & FMODE_WRITE)
                poll_wait(file, &s->dma_dac.wait, wait);
        if (file->f_mode & FMODE_READ)
                poll_wait(file, &s->dma_adc.wait, wait);
        spin_lock_irqsave(&s->lock, flags);
        cs4281_update_ptr(s);
        if (file->f_mode & FMODE_READ) {
                if (s->dma_adc.mapped) {
                        if (s->dma_adc.count >= (signed)s->dma_adc.fragsize)
                                mask |= POLLIN | POLLRDNORM;
                } else {
                        if (s->dma_adc.count > 0)
                                mask |= POLLIN | POLLRDNORM;
                }
        }
        if (file->f_mode & FMODE_WRITE) {
                if (s->dma_dac.mapped) {
                        if (s->dma_dac.count >= (signed)s->dma_dac.fragsize)
                                mask |= POLLOUT | POLLWRNORM;
                } else {
                        if ((signed)s->dma_dac.dmasize > s->dma_dac.count)
                                mask |= POLLOUT | POLLWRNORM;
                }
        }
        spin_unlock_irqrestore(&s->lock, flags);
        return mask;
}


static int cs4281_mmap(struct file *file, struct vm_area_struct *vma)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        struct dmabuf *db;
        int ret;
        unsigned long size;

        VALIDATE_STATE(s);
        if (vma->vm_flags & VM_WRITE) {
                if ((ret = prog_dmabuf_dac(s)) != 0)
                        return ret;
                db = &s->dma_dac;
        } else if (vma->vm_flags & VM_READ) {
                if ((ret = prog_dmabuf_adc(s)) != 0)
                        return ret;
                db = &s->dma_adc;
        } else
                return -EINVAL;
        if (vma->vm_offset != 0)
                return -EINVAL;
        size = vma->vm_end - vma->vm_start;
        if (size > (PAGE_SIZE << db->buforder))
                return -EINVAL;
        if (remap_page_range(vma->vm_start, virt_to_phys(db->rawbuf), size, vma->vm_page_prot))
                return -EAGAIN;
        db->mapped = 1;
        return 0;
}


static int cs4281_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        unsigned long flags;
        audio_buf_info abinfo;
        count_info cinfo;
        int val, mapped, ret;
  
        VALIDATE_STATE(s);
        mapped = ((file->f_mode & FMODE_WRITE) && s->dma_dac.mapped) ||
                ((file->f_mode & FMODE_READ) && s->dma_adc.mapped);
        switch (cmd) {
        case OSS_GETVERSION:
                return put_user(SOUND_VERSION, (int *)arg);

        case SNDCTL_DSP_SYNC:
                if (file->f_mode & FMODE_WRITE)
                        return drain_dac(s, 0/*file->f_flags & O_NONBLOCK*/);
                return 0;

        case SNDCTL_DSP_SETDUPLEX:
                return 0;

        case SNDCTL_DSP_GETCAPS:
                return put_user(DSP_CAP_DUPLEX | DSP_CAP_REALTIME | DSP_CAP_TRIGGER | DSP_CAP_MMAP, (int *)arg);

        case SNDCTL_DSP_RESET:
                if (file->f_mode & FMODE_WRITE) {
                        stop_dac(s);
                        synchronize_irq();
                        s->dma_dac.swptr = s->dma_dac.hwptr = s->dma_dac.count = s->dma_dac.total_bytes = 0;
                }
                if (file->f_mode & FMODE_READ) {
                        stop_adc(s);
                        synchronize_irq();
                        s->dma_adc.swptr = s->dma_adc.hwptr = s->dma_adc.count = s->dma_adc.total_bytes = 0;
                }
                prog_codec(s);
                return 0;

        case SNDCTL_DSP_SPEED:
                get_user_ret(val, (int *)arg, -EFAULT);
                if (val >= 0) {
                        stop_adc(s);
                        stop_dac(s);
                        s->dma_adc.ready = s->dma_dac.ready = 0;
                        // program sampling rates 
                        if (val > 48000)
                                val = 48000;
                        if (val < 6300)
                                val = 6300;
                        s->rate = val;
                        prog_codec(s);
                }
                return put_user(s->rate, (int *)arg);

        case SNDCTL_DSP_STEREO:
                get_user_ret(val, (int *)arg, -EFAULT);
                stop_adc(s);
                stop_dac(s);
                s->dma_adc.ready = s->dma_dac.ready = 0;
                // program channels 
                s->channels = val ? 2 : 1;
                prog_codec(s);
                return 0;

        case SNDCTL_DSP_CHANNELS:
                get_user_ret(val, (int *)arg, -EFAULT);
                if (val != 0) {
                        stop_adc(s);
                        stop_dac(s);
                        s->dma_adc.ready = s->dma_dac.ready = 0;
                        // program channels 
                        s->channels = val ? 2 : 1;
                        prog_codec(s);
                }
                return put_user(s->channels, (int *)arg);

        case SNDCTL_DSP_GETFMTS: // Returns a mask 
                return put_user(AFMT_S16_LE|AFMT_U16_LE|AFMT_S8|AFMT_U8, (int *)arg);

        case SNDCTL_DSP_SETFMT: // Selects ONE fmt
                get_user_ret(val, (int *)arg, -EFAULT);
                if (val != AFMT_QUERY) {
                        stop_adc(s);
                        stop_dac(s);
                        s->dma_adc.ready = s->dma_dac.ready = 0;
                        // program format 
                        if (val != AFMT_S16_LE && val != AFMT_U16_LE &&
                            val != AFMT_S8 && val != AFMT_U8)
                                val = AFMT_U8;
                        s->fmt = val;
                        prog_codec(s);
                }
                return put_user(s->fmt, (int *)arg);

        case SNDCTL_DSP_POST:
                return 0;

        case SNDCTL_DSP_GETTRIGGER:
                val = 0;
                if (file->f_mode & s->ena & FMODE_READ)
                        val |= PCM_ENABLE_INPUT;
                if (file->f_mode & s->ena & FMODE_WRITE)
                        val |= PCM_ENABLE_OUTPUT;
                return put_user(val, (int *)arg);

        case SNDCTL_DSP_SETTRIGGER:
                get_user_ret(val, (int *)arg, -EFAULT);
                if (file->f_mode & FMODE_READ) {
                        if (val & PCM_ENABLE_INPUT) {
                                if (!s->dma_adc.ready && (ret = prog_dmabuf_adc(s)))
                                        return ret;
                                start_adc(s);
                        } else
                                stop_adc(s);
                }
                if (file->f_mode & FMODE_WRITE) {
                        if (val & PCM_ENABLE_OUTPUT) {
                                if (!s->dma_dac.ready && (ret = prog_dmabuf_dac(s)))
                                        return ret;
                                start_dac(s);
                        } else
                                stop_dac(s);
                }
                return 0;

        case SNDCTL_DSP_GETOSPACE:
                if (!(file->f_mode & FMODE_WRITE))
                        return -EINVAL;
                if (!(s->ena & FMODE_WRITE) && (val = prog_dmabuf_dac(s)) != 0)
                        return val;
                spin_lock_irqsave(&s->lock, flags);
                cs4281_update_ptr(s);
                abinfo.fragsize = s->dma_dac.fragsize;
                abinfo.bytes = s->dma_dac.dmasize - s->dma_dac.count;
                abinfo.fragstotal = s->dma_dac.numfrag;
                abinfo.fragments = abinfo.bytes >> s->dma_dac.fragshift;  
                spin_unlock_irqrestore(&s->lock, flags);
                return copy_to_user((void *)arg, &abinfo, sizeof(abinfo)) ? -EFAULT : 0;

        case SNDCTL_DSP_GETISPACE:
                if (!(file->f_mode & FMODE_READ))
                        return -EINVAL;
                if (!(s->ena & FMODE_READ) && (val = prog_dmabuf_adc(s)) != 0)
                        return val;
                spin_lock_irqsave(&s->lock, flags);
                cs4281_update_ptr(s);
                abinfo.fragsize = s->dma_adc.fragsize;
                abinfo.bytes = s->dma_adc.count;
                abinfo.fragstotal = s->dma_adc.numfrag;
                abinfo.fragments = abinfo.bytes >> s->dma_adc.fragshift;
                spin_unlock_irqrestore(&s->lock, flags);
                return copy_to_user((void *)arg, &abinfo, sizeof(abinfo)) ? -EFAULT : 0;

        case SNDCTL_DSP_NONBLOCK:
                file->f_flags |= O_NONBLOCK;
                return 0;

        case SNDCTL_DSP_GETODELAY:
                if (!(file->f_mode & FMODE_WRITE))
                        return -EINVAL;
                spin_lock_irqsave(&s->lock, flags);
                cs4281_update_ptr(s);
                val = s->dma_dac.count;
                spin_unlock_irqrestore(&s->lock, flags);
                return put_user(val, (int *)arg);

        case SNDCTL_DSP_GETIPTR:
                if (!(file->f_mode & FMODE_READ))
                        return -EINVAL;
                spin_lock_irqsave(&s->lock, flags);
                cs4281_update_ptr(s);
                cinfo.bytes = s->dma_adc.total_bytes;
                cinfo.blocks = s->dma_adc.count >> s->dma_adc.fragshift;
                cinfo.ptr = s->dma_adc.hwptr;
                if (s->dma_adc.mapped)
                        s->dma_adc.count &= s->dma_adc.fragsize-1;
                spin_unlock_irqrestore(&s->lock, flags);
                return copy_to_user((void *)arg, &cinfo, sizeof(cinfo));

        case SNDCTL_DSP_GETOPTR:
                if (!(file->f_mode & FMODE_WRITE))
                        return -EINVAL;
                spin_lock_irqsave(&s->lock, flags);
                cs4281_update_ptr(s);
                cinfo.bytes = s->dma_dac.total_bytes;
                cinfo.blocks = s->dma_dac.count >> s->dma_dac.fragshift;
                cinfo.ptr = s->dma_dac.hwptr;
                if (s->dma_dac.mapped)
                        s->dma_dac.count &= s->dma_dac.fragsize-1;
                spin_unlock_irqrestore(&s->lock, flags);
                return copy_to_user((void *)arg, &cinfo, sizeof(cinfo));

        case SNDCTL_DSP_GETBLKSIZE:
                if (file->f_mode & FMODE_WRITE) {
                        if ((val = prog_dmabuf_dac(s)))
                                return val;
                        return put_user(s->dma_dac.fragsize, (int *)arg);
                }
                if ((val = prog_dmabuf_adc(s)))
                        return val;
                return put_user(s->dma_adc.fragsize, (int *)arg);

        case SNDCTL_DSP_SETFRAGMENT:
                get_user_ret(val, (int *)arg, -EFAULT);
                return 0;              // Say OK, but do nothing.

        case SNDCTL_DSP_SUBDIVIDE:
                if ((file->f_mode & FMODE_READ && s->dma_adc.subdivision) ||
                    (file->f_mode & FMODE_WRITE && s->dma_dac.subdivision))
                        return -EINVAL;
                get_user_ret(val, (int *)arg, -EFAULT);
                if (val != 1 && val != 2 && val != 4)
                        return -EINVAL;
                if (file->f_mode & FMODE_READ)
                        s->dma_adc.subdivision = val;
                if (file->f_mode & FMODE_WRITE)
                        s->dma_dac.subdivision = val;
                return 0;

        case SOUND_PCM_READ_RATE:
                return put_user(s->rate, (int *)arg);

        case SOUND_PCM_READ_CHANNELS:
                return put_user(s->channels, (int *)arg);

        case SOUND_PCM_READ_BITS:
                return put_user((s->fmt & (AFMT_S8|AFMT_U8)) ? 8 : 16, (int *)arg);

        case SOUND_PCM_WRITE_FILTER:
        case SNDCTL_DSP_SETSYNCRO:
        case SOUND_PCM_READ_FILTER:
                return -EINVAL;

        }
        return mixer_ioctl(s, cmd, arg);
}


static int cs4281_release(struct inode *inode, struct file *file)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;

        VALIDATE_STATE(s);

        if (file->f_mode & FMODE_WRITE)
                drain_dac(s, file->f_flags & O_NONBLOCK);
        down(&s->open_sem);
        if (file->f_mode & FMODE_WRITE) {
                stop_dac(s);
                dealloc_dmabuf(&s->dma_dac);
        }
        if (file->f_mode & FMODE_READ) {
                stop_adc(s);
                dealloc_dmabuf(&s->dma_adc);
        }
        s->open_mode &= ~(FMODE_READ | FMODE_WRITE);
        up(&s->open_sem);
        wake_up(&s->open_wait);
        MOD_DEC_USE_COUNT;
        return 0;
}

static int cs4281_open(struct inode *inode, struct file *file)
{
        int minor = MINOR(inode->i_rdev);
        struct cs4281_state *s = devs;

        while (s && ((s->dev_audio ^ minor) & ~0xf))
                s = s->next;
        if (!s)
                return -ENODEV;
        VALIDATE_STATE(s);
        file->private_data = s;
        
                // wait for device to become free 
        down(&s->open_sem);
        while (s->open_mode & (FMODE_READ | FMODE_WRITE)) {
                if (file->f_flags & O_NONBLOCK) {
                        up(&s->open_sem);
                        return -EBUSY;
                }
                up(&s->open_sem);
                interruptible_sleep_on(&s->open_wait);
                if (signal_pending(current))
                        return -ERESTARTSYS;
                down(&s->open_sem);
        }
        s->fmt = AFMT_U8;
        s->channels = 1;
        s->rate = 8000;
        s->clkdiv = 96 | 0x80;
        s->ena = 0;
	s->endofbuffer = 0;
        s->dma_adc.ossfragshift = s->dma_adc.ossmaxfrags = s->dma_adc.subdivision = 0;
        s->dma_dac.ossfragshift = s->dma_dac.ossmaxfrags = s->dma_dac.subdivision = 0;
        s->open_mode |= file->f_mode & (FMODE_READ | FMODE_WRITE);
        up(&s->open_sem);
        MOD_INC_USE_COUNT;

        if (prog_dmabuf_dac(s) || prog_dmabuf_adc(s)) {
                
             printk(KERN_ERR "cs4281: Program dmabufs failed.\n");
             cs4281_release(inode, file);
                
             return -ENOMEM;
        }
        prog_codec(s);
        return 0;
}


// ******************************************************************************************
//   Wave (audio) file operations struct.
// ******************************************************************************************
static /*const*/ struct file_operations cs4281_audio_fops = {
        &cs4281_llseek,
        &cs4281_read,
        &cs4281_write,
        NULL,  // readdir 
        &cs4281_poll,
        &cs4281_ioctl,
        &cs4281_mmap,
        &cs4281_open,
        NULL,  // flush 
        &cs4281_release,
        NULL,  // fsync 
        NULL,  // fasync 
        NULL,  // check_media_change 
        NULL,  // revalidate 
        NULL,  // lock 
};

// --------------------------------------------------------------------- 

// hold spinlock for the following! 
static void cs4281_handle_midi(struct cs4281_state *s)
{
        unsigned char ch;
        int wake;
        unsigned temp1;

        wake = 0;
        while (!(readl(s->pBA0+ BA0_MIDSR) & 0x80)) {
                ch = readl(s->pBA0+BA0_MIDRP);
                if (s->midi.icnt < MIDIINBUF) {
                        s->midi.ibuf[s->midi.iwr] = ch;
                        s->midi.iwr = (s->midi.iwr + 1) % MIDIINBUF;
                        s->midi.icnt++;
                }
                wake = 1;
        }
        if (wake)
                wake_up(&s->midi.iwait);
        wake = 0;
        while (!(readl(s->pBA0+ BA0_MIDSR) & 0x40) && s->midi.ocnt > 0) {
                temp1 = ( s->midi.obuf[s->midi.ord] ) & 0x000000ff;
                writel(temp1, s->pBA0+BA0_MIDWP);
                s->midi.ord = (s->midi.ord + 1) % MIDIOUTBUF;
                s->midi.ocnt--;
                if (s->midi.ocnt < MIDIOUTBUF-16)
                        wake = 1;
        }
        if (wake)
                wake_up(&s->midi.owait);
}



static void cs4281_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
        struct cs4281_state *s = (struct cs4281_state *)dev_id;
        unsigned int temp1;

        // fastpath out, to ease interrupt sharing 
        temp1 = readl(s->pBA0+BA0_HISR);                          // Get Int Status reg.
        if (!(temp1 & (HISR_DMA0 | HISR_DMA1 | HISR_MIDI))) {     // If not DMA or MIDI int,
                writel(HICR_IEV| HICR_CHGM, s->pBA0+BA0_HICR);    //  reenable interrupts
                return;                                           //   and return.
        }
        
        if(temp1 & HISR_DMA0)                      // If play interrupt,
                readl(s->pBA0+BA0_HDSR0);              //   clear the source.

        if(temp1 & HISR_DMA1)                      // Same for play.
                readl(s->pBA0+BA0_HDSR1);        
        writel(HICR_IEV| HICR_CHGM, s->pBA0+BA0_HICR);  // Local EOI
        
        spin_lock(&s->lock);
	//
	// ok, at this point we assume that the fifos have been filled
	// with silence and so we now turn off the DMA engine.
	// if FMODE_WRITE is set that means that some thread
	// attempted to start_dac, which probably means that an open
	// occurred, so do not stop the dac in this case.
	//
	if(s->endofbuffer && !(s->ena & FMODE_WRITE))
	{
		writel(temp1|DCRn_MSK, s->pBA0+BA0_DCR0);    // Stop Play DMA
		s->endofbuffer = 0;
	}
	else
	{
        	cs4281_update_ptr(s);
	}
        cs4281_handle_midi(s);
        spin_unlock(&s->lock);
}

// **************************************************************************

static void cs4281_midi_timer(unsigned long data)
{
        struct cs4281_state *s = (struct cs4281_state *)data;
        unsigned long flags;

        spin_lock_irqsave(&s->lock, flags);
        cs4281_handle_midi(s);
        spin_unlock_irqrestore(&s->lock, flags);
        s->midi.timer.expires = jiffies+1;
        add_timer(&s->midi.timer);
}


// --------------------------------------------------------------------- 

static ssize_t cs4281_midi_read(struct file *file, char *buffer, size_t count, loff_t *ppos)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        ssize_t ret;
        unsigned long flags;
        unsigned ptr;
        int cnt;

        VALIDATE_STATE(s);
        if (ppos != &file->f_pos)
                return -ESPIPE;
        if (!access_ok(VERIFY_WRITE, buffer, count))
                return -EFAULT;
        ret = 0;
        while (count > 0) {
                spin_lock_irqsave(&s->lock, flags);
                ptr = s->midi.ird;
                cnt = MIDIINBUF - ptr;
                if (s->midi.icnt < cnt)
                        cnt = s->midi.icnt;
                spin_unlock_irqrestore(&s->lock, flags);
                if (cnt > count)
                        cnt = count;
                if (cnt <= 0) {
                        if (file->f_flags & O_NONBLOCK)
                                return ret ? ret : -EAGAIN;
                        interruptible_sleep_on(&s->midi.iwait);
                        if (signal_pending(current))
                                return ret ? ret : -ERESTARTSYS;
                        continue;
                }
                if (copy_to_user(buffer, s->midi.ibuf + ptr, cnt))
                        return ret ? ret : -EFAULT;
                ptr = (ptr + cnt) % MIDIINBUF;
                spin_lock_irqsave(&s->lock, flags);
                s->midi.ird = ptr;
                s->midi.icnt -= cnt;
                spin_unlock_irqrestore(&s->lock, flags);
                count -= cnt;
                buffer += cnt;
                ret += cnt;
        }
        return ret;
}


static ssize_t cs4281_midi_write(struct file *file, const char *buffer, size_t count, loff_t *ppos)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        ssize_t ret;
        unsigned long flags;
        unsigned ptr;
        int cnt;

        VALIDATE_STATE(s);
        if (ppos != &file->f_pos)
                return -ESPIPE;
        if (!access_ok(VERIFY_READ, buffer, count))
                return -EFAULT;
        ret = 0;
        while (count > 0) {
                spin_lock_irqsave(&s->lock, flags);
                ptr = s->midi.owr;
                cnt = MIDIOUTBUF - ptr;
                if (s->midi.ocnt + cnt > MIDIOUTBUF)
                        cnt = MIDIOUTBUF - s->midi.ocnt;
                if (cnt <= 0)
                        cs4281_handle_midi(s);
                spin_unlock_irqrestore(&s->lock, flags);
                if (cnt > count)
                        cnt = count;
                if (cnt <= 0) {
                        if (file->f_flags & O_NONBLOCK)
                                return ret ? ret : -EAGAIN;
                        interruptible_sleep_on(&s->midi.owait);
                        if (signal_pending(current))
                                return ret ? ret : -ERESTARTSYS;
                        continue;
                }
                if (copy_from_user(s->midi.obuf + ptr, buffer, cnt))
                        return ret ? ret : -EFAULT;
                ptr = (ptr + cnt) % MIDIOUTBUF;
                spin_lock_irqsave(&s->lock, flags);
                s->midi.owr = ptr;
                s->midi.ocnt += cnt;
                spin_unlock_irqrestore(&s->lock, flags);
                count -= cnt;
                buffer += cnt;
                ret += cnt;
                spin_lock_irqsave(&s->lock, flags);
                cs4281_handle_midi(s);
                spin_unlock_irqrestore(&s->lock, flags);
        }
        return ret;
}


static unsigned int cs4281_midi_poll(struct file *file, struct poll_table_struct *wait)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        unsigned long flags;
        unsigned int mask = 0;

        VALIDATE_STATE(s);
        if (file->f_flags & FMODE_WRITE)
                poll_wait(file, &s->midi.owait, wait);
        if (file->f_flags & FMODE_READ)
                poll_wait(file, &s->midi.iwait, wait);
        spin_lock_irqsave(&s->lock, flags);
        if (file->f_flags & FMODE_READ) {
                if (s->midi.icnt > 0)
                        mask |= POLLIN | POLLRDNORM;
        }
        if (file->f_flags & FMODE_WRITE) {
                if (s->midi.ocnt < MIDIOUTBUF)
                        mask |= POLLOUT | POLLWRNORM;
        }
        spin_unlock_irqrestore(&s->lock, flags);
        return mask;
}


static int cs4281_midi_open(struct inode *inode, struct file *file)
{
        int minor = MINOR(inode->i_rdev);
        struct cs4281_state *s = devs;
        unsigned long flags,temp1;
        while (s && s->dev_midi != minor)
                s = s->next;
        if (!s)
                return -ENODEV;
        VALIDATE_STATE(s);
        file->private_data = s;
        // wait for device to become free 
        down(&s->open_sem);
        while (s->open_mode & (file->f_mode << FMODE_MIDI_SHIFT)) {
                if (file->f_flags & O_NONBLOCK) {
                        up(&s->open_sem);
                        return -EBUSY;
                }
                up(&s->open_sem);
                interruptible_sleep_on(&s->open_wait);
                if (signal_pending(current))
                        return -ERESTARTSYS;
                down(&s->open_sem);
        }
        spin_lock_irqsave(&s->lock, flags);
        if (!(s->open_mode & (FMODE_MIDI_READ | FMODE_MIDI_WRITE))) {
                s->midi.ird = s->midi.iwr = s->midi.icnt = 0;
                s->midi.ord = s->midi.owr = s->midi.ocnt = 0;
                writel(1, s->pBA0+BA0_MIDCR);   // Reset the interface.
                writel(0, s->pBA0+BA0_MIDCR);   // Return to normal mode.
                s->midi.ird = s->midi.iwr = s->midi.icnt = 0;
                writel(0x0000000f, s->pBA0+BA0_MIDCR);                // Enable transmit, record, ints.
                temp1 = readl(s->pBA0+BA0_HIMR);
                writel(temp1 & 0xffbfffff, s->pBA0+BA0_HIMR);         // Enable midi int. recognition.
                writel(HICR_IEV | HICR_CHGM, s->pBA0+BA0_HICR);       // Enable interrupts
                init_timer(&s->midi.timer);
                s->midi.timer.expires = jiffies+1;
                s->midi.timer.data = (unsigned long)s;
                s->midi.timer.function = cs4281_midi_timer;
                add_timer(&s->midi.timer);
        }
        if (file->f_mode & FMODE_READ) {
                s->midi.ird = s->midi.iwr = s->midi.icnt = 0;
        }
        if (file->f_mode & FMODE_WRITE) {
                s->midi.ord = s->midi.owr = s->midi.ocnt = 0;
        }
        spin_unlock_irqrestore(&s->lock, flags);
        s->open_mode |= (file->f_mode << FMODE_MIDI_SHIFT) & (FMODE_MIDI_READ | FMODE_MIDI_WRITE);
        up(&s->open_sem);
        MOD_INC_USE_COUNT;
        return 0;
}


static int cs4281_midi_release(struct inode *inode, struct file *file)
{
        struct cs4281_state *s = (struct cs4281_state *)file->private_data;
        DECLARE_WAITQUEUE(wait, current);
        unsigned long flags;
        unsigned count, tmo;

        VALIDATE_STATE(s);

        if (file->f_mode & FMODE_WRITE) {
                current->state = TASK_INTERRUPTIBLE;
                add_wait_queue(&s->midi.owait, &wait);
                for (;;) {
                        spin_lock_irqsave(&s->lock, flags);
                        count = s->midi.ocnt;
                        spin_unlock_irqrestore(&s->lock, flags);
                        if (count <= 0)
                                break;
                        if (signal_pending(current))
                                break;
                        if (file->f_flags & O_NONBLOCK) {
                                remove_wait_queue(&s->midi.owait, &wait);
                                current->state = TASK_RUNNING;
                                return -EBUSY;
                        }
                        tmo = (count * HZ) / 3100;
                        if (!schedule_timeout(tmo ? : 1) && tmo)
                                printk(KERN_DEBUG "cs4281: midi timed out??\n");
                }
                remove_wait_queue(&s->midi.owait, &wait);
                current->state = TASK_RUNNING;
        }
        down(&s->open_sem);
        s->open_mode &= (~(file->f_mode << FMODE_MIDI_SHIFT)) & (FMODE_MIDI_READ|FMODE_MIDI_WRITE);
        spin_lock_irqsave(&s->lock, flags);
        if (!(s->open_mode & (FMODE_MIDI_READ | FMODE_MIDI_WRITE))) {
                writel(0, s->pBA0+BA0_MIDCR);    // Disable Midi interrupts.  
                del_timer(&s->midi.timer);
        }
        spin_unlock_irqrestore(&s->lock, flags);
        up(&s->open_sem);
        wake_up(&s->open_wait);
        MOD_DEC_USE_COUNT;
        return 0;
}

// ******************************************************************************************
//   Midi file operations struct.
// ******************************************************************************************
static /*const*/ struct file_operations cs4281_midi_fops = {
        &cs4281_llseek,
        &cs4281_midi_read,
        &cs4281_midi_write,
        NULL,  // readdir 
        &cs4281_midi_poll,
        NULL,  // ioctl 
        NULL,  // mmap 
        &cs4281_midi_open,
        NULL,  // flush 
        &cs4281_midi_release,
        NULL,  // fsync 
        NULL,  // fasync 
        NULL,  // check_media_change 
        NULL,  // revalidate 
        NULL,  // lock 
};


// --------------------------------------------------------------------- 

// maximum number of devices 
#define NR_DEVICE 8          // Only eight devices supported currently.

// --------------------------------------------------------------------- 

static struct initvol {
        int mixch;
        int vol;
} initvol[] __initdata = {
        { SOUND_MIXER_WRITE_VOLUME, 0x4040 },
        { SOUND_MIXER_WRITE_PCM, 0x4040 },
        { SOUND_MIXER_WRITE_SYNTH, 0x4040 },
        { SOUND_MIXER_WRITE_CD, 0x4040 },
        { SOUND_MIXER_WRITE_LINE, 0x4040 },
        { SOUND_MIXER_WRITE_LINE1, 0x4040 },
        { SOUND_MIXER_WRITE_RECLEV, 0x0000 },
        { SOUND_MIXER_WRITE_SPEAKER, 0x4040 },
        { SOUND_MIXER_WRITE_MIC, 0x0000 }
};


int __init cs4281_probe(void)
{
        struct cs4281_state *s;
        struct pci_dev *pcidev = NULL;
        mm_segment_t fs;
        int i, val, index = 0;
        unsigned int temp1, temp2;
 
        if (!pci_present())   // No PCI bus in this machine! 
                return -ENODEV;
        printk(KERN_INFO "cs4281: version 0.9 time " __TIME__ " " __DATE__ "\n");
        while (index < NR_DEVICE && 
        	(pcidev = pci_find_device(PCI_VENDOR_ID_CIRRUS, PCI_DEVICE_ID_CRYSTAL_CS4281, pcidev)))
        {   
                if (!(s = kmalloc(sizeof(struct cs4281_state), GFP_KERNEL))) {
                        printk(KERN_ERR "cs4281: no memory for state struct.\n");
                        continue;
                }

                memset(s, 0, sizeof(struct cs4281_state));
                init_waitqueue_head(&s->dma_adc.wait);
                init_waitqueue_head(&s->dma_dac.wait);
                init_waitqueue_head(&s->open_wait);
                init_waitqueue_head(&s->midi.iwait);
                init_waitqueue_head(&s->midi.owait);
                init_MUTEX(&s->open_sem);
                s->pBA0phys = pcidev->base_address[0]&PCI_BASE_ADDRESS_MEM_MASK;                  // Get physical addresses
                s->pBA1phys = pcidev->base_address[1]&PCI_BASE_ADDRESS_MEM_MASK;                  //  of part.
		s->pBA0 = ioremap_nocache(s->pBA0phys, 4096);		// Convert phys 
		s->pBA1 = ioremap_nocache(s->pBA1phys, 65536);  	//  to linear. 
                temp1 = readl(s->pBA0+ BA0_PCICFG00);
                temp2 = readl(s->pBA0+ BA0_PCICFG04);
                temp1 = cs4281_hw_init(s);
                if(temp1){
                        printk(KERN_INFO "cs4281: Hardware setup failed. Skipping part.\n");
                        continue;
                }	
                s->magic = CS4281_MAGIC;
		s->pcidev = pcidev;
                s->irq = pcidev->irq;
                if(request_irq(s->irq, cs4281_interrupt, SA_SHIRQ, "Crystal CS4281", s)){
                        printk(KERN_ERR "cs4281: irq %u in use\n", s->irq);
                        goto err_irq;
                }
                if ((s->dev_audio = register_sound_dsp(&cs4281_audio_fops, -1)) < 0)
                        goto err_dev1;
                if ((s->dev_mixer = register_sound_mixer(&cs4281_mixer_fops, -1)) < 0)
                        goto err_dev2;
                if ((s->dev_midi = register_sound_midi(&cs4281_midi_fops, -1)) < 0)
                        goto err_dev3;
                                    
                pci_set_master(pcidev);           // enable bus mastering 

                fs = get_fs();
                set_fs(KERNEL_DS);
                val = SOUND_MASK_LINE;
                mixer_ioctl(s, SOUND_MIXER_WRITE_RECSRC, (unsigned long)&val);
                for (i = 0; i < sizeof(initvol)/sizeof(initvol[0]); i++) {
                        val = initvol[i].vol;
                        mixer_ioctl(s, initvol[i].mixch, (unsigned long)&val);
                }
                val = 1; // enable mic preamp 
                mixer_ioctl(s, SOUND_MIXER_PRIVATE1, (unsigned long)&val);
                set_fs(fs);
                
                // queue it for later freeing 
                s->next = devs;
                devs = s;
                index++;
                continue;

        err_dev3:
                unregister_sound_mixer(s->dev_mixer);
        err_dev2:
                unregister_sound_dsp(s->dev_audio);
        err_dev1:
                printk(KERN_ERR "cs4281: cannot register dsp device\n");
                free_irq(s->irq, s);
        err_irq:
                kfree_s(s, sizeof(struct cs4281_state));
        } // endwhile
        if (!devs)
                return -ENODEV;
        return 0;
} // init_cs4281


// --------------------------------------------------------------------- 


#ifdef MODULE

MODULE_AUTHOR("gw boynton, wesb@crystal.cirrus.com");
MODULE_DESCRIPTION("Cirrus Logic CS4281 Driver");

int init_module(void)
{
	return cs4281_probe();
}

void cleanup_module(void)
{
        struct cs4281_state *s;
        while ((s = devs)) {
                devs = devs->next;
                // stop DMA controller 
                synchronize_irq();
                free_irq(s->irq, s);              
                unregister_sound_dsp(s->dev_audio);
                unregister_sound_mixer(s->dev_mixer);
                unregister_sound_midi(s->dev_midi);
                iounmap(s->pBA0);
                iounmap(s->pBA1);
                kfree_s(s, sizeof(struct cs4281_state));
        }
}

// --------------------------------------------------------------------- 

#endif
