/*
 * This file is part of baseflight
 * Licensed under GPL V3 or modified DCL - see https://github.com/multiwii/baseflight/blob/master/README.md
 */

#include "board.h"
#include "mw.h"
#include "config.h"

// Multiwii Serial Protocol 0
#define MSP_STATUS               101    //out message         cycletime & errors_count & sensor present & box activation & current setting number
#define MSP_RAW_IMU              102    //out message         9 DOF
#define MSP_MOTOR                104    //out message         8 motors
#define MSP_RC                   105    //out message         8 rc chan and more
#define MSP_ATTITUDE             108    //out message         2 angles 1 heading
#define MSP_ALTITUDE             109    //out message         altitude, variometer

#define MSP_PX4FLOW              125
#define MSP_LIDARLITE            126

#define MSP_SET_RAW_RC           200    //in message          8 rc chan
#define MSP_SET_MOTOR            214    //in message          PropBalance function


// Additional private MSP for baseflight configurator
#define MSP_REBOOT               68     //in message          reboot settings
#define MSP_BUILDINFO            69     //out message         build date as well as some space for future expansion

#define INBUF_SIZE 128

extern serialPort_t *  Serial1;

// from mixer.c
extern int16_t motor_disarmed[4];
// cause reboot after MSP processing complete
static bool pendReboot = false;

typedef enum serialState_t {
    IDLE,
    HEADER_START,
    HEADER_M,
    HEADER_ARROW,
    HEADER_SIZE,
    HEADER_CMD,
} serialState_t;

typedef  struct mspPortState_t {
    serialPort_t *port;
    uint8_t checksum;
    uint8_t indRX;
    uint8_t inBuf[INBUF_SIZE];
    uint8_t cmdMSP;
    uint8_t offset;
    uint8_t dataSize;
    serialState_t c_state;
} mspPortState_t;

static mspPortState_t ports[2];
static mspPortState_t *currentPortState = &ports[0];

static bool rxMspFrameDone = false;

static void mspFrameReceive(void)
{
    rxMspFrameDone = true;
}

static void serialize8(uint8_t a)
{
    serialWrite(currentPortState->port, a);
    currentPortState->checksum ^= a;
}

static void serialize16(int16_t a)
{
    serialize8(a & 0xFF);
    serialize8((a >> 8) & 0xFF);
}

static void serialize32(uint32_t a)
{
    serialize8(a & 0xFF);
    serialize8((a >> 8) & 0xFF);
    serialize8((a >> 16) & 0xFF);
    serialize8((a >> 24) & 0xFF);
}

static uint8_t read8(void)
{
    return currentPortState->inBuf[currentPortState->indRX++] & 0xff;
}

static uint16_t read16(void)
{
    uint16_t t = read8();
    t += (uint16_t)read8() << 8;
    return t;
}

/*
static uint32_t read32(void)
{
    uint32_t t = read16();
    t += (uint32_t)read16() << 16;
    return t;
}
*/

static void headSerialResponse(uint8_t err, uint8_t s)
{
    serialize8('$');
    serialize8('M');
    serialize8(err ? '!' : '>');
    currentPortState->checksum = 0;               // start calculating a new checksum
    serialize8(s);
    serialize8(currentPortState->cmdMSP);
}

static void headSerialReply(uint8_t s)
{
    headSerialResponse(0, s);
}

static void headSerialError(uint8_t s)
{
    headSerialResponse(1, s);
}

static void tailSerialReply(void)
{
    serialize8(currentPortState->checksum);
}

static void s_struct(uint8_t *cb, uint8_t siz)
{
    headSerialReply(siz);
    while (siz--)
        serialize8(*cb++);
}

static void evaluateCommand(void)
{
    uint32_t i;
    const char *build = __DATE__;

    switch (currentPortState->cmdMSP) {

        case MSP_SET_RAW_RC:
            for (i = 0; i < 8; i++)
                rcData[i] = read16();
            headSerialReply(0);
            mspFrameReceive();
            break;

        case MSP_SET_MOTOR:
            for (i = 0; i < 4; i++)
                motor_disarmed[i] = read16();
            headSerialReply(0);
            break;

        case MSP_STATUS:
            headSerialReply(11);
            serialize16(cycleTime);
            serialize16(i2cGetErrorCounter());
            serialize16(0);
           serialize8(0);
            break;

        case MSP_RAW_IMU:
            headSerialReply(18);
            // Retarded hack until multiwiidorks start using real units for sensor data
            if (acc_1G > 1024) {
                for (i = 0; i < 3; i++)
                    serialize16(accSmooth[i] / 8);
            } else {
                for (i = 0; i < 3; i++)
                    serialize16(accSmooth[i]);
            }
            for (i = 0; i < 3; i++)
                serialize16(gyroData[i]);
            for (i = 0; i < 3; i++)
                serialize16(magADC[i]);
            break;

        case MSP_MOTOR:
            s_struct((uint8_t *)motor, 16);
            break;

        case MSP_RC:
            headSerialReply(16);
            for (i = 0; i < 8; i++)
                serialize16(rcData[i]);
            break;

        case MSP_ATTITUDE:
            headSerialReply(6);
            for (i = 0; i < 2; i++)
                serialize16(angle[i]);
            serialize16(heading);
            break;

        case MSP_ALTITUDE:
            headSerialReply(6);
            serialize32(EstAlt);
            serialize16(vario);
            break;

        case MSP_REBOOT:
            headSerialReply(0);
            pendReboot = true;
            break;

        case MSP_BUILDINFO:
            headSerialReply(11 + 4 + 4);
            for (i = 0; i < 11; i++)
                serialize8(build[i]); // MMM DD YYYY as ascii, MMM = Jan/Feb... etc
            serialize32(0); // future exp
            serialize32(0); // future exp
            break;

        default:                   // we do not know how to handle the (valid) message, indicate error MSP $M!
            headSerialError(0);
            break;
    }
    tailSerialReply();
}

// ================================================================================================================

void serialInit()
{
    ports[0].port = Serial1;
}


void serialCom(void)
{
    uint8_t c;

    currentPortState = &ports[0];

    if (pendReboot)
        systemReset(false); // noreturn

    while (serialTotalBytesWaiting(currentPortState->port)) {
        c = serialRead(currentPortState->port);

        if (currentPortState->c_state == IDLE) {
            currentPortState->c_state = (c == '$') ? HEADER_START : IDLE;
            if (currentPortState->c_state == IDLE && !armed) {
                if (c == '#')
                    ;
                else if (c == CONFIG_REBOOT_CHARACTER) 
                    systemReset(true);      // reboot to bootloader
            }
        } else if (currentPortState->c_state == HEADER_START) {
            currentPortState->c_state = (c == 'M') ? HEADER_M : IDLE;
        } else if (currentPortState->c_state == HEADER_M) {
            currentPortState->c_state = (c == '<') ? HEADER_ARROW : IDLE;
        } else if (currentPortState->c_state == HEADER_ARROW) {
            if (c > INBUF_SIZE) {       // now we are expecting the payload size
                currentPortState->c_state = IDLE;
                continue;
            }
            currentPortState->dataSize = c;
            currentPortState->offset = 0;
            currentPortState->checksum = 0;
            currentPortState->indRX = 0;
            currentPortState->checksum ^= c;
            currentPortState->c_state = HEADER_SIZE;      // the command is to follow
        } else if (currentPortState->c_state == HEADER_SIZE) {
            currentPortState->cmdMSP = c;
            currentPortState->checksum ^= c;
            currentPortState->c_state = HEADER_CMD;
        } else if (currentPortState->c_state == HEADER_CMD && 
                currentPortState->offset < currentPortState->dataSize) {
            currentPortState->checksum ^= c;
            currentPortState->inBuf[currentPortState->offset++] = c;
        } else if (currentPortState->c_state == HEADER_CMD && 
                currentPortState->offset >= currentPortState->dataSize) {
            if (currentPortState->checksum == c) {        // compare calculated and transferred checksum
                evaluateCommand();      // we got a valid packet, evaluate it
            }
            currentPortState->c_state = IDLE;
        }
    }
}