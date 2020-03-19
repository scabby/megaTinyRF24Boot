#include <SPI.h>
#include "src/stk500.h"
#include "nRF24L01.h"
#include <string.h>

static const uint16_t VERSION = 0x900;

struct nrfPacket
{
	uint8_t magic0, magic1;
	uint8_t cmd;
	uint8_t addresslo;
	uint8_t addresshi;
	union
	{
		uint8_t numpackets;
		uint8_t eepromvalue;
	};
	uint8_t pad[26];
};
nrfPacket packet = { 0x99, 0x88 };

uint8_t nrf24_status();
uint8_t nrf24_command(uint8_t cmd, uint8_t data = NOP);
uint8_t nrf24_begin(uint8_t cmd);
void nrf24_end();
void nrf24_set_tx_address(const uint8_t address[3]);
inline void nrf24_write_register(uint8_t reg, uint8_t data) { nrf24_command(reg | W_REGISTER, data); }
inline uint8_t nrf24_read_register(uint8_t reg) { return nrf24_command(reg, 0); }
inline uint8_t nrf24_status()
{
	return nrf24_read_register(RF_STATUS);
}
uint8_t nrf24_command(uint8_t cmd, uint8_t data)
{
	nrf24_begin(cmd);
	data = SPI.transfer(data);
	nrf24_end();
	return data;
}
uint8_t nrf24_begin(uint8_t cmd)
{
	digitalWrite(PIN_PB0, LOW);
	return SPI.transfer(cmd);
}
inline void nrf24_end()
{
	digitalWrite(PIN_PB0, HIGH);
}
inline bool nrf24_rx_available()
{
	return !(nrf24_read_register(FIFO_STATUS) & _BV(RX_EMPTY));
}
void nrf24_set_address(uint8_t reg, const uint8_t address[3])
{
	nrf24_begin(W_REGISTER | reg);
	SPI.transfer(address[0]);
	SPI.transfer(address[1]);
	SPI.transfer(address[2]);
	nrf24_end();
}
void nrf24_set_tx_address(const uint8_t address[3])
{
	nrf24_set_address(TX_ADDR, address);
	nrf24_set_address(RX_ADDR_P0, address);
}
void nrf24_init()
{
	SPI.begin();
	digitalWrite(PIN_PB0, HIGH);
	delay(5);
	nrf24_write_register(CONFIG, 0);
	//nrf24_write_register(NRF_STATUS, 0xFF);
	nrf24_write_register(EN_AA, 0x3F);
	nrf24_write_register(SETUP_AW, 1);
	nrf24_write_register(RF_CH, 76);
	nrf24_write_register(SETUP_RETR, 0x7F);
	nrf24_write_register(RF_SETUP, (1 << RF_PWR_LOW) | (1 << RF_PWR_HIGH) | (1 << RF_DR_HIGH));
	//nrf24_write_register(RF_SETUP, (1 << RF_PWR_LOW) | (1 << RF_PWR_HIGH));
	nrf24_write_register(DYNPD, 0);
	nrf24_write_register(FEATURE, 0);
	nrf24_write_register(EN_RXADDR, 1);
	nrf24_command(FLUSH_TX);
	nrf24_command(FLUSH_RX);
}
void nrf24_begin_tx()
{
	nrf24_write_register(CONFIG, (1 << MASK_RX_DR) | (1 << MASK_TX_DS) | (1 << MASK_MAX_RT) | (1 << CRCO) | (1 << EN_CRC) | (1 << PWR_UP));
	//delay(50);
	delay(5);
}
void nrf24_begin_rx()
{
	nrf24_write_register(CONFIG, (1 << MASK_RX_DR) | (1 << MASK_TX_DS) | (1 << MASK_MAX_RT) | (1 << CRCO) | (1 << EN_CRC) | (1 << PWR_UP) | (1 << PRIM_RX));
}
bool nrf24_tx(const void* data, uint8_t len)
{
	uint8_t status;
	const uint8_t* addr = (const uint8_t*) data;
	for (;;)
	{
		status = nrf24_status();
		if (!(status & _BV(TX_FULL)))
			break;
		if (status & _BV(MAX_RT))
		{
			nrf24_write_register(RF_STATUS, status);
			nrf24_command(FLUSH_TX, 0);
			return false;
		}
	}
	nrf24_begin(W_TX_PAYLOAD);
	while (len--)
		SPI.transfer(*addr++);
	nrf24_end();
	return true;
}
bool nrf24_tx_end()
{
	for (;;)
	{
		if (nrf24_read_register(FIFO_STATUS) & _BV(TX_EMPTY))
			return true;
		uint8_t status = nrf24_status();
		if (status & _BV(MAX_RT))
		{
			nrf24_write_register(RF_STATUS, status);
			nrf24_command(FLUSH_TX, 0);
			return false;
		}
	}
}

uint8_t progAddress [] = { 'P','0','1' };
uint8_t uartAddress [] = { 'U','0','1' };

uint16_t startT;
int getch()
{
	while (!Serial.available() && ((uint16_t)millis()) - startT < 1000);
	return Serial.read();
}
inline void putch(int ch)
{
	while (!Serial.availableForWrite());
	Serial.write(ch);
}

bool verifySpace()
{
	bool insync = getch() == CRC_EOP;
	putch(insync ? STK_INSYNC : STK_NOSYNC);
	return insync;
}

uint8_t serialbuf[32];
uint8_t serialbufpos = 0;
uint8_t stk500mode;
uint16_t lastSendTime = 0;

enum eMode
{
	MODE_UART,
	MODE_STK500,
	MODE_CONFIGURE,
};
eMode gMode = MODE_UART;

void OpenUart()
{
	gMode = MODE_UART;
	nrf24_write_register(CONFIG, 0);
	//delay(5);
	nrf24_write_register(FEATURE, (1 << EN_DPL));
	nrf24_write_register(DYNPD, 1);
	nrf24_set_tx_address(uartAddress);
	nrf24_begin_rx();
	serialbufpos = 0;
}

uint8_t OpenStk500()
{
	nrf24_write_register(CONFIG, 0);
	//delay(50);
	nrf24_write_register(FEATURE, 0);
	nrf24_write_register(DYNPD, 0);
	nrf24_set_tx_address(progAddress);
	nrf24_begin_tx();

	packet.cmd = 'F';
	packet.addresshi = 0x34; // SRAM
	packet.addresslo = 0x00;
	packet.numpackets = 0x00;
	// wait for 4 sync packets to be received.  Up to 3 can fit in
	// the receivers FIFO so only with 4 can we be sure the bootloader
	// has actually started pulling them out of the FIFO.
	uint8_t retries = 0;
	uint8_t successes = 0;
	for(;;)
	{
		if (nrf24_tx(&packet, 32) && nrf24_tx_end())
		{
			if (++successes == 4)
				break;
		}
		else
		{
			if (++retries == 20)
				break;
			delay(50);
		}
	}
	return successes;
}

bool RespondToStk500Sync()
{
	lastSendTime = millis();
	serialbufpos = 0;
	putch(STK_INSYNC);
	putch(STK_OK);
	putch(STK_INSYNC);
	if (OpenStk500() == 4)
	{
		putch(STK_OK);
		gMode = MODE_STK500;
	}
	else
	{
		putch(STK_FAILED);
		OpenUart();
	}
}

void PrintAddresses()
{
	uint8_t a0 = uartAddress[1];
	uint8_t a1 = uartAddress[2];
	Serial.printf("UART addr = %02x%02x%02x  Programming addr = %02x%02x%02x\n", 'U', a0, a1, 'P', a0, a1);
}

void OpenConfig()
{
	gMode = MODE_CONFIGURE;
	Serial.write("\nConfigure STK500-nRF24L01+ interface\n");
	PrintAddresses();
	Serial.write("\n addr <xyz>   - set address of target device\n");
	Serial.write(" setid <xyz>  - reprogram target device's listen address\n");
	Serial.write(" r            - reset target device\n>");
	serialbufpos = 0;
	while (Serial.read() >= 0);
}

uint8_t MatchSerialCommand(const char* seq, uint8_t len)
{
	if (len > serialbufpos)
		len = serialbufpos;
	for (; len > 0; --len)
		if (memcmp(&serialbuf[serialbufpos - len], seq, len) == 0)
			break;
	return len;
}

void HandleUart()
{
	uint16_t t = millis();
	uint8_t stk500match = 0;

	if (serialbufpos < 32 && Serial.available())
		serialbuf[serialbufpos++] = Serial.read();

	stk500match = MatchSerialCommand("0 0 ", 4);
	if (stk500match == 4)
	{
		RespondToStk500Sync();
		return;
	}
	if (!stk500match) // don't talk back on serial if STK500 is being initiated
	{
		while (nrf24_rx_available())
		{
			uint8_t size = nrf24_command(R_RX_PL_WID);
			nrf24_begin(R_RX_PAYLOAD);
			while (size--)
				putch(SPI.transfer(0));
			nrf24_end();
			delay(5);
		}
	}

	uint8_t idcmd = MatchSerialCommand("*cfg\n", 4) + MatchSerialCommand("*cfg\r", 4);
	if (idcmd == 4)
	{
		OpenConfig();
		return;
	}

	if (serialbufpos == 32 || (serialbufpos > 0 && t - lastSendTime > 100 && !stk500match && !idcmd))
	{
		nrf24_write_register(CONFIG, 0);
		nrf24_begin_tx();
		nrf24_tx(serialbuf, serialbufpos);
		nrf24_tx_end();
		nrf24_begin_rx();
		serialbufpos = 0;
	}
	if (serialbufpos == 0)
	{
		lastSendTime = t;
	}
}

void HandleStk500()
{
	if (!Serial.available())
		return;

	uint8_t tmp[32];
	bool failed = false;
	bool finished = false;

	startT = millis();

	switch (Serial.read())
	{
	case STK_GET_SYNC:
	{
		if (!verifySpace())
			return;
		packet.cmd = 'F';
		packet.addresshi = 0x34; // SRAM
		packet.addresslo = 0x00;
		packet.numpackets = 0x00;
		if (!nrf24_tx(&packet, 32) || !nrf24_tx_end())
			failed = true;
		break;
	}
	case STK_GET_PARAMETER:
	{
		uint8_t which = getch();
		if (!verifySpace())
			return;

		/*
		 * Send optiboot version as "SW version"
		 * Note that the references to memory are optimized away.
		 */
		if (which == STK_SW_MINOR)
		{
			putch(VERSION & 0xFF);
		}
		else if (which == STK_SW_MAJOR)
		{
			putch(VERSION >> 8);
		}
		else
		{
			/*
				* GET PARAMETER returns a generic 0x03 reply for
				* other parameters - enough to keep Avrdude happy
				*/
			putch(0x03);
		}
		break;
	}
	case STK_SET_DEVICE:
	{
		// SET DEVICE is ignored
		for (uint8_t i = 0; i < 20; ++i)
			getch();
		if (!verifySpace())
			return;
		break;
	}
	case STK_SET_DEVICE_EXT:
	{
		for (uint8_t i = 0; i < 5; ++i)
			getch();
		if (!verifySpace())
			return;
		break;
	}
	case STK_LOAD_ADDRESS:
	{
		packet.addresslo = getch();
		packet.addresshi = getch();
		if (!verifySpace())
			return;
		break;
	}
	case STK_UNIVERSAL:
	{
#ifndef RAMPZ
		// UNIVERSAL command is ignored
		for (uint8_t i = 0; i < 4; ++i)
			getch();
		if (!verifySpace())
			return;
		putch('\0');
#endif
		break;
	}
	/* Write memory, length is big endian and is in bytes */
	case STK_PROG_PAGE:
	{
		// PROGRAM PAGE - any kind of page!
		int16_t length = getch() << 8;
		length |= getch();
		uint8_t desttype = getch();

		if (desttype == 'F')
		{
			packet.cmd = 'F';
			packet.addresshi += 0x80;
			while (length > 0)
			{
				packet.numpackets = (length + 31) / 32;
				if (!failed && !nrf24_tx(&packet, 32))
					failed = true;
				for (uint8_t i = 0; i < packet.numpackets; ++i)
				{
					uint8_t j = 0;
					for (; j < 32 && length > 0; ++j, --length)
						tmp[j] = getch();
					for (; j < 32; ++j)
						tmp[j] = 0;
					if (!failed && !nrf24_tx(tmp, 32))
						failed = true;
				}
				packet.addresslo += 64;
				if (packet.addresslo < 64)
					++packet.addresshi;				
			}
		}
		else
		{
			packet.cmd = 'E';
			packet.addresshi += 0x14;
			while (length--)
			{
				packet.eepromvalue = getch();
				if (!failed && !nrf24_tx(&packet, 32))
					failed = true;
				++packet.addresslo;
			}
		}
		if (!nrf24_tx_end())
			failed = true;
		// Read command terminator, start reply
		if (!verifySpace())
			return;
		break;
	}
	/* Read memory block mode, length is big endian.  */
	case STK_READ_PAGE:
	{
		int16_t length = getch() << 8;
		length |= getch();
		uint8_t desttype = getch();
		if (!verifySpace())
			return;
		//failed = true;
		do {
			putch(0xFF);
		} while (--length);
		break;
	}
	/* Get device signature bytes  */
	case STK_READ_SIGN:
	{
		// READ SIGN - return what Avrdude wants to hear
		if (!verifySpace())
			return;
		putch(SIGROW_DEVICEID0);
		putch(SIGROW_DEVICEID1);
		putch(SIGROW_DEVICEID2);
		break;
	}
	case STK_LEAVE_PROGMODE:
	{
		if (!verifySpace())
			return;
		packet.cmd = 'R';
		if (!nrf24_tx(&packet, 32) || !nrf24_tx_end())
			failed = true;
		
		finished = true;
		break;
	}
	default:
	{
		// This covers the response to commands like STK_ENTER_PROGMODE
		if (!verifySpace())
			return;
		break;
	}
	}
	putch(failed ? STK_FAILED : STK_OK);
	uint16_t t = millis();
	if (failed || finished || t - lastSendTime > 5000)
	{
		Serial.flush();
		OpenUart();
	}
	else
	{
		lastSendTime = t;
	}
}

bool ResetDevice()
{
	uint8_t success = OpenStk500();
	if (success >= 4)
	{
		Serial.println("Device reset successfully");
		return true;
	}
	else
	{
		Serial.println("Error communicating with device");
		Serial.printf("%i packets transmitted successfully\n", success);
		return false;
	}
}

void HandleConfigure()
{
	if (!Serial.available())
		return;
	char ch = Serial.read();
	Serial.write(ch);
	if (ch == '\r')
		return;
	if (ch == '\n')
		ch = 0;
	if (serialbufpos >= sizeof(serialbuf))
		--serialbufpos;
	serialbuf[serialbufpos++] = ch;
	if (MatchSerialCommand("0 0 ", 4) == 4)
	{
		RespondToStk500Sync();
		return;
	}
	if (ch != 0)
		return;
	if (serialbufpos == 1 || serialbuf[0] == 'q')
	{
		Serial.println("done.");
		OpenUart();
		return;
	}
	if (serialbuf[0] == 'r')
	{
		ResetDevice();
	}
	else if (memcmp(serialbuf, "addr ", 5) == 0)
	{
		uartAddress[1] = progAddress[1] = serialbuf[6];
		uartAddress[2] = progAddress[2] = serialbuf[7];
		PrintAddresses();
		ResetDevice();
	}
	else if (memcmp(serialbuf, "setid ", 6) == 0)
	{
		if (ResetDevice())
		{
			bool failed = false;
			// reprogram the user signature area with new address
			packet.cmd = 'E';
			packet.addresshi = 0x13; 
			for (uint8_t i = 0; i < 3; ++i)
			{
				packet.addresslo = i;
				packet.eepromvalue = serialbuf[i + 6];
				if (!failed && !nrf24_tx(&packet, 32))
					failed = true;
			}
			// exit from the bootloader
			packet.cmd = 'R'; 
			if (!failed && !nrf24_tx(&packet, 32)) 
				failed = true;
			// trigger a reset back into the bootloader to reconfigure the radio address
			packet.cmd = 'F';
			packet.addresshi = 0x34;
			packet.addresslo = 0x00;
			packet.numpackets = 0x00;
			if (!failed && nrf24_tx(&packet, 32) && nrf24_tx_end())
			{
				// address updated successfully
				uartAddress[1] = progAddress[1] = serialbuf[7];
				uartAddress[2] = progAddress[2] = serialbuf[8];
				PrintAddresses();
			}
			// re-establish connection on new address
			ResetDevice();
		}
		else
		{
			Serial.println("Error communicating with target device");
		}
	}
	serialbufpos = 0;
	Serial.write(">");
	Serial.flush();
}

void setup()
{
	pinMode(PIN_PB0, OUTPUT);
	pinMode(PIN_PB1, OUTPUT);
	digitalWrite(PIN_PB1, HIGH);
	Serial.begin(500000);	
	nrf24_init();
	while (nrf24_read_register(RF_SETUP) != ((1 << RF_PWR_LOW) | (1 << RF_PWR_HIGH) | (1 << RF_DR_HIGH)))
	{
		Serial.println("radio not connected");
		nrf24_init();
		delay(500);
	}
	OpenUart();
}

void loop()
{
	switch (gMode)
	{
	case MODE_STK500: HandleStk500(); break;
	case MODE_UART: HandleUart(); break;
	case MODE_CONFIGURE: HandleConfigure(); break;
	}
}