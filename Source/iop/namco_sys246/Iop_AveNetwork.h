#pragma once

#include "../Iop_Module.h"

class CIopBios;

namespace Iop
{
	namespace Namco
	{
		class CAveNetworkContext
		{
		public:
			CAveNetworkContext(CIopBios&, uint8*);
			~CAveNetworkContext();

			int32 InvokeTcp(CMIPS&, unsigned int);
			int32 InvokePpp(CMIPS&, unsigned int);
			int32 InvokeDhcp(CMIPS&, unsigned int);
			void CountTicks(uint32);

		private:
			struct Implementation;
			std::unique_ptr<Implementation> m_impl;
		};

		class CAveTcp final : public CModule, public CTickableModule
		{
		public:
			explicit CAveTcp(const std::shared_ptr<CAveNetworkContext>&);

			std::string GetId() const override;
			std::string GetFunctionName(unsigned int) const override;
			void Invoke(CMIPS&, unsigned int) override;
			void CountTicks(uint32) override;

		private:
			std::shared_ptr<CAveNetworkContext> m_context;
		};

		class CAvePpp final : public CModule
		{
		public:
			explicit CAvePpp(const std::shared_ptr<CAveNetworkContext>&);

			std::string GetId() const override;
			std::string GetFunctionName(unsigned int) const override;
			void Invoke(CMIPS&, unsigned int) override;

		private:
			std::shared_ptr<CAveNetworkContext> m_context;
		};

		class CAveDhcp final : public CModule
		{
		public:
			explicit CAveDhcp(const std::shared_ptr<CAveNetworkContext>&);

			std::string GetId() const override;
			std::string GetFunctionName(unsigned int) const override;
			void Invoke(CMIPS&, unsigned int) override;

		private:
			std::shared_ptr<CAveNetworkContext> m_context;
		};

		class CAveDevGlue final : public CModule
		{
		public:
			explicit CAveDevGlue(uint8*);

			std::string GetId() const override;
			std::string GetFunctionName(unsigned int) const override;
			void Invoke(CMIPS&, unsigned int) override;

		private:
			void FillEthernetInfo(uint32);
			int32 GetDeviceInfo(uint32, uint32);
			int32 SelectDevice(int32);
			int32 GetDeviceInfoNum(uint32);
			int32 SetOption(uint32, uint32, uint32);
			int32 GetOption(uint32, uint32, uint32);

			uint8* m_ram = nullptr;
			bool m_changePending = true;
			int32 m_selectedDevice = 2;
			uint32 m_threadPriority = 50;
			uint32 m_deviceThreadPriority = 49;
			uint32 m_pppoeEnabled = 0;
			uint32 m_negotiationMode = 0;
		};

		class CAveAn986 final : public CModule
		{
		public:
			std::string GetId() const override;
			std::string GetFunctionName(unsigned int) const override;
			void Invoke(CMIPS&, unsigned int) override;
		};
	}
}
