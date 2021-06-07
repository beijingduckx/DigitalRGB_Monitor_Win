//
// firmware for Cypress EZ-USB FX2LP
// 8bit Synchronous Slave FIFO
//
// Original by 4-Apr-2021 by Minatsu (@tksm372)
//
// Modified for using CS2300-CP by BeijingDuck (@BeijingDuckX)
//
#include "Fx2.h"
#include "fx2regs.h"
#include "syncdly.h"

void Initialize() {
    // ----------------------------------------------------------------------
    // CPU Clock
    // ----------------------------------------------------------------------
    // bit7:6 -
    // bit5   1=PortC RD#/WR# Strobe enable
    // bit4:3 00=12MHz, 01=24MHz, 10=48MHz, 11=reserved
    // bit2   1=CLKOUT inverted
    // bit1   1=CLKOUT enable
    // bit0   1=reset
    CPUCS = 0x10; // 0b0001_0000; 48MHz
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // Interface Config
    // ----------------------------------------------------------------------
    // bit7   1=Internal clock, 0=External
    // bit6   1=48MHz, 0=30MHz
    // bit5   1=IFCLK out enable
    // bit4   1=IFCLK inverted
    // bit3   1=Async, 0=Sync
    // bit2   1=GPIF GSTATE out enable
    // bit1:0 00=Ports, 01=Reserved, 10=GPIF, 11=Slave FIFO
    IFCONFIG = 0x13; // 0b0000_0011; External clock, Sync, Slave FIFO
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // Chip Revision Control
    // ----------------------------------------------------------------------
    REVCTL = 0x03; // Recommended setting.
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // EP Config
    // ----------------------------------------------------------------------
    // bit7   1=Valid
    // bit6   1=IN, 0=OUT
    // bit5:4 00=Invalid, 01=Isochronous, 10=Bulk(default), 11=Interrupt
    // bit3   1=1024bytes buffer(EP2,6 only), 0=512bytes
    // bit2   -
    // bit1:0 00=Quad, 01=Invalid, 10=Double, 11=Triple
    EP1OUTCFG = 0xa0; //0b1010_0000  // Valid, Bulk
    SYNCDELAY;
    EP1INCFG = 0x7f; // disable
    SYNCDELAY;
    EP2CFG &= 0x7f; // disable
    SYNCDELAY;
    EP4CFG &= 0x7f; // disable
    SYNCDELAY;
    EP6CFG = 0xe0; // 0b1110_0000; Bulk-IN, 512bytes Quad buffer
    SYNCDELAY;
    EP8CFG &= 0x7f; // disable
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // FIFO Reset
    // ----------------------------------------------------------------------
    FIFORESET = 0x80; // NAK all transfer
    SYNCDELAY;
    FIFORESET = 0x86; // RESET EP6 FIFO
    SYNCDELAY;
    FIFORESET = 0x86; // Reset EP6 FIFO
    SYNCDELAY;
    FIFORESET = 0x00; // Resume
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // EP FIFO Config
    // ----------------------------------------------------------------------
    // bit7   -
    // bit6   1=IN Full Minus One
    // bit5   1=OUT Empty Minus One
    // bit4   1=AUTOOUT
    // bit3   1=AUTOIN
    // bit2   1=Zero length IN Packerts enable
    // bit1   -
    // bit0   1=16bit wide, 0=8bit wide
    EP6FIFOCFG = 0x0e; // 0b0000_1100; Auto-IN, 8bit
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // Auto IN Length
    // ----------------------------------------------------------------------
    EP6AUTOINLENH = (512 >> 8);
    SYNCDELAY;
    EP6AUTOINLENL = 0;
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // Start EP1
    // ----------------------------------------------------------------------
    EP1OUTBC = 0x1;   // Any value enables EP1 transfer
    SYNCDELAY;
}

#define CS2300_ADDR     (0x4F)   // AD0 terminal = H

void I2C_WaitStop(void)
{
    while(I2CS & bmSTOP);
}

int I2C_Write(BYTE cs, BYTE data)
{
    I2CS = cs;
    I2DAT = data;
    while((I2CS & bmDONE) == 0);

    return 0;
}

int I2C_SendStop()
{
    I2CS |= bmSTOP;

    return 0;
}

BYTE I2C_Read(void)
{
    while((I2CS & bmDONE) == 0);
    return I2DAT;
}

void SendPLL(BYTE reg_addr, BYTE data)
{
    I2C_WaitStop();
    I2C_Write(bmSTART, (BYTE)(CS2300_ADDR << 1));
    I2C_Write(0, reg_addr);
    I2C_Write(0, data);
    I2C_SendStop();
}

BYTE ReadPLL(BYTE reg_addr)
{
    volatile BYTE dummy;

    // Address
    I2C_WaitStop();
    I2C_Write(bmSTART, (BYTE)(CS2300_ADDR << 1));
    I2C_Write(0, reg_addr);
    I2C_SendStop();

    // Read 
    I2C_WaitStop();
    I2C_Write(bmSTART, (BYTE)(CS2300_ADDR << 1) | 1);
    if(!(I2CS & bmACK)){
        I2C_SendStop();
        return 0xff;
    }
    I2CS |= bmLASTRD;
    dummy = I2DAT;   // dummy read for the 1st transfer
    I2C_SendStop();
    return I2C_Read();
}

void InitPLL(void)
{
    //
    SendPLL(0x03, 0x01);  // EnDevCfg1
    SendPLL(0x05, 0x01);  // EnvDevCfg2
    SendPLL(0x16, 0x10);  // EnvDevCfg3

    SendPLL(0x03, 0x07);  // AuxOUT= Lock, EnDevCfg1
    SendPLL(0x16, 0x90);  // ClkSkipEn, EnvDevCfg3

    // Ratio
    SendPLL(0x06, 0x00);  //0x00700.000   for X1/turbo
    SendPLL(0x07, 0x70);
    SendPLL(0x08, 0x00);
    SendPLL(0x09, 0x00);

#if 0
    SendPLL(0x06, 0x00);  //0x00720.000   for Pasopia7
    SendPLL(0x07, 0x72);
    SendPLL(0x08, 0x00); 
    SendPLL(0x09, 0x00);
#endif
}

void WaitPllLock(void)
{
    while(ReadPLL(0x02) & 0x80);  // bit7 = Unlocked 
}

void ResetFifo(void)
{
    FIFORESET = 0x80; // NAK all transfer
    SYNCDELAY;
    EP6FIFOCFG = 0x00; // MANUAL mode
    SYNCDELAY;
    FIFORESET = 0x06; // Reset EP6 FIFO
    SYNCDELAY;
}

void ResumeFifo(void)
{
    EP6FIFOCFG = 0x0C; // MANUAL mode
    SYNCDELAY;
    FIFORESET = 0x00; // Resume
    SYNCDELAY;
}

void ProcessCommand(void)
{
    unsigned char *src = EP1OUTBUF;
    unsigned int len = ((int) EP1OUTBC);
    unsigned char command = *(src++);

    IOA = 0x03;

    switch(command){
        case 0x01:
            ResetFifo();
            SendPLL(0x06, *(src++));
            SendPLL(0x07, *(src++));
            SendPLL(0x08, *(src++));
            SendPLL(0x09, *(src++));
            WaitPllLock();
            ResumeFifo();
            break;
        case 0x02:
            ResetFifo();
            WaitPllLock();
            ResumeFifo();
            break;
    }

    IOA = 0x01;

    EP1OUTBC = 0x01;
    SYNCDELAY;
}

void main() {
    OEA = 0x03;
    IOA = 0x03;

    InitPLL();
    WaitPllLock();

    IOA = 0x01;

    Initialize();

    for (;;) {
        if(!(EP1OUTCS & 2)){
            ProcessCommand();
        }
    }
}
