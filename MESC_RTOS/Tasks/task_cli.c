/*
 **
 ******************************************************************************
 * @file           : task_cli.c
 * @brief          : IO-Task for TTerm
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; Copyright (c) 2022 Jens Kerrinnes.
 * All rights reserved.</center></h2>
 *
 * This software component is licensed under BSD 3-Clause license,
 * the "License"; You may not use this file except in compliance with the
 * License. You may obtain a copy of the License at:
 *                        opensource.org/licenses/BSD-3-Clause
 ******************************************************************************
 *In addition to the usual 3 BSD clauses, it is explicitly noted that you
 *do NOT have the right to take sections of this code for other projects
 *without attribution and credit to the source. Specifically, if you copy into
 *copyleft licenced code without attribution and retention of the permissive BSD
 *3 clause licence, you grant a perpetual licence to do the same regarding turning sections of your code
 *permissive, and lose any rights to use of this code previously granted or assumed.
 *
 *This code is intended to remain permissively licensed wherever it goes,
 *maintaining the freedom to distribute compiled binaries WITHOUT a requirement to supply source.
 *
 *This is to ensure this code can at any point be used commercially, on products that may require
 *such restriction to meet regulatory requirements, or to avoid damage to hardware, or to ensure
 *warranties can reasonably be honoured.
 ******************************************************************************/

#include "main.h"
#include "task_cli.h"
#include <string.h>
#include "cmsis_os.h"
#include "TTerm/Core/include/TTerm.h"
#include <stdio.h>
#include "stdarg.h"
#include "init.h"
#include "MESCinterface.h"



uint32_t flash_clear(void * address, uint32_t len){
	FLASH_WaitForLastOperation(500);
	HAL_FLASH_Unlock();
	FLASH_WaitForLastOperation(500);
	eraseFlash((uint32_t)address, len);
	HAL_FLASH_Lock();
	FLASH_WaitForLastOperation(500);
	return len;
}

uint32_t flash_start_write(void * address, void * data, uint32_t len){
	uint8_t * buffer = data;
	FLASH_WaitForLastOperation(500);
	HAL_FLASH_Unlock();
	FLASH_WaitForLastOperation(500);
	uint32_t written=0;
	while(len){
		if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, (uint32_t)address, *buffer)==HAL_OK){
			written++;
		}
		buffer++;
		address++;
		len--;
	}
	return written;
}

uint32_t flash_write(void * address, void * data, uint32_t len){
	uint8_t * buffer = data;
	uint32_t written=0;
	while(len){
		if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, (uint32_t)address, *buffer)==HAL_OK){
			written++;
		}
		buffer++;
		address++;
		len--;
	}
	return written;
}

uint32_t flash_end_write(void * address, void * data, uint32_t len){
	uint8_t * buffer = data;
	uint32_t written=0;
	while(len){
		if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, (uint32_t)address, *buffer)==HAL_OK){
			written++;
		}
		buffer++;
		address++;
		len--;
	}
	FLASH_WaitForLastOperation(500);
	HAL_FLASH_Lock();
	FLASH_WaitForLastOperation(500);
	return written;
}



void putbuffer_uart(unsigned char *buf, unsigned int len, port_str * port){
	UART_HandleTypeDef *uart_handle = port->hw;


	if(port->half_duplex){
		uart_handle->Instance->CR1 &= ~USART_CR1_RE;
		vTaskDelay(1);
	}
	HAL_UART_Transmit_DMA(uart_handle, buf, len);
	vTaskDelay(1);
	while(uart_handle->gState == HAL_UART_STATE_BUSY_TX){
		vTaskDelay(1);
	}

	if(port->half_duplex) uart_handle->Instance->CR1 |= USART_CR1_RE;
	xSemaphoreGive(port->tx_semaphore);
}
volatile bool cmplt = false;

void USB_CDC_TransmitCplt(){
	cmplt = true;
}

void putbuffer_usb(unsigned char *buf, unsigned int len, port_str * port){
#ifdef MESC_UART_USB
	xSemaphoreTake(port->tx_semaphore, portMAX_DELAY);
	while(CDC_Transmit_FS((uint8_t*)buf, len)== USBD_BUSY){
		vTaskDelay(1);
	}
	xSemaphoreGive(port->tx_semaphore);
#endif
}


void putbuffer_can(unsigned char *buf, unsigned int len, port_str * port){
#ifdef HAL_CAN_MODULE_ENABLED
	xSemaphoreTake(port->tx_semaphore, portMAX_DELAY);
	while(len){
		uint32_t TxMailbox;
		if(HAL_CAN_GetTxMailboxesFreeLevel(port->hw)){
			uint32_t transmit_bytes = len>8 ? 8 : len;

			CAN_TxHeaderTypeDef TxHeader;
			TxHeader.StdId = CAN_ID_1;
			TxHeader.ExtId = 0x01;
			TxHeader.RTR = CAN_RTR_DATA;
			TxHeader.IDE = CAN_ID_STD;
			TxHeader.DLC = transmit_bytes;
			TxHeader.TransmitGlobalTime = DISABLE;

			HAL_StatusTypeDef ret = HAL_CAN_AddTxMessage(port->hw, &TxHeader, buf, &TxMailbox);  //function to add message for transmition
			if(ret== HAL_OK){
				len -= transmit_bytes;
				buf += transmit_bytes;
			}
		}

		vTaskDelay(1);
	}
	xSemaphoreGive(port->tx_semaphore);
#endif
}

void putbuffer(unsigned char *buf, unsigned int len, port_str * port){

	switch(port->hw_type){
	case HW_TYPE_UART:
		putbuffer_uart(buf, len, port);
		break;
	case HW_TYPE_USB:
		putbuffer_usb(buf, len, port);
		break;
	case HW_TYPE_CAN:
		putbuffer_can(buf, len, port);
		break;

	}
}

static void uart_init(port_str * port){
	UART_HandleTypeDef *uart_handle = port->hw;

	HAL_UART_MspInit(uart_handle);
	if(port->half_duplex){
		HAL_HalfDuplex_Init(uart_handle);
	}
	HAL_UART_Receive_DMA(uart_handle, port->rx_buffer, port->rx_buffer_size);
	CLEAR_BIT(uart_handle->Instance->CR3, USART_CR3_EIE);
}

static uint32_t uart_get_write_pos(port_str * port){

	UART_HandleTypeDef *uart_handle = port->hw;
	return ( ((uint32_t)port->rx_buffer_size - __HAL_DMA_GET_COUNTER(uart_handle->hdmarx)) & ((uint32_t)port->rx_buffer_size -1));
}


void ext_printf(port_str * port, const char* format, ...) {
	va_list arg;
	va_start (arg, format);

	if(format != NULL){
		int len;
		char send_buffer[128];
		len = vsnprintf(send_buffer, 128, format, arg);
		if(len > sizeof(send_buffer)){
			len = sizeof(send_buffer);
		}

		if(len > 0) {
			putbuffer((unsigned char*)send_buffer, len, port);
		}
	}else{
		char *s = va_arg(arg, char*);
		int len = va_arg(arg, int);
		putbuffer((unsigned char*)s, len, port);
	}

	va_end (arg);


}

StreamBufferHandle_t rx_stream;

void USB_CDC_Callback(uint8_t *buffer, uint32_t len){
	xStreamBufferSendFromISR(rx_stream, buffer, len, NULL);
}



void CLI_init_can(port_str * port){
#ifdef HAL_CAN_MODULE_ENABLED
	CAN_FilterTypeDef sFilterConfig; //declare CAN filter structure

	sFilterConfig.FilterBank = 0;
	sFilterConfig.FilterMode = CAN_FILTERMODE_IDMASK;
	sFilterConfig.FilterScale = CAN_FILTERSCALE_32BIT;
	sFilterConfig.FilterIdHigh = 0x0000;
	sFilterConfig.FilterIdLow = 0x0000;
	sFilterConfig.FilterMaskIdHigh = 0x0000;
	sFilterConfig.FilterMaskIdLow = 0x0000;
	sFilterConfig.FilterFIFOAssignment = CAN_RX_FIFO0;
	sFilterConfig.FilterActivation = ENABLE;
	sFilterConfig.SlaveStartFilterBank = 14;
	if (HAL_CAN_ConfigFilter(port->hw, &sFilterConfig) != HAL_OK)
	{
	/* Filter configuration Error */
	Error_Handler();
	}


	HAL_CAN_Start(port->hw); //start CAN
#endif
}


volatile TERMINAL_HANDLE * debug;

void task_cli(void * argument)
{
	uint32_t rd_ptr=0;
	port_str * port = (port_str*) argument;
	uint8_t c;

	switch(port->hw_type){
		case HW_TYPE_UART:
			port->rx_buffer = pvPortMalloc(port->rx_buffer_size);
			uart_init(port);
			break;
		case HW_TYPE_USB:
			rx_stream = xStreamBufferCreate(port->rx_buffer_size, 1);
			break;
		case HW_TYPE_CAN:
			CLI_init_can(port);
			break;
	}

	port->tx_semaphore = xSemaphoreCreateBinary();
	xSemaphoreGive(port->tx_semaphore);

	port->term_block = xSemaphoreCreateBinary();
	xSemaphoreGive(port->term_block);

	TERMINAL_HANDLE * term_cli = NULL;

	switch(port->hw_type){
		case HW_TYPE_UART:
			term_cli =  TERM_createNewHandle(ext_printf, port, pdTRUE, &TERM_defaultList, NULL, "uart");
			break;
		case HW_TYPE_USB:
			term_cli =  TERM_createNewHandle(ext_printf, port, pdTRUE, &TERM_defaultList, NULL, "usb");
			debug = term_cli;
			break;
		case HW_TYPE_CAN:
			term_cli =  TERM_createNewHandle(ext_printf, port, pdTRUE, &TERM_defaultList, NULL, "CAN");
			break;
	}

	if(term_cli != NULL){
		null_handle.varHandle = TERM_VAR_init(term_cli, (uint8_t*)getFlashBaseAddress(), getFlashBaseSize(), flash_clear, flash_start_write, flash_write, flash_end_write);
	}

	MESCinterface_init(term_cli);

	if(port->hw_type == HW_TYPE_UART){
		rd_ptr = uart_get_write_pos(port); //Clear input buffer.
	}

	//tcp_serv_init();

  /* Infinite loop */
	for(;;)
	{

		/* `#START TASK_LOOP_CODE` */
		switch(port->hw_type){
			case HW_TYPE_UART:
				while(rd_ptr != uart_get_write_pos(port)) {
					xSemaphoreTake(port->term_block, portMAX_DELAY);
					TERM_processBuffer(&port->rx_buffer[rd_ptr],1,term_cli);
					xSemaphoreGive(port->term_block);
					rd_ptr++;
					rd_ptr &= ((uint32_t)port->rx_buffer_size - 1);
				}
				break;
			case HW_TYPE_USB:
				while(xStreamBufferReceive(rx_stream, &c, 1, 1)){
					xSemaphoreTake(port->term_block, portMAX_DELAY);
					TERM_processBuffer(&c,1,term_cli);
					xSemaphoreGive(port->term_block);
				}
			case HW_TYPE_CAN:
#ifdef HAL_CAN_MODULE_ENABLED
				xSemaphoreTake(port->term_block, portMAX_DELAY);
				while(HAL_CAN_GetRxFifoFillLevel(port->hw, CAN_RX_FIFO0)){
					CAN_RxHeaderTypeDef pheader;
					uint8_t buffer[8];
					HAL_CAN_GetRxMessage(port->hw, CAN_RX_FIFO0, &pheader, buffer);
					if(pheader.StdId == CAN_ID_1){
						TERM_processBuffer(buffer,pheader.DLC,term_cli);
					}

				}
				xSemaphoreGive(port->term_block);
#endif
				break;
			break;
		}

		if(ulTaskNotifyTake(pdTRUE, 1)){
			HAL_UART_MspDeInit(port->hw);
			port->task_handle = NULL;
			vPortFree(port->rx_buffer);
			vTaskDelete(NULL);
			vTaskDelay(portMAX_DELAY);
		}

	}
}

void task_cli_init(port_str * port){
	if(port->task_handle == NULL){
		xTaskCreate(task_cli, "tskCLI", 1024, (void*)port, osPriorityNormal, &port->task_handle);
	}
}

void task_cli_kill(port_str * port){
	if(port->task_handle){
		xTaskNotify(port->task_handle, 0, eIncrement);
		vTaskDelay(200);
	}
}
