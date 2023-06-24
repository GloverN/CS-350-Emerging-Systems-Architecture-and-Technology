/*
 * Copyright (c) 2015-2020, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  ======== gpiointerrupt.c ========
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Driver Header files */
#include <ti/drivers/GPIO.h>
#include <ti/drivers/UART.h>
#include <ti/drivers/I2C.h>
#include <ti/drivers/Timer.h>

/* Driver configuration */
#include "ti_drivers_config.h"

#define DISPLAY(x) UART_write(uart, &output, x);

/*
 *  ======== Global Variables ========
 */
//   == UART Variables ==
char output[64];
int bytesToSend;

//   == I2C Variables ==
static const struct {
    uint8_t address;
    uint8_t resultReg;
char *id;
} sensors[3] = {
    { 0x48, 0x0000, "11X" },
    { 0x49, 0x0000, "116" },
    { 0x41, 0x0001, "006" }
};
uint8_t txBuffer[1];
uint8_t rxBuffer[2];
I2C_Transaction i2cTransaction;

//   == Program Flags & Variables ==
volatile unsigned char TimerFlag = 0;
volatile unsigned char IncrFlag = 0;
volatile unsigned char DecrFlag = 0;
volatile int16_t RoomTemp = 0;
volatile int16_t SetTemp = 0;
volatile unsigned char Heating = 0;
volatile int Duration = 0;

//   == Driver Handles ==
UART_Handle uart;
I2C_Handle i2c;
Timer_Handle timer0;

/*
 *  ======== Task Structure & Variables ========
 */
typedef struct task {
    unsigned long period;      // Rate at which the task should tick
    unsigned long elapsedTime; // Time since task's previous tick
    void (*TickFct)();          // Function to call for task's tick
} task;

task tasks[3];

const unsigned char tasksNum = 3;
const unsigned long tasksPeriodGCD = 100;
const unsigned long periodTempRead = 500;
const unsigned long periodTempManip = 200;
const unsigned long periodTempReport = 1000;

// ==== Tasks ====
void TickFct_Scheduler();
void TickFct_TempRead();
void TickFct_TempManip();
void TickFct_TempReport();

/*
 *  ======== initUART ========
 *  Initialization function for the UART drivers
 */
void initUART(void)
{
    UART_Params uartParams;

    // Init the driver
    UART_init();

    // Configure the driver
    UART_Params_init(&uartParams);
    uartParams.writeDataMode = UART_DATA_BINARY;
    uartParams.readDataMode = UART_DATA_BINARY;
    uartParams.readReturnMode = UART_RETURN_FULL;
    uartParams.baudRate = 115200;

    // Open the driver
    uart = UART_open(CONFIG_UART_0, &uartParams);
    if (uart == NULL) {
        /* UART_open() failed */
        while (1);
    }
}

/*
 *  ======== initI2C ========
 *  Initialization function for the I2C drivers and temp sensor
 */
void initI2C(void)
{
    int8_t i, found;
    I2C_Params i2cParams;

    DISPLAY(snprintf(output, 64, "Initializing I2C Driver - "))

    // Init the driver
    I2C_init();

    // Configure the driver
    I2C_Params_init(&i2cParams);
    i2cParams.bitRate = I2C_400kHz;

    // Open the driver
    i2c = I2C_open(CONFIG_I2C_0, &i2cParams);
    if (i2c == NULL)
    {
        DISPLAY(snprintf(output, 64, "Failed\n\r"))
        while (1);
    }

    DISPLAY(snprintf(output, 32, "Passed\n\r"))

    // Boards were shipped with different sensors.
    // Welcome to the world of embedded systems.
    // Try to determine which sensor we have.
    // Scan through the possible sensor addresses

    /* Common I2C transaction setup */
    i2cTransaction.writeBuf = txBuffer;
    i2cTransaction.writeCount = 1;
    i2cTransaction.readBuf = rxBuffer;
    i2cTransaction.readCount = 0;

    found = false;
    for (i=0; i<3; ++i)
    {
        i2cTransaction.slaveAddress = sensors[i].address;
        txBuffer[0] = sensors[i].resultReg;

        DISPLAY(snprintf(output, 64, "Is this %s? ", sensors[i].id))
        if (I2C_transfer(i2c, &i2cTransaction))
        {
            DISPLAY(snprintf(output, 64, "Found\n\r"))
            found = true;
            break;
        }

        DISPLAY(snprintf(output, 64, "No\n\r"))
    }

    if(found)
    {
        DISPLAY(snprintf(output, 64, "Detected TMP%s I2C address:%x\n\r",
                         sensors[i].id, i2cTransaction.slaveAddress))
    }
    else
    {
        DISPLAY(snprintf(output, 64, "Temperature sensor not found, "
                         "contact professor\n\r"))
    }
}

/*
 *  ======== readTemp ========
 *  Reads the temperature from the temperature sensor and returns it
 */
int16_t readTemp(void)
{
    int16_t temperature = 0;

    i2cTransaction.readCount = 2;
    if (I2C_transfer(i2c, &i2cTransaction))
    {
        /*
         * Extract degrees C from the received data;
         * see TMP sensor datasheet
         */
        temperature = (rxBuffer[0] << 8) | (rxBuffer[1]);
        temperature *= 0.0078125;

        /*
         * If the MSB is set '1', then we have a 2's complement
         * negative value which needs to be sign extended
         */
        if (rxBuffer[0] & 0x80)
        {
            temperature |= 0xF000;
        }
    }
    else
    {
        DISPLAY(snprintf(output, 64, "Error reading temperature sensor(%d)\n\r",
                         i2cTransaction.status))
        DISPLAY(snprintf(output, 64, "Please power cycle your board by "
                         "unplugging USB and plugging back in.\n\r"))
    }

    return temperature;
}

/*
 *  ======== timerCallback ========
 *  Callback function for the Timer interrupt
 */
void timerCallback(Timer_Handle myHandle, int_fast16_t status)
{
    TimerFlag = 1;
}

/*
 *  ======== initTimer ========
 *  Initialization function for the timer
 */
void initTimer(void)
{
    Timer_Params params;

    // Init the driver
    Timer_init();

    // Configure the driver
    Timer_Params_init(&params);
    params.period = 100000;
    params.periodUnits = Timer_PERIOD_US;
    params.timerMode = Timer_CONTINUOUS_CALLBACK;
    params.timerCallback = timerCallback;

    // Open the driver
    timer0 = Timer_open(CONFIG_TIMER_0, &params);

    if (timer0 == NULL) {
        /* Failed to initialized timer */
        while (1) {}
    }

    if (Timer_start(timer0) == Timer_STATUS_ERROR) {
        /* Failed to start timer */
        while (1) {}
    }
}

/*
 *  ======== gpioButtonFxn0 ========
 *  Callback function for the GPIO interrupt on CONFIG_GPIO_BUTTON_0.
 *
 *  Note: GPIO interrupts are cleared prior to invoking callbacks.
 */
void gpioButtonFxn0(uint_least8_t index)
{
    /* Raises the global DecrFlag variable */
    DecrFlag = 1;
}

/*
 *  ======== gpioButtonFxn1 ========
 *  Callback function for the GPIO interrupt on CONFIG_GPIO_BUTTON_1.
 *  This may not be used for all boards.
 *
 *  Note: GPIO interrupts are cleared prior to invoking callbacks.
 */
void gpioButtonFxn1(uint_least8_t index)
{
    /* Raises the global IncrFlag variable */
    IncrFlag = 1;
}

/*
 *  ======== mainThread ========
 */
void *mainThread(void *arg0)
{
    /* Call driver init functions */
    GPIO_init();
    initUART();
    initI2C();
    initTimer();

    /* Configure the LED and button pins */
    GPIO_setConfig(CONFIG_GPIO_LED_0, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_setConfig(CONFIG_GPIO_BUTTON_0, GPIO_CFG_IN_PU | GPIO_CFG_IN_INT_FALLING);
    GPIO_setConfig(CONFIG_GPIO_BUTTON_1, GPIO_CFG_IN_PU | GPIO_CFG_IN_INT_FALLING);

    /* Install Button callbacks */
    GPIO_setCallback(CONFIG_GPIO_BUTTON_0, gpioButtonFxn0);
    GPIO_setCallback(CONFIG_GPIO_BUTTON_1, gpioButtonFxn1);

    /* Enable interrupts */
    GPIO_enableInt(CONFIG_GPIO_BUTTON_0);
    GPIO_enableInt(CONFIG_GPIO_BUTTON_1);

    /* Initialize the values of the Task Scheduler */
    unsigned char i = 0;
    tasks[i].period = periodTempRead;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_TempRead;
    i++;
    tasks[i].period = periodTempManip;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_TempManip;
    i++;
    tasks[i].period = periodTempReport;
    tasks[i].elapsedTime = tasks[i].period;
    tasks[i].TickFct = &TickFct_TempReport;

    while (1) {
        TickFct_Scheduler();

        while (!TimerFlag);  // Wait for timer to trigger
        TimerFlag = 0;       // Lower TimerFlag
    }
}

/*
 *  ======== TickFct_Scheduler ========
 *  This function organizes the task functions and makes sure that they
 *  all execute at the proper times.
 *
 *  The button flags will be checked every 200 ms and the value
 *     of SetTemp will be updated if necessary.
 *  The room temperature is checked every 500 ms and the value
 *     of RoomTemp will be updated.
 *  The state of the system will be reported every 1000 ms
 *     through the status of the LED and the message
 *     printed to the terminal.
 */
void TickFct_Scheduler() {
    unsigned char i;
    for (i = 0; i < tasksNum; i++) {  // Heart of the scheduler code
        if (tasks[i].elapsedTime >= tasks[i].period) {
            tasks[i].TickFct();
            tasks[i].elapsedTime = 0;
        }

        tasks[i].elapsedTime += tasksPeriodGCD;
    }
}

/*
 *  ======== TickFct_TempRead ========
 *  This function reads the temperature from the temp sensor and updates
 *  the global variable RoomTemp accordingly.
 */
void TickFct_TempRead() {
    RoomTemp = readTemp();
}

/*
 *  ======== TickFct_TempManip ========
 *  This function checks if the IncrFlag and DecrFlag flags have been
 *  raised, increments or decrements the global variable SetTemp accordingly,
 *  and then lowers the flag.
 *
 *  Note: This function also ensures the value of SetTemp remains clamped
 *        between 0-99 degrees
 */
void TickFct_TempManip() {
    if (IncrFlag) {
        if (SetTemp < 99)
            SetTemp++;
        IncrFlag = 0;
    }
    if (DecrFlag) {
        if (SetTemp > 0)
            SetTemp--;
        DecrFlag = 0;
    }
}

/*
 *  ======== TickFct_TempReport ========
 *  This function checks if the global variable RoomTemp is less than
 *  or greater than the global variable SetTemp. If SetTemp is greater,
 *  the function turns on the LED that simulates a heater, and if
 *  RoomTemp is greater, the function turns the LED off. After that,
 *  it will print the values of RoomTemp, SetTemp, Heating, and
 *  Duration to the Terminal in this format:
 *  <AA,BB,S,CCCC>
 *
 *  Once it checks the values, this function will update the value of
 *  the global variable Duration, which represents how many seconds
 *  since the last board reset.
 */
void TickFct_TempReport() {
    if (RoomTemp < SetTemp) {
        Heating = 1;
        GPIO_write(CONFIG_GPIO_LED_0, CONFIG_GPIO_LED_ON);
    }
    else if (RoomTemp >= SetTemp) {
        Heating = 0;
        GPIO_write(CONFIG_GPIO_LED_0, CONFIG_GPIO_LED_OFF);
    }

    DISPLAY(snprintf(output, 64, "<%02d,%02d,%d,%04d>\n\r", RoomTemp, SetTemp, Heating, Duration))

    Duration++;
}
