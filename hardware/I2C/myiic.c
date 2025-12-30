#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include "myiic.h"
#include "i2c.h"

static i2c_t *i2c_handles = NULL;
static int i2c_file = 0;
const char *i2c_node = "/dev/i2c-0";

void MyIIC_Init(void)
{
	i2c_handles = i2c_new();
	int error = i2c_open(i2c_handles, i2c_node);
	if (error) 
	{
		fprintf(stderr, "i2c_open(): %s\n", i2c_errmsg(i2c_handles));
		i2c_free(i2c_handles);
		i2c_handles = NULL;
	}
	else
	{
		i2c_file = i2c_fd(i2c_handles);
	}
}

void MyIIC_DeInit(void)
{
	if(i2c_handles == NULL)
		return;
	i2c_close(i2c_handles);
	i2c_free(i2c_handles);
}

void MyIIC_Write_SingleByte(uint8_t SlaveAddress, uint8_t REG_Address, uint8_t REG_data)
{
	struct i2c_msg msgs;
	uint8_t send[2];

	send[0] = REG_Address;
	send[1] = REG_data;

	msgs.addr = SlaveAddress;
	msgs.flags = 0; // Write
	msgs.len = 2;
	msgs.buf = send;
	i2c_transfer(i2c_handles, &msgs, 1);
}

uint8_t MyIIC_Read_SingleByte(uint8_t SlaveAddress, uint8_t REG_Address)
{
	struct i2c_msg msgs[2];
	uint8_t byte = 0;

	msgs[0].addr = SlaveAddress;
	msgs[0].flags = 0; // Write
	msgs[0].len = 1;
	msgs[0].buf = &REG_Address;

	msgs[1].addr = SlaveAddress;
	msgs[1].flags = 1; // Read
	msgs[1].len = 1;
	msgs[1].buf = &byte;

	i2c_transfer(i2c_handles, msgs, 2);

	return byte;
}

void MyIIC_Write_MultiBytes(uint8_t SlaveAddress, uint8_t REG_Address, int BytesNum, uint8_t *buf)
{
    struct i2c_msg msgs;
	uint8_t send[BytesNum + 1];

	send[0] = REG_Address;
	memcpy(&send[1], buf, BytesNum);

	msgs.addr = SlaveAddress;
	msgs.flags = 0; // Write
	msgs.len = BytesNum + 1;
	msgs.buf = send;
	i2c_transfer(i2c_handles, &msgs, 1);
}

uint8_t MyIIC_Read_MultiBytes(uint8_t SlaveAddress, uint8_t REG_Address, int BytesNum, uint8_t *buf)
{
    struct i2c_msg msgs[2];

	msgs[0].addr = SlaveAddress;
	msgs[0].flags = 0; // Write
	msgs[0].len = 1;
	msgs[0].buf = &REG_Address;

	// i2c_transfer(i2c_handles, &msgs[0], 1);

	msgs[1].addr = SlaveAddress;
	msgs[1].flags = 1; // Read
	msgs[1].len = BytesNum;
	msgs[1].buf = buf;

	i2c_transfer(i2c_handles, &msgs[0], 2);
	
	return 1;
}

uint8_t MyIIC_Read_Write(uint8_t addr, uint16_t rw, uint8_t *buf, int read_size)
{
	struct i2c_msg msgs;
	uint8_t send[2];

	send[0] = (rw >> 8) & 0xff;
	send[1] = rw & 0xff;

	msgs.addr = addr;
	msgs.flags = 0; // Write
	msgs.len = 2;
	msgs.buf = send;
	i2c_transfer(i2c_handles, &msgs, 1);

	msgs.addr = addr;
	msgs.flags = 1; // Read
	msgs.len = read_size;
	msgs.buf = buf;
	i2c_transfer(i2c_handles, &msgs, 1);
	return 1;
}

// u8 MPU_Write_Len(uint8_t addr, uint8_t reg, uint8_t len, uint8_t *buf) {
//     struct i2c_rdwr_ioctl_data packets;
//     struct i2c_msg messages[1];
//     uint8_t data[256];

//     if (len + 1 > 256) {
//         return -1;  // Buffer overflow
//     }

//     // Prepare the data buffer
//     data[0] = reg;
//     memcpy(data + 1, buf, len);

//     // Set up the I2C message
//     messages[0].addr  = addr;
//     messages[0].flags = 0;
//     messages[0].len   = len + 1;
//     messages[0].buf   = data;

//     packets.msgs      = messages;
//     packets.nmsgs     = 1;

//     // Perform the I2C transaction
//     if (ioctl(i2c_file, I2C_RDWR, &packets) < 0) {
//         perror("Failed to write to the i2c bus");
//         return 1;
//     }

//     return 0;
// }

// // Function to read multiple bytes from a device
// u8 MPU_Read_Len(uint8_t addr, uint8_t reg, uint8_t len, uint8_t *buf) {
//     struct i2c_rdwr_ioctl_data packets;
//     struct i2c_msg messages[2];

//     // Set up the I2C messages
//     messages[0].addr  = addr;
//     messages[0].flags = 0;
//     messages[0].len   = 1;
//     messages[0].buf   = &reg;

//     messages[1].addr  = addr;
//     messages[1].flags = I2C_M_RD;
//     messages[1].len   = len;
//     messages[1].buf   = buf;

//     packets.msgs      = messages;
//     packets.nmsgs     = 2;

//     // Perform the I2C transaction
//     if (ioctl(i2c_file, I2C_RDWR, &packets) < 0) {
//         perror("Failed to read from the i2c bus");
//         return 1;
//     }

//     return 0;
// }

// // Function to write a single byte to a device
// u8 MPU_Write_Byte(uint8_t reg, uint8_t data) {
//     return MPU_Write_Len(MPU_ADDR, reg, 1, &data);
// }

// // Function to read a single byte from a device
// u8 MPU_Read_Byte(uint8_t reg) {
//     uint8_t data;
//     if (MPU_Read_Len(MPU_ADDR, reg, 1, &data) != 0) {
//         return -1;  // Read failed
//     }
//     return data;
// }