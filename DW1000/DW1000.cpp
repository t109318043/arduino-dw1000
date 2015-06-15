/*
 * Copyright (c) 2015 by Thomas Trojer <thomas@trojer.net>
 * Decawave DW1000 library for Arduino.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "pins_arduino.h"
#include "DW1000.h"

DW1000Class DW1000;

/* ###########################################################################
 * #### Static member variables ##############################################
 * ######################################################################### */

// pins
unsigned int DW1000Class::_ss;
unsigned int DW1000Class::_rst;
unsigned int DW1000Class::_irq;
// IRQ callbacks
void (*DW1000Class::_handleSent)(void) = 0;
void (*DW1000Class::_handleReceived)(void) = 0;
void (*DW1000Class::_handleReceiveError)(void) = 0;
void (*DW1000Class::_handleReceiveTimeout)(void) = 0;
void (*DW1000Class::_handleReceiveTimestampAvailable)(void) = 0;
// message printing
char DW1000Class::_msgBuf[1024];
// registers
byte DW1000Class::_syscfg[LEN_SYS_CFG];
byte DW1000Class::_sysctrl[LEN_SYS_CTRL];
byte DW1000Class::_sysstatus[LEN_SYS_STATUS];
byte DW1000Class::_txfctrl[LEN_TX_FCTRL];
byte DW1000Class::_sysmask[LEN_SYS_MASK];
byte DW1000Class::_chanctrl[LEN_CHAN_CTRL];
byte DW1000Class::_networkAndAddress[LEN_PANADR];
// driver internal state
byte DW1000Class::_extendedFrameLength;
byte DW1000Class::_pacSize;
byte DW1000Class::_pulseFrequency;
byte DW1000Class::_dataRate;
byte DW1000Class::_preambleLength;
byte DW1000Class::_preambleCode;
byte DW1000Class::_channel;
boolean DW1000Class::_permanentReceive = false;
int DW1000Class::_deviceMode = IDLE_MODE;

/* ###########################################################################
 * #### Init and end #######################################################
 * ######################################################################### */

void DW1000Class::end() {
	SPI.end();
}

void DW1000Class::select(int ss) {
	_ss = ss;
	pinMode(_ss, OUTPUT);
	digitalWrite(_ss, HIGH);
}

void DW1000Class::begin(int ss, int rst, int irq) {
	select(ss);
	begin(rst, irq);
}

void DW1000Class::begin(int rst, int irq) {
	// SPI setup
	SPI.begin();
	SPI.setBitOrder(MSBFIRST);
	SPI.setDataMode(SPI_MODE0);
	// TODO increase clock speed after chip clock lock (CPLOCK in 0x0f)
	SPI.setClockDivider(SPI_CLOCK_DIV8);
	// pin and basic member setup
	_rst = rst;
	_irq = irq;
	_deviceMode = IDLE_MODE;
	_extendedFrameLength = false;
	pinMode(_rst, OUTPUT);
	digitalWrite(_rst, HIGH);
	// reset chip
	reset();
	// default network and node id
	writeValueToBytes(_networkAndAddress, 0xFF, LEN_PANADR);
	writeNetworkIdAndDeviceAddress();
	// default system configuration
	memset(_syscfg, 0, LEN_SYS_CFG);
	setDoubleBuffering(false);
	setInterruptPolarity(true);
	writeSystemConfigurationRegister();
	// default interrupt mask, i.e. no interrupts
	clearInterrupts();
	writeSystemEventMaskRegister();
	// tell the chip to load the LDE microcode
	byte pmscctrl0[LEN_PMSC_CTRL0];
	byte otpctrl[LEN_OTP_CTRL];
	writeValueToBytes(otpctrl, 0x8000, LEN_OTP_CTRL);
	writeValueToBytes(pmscctrl0, 0x0301, LEN_PMSC_CTRL0);
	writeBytes(PMSC_CTRL0, NO_SUB, pmscctrl0, LEN_PMSC_CTRL0);
	writeBytes(OTP_CTRL, OTP_CTRL_SUB, otpctrl, LEN_OTP_CTRL);
	delay(10);
	writeValueToBytes(pmscctrl0, 0x0200, LEN_PMSC_CTRL0);
	writeBytes(PMSC_CTRL0, NO_SUB, pmscctrl0, LEN_PMSC_CTRL0);
	tune();
	delay(10);
	// attach interrupt
	attachInterrupt(_irq, DW1000Class::handleInterrupt, RISING);
}

void DW1000Class::reset() {
	digitalWrite(_rst, LOW);
	delay(10);
	digitalWrite(_rst, HIGH);
	delay(10);
	// force into idle mode (although it should be already after reset)
	idle();
}

void DW1000Class::tune() {
	// these registers are going to be tuned/configured
	byte agctune1[LEN_AGC_TUNE1];
	byte agctune2[LEN_AGC_TUNE2];
	byte agctune3[LEN_AGC_TUNE3];
	byte drxtune0b[LEN_DRX_TUNE0b];
	byte drxtune1a[LEN_DRX_TUNE1a];
	byte drxtune1b[LEN_DRX_TUNE1b];
	byte drxtune2[LEN_DRX_TUNE2];
	byte drxtune4H[LEN_DRX_TUNE4H];
	byte ldecfg1[LEN_LDE_CFG1];
	byte ldecfg2[LEN_LDE_CFG2];
	byte lderepc[LEN_LDE_REPC];
	byte txpower[LEN_TX_POWER];
	byte rfrxctrlh[LEN_RF_RXCTRLH];
	byte rftxctrl[LEN_RF_TXCTRL];
	byte tcpgdelay[LEN_TC_PGDELAY];
	byte fspllcfg[LEN_FS_PLLCFG];
	byte fsplltune[LEN_FS_PLLTUNE];
	// AGC_TUNE1
	if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
		writeValueToBytes(agctune1, 0x8870, LEN_AGC_TUNE1);
	} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
		writeValueToBytes(agctune1, 0x889B, LEN_AGC_TUNE1);
	} else {
		// TODO proper error/warning handling
	}
	// AGC_TUNE2
	writeValueToBytes(agctune2, 0x2502A907, LEN_AGC_TUNE2);
	// AGC_TUNE3
	writeValueToBytes(agctune3, 0x0035, LEN_AGC_TUNE3);
	// DRX_TUNE0b
	if(_dataRate == TRX_RATE_110KBPS) {
		writeValueToBytes(drxtune0b, 0x0016, LEN_DRX_TUNE0b);
	} else if(_dataRate == TRX_RATE_850KBPS) {
		writeValueToBytes(drxtune0b, 0x0006, LEN_DRX_TUNE0b);
	} else if(_dataRate == TRX_RATE_6800KBPS) {
		writeValueToBytes(drxtune0b, 0x0001, LEN_DRX_TUNE0b);
	} else {
		// TODO proper error/warning handling
	}
	// DRX_TUNE1a
	if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
		writeValueToBytes(drxtune1a, 0x0087, LEN_DRX_TUNE1a);
	} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
		writeValueToBytes(drxtune1a, 0x008D, LEN_DRX_TUNE1a);
	} else {
		// TODO proper error/warning handling
	}
	// DRX_TUNE1b
	if(_preambleLength ==  TX_PREAMBLE_LEN_1536 || _preambleLength ==  TX_PREAMBLE_LEN_2048 ||  
			_preambleLength ==  TX_PREAMBLE_LEN_4096) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(drxtune1b, 0x0064, LEN_DRX_TUNE1b);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_preambleLength != TX_PREAMBLE_LEN_64) {
		if(_dataRate == TRX_RATE_850KBPS || _dataRate == TRX_RATE_6800KBPS) {
			writeValueToBytes(drxtune1b, 0x0020, LEN_DRX_TUNE1b);
		} else {
			// TODO proper error/warning handling
		}
	} else {
		if(_dataRate == TRX_RATE_6800KBPS) {
			writeValueToBytes(drxtune1b, 0x0010, LEN_DRX_TUNE1b);
		} else {
			// TODO proper error/warning handling
		}
	}
	// DRX_TUNE2
	if(_pacSize == PAC_SIZE_8) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(drxtune2, 0x311A002D, LEN_DRX_TUNE2);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(drxtune2, 0x313B006B, LEN_DRX_TUNE2);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_pacSize == PAC_SIZE_16) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(drxtune2, 0x331A0052, LEN_DRX_TUNE2);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(drxtune2, 0x333B00BE, LEN_DRX_TUNE2);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_pacSize == PAC_SIZE_32) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(drxtune2, 0x351A009A, LEN_DRX_TUNE2);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(drxtune2, 0x353B015E, LEN_DRX_TUNE2);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_pacSize == PAC_SIZE_64) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(drxtune2, 0x371A011D, LEN_DRX_TUNE2);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(drxtune2, 0x373B0296, LEN_DRX_TUNE2);
		} else {
			// TODO proper error/warning handling
		}
	} else {
		// TODO proper error/warning handling
	}
	// DRX_TUNE4H
	if(_preambleLength == TX_PREAMBLE_LEN_64) {
		writeValueToBytes(drxtune4H, 0x0010, LEN_DRX_TUNE4H);
	} else {
		writeValueToBytes(drxtune4H, 0x0028, LEN_DRX_TUNE4H);
	}
	// RF_RXCTRLH
	if(_channel != CHANNEL_4 && _channel != CHANNEL_7) {
		writeValueToBytes(rfrxctrlh, 0xD8, LEN_RF_RXCTRLH);
	} else {
		writeValueToBytes(rfrxctrlh, 0xBC, LEN_RF_RXCTRLH);
	}
	// RX_TXCTRL
	if(_channel == CHANNEL_1) {
		writeValueToBytes(rftxctrl, 0x00005C40, LEN_RF_TXCTRL);
	} else if(_channel == CHANNEL_2) {
		writeValueToBytes(rftxctrl, 0x00045CA0, LEN_RF_TXCTRL);
	} else if(_channel == CHANNEL_3) {
		writeValueToBytes(rftxctrl, 0x00086CC0, LEN_RF_TXCTRL);
	} else if(_channel == CHANNEL_4) {
		writeValueToBytes(rftxctrl, 0x00045C80, LEN_RF_TXCTRL);
	} else if(_channel == CHANNEL_5) {
		writeValueToBytes(rftxctrl, 0x001E3FE0, LEN_RF_TXCTRL);
	} else if(_channel == CHANNEL_7) {
		writeValueToBytes(rftxctrl, 0x001E7DE0, LEN_RF_TXCTRL);
	} else {
		// TODO proper error/warning handling
	}
	// TC_PGDELAY
	if(_channel == CHANNEL_1) {
		writeValueToBytes(tcpgdelay, 0xC9, LEN_TC_PGDELAY);
	} else if(_channel == CHANNEL_2) {
		writeValueToBytes(tcpgdelay, 0xC2, LEN_TC_PGDELAY);
	} else if(_channel == CHANNEL_3) {
		writeValueToBytes(tcpgdelay, 0xC5, LEN_TC_PGDELAY);
	} else if(_channel == CHANNEL_4) {
		writeValueToBytes(tcpgdelay, 0x95, LEN_TC_PGDELAY);
	} else if(_channel == CHANNEL_5) {
		writeValueToBytes(tcpgdelay, 0xC0, LEN_TC_PGDELAY);
	} else if(_channel == CHANNEL_7) {
		writeValueToBytes(tcpgdelay, 0x93, LEN_TC_PGDELAY);
	} else {
		// TODO proper error/warning handling
	}
	// FS_PLLCFG and FS_PLLTUNE
	if(_channel == CHANNEL_1) {
		writeValueToBytes(fspllcfg, 0x09000407, LEN_FS_PLLCFG);
		writeValueToBytes(fsplltune, 0x1E, LEN_FS_PLLTUNE);
	} else if(_channel == CHANNEL_2 || _channel == CHANNEL_4) {
		writeValueToBytes(fspllcfg, 0x08400508, LEN_FS_PLLCFG);
		writeValueToBytes(fsplltune, 0x26, LEN_FS_PLLTUNE);
	} else if(_channel == CHANNEL_3) {
		writeValueToBytes(fspllcfg, 0x08401009, LEN_FS_PLLCFG);
		writeValueToBytes(fsplltune, 0x5E, LEN_FS_PLLTUNE);
	} else if(_channel == CHANNEL_5 || _channel == CHANNEL_7) {
		writeValueToBytes(fspllcfg, 0x0800041D, LEN_FS_PLLCFG);
		writeValueToBytes(fsplltune, 0xA6, LEN_FS_PLLTUNE);
	} else {
		// TODO proper error/warning handling
	}
	// LDE_CFG1
	writeValueToBytes(ldecfg1, 0xD, LEN_LDE_CFG1);
	// LDE_CFG2
	if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
		writeValueToBytes(ldecfg2, 0x1607, LEN_LDE_CFG2);
	} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
		writeValueToBytes(ldecfg2, 0x0607, LEN_LDE_CFG2);
	} else {
		// TODO proper error/warning handling
	}
	// LDE_REPC
	if(_preambleCode == PREAMBLE_CODE_16MHZ_1 || _preambleCode == PREAMBLE_CODE_16MHZ_2) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x5998 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x5998, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_16MHZ_3 || _preambleCode == PREAMBLE_CODE_16MHZ_8) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x51EA >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x51EA, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_16MHZ_4) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x428E >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x428E, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_16MHZ_5) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x451E >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x451E, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_16MHZ_6) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x2E14 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x2E14, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_16MHZ_7) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x8000 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x8000, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_64MHZ_9) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x28F4 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x28F4, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_64MHZ_10 || _preambleCode == PREAMBLE_CODE_64MHZ_17) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x3332 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x3332, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_64MHZ_11) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x3AE0 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x3AE0, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_64MHZ_12) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x3D70 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x3D70, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_64MHZ_18 || _preambleCode == PREAMBLE_CODE_64MHZ_19) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x35C2 >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x35C2, LEN_LDE_REPC);
		}
	} else if(_preambleCode == PREAMBLE_CODE_64MHZ_20) {
		if(_dataRate == TRX_RATE_110KBPS) {
			writeValueToBytes(lderepc, ((0x47AE >> 3) & 0xFFFF), LEN_LDE_REPC);
		} else {
			writeValueToBytes(lderepc, 0x47AE, LEN_LDE_REPC);
		}
	} else {
		// TODO proper error/warning handling
	}
	// TX_POWER (enabled smart transmit power control)
	if(_channel == CHANNEL_1 || _channel == CHANNEL_2) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(txpower, 0x15355575, LEN_TX_POWER);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(txpower, 0x07274767, LEN_TX_POWER);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_channel == CHANNEL_3) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(txpower, 0x0F2F4F6F, LEN_TX_POWER);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(txpower, 0x2B4B6B8B, LEN_TX_POWER);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_channel == CHANNEL_4) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(txpower, 0x1F1F3F5F, LEN_TX_POWER);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(txpower, 0x3A5A7A9A, LEN_TX_POWER);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_channel == CHANNEL_5) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(txpower, 0x0E082848, LEN_TX_POWER);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(txpower, 0x25456585, LEN_TX_POWER);
		} else {
			// TODO proper error/warning handling
		}
	} else if(_channel == CHANNEL_7) {
		if(_pulseFrequency == TX_PULSE_FREQ_16MHZ) {
			writeValueToBytes(txpower, 0x32527292, LEN_TX_POWER);
		} else if(_pulseFrequency == TX_PULSE_FREQ_64MHZ) {
			writeValueToBytes(txpower, 0x5171B1D1, LEN_TX_POWER);
		} else {
			// TODO proper error/warning handling
		}
	} else {
		// TODO proper error/warning handling
	}
	// write configuration back to chip
	writeBytes(AGC_TUNE, AGC_TUNE1_SUB, agctune1, LEN_AGC_TUNE1);
	writeBytes(AGC_TUNE, AGC_TUNE2_SUB, agctune2, LEN_AGC_TUNE2);
	writeBytes(AGC_TUNE, AGC_TUNE3_SUB, agctune3, LEN_AGC_TUNE3);
	writeBytes(DRX_TUNE, DRX_TUNE0b_SUB, drxtune0b, LEN_DRX_TUNE0b);
	writeBytes(DRX_TUNE, DRX_TUNE1a_SUB, drxtune1a, LEN_DRX_TUNE1a);
	writeBytes(DRX_TUNE, DRX_TUNE1b_SUB, drxtune1b, LEN_DRX_TUNE1b);
	writeBytes(DRX_TUNE, DRX_TUNE2_SUB, drxtune2, LEN_DRX_TUNE2);
	writeBytes(DRX_TUNE, DRX_TUNE4H_SUB, drxtune4H, LEN_DRX_TUNE4H);
	writeBytes(LDE_CFG, LDE_CFG1_SUB, ldecfg1, LEN_LDE_CFG1);
	writeBytes(LDE_CFG, LDE_CFG2_SUB, ldecfg2, LEN_LDE_CFG2);
	writeBytes(LDE_CFG, LDE_REPC_SUB, lderepc, LEN_LDE_REPC);
	writeBytes(TX_POWER, NO_SUB, txpower, LEN_TX_POWER);
	writeBytes(RF_CONF, RF_RXCTRLH_SUB, rfrxctrlh, LEN_RF_RXCTRLH);
	writeBytes(RF_CONF, RF_TXCTRL_SUB, rftxctrl, LEN_RF_TXCTRL);
	writeBytes(TX_CAL, TC_PGDELAY_SUB, tcpgdelay, LEN_TC_PGDELAY);
	writeBytes(FS_CTRL, FS_PLLTUNE_SUB, fsplltune, LEN_FS_PLLTUNE);
	writeBytes(FS_CTRL, FS_PLLCFG_SUB, fspllcfg, LEN_FS_PLLCFG);
	// TODO LDOTUNE, see 2.5.5, p. 21

// preamble length set
	// TODO set PAC size accordingly for RX (see table 6, page 31)
}

/* ###########################################################################
 * #### Interrupt handling ###################################################
 * ######################################################################### */

void DW1000Class::handleInterrupt() {
	// read current status and handle via callbacks
	readSystemEventStatusRegister();
	if(isTransmitDone() && _handleSent != 0) {
		(*_handleSent)();
		clearTransmitStatus();
	}
	if(isReceiveDone() && _handleReceived != 0) {
		(*_handleReceived)();
		clearReceiveStatus();
		if(_permanentReceive) {
			startReceive();
		}
	} else if(isReceiveError() && _handleReceiveError != 0) {
		(*_handleReceiveError)();
		clearReceiveStatus();
		if(_permanentReceive) {
			startReceive();
		}
	} else if(isReceiveTimeout() && _handleReceiveTimeout != 0) {
		(*_handleReceiveTimeout)();
		clearReceiveStatus();
		if(_permanentReceive) {
			startReceive();
		}
	}
	if(isReceiveTimestampAvailable() && _handleReceiveTimestampAvailable != 0) {
		(*_handleReceiveTimestampAvailable)();
		clearReceiveTimestampAvailableStatus();
	}
	// TODO impl other callbacks
}

/* ###########################################################################
 * #### Pretty printed device information ####################################
 * ######################################################################### */

char* DW1000Class::getPrintableDeviceIdentifier() {
	byte data[LEN_DEV_ID];
	readBytes(DEV_ID, NO_SUB, data, LEN_DEV_ID);
	sprintf(_msgBuf, "DECA - model: %d, version: %d, revision: %d", 
		data[1], (data[0] >> 4) & 0x0F, data[0] & 0x0F);
	return _msgBuf;
}

char* DW1000Class::getPrintableExtendedUniqueIdentifier() {
	byte data[LEN_EUI];
	readBytes(EUI, NO_SUB, data, LEN_EUI);
	sprintf(_msgBuf, "EUI: %d:%d:%d:%d:%d, OUI: %d:%d:%d",
		data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
	return _msgBuf;
}

char* DW1000Class::getPrintableNetworkIdAndShortAddress() {
	byte data[LEN_PANADR];
	readBytes(PANADR, NO_SUB, data, LEN_PANADR);
	sprintf(_msgBuf, "PAN: %u, Short Address: %u",
		(unsigned int)((data[3] << 8) | data[2]), (unsigned int)((data[1] << 8) | data[0]));
	return _msgBuf;
}

/* ###########################################################################
 * #### DW1000 register read/write ###########################################
 * ######################################################################### */

void DW1000Class::readSystemConfigurationRegister() {
	readBytes(SYS_CFG, NO_SUB, _syscfg, LEN_SYS_CFG);
}

void DW1000Class::writeSystemConfigurationRegister() {
	writeBytes(SYS_CFG, NO_SUB, _syscfg, LEN_SYS_CFG);
}

void DW1000Class::readSystemEventStatusRegister() {
	readBytes(SYS_STATUS, NO_SUB, _sysstatus, LEN_SYS_STATUS);
}

void DW1000Class::readNetworkIdAndDeviceAddress() {
	readBytes(PANADR, NO_SUB, _networkAndAddress, LEN_PANADR);
}

void DW1000Class::writeNetworkIdAndDeviceAddress() {
	writeBytes(PANADR, NO_SUB, _networkAndAddress, LEN_PANADR);
}

void DW1000Class::readSystemEventMaskRegister() {
	readBytes(SYS_MASK, NO_SUB, _sysmask, LEN_SYS_MASK);
}

void DW1000Class::writeSystemEventMaskRegister() {
	writeBytes(SYS_MASK, NO_SUB, _sysmask, LEN_SYS_MASK);
}

void DW1000Class::readChannelControlRegister() {
	readBytes(CHAN_CTRL, NO_SUB, _chanctrl, LEN_CHAN_CTRL);
}

void DW1000Class::writeChannelControlRegister() {
	writeBytes(CHAN_CTRL, NO_SUB, _chanctrl, LEN_CHAN_CTRL);
}

void DW1000Class::readTransmitFrameControlRegister() {
	readBytes(TX_FCTRL, NO_SUB, _txfctrl, LEN_TX_FCTRL);
}

void DW1000Class::writeTransmitFrameControlRegister() {
	writeBytes(TX_FCTRL, NO_SUB, _txfctrl, LEN_TX_FCTRL);
}

/* ###########################################################################
 * #### DW1000 operation functions ###########################################
 * ######################################################################### */

void DW1000Class::setNetworkId(unsigned int val) {
	_networkAndAddress[2] = (byte)(val & 0xFF);
	_networkAndAddress[3] = (byte)((val >> 8) & 0xFF);
}

void DW1000Class::setDeviceAddress(unsigned int val) {
	_networkAndAddress[0] = (byte)(val & 0xFF);
	_networkAndAddress[1] = (byte)((val >> 8) & 0xFF);
}

void DW1000Class::setFrameFilter(boolean val) {
	setBit(_syscfg, LEN_SYS_CFG, FFEN_BIT, val);
}

void DW1000Class::setDoubleBuffering(boolean val) {
	setBit(_syscfg, LEN_SYS_CFG, DIS_DRXB_BIT, !val);
}

void DW1000Class::setInterruptPolarity(boolean val) {
	setBit(_syscfg, LEN_SYS_CFG, HIRQ_POL_BIT, val);
}

void DW1000Class::setReceiverAutoReenable(boolean val) {
	setBit(_syscfg, LEN_SYS_CFG, RXAUTR_BIT, val);
}

void DW1000Class::interruptOnSent(boolean val) {
	setBit(_sysmask, LEN_SYS_MASK, TXFRS_BIT, val);
}

void DW1000Class::interruptOnReceived(boolean val) {
	setBit(_sysmask, LEN_SYS_MASK, RXDFR_BIT, val);
}

void DW1000Class::interruptOnReceiveError(boolean val) {
	setBit(_sysmask, LEN_SYS_STATUS, LDEERR_BIT, val);
	setBit(_sysmask, LEN_SYS_STATUS, RXFCE_BIT, val);
	setBit(_sysmask, LEN_SYS_STATUS, RXPHE_BIT, val);
	setBit(_sysmask, LEN_SYS_STATUS, RXRFSL_BIT, val);
}

void DW1000Class::interruptOnReceiveTimeout(boolean val) {
	setBit(_sysmask, LEN_SYS_MASK, RXRFTO_BIT, val);
}

void DW1000Class::interruptOnReceiveTimestampAvailable(boolean val) {
	setBit(_sysmask, LEN_SYS_MASK, LDEDONE_BIT, val);
}

void DW1000Class::interruptOnAutomaticAcknowledgeTrigger(boolean val) {
	setBit(_sysmask, LEN_SYS_MASK, AAT_BIT, val);
}

void DW1000Class::clearInterrupts() {
	memset(_sysmask, 0, LEN_SYS_MASK);
}

void DW1000Class::idle() {
	memset(_sysctrl, 0, LEN_SYS_CTRL);
	setBit(_sysctrl, LEN_SYS_CTRL, TRXOFF_BIT, true);
	_deviceMode = IDLE_MODE;
	writeBytes(SYS_CTRL, NO_SUB, _sysctrl, LEN_SYS_CTRL);
}

void DW1000Class::newReceive() {
	idle();
	memset(_sysctrl, 0, LEN_SYS_CTRL);
	clearReceiveStatus();
	_deviceMode = RX_MODE;
}

void DW1000Class::startReceive() {
	_sysctrl[3] |= _extendedFrameLength;
	setBit(_sysctrl, LEN_SYS_CTRL, SFCST_BIT, !_frameCheck);
	setBit(_sysctrl, LEN_SYS_CTRL, RXENAB_BIT, true);
	writeBytes(SYS_CTRL, NO_SUB, _sysctrl, LEN_SYS_CTRL);
}

void DW1000Class::newTransmit() {
	idle();
	memset(_sysctrl, 0, LEN_SYS_CTRL);
	clearTransmitStatus();
	_deviceMode = TX_MODE;
}

void DW1000Class::startTransmit() {
	_sysctrl[3] |= _extendedFrameLength;
	setBit(_sysctrl, LEN_SYS_CTRL, SFCST_BIT, !_frameCheck);
	setBit(_sysctrl, LEN_SYS_CTRL, TXSTRT_BIT, true);
	writeBytes(SYS_CTRL, NO_SUB, _sysctrl, LEN_SYS_CTRL);
	if(_permanentReceive) {
		memset(_sysctrl, 0, LEN_SYS_CTRL);
		startReceive();
	} else {
		_deviceMode = IDLE_MODE;
	}
}

void DW1000Class::newConfiguration() {
	idle();
	readNetworkIdAndDeviceAddress();
	readSystemConfigurationRegister();
	readChannelControlRegister();
	readTransmitFrameControlRegister();
	readSystemEventMaskRegister();
}

void DW1000Class::commitConfiguration() {
	writeNetworkIdAndDeviceAddress();
	writeSystemConfigurationRegister();
	writeChannelControlRegister();
	writeTransmitFrameControlRegister();
	writeSystemEventMaskRegister();
}

void DW1000Class::waitForResponse(boolean val) {
	setBit(_sysctrl, LEN_SYS_CTRL, WAIT4RESP_BIT, val);
}

void DW1000Class::suppressFrameCheck(boolean val) {
	_frameCheck = false;
}

float DW1000Class::setDelay(unsigned int value, unsigned long factorUs) {
	if(_deviceMode == TX_MODE) {
		setBit(_sysctrl, LEN_SYS_CTRL, TXDLYS_BIT, true);
	} else if(_deviceMode == RX_MODE) {
		setBit(_sysctrl, LEN_SYS_CTRL, RXDLYS_BIT, true);
	} else {
		// in idle, ignore
		return -1;
	}
	byte delayBytes[5];
	float tsValue = getSystemTimestamp() + (value * factorUs);
	tsValue = fmod(tsValue, TIME_OVERFLOW);
	writeFloatUsToTimestamp(tsValue, delayBytes);
	delayBytes[0] = 0;
	delayBytes[1] &= 0xFE;
	writeBytes(DX_TIME, NO_SUB, delayBytes, LEN_DX_TIME);
	return tsValue;
}

void DW1000Class::setDataRate(byte rate) {
	rate &= 0x03;
	if(rate >= 0x03) {
		rate = TRX_RATE_850KBPS;
	}
	_txfctrl[1] |= (byte)((rate << 5) & 0xFF);
	if(rate == TRX_RATE_110KBPS) {
		setBit(_syscfg, LEN_SYS_CFG, RXM110K_BIT, true);
	} else {
		setBit(_syscfg, LEN_SYS_CFG, RXM110K_BIT, false);
	}
	_dataRate = rate;
}

void DW1000Class::setPulseFrequency(byte freq) {
	freq &= 0x03;
	if(freq == 0x00 || freq >= 0x03) {
		freq = TX_PULSE_FREQ_16MHZ;
	}
	_txfctrl[2] |= (byte)(freq & 0xFF);
	_chanctrl[2] |= (byte)((freq << 2) & 0xFF);
	_pulseFrequency = freq;
}

void DW1000Class::setPreambleLength(byte prealen) {
	prealen &= 0x0F;
	_txfctrl[2] |= (byte)((prealen << 2) & 0xFF);
	if(prealen == TX_PREAMBLE_LEN_64 || prealen == TX_PREAMBLE_LEN_128) {
		_pacSize = PAC_SIZE_8;
	} else if(prealen == TX_PREAMBLE_LEN_256 || prealen == TX_PREAMBLE_LEN_512) {
		_pacSize = PAC_SIZE_16;
	} else if(prealen == TX_PREAMBLE_LEN_1024) {
		_pacSize = PAC_SIZE_32;
	} else {
		_pacSize = PAC_SIZE_64;
	}
	_preambleLength = prealen;
}

void DW1000Class::useExtendedFrameLength(boolean val) {
	_extendedFrameLength = (val ? FRAME_LENGTH_EXTENDED : FRAME_LENGTH_NORMAL);
}

void DW1000Class::receivePermanently(boolean val) {
	_permanentReceive = val;
	if(val) {
		// in case permanent, also reenable receiver once failed
		setReceiverAutoReenable(true);
		writeSystemConfigurationRegister();
	}
}

void DW1000Class::setChannel(byte channel) {
	_channel = channel;
	// TODO channel ctrl !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
}

void DW1000Class::setPreambleCode(byte preacode) {
	_preambleCode = preacode;
	// TODO channel ctrl !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
}

void DW1000Class::setDefaults() {
	if(_deviceMode == TX_MODE) {

	} else if(_deviceMode == RX_MODE) {

	} else if(_deviceMode == IDLE_MODE) {
		suppressFrameCheck(false);
		interruptOnSent(true);
		interruptOnReceived(true);
		writeSystemEventMaskRegister();
		interruptOnAutomaticAcknowledgeTrigger(true);
		setReceiverAutoReenable(true);
		// TODO enableMode(MODE_2) + impl all other modes !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	}
}

void DW1000Class::setData(byte data[], int n) {
	if(_frameCheck) {
		n+=2; // two bytes CRC-16
	}
	if(n > LEN_EXT_UWB_FRAMES) {
		return; // TODO proper error handling: frame/buffer size
	}
	if(n > LEN_UWB_FRAMES && !_extendedFrameLength) {
		return; // TODO proper error handling: frame/buffer size
	}
	// transmit data and length
	writeBytes(TX_BUFFER, NO_SUB, data, n);
	_txfctrl[0] = (byte)(n & 0xFF); // 1 byte (regular length + 1 bit)
	_txfctrl[1] |= (byte)((n >> 8) & 0x03);	// 2 added bits if extended length
	writeTransmitFrameControlRegister();
}

void DW1000Class::setData(const String& data) {
	int n = data.length()+1;
	byte* dataBytes = (byte*)malloc(n);
	data.getBytes(dataBytes, n);
	setData(dataBytes, n);
	free(dataBytes);
	
}

int DW1000Class::getDataLength() {
	if(_deviceMode == TX_MODE) {
		// 10 bits of TX frame control register
		return (((_txfctrl[1] << 8) | _txfctrl[0]) & 0x03FF);
	} else if(_deviceMode == RX_MODE) {
		// 10 bits of RX frame control register
		byte rxFrameInfo[LEN_RX_FINFO];
		readBytes(RX_FINFO, NO_SUB, rxFrameInfo, LEN_RX_FINFO);
		// TODO if other frame info bits are used somewhere else, store/cache bytes
		return ((((rxFrameInfo[1] << 8) | rxFrameInfo[0]) & 0x03FF) - 2); // w/o FCS 
	} else {
		return -1; // ignore in idle state
	}
}

void DW1000Class::getData(byte data[], int n) {
	if(n < 0) {
		return;
	}
	readBytes(RX_BUFFER, NO_SUB, data, n);
}

void DW1000Class::getData(String& data) {
	int i;
	int n = getDataLength(); // number of bytes w/o the two FCS ones
	if(n < 0) {
		return;
	}
	byte* dataBytes = (byte*)malloc(n);
	getData(dataBytes, n);
	// clear string
	data.remove(0);
	data = "";
	// append to string
	for(i = 0; i < n; i++) {
		data += (char)dataBytes[i];
	}
	free(dataBytes);
}

float DW1000Class::getTransmitTimestamp() {
	byte txTimeBytes[LEN_TX_STAMP];
	readBytes(TX_TIME, TX_STAMP_SUB, txTimeBytes, LEN_TX_STAMP);
	return readTimestampAsFloatUs(txTimeBytes);
}

float DW1000Class::getReceiveTimestamp() {
	byte rxTimeBytes[LEN_RX_STAMP];
	readBytes(RX_TIME, RX_STAMP_SUB, rxTimeBytes, LEN_RX_STAMP);
	return readTimestampAsFloatUs(rxTimeBytes);
}

float DW1000Class::getSystemTimestamp() {
	byte sysTimeBytes[LEN_SYS_TIME];
	readBytes(SYS_TIME, NO_SUB, sysTimeBytes, LEN_SYS_TIME);
	return readTimestampAsFloatUs(sysTimeBytes);
}

float DW1000Class::readTimestampAsFloatUs(byte ts[]) {
	float tsValue = ts[0] & 0xFF;
	tsValue += ((ts[1] & 0xFF) * 256.0f);
	tsValue += ((ts[2] & 0xFF) * 65536.0f);
	tsValue += ((ts[3] & 0xFF) * 16777216.0f);
	tsValue += ((ts[4] & 0xFF) * 4294967296.0f);
	return tsValue * TIME_RES;
}

void DW1000Class::writeFloatUsToTimestamp(float tsValue, byte ts[]) {
	int i = 0;
	byte val = 0;
	memset(ts, 0, LEN_STAMP);
	tsValue *= TIME_RES_INV;
	while(i < LEN_STAMP && tsValue >= 1) {
		ts[i] = ((byte)fmod(tsValue, 256.0f) & 0xFF);
		tsValue = floor(tsValue / 256);
		i++;
	} 
}

boolean DW1000Class::isTransmitDone() {
	return getBit(_sysstatus, LEN_SYS_STATUS, TXFRS_BIT);
}

boolean DW1000Class::isReceiveTimestampAvailable() {
	return getBit(_sysstatus, LEN_SYS_STATUS, LDEDONE_BIT);
}

boolean DW1000Class::isReceiveDone() {
	return getBit(_sysstatus, LEN_SYS_STATUS, RXDFR_BIT);
}

boolean DW1000Class::isReceiveError() {
	boolean ldeErr, rxCRCErr, rxHeaderErr, rxDecodeErr;
	ldeErr = getBit(_sysstatus, LEN_SYS_STATUS, LDEERR_BIT);
	rxCRCErr = getBit(_sysstatus, LEN_SYS_STATUS, RXFCE_BIT);
	rxHeaderErr = getBit(_sysstatus, LEN_SYS_STATUS, RXPHE_BIT);
	rxDecodeErr = getBit(_sysstatus, LEN_SYS_STATUS, RXRFSL_BIT);
	if(ldeErr || rxCRCErr || rxHeaderErr || rxDecodeErr) {
		return true; 
	}
	return false;
}

boolean DW1000Class::isReceiveTimeout() {
	return getBit(_sysstatus, LEN_SYS_STATUS, RXRFTO_BIT);
}

void DW1000Class::clearAllStatus() {
	memset(_sysstatus, 0, LEN_SYS_STATUS);
	writeBytes(SYS_STATUS, NO_SUB, _sysstatus, LEN_SYS_STATUS);
}

void DW1000Class::clearReceiveTimestampAvailableStatus() {
	setBit(_sysstatus, LEN_SYS_STATUS, LDEDONE_BIT, true);
	writeBytes(SYS_STATUS, NO_SUB, _sysstatus, LEN_SYS_STATUS);
}

void DW1000Class::clearReceiveStatus() {
	// clear latched RX bits (i.e. write 1 to clear)
	setBit(_sysstatus, LEN_SYS_STATUS, RXDFR_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, LDEDONE_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, LDEERR_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, RXPHE_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, RXFCE_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, RXFCG_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, RXRFSL_BIT, true);
	writeBytes(SYS_STATUS, NO_SUB, _sysstatus, LEN_SYS_STATUS);
}

void DW1000Class::clearTransmitStatus() {
	// clear latched TX bits
	setBit(_sysstatus, LEN_SYS_STATUS, TXFRB_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, TXPRS_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, TXPHS_BIT, true);
	setBit(_sysstatus, LEN_SYS_STATUS, TXFRS_BIT, true);
	writeBytes(SYS_STATUS, NO_SUB, _sysstatus, LEN_SYS_STATUS);
}

/* ###########################################################################
 * #### Helper functions #####################################################
 * ######################################################################### */

/*
 * Set the value of a bit in an array of bytes that are considered
 * consecutive and stored from MSB to LSB.
 * @param data
 * 		The number as byte array.
 * @param n
 * 		The number of bytes in the array.
 * @param bit
 * 		The position of the bit to be set.
 * @param val
 *		The boolean value to be set to the given bit position.
 */
void DW1000Class::setBit(byte data[], int n, int bit, boolean val) {
	int idx;
	int shift;

	idx = bit / 8;
	if(idx >= n) {
		return; // TODO proper error handling: out of bounds
	}
	byte* targetByte = &data[idx];
	shift = bit % 8;
	if(val) {
		bitSet(*targetByte, shift);
	} else {
		bitClear(*targetByte, shift);
	}
}

/*
 * Check the value of a bit in an array of bytes that are considered
 * consecutive and stored from MSB to LSB.
 * @param data
 * 		The number as byte array.
 * @param n
 * 		The number of bytes in the array.
 * @param bit
 * 		The position of the bit to be checked.
 */
boolean DW1000Class::getBit(byte data[], int n, int bit) {
	int idx;
	int shift;

	idx = bit / 8;
	if(idx >= n) {
		return false; // TODO proper error handling: out of bounds
	}
	byte targetByte = data[idx];
	shift = bit % 8;
	
	return bitRead(targetByte, shift);
}

void DW1000Class::writeValueToBytes(byte data[], int val, int n) {
	int i;	
	for(i = 0; i < n; i++) {
		data[i] = ((val >> (i * 8)) & 0xFF);
	}
}

/*
 * Read bytes from the DW1000. Number of bytes depend on register length.
 * @param cmd 
 * 		The register address (see Chapter 7 in the DW1000 user manual).
 * @param data 
 *		The data array to be read into.
 * @param n
 *		The number of bytes expected to be received.
 */
void DW1000Class::readBytes(byte cmd, word offset, byte data[], int n) {
	byte header[3];
	int headerLen = 1;
	int i;
	if(offset == NO_SUB) {
		header[0] = READ | cmd;
	} else {
		header[0] = READ_SUB | cmd;
		if(offset < 128) {
			header[1] = (byte)offset;
			headerLen++;
		} else {
			header[1] = READ | (byte)offset;
			header[2] = (byte)(offset >> 7);
			headerLen+=2;
		}
	}
	noInterrupts();
	digitalWrite(_ss, LOW);
	for(i = 0; i < headerLen; i++) {
		SPI.transfer(header[i]);
	}
	for(i = 0; i < n; i++) {
		data[i] = SPI.transfer(JUNK);
	}
	digitalWrite(_ss,HIGH);
	interrupts();
}

/*
 * Write bytes to the DW1000. Single bytes can be written to registers via sub-addressing.
 * @param cmd 
 * 		The register address (see Chapter 7 in the DW1000 user manual).
 * @param offset
 *		The offset to select register sub-parts for writing, or 0x00 to disable 
 * 		sub-adressing.
 * @param data 
 *		The data array to be written.
 * @param n
 *		The number of bytes to be written (take care not to go out of bounds of 
 * 		the register).
 */
void DW1000Class::writeBytes(byte cmd, word offset, byte data[], int n) {
	byte header[3];
	int headerLen = 1;
	int i;
	// TODO proper error handling: address out of bounds
	if(offset == NO_SUB) {
		header[0] = WRITE | cmd;
	} else {
		header[0] = WRITE_SUB | cmd;
		if(offset < 128) {
			header[1] = (byte)offset;
			headerLen++;
		} else {
			header[1] = WRITE | (byte)offset;
			header[2] = (byte)(offset >> 7);
			headerLen+=2;
		}
	}
	noInterrupts();
	digitalWrite(_ss, LOW);
	for(i = 0; i < headerLen; i++) {
		SPI.transfer(header[i]);
	}
	for(i = 0; i < n; i++) {
		SPI.transfer(data[i]);
	}
	delay(1);
	digitalWrite(_ss,HIGH);
	interrupts();
	delay(1);
}

char* DW1000Class::getPrettyBytes(byte data[], int n) {
	unsigned int i, j, b;
	b = sprintf(_msgBuf, "Data, bytes: %d\nB: 7 6 5 4 3 2 1 0\n", n);
	for(i = 0; i < n; i++) {
		byte curByte = data[i];
		snprintf(&_msgBuf[b++], 2, "%d", (i + 1));
		_msgBuf[b++] = (char)((i + 1) & 0xFF); _msgBuf[b++] = ':'; _msgBuf[b++] = ' ';
		for(j = 0; j < 8; j++) {
			_msgBuf[b++] = ((curByte >> (7 - j)) & 0x01) ? '1' : '0';
			if(j < 7) {
				_msgBuf[b++] = ' '; 
			} else if(i < n-1) {
				_msgBuf[b++] = '\n';
			} else {
				_msgBuf[b++] = '\0';
			}
		}
		
	}
	_msgBuf[b++] = '\0';
	return _msgBuf;
}

char* DW1000Class::getPrettyBytes(byte cmd, word offset, int n) {
	unsigned int i, j, b;
	byte* readBuf = (byte*)malloc(n);
	readBytes(cmd, offset, readBuf, n);
	b = sprintf(_msgBuf, "Reg: 0x%02x, bytes: %d\nB: 7 6 5 4 3 2 1 0\n", cmd, n);
	for(i = 0; i < n; i++) {
		byte curByte = readBuf[i];
		snprintf(&_msgBuf[b++], 2, "%d", (i + 1));
		_msgBuf[b++] = (char)((i + 1) & 0xFF); _msgBuf[b++] = ':'; _msgBuf[b++] = ' ';
		for(j = 0; j < 8; j++) {
			_msgBuf[b++] = ((curByte >> (7 - j)) & 0x01) ? '1' : '0';
			if(j < 7) {
				_msgBuf[b++] = ' '; 
			} else if(i < n-1) {
				_msgBuf[b++] = '\n';
			} else {
				_msgBuf[b++] = '\0';
			}
		}
		
	}
	_msgBuf[b++] = '\0';
	free(readBuf);
	return _msgBuf;
}
