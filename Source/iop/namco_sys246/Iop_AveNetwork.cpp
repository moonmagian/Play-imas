#include "Iop_AveNetwork.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Log.h"
#include "SocketDef.h"
#include "SocketStream.h"
#include "../IopBios.h"
#include "../Iop_Sysmem.h"

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/ioctl.h>
#endif

using namespace Iop;
using namespace Iop::Namco;

#define LOG_NAME ("iop_ave_network")

namespace
{
	constexpr int32 AVE_ERROR_BAD_HANDLE = -4;
	constexpr int32 AVE_ERROR_CONNECTION = -6;
	constexpr int32 AVE_ERROR_CLOSED = -8;
	constexpr int32 AVE_ERROR_NOT_CONNECTED = -10;
	constexpr int32 AVE_ERROR_BUSY = -16;

	uint32 GetArgument(CMIPS& context, unsigned int index)
	{
		if(index < 4)
		{
			return context.m_State.nGPR[CMIPS::A0 + index].nV0;
		}
		auto stackAddress = context.m_State.nGPR[CMIPS::SP].nV0 + 0x10 + ((index - 4) * 4);
		return context.m_pMemoryMap->GetWord(stackAddress);
	}

	void CloseHostSocket(SOCKET socket)
	{
		if(socket == INVALID_SOCKET) return;
#ifdef _WIN32
		closesocket(socket);
#else
		close(socket);
#endif
	}

	int ShutdownHostSocket(SOCKET socket)
	{
		return shutdown(socket, 1);
	}

	bool SetNonBlocking(SOCKET socket)
	{
#ifdef SO_NOSIGPIPE
		int suppressSigPipe = 1;
		setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&suppressSigPipe), sizeof(suppressSigPipe));
#endif
#ifdef _WIN32
		u_long enabled = 1;
		return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
		auto flags = fcntl(socket, F_GETFL, 0);
		return (flags >= 0) && (fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0);
#endif
	}

	bool IsWouldBlockError()
	{
#ifdef _WIN32
		auto error = WSAGetLastError();
		return (error == WSAEWOULDBLOCK) || (error == WSAEINPROGRESS) || (error == WSAEALREADY);
#else
		return (errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINPROGRESS) || (errno == EALREADY);
#endif
	}

	int GetSendFlags()
	{
#ifdef MSG_NOSIGNAL
		return MSG_NOSIGNAL;
#else
		return 0;
#endif
	}

	bool IsSocketReady(SOCKET socket, bool read, bool write)
	{
		fd_set readSet;
		fd_set writeSet;
		FD_ZERO(&readSet);
		FD_ZERO(&writeSet);
		if(read) FD_SET(socket, &readSet);
		if(write) FD_SET(socket, &writeSet);
		timeval timeout = {};
		auto result = select(static_cast<int>(socket + 1), read ? &readSet : nullptr, write ? &writeSet : nullptr, nullptr, &timeout);
		if(result <= 0) return false;
		return (read && FD_ISSET(socket, &readSet)) || (write && FD_ISSET(socket, &writeSet));
	}

	uint32 MakeIpv4Address(const char* address)
	{
		return static_cast<uint32>(inet_addr(address));
	}

	template <typename ValueType>
	ValueType ReadGuest(const uint8* ram, uint32 address)
	{
		ValueType result = {};
		if(address != 0)
		{
			memcpy(&result, ram + address, sizeof(result));
		}
		return result;
	}

	template <typename ValueType>
	void WriteGuest(uint8* ram, uint32 address, ValueType value)
	{
		if(address != 0)
		{
			memcpy(ram + address, &value, sizeof(value));
		}
	}

	void WriteGuestString(uint8* ram, uint32 address, const char* value, size_t capacity)
	{
		if((address == 0) || (capacity == 0)) return;
		strncpy(reinterpret_cast<char*>(ram + address), value, capacity);
		ram[address + capacity - 1] = 0;
	}

	std::string FormatIpv4Address(uint32 address)
	{
		auto bytes = reinterpret_cast<const uint8*>(&address);
		char result[16] = {};
		snprintf(result, sizeof(result), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
		return result;
	}

	int GetLastSocketError()
	{
#ifdef _WIN32
		return WSAGetLastError();
#else
		return errno;
#endif
	}

	std::string FormatHexPreview(const uint8* data, size_t size)
	{
		constexpr size_t maxPreviewSize = 16;
		char byteText[4] = {};
		std::string result;
		auto previewSize = std::min(size, maxPreviewSize);
		for(size_t i = 0; i < previewSize; i++)
		{
			snprintf(byteText, sizeof(byteText), "%02X", data[i]);
			if(!result.empty()) result += ' ';
			result += byteText;
		}
		if(size > previewSize) result += " ...";
		return result;
	}

	bool EqualsNoCase(const std::string& left, const char* right)
	{
		auto rightLength = strlen(right);
		if(left.size() != rightLength) return false;
		for(size_t i = 0; i < left.size(); i++)
		{
			if(std::tolower(static_cast<unsigned char>(left[i])) !=
			   std::tolower(static_cast<unsigned char>(right[i])))
			{
				return false;
			}
		}
		return true;
	}

	std::string GetHttpHeader(const std::string& headers, const char* name)
	{
		size_t lineStart = 0;
		while(lineStart < headers.size())
		{
			auto lineEnd = headers.find('\n', lineStart);
			if(lineEnd == std::string::npos) lineEnd = headers.size();
			auto contentEnd = lineEnd;
			if((contentEnd > lineStart) && (headers[contentEnd - 1] == '\r')) contentEnd--;
			auto separator = headers.find(':', lineStart);
			if((separator < contentEnd) && EqualsNoCase(headers.substr(lineStart, separator - lineStart), name))
			{
				auto valueStart = separator + 1;
				while((valueStart < contentEnd) && std::isspace(static_cast<unsigned char>(headers[valueStart])))
				{
					valueStart++;
				}
				return headers.substr(valueStart, contentEnd - valueStart);
			}
			lineStart = lineEnd + 1;
		}
		return {};
	}
}

struct CAveNetworkContext::Implementation
{
	static constexpr unsigned int MAX_TCP_SOCKETS = 8;
	static constexpr unsigned int MAX_UDP_SOCKETS = 8;
	static constexpr unsigned int MAX_DNS_TICKETS = 8;
	static constexpr uint16 TCP_WINDOW_SIZE = 4096;

	enum class SOCKET_STATE
	{
		CLOSED,
		LISTENING,
		CONNECTING,
		CONNECTED,
		PEER_CLOSED,
		FAILED,
	};

	enum class UDP_CALLBACK_STAGE
	{
		IDLE,
		RESERVE_BUFFER,
		RELEASE_BUFFER,
	};

	struct PENDING_RECEIVE
	{
		bool active = false;
		uint32 bufferPtr = 0;
		uint16 size = 0;
		uint32 callbackPtr = 0;
		int32 threadId = -1;
	};

	struct PENDING_SEND
	{
		bool active = false;
		std::vector<uint8> data;
		size_t offset = 0;
		uint32 callbackPtr = 0;
		uint32 completionArg = 0;
		int32 threadId = -1;
	};

	struct PENDING_ACCEPT
	{
		bool active = false;
		uint32 callbackPtr = 0;
		uint32 acceptedAsrPtr = 0;
		uint32 outputPtr = 0;
		int32 threadId = -1;
	};

	struct TCP_SOCKET
	{
		SOCKET socket = INVALID_SOCKET;
		SOCKET_STATE state = SOCKET_STATE::CLOSED;
		uint32 remoteAddress = 0;
		uint32 localAddress = 0;
		uint16 remotePort = 0;
		uint16 localPort = 0;
		uint32 asrPtr = 0;
		bool connectNotifyPending = false;
		bool dataNotified = false;
		bool peerCloseNotified = false;
		uint64 bytesSent = 0;
		uint64 bytesReceived = 0;
		PENDING_RECEIVE receive;
		PENDING_SEND send;
		PENDING_ACCEPT accept;
	};

	struct DNS_TICKET
	{
		bool used = false;
		std::string hostname;
		std::vector<uint32> addresses;
	};

	struct UDP_SOCKET
	{
		SOCKET socket = INVALID_SOCKET;
		bool used = false;
		uint32 remoteAddress = 0;
		uint16 remotePort = 0;
		uint16 localPort = 0;
		uint32 asrPtr = 0;
		uint32 receiverPtr = 0;
		UDP_CALLBACK_STAGE callbackStage = UDP_CALLBACK_STAGE::IDLE;
		int32 callbackThreadId = -1;
		std::vector<uint8> receiveData;
	};

	Implementation(CIopBios& bios, uint8* ram)
	    : m_bios(bios)
	    , m_ram(ram)
	{
		Framework::InitializeSocketSupport();
		m_ipAddress = MakeIpv4Address("10.0.2.15");
		m_netMask = MakeIpv4Address("255.255.255.0");
		m_broadcast = MakeIpv4Address("10.0.2.255");
		m_gateway = MakeIpv4Address("10.0.2.2");
		m_dnsPrimary = MakeIpv4Address("8.8.8.8");
		m_dnsSecondary = MakeIpv4Address("1.1.1.1");
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "AVE network HLE created (lease=%s mask=%s gateway=%s dns=%s,%s).\r\n",
		    FormatIpv4Address(m_ipAddress).c_str(),
		    FormatIpv4Address(m_netMask).c_str(),
		    FormatIpv4Address(m_gateway).c_str(),
		    FormatIpv4Address(m_dnsPrimary).c_str(),
		    FormatIpv4Address(m_dnsSecondary).c_str());
	}

	~Implementation()
	{
		for(auto& socket : m_tcpSockets)
		{
			CloseHostSocket(socket.socket);
			socket = {};
		}
		for(auto& socket : m_udpSockets)
		{
			CloseHostSocket(socket.socket);
			socket = {};
		}
	}

	template <typename ValueType>
	ValueType Read(uint32 address) const
	{
		ValueType result = {};
		if(address != 0)
		{
			memcpy(&result, m_ram + address, sizeof(result));
		}
		return result;
	}

	template <typename ValueType>
	void Write(uint32 address, ValueType value)
	{
		if(address != 0)
		{
			memcpy(m_ram + address, &value, sizeof(value));
		}
	}

	void CompleteThread(int32 threadId, int32 result)
	{
		if(threadId < 0) return;
		auto thread = m_bios.GetThread(threadId);
		if(!thread) return;
		CLog::GetInstance().Print(LOG_NAME, "TCP waking thread=%d result=%d.\r\n", threadId, result);
		thread->context.gpr[CMIPS::V0] = static_cast<uint32>(result);
		m_bios.WakeupThread(threadId, false);
	}

	int32 SuspendCurrentThread()
	{
		auto threadId = m_bios.GetCurrentThreadIdRaw();
		if(threadId < 0) return -1;
		m_bios.SleepThread();
		return threadId;
	}

	void TriggerAsr(unsigned int handle, int32 event)
	{
		auto& socket = m_tcpSockets[handle];
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP ASR handle=%u event=%d callback=0x%08X%s.\r\n",
		    handle,
		    event,
		    socket.asrPtr,
		    socket.asrPtr ? "" : " (not registered)");
		if(socket.asrPtr != 0)
		{
			m_bios.TriggerCallback(socket.asrPtr, handle, event);
		}
	}

	void NotifyPeerClosed(unsigned int handle)
	{
		auto& socket = m_tcpSockets[handle];
		socket.state = SOCKET_STATE::PEER_CLOSED;
		if(socket.peerCloseNotified) return;
		socket.peerCloseNotified = true;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP peer FIN handle=%u peer=%s:%u totals(sent=%llu received=%llu).\r\n",
		    handle,
		    FormatIpv4Address(socket.remoteAddress).c_str(),
		    ntohs(socket.remotePort),
		    static_cast<unsigned long long>(socket.bytesSent),
		    static_cast<unsigned long long>(socket.bytesReceived));
		TriggerAsr(handle, 2);
	}

	void LogTcpPayload(const char* direction, unsigned int handle, const uint8* data, size_t size)
	{
		auto& socket = m_tcpSockets[handle];
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP packet forwarded direction=%s handle=%u peer=%s:%u bytes=%u totals(sent=%llu received=%llu) hex=[%s].\r\n",
		    direction,
		    handle,
		    FormatIpv4Address(socket.remoteAddress).c_str(),
		    ntohs(socket.remotePort),
		    static_cast<unsigned int>(size),
		    static_cast<unsigned long long>(socket.bytesSent),
		    static_cast<unsigned long long>(socket.bytesReceived),
		    FormatHexPreview(data, size).c_str());

		auto headerSize = std::min<size_t>(size, 4096);
		std::string headers(reinterpret_cast<const char*>(data), headerSize);
		auto lineEnd = headers.find('\n');
		if(lineEnd == std::string::npos) return;
		auto firstLine = headers.substr(0, lineEnd);
		if(!firstLine.empty() && (firstLine.back() == '\r')) firstLine.pop_back();
		if(firstLine.size() > 512) firstLine.resize(512);

		if(firstLine.compare(0, 5, "HTTP/") == 0)
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "HTTP response handle=%u peer=%s:%u status='%s' contentLength='%s' transferEncoding='%s'.\r\n",
			    handle,
			    FormatIpv4Address(socket.remoteAddress).c_str(),
			    ntohs(socket.remotePort),
			    firstLine.c_str(),
			    GetHttpHeader(headers, "Content-Length").c_str(),
			    GetHttpHeader(headers, "Transfer-Encoding").c_str());
			return;
		}

		static const char* methods[] = {"GET ", "POST ", "HEAD ", "PUT ", "DELETE ", "OPTIONS ", "CONNECT ", "PATCH "};
		auto isRequest = std::any_of(std::begin(methods), std::end(methods), [&](const char* method) {
			return firstLine.compare(0, strlen(method), method) == 0;
		});
		if(isRequest)
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "HTTP request handle=%u peer=%s:%u request='%s' host='%s' contentLength='%s'.\r\n",
			    handle,
			    FormatIpv4Address(socket.remoteAddress).c_str(),
			    ntohs(socket.remotePort),
			    firstLine.c_str(),
			    GetHttpHeader(headers, "Host").c_str(),
			    GetHttpHeader(headers, "Content-Length").c_str());
		}
	}

	int AllocateTcpSocket(SOCKET hostSocket)
	{
		for(unsigned int handle = 0; handle < m_tcpSockets.size(); handle++)
		{
			auto& socket = m_tcpSockets[handle];
			if(socket.state != SOCKET_STATE::CLOSED) continue;
			socket = {};
			socket.socket = hostSocket;
			return handle;
		}
		return -1;
	}

	TCP_SOCKET* GetTcpSocket(int32 handle)
	{
		if((handle < 0) || (handle >= static_cast<int32>(m_tcpSockets.size()))) return nullptr;
		auto& socket = m_tcpSockets[handle];
		if(socket.state == SOCKET_STATE::CLOSED) return nullptr;
		return &socket;
	}

	void CompleteReceive(unsigned int handle, int32 result)
	{
		auto& socket = m_tcpSockets[handle];
		auto pending = socket.receive;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP receive completion handle=%u result=%d callback=0x%08X thread=%d.\r\n",
		    handle,
		    result,
		    pending.callbackPtr,
		    pending.threadId);
		socket.receive = {};
		socket.dataNotified = false;
		if(pending.callbackPtr != 0)
		{
			m_bios.TriggerCallback(pending.callbackPtr, result, handle, 0);
		}
		else
		{
			CompleteThread(pending.threadId, result);
		}
	}

	void CompleteSend(unsigned int handle, int32 result)
	{
		auto& socket = m_tcpSockets[handle];
		auto pending = std::move(socket.send);
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP send completion handle=%u result=%d callback=0x%08X thread=%d completionArg=0x%08X.\r\n",
		    handle,
		    result,
		    pending.callbackPtr,
		    pending.threadId,
		    pending.completionArg);
		socket.send = {};
		if(pending.callbackPtr != 0)
		{
			m_bios.TriggerCallback(pending.callbackPtr, result, handle, pending.completionArg);
		}
		else
		{
			CompleteThread(pending.threadId, result);
		}
	}

	void CancelPending(unsigned int handle, int32 result)
	{
		auto& socket = m_tcpSockets[handle];
		if(socket.receive.active) CompleteReceive(handle, result);
		if(socket.send.active) CompleteSend(handle, result);
		if(socket.accept.active)
		{
			auto pending = socket.accept;
			socket.accept = {};
			if(pending.callbackPtr != 0)
			{
				m_bios.TriggerCallback(pending.callbackPtr, -1, handle, 0, 0);
			}
			else
			{
				CompleteThread(pending.threadId, result);
			}
		}
	}

	void CloseTcpSocket(unsigned int handle, bool notify)
	{
		auto& socket = m_tcpSockets[handle];
		auto asrPtr = socket.asrPtr;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP close handle=%u notify=%u state=%u.\r\n",
		    handle,
		    notify,
		    static_cast<unsigned int>(socket.state));
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP close totals handle=%u sent=%llu received=%llu peer=%s:%u.\r\n",
		    handle,
		    static_cast<unsigned long long>(socket.bytesSent),
		    static_cast<unsigned long long>(socket.bytesReceived),
		    FormatIpv4Address(socket.remoteAddress).c_str(),
		    ntohs(socket.remotePort));
		CancelPending(handle, AVE_ERROR_CONNECTION);
		CloseHostSocket(socket.socket);
		socket = {};
		if(notify && (asrPtr != 0))
		{
			m_bios.TriggerCallback(asrPtr, handle, 4);
		}
	}

	void ResetTcp()
	{
		for(unsigned int handle = 0; handle < m_tcpSockets.size(); handle++)
		{
			if(m_tcpSockets[handle].state != SOCKET_STATE::CLOSED)
			{
				CloseTcpSocket(handle, false);
			}
		}
	}

	void RefreshSocketAddresses(TCP_SOCKET& socket)
	{
		sockaddr_in address = {};
		socklen_t addressSize = sizeof(address);
		if(getsockname(socket.socket, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0)
		{
			socket.localAddress = address.sin_addr.s_addr;
			socket.localPort = address.sin_port;
		}
		addressSize = sizeof(address);
		if(getpeername(socket.socket, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0)
		{
			socket.remoteAddress = address.sin_addr.s_addr;
			socket.remotePort = address.sin_port;
		}
	}

	int32 OpenTcp(uint32 remoteAddress, uint16 remotePort, uint16 localPort, uint32 asrPtr)
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP open remote=%s:%u localPort=%u asr=0x%08X.\r\n",
		    FormatIpv4Address(remoteAddress).c_str(),
		    ntohs(remotePort),
		    ntohs(localPort),
		    asrPtr);
		auto hostSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if(hostSocket == INVALID_SOCKET)
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP open socket() failed error=%d.\r\n", GetLastSocketError());
			return -2;
		}
		if(!SetNonBlocking(hostSocket))
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP open nonblocking setup failed error=%d.\r\n", GetLastSocketError());
			CloseHostSocket(hostSocket);
			return -2;
		}

		int handle = AllocateTcpSocket(hostSocket);
		if(handle < 0)
		{
			CloseHostSocket(hostSocket);
			return -2;
		}

		auto& socketEntry = m_tcpSockets[handle];
		socketEntry.remoteAddress = remoteAddress;
		socketEntry.remotePort = remotePort;
		socketEntry.localPort = localPort;
		socketEntry.asrPtr = asrPtr;

		if(localPort != 0)
		{
			sockaddr_in local = {};
			local.sin_family = AF_INET;
			local.sin_addr.s_addr = htonl(INADDR_ANY);
			local.sin_port = localPort;
			if(bind(hostSocket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
			{
				CLog::GetInstance().Print(LOG_NAME, "TCP open bind() failed error=%d.\r\n", GetLastSocketError());
				CloseTcpSocket(handle, false);
				return -3;
			}
		}

		sockaddr_in remote = {};
		remote.sin_family = AF_INET;
		remote.sin_addr.s_addr = remoteAddress;
		remote.sin_port = remotePort;
		auto connectResult = connect(hostSocket, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
		if(connectResult == 0)
		{
			socketEntry.state = SOCKET_STATE::CONNECTED;
			socketEntry.connectNotifyPending = true;
			RefreshSocketAddresses(socketEntry);
		}
		else if(IsWouldBlockError())
		{
			socketEntry.state = SOCKET_STATE::CONNECTING;
		}
		else
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP open connect() failed error=%d.\r\n", GetLastSocketError());
			CloseTcpSocket(handle, false);
			return -6;
		}
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP open handle=%d state=%s.\r\n",
		    handle,
		    socketEntry.state == SOCKET_STATE::CONNECTED ? "connected" : "connecting");
		return handle;
	}

	int32 OpenTcpListener(uint32 remoteAddress, uint16 remotePort, uint16 localPort, uint32 asrPtr)
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP listen filter=%s:%u localPort=%u.\r\n",
		    FormatIpv4Address(remoteAddress).c_str(),
		    ntohs(remotePort),
		    ntohs(localPort));
		auto hostSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if(hostSocket == INVALID_SOCKET) return -2;
		if(!SetNonBlocking(hostSocket))
		{
			CloseHostSocket(hostSocket);
			return -2;
		}

		int reuseAddress = 1;
		setsockopt(hostSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));

		sockaddr_in local = {};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port = localPort;
		if(bind(hostSocket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP listen bind() failed error=%d.\r\n", GetLastSocketError());
			CloseHostSocket(hostSocket);
			return -3;
		}
		if(listen(hostSocket, 4) != 0)
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP listen listen() failed error=%d.\r\n", GetLastSocketError());
			CloseHostSocket(hostSocket);
			return -7;
		}

		int handle = AllocateTcpSocket(hostSocket);
		if(handle < 0)
		{
			CloseHostSocket(hostSocket);
			return -2;
		}
		auto& socketEntry = m_tcpSockets[handle];
		socketEntry.state = SOCKET_STATE::LISTENING;
		socketEntry.remoteAddress = remoteAddress;
		socketEntry.remotePort = remotePort;
		socketEntry.localPort = localPort;
		socketEntry.asrPtr = asrPtr;
		RefreshSocketAddresses(socketEntry);
		CLog::GetInstance().Print(LOG_NAME, "TCP listener handle=%d boundPort=%u.\r\n", handle, ntohs(socketEntry.localPort));
		return handle;
	}

	int32 AcceptNow(unsigned int listenerHandle, uint32 outputPtr, uint32 acceptedAsrPtr)
	{
		auto& listener = m_tcpSockets[listenerHandle];
		sockaddr_in remote = {};
		socklen_t remoteSize = sizeof(remote);
		auto acceptedSocket = accept(listener.socket, reinterpret_cast<sockaddr*>(&remote), &remoteSize);
		if(acceptedSocket == INVALID_SOCKET)
		{
			return IsWouldBlockError() ? -1 : AVE_ERROR_CONNECTION;
		}
		if(!SetNonBlocking(acceptedSocket))
		{
			CloseHostSocket(acceptedSocket);
			return AVE_ERROR_CONNECTION;
		}

		auto acceptedHandle = AllocateTcpSocket(acceptedSocket);
		if(acceptedHandle < 0)
		{
			CloseHostSocket(acceptedSocket);
			return -2;
		}
		auto& accepted = m_tcpSockets[acceptedHandle];
		accepted.state = SOCKET_STATE::CONNECTED;
		accepted.asrPtr = acceptedAsrPtr;
		accepted.remoteAddress = remote.sin_addr.s_addr;
		accepted.remotePort = remote.sin_port;
		RefreshSocketAddresses(accepted);
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP accept listener=%u handle=%d peer=%s:%u.\r\n",
		    listenerHandle,
		    acceptedHandle,
		    FormatIpv4Address(accepted.remoteAddress).c_str(),
		    ntohs(accepted.remotePort));
		if(outputPtr != 0)
		{
			Write<uint32>(outputPtr + 0, accepted.remoteAddress);
			Write<uint16>(outputPtr + 4, accepted.localPort);
			Write<uint16>(outputPtr + 6, accepted.remotePort);
		}
		return acceptedHandle;
	}

	int32 AcceptTcp(CMIPS& context, int32 listenerHandle, uint32 callbackPtr, uint32 acceptedAsrPtr, uint32 outputPtr)
	{
		auto listener = GetTcpSocket(listenerHandle);
		if(!listener || (listener->state != SOCKET_STATE::LISTENING)) return AVE_ERROR_BAD_HANDLE;
		auto result = AcceptNow(listenerHandle, outputPtr, acceptedAsrPtr);
		if(result >= 0) return result;
		if(result != -1) return result;
		if(listener->accept.active) return AVE_ERROR_BUSY;

		listener->accept.active = true;
		listener->accept.callbackPtr = callbackPtr;
		listener->accept.acceptedAsrPtr = acceptedAsrPtr;
		listener->accept.outputPtr = outputPtr;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP accept pending listener=%d callback=0x%08X acceptedAsr=0x%08X output=0x%08X.\r\n",
		    listenerHandle,
		    callbackPtr,
		    acceptedAsrPtr,
		    outputPtr);
		if(callbackPtr == 0)
		{
			listener->accept.threadId = SuspendCurrentThread();
			if(listener->accept.threadId < 0)
			{
				listener->accept = {};
				return AVE_ERROR_BUSY;
			}
		}
		return -1;
	}

	int32 SendBuffer(CMIPS& context, int32 handle, std::vector<uint8> data, uint32 callbackPtr, uint32 completionArg = 0)
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP send handle=%d size=%u callback=0x%08X.\r\n",
		    handle,
		    static_cast<unsigned int>(data.size()),
		    callbackPtr);
		auto socket = GetTcpSocket(handle);
		if(!socket || (socket->state != SOCKET_STATE::CONNECTED)) return AVE_ERROR_BAD_HANDLE;
		if(socket->send.active) return AVE_ERROR_BUSY;
		if(data.empty())
		{
			if(callbackPtr != 0)
			{
				m_bios.TriggerCallback(callbackPtr, 0, handle, completionArg);
				return 0;
			}
			return 0;
		}

		auto result = send(
		    socket->socket,
		    reinterpret_cast<const char*>(data.data()),
		    static_cast<int>(data.size()),
		    GetSendFlags());
		if(result > 0)
		{
			socket->bytesSent += result;
			LogTcpPayload("guest->host", handle, data.data(), result);
		}
		if(result < 0)
		{
			if(!IsWouldBlockError())
			{
				CLog::GetInstance().Print(
				    LOG_NAME,
				    "TCP send failed handle=%d error=%d.\r\n",
				    handle,
				    GetLastSocketError());
				return AVE_ERROR_CONNECTION;
			}
			result = 0;
		}
		socket->send.active = true;
		socket->send.data = std::move(data);
		socket->send.offset = result;
		socket->send.callbackPtr = callbackPtr;
		socket->send.completionArg = completionArg;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP send handle=%d queued completion offset=%u/%u callback=0x%08X.\r\n",
		    handle,
		    static_cast<unsigned int>(socket->send.offset),
		    static_cast<unsigned int>(socket->send.data.size()),
		    callbackPtr);
		if(callbackPtr == 0)
		{
			socket->send.threadId = SuspendCurrentThread();
			if(socket->send.threadId < 0)
			{
				socket->send = {};
				return AVE_ERROR_BUSY;
			}
		}
		return callbackPtr ? 0 : -1;
	}

	int32 SendVectors(CMIPS& context, uint32 inputPtr)
	{
		auto handle = static_cast<int16>(Read<uint16>(inputPtr + 0));
		auto callbackPtr = Read<uint32>(inputPtr + 4);
		auto vectorCount = Read<uint8>(inputPtr + 9);
		if(vectorCount >= 5) return -12;
		std::vector<uint8> data;
		for(unsigned int i = 0; i < vectorCount; i++)
		{
			auto length = Read<uint16>(inputPtr + 12 + (i * 8));
			auto dataPtr = Read<uint32>(inputPtr + 16 + (i * 8));
			if((length == 0) || (dataPtr == 0)) continue;
			auto oldSize = data.size();
			data.resize(oldSize + length);
			memcpy(data.data() + oldSize, m_ram + dataPtr, length);
		}
		return SendBuffer(context, handle, std::move(data), callbackPtr);
	}

	int32 SendLinear(CMIPS& context, int32 handle, uint32 bufferPtr, uint16 size)
	{
		std::vector<uint8> data(size);
		if((size != 0) && (bufferPtr != 0))
		{
			memcpy(data.data(), m_ram + bufferPtr, size);
		}
		return SendBuffer(context, handle, std::move(data), 0);
	}

	int32 ReceiveBuffer(CMIPS& context, int32 handle, uint32 bufferPtr, uint16 size, uint32 callbackPtr)
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP receive handle=%d size=%u callback=0x%08X.\r\n",
		    handle,
		    size,
		    callbackPtr);
		auto socket = GetTcpSocket(handle);
		if(!socket || ((socket->state != SOCKET_STATE::CONNECTED) && (socket->state != SOCKET_STATE::PEER_CLOSED)))
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP receive rejected handle=%d result=%d (invalid state).\r\n", handle, AVE_ERROR_BAD_HANDLE);
			return AVE_ERROR_BAD_HANDLE;
		}
		if(socket->state == SOCKET_STATE::PEER_CLOSED)
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "TCP receive rejected handle=%d result=%d (peer already closed; callback not dispatched).\r\n",
			    handle,
			    AVE_ERROR_NOT_CONNECTED);
			return AVE_ERROR_NOT_CONNECTED;
		}
		if(socket->receive.active) return AVE_ERROR_BUSY;
		if((bufferPtr == 0) || (size == 0)) return 0;

		auto result = recv(socket->socket, reinterpret_cast<char*>(m_ram + bufferPtr), size, 0);
		if(result > 0)
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP receive handle=%d completed size=%d.\r\n", handle, result);
			socket->bytesReceived += result;
			LogTcpPayload("host->guest", handle, m_ram + bufferPtr, result);
			socket->dataNotified = false;
			if(callbackPtr != 0)
			{
				m_bios.TriggerCallback(callbackPtr, result, handle, 0);
				return 0;
			}
			return result;
		}
		if(result == 0)
		{
			CLog::GetInstance().Print(LOG_NAME, "TCP receive handle=%d peer closed.\r\n", handle);
			if(callbackPtr != 0)
			{
				m_bios.TriggerCallback(callbackPtr, AVE_ERROR_CLOSED, handle, 0);
				NotifyPeerClosed(handle);
				return 0;
			}
			NotifyPeerClosed(handle);
			return AVE_ERROR_CLOSED;
		}
		if(!IsWouldBlockError())
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "TCP receive failed handle=%d error=%d.\r\n",
			    handle,
			    GetLastSocketError());
			return AVE_ERROR_CONNECTION;
		}

		socket->receive.active = true;
		socket->receive.bufferPtr = bufferPtr;
		socket->receive.size = size;
		socket->receive.callbackPtr = callbackPtr;
		CLog::GetInstance().Print(LOG_NAME, "TCP receive handle=%d pending.\r\n", handle);
		if(callbackPtr == 0)
		{
			socket->receive.threadId = SuspendCurrentThread();
			if(socket->receive.threadId < 0)
			{
				socket->receive = {};
				return AVE_ERROR_BUSY;
			}
		}
		return callbackPtr ? 0 : -1;
	}

	int32 Receive(CMIPS& context, uint32 inputPtr)
	{
		return ReceiveBuffer(
		    context,
		    static_cast<int16>(Read<uint16>(inputPtr + 0)),
		    Read<uint32>(inputPtr + 4),
		    Read<uint16>(inputPtr + 2),
		    Read<uint32>(inputPtr + 8));
	}

	int32 TcpStat(int32 handle, uint32 outputPtr)
	{
		auto socket = GetTcpSocket(handle);
		if(!socket) return AVE_ERROR_BAD_HANDLE;
		int16 state = -1;
		switch(socket->state)
		{
		case SOCKET_STATE::LISTENING:
			state = 1;
			break;
		case SOCKET_STATE::CONNECTING:
			state = 2;
			break;
		case SOCKET_STATE::CONNECTED:
			state = 4;
			break;
		case SOCKET_STATE::PEER_CLOSED:
		case SOCKET_STATE::FAILED:
			state = 10;
			break;
		default:
			break;
		}

		u_long available = 0;
		if(socket->socket != INVALID_SOCKET)
		{
#ifdef _WIN32
			ioctlsocket(socket->socket, FIONREAD, &available);
#else
			ioctl(socket->socket, FIONREAD, &available);
#endif
		}
		auto pendingSendSize = socket->send.active ? (socket->send.data.size() - socket->send.offset) : 0;
		auto sendWindowSpace =
		    TCP_WINDOW_SIZE - static_cast<uint16>(std::min<size_t>(pendingSendSize, TCP_WINDOW_SIZE));
		if(outputPtr != 0)
		{
			Write<int16>(outputPtr + 0, state);
			Write<int16>(outputPtr + 2, 0);
			Write<uint16>(outputPtr + 4, sendWindowSpace);
			Write<uint16>(outputPtr + 6, static_cast<uint16>(std::min<u_long>(available, 0xFFFF)));
		}
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "TCP stat handle=%d state=%d acceptPending=0 sendWindow=%u receiveAvailable=%u sendPending=%u receivePending=%u.\r\n",
		    handle,
		    state,
		    sendWindowSpace,
		    static_cast<unsigned int>(available),
		    socket->send.active,
		    socket->receive.active);
		if(socket->state == SOCKET_STATE::FAILED) return AVE_ERROR_CONNECTION;
		return 0;
	}

	int32 GetTcpAddress(int32 handle, uint32 outputPtr)
	{
		auto socket = GetTcpSocket(handle);
		if(!socket) return AVE_ERROR_BAD_HANDLE;
		RefreshSocketAddresses(*socket);
		Write<uint32>(outputPtr + 0, socket->remoteAddress);
		Write<uint16>(outputPtr + 4, socket->localPort);
		Write<uint16>(outputPtr + 6, socket->remotePort);
		return 0;
	}

	int32 CancelTcp(int32 handle)
	{
		auto socket = GetTcpSocket(handle);
		if(!socket) return AVE_ERROR_BAD_HANDLE;
		if(socket->receive.active) CompleteReceive(handle, AVE_ERROR_CONNECTION);
		if(socket->send.active) CompleteSend(handle, AVE_ERROR_CONNECTION);
		return 0;
	}

	UDP_SOCKET* GetUdpSocket(int32 handle)
	{
		if((handle & 0xC0) != 0x80) return nullptr;
		auto index = handle & 0x3F;
		if(index >= static_cast<int32>(m_udpSockets.size())) return nullptr;
		auto& socket = m_udpSockets[index];
		return socket.used ? &socket : nullptr;
	}

	int AllocateUdpSocket(SOCKET hostSocket)
	{
		for(unsigned int index = 0; index < m_udpSockets.size(); index++)
		{
			auto& socket = m_udpSockets[index];
			if(socket.used || (socket.callbackStage != UDP_CALLBACK_STAGE::IDLE)) continue;
			socket = {};
			socket.socket = hostSocket;
			socket.used = true;
			return index;
		}
		return -1;
	}

	int32 OpenUdp(uint32 inputPtr)
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "UDP open filter=%s:%u localPort=%u receiver=0x%08X.\r\n",
		    FormatIpv4Address(Read<uint32>(inputPtr + 0)).c_str(),
		    ntohs(Read<uint16>(inputPtr + 4)),
		    ntohs(Read<uint16>(inputPtr + 6)),
		    Read<uint32>(inputPtr + 24));
		auto hostSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if(hostSocket == INVALID_SOCKET) return -2;
		if(!SetNonBlocking(hostSocket))
		{
			CloseHostSocket(hostSocket);
			return -2;
		}
		int broadcast = 1;
		setsockopt(hostSocket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

		auto localPort = Read<uint16>(inputPtr + 6);
		sockaddr_in local = {};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port = localPort;
		if(bind(hostSocket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
		{
			CLog::GetInstance().Print(LOG_NAME, "UDP bind() failed error=%d.\r\n", GetLastSocketError());
			CloseHostSocket(hostSocket);
			return -3;
		}

		auto index = AllocateUdpSocket(hostSocket);
		if(index < 0)
		{
			CloseHostSocket(hostSocket);
			return -2;
		}
		auto& socket = m_udpSockets[index];
		socket.remoteAddress = Read<uint32>(inputPtr + 0);
		socket.remotePort = Read<uint16>(inputPtr + 4);
		socket.asrPtr = Read<uint32>(inputPtr + 20);
		socket.receiverPtr = Read<uint32>(inputPtr + 24);
		socklen_t localSize = sizeof(local);
		if(getsockname(hostSocket, reinterpret_cast<sockaddr*>(&local), &localSize) == 0)
		{
			socket.localPort = local.sin_port;
		}
		CLog::GetInstance().Print(LOG_NAME, "UDP open handle=%d boundPort=%u.\r\n", index | 0x80, ntohs(socket.localPort));
		return index | 0x80;
	}

	int32 CloseUdp(int32 handle)
	{
		CLog::GetInstance().Print(LOG_NAME, "UDP close handle=%d.\r\n", handle);
		auto socket = GetUdpSocket(handle);
		if(!socket) return AVE_ERROR_BAD_HANDLE;
		CloseHostSocket(socket->socket);
		socket->socket = INVALID_SOCKET;
		socket->used = false;
		if(socket->asrPtr != 0)
		{
			m_bios.TriggerCallback(socket->asrPtr, handle, 255);
		}
		if(socket->callbackStage == UDP_CALLBACK_STAGE::IDLE)
		{
			*socket = {};
		}
		return 0;
	}

	void ResetNetwork()
	{
		ResetTcp();
		for(unsigned int index = 0; index < m_udpSockets.size(); index++)
		{
			if(m_udpSockets[index].used)
			{
				CloseUdp(index | 0x80);
			}
		}
	}

	int32 SetUdpAsr(int32 handle, uint32 asrPtr)
	{
		auto socket = GetUdpSocket(handle);
		if(!socket) return AVE_ERROR_BAD_HANDLE;
		socket->asrPtr = asrPtr;
		CLog::GetInstance().Print(LOG_NAME, "UDP ASR registered handle=%d callback=0x%08X.\r\n", handle, asrPtr);
		return 0;
	}

	int32 GetUdpAddress(int32 handle, uint32 outputPtr)
	{
		auto socket = GetUdpSocket(handle);
		if(!socket) return AVE_ERROR_BAD_HANDLE;
		Write<uint32>(outputPtr + 0, socket->remoteAddress);
		Write<uint16>(outputPtr + 4, socket->localPort);
		Write<uint16>(outputPtr + 6, socket->remotePort);
		return 0;
	}

	int32 SendUdp(uint32 inputPtr, bool extended)
	{
		auto handle = static_cast<int16>(Read<uint16>(inputPtr + 0));
		auto socket = GetUdpSocket(handle);
		if(!socket) return AVE_ERROR_BAD_HANDLE;
		auto vectorCount = Read<uint8>(inputPtr + 10);
		if(vectorCount >= 5) return -12;

		auto vectorBase = extended ? 16 : 12;
		std::vector<uint8> data;
		for(unsigned int i = 0; i < vectorCount; i++)
		{
			auto length = Read<uint16>(inputPtr + vectorBase + (i * 8));
			auto dataPtr = Read<uint32>(inputPtr + vectorBase + 4 + (i * 8));
			if((length == 0) || (dataPtr == 0)) continue;
			auto oldSize = data.size();
			data.resize(oldSize + length);
			memcpy(data.data() + oldSize, m_ram + dataPtr, length);
		}

		sockaddr_in remote = {};
		remote.sin_family = AF_INET;
		remote.sin_addr.s_addr = Read<uint32>(inputPtr + 4);
		remote.sin_port = Read<uint16>(inputPtr + 8);
		auto result = sendto(
		    socket->socket,
		    reinterpret_cast<const char*>(data.data()),
		    static_cast<int>(data.size()),
		    GetSendFlags(),
		    reinterpret_cast<const sockaddr*>(&remote),
		    sizeof(remote));
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "UDP packet forwarded direction=guest->host handle=%d remote=%s:%u bytes=%u result=%d error=%d hex=[%s].\r\n",
		    handle,
		    FormatIpv4Address(remote.sin_addr.s_addr).c_str(),
		    ntohs(remote.sin_port),
		    static_cast<unsigned int>(data.size()),
		    result,
		    result < 0 ? GetLastSocketError() : 0,
		    FormatHexPreview(data.data(), data.size()).c_str());
		return result == static_cast<int>(data.size()) ? 0 : AVE_ERROR_BUSY;
	}

	bool IsCallbackComplete(int32 threadId)
	{
		auto thread = m_bios.GetThread(threadId);
		return !thread || (thread->status == CIopBios::THREAD_STATUS_DORMANT);
	}

	int32 GetCallbackResult(int32 threadId)
	{
		auto thread = m_bios.GetThread(threadId);
		return thread ? static_cast<int32>(thread->context.gpr[CMIPS::V0]) : 0;
	}

	void PollUdpCallback(unsigned int index)
	{
		auto& socket = m_udpSockets[index];
		if(socket.callbackStage == UDP_CALLBACK_STAGE::IDLE) return;
		if(!IsCallbackComplete(socket.callbackThreadId)) return;

		auto parameterPtr = m_udpCallbackParamPtrs[index];
		if(socket.callbackStage == UDP_CALLBACK_STAGE::RESERVE_BUFFER)
		{
			if(GetCallbackResult(socket.callbackThreadId) != 0)
			{
				socket.callbackStage = UDP_CALLBACK_STAGE::IDLE;
				socket.callbackThreadId = -1;
				socket.receiveData.clear();
				if(!socket.used) socket = {};
				return;
			}
			auto destinationPtr = Read<uint32>(parameterPtr + 20);
			auto size = std::min<size_t>(Read<uint16>(parameterPtr + 8), socket.receiveData.size());
			if((destinationPtr != 0) && (size != 0))
			{
				memcpy(m_ram + destinationPtr, socket.receiveData.data(), size);
				Write<uint16>(parameterPtr + 8, static_cast<uint16>(size));
			}
			socket.callbackStage = UDP_CALLBACK_STAGE::RELEASE_BUFFER;
			socket.callbackThreadId = m_bios.TriggerCallback(socket.receiverPtr, 1, parameterPtr);
			return;
		}

		socket.callbackStage = UDP_CALLBACK_STAGE::IDLE;
		socket.callbackThreadId = -1;
		socket.receiveData.clear();
		if(!socket.used)
		{
			socket = {};
		}
	}

	void PollUdpReceive(unsigned int index)
	{
		auto& socket = m_udpSockets[index];
		if(!socket.used || (socket.callbackStage != UDP_CALLBACK_STAGE::IDLE) || (socket.receiverPtr == 0)) return;
		if(!IsSocketReady(socket.socket, true, false)) return;

		std::vector<uint8> data(0x10000);
		sockaddr_in remote = {};
		socklen_t remoteSize = sizeof(remote);
		auto result = recvfrom(
		    socket.socket,
		    reinterpret_cast<char*>(data.data()),
		    static_cast<int>(data.size()),
		    0,
		    reinterpret_cast<sockaddr*>(&remote),
		    &remoteSize);
		if(result <= 0) return;
		if((socket.remoteAddress != 0) && (socket.remoteAddress != remote.sin_addr.s_addr)) return;
		if((socket.remotePort != 0) && (socket.remotePort != remote.sin_port)) return;

		data.resize(result);
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "UDP packet forwarded direction=host->guest handle=%u peer=%s:%u bytes=%d hex=[%s].\r\n",
		    index | 0x80,
		    FormatIpv4Address(remote.sin_addr.s_addr).c_str(),
		    ntohs(remote.sin_port),
		    result,
		    FormatHexPreview(data.data(), data.size()).c_str());
		auto& parameterPtr = m_udpCallbackParamPtrs[index];
		if(parameterPtr == 0)
		{
			parameterPtr = m_bios.GetSysmem()->AllocateMemory(24, 0, 0);
			if(parameterPtr == 0) return;
		}
		memset(m_ram + parameterPtr, 0, 24);
		Write<uint16>(parameterPtr + 0, static_cast<uint16>(index | 0x80));
		Write<uint16>(parameterPtr + 8, static_cast<uint16>(std::min<int>(result, 0x7FFF)));
		Write<uint32>(parameterPtr + 12, remote.sin_addr.s_addr);
		Write<uint16>(parameterPtr + 16, remote.sin_port);

		socket.receiveData = std::move(data);
		socket.callbackStage = UDP_CALLBACK_STAGE::RESERVE_BUFFER;
		socket.callbackThreadId = m_bios.TriggerCallback(socket.receiverPtr, 0, parameterPtr);
	}

	void PollConnect(unsigned int handle)
	{
		auto& socket = m_tcpSockets[handle];
		if(socket.connectNotifyPending)
		{
			socket.connectNotifyPending = false;
			TriggerAsr(handle, 1);
			return;
		}
		if((socket.state != SOCKET_STATE::CONNECTING) || !IsSocketReady(socket.socket, false, true)) return;

		int error = 0;
		socklen_t errorSize = sizeof(error);
		if(getsockopt(socket.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorSize) != 0)
		{
			error = -1;
		}
		if(error == 0)
		{
			socket.state = SOCKET_STATE::CONNECTED;
			RefreshSocketAddresses(socket);
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "TCP connected handle=%u localPort=%u remote=%s:%u.\r\n",
			    handle,
			    ntohs(socket.localPort),
			    FormatIpv4Address(socket.remoteAddress).c_str(),
			    ntohs(socket.remotePort));
			TriggerAsr(handle, 1);
		}
		else
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "TCP connect failed handle=%u remote=%s:%u error=%d.\r\n",
			    handle,
			    FormatIpv4Address(socket.remoteAddress).c_str(),
			    ntohs(socket.remotePort),
			    error);
			socket.state = SOCKET_STATE::FAILED;
			CloseHostSocket(socket.socket);
			socket.socket = INVALID_SOCKET;
			TriggerAsr(handle, 10);
		}
	}

	void PollAccept(unsigned int handle)
	{
		auto& listener = m_tcpSockets[handle];
		if(!listener.accept.active || !IsSocketReady(listener.socket, true, false)) return;
		auto pending = listener.accept;
		auto acceptedHandle = AcceptNow(handle, pending.outputPtr, pending.acceptedAsrPtr);
		if(acceptedHandle < 0) return;
		listener.accept = {};
		if(pending.callbackPtr != 0)
		{
			auto& accepted = m_tcpSockets[acceptedHandle];
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "TCP accept callback=0x%08X listener=%u accepted=%d peer=%s:%u.\r\n",
			    pending.callbackPtr,
			    handle,
			    acceptedHandle,
			    FormatIpv4Address(accepted.remoteAddress).c_str(),
			    ntohs(accepted.remotePort));
			m_bios.TriggerCallback(pending.callbackPtr, acceptedHandle, handle, accepted.remoteAddress, accepted.remotePort);
		}
		else
		{
			CompleteThread(pending.threadId, acceptedHandle);
		}
	}

	void PollSend(unsigned int handle)
	{
		auto& socket = m_tcpSockets[handle];
		if(!socket.send.active) return;
		if(socket.send.offset == socket.send.data.size())
		{
			CompleteSend(handle, static_cast<int32>(socket.send.data.size()));
			return;
		}
		if(!IsSocketReady(socket.socket, false, true)) return;
		auto remaining = socket.send.data.size() - socket.send.offset;
		auto result = send(
		    socket.socket,
		    reinterpret_cast<const char*>(socket.send.data.data() + socket.send.offset),
		    static_cast<int>(remaining),
		    GetSendFlags());
		if(result < 0)
		{
			if(IsWouldBlockError()) return;
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "TCP pending send failed handle=%u offset=%u remaining=%u error=%d.\r\n",
			    handle,
			    static_cast<unsigned int>(socket.send.offset),
			    static_cast<unsigned int>(remaining),
			    GetLastSocketError());
			CompleteSend(handle, AVE_ERROR_CONNECTION);
			return;
		}
		if(result > 0)
		{
			auto oldOffset = socket.send.offset;
			socket.bytesSent += result;
			LogTcpPayload("guest->host", handle, socket.send.data.data() + oldOffset, result);
		}
		socket.send.offset += result;
		if(socket.send.offset == socket.send.data.size())
		{
			CompleteSend(handle, static_cast<int32>(socket.send.data.size()));
		}
	}

	void PollReceive(unsigned int handle)
	{
		auto& socket = m_tcpSockets[handle];
		if((socket.state != SOCKET_STATE::CONNECTED) && (socket.state != SOCKET_STATE::PEER_CLOSED)) return;
		if(!IsSocketReady(socket.socket, true, false)) return;

		if(socket.receive.active)
		{
			auto result = recv(
			    socket.socket,
			    reinterpret_cast<char*>(m_ram + socket.receive.bufferPtr),
			    socket.receive.size,
			    0);
			if(result > 0)
			{
				socket.bytesReceived += result;
				LogTcpPayload("host->guest", handle, m_ram + socket.receive.bufferPtr, result);
				CompleteReceive(handle, result);
			}
			else if(result == 0)
			{
				CompleteReceive(handle, AVE_ERROR_CLOSED);
				NotifyPeerClosed(handle);
			}
			else if(!IsWouldBlockError())
			{
				CLog::GetInstance().Print(
				    LOG_NAME,
				    "TCP pending receive failed handle=%u error=%d.\r\n",
				    handle,
				    GetLastSocketError());
				CompleteReceive(handle, AVE_ERROR_CONNECTION);
				socket.state = SOCKET_STATE::FAILED;
				TriggerAsr(handle, 10);
			}
			return;
		}

		char probe = 0;
		auto probeResult = recv(socket.socket, &probe, sizeof(probe), MSG_PEEK);
		if(probeResult == 0)
		{
			NotifyPeerClosed(handle);
		}
		else if((probeResult > 0) && !socket.dataNotified)
		{
			socket.dataNotified = true;
			TriggerAsr(handle, 3);
		}
		else if((probeResult < 0) && !IsWouldBlockError())
		{
			socket.state = SOCKET_STATE::FAILED;
			TriggerAsr(handle, 10);
		}
	}

	void CountTicks(uint32)
	{
		for(unsigned int handle = 0; handle < m_tcpSockets.size(); handle++)
		{
			auto& socket = m_tcpSockets[handle];
			if(socket.state == SOCKET_STATE::CLOSED) continue;
			PollConnect(handle);
			if(socket.state == SOCKET_STATE::LISTENING)
			{
				PollAccept(handle);
				continue;
			}
			if((socket.state == SOCKET_STATE::CONNECTED) || (socket.state == SOCKET_STATE::PEER_CLOSED))
			{
				PollSend(handle);
				PollReceive(handle);
			}
		}
		for(unsigned int index = 0; index < m_udpSockets.size(); index++)
		{
			PollUdpCallback(index);
			PollUdpReceive(index);
		}
	}

	int32 InvokeTcpApi(CMIPS& context, uint16 functionCode, uint32 inputPtr, uint32 outputPtr)
	{
		if((functionCode != 0x4119) && (functionCode != 0x4176) && (functionCode != 0x417A))
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "AT_apiCall code=0x%04X input=0x%08X output=0x%08X.\r\n",
			    functionCode,
			    inputPtr,
			    outputPtr);
		}
		switch(functionCode)
		{
		case 0x4100:
			return -32;
		case 0x4101:
			if(m_configPtr == 0)
			{
				m_configPtr = m_bios.GetSysmem()->AllocateMemory(12, 0, 0);
			}
			if(m_configPtr == 0) return -12;
			Write<uint32>(m_configPtr + 0, m_ipAddress);
			Write<uint32>(m_configPtr + 4, m_netMask);
			Write<uint32>(m_configPtr + 8, m_gateway);
			Write<uint32>(outputPtr, m_configPtr);
			return 0;
		case 0x4102:
		{
			auto handle = static_cast<int16>(Read<uint16>(inputPtr));
			if((handle & 0xC0) == 0x80)
			{
				return SetUdpAsr(handle, Read<uint32>(inputPtr + 4));
			}
			auto socket = GetTcpSocket(handle);
			if(!socket) return AVE_ERROR_BAD_HANDLE;
			socket->asrPtr = Read<uint32>(inputPtr + 4);
			return 0;
		}
		case 0x4103:
			return AVE_ERROR_BUSY;
		case 0x4104:
			return -32;
		case 0x4105:
			m_ipAddress = Read<uint32>(inputPtr + 4);
			m_netMask = Read<uint32>(inputPtr + 8);
			m_gateway = Read<uint32>(inputPtr + 12);
			m_interfaceUp = true;
			return 0;
		case 0x4106:
			m_interfaceUp = false;
			return 0;
		case 0x4108:
			return 0;
		case 0x4109:
			return Read<uint16>(inputPtr + 4);
		case 0x4110:
			return OpenTcp(
			    Read<uint32>(inputPtr + 0),
			    Read<uint16>(inputPtr + 4),
			    Read<uint16>(inputPtr + 6),
			    Read<uint32>(inputPtr + 20));
		case 0x4111:
			return OpenTcpListener(
			    Read<uint32>(inputPtr + 0),
			    Read<uint16>(inputPtr + 4),
			    Read<uint16>(inputPtr + 6),
			    0);
		case 0x4112:
			return AcceptTcp(
			    context,
			    static_cast<int16>(Read<uint16>(inputPtr + 0)),
			    Read<uint32>(inputPtr + 4),
			    Read<uint32>(inputPtr + 8),
			    outputPtr);
		case 0x4113:
		case 0x4115:
		case 0x411C:
		{
			auto handle = static_cast<int16>(Read<uint16>(inputPtr));
			if((handle & 0xC0) == 0x80)
			{
				return CloseUdp(handle);
			}
			if(!GetTcpSocket(handle)) return AVE_ERROR_BAD_HANDLE;
			CloseTcpSocket(handle, functionCode != 0x411C);
			return 0;
		}
		case 0x4114:
		{
			auto socket = GetTcpSocket(static_cast<int16>(Read<uint16>(inputPtr)));
			if(!socket) return AVE_ERROR_BAD_HANDLE;
			return ShutdownHostSocket(socket->socket) == 0 ? 0 : AVE_ERROR_BAD_HANDLE;
		}
		case 0x4116:
		{
			auto handle = static_cast<int16>(Read<uint16>(inputPtr));
			return (handle & 0xC0) == 0x80 ? GetUdpAddress(handle, outputPtr) : GetTcpAddress(handle, outputPtr);
		}
		case 0x4117:
			return TcpStat(static_cast<int16>(Read<uint16>(inputPtr)), outputPtr);
		case 0x4118:
			return SendVectors(context, inputPtr);
		case 0x4119:
			return Receive(context, inputPtr);
		case 0x411A:
			return CancelTcp(static_cast<int16>(Read<uint16>(inputPtr)));
		case 0x411B:
		{
			auto size = Read<uint16>(inputPtr + 10);
			auto dataPtr = Read<uint32>(inputPtr + 12);
			std::vector<uint8> data(size);
			if(size != 0) memcpy(data.data(), m_ram + dataPtr, size);
			return SendBuffer(
			    context,
			    static_cast<int16>(Read<uint16>(inputPtr)),
			    std::move(data),
			    Read<uint32>(inputPtr + 4),
			    dataPtr);
		}
		case 0x4120:
			return OpenUdp(inputPtr);
		case 0x4121:
			return SendUdp(inputPtr, false);
		case 0x4122:
			return SendUdp(inputPtr, true);
		case 0x4124:
			return -2;
		case 0x4125:
			return AVE_ERROR_BUSY;
		case 0x412E:
			return 0;
		case 0x412F:
			return 0;
		case 0x4130:
		case 0x4131:
		case 0x4132:
		case 0x4134:
		case 0x4135:
		case 0x4136:
			return 0;
		case 0x4141:
		case 0x4142:
			return 0;
		case 0x4143:
			Write<uint32>(outputPtr, GetDriverEntry());
			return 0;
		case 0x4144:
			return 0;
		case 0x4151:
			m_modemPropertyPtr = inputPtr;
			m_pppConnected = false;
			return 0;
		case 0x4152:
			m_pppConnected = false;
			return 0;
		case 0x4153:
			Write<uint32>(outputPtr, 0);
			return 0;
		case 0x4154:
			m_pppConnected = true;
			return 0;
		case 0x4155:
			m_pppConnected = false;
			return 0;
		case 0x4156:
			m_modemPropertyPtr = inputPtr;
			Write<uint32>(outputPtr, m_modemPropertyPtr);
			return 0;
		case 0x4157:
			Write<uint32>(outputPtr, m_modemPropertyPtr);
			return 0;
		case 0x4158:
			return WritePppStatus(outputPtr);
		case 0x4159:
			return -1;
		case 0x415A:
			return SetPppOption(
			    Read<uint16>(inputPtr + 0),
			    Read<uint16>(inputPtr + 2),
			    Read<uint32>(inputPtr + 4));
		case 0x415B:
			return GetPppOption(
			    Read<uint16>(inputPtr + 0),
			    Read<uint16>(inputPtr + 2),
			    Read<uint32>(inputPtr + 4));
		case 0x4161:
			m_ndgInitialized = true;
			m_ndgStarted = false;
			return 0;
		case 0x4162:
			m_ndgInitialized = false;
			m_ndgStarted = false;
			return 0;
		case 0x4163:
			Write<uint32>(outputPtr, GetDriverEntry());
			return 0;
		case 0x4164:
		{
			if(!m_ndgInitialized) return -1;
			auto mode = inputPtr ? Read<uint16>(inputPtr + 2) : 1;
			if(mode == 3)
			{
				CLog::GetInstance().Print(
				    LOG_NAME,
				    "NDG readiness poll result=%d.\r\n",
				    m_ndgStarted ? 1 : 0);
				return m_ndgStarted ? 1 : 0;
			}
			m_ndgStarted = true;
			CLog::GetInstance().Print(LOG_NAME, "NDG started mode=%u.\r\n", mode);
			return 0;
		}
		case 0x4165:
			m_ndgStarted = false;
			return 0;
		case 0x4171:
			return DhcpInitialize();
		case 0x4172:
			return DhcpTerminate();
		case 0x4173:
			return DhcpGetDns(
			    Read<uint32>(inputPtr + 4),
			    Read<uint32>(inputPtr + 8),
			    Read<uint32>(inputPtr + 12));
		case 0x4174:
			return DhcpGetGateway(Read<uint32>(inputPtr + 4));
		case 0x4175:
			return DhcpGetLeaseTime(
			    Read<uint32>(inputPtr + 0),
			    Read<uint32>(inputPtr + 4),
			    Read<uint32>(inputPtr + 8));
		case 0x4176:
			return m_dhcpState;
		case 0x4177:
			return DhcpSetHostname(Read<uint32>(inputPtr));
		case 0x4178:
		case 0x4179:
			return DhcpRequest(
			    Read<uint32>(inputPtr + 16),
			    Read<uint32>(inputPtr + 20),
			    Read<uint32>(inputPtr + 24));
		case 0x417A:
			return m_dhcpState;
		case 0x417B:
		case 0x417C:
			return DhcpRelease();
		case 0x4181:
			return DnsInitialize();
		case 0x4182:
			return DnsFinalize();
		case 0x4183:
			return DnsGetTicket(Read<uint32>(inputPtr), Read<uint32>(inputPtr + 4));
		case 0x4184:
			return DnsReleaseTicket(static_cast<int16>(Read<uint16>(inputPtr)));
		case 0x4185:
		{
			uint16 count = 0;
			auto result = DnsLookup(
			    static_cast<int16>(Read<uint16>(inputPtr)),
			    Read<uint32>(inputPtr + 4),
			    Read<uint16>(inputPtr + 8),
			    &count);
			return result == 0 ? count : result;
		}
		case 0x4186:
		{
			uint16 count = 0;
			auto result = DnsLookupReverse(
			    static_cast<int16>(Read<uint16>(inputPtr)),
			    Read<uint32>(inputPtr + 4),
			    &count);
			return result == 0 ? count : result;
		}
		case 0x4187:
		{
			auto ticketId = static_cast<int16>(Read<uint16>(inputPtr));
			auto infoPtr = Read<uint32>(inputPtr + 4);
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "DNS ticket info requested ticket=%d output=0x%08X (no extra information).\r\n",
			    ticketId,
			    infoPtr);
			Write<uint32>(infoPtr, 0);
			return 0;
		}
		default:
			CLog::GetInstance().Warn(LOG_NAME, "Unsupported AVETCP API call (0x%04X).\r\n", functionCode);
			return -32;
		}
	}

	int32 WritePppStatus(uint32 statusPtr)
	{
		Write<uint32>(statusPtr + 0, m_ipAddress);
		Write<uint32>(statusPtr + 4, m_gateway);
		Write<int16>(statusPtr + 8, m_pppConnected ? 4 : 0);
		Write<uint32>(statusPtr + 12, m_pppConnected ? 100000000 : 0);
		Write<int16>(statusPtr + 16, 0);
		Write<int16>(statusPtr + 18, 0);
		return 0;
	}

	uint32 GetDriverEntry()
	{
		if(m_driverEntry <= 1)
		{
			auto driverEntry = m_bios.GetSysmem()->AllocateMemory(4, 0, 0);
			if(driverEntry != 0)
			{
				m_driverEntry = driverEntry;
				Write<uint32>(m_driverEntry, 0);
			}
		}
		return m_driverEntry;
	}

	int32 SetPppOption(uint16 option, uint16 length, uint32 valuePtr)
	{
		if((length != 4) || (valuePtr == 0)) return -1;
		auto value = Read<uint32>(valuePtr);
		switch(option)
		{
		case 1:
			if(value == 0) return -1;
			m_pppTimerInterval = value;
			return 0;
		case 2:
			if(value > 1) return -1;
			m_pppoeEnabled = value;
			return 0;
		default:
			return -1;
		}
	}

	int32 GetPppOption(uint16 option, uint16 length, uint32 valuePtr)
	{
		if((option != 1) || (length < 4) || (valuePtr == 0)) return -1;
		Write<uint32>(valuePtr, m_pppTimerInterval);
		return 4;
	}

	int32 DhcpInitialize()
	{
		CLog::GetInstance().Print(LOG_NAME, "DHCP initialize.\r\n");
		m_dhcpState = 0;
		m_interfaceUp = false;
		return 0;
	}

	int32 DhcpTerminate()
	{
		CLog::GetInstance().Print(LOG_NAME, "DHCP terminate.\r\n");
		m_dhcpState = 0;
		m_interfaceUp = false;
		return 0;
	}

	int32 DhcpRequest(uint32 ipAddressPtr, uint32 netMaskPtr, uint32 broadcastPtr)
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DHCP request outputs ip=0x%08X mask=0x%08X broadcast=0x%08X.\r\n",
		    ipAddressPtr,
		    netMaskPtr,
		    broadcastPtr);
		if((ipAddressPtr == 0) || (netMaskPtr == 0) || (broadcastPtr == 0))
		{
			CLog::GetInstance().Print(LOG_NAME, "DHCP request rejected because an output pointer is null.\r\n");
			return -1;
		}
		Write<uint32>(ipAddressPtr, m_ipAddress);
		Write<uint32>(netMaskPtr, m_netMask);
		Write<uint32>(broadcastPtr, m_broadcast);
		m_dhcpState = 3;
		m_interfaceUp = true;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DHCP bound lease=%s mask=%s broadcast=%s gateway=%s.\r\n",
		    FormatIpv4Address(m_ipAddress).c_str(),
		    FormatIpv4Address(m_netMask).c_str(),
		    FormatIpv4Address(m_broadcast).c_str(),
		    FormatIpv4Address(m_gateway).c_str());
		return 0;
	}

	int32 DhcpGetDns(uint32 domainPtr, uint32 primaryPtr, uint32 secondaryPtr)
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DHCP get DNS state=%d domain=0x%08X primary=0x%08X secondary=0x%08X.\r\n",
		    m_dhcpState,
		    domainPtr,
		    primaryPtr,
		    secondaryPtr);
		if(m_dhcpState < 3) return -1;
		if(domainPtr != 0) *reinterpret_cast<char*>(m_ram + domainPtr) = 0;
		Write<uint32>(primaryPtr, m_dnsPrimary);
		Write<uint32>(secondaryPtr, m_dnsSecondary);
		return 0;
	}

	int32 DhcpGetGateway(uint32 gatewayPtr)
	{
		CLog::GetInstance().Print(LOG_NAME, "DHCP get gateway state=%d output=0x%08X.\r\n", m_dhcpState, gatewayPtr);
		if(m_dhcpState < 3) return -1;
		Write<uint32>(gatewayPtr, m_gateway);
		return 0;
	}

	int32 DhcpGetLeaseTime(uint32 leasePtr, uint32 renewalPtr, uint32 rebindingPtr)
	{
		if(m_dhcpState < 3) return -1;
		Write<uint32>(leasePtr, 86400);
		Write<uint32>(renewalPtr, 43200);
		Write<uint32>(rebindingPtr, 75600);
		return 0;
	}

	int32 DhcpSetHostname(uint32 hostnamePtr)
	{
		if(hostnamePtr == 0) return -1;
		auto hostname = reinterpret_cast<const char*>(m_ram + hostnamePtr);
		if(strlen(hostname) >= 32) return -1;
		m_hostname = hostname;
		return 0;
	}

	int32 DhcpRelease()
	{
		CLog::GetInstance().Print(LOG_NAME, "DHCP release.\r\n");
		m_dhcpState = 0;
		m_interfaceUp = false;
		return 0;
	}

	int32 DnsInitialize()
	{
		CLog::GetInstance().Print(LOG_NAME, "DNS initialize (host resolver).\r\n");
		for(auto& ticket : m_dnsTickets)
		{
			ticket = {};
		}
		return 0;
	}

	int32 DnsFinalize()
	{
		CLog::GetInstance().Print(LOG_NAME, "DNS finalize; releasing all host resolver tickets.\r\n");
		for(auto& ticket : m_dnsTickets)
		{
			ticket = {};
		}
		return 0;
	}

	int32 DnsGetTicket(uint32 hostnamePtr, uint32 optionPtr)
	{
		if(hostnamePtr == 0)
		{
			CLog::GetInstance().Print(LOG_NAME, "DNS ticket request rejected: hostname pointer is null.\r\n");
			return -1;
		}
		auto hostname = reinterpret_cast<const char*>(m_ram + hostnamePtr);
		for(unsigned int ticketId = 0; ticketId < m_dnsTickets.size(); ticketId++)
		{
			auto& ticket = m_dnsTickets[ticketId];
			if(ticket.used) continue;
			ticket.used = true;
			ticket.hostname = hostname;
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "DNS ticket allocated ticket=%u host='%s' hostnamePtr=0x%08X optionPtr=0x%08X.\r\n",
			    ticketId,
			    hostname,
			    hostnamePtr,
			    optionPtr);
			return ticketId;
		}
		CLog::GetInstance().Print(LOG_NAME, "DNS ticket request failed for host='%s': ticket table full.\r\n", hostname);
		return -1;
	}

	int32 DnsReleaseTicket(int32 ticketId)
	{
		if((ticketId < 0) || (ticketId >= static_cast<int32>(m_dnsTickets.size())))
		{
			CLog::GetInstance().Print(LOG_NAME, "DNS release rejected ticket=%d.\r\n", ticketId);
			return -1;
		}
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DNS ticket released ticket=%d host='%s'.\r\n",
		    ticketId,
		    m_dnsTickets[ticketId].hostname.c_str());
		m_dnsTickets[ticketId] = {};
		return 0;
	}

	int32 DnsLookup(int32 ticketId, uint32 outputPtr, uint16 maxResults, uint16* resultCount)
	{
		if(resultCount) *resultCount = 0;
		if((ticketId < 0) || (ticketId >= static_cast<int32>(m_dnsTickets.size()))) return -1;
		auto& ticket = m_dnsTickets[ticketId];
		if(!ticket.used || (outputPtr == 0) || (maxResults == 0)) return -1;

		ticket.addresses.clear();
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DNS lookup ticket=%d host='%s' max=%u.\r\n",
		    ticketId,
		    ticket.hostname.c_str(),
		    maxResults);
		auto host = gethostbyname(ticket.hostname.c_str());
		if(!host || (host->h_addrtype != AF_INET) || (host->h_length != 4))
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "DNS lookup failed ticket=%d host='%s' resolverError=%d socketError=%d.\r\n",
			    ticketId,
			    ticket.hostname.c_str(),
			    h_errno,
			    GetLastSocketError());
			return -1;
		}
		for(auto address = host->h_addr_list; *address && (ticket.addresses.size() < maxResults); address++)
		{
			uint32 ipv4 = 0;
			memcpy(&ipv4, *address, sizeof(ipv4));
			if(std::find(ticket.addresses.begin(), ticket.addresses.end(), ipv4) == ticket.addresses.end())
			{
				ticket.addresses.push_back(ipv4);
			}
		}
		if(ticket.addresses.empty()) return -1;
		memcpy(m_ram + outputPtr, ticket.addresses.data(), ticket.addresses.size() * sizeof(uint32));
		if(resultCount) *resultCount = static_cast<uint16>(ticket.addresses.size());
		for(auto address : ticket.addresses)
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "DNS result ticket=%d address=%s.\r\n",
			    ticketId,
			    FormatIpv4Address(address).c_str());
		}
		return 0;
	}

	int32 DnsLookupReverse(int32 ticketId, uint32 outputPtrPtr, uint16* resultCount)
	{
		if(resultCount) *resultCount = 0;
		if((ticketId < 0) || (ticketId >= static_cast<int32>(m_dnsTickets.size()))) return -1;
		auto& ticket = m_dnsTickets[ticketId];
		if(!ticket.used || (outputPtrPtr == 0)) return -1;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DNS reverse lookup ticket=%d source='%s' outputPtrPtr=0x%08X.\r\n",
		    ticketId,
		    ticket.hostname.c_str(),
		    outputPtrPtr);

		uint32 address = MakeIpv4Address(ticket.hostname.c_str());
		if(address == INADDR_NONE)
		{
			auto forward = gethostbyname(ticket.hostname.c_str());
			if(!forward || !forward->h_addr_list[0] || (forward->h_length != 4))
			{
				CLog::GetInstance().Print(
				    LOG_NAME,
				    "DNS reverse prerequisite lookup failed ticket=%d resolverError=%d.\r\n",
				    ticketId,
				    h_errno);
				return -1;
			}
			memcpy(&address, forward->h_addr_list[0], sizeof(address));
		}
		auto host = gethostbyaddr(reinterpret_cast<const char*>(&address), sizeof(address), AF_INET);
		if(!host || !host->h_name)
		{
			CLog::GetInstance().Print(
			    LOG_NAME,
			    "DNS reverse lookup failed ticket=%d address=%s resolverError=%d.\r\n",
			    ticketId,
			    FormatIpv4Address(address).c_str(),
			    h_errno);
			return -1;
		}
		auto outputPtr = Read<uint32>(outputPtrPtr);
		if(outputPtr == 0) return -1;
		strncpy(reinterpret_cast<char*>(m_ram + outputPtr), host->h_name, 255);
		m_ram[outputPtr + 255] = 0;
		if(resultCount) *resultCount = 1;
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DNS reverse result ticket=%d address=%s host='%s'.\r\n",
		    ticketId,
		    FormatIpv4Address(address).c_str(),
		    host->h_name);
		return 0;
	}

	int32 InvokeTcp(CMIPS& context, unsigned int functionId)
	{
		switch(functionId)
		{
		case 0:
		case 1:
		case 2:
		case 3:
			return 0;
		case 4:
			return InvokeTcpApi(
			    context,
			    static_cast<uint16>(GetArgument(context, 0)),
			    GetArgument(context, 1),
			    GetArgument(context, 2));
		case 5:
			return DnsInitialize();
		case 6:
			return DnsFinalize();
		case 7:
			return DnsGetTicket(GetArgument(context, 0), GetArgument(context, 1));
		case 8:
			return DnsReleaseTicket(static_cast<int16>(GetArgument(context, 0)));
		case 9:
		{
			uint16 count = 0;
			auto result = DnsLookup(
			    static_cast<int16>(GetArgument(context, 0)),
			    GetArgument(context, 1),
			    static_cast<uint16>(GetArgument(context, 2)),
			    &count);
			Write<uint16>(GetArgument(context, 3), count);
			return result;
		}
		case 10:
		{
			uint16 count = 0;
			auto result = DnsLookupReverse(
			    static_cast<int16>(GetArgument(context, 0)),
			    GetArgument(context, 1),
			    &count);
			Write<uint16>(GetArgument(context, 3), count);
			return result;
		}
		case 11:
			Write<uint32>(GetArgument(context, 1), 0);
			return 0;
		case 12:
			return rand();
		case 13:
			srand(GetArgument(context, 0));
			return 0;
		case 14:
			return strcmp(
			    reinterpret_cast<const char*>(m_ram + GetArgument(context, 0)),
			    reinterpret_cast<const char*>(m_ram + GetArgument(context, 1)));
		case 15:
			strcpy(
			    reinterpret_cast<char*>(m_ram + GetArgument(context, 0)),
			    reinterpret_cast<const char*>(m_ram + GetArgument(context, 1)));
			return GetArgument(context, 0);
		case 16:
			return static_cast<int32>(strlen(reinterpret_cast<const char*>(m_ram + GetArgument(context, 0))));
		case 17:
			return strncmp(
			    reinterpret_cast<const char*>(m_ram + GetArgument(context, 0)),
			    reinterpret_cast<const char*>(m_ram + GetArgument(context, 1)),
			    GetArgument(context, 2));
		case 18:
		{
			auto first = reinterpret_cast<const uint8*>(m_ram + GetArgument(context, 0));
			auto second = reinterpret_cast<const uint8*>(m_ram + GetArgument(context, 1));
			for(uint32 i = 0; i < GetArgument(context, 2); i++)
			{
				auto left = static_cast<uint8>(std::tolower(first[i]));
				auto right = static_cast<uint8>(std::tolower(second[i]));
				if(left != right) return static_cast<int32>(left) - static_cast<int32>(right);
				if(left == 0) break;
			}
			return 0;
		}
		case 19:
			strcat(
			    reinterpret_cast<char*>(m_ram + GetArgument(context, 0)),
			    reinterpret_cast<const char*>(m_ram + GetArgument(context, 1)));
			return GetArgument(context, 0);
		case 20:
			return 0;
		case 21:
			m_interfaceUp = false;
			return 0;
		case 22:
			m_interfaceUp = true;
			return 0;
		case 23:
			m_driverEntry = GetArgument(context, 1);
			return 0;
		case 24:
			m_driverEntry = 0;
			return 0;
		case 25:
			return GetDriverEntry();
		case 26:
		{
			auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
			return static_cast<uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
		}
		case 27:
			return 0;
		case 32:
			ResetNetwork();
			return 0;
		case 36:
			ResetNetwork();
			return 0;
		case 37:
			return memcmp(m_ram + GetArgument(context, 0), m_ram + GetArgument(context, 1), GetArgument(context, 2));
		case 38:
			memcpy(m_ram + GetArgument(context, 0), m_ram + GetArgument(context, 1), GetArgument(context, 2));
			return GetArgument(context, 0);
		case 39:
			memmove(m_ram + GetArgument(context, 0), m_ram + GetArgument(context, 1), GetArgument(context, 2));
			return GetArgument(context, 0);
		case 40:
			memset(m_ram + GetArgument(context, 0), GetArgument(context, 1), GetArgument(context, 2));
			return GetArgument(context, 0);
		case 49:
		case 50:
		case 51:
			return 0;
		case 52:
			return 0;
		default:
			return 0;
		}
	}

	int32 InvokePpp(CMIPS& context, unsigned int functionId)
	{
		switch(functionId)
		{
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
			m_pppConnected = false;
			return 0;
		case 5:
			m_pppConnected = false;
			return 0;
		case 6:
			m_pppConnected = true;
			return 0;
		case 7:
			m_pppConnected = false;
			return 0;
		case 8:
			return WritePppStatus(GetArgument(context, 0));
		case 9:
			ResetNetwork();
			return 0;
		case 10:
			ResetNetwork();
			return 0;
		case 11:
			return OpenTcp(
			    GetArgument(context, 0),
			    static_cast<uint16>(GetArgument(context, 1)),
			    static_cast<uint16>(GetArgument(context, 2)),
			    0);
		case 12:
		case 13:
		{
			auto handle = static_cast<int16>(GetArgument(context, 0));
			if(!GetTcpSocket(handle)) return AVE_ERROR_BAD_HANDLE;
			CloseTcpSocket(handle, functionId == 13);
			return 0;
		}
		case 14:
			return TcpStat(static_cast<int16>(GetArgument(context, 0)), GetArgument(context, 1));
		case 15:
			return SendLinear(
			    context,
			    static_cast<int16>(GetArgument(context, 0)),
			    GetArgument(context, 1),
			    static_cast<uint16>(GetArgument(context, 2)));
		case 16:
			return ReceiveBuffer(
			    context,
			    static_cast<int16>(GetArgument(context, 0)),
			    GetArgument(context, 1),
			    static_cast<uint16>(GetArgument(context, 2)),
			    0);
		case 17:
			m_pppConnected = false;
			return 0;
		case 18:
			m_pppConnected = false;
			return 0;
		case 19:
			return 0;
		case 20:
			m_pppConnected = true;
			return 0;
		case 21:
			m_pppConnected = false;
			return 0;
		case 22:
			return InvokePpp(context, 8);
		case 23:
			m_modemPropertyPtr = GetArgument(context, 0);
			return m_modemPropertyPtr;
		case 24:
			return m_modemPropertyPtr;
		case 25:
			return -1;
		case 26:
			return SetPppOption(
			    static_cast<uint16>(GetArgument(context, 0)),
			    static_cast<uint16>(GetArgument(context, 1)),
			    GetArgument(context, 2));
		case 27:
			return GetPppOption(
			    static_cast<uint16>(GetArgument(context, 0)),
			    static_cast<uint16>(GetArgument(context, 1)),
			    GetArgument(context, 2));
		default:
			return 0;
		}
	}

	int32 InvokeDhcp(CMIPS& context, unsigned int functionId)
	{
		switch(functionId)
		{
		case 0:
		case 1:
		case 2:
		case 3:
			return 0;
		case 4:
			return DhcpInitialize();
		case 5:
			return DhcpGetDns(GetArgument(context, 1), GetArgument(context, 2), GetArgument(context, 3));
		case 6:
			return DhcpGetGateway(GetArgument(context, 1));
		case 7:
			return DhcpGetLeaseTime(GetArgument(context, 0), GetArgument(context, 1), GetArgument(context, 2));
		case 8:
			return m_dhcpState;
		case 9:
			return DhcpSetHostname(GetArgument(context, 0));
		case 10:
		case 11:
			return DhcpRequest(GetArgument(context, 4), GetArgument(context, 5), GetArgument(context, 6));
		case 12:
			return DhcpTerminate();
		case 13:
			return m_dhcpState;
		case 14:
		case 15:
			return DhcpRelease();
		default:
			return -1;
		}
	}

	CIopBios& m_bios;
	uint8* m_ram = nullptr;
	std::array<TCP_SOCKET, MAX_TCP_SOCKETS> m_tcpSockets;
	std::array<UDP_SOCKET, MAX_UDP_SOCKETS> m_udpSockets;
	std::array<uint32, MAX_UDP_SOCKETS> m_udpCallbackParamPtrs = {};
	std::array<DNS_TICKET, MAX_DNS_TICKETS> m_dnsTickets;
	uint32 m_ipAddress = 0;
	uint32 m_netMask = 0;
	uint32 m_broadcast = 0;
	uint32 m_gateway = 0;
	uint32 m_dnsPrimary = 0;
	uint32 m_dnsSecondary = 0;
	uint32 m_configPtr = 0;
	uint32 m_driverEntry = 1;
	uint32 m_modemPropertyPtr = 0;
	uint32 m_pppTimerInterval = 200;
	uint32 m_pppoeEnabled = 0;
	int32 m_dhcpState = 0;
	bool m_interfaceUp = false;
	bool m_pppConnected = false;
	bool m_ndgInitialized = false;
	bool m_ndgStarted = false;
	std::string m_hostname;
};

CAveNetworkContext::CAveNetworkContext(CIopBios& bios, uint8* ram)
    : m_impl(std::make_unique<Implementation>(bios, ram))
{
}

CAveNetworkContext::~CAveNetworkContext() = default;

int32 CAveNetworkContext::InvokeTcp(CMIPS& context, unsigned int functionId)
{
	return m_impl->InvokeTcp(context, functionId);
}

int32 CAveNetworkContext::InvokePpp(CMIPS& context, unsigned int functionId)
{
	return m_impl->InvokePpp(context, functionId);
}

int32 CAveNetworkContext::InvokeDhcp(CMIPS& context, unsigned int functionId)
{
	return m_impl->InvokeDhcp(context, functionId);
}

void CAveNetworkContext::CountTicks(uint32 ticks)
{
	m_impl->CountTicks(ticks);
}

CAveTcp::CAveTcp(const std::shared_ptr<CAveNetworkContext>& context)
    : m_context(context)
{
}

std::string CAveTcp::GetId() const
{
	return "avetcp";
}

std::string CAveTcp::GetFunctionName(unsigned int functionId) const
{
	static const char* functionNames[] = {
	    "aveTcpInit",
	    "_retonly",
	    "_retonly",
	    "_retonly",
	    "AT_apiCall",
	    "DNS_Initialize",
	    "DNS_Finalize",
	    "DNS_GetTicket",
	    "DNS_ReleaseTicket",
	    "DNS_LookUp",
	    "DNS_LookUp_Rev",
	    "DNS_GetTicketInfo",
	    "AU_rand",
	    "AU_srand",
	    "AU_strcmp",
	    "AU_strcpy",
	    "AU_strlen",
	    "AU_strncmp",
	    "AU_strnicmp",
	    "AU_strcat",
	    "AT_DL_GetIfEntry",
	    "AT_PKTDRV_IfDown",
	    "AT_PKTDRV_IfUp",
	    "AT_NDI_addDriverEntry",
	    "AT_NDI_delDriverEntry",
	    "AT_NDI_getDriverEntry",
	    "AT_OS_GetCounter",
	    "AT_OS_Wait",
	    "AT_cre_sem",
	    "AT_cre_tsk",
	    "AT_del_sem",
	    "AT_del_tsk",
	    "AT_disp",
	    "AT_ext_tsk",
	    "AT_get_tim",
	    "AT_halt",
	    "AT_init",
	    "AT_memcmp",
	    "AT_memcpy",
	    "AT_memmove",
	    "AT_memset",
	    "AT_sig_sem",
	    "AT_sta_tsk",
	    "AT_tslp_tsk",
	    "AT_wai_sem",
	    "AT_InetAllocPkt",
	    "AT_InetFreePkt",
	    "AT_InetSeekPkt",
	    "headbufchk",
	    "AT_addPppApi",
	    "AT_addNdgApi",
	    "AT_addDhcpApi",
	    "AT_PS2_GetInitParam",
	};
	return functionId < std::size(functionNames) ? functionNames[functionId] : "unknown";
}

void CAveTcp::Invoke(CMIPS& context, unsigned int functionId)
{
	if(functionId != 4)
	{
		CLog::GetInstance().Print(LOG_NAME, "AVETCP export=%u (%s).\r\n", functionId, GetFunctionName(functionId).c_str());
	}
	context.m_State.nGPR[CMIPS::V0].nD0 = m_context->InvokeTcp(context, functionId);
}

void CAveTcp::CountTicks(uint32 ticks)
{
	m_context->CountTicks(ticks);
}

CAvePpp::CAvePpp(const std::shared_ptr<CAveNetworkContext>& context)
    : m_context(context)
{
}

std::string CAvePpp::GetId() const
{
	return "aveppp";
}

std::string CAvePpp::GetFunctionName(unsigned int functionId) const
{
	static const char* functionNames[] = {
	    "avePppInit",
	    "_retonly",
	    "_retonly",
	    "_retonly",
	    "AvepppInitialize",
	    "AvepppTerminate",
	    "AvepppOpen",
	    "AvepppClose",
	    "AvepppGetStatus",
	    "AvetcpInitialize",
	    "AvetcpTerminate",
	    "AvetcpOpen",
	    "AvetcpClose",
	    "AvetcpAbort",
	    "AvetcpStat",
	    "AvetcpSend",
	    "AvetcpReceive",
	    "PP_init",
	    "PP_disp",
	    "PP_getDriverEntry",
	    "PP_start",
	    "PP_finish",
	    "PP_status",
	    "PP_setModemProperty",
	    "PP_getModemProperty",
	    "PP_getUsbDeviceId",
	    "PP_setOption",
	    "PP_getOption",
	};
	return functionId < std::size(functionNames) ? functionNames[functionId] : "unknown";
}

void CAvePpp::Invoke(CMIPS& context, unsigned int functionId)
{
	CLog::GetInstance().Print(LOG_NAME, "AVEPPP export=%u (%s).\r\n", functionId, GetFunctionName(functionId).c_str());
	context.m_State.nGPR[CMIPS::V0].nD0 = m_context->InvokePpp(context, functionId);
}

CAveDhcp::CAveDhcp(const std::shared_ptr<CAveNetworkContext>& context)
    : m_context(context)
{
}

std::string CAveDhcp::GetId() const
{
	return "avedhcp";
}

std::string CAveDhcp::GetFunctionName(unsigned int functionId) const
{
	static const char* functionNames[] = {
	    "aveDhcpInit",
	    "_retonly",
	    "_retonly",
	    "_retonly",
	    "DHCP_init",
	    "DHCP_get_dns",
	    "DHCP_get_gateway",
	    "DHCP_get_leasetime",
	    "DHCP_get_state",
	    "DHCP_hostname",
	    "DHCP_request",
	    "DHCP_request_nb",
	    "DHCP_terminate",
	    "DHCP_timer",
	    "DHCP_release",
	    "DHCP_release_nb",
	};
	return functionId < std::size(functionNames) ? functionNames[functionId] : "unknown";
}

void CAveDhcp::Invoke(CMIPS& context, unsigned int functionId)
{
	CLog::GetInstance().Print(LOG_NAME, "AVEDHCP export=%u (%s).\r\n", functionId, GetFunctionName(functionId).c_str());
	context.m_State.nGPR[CMIPS::V0].nD0 = m_context->InvokeDhcp(context, functionId);
}

CAveDevGlue::CAveDevGlue(uint8* ram)
    : m_ram(ram)
{
	CLog::GetInstance().Print(
	    LOG_NAME,
	    "DEVGLUE HLE created (device=2 class=0 vendor='ADMtek' product='PegasusIII' module='an986').\r\n");
}

std::string CAveDevGlue::GetId() const
{
	return "devglue";
}

std::string CAveDevGlue::GetFunctionName(unsigned int functionId) const
{
	switch(functionId)
	{
	case 4:
		return "DG_getDeviceInfo";
	case 5:
		return "DG_selectDevice";
	case 6:
		return "DG_getDeviceInfoNum";
	case 7:
		return "DG_setOption";
	case 8:
		return "DG_getOption";
	default:
		return "unknown";
	}
}

void CAveDevGlue::FillEthernetInfo(uint32 infoPtr)
{
	constexpr uint32 deviceInfoSize = 780;
	memset(m_ram + infoPtr, 0, deviceInfoSize);
	WriteGuest<uint32>(m_ram, infoPtr + 0, 2);
	WriteGuest<uint32>(m_ram, infoPtr + 4, 0);
	WriteGuest<uint32>(m_ram, infoPtr + 8, 1);
	WriteGuestString(m_ram, infoPtr + 12, "ADMtek", 256);
	WriteGuestString(m_ram, infoPtr + 268, "PegasusIII", 256);
	WriteGuestString(m_ram, infoPtr + 524, "an986", 256);
}

int32 CAveDevGlue::GetDeviceInfo(uint32 infoPtr, uint32 infoSize)
{
	constexpr uint32 deviceInfoSize = 780;
	if((infoPtr == 0) || (infoSize < deviceInfoSize)) return 0;
	FillEthernetInfo(infoPtr);
	m_changePending = false;
	CLog::GetInstance().Print(LOG_NAME, "DEVGLUE enumerated virtual AN986 device.\r\n");
	return 1;
}

int32 CAveDevGlue::SelectDevice(int32 deviceId)
{
	if(deviceId != 2)
	{
		CLog::GetInstance().Print(LOG_NAME, "DEVGLUE rejected device id=%d.\r\n", deviceId);
		return -1;
	}
	m_selectedDevice = deviceId;
	CLog::GetInstance().Print(LOG_NAME, "DEVGLUE selected virtual AN986 device id=2.\r\n");
	return 0;
}

int32 CAveDevGlue::GetDeviceInfoNum(uint32 countPtr)
{
	WriteGuest<uint32>(m_ram, countPtr, 1);
	return m_changePending ? 1 : 0;
}

int32 CAveDevGlue::SetOption(uint32 option, uint32 valuePtr, uint32 valueSize)
{
	if((valuePtr == 0) || (valueSize != 4)) return -1;
	auto value = ReadGuest<uint32>(m_ram, valuePtr);
	CLog::GetInstance().Print(LOG_NAME, "DEVGLUE set option=%u value=%u size=%u.\r\n", option, value, valueSize);
	switch(option)
	{
	case 1:
		m_threadPriority = value;
		return 0;
	case 2:
		m_deviceThreadPriority = value;
		return 0;
	case 3:
		if(value > 1) return -1;
		m_pppoeEnabled = value;
		return 0;
	case 9:
		m_negotiationMode = value;
		return 0;
	default:
		return -1;
	}
}

int32 CAveDevGlue::GetOption(uint32 option, uint32 valuePtr, uint32 valueSize)
{
	switch(option)
	{
	case 1:
		if((valuePtr == 0) || (valueSize < 4)) return -1;
		WriteGuest<uint32>(m_ram, valuePtr, m_threadPriority);
		return 4;
	case 2:
		if((valuePtr == 0) || (valueSize < 4)) return -1;
		WriteGuest<uint32>(m_ram, valuePtr, m_deviceThreadPriority);
		return 4;
	case 3:
		if((valuePtr == 0) || (valueSize != 4)) return -1;
		WriteGuest<uint32>(m_ram, valuePtr, m_pppoeEnabled);
		return 4;
	case 7:
		if((m_selectedDevice != 2) || (valuePtr == 0) || (valueSize < 780)) return -1;
		FillEthernetInfo(valuePtr);
		return 780;
	case 8:
		if((m_selectedDevice != 2) || (valuePtr == 0) || (valueSize < 4)) return -1;
		WriteGuest<uint32>(m_ram, valuePtr, 1);
		return 4;
	case 9:
		if((m_selectedDevice != 2) || (valuePtr == 0) || (valueSize < 4)) return -1;
		WriteGuest<uint32>(m_ram, valuePtr, m_negotiationMode);
		return 4;
	case 10:
	{
		if((m_selectedDevice != 2) || (valuePtr == 0) || (valueSize < 6)) return -1;
		const uint8 macAddress[6] = {0x02, 0x50, 0xF2, 0x00, 0x00, 0x01};
		memcpy(m_ram + valuePtr, macAddress, sizeof(macAddress));
		return 0;
	}
	default:
		return -1;
	}
}

void CAveDevGlue::Invoke(CMIPS& context, unsigned int functionId)
{
	int32 result = -1;
	switch(functionId)
	{
	case 0:
	case 1:
	case 2:
	case 3:
		result = 0;
		break;
	case 4:
		result = GetDeviceInfo(
		    context.m_State.nGPR[CMIPS::A0].nV0,
		    context.m_State.nGPR[CMIPS::A1].nV0);
		break;
	case 5:
		result = SelectDevice(context.m_State.nGPR[CMIPS::A0].nD0);
		break;
	case 6:
		result = GetDeviceInfoNum(context.m_State.nGPR[CMIPS::A0].nV0);
		break;
	case 7:
		result = SetOption(
		    context.m_State.nGPR[CMIPS::A0].nV0,
		    context.m_State.nGPR[CMIPS::A1].nV0,
		    context.m_State.nGPR[CMIPS::A2].nV0);
		break;
	case 8:
		result = GetOption(
		    context.m_State.nGPR[CMIPS::A0].nV0,
		    context.m_State.nGPR[CMIPS::A1].nV0,
		    context.m_State.nGPR[CMIPS::A2].nV0);
		break;
	default:
		CLog::GetInstance().Warn(LOG_NAME, "Unknown DEVGLUE export=%u.\r\n", functionId);
		break;
	}
	if(((functionId != 6) && (functionId != 8)) || (result < 0))
	{
		CLog::GetInstance().Print(
		    LOG_NAME,
		    "DEVGLUE export=%u (%s) result=%d.\r\n",
		    functionId,
		    GetFunctionName(functionId).c_str(),
		    result);
	}
	context.m_State.nGPR[CMIPS::V0].nD0 = result;
}

std::string CAveAn986::GetId() const
{
	return "an986";
}

std::string CAveAn986::GetFunctionName(unsigned int functionId) const
{
	return functionId < 4 ? "_retonly" : "unknown";
}

void CAveAn986::Invoke(CMIPS& context, unsigned int functionId)
{
	CLog::GetInstance().Print(LOG_NAME, "AN986 HLE export=%u.\r\n", functionId);
	context.m_State.nGPR[CMIPS::V0].nD0 = 0;
}
