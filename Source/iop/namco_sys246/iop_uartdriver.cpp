#include "iop_uartdriver.h"
#include <iostream>
#include "Log.h"
#include <stdio.h>
#include <windows.h>
#include <math.h>
Iop::UARTDriver::UARTDriver(uint8* ram): m_ram(ram)
{
    this->m_pipe_handle = CreateFile(
        TEXT("\\\\.\\pipe\\imas"),  // Pipe name
        GENERIC_READ | GENERIC_WRITE, // Access: Read and Write
        0,                            // No sharing (pipes don't use this)
        NULL,                         // Default security
        OPEN_EXISTING,                // Vital: Pipes must already exist
        0,                            // File attributes (use FILE_FLAG_OVERLAPPED for async)
        NULL                          // No template file
        );
}

Iop::UARTDriver::~UARTDriver() {
}

std::string Iop::UARTDriver::GetId() const
{
    return "acuart";
}

std::string Iop::UARTDriver::GetFunctionName(unsigned int functionId) const
{
    switch (functionId) {
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

void Iop::UARTDriver::Invoke(CMIPS & ctx, unsigned int functionId)
{
    switch (functionId) {
    case 8:
    {
        uint32 bufferPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
        uint32 size = ctx.m_State.nGPR[CMIPS::A1].nV0;
        ctx.m_State.nGPR[CMIPS::V0].nV0 = this->Read(bufferPtr, size);
    }
        break;
    case 9:
    {
        uint32 bufferPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
        uint32 size = ctx.m_State.nGPR[CMIPS::A1].nV0;
        ctx.m_State.nGPR[CMIPS::V0].nV0 = this->Write(bufferPtr, size);
    }
        break;
    case 11:
    {
        uint32 attrPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
        ctx.m_State.nGPR[CMIPS::V0].nV0 = this->GetAttr(reinterpret_cast<acUartAttrData*>(&this->m_ram[attrPtr]));
    }
    case 12:
    {
        uint32 attrPtr = ctx.m_State.nGPR[CMIPS::A0].nV0;
        ctx.m_State.nGPR[CMIPS::V0].nV0 = this->SetAttr(reinterpret_cast<const acUartAttrData*>(&this->m_ram[attrPtr]));
    }
    default:
        std::cout << "Unknown" << std::endl;
        return;
    }
}

void Iop::UARTDriver::SaveState(Framework::CZipArchiveWriter &) const
{

}

void Iop::UARTDriver::LoadState(Framework::CZipArchiveReader &)
{

}

int32 Iop::UARTDriver::Read(uint32 bufferPtr, int size)
{
    DWORD bytesAvailable = 0;
    PeekNamedPipe(this->m_pipe_handle, NULL, 0, NULL, &bytesAvailable, NULL);
    if (bytesAvailable == 0) {
        return 0;
    }
    size = min(size, bytesAvailable);
    DWORD dwRet;
    ReadFile(this->m_pipe_handle, &this->m_ram[bufferPtr], size, &dwRet, NULL);
    return dwRet;
}

int32 Iop::UARTDriver::Write(uint32 bufferPtr, int size)
{
    DWORD dwRet;
    WriteFile(this->m_pipe_handle, &this->m_ram[bufferPtr], size, &dwRet, NULL);
    return dwRet;
}

uint32 Iop::UARTDriver::Wait(UartFlag flag, int msec)
{
    return 0;
}

uint32 Iop::UARTDriver::GetAttr(acUartAttrData *attr)
{
    attr->ua_fifo = this->m_uartAttrData.ua_fifo;
    attr->ua_loopback = this->m_uartAttrData.ua_loopback;
    attr->ua_padding = this->m_uartAttrData.ua_padding;
    attr->ua_speed = this->m_uartAttrData.ua_speed;
    return 0;
}

uint32 Iop::UARTDriver::SetAttr(const acUartAttrData *attr)
{
    this->m_uartAttrData = *attr;
    return 0;
}
