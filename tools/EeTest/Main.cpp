#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include "BasicBlock.h"
#include "COP_FPU.h"
#include "MIPS.h"
#include "ee/MA_EE.h"

namespace
{
	constexpr uint32 SOURCE_REGISTER = 1;
	constexpr uint32 DESTINATION_REGISTER = 2;

	uint32 MakeConvertOpcode(uint32 function)
	{
		return (0x11 << 26) |
		       (0x10 << 21) |
		       (SOURCE_REGISTER << 11) |
		       (DESTINATION_REGISTER << 6) |
		       function;
	}

	uint32 ExecuteConvert(uint32 function, uint32 value)
	{
		alignas(16) std::array<uint8, 0x1000> instructionMemory = {};
		auto instruction = MakeConvertOpcode(function);
		std::memcpy(instructionMemory.data(), &instruction, sizeof(instruction));

		CMIPS context(MEMORYMAP_ENDIAN_LSBF);
		CMA_EE architecture;
		CCOP_FPU fpu(MIPS_REGSIZE_64);

		context.m_pMemoryMap->InsertInstructionMap(
		    0, static_cast<uint32>(instructionMemory.size() - 1), instructionMemory.data(), 0);
		context.m_pArch = &architecture;
		context.m_pCOP[1] = &fpu;
		context.m_pAddrTranslator = CMIPS::TranslateAddress64;
		context.Reset();

		context.m_State.nCOP1[SOURCE_REGISTER] = value;
		context.m_State.nPC = 0;
		context.m_State.nDelayedJumpAddr = MIPS_INVALID_PC;
		context.m_State.cycleQuota = 1;

		CBasicBlock block(context, 0, 0, BLOCK_CATEGORY_PS2_EE);
		block.Compile();
		block.Execute();
		return context.m_State.nCOP1[DESTINATION_REGISTER];
	}

	bool VerifyConvert(uint32 function, uint32 value, uint32 expected)
	{
		auto result = ExecuteConvert(function, value);
		if(result == expected)
		{
			return true;
		}

		std::fprintf(
		    stderr,
		    "Conversion function 0x%02X of 0x%08X returned 0x%08X, expected 0x%08X.\n",
		    function,
		    value,
		    result,
		    expected);
		return false;
	}
}

int main()
{
	constexpr uint32 TRUNC_W_S = 0x0D;
	constexpr uint32 CVT_W_S = 0x24;
	constexpr std::array<std::pair<uint32, uint32>, 5> cases = {{
	    {0x42F78000, 0x0000007B},
	    {0xC2F78000, 0xFFFFFF85},
	    {0x4EFFFFFF, 0x7FFFFF80},
	    {0x4F7FFFFF, 0x7FFFFFFF},
	    {0xCF7FFFFF, 0x80000000},
	}};

	for(auto function : {TRUNC_W_S, CVT_W_S})
	{
		for(const auto& [value, expected] : cases)
		{
			if(!VerifyConvert(function, value, expected))
			{
				return 1;
			}
		}
	}

	return 0;
}
