/*
 * This code borrows heavily from the Mbed SDBlockDevice:
 *       https://os.mbed.com/docs/mbed-os/v5.15/apis/sdblockdevice.html
 *       mbed-os/components/storage/blockdevice/COMPONENT_SD/SDBlockDevice.cpp
 *
 * Editor: Carl Kugler (carlk3@gmail.com)
 *
 * Remember your ABCs: "Always Be Cobbling!"
 */

/* mbed Microcontroller Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Introduction
 * ------------
 * SD and MMC cards support a number of interfaces, but common to them all
 * is one based on SPI. Since we already have the mbed SPI Interface, it will
 * be used for SD cards.
 *
 * The main reference I'm using is Chapter 7, "SPI Mode" of:
 *  http://www.sdcard.org/developers/tech/sdcard/pls/Simplified_Physical_Layer_Spec.pdf
 *
 * SPI Startup
 * -----------
 * The SD card powers up in SD mode. The start-up procedure is complicated
 * by the requirement to support older SDCards in a backwards compatible
 * way with the new higher capacity variants SDHC and SDHC.
 *
 * The following figures from the specification with associated text describe
 * the SPI mode initialisation process:
 *  - Figure 7-1: SD Memory Card State Diagram (SPI mode)
 *  - Figure 7-2: SPI Mode Initialization Flow
 *
 * Firstly, a low initial clock should be selected (in the range of 100-
 * 400kHZ). After initialisation has been completed, the switch to a
 * higher clock speed can be made (e.g. 1MHz). Newer cards will support
 * higher speeds than the default _transfer_sck defined here.
 *
 * Next, note the following from the SDCard specification (note to
 * Figure 7-1):
 *
 *  In any of the cases CMD1 is not recommended because it may be difficult for
 * the host to distinguish between MultiMediaCard and SD Memory Card
 *
 * Hence CMD1 is not used for the initialisation sequence.
 *
 * The SPI interface mode is selected by asserting CS low and sending the
 * reset command (CMD0). The card will respond with a (R1) response.
 * In practice many cards initially respond with 0xff or invalid data
 * which is ignored. Data is read until a valid response is received
 * or the number of re-reads has exceeded a maximim count. If a valid
 * response is not received then the CMD0 can be retried. This
 * has been found to successfully initialise cards where the SPI master
 * (on MCU) has been reset but the SDCard has not, so the first
 * CMD0 may be lost.
 *
 * CMD8 is optionally sent to determine the voltage range supported, and
 * indirectly determine whether it is a version 1.x SD/non-SD card or
 * version 2.x. I'll just ignore this for now.
 *
 * ACMD41 is repeatedly issued to initialise the card, until "in idle"
 * (bit 0) of the R1 response goes to '0', indicating it is initialised.
 *
 * You should also indicate whether the host supports High Capicity cards,
 * and check whether the card is high capacity - i'll also ignore this.
 *
 * SPI Protocol
 * ------------
 * The SD SPI protocol is based on transactions made up of 8-bit words, with
 * the host starting every bus transaction by asserting the CS signal low. The
 * card always responds to commands, data blocks and errors.
 *
 * The protocol supports a CRC, but by default it is off (except for the
 * first reset CMD0, where the CRC can just be pre-calculated, and CMD8)
 * I'll leave the CRC off I think!
 *
 * Standard capacity cards have variable data block sizes, whereas High
 * Capacity cards fix the size of data block to 512 bytes. I'll therefore
 * just always use the Standard Capacity cards with a block size of 512 bytes.
 * This is set with CMD16.
 *
 * You can read and write single blocks (CMD17, CMD25) or multiple blocks
 * (CMD18, CMD25). For simplicity, I'll just use single block accesses. When
 * the card gets a read command, it responds with a response token, and then
 * a data token or an error.
 *
 * SPI Command Format
 * ------------------
 * Commands are 6-bytes long, containing the command, 32-bit argument, and CRC.
 *
 * +---------------+------------+------------+-----------+----------+--------------+
 * | 01 | cmd[5:0] | arg[31:24] | arg[23:16] | arg[15:8] | arg[7:0] | crc[6:0] |
 * 1 |
 * +---------------+------------+------------+-----------+----------+--------------+
 *
 * As I'm not using CRC, I can fix that byte to what is needed for CMD0 (0x95)
 *
 * All Application Specific commands shall be preceded with APP_CMD (CMD55).
 *
 * SPI Response Format
 * -------------------
 * The main response format (R1) is a status byte (normally zero). Key flags:
 *  idle - 1 if the card is in an idle state/initialising
 *  cmd  - 1 if an illegal command code was detected
 *
 *    +-------------------------------------------------+
 * R1 | 0 | arg | addr | seq | crc | cmd | erase | idle |
 *    +-------------------------------------------------+
 *
 * R1b is the same, except it is followed by a busy signal (zeros) until
 * the first non-zero byte when it is ready again.
 *
 * Data Response Token
 * -------------------
 * Every data block written to the card is acknowledged by a byte
 * response token
 *
 * +----------------------+
 * | xxx | 0 | status | 1 |
 * +----------------------+
 *              010 - OK!
 *              101 - CRC Error
 *              110 - Write Error
 *
 * Single Block Read and Write
 * ---------------------------
 *
 * Block transfers have a byte header, followed by the data, followed
 * by a 16-bit CRC. In our case, the data will always be 512 bytes.
 *
 * +------+---------+---------+- -  - -+---------+-----------+----------+
 * | 0xFE | data[0] | data[1] |        | data[n] | crc[15:8] | crc[7:0] |
 * +------+---------+---------+- -  - -+---------+-----------+----------+
 */

#include <assert.h>
#include <inttypes.h>
#include "crc.h"
#include "sd_card.h"
#include "util.h"
#include "my_debug.h"
//
#include "ff.h"
#include "diskio.h" /* Declarations of disk functions */  // Needed for STA_NOINIT, ...

#if defined(NDEBUG)
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "sd_spi.h"

#ifndef TRACE
#  define TRACE 0
#endif

#ifndef SD_CRC_ENABLED
#define SD_CRC_ENABLED 1
#endif

#if SD_CRC_ENABLED
static bool crc_on = true;
#else
static bool crc_on = false;
#endif

#define TRACE_PRINTF(fmt, args...)
//#define TRACE_PRINTF printf  // task_printf

/* Control Tokens   */
#define SPI_DATA_RESPONSE_MASK (0x1F)
#define SPI_DATA_ACCEPTED (0x05)
#define SPI_DATA_CRC_ERROR (0x0B)
#define SPI_DATA_WRITE_ERROR (0x0D)
#define SPI_START_BLOCK (0xFE)         /*!< For Single Block Read/Write and Multiple Block Read */
#define SPI_START_BLK_MUL_WRITE (0xFC) /*!< Start Multi-block write */
#define SPI_STOP_TRAN (0xFD)           /*!< Stop Multi-block write */

#define SPI_DATA_READ_ERROR_MASK (0xF)  /*!< Data Error Token: 4 LSB bits */
#define SPI_READ_ERROR (0x1 << 0)       /*!< Error */
#define SPI_READ_ERROR_CC (0x1 << 1)    /*!< CC Error*/
#define SPI_READ_ERROR_ECC_C (0x1 << 2) /*!< Card ECC failed */
#define SPI_READ_ERROR_OFR (0x1 << 3)   /*!< Out of Range */

// SPI Slave Select
#define SSEL_ACTIVE (0)
#define SSEL_INACTIVE (1)

/* R1 Response Format */
#define R1_NO_RESPONSE (0xFF)
#define R1_RESPONSE_RECV (0x80)
#define R1_IDLE_STATE (1 << 0)
#define R1_ERASE_RESET (1 << 1)
#define R1_ILLEGAL_COMMAND (1 << 2)
#define R1_COM_CRC_ERROR (1 << 3)
#define R1_ERASE_SEQUENCE_ERROR (1 << 4)
#define R1_ADDRESS_ERROR (1 << 5)
#define R1_PARAMETER_ERROR (1 << 6)

/* SIZE in Bytes */
#define PACKET_SIZE 6         /*!< SD Packet size CMD+ARG+CRC */
#define R1_RESPONSE_SIZE 1    /*!< Size of R1 response */
#define R2_RESPONSE_SIZE 2    /*!< Size of R2 response */
#define R3_R7_RESPONSE_SIZE 5 /*!< Size of R3/R7 response */

/* R3 Response : OCR Register */
#define OCR_HCS_CCS (0x1 << 30)
#define OCR_LOW_VOLTAGE (0x01 << 24)
#define OCR_3_3V (0x1 << 20)

#define SPI_CMD(x) (0x40 | (x & 0x3f))

#ifdef NDEBUG 
#  pragma GCC diagnostic ignored "-Wunused-variable"
#endif

static uint8_t sd_cmd_spi(sd_card_t *sd_card_p, cmdSupported cmd, uint32_t arg) {
    uint8_t response;
    char cmdPacket[PACKET_SIZE];

    // Prepare the command packet
    cmdPacket[0] = SPI_CMD(cmd);
    cmdPacket[1] = (arg >> 24);
    cmdPacket[2] = (arg >> 16);
    cmdPacket[3] = (arg >> 8);
    cmdPacket[4] = (arg >> 0);

    if (crc_on) {
        cmdPacket[5] = (crc7(cmdPacket, 5) << 1) | 0x01;
    } else
    {
        // CMD0 is executed in SD mode, hence should have correct CRC
        // CMD8 CRC verification is always enabled
        switch (cmd) {
            case CMD0_GO_IDLE_STATE:
                cmdPacket[5] = 0x95;
                break;
            case CMD8_SEND_IF_COND:
                cmdPacket[5] = 0x87;
                break;
            default:
                cmdPacket[5] = 0xFF;  // Make sure bit 0-End bit is high
                break;
        }
    }
    // send a command
    for (int i = 0; i < PACKET_SIZE; i++) {
        sd_spi_write(sd_card_p, cmdPacket[i]);
    }
    // The received byte immediataly following CMD12 is a stuff byte,
    // it should be discarded before receive the response of the CMD12.
    if (CMD12_STOP_TRANSMISSION == cmd) {
        sd_spi_write(sd_card_p, SPI_FILL_CHAR);
    }
    // Loop for response: Response is sent back within command response time
    // (NCR), 0 to 8 bytes for SDC
    for (int i = 0; i < 0x10; i++) {
        response = sd_spi_write(sd_card_p, SPI_FILL_CHAR);
        // Got the response
        if (!(response & R1_RESPONSE_RECV)) {
            break;
        }
    }
    return response;
}

static bool sd_wait_ready(sd_card_t *sd_card_p, int timeout) {
    char resp;

    // Keep sending dummy clocks with DI held high until the card releases the
    // DO line
    absolute_time_t timeout_time = make_timeout_time_ms(timeout);
    do {
        resp = sd_spi_write(sd_card_p, 0xFF);
    } while (resp == 0x00 &&
             0 < absolute_time_diff_us(get_absolute_time(), timeout_time));

    if (resp == 0x00) DBG_PRINTF("%s failed\r\n", __FUNCTION__);

    // Return success/failure
    return (resp > 0x00);
}

// An SD card can only do one thing at a time.
static void sd_lock(sd_card_t *sd_card_p) {
    assert(mutex_is_initialized(&sd_card_p->mutex));
    mutex_enter_blocking(&sd_card_p->mutex);
}
static void sd_unlock(sd_card_t *sd_card_p) {
    assert(mutex_is_initialized(&sd_card_p->mutex));
    mutex_exit(&sd_card_p->mutex);
}
static bool sd_is_locked(sd_card_t *sd_card_p) {
    assert(mutex_is_initialized(&sd_card_p->mutex));
    uint32_t owner_out;
    return !mutex_try_enter(&sd_card_p->mutex, &owner_out);
}

// Locks the SD card and acquires its SPI
static void sd_acquire(sd_card_t *sd_card_p) {
    sd_lock(sd_card_p);
    sd_spi_acquire(sd_card_p);
}
static void sd_release(sd_card_t *sd_card_p) {
    sd_unlock(sd_card_p);
    sd_spi_release(sd_card_p);
}

#if TRACE
static const char *cmd2str(const cmdSupported cmd) {
    switch (cmd) {
        default:
            return "CMD_NOT_SUPPORTED";
        case CMD0_GO_IDLE_STATE:
            return "CMD0_GO_IDLE_STATE";
        case CMD1_SEND_OP_COND:
            return "CMD1_SEND_OP_COND";
        case CMD6_SWITCH_FUNC:
            return "CMD6_SWITCH_FUNC";
        case CMD8_SEND_IF_COND:
            return "CMD8_SEND_IF_COND";
        case CMD9_SEND_CSD:
            return "CMD9_SEND_CSD";
        case CMD10_SEND_CID:
            return "CMD10_SEND_CID";
        case CMD12_STOP_TRANSMISSION:
            return "CMD12_STOP_TRANSMISSION";
        case CMD13_SEND_STATUS:
            return "CMD13_SEND_STATUS or ACMD6_SET_BUS_WIDTH or "
                   "ACMD13_SD_STATUS";
        case CMD16_SET_BLOCKLEN:
            return "CMD16_SET_BLOCKLEN";
        case CMD17_READ_SINGLE_BLOCK:
            return "CMD17_READ_SINGLE_BLOCK";
        case CMD18_READ_MULTIPLE_BLOCK:
            return "CMD18_READ_MULTIPLE_BLOCK";
        case CMD24_WRITE_BLOCK:
            return "CMD24_WRITE_BLOCK";
        case CMD25_WRITE_MULTIPLE_BLOCK:
            return "CMD25_WRITE_MULTIPLE_BLOCK";
        case CMD27_PROGRAM_CSD:
            return "CMD27_PROGRAM_CSD";
        case CMD32_ERASE_WR_BLK_START_ADDR:
            return "CMD32_ERASE_WR_BLK_START_ADDR";
        case CMD33_ERASE_WR_BLK_END_ADDR:
            return "CMD33_ERASE_WR_BLK_END_ADDR";
        case CMD38_ERASE:
            return "CMD38_ERASE";
        case CMD55_APP_CMD:
            return "CMD55_APP_CMD";
        case CMD56_GEN_CMD:
            return "CMD56_GEN_CMD";
        case CMD58_READ_OCR:
            return "CMD58_READ_OCR";
        case CMD59_CRC_ON_OFF:
            return "CMD59_CRC_ON_OFF";
        // case ACMD6_SET_BUS_WIDTH:
        // case ACMD13_SD_STATUS:
        case ACMD22_SEND_NUM_WR_BLOCKS:
            return "ACMD22_SEND_NUM_WR_BLOCKS";
        case ACMD23_SET_WR_BLK_ERASE_COUNT:
            return "ACMD23_SET_WR_BLK_ERASE_COUNT";
        case ACMD41_SD_SEND_OP_COND:
            return "ACMD41_SD_SEND_OP_COND";
        case ACMD42_SET_CLR_CARD_DETECT:
            return "ACMD42_SET_CLR_CARD_DETECT";
        case ACMD51_SEND_SCR:
            return "ACMD51_SEND_SCR";
    }
}
#endif

#define SD_COMMAND_RETRIES 3 /*!< Times SPI cmd is retried when there is no response */
#define SD_COMMAND_TIMEOUT 2000 /*!< Timeout in ms for response */

static int sd_cmd(sd_card_t *sd_card_p, const cmdSupported cmd, uint32_t arg,
                  bool isAcmd, uint32_t *resp) {
    TRACE_PRINTF("%s(%s(0x%08lx)): ", __FUNCTION__, cmd2str(cmd), arg);
    assert(sd_is_locked(sd_card_p));
    assert(0 == gpio_get(sd_card_p->spi_if_p->ss_gpio));

    int32_t status = SD_BLOCK_DEVICE_ERROR_NONE;
    uint32_t response;

    // No need to wait for card to be ready when sending the stop command
    if (CMD12_STOP_TRANSMISSION != cmd
        && CMD0_GO_IDLE_STATE != cmd) 
    {
        if (false == sd_wait_ready(sd_card_p, SD_COMMAND_TIMEOUT)) {
            DBG_PRINTF("%s:%d: Card not ready yet\r\n", __FILE__, __LINE__);
            return SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
        }
    }
    // Re-try command
    for (int i = 0; i < SD_COMMAND_RETRIES; i++) {
        // Send CMD55 for APP command first
        if (isAcmd) {
            response = sd_cmd_spi(sd_card_p, CMD55_APP_CMD, 0x0);
            // Wait for card to be ready after CMD55
            if (false == sd_wait_ready(sd_card_p, SD_COMMAND_TIMEOUT)) {
                DBG_PRINTF("%s:%d: Card not ready yet\r\n", __FILE__, __LINE__);
            }
        }
        // Send command over SPI interface
        response = sd_cmd_spi(sd_card_p, cmd, arg);
        if (R1_NO_RESPONSE == response) {
            DBG_PRINTF("No response CMD:%d\r\n", cmd);
            continue;
        }
        break;
    }
    // Pass the response to the command call if required
    if (NULL != resp) {
        *resp = response;
    }
    // Process the response R1  : Exit on CRC/Illegal command error/No response
    if (R1_NO_RESPONSE == response) {
        DBG_PRINTF("No response CMD:%d response: 0x%" PRIx32 "\r\n", cmd,
                   response);
        return SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
    }
    if (response & R1_COM_CRC_ERROR && ACMD23_SET_WR_BLK_ERASE_COUNT != cmd) {
        DBG_PRINTF("CRC error CMD:%d response 0x%" PRIx32 "\r\n", cmd, response);
        return SD_BLOCK_DEVICE_ERROR_CRC;  // CRC error
    }
    if (response & R1_ILLEGAL_COMMAND) {
        if (ACMD23_SET_WR_BLK_ERASE_COUNT != cmd)
            DBG_PRINTF("Illegal command CMD:%d response 0x%" PRIx32 "\r\n", cmd,
                       response);
        if (CMD8_SEND_IF_COND == cmd) {
            // Illegal command is for Ver1 or not SD Card
            sd_card_p->card_type = CARD_UNKNOWN;
        }
        return SD_BLOCK_DEVICE_ERROR_UNSUPPORTED;  // Command not supported
    }

    //	DBG_PRINTF("CMD:%d \t arg:0x%" PRIx32 " \t Response:0x%" PRIx32 "\r\n",
    // cmd, arg, response);
    // Set status for other errors
    if ((response & R1_ERASE_RESET) || (response & R1_ERASE_SEQUENCE_ERROR)) {
        status = SD_BLOCK_DEVICE_ERROR_ERASE;  // Erase error
    } else if ((response & R1_ADDRESS_ERROR) ||
               (response & R1_PARAMETER_ERROR)) {
        // Misaligned address / invalid address block length
        status = SD_BLOCK_DEVICE_ERROR_PARAMETER;
    }

    // Get rest of the response part for other commands
    switch (cmd) {
        case CMD8_SEND_IF_COND:  // Response R7
            DBG_PRINTF("V2-Version Card\r\n");
            sd_card_p->card_type = SDCARD_V2;  // fallthrough
            // Note: No break here, need to read rest of the response
        case CMD58_READ_OCR:  // Response R3
            response = (sd_spi_write(sd_card_p, SPI_FILL_CHAR) << 24);
            response |= (sd_spi_write(sd_card_p, SPI_FILL_CHAR) << 16);
            response |= (sd_spi_write(sd_card_p, SPI_FILL_CHAR) << 8);
            response |= sd_spi_write(sd_card_p, SPI_FILL_CHAR);
            DBG_PRINTF("R3/R7: 0x%" PRIx32 "\r\n", response);
            break;
        case CMD12_STOP_TRANSMISSION:  // Response R1b
        case CMD38_ERASE:
            sd_wait_ready(sd_card_p, SD_COMMAND_TIMEOUT);
            break;
        case CMD13_SEND_STATUS:  // Response R2
            response <<= 8;
            response |= sd_spi_write(sd_card_p, SPI_FILL_CHAR);
            if (response) {
                DBG_PRINTF("R2: 0x%" PRIx32 "\r\n", response);
                if (response & 0x01 << 0) {
                    DBG_PRINTF("Card is Locked                         \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_WRITE;
                }
                if (response & 0x01 << 1) {
                    DBG_PRINTF("WP Erase Skip, Lock/Unlock Cmd Failed  \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_WRITE_PROTECTED;
                }
                if (response & 0x01 << 2) {
                    DBG_PRINTF("Error                                  \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_WRITE;
                }
                if (response & 0x01 << 3) {
                    DBG_PRINTF("CC Error                               \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_WRITE;
                }
                if (response & 0x01 << 4) {
                    DBG_PRINTF("Card ECC Failed                        \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_WRITE;
                }
                if (response & 0x01 << 5) {
                    DBG_PRINTF("WP Violation                           \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_WRITE_PROTECTED;
                }
                if (response & 0x01 << 6) {
                    DBG_PRINTF("Erase Param                            \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_ERASE;
                }
                if (response & 0x01 << 7) {
                    DBG_PRINTF("Out of Range, CSD_Overwrite            \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_PARAMETER;
                }
                if (response & 0x01 << 8) {
                    DBG_PRINTF("In Idle State                          \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_NONE;
                }
                if (response & 0x01 << 9) {
                    DBG_PRINTF("Erase Reset                            \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_ERASE;
                }
                if (response & 0x01 << 10) {
                    DBG_PRINTF("Illegal Command                        \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_UNSUPPORTED;
                }
                if (response & 0x01 << 11) {
                    DBG_PRINTF("Com CRC Error                          \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_CRC;
                }
                if (response & 0x01 << 12) {
                    DBG_PRINTF("Erase Sequence Error                   \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_ERASE;
                }
                if (response & 0x01 << 13) {
                    DBG_PRINTF("Address Error                          \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_PARAMETER;
                }
                if (response & 0x01 << 14) {
                    DBG_PRINTF("Parameter Error                        \r\n");
                    status = SD_BLOCK_DEVICE_ERROR_PARAMETER;
                }
                break;
            }
        default:  // Response R1
            break;
    }
    // Pass the updated response to the command
    if (NULL != resp) {
        *resp = response;
    }
    return status;
}


/* R7 response pattern for CMD8 */
#define CMD8_PATTERN (0xAA)

static int sd_cmd8(sd_card_t *sd_card_p) {
    uint32_t arg = (CMD8_PATTERN << 0);  // [7:0]check pattern
    uint32_t response = 0;
    int32_t status = SD_BLOCK_DEVICE_ERROR_NONE;

    arg |= (0x1 << 8);  // 2.7-3.6V             // [11:8]supply voltage(VHS)

    status = sd_cmd(sd_card_p, CMD8_SEND_IF_COND, arg, false, &response);
    // Verify voltage and pattern for V2 version of card
    if ((SD_BLOCK_DEVICE_ERROR_NONE == status) &&
        (SDCARD_V2 == sd_card_p->card_type)) {
        // If check pattern is not matched, CMD8 communication is not valid
        if ((response & 0xFFF) != arg) {
            DBG_PRINTF("CMD8 Pattern mismatch 0x%" PRIx32 " : 0x%" PRIx32
                       "\r\n", arg, response);
            sd_card_p->card_type = CARD_UNKNOWN;
            status = SD_BLOCK_DEVICE_ERROR_UNUSABLE;
        }
    }
    return status;
}

static int sd_read_bytes(sd_card_t *sd_card_p, uint8_t *buffer, uint32_t length);

static uint64_t in_sd_spi_sectors(sd_card_t *sd_card_p) {
    uint32_t c_size, c_size_mult, read_bl_len;
    uint32_t block_len, mult, blocknr;
    uint32_t hc_c_size;
    uint64_t blocks = 0, capacity = 0;

    // CMD9, Response R2 (R1 byte + 16-byte block read)
    if (sd_cmd(sd_card_p, CMD9_SEND_CSD, 0x0, false, 0) != 0x0) {
        DBG_PRINTF("Didn't get a response from the disk\r\n");
        return 0;
    }
    if (sd_read_bytes(sd_card_p, sd_card_p->csd.csd, 16) != 0) {
        DBG_PRINTF("Couldn't read CSD response from disk\r\n");
        return 0;
    }
    // csd_structure : csd[127:126]
    int csd_structure = ext_bits(sd_card_p->csd.csd, 127, 126);
    switch (csd_structure) {
        case 0:
            c_size = ext_bits(sd_card_p->csd.csd, 73, 62);       // c_size        : csd[73:62]
            c_size_mult = ext_bits(sd_card_p->csd.csd, 49, 47);  // c_size_mult   : csd[49:47]
            read_bl_len =
                ext_bits(sd_card_p->csd.csd, 83, 80);     // read_bl_len   : csd[83:80] - the
                                           // *maximum* read block length
            block_len = 1 << read_bl_len;  // BLOCK_LEN = 2^READ_BL_LEN
            mult = 1 << (c_size_mult +
                         2);                // MULT = 2^C_SIZE_MULT+2 (C_SIZE_MULT < 8)
            blocknr = (c_size + 1) * mult;  // BLOCKNR = (C_SIZE+1) * MULT
            capacity = (uint64_t)blocknr *
                       block_len;  // memory capacity = BLOCKNR * BLOCK_LEN
            blocks = capacity / _block_size;
            break;

        case 1:
            hc_c_size =
                ext_bits(sd_card_p->csd.csd, 69, 48);       // device size : C_SIZE : [69:48]
            blocks = (hc_c_size + 1) << 10;  // block count = C_SIZE+1) * 1K
                                             // byte (512B is block size)
            break;

        default:
            DBG_PRINTF("CSD struct unsupported\r\n");
            assert(!"CSD struct unsupported\r\n");
            return 0;
    };
    return blocks;
}
uint64_t sd_spi_sectors(sd_card_t *sd_card_p) {
    sd_acquire(sd_card_p);
    uint64_t sectors = in_sd_spi_sectors(sd_card_p);
    sd_release(sd_card_p);
    return sectors;
}

// SPI function to wait till chip is ready and sends start token
static bool sd_wait_token(sd_card_t *sd_card_p, uint8_t token) {
    TRACE_PRINTF("%s(0x%02hhx)\r\n", __FUNCTION__, token);

    const uint32_t timeout = SD_COMMAND_TIMEOUT;  // Wait for start token
    absolute_time_t timeout_time = make_timeout_time_ms(timeout);
    do {
        if (token == sd_spi_write(sd_card_p, SPI_FILL_CHAR)) {
            return true;
        }
    } while (0 < absolute_time_diff_us(get_absolute_time(), timeout_time));
    DBG_PRINTF("sd_wait_token: timeout\r\n");
    return false;
}

static bool chk_crc16(uint8_t *buffer, size_t length, uint16_t crc) {
    if (crc_on) {
        uint32_t crc_result;
        // Compute and verify checksum
        crc_result = crc16((void *)buffer, length);
        return ((uint16_t)crc_result == crc);
    }
    return true;
}

#define SPI_START_BLOCK (0xFE) /* For Single Block Read/Write and Multiple Block Read */

static int sd_read_bytes(sd_card_t *sd_card_p, uint8_t *buffer, uint32_t length) {
    uint16_t crc;

    // read until start byte (0xFE)
    if (false == sd_wait_token(sd_card_p, SPI_START_BLOCK)) {
        DBG_PRINTF("%s:%d Read timeout\r\n", __FILE__, __LINE__);
        return SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
    }
    // read data
    for (uint32_t i = 0; i < length; i++) {
        buffer[i] = sd_spi_write(sd_card_p, SPI_FILL_CHAR);
    }
    // Read the CRC16 checksum for the data block
    crc = (sd_spi_write(sd_card_p, SPI_FILL_CHAR) << 8);
    crc |= sd_spi_write(sd_card_p, SPI_FILL_CHAR);

    if (!chk_crc16(buffer, length, crc)) {
        DBG_PRINTF("%s: Invalid CRC received: 0x%" PRIx16 "\r\n", __func__, crc);
        return SD_BLOCK_DEVICE_ERROR_CRC;
    }
    return 0;
}

/* Transfer tx to SPI while receiving SPI to rx. 
tx or rx can be NULL if not important. */
static void sd_spi_transfer_start(sd_card_t *sd_card_p, const uint8_t *tx, uint8_t *rx, size_t length) {
    return spi_transfer_start(sd_card_p->spi_if_p->spi, tx, rx, length);
}
static bool sd_spi_transfer_wait_complete(sd_card_t *sd_card_p, uint32_t timeout_ms) {
    return spi_transfer_wait_complete(sd_card_p->spi_if_p->spi, timeout_ms);
}

static int in_sd_read_blocks(sd_card_t *sd_card_p, uint8_t *buffer_addr,
                             uint64_t ulSectorNumber, uint32_t ulSectorCount) {

    if (sd_card_p->m_Status & (STA_NOINIT | STA_NODISK))
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;
    if (ulSectorNumber + ulSectorCount > sd_card_p->sectors)
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;

    uint64_t addr;
    // SDSC Card (CCS=0) uses byte unit address
    // SDHC and SDXC Cards (CCS=1) use block unit address (512 Bytes unit)
    if (SDCARD_V2HC == sd_card_p->card_type) {
        addr = ulSectorNumber;
    } else {
        addr = ulSectorNumber * _block_size;
    }

    int status = SD_BLOCK_DEVICE_ERROR_NONE;

    // Write command to receive data
    if (ulSectorCount == 1)    
        status = sd_cmd(sd_card_p, CMD17_READ_SINGLE_BLOCK, addr, false, 0);
    else 
        status = sd_cmd(sd_card_p, CMD18_READ_MULTIPLE_BLOCK, addr, false, 0);
    if (SD_BLOCK_DEVICE_ERROR_NONE != status) {
        return status;
    }
    /* Optimization:
    While the DMA is busy transfering the block data,
    use the some of the wait time to check the CRC
    for the previous block.
    */
    uint16_t prev_block_crc = 0;
    uint8_t *prev_buffer_addr = 0;

    uint32_t blockCnt = ulSectorCount;
    // receive the data : one block at a time
    while (blockCnt && (SD_BLOCK_DEVICE_ERROR_NONE == status)) {
        // read until start byte (0xFE)
        if (!sd_wait_token(sd_card_p, SPI_START_BLOCK)) {
            DBG_PRINTF("%s:%d Read timeout\r\n", __FILE__, __LINE__);
            status = SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
            break;
        }
        // read data
        sd_spi_transfer_start(sd_card_p, NULL, buffer_addr, _block_size);
        
        if (prev_buffer_addr) { 
            // Check previous block's CRC:
            if (!chk_crc16(prev_buffer_addr, _block_size, prev_block_crc)) {
                DBG_PRINTF("%s: Invalid CRC received: 0x%" PRIx16 "\r\n", __func__, prev_block_crc);
                status = SD_BLOCK_DEVICE_ERROR_CRC;
            }
        }

        bool ok = sd_spi_transfer_wait_complete(sd_card_p, 1000);
        if (!ok) {
            status = SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
            break;
        }
        // Read the CRC16 checksum for the data block
        prev_block_crc = (sd_spi_write(sd_card_p, SPI_FILL_CHAR) << 8);
        prev_block_crc |= sd_spi_write(sd_card_p, SPI_FILL_CHAR);
        prev_buffer_addr = buffer_addr;

        buffer_addr += _block_size;
        --blockCnt;
    }
    // Send CMD12(0x00000000) to stop the transmission for multi-block transfer
    if (ulSectorCount > 1)
        sd_cmd(sd_card_p, CMD12_STOP_TRANSMISSION, 0x0, false, 0);

    if (SD_BLOCK_DEVICE_ERROR_NONE == status) {
        // Check final block's CRC:
        if (!chk_crc16(prev_buffer_addr, _block_size, prev_block_crc)) {
            DBG_PRINTF("%s: Invalid CRC received: 0x%" PRIx16 "\r\n", __func__, prev_block_crc);
            status = SD_BLOCK_DEVICE_ERROR_CRC;
        }
    }
    return status;
}
static int sd_read_blocks(sd_card_t *sd_card_p, uint8_t *buffer, uint64_t ulSectorNumber,
                          uint32_t ulSectorCount) {
    TRACE_PRINTF("sd_read_blocks(0x%p, 0x%llx, 0x%lx)\r\n", buffer,
                 ulSectorNumber, ulSectorCount);
    sd_acquire(sd_card_p);
    int status = SD_BLOCK_DEVICE_ERROR_NONE;
    status = in_sd_read_blocks(sd_card_p, buffer, ulSectorNumber, ulSectorCount);   
    sd_release(sd_card_p);
    return status;
}

static int sd_write_block(sd_card_t *sd_card_p, const uint8_t *buffer,
                           uint8_t token, uint32_t length) {
    uint16_t crc = (~0);
    // indicate start of block
    sd_spi_write(sd_card_p, token);

    // write the data
    sd_spi_transfer_start(sd_card_p, buffer, NULL, length);

    /* Optimization:
    While the DMA is busy transfering the block data, 
    use the some of the wait time to calculate the CRC.
    Typically, DMA transfer of the block data takes about 244 us, 
    but the CRC16 calculation takes only about 66 us.
    */

    // While DMA transfers the block, compute CRC:
    if (crc_on) {
        // Compute CRC
        crc = crc16((void *)buffer, length);
    }

    bool ok = sd_spi_transfer_wait_complete(sd_card_p, 1000);
    if (!ok)
        return SD_BLOCK_DEVICE_ERROR_WRITE;

    // write the checksum CRC16
    sd_spi_write(sd_card_p, crc >> 8);
    sd_spi_write(sd_card_p, crc);

    // check the response token
    uint8_t response = sd_spi_write(sd_card_p, SPI_FILL_CHAR);

    // Only CRC and general write error are communicated via response token
    if ((response & SPI_DATA_RESPONSE_MASK) != SPI_DATA_ACCEPTED) {
        DBG_PRINTF("Block Write not accepted. Response token: 0x%x \r\n", response);
        return SD_BLOCK_DEVICE_ERROR_WRITE;
    }
    // Wait while card is busy programming
    if (false == sd_wait_ready(sd_card_p, SD_COMMAND_TIMEOUT)) {
        DBG_PRINTF("%s:%d: Card not ready yet\r\n", __FILE__, __LINE__);
        return SD_BLOCK_DEVICE_ERROR_WRITE;
    }
    return SD_BLOCK_DEVICE_ERROR_NONE;
}
/** Program blocks to a block device
 *
 *
 *  @param buffer       Buffer of data to write to blocks
 *  @param ulSectorNumber     Logical Address of block to begin writing to (LBA)
 *  @param blockCnt     Size to write in blocks
 *  @return         SD_BLOCK_DEVICE_ERROR_NONE(0) - success
 *                  SD_BLOCK_DEVICE_ERROR_NO_DEVICE - device (SD card) is
 * missing or not connected SD_BLOCK_DEVICE_ERROR_CRC - crc error
 *                  SD_BLOCK_DEVICE_ERROR_PARAMETER - invalid parameter
 *                  SD_BLOCK_DEVICE_ERROR_UNSUPPORTED - unsupported command
 *                  SD_BLOCK_DEVICE_ERROR_NO_INIT - device is not initialized
 *                  SD_BLOCK_DEVICE_ERROR_WRITE - SPI write error
 *                  SD_BLOCK_DEVICE_ERROR_ERASE - erase error
 */
static int in_sd_write_blocks(sd_card_t *sd_card_p, const uint8_t *buffer,
                              uint64_t ulSectorNumber, uint32_t blockCnt) {
    if (ulSectorNumber + blockCnt > sd_card_p->sectors)
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;
    if (sd_card_p->m_Status & (STA_NOINIT | STA_NODISK))
        return SD_BLOCK_DEVICE_ERROR_PARAMETER;

    int status = SD_BLOCK_DEVICE_ERROR_NONE;    
    uint64_t addr;

    // SDSC Card (CCS=0) uses byte unit address
    // SDHC and SDXC Cards (CCS=1) use block unit address (512 Bytes unit)
    if (SDCARD_V2HC == sd_card_p->card_type) {
        addr = ulSectorNumber;
    } else {
        addr = ulSectorNumber * _block_size;
    }
    // Send command to perform write operation
    if (blockCnt == 1) {
        // Single block write command
        status = sd_cmd(sd_card_p, CMD24_WRITE_BLOCK, addr, false, 0);
        if (SD_BLOCK_DEVICE_ERROR_NONE != status) {
            return status;
        }
        // Write data
        sd_write_block(sd_card_p, buffer, SPI_START_BLOCK, _block_size);
    } else {
        // Pre-erase setting prior to multiple block write operation
        sd_cmd(sd_card_p, ACMD23_SET_WR_BLK_ERASE_COUNT, blockCnt, 1, 0);

        // Some SD cards want to be deselected between every command:
        sd_spi_deselect_pulse(sd_card_p);

        // Multiple block write command
        status = sd_cmd(sd_card_p, CMD25_WRITE_MULTIPLE_BLOCK, addr, false, 0);
        if (SD_BLOCK_DEVICE_ERROR_NONE != status) {
            return status;
        }
        // Write the data: one block at a time
        do {
            status = sd_write_block(sd_card_p, buffer, SPI_START_BLK_MUL_WRITE, _block_size);
            buffer += _block_size;
        } while (--blockCnt && SD_BLOCK_DEVICE_ERROR_NONE == status);  // Send all blocks of data
        /* In a Multiple Block write operation, the stop transmission will be
         * done by sending 'Stop Tran' token instead of 'Start Block' token at
         * the beginning of the next block
         */
        sd_spi_write(sd_card_p, SPI_STOP_TRAN);
    }
    /* 
    Once the programming operation is completed, the
    host must check the results of the programming
    using the SEND_STATUS command (CMD13).
    Some errors (e.g. address out of range, write
    protect violation, etc.) are detected during
    programming only. The only validation check
    performed on the data block and communicated to
    the host via the data-response token is CRC.
    */
    // Some SD cards want to be deselected between every command:
    sd_spi_deselect_pulse(sd_card_p);
    uint32_t stat = 0;
    return sd_cmd(sd_card_p, CMD13_SEND_STATUS, 0, false, &stat);
}
static int sd_write_blocks(sd_card_t *sd_card_p, const uint8_t *buffer,
                    uint64_t ulSectorNumber, uint32_t blockCnt) {
    sd_acquire(sd_card_p);
    TRACE_PRINTF("sd_write_blocks(0x%p, 0x%llx, 0x%lx)\r\n", buffer,
                 ulSectorNumber, blockCnt);
    int status = in_sd_write_blocks(sd_card_p, buffer, ulSectorNumber, blockCnt);
    sd_release(sd_card_p);
    return status;
}

/*!< Number of retries for sending CMDO */
#define SD_CMD0_GO_IDLE_STATE_RETRIES 10

static uint32_t sd_go_idle_state(sd_card_t *sd_card_p) {
    uint32_t response = R1_NO_RESPONSE;

    /* Resetting the MCU SPI master may not reset the on-board SDCard, in which
     * case when MCU power-on occurs the SDCard will resume operations as
     * though there was no reset. In this scenario the first CMD0 will
     * not be interpreted as a command and get lost. For some cards retrying
     * the command overcomes this situation. */
    for (int i = 0; i < SD_CMD0_GO_IDLE_STATE_RETRIES; i++) {   
        sd_cmd(sd_card_p, CMD0_GO_IDLE_STATE, 0x0, false, &response);
        if (R1_IDLE_STATE == response) {
            break;
        }
        sd_spi_deselect(sd_card_p);
        busy_wait_us(100 * 1000);
        sd_spi_select(sd_card_p);
    }
    return response;
}

static int sd_init_medium(sd_card_t *sd_card_p) {
    int32_t status = SD_BLOCK_DEVICE_ERROR_NONE;
    uint32_t response, arg;
    /*
    Power ON or card insersion
    After supply voltage reached above 2.2 volts,
    wait for one millisecond at least.
    Set SPI clock rate between 100 kHz and 400 kHz.
    Set DI and CS high and apply 74 or more clock pulses to SCLK.
    The card will enter its native operating mode and go ready to accept native
    command.
    */
    sd_spi_go_low_frequency(sd_card_p);
    sd_spi_send_initializing_sequence(sd_card_p);

    // The card is transitioned from SDCard mode to SPI mode by sending the CMD0
    // + CS Asserted("0")
    if (sd_go_idle_state(sd_card_p) != R1_IDLE_STATE) {
        DBG_PRINTF("No disk, or could not put SD card in to SPI idle state\r\n");
        return SD_BLOCK_DEVICE_ERROR_NO_DEVICE;
    }

    // Send CMD8, if the card rejects the command then it's probably using the
    // legacy protocol, or is a MMC, or just flat-out broken
    status = sd_cmd8(sd_card_p);
    if (SD_BLOCK_DEVICE_ERROR_NONE != status &&
        SD_BLOCK_DEVICE_ERROR_UNSUPPORTED != status) {
        return status;
    }

    if (crc_on) {
        size_t retries = 3;
        do {
            // Enable CRC
            // int sd_cmd(sd_card_t *sd_card_p, cmdSupported cmd, uint32_t arg, bool
            // isAcmd, uint32_t *resp)
            status = sd_cmd(sd_card_p, CMD59_CRC_ON_OFF, 1, false, 0);
        } while (--retries && (SD_BLOCK_DEVICE_ERROR_NONE != status));
    }

    // Read OCR - CMD58 Response contains OCR register
    if (SD_BLOCK_DEVICE_ERROR_NONE !=
        (status = sd_cmd(sd_card_p, CMD58_READ_OCR, 0x0, false, &response))) {
        return status;
    }
    // Check if card supports voltage range: 3.3V
    if (!(response & OCR_3_3V)) {
        sd_card_p->card_type = CARD_UNKNOWN;
        status = SD_BLOCK_DEVICE_ERROR_UNUSABLE;
        return status;
    }

    // HCS is set 1 for HC/XC capacity cards for ACMD41, if supported
    arg = 0x0;
    if (SDCARD_V2 == sd_card_p->card_type) {
        arg |= OCR_HCS_CCS;
    }

    /* Idle state bit in the R1 response of ACMD41 is used by the card to inform
     * the host if initialization of ACMD41 is completed. "1" indicates that the
     * card is still initializing. "0" indicates completion of initialization.
     * The host repeatedly issues ACMD41 until this bit is set to "0".
     */
    absolute_time_t timeout_time = make_timeout_time_ms(SD_COMMAND_TIMEOUT);
    do {
        status = sd_cmd(sd_card_p, ACMD41_SD_SEND_OP_COND, arg, true, &response);
    } while (response & R1_IDLE_STATE &&
             0 < absolute_time_diff_us(get_absolute_time(), timeout_time));

    // Initialization complete: ACMD41 successful
    if ((SD_BLOCK_DEVICE_ERROR_NONE != status) || (0x00 != response)) {
        sd_card_p->card_type = CARD_UNKNOWN;
        DBG_PRINTF("Timeout waiting for card\r\n");
        return status;
    }

    if (SDCARD_V2 == sd_card_p->card_type) {
        // Get the card capacity CCS: CMD58
        if (SD_BLOCK_DEVICE_ERROR_NONE ==
            (status = sd_cmd(sd_card_p, CMD58_READ_OCR, 0x0, false, &response))) {
            // High Capacity card
            if (response & OCR_HCS_CCS) {
                sd_card_p->card_type = SDCARD_V2HC;
                DBG_PRINTF("Card Initialized: High Capacity Card\r\n");
            } else {
                DBG_PRINTF(
                    "Card Initialized: Standard Capacity Card: Version 2.x\r\n");
            }
        }
    } else {
        sd_card_p->card_type = SDCARD_V1;
        DBG_PRINTF("Card Initialized: Version 1.x Card\r\n");
    }

    if (!crc_on) {
        // Disable CRC
        status = sd_cmd(sd_card_p, CMD59_CRC_ON_OFF, 0, false, 0);  
    }

    return status;
}

static bool sd_spi_test_com(sd_card_t *sd_card_p) {
    // This is allowed to be called before initialization, so ensure mutex is created
    if (!mutex_is_initialized(&sd_card_p->mutex)) 
        mutex_init(&sd_card_p->mutex);

    sd_acquire(sd_card_p);

    bool success = false;

    if (!(sd_card_p->m_Status & STA_NOINIT)) {
        // SD card is currently initialized

        // Timeout of 0 means only check once
        if (sd_wait_ready(sd_card_p, 0)) {
            // DO has been released, try to get status
            uint32_t response;
            for (int i = 0; i < SD_COMMAND_RETRIES; i++) {
                // Send command over SPI interface
                response = sd_cmd_spi(sd_card_p, CMD13_SEND_STATUS, 0);
                if (R1_NO_RESPONSE != response) {
                    // Got a response!
                    success = true;
                    break;
                }
            }

            if (!success) {
                // Card no longer sensed - ensure card is initialized once re-attached
                sd_card_p->m_Status |= STA_NOINIT;
            }
        } else {
            // SD card is currently holding DO which is sufficient enough to know it's still there
            success = true;
        }
    } else {
        // Do a "light" version of init, just enough to test com

        // Initialize the member variables
        sd_card_p->card_type = SDCARD_NONE;

        sd_spi_go_low_frequency(sd_card_p);
        sd_spi_send_initializing_sequence(sd_card_p);

        if (sd_wait_ready(sd_card_p, 0)) {
            // DO has been released, try to make SD card go idle
            uint32_t response;
            for (int i = 0; i < SD_COMMAND_RETRIES; i++) {
                // Send command over SPI interface
                response = sd_cmd_spi(sd_card_p, CMD0_GO_IDLE_STATE, 0);
                if (R1_NO_RESPONSE != response) {
                    // Got a response!
                    success = true;
                    break;
                }
            }
        } else {
            // Something is holding DO - better to return false and allow user to try again later
            success = false;
        }
    }

    sd_release(sd_card_p);

    return success;
}

int sd_init(sd_card_t *sd_card_p) {
    TRACE_PRINTF("> %s\r\n", __FUNCTION__);

    //	STA_NOINIT = 0x01, /* Drive not initialized */
    //	STA_NODISK = 0x02, /* No medium in the drive */
    //	STA_PROTECT = 0x04 /* Write protected */

    if (!mutex_is_initialized(&sd_card_p->mutex)) 
        mutex_init(&sd_card_p->mutex);
    sd_lock(sd_card_p);

    // Make sure there's a card in the socket before proceeding
    sd_card_detect(sd_card_p);
    if (sd_card_p->m_Status & STA_NODISK) {
        sd_unlock(sd_card_p);
        return sd_card_p->m_Status;
    }
    // Make sure we're not already initialized before proceeding
    if (!(sd_card_p->m_Status & STA_NOINIT)) {
        sd_unlock(sd_card_p);
        return sd_card_p->m_Status;
    }
    // Initialize the member variables
    sd_card_p->card_type = SDCARD_NONE;

    sd_spi_acquire(sd_card_p);

    int err = sd_init_medium(sd_card_p);
    if (SD_BLOCK_DEVICE_ERROR_NONE != err) {
        DBG_PRINTF("Failed to initialize card\r\n");
        sd_release(sd_card_p);
        return sd_card_p->m_Status;
    }
    DBG_PRINTF("SD card initialized\r\n");

    sd_card_p->sectors = in_sd_spi_sectors(sd_card_p);
    if (0 == sd_card_p->sectors) {
        // CMD9 failed
        sd_release(sd_card_p);
        return sd_card_p->m_Status;
    }
    // CMD10, Response R2 (R1 byte + 16-byte block read)
    if (sd_cmd(sd_card_p, CMD10_SEND_CID, 0x0, false, 0) != 0x0) {
        DBG_PRINTF("Didn't get a response from the disk\r\n");
        sd_release(sd_card_p);
        return sd_card_p->m_Status;
    }
    if (sd_read_bytes(sd_card_p, (uint8_t *)&sd_card_p->cid, sizeof(cid_t)) != 0) {
        DBG_PRINTF("Couldn't read CID response from disk\r\n");
        sd_release(sd_card_p);
        return sd_card_p->m_Status;
    }

    // Set block length to 512 (CMD16)
    if (sd_cmd(sd_card_p, CMD16_SET_BLOCKLEN, _block_size, false, 0) != 0) {
        DBG_PRINTF("Set %" PRIu32 "-byte block timed out\r\n", _block_size);
        sd_release(sd_card_p);
        return sd_card_p->m_Status;
    }
    sd_spi_deselect(sd_card_p);

    // Set SCK for data transfer
    sd_spi_go_high_frequency(sd_card_p);

    // The card is now initialized
    sd_card_p->m_Status &= ~STA_NOINIT;

    sd_release(sd_card_p);

    // Return the disk status
    return sd_card_p->m_Status;
}

void sd_spi_ctor(sd_card_t *sd_card_p) {
    assert(sd_card_p->spi_if_p); // Must have an interface object

    // State variables:
    sd_card_p->m_Status = STA_NOINIT;
    sd_card_p->write_blocks = sd_write_blocks;
    sd_card_p->read_blocks = sd_read_blocks;
    sd_card_p->init = sd_init;
    sd_card_p->get_num_sectors = sd_spi_sectors;
    sd_card_p->sd_test_com = sd_spi_test_com;

    if (sd_card_p->spi_if_p->set_drive_strength) {
        gpio_set_drive_strength(sd_card_p->spi_if_p->ss_gpio, sd_card_p->spi_if_p->ss_gpio_drive_strength);
    }
    // Chip select is active-low, so we'll initialise it to a
    // driven-high state.
    gpio_init(sd_card_p->spi_if_p->ss_gpio);
    gpio_put(sd_card_p->spi_if_p->ss_gpio, 1);  // Avoid any glitches when enabling output
    gpio_set_dir(sd_card_p->spi_if_p->ss_gpio, GPIO_OUT);
    gpio_put(sd_card_p->spi_if_p->ss_gpio, 1);  // In case set_dir does anything
}