#include "iop_uartdriver.h"
#include <iostream>
#include <stdexcept>
#include "Log.h"

#define LOG_NAME ("iop_uartdriver")

namespace
{
	const char* NAMED_PIPE_PATH = R"(\\.\pipe\imas)";

	std::string MakeSerialPortPath(const std::string& portName)
	{
		if(portName.rfind(R"(\\.\)", 0) == 0)
		{
			return portName;
		}
		return R"(\\.\)" + portName;
	}
}

Iop::UARTDriver::UARTDriver(uint8* ram)
    : UARTDriver(ram, false, std::string())
{
}

Iop::UARTDriver::UARTDriver(uint8* ram, bool useRealCardReader, const std::string& cardReaderComPort)
    : m_transport(useRealCardReader ? TRANSPORT::SERIAL_PORT : TRANSPORT::NAMED_PIPE)
    , m_ram(ram)
{
	if(useRealCardReader)
	{
		OpenSerialPort(cardReaderComPort);
	}
	else
	{
		OpenNamedPipe();
	}
}

Iop::UARTDriver::~UARTDriver()
{
	if(IsOpen())
	{
		CloseHandle(m_deviceHandle);
	}
}

void Iop::UARTDriver::OpenNamedPipe()
{
	m_deviceHandle = CreateFileA(
	    NAMED_PIPE_PATH,
	    GENERIC_READ | GENERIC_WRITE,
	    0,
	    nullptr,
	    OPEN_EXISTING,
	    0,
	    nullptr);
	if(!IsOpen())
	{
		auto error = GetLastError();
		CLog::GetInstance().Warn(LOG_NAME, "Failed to open card reader named pipe '%s' (error %lu).\r\n", NAMED_PIPE_PATH, error);
	}
}

void Iop::UARTDriver::OpenSerialPort(const std::string& portName)
{
	if(portName.empty())
	{
		throw std::runtime_error("Cannot use the real card reader because no COM port is configured.");
	}

	auto portPath = MakeSerialPortPath(portName);
	m_deviceHandle = CreateFileA(
	    portPath.c_str(),
	    GENERIC_READ | GENERIC_WRITE,
	    0,
	    nullptr,
	    OPEN_EXISTING,
	    0,
	    nullptr);
	if(!IsOpen())
	{
		auto error = GetLastError();
		throw std::runtime_error(
		    "Failed to open card reader serial port '" + portName + "' (Windows error " + std::to_string(error) +
		    "). Make sure the adapter is connected and the port is not open in another program.");
	}

	auto closeAndThrow = [this, &portName](const char* operation) {
		auto error = GetLastError();
		CloseHandle(m_deviceHandle);
		m_deviceHandle = INVALID_HANDLE_VALUE;
		throw std::runtime_error(
		    "Failed to " + std::string(operation) + " card reader serial port '" + portName +
		    "' (Windows error " + std::to_string(error) + ").");
	};

	DCB serialConfig = {};
	serialConfig.DCBlength = sizeof(serialConfig);
	if(!GetCommState(m_deviceHandle, &serialConfig))
	{
		closeAndThrow("read the configuration of");
	}

	serialConfig.BaudRate = CBR_9600;
	serialConfig.ByteSize = 8;
	serialConfig.Parity = NOPARITY;
	serialConfig.StopBits = ONESTOPBIT;
	serialConfig.fBinary = TRUE;
	serialConfig.fParity = FALSE;
	serialConfig.fOutxCtsFlow = FALSE;
	serialConfig.fOutxDsrFlow = FALSE;
	// ACUART asserts its modem-control outputs even though it doesn't use hardware handshaking.
	serialConfig.fDtrControl = DTR_CONTROL_ENABLE;
	serialConfig.fDsrSensitivity = FALSE;
	serialConfig.fTXContinueOnXoff = TRUE;
	serialConfig.fOutX = FALSE;
	serialConfig.fInX = FALSE;
	serialConfig.fErrorChar = FALSE;
	serialConfig.fNull = FALSE;
	serialConfig.fRtsControl = RTS_CONTROL_ENABLE;
	serialConfig.fAbortOnError = FALSE;
	if(!SetCommState(m_deviceHandle, &serialConfig))
	{
		closeAndThrow("configure");
	}

	COMMTIMEOUTS timeouts = {};
	timeouts.ReadIntervalTimeout = MAXDWORD;
	timeouts.WriteTotalTimeoutMultiplier = 2;
	timeouts.WriteTotalTimeoutConstant = 1000;
	if(!SetCommTimeouts(m_deviceHandle, &timeouts))
	{
		closeAndThrow("configure timeouts for");
	}

	if(!PurgeComm(m_deviceHandle, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR))
	{
		closeAndThrow("clear pending data from");
	}

	CLog::GetInstance().Print(LOG_NAME, "Using real card reader on '%s' (9600 8-N-1, no flow control, DTR/RTS asserted).\r\n", portName.c_str());
}

bool Iop::UARTDriver::IsOpen() const
{
	return m_deviceHandle != INVALID_HANDLE_VALUE;
}

DWORD Iop::UARTDriver::GetBytesAvailable()
{
	if(!IsOpen())
	{
		return 0;
	}

	if(m_transport == TRANSPORT::NAMED_PIPE)
	{
		DWORD bytesAvailable = 0;
		if(!PeekNamedPipe(m_deviceHandle, nullptr, 0, nullptr, &bytesAvailable, nullptr))
		{
			auto error = GetLastError();
			CLog::GetInstance().Warn(LOG_NAME, "Failed to query the card reader named pipe (error %lu).\r\n", error);
			return 0;
		}
		return bytesAvailable;
	}

	DWORD errors = 0;
	COMSTAT status = {};
	if(!ClearCommError(m_deviceHandle, &errors, &status))
	{
		auto error = GetLastError();
		CLog::GetInstance().Warn(LOG_NAME, "Failed to query the card reader serial port (error %lu).\r\n", error);
		return 0;
	}
	return status.cbInQue;
}

std::string Iop::UARTDriver::GetId() const
{
	return "acuart";
}

std::string Iop::UARTDriver::GetFunctionName(unsigned int functionId) const
{
	switch(functionId)
	{
	case 8:
		return "acUartRead";
	case 9:
		return "acUartWrite";
	case 10:
		return "acUartWait";
	case 11:
		return "acUartGetAttr";
	case 12:
		return "acUartSetAttr";
	}
	return "unknown";
}

void Iop::UARTDriver::Invoke(CMIPS& ctx, unsigned int functionId)
{
	switch(functionId)
	{
	case 8:
	{
		uint32 bufferPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
		uint32 size = ctx.m_State.nGPR[CMIPS::A1].nV0;
		ctx.m_State.nGPR[CMIPS::V0].nV0 = Read(bufferPtr, size);
	}
	break;
	case 9:
	{
		uint32 bufferPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
		uint32 size = ctx.m_State.nGPR[CMIPS::A1].nV0;
		ctx.m_State.nGPR[CMIPS::V0].nV0 = Write(bufferPtr, size);
	}
	break;
	case 11:
	{
		uint32 attrPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
		ctx.m_State.nGPR[CMIPS::V0].nV0 = GetAttr(reinterpret_cast<acUartAttrData*>(&m_ram[attrPtr]));
	}
	case 12:
	{
		uint32 attrPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
		ctx.m_State.nGPR[CMIPS::V0].nV0 = SetAttr(reinterpret_cast<const acUartAttrData*>(&m_ram[attrPtr]));
	}
	default:
		std::cout << "Unknown" << std::endl;
		return;
	}
}

void Iop::UARTDriver::SaveState(Framework::CZipArchiveWriter&) const
{
}

void Iop::UARTDriver::LoadState(Framework::CZipArchiveReader&)
{
}

int32 Iop::UARTDriver::Read(uint32 bufferPtr, int size)
{
	if((size <= 0) || !IsOpen())
	{
		return 0;
	}

	auto bytesAvailable = GetBytesAvailable();
	if(bytesAvailable == 0)
	{
		return 0;
	}

	auto requestedSize = static_cast<DWORD>(size);
	auto readSize = (requestedSize < bytesAvailable) ? requestedSize : bytesAvailable;
	DWORD bytesRead = 0;
	if(!ReadFile(m_deviceHandle, &m_ram[bufferPtr], readSize, &bytesRead, nullptr))
	{
		auto error = GetLastError();
		CLog::GetInstance().Warn(LOG_NAME, "Failed to read from the card reader transport (error %lu).\r\n", error);
		return 0;
	}
	return static_cast<int32>(bytesRead);
}

int32 Iop::UARTDriver::Write(uint32 bufferPtr, int size)
{
	if((size <= 0) || !IsOpen())
	{
		return 0;
	}

	DWORD bytesWritten = 0;
	if(!WriteFile(m_deviceHandle, &m_ram[bufferPtr], static_cast<DWORD>(size), &bytesWritten, nullptr))
	{
		auto error = GetLastError();
		CLog::GetInstance().Warn(LOG_NAME, "Failed to write to the card reader transport (error %lu).\r\n", error);
		return 0;
	}
	return static_cast<int32>(bytesWritten);
}

uint32 Iop::UARTDriver::Wait(UartFlag flag, int msec)
{
	return 0;
}

uint32 Iop::UARTDriver::GetAttr(acUartAttrData* attr)
{
	attr->ua_fifo = m_uartAttrData.ua_fifo;
	attr->ua_loopback = m_uartAttrData.ua_loopback;
	attr->ua_padding = m_uartAttrData.ua_padding;
	attr->ua_speed = m_uartAttrData.ua_speed;
	return 0;
}

uint32 Iop::UARTDriver::SetAttr(const acUartAttrData* attr)
{
	m_uartAttrData = *attr;
	return 0;
}
