#ifndef __MYIIC_H
#define __MYIIC_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>

void MyIIC_Init(void);
void MyIIC_DeInit(void);

void MyIIC_Write_SingleByte(uint8_t SlaveAddress, uint8_t REG_Address, uint8_t REG_data);
uint8_t MyIIC_Read_SingleByte(uint8_t SlaveAddress, uint8_t REG_Address);
void MyIIC_Write_MultiBytes(uint8_t SlaveAddress, uint8_t REG_Address, int BytesNum, uint8_t *buf);
uint8_t MyIIC_Read_MultiBytes(uint8_t SlaveAddress, uint8_t REG_Address, int BytesNum, uint8_t *buf);
uint8_t MyIIC_Read_Write(uint8_t addr, uint16_t rw, uint8_t *buf, int read_size);
#endif
