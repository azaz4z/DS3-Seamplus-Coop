#include "lan_coop_extension.h"
#include "../../network/lan_transport.h"
#include "../../render/title_menu.h"
#include "../../../tools/vendor/minhook-1.3.4/include/MinHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ds3sc::extensions {
namespace {

constexpr uint64_t kLanLobbyId = 0x110000100000001ULL;

// FNV-1a 32-bit hash
uint32_t HashPassword(const std::string& pwd) noexcept {
    uint32_t hash = 0x811c9dc5;
    for (unsigned char c : pwd) {
        hash ^= c;
        hash *= 0x01000193;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Steam Callback Definitions
// ---------------------------------------------------------------------------
struct SteamCallbackHeader {
    uint8_t m_nCallbackFlags;
    int m_iCallback;
};

using CallbackRunFn = void(__thiscall*)(void* pThis, void* pParam);

struct RegisteredCallback {
    void* pCallback;
    int iCallback;
};

std::mutex g_callbackMutex;
std::vector<RegisteredCallback> g_registeredCallbacks;

struct PendingCallback {
    int iCallback;
    std::vector<uint8_t> data;
};

std::mutex g_pendingMutex;
std::vector<PendingCallback> g_pendingCallbacks;

void QueueCallback(int iCallback, const void* pData, size_t size) {
    PendingCallback cb{};
    cb.iCallback = iCallback;
    cb.data.assign(reinterpret_cast<const uint8_t*>(pData),
                   reinterpret_cast<const uint8_t*>(pData) + size);
    std::lock_guard lock(g_pendingMutex);
    g_pendingCallbacks.push_back(std::move(cb));
}

static void SafeInvokeCallback(void* pCallback, uint8_t* pData) noexcept {
    __try {
        void** vtable = *reinterpret_cast<void***>(pCallback);
        if (vtable && vtable[0]) {
            auto run = reinterpret_cast<CallbackRunFn>(vtable[0]);
            run(pCallback, pData);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Ignore transient dispatch exception
    }
}

void DispatchPendingCallbacks() {
    std::vector<PendingCallback> toDispatch;
    {
        std::lock_guard lock(g_pendingMutex);
        if (g_pendingCallbacks.empty()) return;
        toDispatch.swap(g_pendingCallbacks);
    }

    std::lock_guard lock(g_callbackMutex);
    for (const auto& pending : toDispatch) {
        for (const auto& reg : g_registeredCallbacks) {
            if (reg.iCallback == pending.iCallback && reg.pCallback) {
                SafeInvokeCallback(reg.pCallback, const_cast<uint8_t*>(pending.data.data()));
            }
        }
    }
}

// Steam callback structures
#pragma pack(push, 8)
struct LobbyCreated_t {
    enum { k_iCallback = 513 };
    int m_eResult; // 1 = k_EResultOK
    uint64_t m_ulSteamIDLobby;
};

struct LobbyEnter_t {
    enum { k_iCallback = 504 };
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;
    bool m_bLocked;
    uint32_t m_EChatRoomEnterResponse; // 1 = Success
};

struct LobbyMatchList_t {
    enum { k_iCallback = 510 };
    uint32_t m_nLobbiesMatching;
};

struct P2PSessionRequest_t {
    enum { k_iCallback = 1202 };
    uint64_t m_steamIDRemote;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Virtual ISteamMatchmaking009 Implementation
// ---------------------------------------------------------------------------
class VirtualSteamMatchmaking009 {
public:
    void SetReal(void* pReal) noexcept { m_realMatchmaking = pReal; }
    void* GetReal() const noexcept { return m_realMatchmaking; }

private:
    void* m_realMatchmaking = nullptr;

    template <typename Ret, typename... Args>
    Ret CallReal(size_t slot, Args... args) {
        if (!m_realMatchmaking) return Ret{};
        void** vtable = *reinterpret_cast<void***>(m_realMatchmaking);
        using Fn = Ret(*)(void*, Args...);
        auto fn = reinterpret_cast<Fn>(vtable[slot]);
        return fn(m_realMatchmaking, args...);
    }

    template <typename... Args>
    void CallRealVoid(size_t slot, Args... args) {
        if (!m_realMatchmaking) return;
        void** vtable = *reinterpret_cast<void***>(m_realMatchmaking);
        using Fn = void(*)(void*, Args...);
        auto fn = reinterpret_cast<Fn>(vtable[slot]);
        fn(m_realMatchmaking, args...);
    }

public:
    virtual int GetFavoriteGameCount() {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) return CallReal<int>(0);
        return 0;
    }
    virtual bool GetFavoriteGame(int iGame, uint32_t* pnAppID, uint32_t* pnIP, uint16_t* pnConnPort, uint16_t* pnQueryPort, uint32_t* punFlags, uint32_t* pRTime32LastPlayedOnServer) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) return CallReal<bool>(1, iGame, pnAppID, pnIP, pnConnPort, pnQueryPort, punFlags, pRTime32LastPlayedOnServer);
        return false;
    }
    virtual int AddFavoriteGame(uint32_t nAppID, uint32_t nIP, uint16_t nConnPort, uint16_t nQueryPort, uint32_t unFlags, uint32_t rTime32LastPlayedOnServer) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) return CallReal<int>(2, nAppID, nIP, nConnPort, nQueryPort, unFlags, rTime32LastPlayedOnServer);
        return 0;
    }
    virtual bool RemoveFavoriteGame(uint32_t nAppID, uint32_t nIP, uint16_t nConnPort, uint16_t nQueryPort, uint32_t unFlags) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) return CallReal<bool>(3, nAppID, nIP, nConnPort, nQueryPort, unFlags);
        return false;
    }

    virtual uint64_t RequestLobbyList() {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<uint64_t>(4);
        }
        OutputDebugStringA("[ds3sc-lan-coop] RequestLobbyList: Searching LAN beacons...\n");
        auto* inst = GetLanCoopInstance();
        if (inst) {
            inst->TriggerGuestSearch();
        }
        uint16_t port = inst ? inst->GetPort() : network::kDefaultLanPort;
        if (!network::LanTransport::Instance().IsInitialized()) {
            network::LanTransport::Instance().Initialize(port, false);
        }

        uint32_t hash = inst ? inst->GetPasswordHash() : 0;
        std::string hostIp;
        uint16_t hostPort = 0;
        uint64_t hostSteamId = 0;

        bool found = network::LanTransport::Instance().DiscoverHost(
            hostIp, hostPort, hostSteamId, hash, 1500);

        LobbyMatchList_t match{};
        match.m_nLobbiesMatching = found ? 1 : 0;
        QueueCallback(LobbyMatchList_t::k_iCallback, &match, sizeof(match));
        return 0x2001ULL;
    }

    virtual void AddRequestLobbyListStringFilter(const char* pchKeyToMatch, const char* pchValueToMatch, int eComparisonType) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(5, pchKeyToMatch, pchValueToMatch, eComparisonType);
            return;
        }
    }
    virtual void AddRequestLobbyListNumericalFilter(const char* pchKeyToMatch, int nValueToMatch, int eComparisonType) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(6, pchKeyToMatch, nValueToMatch, eComparisonType);
            return;
        }
    }
    virtual void AddRequestLobbyListNearValueFilter(const char* pchKeyToMatch, int nValueToBeCloseTo) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(7, pchKeyToMatch, nValueToBeCloseTo);
            return;
        }
    }
    virtual void AddRequestLobbyListFilterSlotsAvailable(int nSlotsAvailable) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(8, nSlotsAvailable);
            return;
        }
    }
    virtual void AddRequestLobbyListDistanceFilter(int eLobbyDistanceFilter) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(9, eLobbyDistanceFilter);
            return;
        }
    }
    virtual void AddRequestLobbyListResultCountFilter(int cMaxResults) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(10, cMaxResults);
            return;
        }
    }
    virtual void AddRequestLobbyListCompatibleMembersFilter(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(11, steamIDLobby);
            return;
        }
    }

    virtual uint64_t GetLobbyByIndex(int iLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<uint64_t>(12, iLobby);
        }
        return (iLobby == 0) ? kLanLobbyId : 0;
    }

    virtual uint64_t CreateLobby(int eLobbyType, int cMaxMembers) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<uint64_t>(13, eLobbyType, cMaxMembers);
        }
        OutputDebugStringA("[ds3sc-lan-coop] CreateLobby: Starting LAN host beacon...\n");
        auto* inst = GetLanCoopInstance();
        if (inst) {
            inst->TriggerHostSession();
        } else {
            if (!network::LanTransport::Instance().IsInitialized()) {
                network::LanTransport::Instance().Initialize(network::kDefaultLanPort, true);
            }
        }

        LobbyCreated_t created{};
        created.m_eResult = 1; // k_EResultOK
        created.m_ulSteamIDLobby = kLanLobbyId;
        QueueCallback(LobbyCreated_t::k_iCallback, &created, sizeof(created));
        return 0x2002ULL;
    }

    virtual uint64_t JoinLobby(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<uint64_t>(14, steamIDLobby);
        }
        OutputDebugStringA("[ds3sc-lan-coop] JoinLobby: Joining virtual LAN lobby...\n");
        auto* inst = GetLanCoopInstance();
        uint16_t port = inst ? inst->GetPort() : network::kDefaultLanPort;
        if (!network::LanTransport::Instance().IsInitialized()) {
            network::LanTransport::Instance().Initialize(port, false);
        }
        LobbyEnter_t entered{};
        entered.m_ulSteamIDLobby = steamIDLobby;
        entered.m_rgfChatPermissions = 0xFFFFFFFF;
        entered.m_bLocked = false;
        entered.m_EChatRoomEnterResponse = 1; // Success
        QueueCallback(LobbyEnter_t::k_iCallback, &entered, sizeof(entered));
        return 0x2003ULL;
    }

    virtual void LeaveLobby(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(15, steamIDLobby);
            return;
        }
        OutputDebugStringA("[ds3sc-lan-coop] LeaveLobby: Exiting LAN session.\n");
        auto* inst = GetLanCoopInstance();
        if (inst) {
            inst->DissolveSession();
        } else {
            network::LanTransport::Instance().CloseSession();
        }
    }

    virtual bool InviteUserToLobby(uint64_t steamIDLobby, uint64_t steamIDInvitee) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(16, steamIDLobby, steamIDInvitee);
        }
        return true;
    }

    virtual int GetNumLobbyMembers(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<int>(17, steamIDLobby);
        }
        return 2;
    }

    virtual uint64_t GetLobbyMemberByIndex(uint64_t steamIDLobby, int iMember) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<uint64_t>(18, steamIDLobby, iMember);
        }
        return (iMember == 0) ? network::LanTransport::Instance().LocalSteamId() : 0x0110000100000002ULL;
    }

    virtual const char* GetLobbyData(uint64_t steamIDLobby, const char* pchKey) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<const char*>(19, steamIDLobby, pchKey);
        }
        if (!pchKey) return "";
        if (strcmp(pchKey, "ds3sc_hash") == 0) {
            static char hashStr[32];
            auto* inst = GetLanCoopInstance();
            snprintf(hashStr, sizeof(hashStr), "%u", inst ? inst->GetPasswordHash() : 0);
            return hashStr;
        }
        return "";
    }

    virtual bool SetLobbyData(uint64_t steamIDLobby, const char* pchKey, const char* pchValue) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(20, steamIDLobby, pchKey, pchValue);
        }
        return true;
    }

    virtual int GetLobbyDataCount(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<int>(21, steamIDLobby);
        }
        return 1;
    }

    virtual bool GetLobbyDataByIndex(uint64_t steamIDLobby, int iLobbyData, char* pchKey, int cchKeyBufferSize, char* pchValue, int cchValueBufferSize) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(22, steamIDLobby, iLobbyData, pchKey, cchKeyBufferSize, pchValue, cchValueBufferSize);
        }
        return false;
    }

    virtual bool DeleteLobbyData(uint64_t steamIDLobby, const char* pchKey) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(23, steamIDLobby, pchKey);
        }
        return true;
    }

    virtual const char* GetLobbyMemberData(uint64_t steamIDLobby, uint64_t steamIDUser, const char* pchKey) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<const char*>(24, steamIDLobby, steamIDUser, pchKey);
        }
        return "";
    }

    virtual void SetLobbyMemberData(uint64_t steamIDLobby, const char* pchKey, const char* pchValue) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(25, steamIDLobby, pchKey, pchValue);
            return;
        }
    }

    virtual bool SendLobbyChatMsg(uint64_t steamIDLobby, const void* pvMsgBody, int cubMsgBody) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(26, steamIDLobby, pvMsgBody, cubMsgBody);
        }
        return true;
    }

    virtual int GetLobbyChatEntry(uint64_t steamIDLobby, int iChatID, uint64_t* pSteamIDUser, void* pvData, int cubData, int* peChatEntryType) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<int>(27, steamIDLobby, iChatID, pSteamIDUser, pvData, cubData, peChatEntryType);
        }
        return 0;
    }

    virtual bool RequestLobbyData(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(28, steamIDLobby);
        }
        return true;
    }

    virtual void SetLobbyGameServer(uint64_t steamIDLobby, uint32_t unGameServerIP, uint16_t unGameServerPort, uint64_t steamIDGameServer) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            CallRealVoid(29, steamIDLobby, unGameServerIP, unGameServerPort, steamIDGameServer);
            return;
        }
    }

    virtual bool GetLobbyGameServer(uint64_t steamIDLobby, uint32_t* punGameServerIP, uint16_t* punGameServerPort, uint64_t* psteamIDGameServer) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(30, steamIDLobby, punGameServerIP, punGameServerPort, psteamIDGameServer);
        }
        return false;
    }

    virtual bool SetLobbyMemberLimit(uint64_t steamIDLobby, int cMaxMembers) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(31, steamIDLobby, cMaxMembers);
        }
        return true;
    }

    virtual int GetLobbyMemberLimit(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<int>(32, steamIDLobby);
        }
        return 5;
    }

    virtual bool SetLobbyType(uint64_t steamIDLobby, int eLobbyType) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(33, steamIDLobby, eLobbyType);
        }
        return true;
    }

    virtual bool SetLobbyJoinable(uint64_t steamIDLobby, bool bLobbyJoinable) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(34, steamIDLobby, bLobbyJoinable);
        }
        return true;
    }

    virtual uint64_t GetLobbyOwner(uint64_t steamIDLobby) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<uint64_t>(35, steamIDLobby);
        }
        return network::LanTransport::Instance().LocalSteamId();
    }

    virtual bool SetLobbyOwner(uint64_t steamIDLobby, uint64_t steamIDNewOwner) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(36, steamIDLobby, steamIDNewOwner);
        }
        return true;
    }

    virtual bool SetLinkedLobby(uint64_t steamIDLobby, uint64_t steamIDLobbyDependent) {
        if (ds3scConnectionMode == 0 && m_realMatchmaking) {
            return CallReal<bool>(37, steamIDLobby, steamIDLobbyDependent);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Virtual ISteamNetworking005 Implementation
// ---------------------------------------------------------------------------
class VirtualSteamNetworking005 {
public:
    void SetReal(void* pReal) noexcept { m_realNetworking = pReal; }
    void* GetReal() const noexcept { return m_realNetworking; }

private:
    void* m_realNetworking = nullptr;

    template <typename Ret, typename... Args>
    Ret CallReal(size_t slot, Args... args) {
        if (!m_realNetworking) return Ret{};
        void** vtable = *reinterpret_cast<void***>(m_realNetworking);
        using Fn = Ret(*)(void*, Args...);
        auto fn = reinterpret_cast<Fn>(vtable[slot]);
        return fn(m_realNetworking, args...);
    }

public:
    virtual bool SendP2PPacket(uint64_t steamIDRemote, const void* pubData, uint32_t cubData,
                               int eP2PSendType, int nChannel = 0) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(0, steamIDRemote, pubData, cubData, eP2PSendType, nChannel);
        }
        uint64_t myId = network::LanTransport::Instance().LocalSteamId();
        return network::LanTransport::Instance().SendFramed(steamIDRemote, myId, nChannel, pubData, cubData) >= 0;
    }

    virtual bool IsP2PPacketAvailable(uint32_t* pcubMsgSize, int nChannel = 0) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(1, pcubMsgSize, nChannel);
        }
        return network::LanTransport::Instance().IsPacketAvailable(nChannel, pcubMsgSize);
    }

    virtual bool ReadP2PPacket(void* pubDest, uint32_t cubDest, uint32_t* pcubMsgSize,
                               uint64_t* psteamIDRemote, int nChannel = 0) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(2, pubDest, cubDest, pcubMsgSize, psteamIDRemote, nChannel);
        }
        return network::LanTransport::Instance().ReadPacket(pubDest, cubDest, pcubMsgSize, psteamIDRemote, nChannel);
    }

    virtual bool AcceptP2PSessionWithUser(uint64_t steamIDRemote) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(3, steamIDRemote);
        }
        return true;
    }

    virtual bool CloseP2PSessionWithUser(uint64_t steamIDRemote) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(4, steamIDRemote);
        }
        network::LanTransport::Instance().UnregisterPeer(steamIDRemote);
        return true;
    }

    virtual bool CloseP2PChannelWithUser(uint64_t steamIDRemote, int nChannel) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(5, steamIDRemote, nChannel);
        }
        return true;
    }

    virtual bool GetP2PSessionState(uint64_t steamIDRemote, void* pConnectionState) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(6, steamIDRemote, pConnectionState);
        }
        return true;
    }

    virtual bool AllowP2PPacketRelay(bool bAllow) {
        if (ds3scConnectionMode == 0 && m_realNetworking) {
            return CallReal<bool>(7, bAllow);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Virtual ISteamNetworkingMessages002 Implementation
// ---------------------------------------------------------------------------
class VirtualSteamNetworkingMessages002 {
public:
    void SetReal(void* pReal) noexcept { m_realNetworkingMessages = pReal; }
    void* GetReal() const noexcept { return m_realNetworkingMessages; }

private:
    void* m_realNetworkingMessages = nullptr;

    template <typename Ret, typename... Args>
    Ret CallReal(size_t slot, Args... args) {
        if (!m_realNetworkingMessages) return Ret{};
        void** vtable = *reinterpret_cast<void***>(m_realNetworkingMessages);
        using Fn = Ret(*)(void*, Args...);
        auto fn = reinterpret_cast<Fn>(vtable[slot]);
        return fn(m_realNetworkingMessages, args...);
    }

public:
    virtual int SendMessageToUser(const void* identityRemote, const void* pubData,
                                  uint32_t cubData, int nSendFlags, int nRemoteChannel) {
        if (ds3scConnectionMode == 0 && m_realNetworkingMessages) {
            return CallReal<int>(0, identityRemote, pubData, cubData, nSendFlags, nRemoteChannel);
        }
        if (!identityRemote) return 2; // k_EResultFail
        uint64_t targetId = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(identityRemote) + 8);
        uint64_t myId = network::LanTransport::Instance().LocalSteamId();
        int res = network::LanTransport::Instance().SendFramed(targetId, myId, nRemoteChannel, pubData, cubData);
        return (res >= 0) ? 1 : 2; // 1 = k_EResultOK
    }

    virtual int ReceiveMessagesOnChannel(int nLocalChannel, void** ppOutMessages, int nMaxMessages) {
        if (ds3scConnectionMode == 0 && m_realNetworkingMessages) {
            return CallReal<int>(1, nLocalChannel, ppOutMessages, nMaxMessages);
        }
        if (!ppOutMessages || nMaxMessages <= 0) return 0;
        uint32_t msgSize = 0;
        if (!network::LanTransport::Instance().IsPacketAvailable(nLocalChannel, &msgSize)) {
            return 0;
        }
        return 0;
    }

    virtual bool AcceptSessionWithUser(const void* identityRemote) {
        if (ds3scConnectionMode == 0 && m_realNetworkingMessages) {
            return CallReal<bool>(2, identityRemote);
        }
        return true;
    }

    virtual bool CloseSessionWithUser(const void* identityRemote) {
        if (ds3scConnectionMode == 0 && m_realNetworkingMessages) {
            return CallReal<bool>(3, identityRemote);
        }
        if (identityRemote) {
            uint64_t targetId = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(identityRemote) + 8);
            network::LanTransport::Instance().UnregisterPeer(targetId);
        }
        return true;
    }

    virtual bool CloseChannelWithUser(const void* identityRemote, int nChannel) {
        if (ds3scConnectionMode == 0 && m_realNetworkingMessages) {
            return CallReal<bool>(4, identityRemote, nChannel);
        }
        return true;
    }

    virtual int GetSessionConnectionInfo(const void* identityRemote, void* pConnectionInfo, void* pQuickStatus) {
        if (ds3scConnectionMode == 0 && m_realNetworkingMessages) {
            return CallReal<int>(5, identityRemote, pConnectionInfo, pQuickStatus);
        }
        return 1;
    }
};

VirtualSteamMatchmaking009 g_lanMatchmaking;
VirtualSteamNetworking005 g_lanNetworking;
VirtualSteamNetworkingMessages002 g_lanNetworkingMessages;

// ---------------------------------------------------------------------------
// Hook Detours
// ---------------------------------------------------------------------------
using GetGenericInterfaceFn = void*(STDMETHODCALLTYPE*)(void*, int32_t, int32_t, const char*);
GetGenericInterfaceFn g_origGetGenericInterface = nullptr;
void** g_steamClientVTable = nullptr;

void* STDMETHODCALLTYPE DetourGetISteamGenericInterface(void* thisptr, int32_t hUser, int32_t hPipe, const char* pchVersion) {
    void* realInterface = nullptr;
    if (g_origGetGenericInterface) {
        realInterface = g_origGetGenericInterface(thisptr, hUser, hPipe, pchVersion);
    }
    if (pchVersion) {
        if (strcmp(pchVersion, "SteamMatchMaking009") == 0) {
            if (realInterface) g_lanMatchmaking.SetReal(realInterface);
            OutputDebugStringA("[ds3sc-lan-coop] Returning unified VirtualSteamMatchmaking009 proxy\n");
            return &g_lanMatchmaking;
        }
        if (strcmp(pchVersion, "SteamNetworking005") == 0) {
            if (realInterface) g_lanNetworking.SetReal(realInterface);
            OutputDebugStringA("[ds3sc-lan-coop] Returning unified VirtualSteamNetworking005 proxy\n");
            return &g_lanNetworking;
        }
        if (strcmp(pchVersion, "SteamNetworkingMessages002") == 0) {
            if (realInterface) g_lanNetworkingMessages.SetReal(realInterface);
            OutputDebugStringA("[ds3sc-lan-coop] Returning unified VirtualSteamNetworkingMessages002 proxy\n");
            return &g_lanNetworkingMessages;
        }
    }
    return realInterface;
}

using CreateInterfaceFn = void*(*)(const char*, int*);
CreateInterfaceFn g_origCreateInterface = nullptr;

void* DetourCreateInterface(const char* pName, int* pReturnCode) {
    void* result = g_origCreateInterface ? g_origCreateInterface(pName, pReturnCode) : nullptr;
    if (result && pName && strcmp(pName, "SteamClient017") == 0) {
        void** vtable = *reinterpret_cast<void***>(result);
        if (vtable && vtable[12] != reinterpret_cast<void*>(&DetourGetISteamGenericInterface)) {
            g_steamClientVTable = vtable;
            DWORD oldProtect = 0;
            if (VirtualProtect(&vtable[12], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                g_origGetGenericInterface = reinterpret_cast<GetGenericInterfaceFn>(vtable[12]);
                vtable[12] = reinterpret_cast<void*>(&DetourGetISteamGenericInterface);
                VirtualProtect(&vtable[12], sizeof(void*), oldProtect, &oldProtect);
                OutputDebugStringA("[ds3sc-lan-coop] Hooked ISteamClient::GetISteamGenericInterface slot 12\n");
            }
        }
    }
    return result;
}

using RegisterCallbackFn = void(*)(void*, int);
RegisterCallbackFn g_origRegisterCallback = nullptr;

void DetourRegisterCallback(void* pCallback, int iCallback) {
    if (pCallback) {
        std::lock_guard lock(g_callbackMutex);
        g_registeredCallbacks.push_back({pCallback, iCallback});
    }
    if (g_origRegisterCallback) {
        g_origRegisterCallback(pCallback, iCallback);
    }
}

using UnregisterCallbackFn = void(*)(void*);
UnregisterCallbackFn g_origUnregisterCallback = nullptr;

void DetourUnregisterCallback(void* pCallback) {
    if (pCallback) {
        std::lock_guard lock(g_callbackMutex);
        g_registeredCallbacks.erase(
            std::remove_if(g_registeredCallbacks.begin(), g_registeredCallbacks.end(),
                           [pCallback](const RegisteredCallback& r) { return r.pCallback == pCallback; }),
            g_registeredCallbacks.end());
    }
    if (g_origUnregisterCallback) {
        g_origUnregisterCallback(pCallback);
    }
}

using RunCallbacksFn = void(*)();
RunCallbacksFn g_origRunCallbacks = nullptr;

void DetourRunCallbacks() {
    DispatchPendingCallbacks();
    if (g_origRunCallbacks) {
        g_origRunCallbacks();
    }
}

LanCoopExtension* g_lanCoopInstance = nullptr;

} // namespace

LanCoopExtension::LanCoopExtension() noexcept {
    g_lanCoopInstance = this;
}

void LanCoopExtension::LoadSettings() noexcept {
    iniPath_[0] = '\0';
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&g_lanCoopInstance), &hMod) && hMod) {
        char modPath[MAX_PATH] = {};
        GetModuleFileNameA(hMod, modPath, sizeof(modPath));
        char* lastSlash = strrchr(modPath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
            snprintf(iniPath_, sizeof(iniPath_), "%s\\ds3sc_settings.ini", modPath);
        }
    }

    if (iniPath_[0] != '\0') {
        int connMode = GetPrivateProfileIntA("NETWORK", "connection_mode", 0, iniPath_);
        InterlockedExchange(&ds3scConnectionMode, connMode);

        int port = GetPrivateProfileIntA("NETWORK", "lan_port", 27015, iniPath_);
        port_.store(static_cast<uint16_t>(std::clamp(port, 1024, 65535)), std::memory_order_relaxed);
        InterlockedExchange(&ds3scLanPort, port_.load());

        char pwdBuf[128] = {};
        GetPrivateProfileStringA("PASSWORD", "cooppassword", "", pwdBuf, sizeof(pwdBuf), iniPath_);
        sessionPassword_ = pwdBuf;
        passwordHash_.store(HashPassword(sessionPassword_), std::memory_order_relaxed);
    }
}

bool LanCoopExtension::InstallSteamHooks() noexcept {
    HMODULE hSteamClient = GetModuleHandleA("steamclient64.dll");
    if (!hSteamClient) {
        hSteamClient = LoadLibraryA("steamclient64.dll");
    }
    if (hSteamClient) {
        auto pCreateInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(hSteamClient, "CreateInterface"));
        if (pCreateInterface) {
            MH_CreateHook(reinterpret_cast<void*>(pCreateInterface),
                          reinterpret_cast<void*>(&DetourCreateInterface),
                          reinterpret_cast<void**>(&g_origCreateInterface));
            MH_EnableHook(reinterpret_cast<void*>(pCreateInterface));
            OutputDebugStringA("[ds3sc-lan-coop] Hooked steamclient64.dll CreateInterface\n");

            int err = 0;
            void* pClient = pCreateInterface("SteamClient017", &err);
            if (pClient) {
                void** vtable = *reinterpret_cast<void***>(pClient);
                if (vtable && vtable[12] != reinterpret_cast<void*>(&DetourGetISteamGenericInterface)) {
                    g_steamClientVTable = vtable;
                    DWORD oldProtect = 0;
                    if (VirtualProtect(&vtable[12], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                        g_origGetGenericInterface = reinterpret_cast<GetGenericInterfaceFn>(vtable[12]);
                        vtable[12] = reinterpret_cast<void*>(&DetourGetISteamGenericInterface);
                        VirtualProtect(&vtable[12], sizeof(void*), oldProtect, &oldProtect);
                        OutputDebugStringA("[ds3sc-lan-coop] Immediately hooked ISteamClient::GetISteamGenericInterface slot 12 on startup\n");
                    }
                }
            }
        }
    }

    HMODULE hSteamApi = GetModuleHandleA("steam_api64.dll");
    if (hSteamApi) {
        auto pReg = GetProcAddress(hSteamApi, "SteamAPI_RegisterCallback");
        if (pReg) {
            MH_CreateHook(reinterpret_cast<void*>(pReg),
                          reinterpret_cast<void*>(&DetourRegisterCallback),
                          reinterpret_cast<void**>(&g_origRegisterCallback));
            MH_EnableHook(reinterpret_cast<void*>(pReg));
        }

        auto pUnreg = GetProcAddress(hSteamApi, "SteamAPI_UnregisterCallback");
        if (pUnreg) {
            MH_CreateHook(reinterpret_cast<void*>(pUnreg),
                          reinterpret_cast<void*>(&DetourUnregisterCallback),
                          reinterpret_cast<void**>(&g_origUnregisterCallback));
            MH_EnableHook(reinterpret_cast<void*>(pUnreg));
        }

        auto pRun = GetProcAddress(hSteamApi, "SteamAPI_RunCallbacks");
        if (pRun) {
            MH_CreateHook(reinterpret_cast<void*>(pRun),
                          reinterpret_cast<void*>(&DetourRunCallbacks),
                          reinterpret_cast<void**>(&g_origRunCallbacks));
            MH_EnableHook(reinterpret_cast<void*>(pRun));
        }
    }
    return true;
}

void LanCoopExtension::RemoveSteamHooks() noexcept {
    if (g_steamClientVTable && g_origGetGenericInterface) {
        DWORD oldProtect = 0;
        if (VirtualProtect(&g_steamClientVTable[12], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            g_steamClientVTable[12] = reinterpret_cast<void*>(g_origGetGenericInterface);
            VirtualProtect(&g_steamClientVTable[12], sizeof(void*), oldProtect, &oldProtect);
        }
    }
}

bool LanCoopExtension::Initialize() noexcept {
    LoadSettings();

    // Generate/retrieve default SteamID for local player
    uint64_t localId = 0x0110000100000001ULL;
    char compName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(compName);
    if (GetComputerNameA(compName, &size) && size > 0) {
        uint32_t nameHash = HashPassword(compName);
        localId = 0x0110000100000000ULL | static_cast<uint64_t>(nameHash);
    }
    network::LanTransport::Instance().SetLocalSteamId(localId);

    // Socket is bound on-demand upon hosting or searching/joining,
    // and closed immediately upon dissolving the party.

    InstallSteamHooks();

    active_.store(ds3scConnectionMode != 0, std::memory_order_release);
    initialized_.store(true, std::memory_order_release);
    OutputDebugStringA("[ds3sc-lan-coop] LAN Co-op extension initialized successfully.\n");
    return true;
}

void LanCoopExtension::Shutdown() noexcept {
    RemoveSteamHooks();
    network::LanTransport::Instance().Shutdown();
    initialized_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
}

void LanCoopExtension::OnTick() noexcept {
    if (!initialized_.load(std::memory_order_relaxed)) return;

    active_.store(ds3scConnectionMode != 0, std::memory_order_relaxed);
    if (active_.load(std::memory_order_relaxed)) {
        network::LanTransport::Instance().PollIncomingPackets();
    }
}

bool LanCoopExtension::IsActive() const noexcept {
    return ds3scConnectionMode != 0;
}

bool LanCoopExtension::IsHost() const noexcept {
    return isHost_.load(std::memory_order_acquire);
}

uint16_t LanCoopExtension::GetPort() const noexcept {
    return port_.load(std::memory_order_relaxed);
}

uint32_t LanCoopExtension::GetPasswordHash() const noexcept {
    return passwordHash_.load(std::memory_order_relaxed);
}

uint64_t LanCoopExtension::GetVirtualLobbyId() const noexcept {
    return kLanLobbyId;
}

void LanCoopExtension::TriggerHostSession() noexcept {
    isHost_.store(true, std::memory_order_release);
    uint16_t port = port_.load(std::memory_order_relaxed);
    uint32_t hash = passwordHash_.load(std::memory_order_relaxed);
    if (!network::LanTransport::Instance().IsInitialized()) {
        network::LanTransport::Instance().Initialize(port, true);
    }
    network::LanTransport::Instance().StartBeaconBroadcaster("TheAshenLink_LAN", hash, port);
    OutputDebugStringA("[ds3sc-lan-coop] Host session active on LAN port.\n");
}

void LanCoopExtension::TriggerGuestSearch() noexcept {
    isHost_.store(false, std::memory_order_release);
    uint16_t port = port_.load(std::memory_order_relaxed);
    if (!network::LanTransport::Instance().IsInitialized()) {
        network::LanTransport::Instance().Initialize(port, false);
    }
}

void LanCoopExtension::DissolveSession() noexcept {
    isHost_.store(false, std::memory_order_release);
    network::LanTransport::Instance().CloseSession();
    OutputDebugStringA("[ds3sc-lan-coop] LAN session dissolved and UDP port closed.\n");
}

std::shared_ptr<IExtension> CreateLanCoopExtension() noexcept {
    return std::make_shared<LanCoopExtension>();
}

LanCoopExtension* GetLanCoopInstance() noexcept {
    return g_lanCoopInstance;
}

} // namespace ds3sc::extensions

extern "C" {

__declspec(dllexport) int ds3sc_is_lan_coop_active() {
    return (ds3sc::extensions::GetLanCoopInstance() &&
            ds3sc::extensions::GetLanCoopInstance()->IsActive()) ? 1 : 0;
}

__declspec(dllexport) int ds3sc_is_lan_host() {
    return (ds3sc::extensions::GetLanCoopInstance() &&
            ds3sc::extensions::GetLanCoopInstance()->IsHost()) ? 1 : 0;
}

__declspec(dllexport) uint16_t ds3sc_get_lan_port() {
    return ds3sc::extensions::GetLanCoopInstance() ?
           ds3sc::extensions::GetLanCoopInstance()->GetPort() : 27015;
}

__declspec(dllexport) void ds3sc_set_lan_mode(int mode) {
    InterlockedExchange(&ds3scConnectionMode, mode != 0 ? 1 : 0);
    if (mode == 0 && ds3sc::extensions::GetLanCoopInstance()) {
        ds3sc::extensions::GetLanCoopInstance()->DissolveSession();
    }
}

}
