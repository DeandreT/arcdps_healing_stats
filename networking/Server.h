#pragma once
#include "ServerStatistics.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif _WIN32
#pragma warning(push, 0)
#pragma warning(disable : 4127)
#pragma warning(disable : 4702)
#pragma warning(disable : 5054)
#pragma warning(disable : 6385)
#pragma warning(disable : 6387)
#pragma warning(disable : 26451)
#pragma warning(disable : 26495)
#endif
#include <evtc_rpc.grpc.pb.h>

#include <grpc/grpc.h>
#include <grpcpp/alarm.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_credentials_options.h>
#ifdef __clang__
#pragma clang diagnostic pop
#elif _WIN32
#pragma warning(pop)
#endif

#include "evtc_rpc_messages.h"

#include <chrono>
#include <deque>
#include <shared_mutex>

class evtc_rpc_server
{
	struct ConnectionContext
	{
		std::map<std::string, std::shared_ptr<ConnectionContext>>::iterator Iterator{}; // Protected by mRegisteredAgentsLock on the server that owns this ConnectionContext
		uint16_t InstanceId = 0; // Protected by mRegisteredAgentsLock on the server that owns this ConnectionContext
		std::map<std::string, uint16_t> Peers; // Protected by mRegisteredAgentsLock on the server that owns this ConnectionContext

		std::atomic<std::chrono::steady_clock::time_point> LastCallTime;

		grpc::ServerContext ServerContext;
		// Reads against the stream are protected since they are serialized, writes are protected with WriteLock
		grpc::ServerAsyncReaderWriter<evtc_rpc::Message, evtc_rpc::Message> Stream{&ServerContext};

		std::mutex WriteLock;
		bool ForceDisconnected = false; // Protected by WriteLock
		bool WritePending = false; // Protected by WriteLock
		std::deque<evtc_rpc::messages::CombatEvent> QueuedEvents; // Protected by WriteLock
	};

	struct CallDataBase
	{
		CallDataBase(CallDataType pType, std::shared_ptr<ConnectionContext>&& pContext)
			: Type{pType}, Context{pContext}
		{
		}

		const CallDataType Type;
		std::shared_ptr<ConnectionContext> Context;
	};

	struct FinishCallData : public CallDataBase
	{
		FinishCallData(std::shared_ptr<ConnectionContext>&& pContext)
			: CallDataBase{CallDataType::Finish, std::move(pContext)}
		{
		}
	};

	struct ConnectCallData : public CallDataBase
	{
		ConnectCallData(std::shared_ptr<ConnectionContext>&& pContext)
			: CallDataBase{CallDataType::Connect, std::move(pContext)}
		{
		}
	};

	struct ReadMessageCallData : public CallDataBase
	{
		ReadMessageCallData(std::shared_ptr<ConnectionContext>&& pContext)
			: CallDataBase{CallDataType::ReadMessage, std::move(pContext)}
		{
		}

		evtc_rpc::Message Message;
	};

	struct WriteEventCallData : public CallDataBase
	{
		WriteEventCallData(std::shared_ptr<ConnectionContext>&& pContext)
			: CallDataBase{CallDataType::WriteEvent, std::move(pContext)}
		{
		}
	};

	struct DisconnectCallData : public CallDataBase
	{
		DisconnectCallData(std::shared_ptr<ConnectionContext>&& pContext)
			: CallDataBase{CallDataType::Disconnect, std::move(pContext)}
		{
		}
	};

	struct WakeUpCallData : public CallDataBase
	{
		WakeUpCallData()
			: CallDataBase{CallDataType::WakeUp, nullptr}
			, Alarm{new grpc::Alarm}
		{
		}

		std::unique_ptr<grpc::Alarm> Alarm;
	};

	enum class ShutdownState
	{
		Online,
		ShouldShutdown,
		ShuttingDown
	};

public:
	evtc_rpc_server(const char* pListeningEndpoint, const char* pPrometheusEndpoint, const grpc::SslServerCredentialsOptions* pCredentialsOptions);
	~evtc_rpc_server();

	ServerStatisticsSample GetStatistics();

	static void ThreadStartServe(void* pThis);
	void Serve();
	void Shutdown();

#ifndef TEST
private:
#endif
	void HandleConnect(ConnectCallData* pCallData);
	void HandleReadMessage(ReadMessageCallData* pCallData);
	void HandleWriteEvent(WriteEventCallData* pCallData);

	const char* HandleRegisterSelf(uint16_t pInstanceId, std::string_view pAccountName, std::shared_ptr<ConnectionContext>& pClient);
	const char* HandleSetSelfId(uint16_t pInstanceId, std::shared_ptr<ConnectionContext>& pClient);
	const char* HandleAddPeer(uint16_t pInstanceId, std::string_view pAccountName, std::shared_ptr<ConnectionContext>& pClient);
	const char* HandleRemovePeer(uint16_t pInstanceId, std::shared_ptr<ConnectionContext>& pClient);
	const char* HandleCombatEvent(const cbtevent& pEvent, std::shared_ptr<ConnectionContext>& pClient);

	void SendEvent(const evtc_rpc::messages::CombatEvent& pEvent, WriteEventCallData* pCallData, const std::shared_ptr<ConnectionContext>& pClient);
	void ForceDisconnect(const char* pErrorMessage, const std::shared_ptr<ConnectionContext>& pClient);
	void ForceDisconnectInternal(const char* pErrorMessage, const std::shared_ptr<ConnectionContext>& pClient, bool pRemovedFromTable);

	std::mutex mRegisteredAgentsLock;
	std::map<std::string, std::shared_ptr<ConnectionContext>> mRegisteredAgents;

	std::shared_ptr<ServerStatistics> mStatistics;
	prometheus::Exposer mPrometheusExposer;

	evtc_rpc::evtc_rpc::AsyncService mService;
	std::unique_ptr<grpc::Server> mServer;
	std::unique_ptr<grpc::ServerCompletionQueue> mCompletionQueue;

	std::shared_mutex mShutdownLock;
	std::atomic<ShutdownState> mShutdownState = ShutdownState::Online;
	
	std::atomic<uint64_t> mConflictingClientDisconnectThresholdMs = 30000;
};
