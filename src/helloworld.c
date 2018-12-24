/******************************************************************************
*
* Copyright (C) 2009 - 2014 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

/*
 * helloworld.c: simple test application
 *
 * This application configures UART 16550 to baud rate 9600.
 * PS7 UART (Zynq) is not initialized by this application, since
 * bootrom/bsp configures it to baud rate 115200
 *
 * ------------------------------------------------
 * | UART TYPE   BAUD RATE                        |
 * ------------------------------------------------
 *   uartns550   9600
 *   uartlite    Configurable only in HW design
 *   ps7_uart    115200 (configured by bootrom/bsp)
 */

#include <stdio.h>
#include "platform.h"
#include "xil_printf.h"


// Timer includes
#include "xadcps.h"
#include "xil_types.h"
#include "Xscugic.h"
#include "Xil_exception.h"
#include "xscutimer.h"

// DMA includes
#include "xaxidma.h"
#include "xbasic_types.h"
#include "xparameters.h"
#include "xdebug.h"

// Cntl includes
#include "axilite_aes_cntl.h"

//timer info

#define TIMER_DEVICE_ID     XPAR_XSCUTIMER_0_DEVICE_ID
#define INTC_DEVICE_ID      XPAR_SCUGIC_SINGLE_DEVICE_ID
#define TIMER_IRPT_INTR     XPAR_SCUTIMER_INTR
// 5 secs (330MHz * 1000 * 1000 - 1)
#define TIMER_LOAD_VALUE 	0X633DE23F
static XScuGic Intc; //GIC
static XScuTimer Timer;//timer
static int must_finish = 0;
static void SetupInterruptSystem(XScuGic *GicInstancePtr,XScuTimer *TimerInstancePtr, u16 TimerIntrId);
static void TimerIntrHandler(void *CallBackRef);

// DMA informations

#if defined(XPAR_UARTNS550_0_BASEADDR)
#include "xuartns550_l.h"       /* to use uartns550 */
#endif

/******************** Constant Definitions **********************************/

/*
 * Device hardware build related constants.
 */

#define DMA_DEV_ID		XPAR_AXIDMA_0_DEVICE_ID

#ifdef XPAR_AXI_7SDDR_0_S_AXI_BASEADDR
#define DDR_BASE_ADDR		XPAR_AXI_7SDDR_0_S_AXI_BASEADDR
#elif XPAR_MIG7SERIES_0_BASEADDR
#define DDR_BASE_ADDR	XPAR_MIG7SERIES_0_BASEADDR
#elif XPAR_MIG_0_BASEADDR
#define DDR_BASE_ADDR	XPAR_MIG_0_BASEADDR
#elif XPAR_PSU_DDR_0_S_AXI_BASEADDR
#define DDR_BASE_ADDR	XPAR_PSU_DDR_0_S_AXI_BASEADDR
#endif

#ifndef DDR_BASE_ADDR
#warning CHECK FOR THE VALID DDR ADDRESS IN XPARAMETERS.H, \
		 DEFAULT SET TO 0x01000000
#define MEM_BASE_ADDR		0x01000000
#else
#define MEM_BASE_ADDR		(DDR_BASE_ADDR + 0x1000000)
#endif

#define TX_BUFFER_BASE		(MEM_BASE_ADDR + 0x00100000)
#define RX_BUFFER_BASE		(MEM_BASE_ADDR + 0x00300000)
#define RX_BUFFER_HIGH		(MEM_BASE_ADDR + 0x004FFFFF)

static int encrypt_data(int chunksize);

static int DoEncrypt(u8* in, u8* out, int chunksize);

/*
 * Device instance definitions
 */
XAxiDma AxiDma;

// AXI-Lite cntl infomations

#define AXI_CTL_BASEADDR XPAR_AXILITE_AES_CNTL_0_S00_AXI_BASEADDR

static int si = 1; /* SI by default */
static void value2human(int si, double bytes, double time, double* data, double* speed,char* metric);
static char *units[] = { "", "Ki", "Mi", "Gi", "Ti", 0};
static char *si_units[] = { "", "K", "M", "G", "T", 0};

int main()
{
    init_platform();

	// Timer schedules
	XScuTimer_Config *TMRConfigPtr;     //timer config
	TMRConfigPtr = XScuTimer_LookupConfig(TIMER_DEVICE_ID);
    XScuTimer_CfgInitialize(&Timer, TMRConfigPtr,TMRConfigPtr->BaseAddr);
    XScuTimer_SelfTest(&Timer);
    XScuTimer_LoadTimer(&Timer, TIMER_LOAD_VALUE);
//    XScuTimer_EnableAutoReload(&Timer);
//    XScuTimer_Start(&Timer);
    SetupInterruptSystem(&Intc,&Timer,TIMER_IRPT_INTR);

    // DMA Schedules
	XAxiDma_Config *CfgPtr;
	int Status;
//	int Tries = NUMBER_OF_TRANSFERS;
	int Index;
	u8 *TxBufferPtr;
	u8 *RxBufferPtr;

	TxBufferPtr = (u8 *)TX_BUFFER_BASE ;
	RxBufferPtr = (u8 *)RX_BUFFER_BASE;

	/* Initialize the XAxiDma device.
	 */
	CfgPtr = XAxiDma_LookupConfig(DMA_DEV_ID);
	if (!CfgPtr) {
		xil_printf("No config found for %d\r\n", DMA_DEV_ID);
		return XST_FAILURE;
	}

	Status = XAxiDma_CfgInitialize(&AxiDma, CfgPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("Initialization failed %d\r\n", Status);
		return XST_FAILURE;
	}

	if(XAxiDma_HasSg(&AxiDma)){
		xil_printf("Device configured as SG mode \r\n");
		return XST_FAILURE;
	}

	/* Disable interrupts, we use polling mode
	 */
	XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK,
						XAXIDMA_DEVICE_TO_DMA);
	XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK,
						XAXIDMA_DMA_TO_DEVICE);

	// Cntl IP core Schedules
	int write_loop_index;

	// Set Key
//	for (write_loop_index = 0 ; write_loop_index < 4; write_loop_index++)
//		  AXILITE_AES_CNTL_mWriteReg (AXI_CTL_BASEADDR, write_loop_index*4, 0xDCBA9876 << write_loop_index);
//	AXILITE_AES_CNTL_mWriteReg(AXI_CTL_BASEADDR, 16, 0x00000001);

	Xuint32 *baseaddr_p = (Xuint32 *)0x43C00000;
	*(baseaddr_p+0) = 0x0C0D0E0F;
	*(baseaddr_p+1) = 0x08090A0B;
	*(baseaddr_p+2) = 0x04050607;
	*(baseaddr_p+3) = 0x00010203;
	*(baseaddr_p+4) = 0x00000001;
	xil_printf("Write key to the AXI Lite Regs\n");

	// try different size of block for encryption speed
//	int i;
//	for(i = 16; i < (32 * 1024); i *= 2){
//		if(encrypt_data(i))
//			return 1;
//	}

	for(Index = 0; Index < 0x20; Index ++) {
		TxBufferPtr[Index] = 0;
	}
	DoEncrypt(TxBufferPtr, RxBufferPtr, 16);

	xil_printf("Returned value: \n");
	for(Index = 0x10 - 1; Index >= 0; Index--) {
		xil_printf("%02x", (unsigned int)RxBufferPtr[Index]);

	}
	xil_printf("\n\n");
#ifndef __aarch64__
	Xil_DCacheInvalidateRange((UINTPTR)RxBufferPtr, 0x10);
#endif
	*(baseaddr_p+4) = 0x00000000;

//	DoEncrypt(TxBufferPtr, RxBufferPtr, 32);
//	xil_printf("Returned value: \n");
//	for(Index = 0x20 - 1; Index >= 0; Index--) {
//		xil_printf("%02x", (unsigned int)RxBufferPtr[Index]);
//
//	}
//	xil_printf("\n\ndone!");
//    print("Hello World\n\r");
	// try different size of block for encryption speed
	int i;
	for(i = 16; i <= (64 * 1024); i *= 2){
		if(encrypt_data(i))
			return 1;
	}

    cleanup_platform();
    return 0;
}


void SetupInterruptSystem(XScuGic *GicInstancePtr,XScuTimer *TimerInstancePtr, u16 TimerIntrId)
{

        XScuGic_Config *IntcConfig; //GIC config
        Xil_ExceptionInit();
        //initialise the GIC
        IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
        XScuGic_CfgInitialize(GicInstancePtr, IntcConfig, IntcConfig->CpuBaseAddress);
        //connect to the hardware
        Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, GicInstancePtr);
        //set up the timer interrupt
        XScuGic_Connect(GicInstancePtr, TimerIntrId, (Xil_ExceptionHandler)TimerIntrHandler, (void *)TimerInstancePtr);
        //enable the interrupt for the Timer at GIC
        XScuGic_Enable(GicInstancePtr, TimerIntrId);
        //enable interrupt on the timer
        XScuTimer_EnableInterrupt(TimerInstancePtr);
        // Enable interrupts in the Processor.
        Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);

}

static void TimerIntrHandler(void *CallBackRef)
{
    XScuTimer *TimerInstancePtr = (XScuTimer *) CallBackRef;
    XScuTimer_ClearInterruptStatus(TimerInstancePtr);
    must_finish = 1;
//    printf("time is up, now must_finish=%d\n", must_finish);

}
static int encrypt_data(int chunksize){

	static int val = 71;
	double total = 0;
	double secs, ddata, dspeed;
	char metric[16];
	int start;
	u8 *TxBufferPtr;
	u8 *RxBufferPtr;

	TxBufferPtr = (u8 *)TX_BUFFER_BASE;
	RxBufferPtr = (u8 *)RX_BUFFER_BASE;

	printf("\tEncrypting in chunks of %d bytes: \n", chunksize);
	memset(TxBufferPtr, val++, chunksize);

	must_finish = 0;
	XScuTimer_Start(&Timer);

	start = XScuTimer_GetCounterValue(&Timer);
	do{
		if(DoEncrypt(TxBufferPtr, RxBufferPtr, chunksize))
			return 1;
		total += chunksize;
	}while(must_finish==0);

	secs = start / 333000000.0;

//	printf("total = %e",total);
//	xil_printf("Returned value: \n");
//	for(Index = chunksize - 1; Index >= 0; Index--) {
//		xil_printf("%02x", (unsigned int)RxBufferPtr[Index]);
//		if(Index%16==0)
//			printf("\n");
//	}

	value2human(si, total, secs, &ddata, &dspeed, metric);
	printf ("done. %.2f %s in %.2f secs: ", ddata, metric, secs);
	printf ("%.2f %s/sec\n", dspeed, metric);
	XScuTimer_RestartTimer(&Timer);

	return 0;
}

static int DoEncrypt(u8* in, u8* out, int chunksize){

	int Status = 0;

//	for(int i = 0; i < chunksize ; i += 16 ){
		Status = XAxiDma_SimpleTransfer(&AxiDma,(UINTPTR) out,
				chunksize, XAXIDMA_DEVICE_TO_DMA);

		if (Status != XST_SUCCESS) {
			return XST_FAILURE;
		}

		Status = XAxiDma_SimpleTransfer(&AxiDma,(UINTPTR) in,
				chunksize, XAXIDMA_DMA_TO_DEVICE);

		if (Status != XST_SUCCESS) {
			return XST_FAILURE;
		}

		while ((XAxiDma_Busy(&AxiDma,XAXIDMA_DEVICE_TO_DMA)) ||
				(XAxiDma_Busy(&AxiDma,XAXIDMA_DMA_TO_DEVICE))) {
				/* Wait */
//			printf("busy...");
		}
//		AXILITE_AES_CNTL_mWriteReg(AXI_CTL_BASEADDR, 16, 0x00000000);
#ifndef __aarch64__
	Xil_DCacheInvalidateRange((UINTPTR)out, chunksize);
#endif
//	}
	return 0;
}
static void value2human(int si, double bytes, double time, double* data, double* speed,char* metric)
{
	int unit = 0;

	*data = bytes;

	if (si) {
		while (*data > 1000 && si_units[unit + 1]) {
			*data /= 1000;
			unit++;
		}
		*speed = *data / time;
		sprintf(metric, "%sB", si_units[unit]);
	} else {
		while (*data > 1024 && units[unit + 1]) {
			*data /= 1024;
			unit++;
		}
		*speed = *data / time;
		sprintf(metric, "%sB", units[unit]);
	}
}
