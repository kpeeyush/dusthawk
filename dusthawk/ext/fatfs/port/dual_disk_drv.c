/*-----------------------------------------------------------------------*/
/*-----------------------------------------------------------------------*/
/* TivaWare USB MSC module                                               */
/*-----------------------------------------------------------------------*/
/*-----------------------------------------------------------------------*/

/*
 * This file was modified from a sample available from the FatFs
 * web site. It was modified to work with the TivaWare USB Library.
 */
#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_types.h"
#include "driverlib/sysctl.h"
#include "usblib/usblib.h"
#include "usblib/usbmsc.h"
#include "usblib/host/usbhost.h"
#include "usblib/host/usbhmsc.h"
#include "fatfs/src/diskio.h"
#include "fatfs/src/ff.h"
#include "driverlib/gpio.h"
#include "driverlib/ssi.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "utils/uartstdio.h"


/* Definitions for MMC/SDC command */
#define CMD0    (0x40+0)    /* GO_IDLE_STATE */
#define CMD1    (0x40+1)    /* SEND_OP_COND */
#define CMD8    (0x40+8)    /* SEND_IF_COND */
#define CMD9    (0x40+9)    /* SEND_CSD */
#define CMD10    (0x40+10)    /* SEND_CID */
#define CMD12    (0x40+12)    /* STOP_TRANSMISSION */
#define CMD16    (0x40+16)    /* SET_BLOCKLEN */
#define CMD17    (0x40+17)    /* READ_SINGLE_BLOCK */
#define CMD18    (0x40+18)    /* READ_MULTIPLE_BLOCK */
#define CMD23    (0x40+23)    /* SET_BLOCK_COUNT */
#define CMD24    (0x40+24)    /* WRITE_BLOCK */
#define CMD25    (0x40+25)    /* WRITE_MULTIPLE_BLOCK */
#define CMD41    (0x40+41)    /* SEND_OP_COND (ACMD) */
#define CMD55    (0x40+55)    /* APP_CMD */
#define CMD58    (0x40+58)    /* READ_OCR */

/* Peripheral definitions for DK-TM4C123G board */
// SSI port
#define SDC_SSI_BASE            SSI3_BASE
#define SDC_SSI_SYSCTL_PERIPH   SYSCTL_PERIPH_SSI3

// GPIO for SSI pins
#define SDC_GPIO_PORT_BASE      GPIO_PORTD_BASE
#define SDC_GPIO_SYSCTL_PERIPH  SYSCTL_PERIPH_GPIOD
#define SDC_SSI_CLK             GPIO_PIN_0
#define SDC_SSI_TX              GPIO_PIN_3
#define SDC_SSI_RX              GPIO_PIN_2
#define SDC_SSI_FSS             GPIO_PIN_1
#define SDC_SSI_PINS            (SDC_SSI_TX | SDC_SSI_RX | SDC_SSI_CLK | SDC_SSI_FSS)

extern tUSBHMSCInstance *g_psMSCInstance;
static volatile DSTATUS USBStat = STA_NOINIT;    /* Disk status */

/*-----------------------------------------------------------------------*/
/* SD Card Disk Drive                                                 */
/*-----------------------------------------------------------------------*/
static void SELECT (void)
{
    GPIOPinWrite(SDC_GPIO_PORT_BASE, SDC_SSI_FSS, 0);
}

// de-asserts the CS pin to the card
static void DESELECT (void)
{
    GPIOPinWrite(SDC_GPIO_PORT_BASE, SDC_SSI_FSS, SDC_SSI_FSS);
}

/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/

static volatile DSTATUS Stat = STA_NOINIT;    /* Disk status */

static volatile BYTE Timer1, Timer2;    	  /* 100Hz decrement timer */

static BYTE CardType;            			 /* b0:MMC, b1:SDC, b2:Block addressing */

static BYTE PowerFlag = 0;     				 /* indicates if "power" is on */

/*-----------------------------------------------------------------------*/
/* Transmit a byte to MMC via SPI  (Platform dependent)                  */
/*-----------------------------------------------------------------------*/

static
void xmit_spi(BYTE dat)
{
    uint32_t ui32RcvDat;

    ROM_SSIDataPut(SDC_SSI_BASE, dat); /* Write the data to the tx fifo */

    ROM_SSIDataGet(SDC_SSI_BASE, &ui32RcvDat); /* flush data read during the write */
}


/*-----------------------------------------------------------------------*/
/* Receive a byte from MMC via SPI  (Platform dependent)                 */
/*-----------------------------------------------------------------------*/

static
BYTE rcvr_spi (void)
{
    uint32_t ui32RcvDat;

    ROM_SSIDataPut(SDC_SSI_BASE, 0xFF); /* write dummy data */

    ROM_SSIDataGet(SDC_SSI_BASE, &ui32RcvDat); /* read data frm rx fifo */

    return (BYTE)ui32RcvDat;
}


static
void rcvr_spi_m (BYTE *dst)
{
    *dst = rcvr_spi();
}

/*-----------------------------------------------------------------------*/
/* Wait for card ready                                                   */
/*-----------------------------------------------------------------------*/

static
BYTE wait_ready (void)
{
    BYTE res;


    Timer2 = 50;    /* Wait for ready in timeout of 500ms */
    rcvr_spi();
    do
        res = rcvr_spi();
    while ((res != 0xFF) && Timer2);

    return res;
}

/*-----------------------------------------------------------------------*/
/* Send 80 or so clock transitions with CS and DI held high. This is     */
/* required after card power up to get it into SPI mode                  */
/*-----------------------------------------------------------------------*/
static
void send_initial_clock_train(void)
{
    unsigned int i;
    uint32_t ui32Dat;

    /* Ensure CS is held high. */
    DESELECT();

    /* Switch the SSI TX line to a GPIO and drive it high too. */
    ROM_GPIOPinTypeGPIOOutput(SDC_GPIO_PORT_BASE, SDC_SSI_TX);
    ROM_GPIOPinWrite(SDC_GPIO_PORT_BASE, SDC_SSI_TX, SDC_SSI_TX);

    /* Send 10 bytes over the SSI. This causes the clock to wiggle the */
    /* required number of times. */
    for(i = 0 ; i < 10 ; i++)
    {
        /* Write DUMMY data. SSIDataPut() waits until there is room in the */
        /* FIFO. */
        ROM_SSIDataPut(SDC_SSI_BASE, 0xFF);

        /* Flush data read during data write. */
        ROM_SSIDataGet(SDC_SSI_BASE, &ui32Dat);
    }

    /* Revert to hardware control of the SSI TX line. */
    ROM_GPIOPinTypeSSI(SDC_GPIO_PORT_BASE, SDC_SSI_TX);
}

/*-----------------------------------------------------------------------*/
/* Power Control  (Platform dependent)                                   */
/*-----------------------------------------------------------------------*/
/* When the target system does not support socket power control, there   */
/* is nothing to do in these functions and chk_power always returns 1.   */

static
void power_on (void)
{
    /*
     * This doesn't really turn the power on, but initializes the
     * SSI port and pins needed to talk to the card.
     */

    /* Enable the peripherals used to drive the SDC on SSI */
    ROM_SysCtlPeripheralEnable(SDC_SSI_SYSCTL_PERIPH);
    ROM_SysCtlPeripheralEnable(SDC_GPIO_SYSCTL_PERIPH);

    /*
     * Configure the appropriate pins to be SSI instead of GPIO. The FSS (CS)
     * signal is directly driven to ensure that we can hold it low through a
     * complete transaction with the SD card.
     */
    ROM_GPIOPinTypeSSI(SDC_GPIO_PORT_BASE, SDC_SSI_TX | SDC_SSI_RX | SDC_SSI_CLK);
    ROM_GPIOPinTypeGPIOOutput(SDC_GPIO_PORT_BASE, SDC_SSI_FSS);

    /*
     * Set the SSI output pins to 4MA drive strength and engage the
     * pull-up on the receive line.
     */
    MAP_GPIOPadConfigSet(SDC_GPIO_PORT_BASE, SDC_SSI_RX, GPIO_STRENGTH_4MA,
                         GPIO_PIN_TYPE_STD_WPU);
    MAP_GPIOPadConfigSet(SDC_GPIO_PORT_BASE, SDC_SSI_CLK | SDC_SSI_TX | SDC_SSI_FSS,
                         GPIO_STRENGTH_4MA, GPIO_PIN_TYPE_STD);

    /* Configure the SSI0 port */
    ROM_SSIConfigSetExpClk(SDC_SSI_BASE, ROM_SysCtlClockGet(),
                           SSI_FRF_MOTO_MODE_0, SSI_MODE_MASTER, 400000, 8);
    ROM_SSIEnable(SDC_SSI_BASE);

    /* Set DI and CS high and apply more than 74 pulses to SCLK for the card */
    /* to be able to accept a native command. */
    send_initial_clock_train();

    PowerFlag = 1;
}

// set the SSI speed to the max setting
static
void set_max_speed(void)
{
    unsigned long i;

    /* Disable the SSI */
    ROM_SSIDisable(SDC_SSI_BASE);

    /* Set the maximum speed as half the system clock, with a max of 12.5 MHz. */
    i = ROM_SysCtlClockGet() / 2;
    if(i > 12500000)
    {
        i = 12500000;
    }

    /* Configure the SSI0 port to run at 12.5MHz */
    ROM_SSIConfigSetExpClk(SDC_SSI_BASE, ROM_SysCtlClockGet(),SSI_FRF_MOTO_MODE_0, SSI_MODE_MASTER,4000000, 8);

    /* Enable the SSI */
    ROM_SSIEnable(SDC_SSI_BASE);
}

static
void power_off (void)
{
    PowerFlag = 0;
}

static
int chk_power(void)        /* Socket power state: 0=off, 1=on */
{
    return PowerFlag;
}



/*-----------------------------------------------------------------------*/
/* Receive a data packet from MMC                                        */
/*-----------------------------------------------------------------------*/

static
BOOL rcvr_datablock (
    BYTE *buff,            /* Data buffer to store received data */
    UINT btr            /* Byte count (must be even number) */
)
{
    BYTE token;


    Timer1 = 100;
    do {                            /* Wait for data packet in timeout of 100ms */
        token = rcvr_spi();
    } while ((token == 0xFF) && Timer1);
    if(token != 0xFE) return FALSE;    /* If not valid data token, retutn with error */

    do {                            /* Receive the data block into buffer */
        rcvr_spi_m(buff++);
        rcvr_spi_m(buff++);
    } while (btr -= 2);
    rcvr_spi();                        /* Discard CRC */
    rcvr_spi();

    return TRUE;                    /* Return with success */
}



/*-----------------------------------------------------------------------*/
/* Send a data packet to MMC                                             */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0
static
BOOL xmit_datablock (
    const BYTE *buff,    /* 512 byte data block to be transmitted */
    BYTE token            /* Data/Stop token */
)
{
    BYTE resp, wc;


    if (wait_ready() != 0xFF) return FALSE;

    xmit_spi(token);                    /* Xmit data token */
    if (token != 0xFD) {    /* Is data token */
        wc = 0;
        do {                            /* Xmit the 512 byte data block to MMC */
            xmit_spi(*buff++);
            xmit_spi(*buff++);
        } while (--wc);
        xmit_spi(0xFF);                    /* CRC (Dummy) */
        xmit_spi(0xFF);
        resp = rcvr_spi();                /* Reveive data response */
        if ((resp & 0x1F) != 0x05)        /* If not accepted, return with error */
            return FALSE;
    }

    return TRUE;
}
#endif /* _READONLY */



/*-----------------------------------------------------------------------*/
/* Send a command packet to MMC                                          */
/*-----------------------------------------------------------------------*/

static
BYTE send_cmd (
    BYTE cmd,        /* Command byte */
    DWORD arg        /* Argument */
)
{
    BYTE n, res;


    if (wait_ready() != 0xFF) return 0xFF;

    /* Send command packet */
    xmit_spi(cmd);                        /* Command */
    xmit_spi((BYTE)(arg >> 24));        /* Argument[31..24] */
    xmit_spi((BYTE)(arg >> 16));        /* Argument[23..16] */
    xmit_spi((BYTE)(arg >> 8));            /* Argument[15..8] */
    xmit_spi((BYTE)arg);                /* Argument[7..0] */
    n = 0xff;
    if (cmd == CMD0) n = 0x95;            /* CRC for CMD0(0) */
    if (cmd == CMD8) n = 0x87;            /* CRC for CMD8(0x1AA) */
    xmit_spi(n);

    /* Receive command response */
    if (cmd == CMD12) rcvr_spi();        /* Skip a stuff byte when stop reading */
    n = 10;                                /* Wait for a valid response in timeout of 10 attempts */
    do
        res = rcvr_spi();
    while ((res & 0x80) && --n);

    return res;            /* Return with the response value */
}

/*-----------------------------------------------------------------------*
 * Send the special command used to terminate a multi-sector read.
 *
 * This is the only command which can be sent while the SDCard is sending
 * data. The SDCard spec indicates that the data transfer will stop 2 bytes
 * after the 6 byte CMD12 command is sent and that the card will then send
 * 0xFF for between 2 and 6 more bytes before the R1 response byte.  This
 * response will be followed by another 0xFF byte.  In testing, however, it
 * seems that some cards don't send the 2 to 6 0xFF bytes between the end of
 * data transmission and the response code.  This function, therefore, merely
 * reads 10 bytes and, if the last one read is 0xFF, returns the value of the
 * latest non-0xFF byte as the response code.
 *
 *-----------------------------------------------------------------------*/

static
BYTE send_cmd12 (void)
{
    BYTE n, res, val;

    /* For CMD12, we don't wait for the card to be idle before we send
     * the new command.
     */

    /* Send command packet - the argument for CMD12 is ignored. */
    xmit_spi(CMD12);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);

    /* Read up to 10 bytes from the card, remembering the value read if it's
       not 0xFF */
    for(n = 0; n < 10; n++)
    {
        val = rcvr_spi();
        if(val != 0xFF)
        {
            res = val;
        }
    }

    return res;            /* Return with the response value */
}


////////////////////////////////////DRIVE 0/////////////////////////////////////////

/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/

DSTATUS disk0_initialize (
    BYTE drv        /* Physical drive nmuber (0) */
)
{
    BYTE n, ty, ocr[4];


    if (drv) return STA_NOINIT;            /* Supports only single drive */
    if (Stat & STA_NODISK) return Stat;    /* No card in the socket */

    power_on();                            /* Force socket power on */
    send_initial_clock_train();            /* Ensure the card is in SPI mode */

    SELECT();                /* CS = L */
    ty = 0;
    if (send_cmd(CMD0, 0) == 1) {            /* Enter Idle state */
        Timer1 = 100;                        /* Initialization timeout of 1000 msec */
        if (send_cmd(CMD8, 0x1AA) == 1) {    /* SDC Ver2+ */
            for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {    /* The card can work at vdd range of 2.7-3.6V */
                do {
                    if (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 1UL << 30) == 0)    break;    /* ACMD41 with HCS bit */
                } while (Timer1);
                if (Timer1 && send_cmd(CMD58, 0) == 0) {    /* Check CCS bit */
                    for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
                    ty = (ocr[0] & 0x40) ? 6 : 2;
                }
            }
        } else {                            /* SDC Ver1 or MMC */
            ty = (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) <= 1) ? 2 : 1;    /* SDC : MMC */
            do {
                if (ty == 2) {
                    if (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) == 0) break;    /* ACMD41 */
                } else {
                    if (send_cmd(CMD1, 0) == 0) break;                                /* CMD1 */
                }
            } while (Timer1);
            if (!Timer1 || send_cmd(CMD16, 512) != 0)    /* Select R/W block length */
                ty = 0;
        }
    }
    CardType = ty;
    DESELECT();            /* CS = H */
    rcvr_spi();            /* Idle (Release DO) */

    if (ty) {            /* Initialization succeded */
        Stat &= ~STA_NOINIT;        /* Clear STA_NOINIT */
        set_max_speed();
					UARTprintf("SDC INIT SUCCESS\n");
    } else {            /* Initialization failed */
				UARTprintf("SDC INIT FAILED\n");
        power_off();
    }

    return Stat;
}

/*-----------------------------------------------------------------------*/
/* Get Disk Status                                                       */
/*-----------------------------------------------------------------------*/

DSTATUS disk0_status (
    BYTE drv        /* Physical drive nmuber (0) */
)
{
    if (drv) return STA_NOINIT;        /* Supports only single drive */
    return Stat;
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk0_read (
    BYTE drv,            /* Physical drive nmuber (0) */
    BYTE *buff,            /* Pointer to the data buffer to store read data */
    DWORD sector,        /* Start sector number (LBA) */
    BYTE count            /* Sector count (1..255) */
)
{
    if (drv || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    if (!(CardType & 4)) sector *= 512;    /* Convert to byte address if needed */

    SELECT();            /* CS = L */

    if (count == 1) {    /* Single block read */
        if ((send_cmd(CMD17, sector) == 0)    /* READ_SINGLE_BLOCK */
            && rcvr_datablock(buff, 512))
            count = 0;
    }
    else {                /* Multiple block read */
        if (send_cmd(CMD18, sector) == 0) {    /* READ_MULTIPLE_BLOCK */
            do {
                if (!rcvr_datablock(buff, 512)) break;
                buff += 512;
            } while (--count);
            send_cmd12();                /* STOP_TRANSMISSION */
        }
    }

    DESELECT();            /* CS = H */
    rcvr_spi();            /* Idle (Release DO) */

    return count ? RES_ERROR : RES_OK;
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0
DRESULT disk0_write (
    BYTE drv,            /* Physical drive nmuber (0) */
    const BYTE *buff,    /* Pointer to the data to be written */
    DWORD sector,        /* Start sector number (LBA) */
    BYTE count            /* Sector count (1..255) */
)
{
    if (drv || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;

    if (!(CardType & 4)) sector *= 512;    /* Convert to byte address if needed */

    SELECT();            /* CS = L */

    if (count == 1) {    /* Single block write */
        if ((send_cmd(CMD24, sector) == 0)    /* WRITE_BLOCK */
            && xmit_datablock(buff, 0xFE))
            count = 0;
    }
    else {                /* Multiple block write */
        if (CardType & 2) {
            send_cmd(CMD55, 0); send_cmd(CMD23, count);    /* ACMD23 */
        }
        if (send_cmd(CMD25, sector) == 0) {    /* WRITE_MULTIPLE_BLOCK */
            do {
                if (!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!xmit_datablock(0, 0xFD))    /* STOP_TRAN token */
                count = 1;
        }
    }

    DESELECT();            /* CS = H */
    rcvr_spi();            /* Idle (Release DO) */

    return count ? RES_ERROR : RES_OK;
}
#endif /* _READONLY */



/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk0_ioctl (
    BYTE drv,        /* Physical drive nmuber (0) */
    BYTE ctrl,        /* Control code */
    void *buff        /* Buffer to send/receive control data */
)
{
    DRESULT res;
    BYTE n, csd[16], *ptr = buff;
    WORD csize;


    if (drv) return RES_PARERR;

    res = RES_ERROR;

    if (ctrl == CTRL_POWER) {
        switch (*ptr) {
        case 0:        /* Sub control code == 0 (POWER_OFF) */
            if (chk_power())
                power_off();        /* Power off */
            res = RES_OK;
            break;
        case 1:        /* Sub control code == 1 (POWER_ON) */
            power_on();                /* Power on */
            res = RES_OK;
            break;
        case 2:        /* Sub control code == 2 (POWER_GET) */
            *(ptr+1) = (BYTE)chk_power();
            res = RES_OK;
            break;
        default :
            res = RES_PARERR;
        }
    }
    else {
        if (Stat & STA_NOINIT) return RES_NOTRDY;

        SELECT();        /* CS = L */

        switch (ctrl) {
        case GET_SECTOR_COUNT :    /* Get number of sectors on the disk (DWORD) */
            if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
                if ((csd[0] >> 6) == 1) {    /* SDC ver 2.00 */
                    csize = csd[9] + ((WORD)csd[8] << 8) + 1;
                    *(DWORD*)buff = (DWORD)csize << 10;
                } else {                    /* MMC or SDC ver 1.XX */
                    n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                    csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                    *(DWORD*)buff = (DWORD)csize << (n - 9);
                }
                res = RES_OK;
            }
            break;

        case GET_SECTOR_SIZE :    /* Get sectors on the disk (WORD) */
            *(WORD*)buff = 512;
            res = RES_OK;
            break;

        case CTRL_SYNC :    /* Make sure that data has been written */
            if (wait_ready() == 0xFF)
                res = RES_OK;
            break;

        case MMC_GET_CSD :    /* Receive CSD as a data block (16 bytes) */
            if (send_cmd(CMD9, 0) == 0        /* READ_CSD */
                && rcvr_datablock(ptr, 16))
                res = RES_OK;
            break;

        case MMC_GET_CID :    /* Receive CID as a data block (16 bytes) */
            if (send_cmd(CMD10, 0) == 0        /* READ_CID */
                && rcvr_datablock(ptr, 16))
                res = RES_OK;
            break;

        case MMC_GET_OCR :    /* Receive OCR as an R3 resp (4 bytes) */
            if (send_cmd(CMD58, 0) == 0) {    /* READ_OCR */
                for (n = 0; n < 4; n++)
                    *ptr++ = rcvr_spi();
                res = RES_OK;
            }

//        case MMC_GET_TYPE :    /* Get card type flags (1 byte) */
//            *ptr = CardType;
//            res = RES_OK;
//            break;

        default:
            res = RES_PARERR;
        }

        DESELECT();            /* CS = H */
        rcvr_spi();            /* Idle (Release DO) */
    }

    return res;
}



/*-----------------------------------------------------------------------*/
/* Device Timer Interrupt Procedure  (Platform dependent)                */
/*-----------------------------------------------------------------------*/
/* This function must be called in period of 10ms                        */

void disk_timerproc (void)
{
//    BYTE n, s;
    BYTE n;

    n = Timer1;                        /* 100Hz decrement timer */
    if (n) Timer1 = --n;
    n = Timer2;
    if (n) Timer2 = --n;

}




//////////////////////////////////////DRIVE 1///////////////////////////////////////
/*-----------------------------------------------------------------------*/
/* This function reads sector(s) from the disk drive                     */
/*-----------------------------------------------------------------------*/

/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/

DSTATUS
disk1_initialize(
    BYTE bValue)                /* Physical drive number (0) */
{
    /* Set the not initialized flag again. If all goes well and the disk is */
    /* present, this will be cleared at the end of the function.            */
    USBStat |= STA_NOINIT;

    /* Find out if drive is ready yet. */
    if (USBHMSCDriveReady(g_psMSCInstance)) return(FR_NOT_READY);

    /* Clear the not init flag. */
    USBStat &= ~STA_NOINIT;

    return 0;
}



/*-----------------------------------------------------------------------*/
/* Returns the current status of a drive                                 */
/*-----------------------------------------------------------------------*/

DSTATUS disk1_status (
    BYTE drv)                   /* Physical drive number (0) */
{
    if (drv) return STA_NOINIT;        /* Supports only single drive */
    return USBStat;
}

DRESULT disk1_read (
    BYTE drv,               /* Physical drive number (0) */
    BYTE* buff,             /* Pointer to the data buffer to store read data */
    DWORD sector,           /* Physical drive nmuber (0) */
    BYTE count)             /* Sector count (1..255) */
{
    if(USBStat & STA_NOINIT)
    {
        return(RES_NOTRDY);
    }

    /* READ BLOCK */
    if (USBHMSCBlockRead(g_psMSCInstance, sector, buff, count) == 0)
        return RES_OK;

    return RES_ERROR;
}



/*-----------------------------------------------------------------------*/
/* This function writes sector(s) to the disk drive                     */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0
DRESULT disk1_write (
    BYTE ucDrive,           /* Physical drive number (0) */
    const BYTE* buff,       /* Pointer to the data to be written */
    DWORD sector,           /* Start sector number (LBA) */
    BYTE count)             /* Sector count (1..255) */
{
    if (ucDrive || !count) return RES_PARERR;
    if (USBStat & STA_NOINIT) return RES_NOTRDY;
    if (USBStat & STA_PROTECT) return RES_WRPRT;

    /* WRITE BLOCK */
    if(USBHMSCBlockWrite(g_psMSCInstance, sector, (unsigned char *)buff,
                         count) == 0)
        return RES_OK;

    return RES_ERROR;
}
#endif /* _READONLY */

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk1_ioctl (
    BYTE drv,               /* Physical drive number (0) */
    BYTE ctrl,              /* Control code */
    void *buff)             /* Buffer to send/receive control data */
{
    if(USBStat & STA_NOINIT)
    {
        return(RES_NOTRDY);
    }

    switch(ctrl)
    {
        case CTRL_SYNC:
            return(RES_OK);

        default:
            return(RES_PARERR);
    }
}


DWORD disk0_get_fattime (void)
{
    return  ((2007UL-1980) << 25) // Year = 2007
            | (6UL << 21)           // Month = June
            | (5UL << 16)           // Day = 5
            | (11U << 11)           // Hour = 11
            | (38U << 5)            // Min = 38
            | (0U >> 1)             // Sec = 0
            ;
}

DWORD disk1_get_fattime (void)
{
    return  ((2007UL-1980) << 25) // Year = 2007
            | (6UL << 21)           // Month = June
            | (5UL << 16)           // Day = 5
            | (11U << 11)           // Hour = 11
            | (38U << 5)            // Min = 38
            | (0U >> 1)             // Sec = 0
            ;
}

//////////////////////////////DUAL DIVER///////////////////////////////

/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/
DSTATUS
disk_initialize(
    BYTE bValue)                /* Physical drive number */
{
    /* Depending upon the physical drive, call the relevant low level */
    /* driver. Note that we call each with physical drive number 0 since the */
    /* low level drivers typically expect this (they only support a single */
    /* drive). */
    if(bValue == 0)
    {
        return(disk0_initialize(0));
    }
    else
    {
        return(disk1_initialize(0));
    }
}

/*-----------------------------------------------------------------------*/
/* Returns the current status of a drive                                 */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status (
    BYTE drv)                   /* Physical drive number */
{
    /* Depending upon the physical drive, call the relevant low level */
    /* driver. Note that we call each with physical drive number 0 since the */
    /* low level drivers typically expect this (they only support a single */
    /* drive). */
    if(drv == 0)
    {
        return(disk0_status(0));
    }
    else
    {
        return(disk1_status(0));
    }

}

/*-----------------------------------------------------------------------*/
/* This function reads sector(s) from the disk drive                     */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (
    BYTE drv,               /* Physical drive number */
    BYTE* buff,             /* Pointer to the data buffer to store read data */
    DWORD sector,           /* Sector number */
    BYTE count)             /* Sector count (1..255) */
{
    /* Depending upon the physical drive, call the relevant low level */
    /* driver. Note that we call each with physical drive number 0 since the */
    /* low level drivers typically expect this (they only support a single */
    /* drive). */
    if(drv == 0)
    {
        return(disk0_read(0, buff, sector, count));
    }
    else
    {
        return(disk1_read(0, buff, sector, count));
    }
}



/*-----------------------------------------------------------------------*/
/* This function writes sector(s) to the disk drive                     */
/*-----------------------------------------------------------------------*/
#if _READONLY == 0
DRESULT disk_write (
    BYTE ucDrive,           /* Physical drive number (0) */
    const BYTE* buff,       /* Pointer to the data to be written */
    DWORD sector,           /* Sector number */
    BYTE count)             /* Sector count (1..255) */
{
    /* Depending upon the physical drive, call the relevant low level */
    /* driver. Note that we call each with physical drive number 0 since the */
    /* low level drivers typically expect this (they only support a single */
    /* drive). */
    if(ucDrive == 0)
    {
        return(disk0_write(0, buff, sector, count));
    }
    else
    {
        return(disk1_write(0, buff, sector, count));
    }
}
#endif /* _READONLY */

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl (
    BYTE drv,               /* Physical drive number (0) */
    BYTE ctrl,              /* Control code */
    void *buff)             /* Buffer to send/receive control data */
{
    /* Depending upon the physical drive, call the relevant low level */
    /* driver. Note that we call each with physical drive number 0 since the */
    /* low level drivers typically expect this (they only support a single */
    /* drive). */
    if(drv == 0)
    {
        return(disk0_ioctl(0, ctrl, buff));
    }
    else
    {
        return(disk1_ioctl(0, ctrl, buff));
    }
}

/*---------------------------------------------------------*/
/* User Provided Timer Function for FatFs module           */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support a real time clock.          */
/*                                                         */
/* Note that each driver implements the same function but, */
/* since the function doesn't take any parameter, we can't */
/* decide automatically which driver's get_fattime() to    */
/* invoke. Instead, we control this with a #define         */
/* which defaults to calling the first driver.             */
/*                                                         */
DWORD get_fattime (void)
{
#ifndef DRIVE1_TIME_MASTER
    return(disk0_get_fattime());
#else
    return(disk1_get_fattime());
#endif
}




/*---------------------------------------------------------*/
/* User Provided Timer Function for FatFs module           */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support a real time clock.          */


