/*
 * dsp_uart2.c
 *
 *  Created on: 2018-12-8
 *      Author: zhtro
 */

#include "psc.h"
#include "TL6748.h"
#include "uart.h"
#include <xdc/std.h>
#include "soc_C6748.h"               // DSP C6748 外设寄存器


// 时钟
#define SYSCLK_1_FREQ     (456000000)
#define SYSCLK_2_FREQ     (SYSCLK_1_FREQ/2)
#define UART_2_FREQ       (SYSCLK_2_FREQ)

/****************************************************************************/
/*                                                                          */
/*              PSC 初始化                                                  */
/*                                                                          */
/****************************************************************************/
static void PSCInit(void)
{
    // 使能 UART2 模块
    PSCModuleControl(SOC_PSC_1_REGS, HW_PSC_UART2, PSC_POWERDOMAIN_ALWAYS_ON,PSC_MDCTL_NEXT_ENABLE);
}

/****************************************************************************/
/*                                                                          */
/*              GPIO 管脚复用配置                                           */
/*                                                                          */
/****************************************************************************/
static void GPIOBankPinMuxSet(void)
{
	// UART2 禁用流控
	UARTPinMuxSetup(2, FALSE);
}
/****************************************************************************/
/*                                                                          */
/*              UART 初始化                                                 */
/*                                                                          */
/****************************************************************************/
static void UARTInit(void)
{
	// 配置 UART2 参数
	// 波特率 115200 数据位 8 停止位 1 无校验位
    UARTConfigSetExpClk(SOC_UART_2_REGS, UART_2_FREQ, BAUD_115200,
    		              UART_WORDL_8BITS, UART_OVER_SAMP_RATE_16);
    
    UARTDisable(SOC_UART_2_REGS);
    
	// 使能 UART2
	UARTEnable(SOC_UART_2_REGS);

    // 使能接收 / 发送 FIFO
    UARTFIFOEnable(SOC_UART_2_REGS);

    // 设置 FIFO 级别
    UARTFIFOLevelSet(SOC_UART_2_REGS, UART_RX_TRIG_LEVEL_1);
}

/****************************************************************************/
/*                                                                          */
/*              UART 中断初始化                                             */
/*                                                                          */
/****************************************************************************/
static void UARTInterruptInit(void)
{
	// 使能中断
	unsigned int intFlags = 0;
    int delayCnt = 1000;
    intFlags |= (UART_INT_LINE_STAT  |  \
                 UART_INT_RXDATA_CTI);
    UARTIntDisable(SOC_UART_2_REGS, intFlags);
    while(delayCnt--);
    UARTIntEnable(SOC_UART_2_REGS, intFlags);
}

void dsp_uart2_init()
{
	// 外设使能配置
	PSCInit();

    // 管脚复用配置
    GPIOBankPinMuxSet();

	// UART 初始化
	UARTInit();

	// UART 中断初始化
	UARTInterruptInit();
}

unsigned int UART2Puts(char *pTxBuffer, int numBytesToWrite)
{
     unsigned int count = 0;
     unsigned int flag = 0;

     if(numBytesToWrite < 0)
     {
          flag = 1;
     }

     while('\0' != *pTxBuffer)
     {
          if('\n' == *pTxBuffer)
          {
               UARTCharPut(SOC_UART_2_REGS, '\r');
               UARTCharPut(SOC_UART_2_REGS, '\n');
          }
          else
          {
              UARTCharPut(SOC_UART_2_REGS, (unsigned char)*pTxBuffer);
          }
          pTxBuffer++;
          count++;

          if((0 == flag) && (count == numBytesToWrite))
          {
               break;
          }

     }

   return count;
}

unsigned int UART2Send(const char *pcBuf, unsigned int len)
{
    unsigned int uIdx;

    /* Send the characters */
    for(uIdx = 0; uIdx < len; uIdx++)
    {
        /* Send the character to the UART output. */
       UARTCharPut(SOC_UART_2_REGS,pcBuf[uIdx]);
    }

    /* Return the number of characters written. */
    return(uIdx);
}

unsigned char UART2Getc(void)
{
    return ((unsigned char)UARTCharGet(SOC_UART_2_REGS));
}

