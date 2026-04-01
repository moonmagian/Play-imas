#pragma once
#include "../Iop_Module.h"
#include <stdio.h>
#include <windows.h>
namespace Iop {
    typedef enum ac_uart_flag
    {
        AC_UART_FLAG_READ = 0x1,
        AC_UART_FLAG_WRITE = 0x2,
        AC_UART_FLAG_BOTH = 0x3,
    } UartFlag;

    typedef struct ac_uart_attr
    {
        int32 ua_speed;
        int32 ua_fifo;
        int32 ua_loopback;
        int32 ua_padding;
    } acUartAttrData;
    class UARTDriver : public CModule
    {
    public:

        UARTDriver(uint8*);
        virtual ~UARTDriver();

        std::string GetId() const override;
        std::string GetFunctionName(unsigned int) const override;
        void Invoke(CMIPS&, unsigned int) override;

        void SaveState(Framework::CZipArchiveWriter&) const override;
        void LoadState(Framework::CZipArchiveReader&) override;

    private:
        HANDLE m_pipe_handle;
        uint8* m_ram = nullptr;

        acUartAttrData m_uartAttrData;

        int32 Read(uint32, int);
        int32 Write(uint32, int);
        uint32 Wait(UartFlag, int);

        uint32 GetAttr(acUartAttrData* attr);
        uint32 SetAttr(const acUartAttrData* attr);
    };
}

