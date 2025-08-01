#pragma once
#include <ydb/core/driver_lib/run/config.h>
#include <ydb/core/tablet/defs.h>
#include <ydb/core/viewer/json/json.h>
#include <ydb/core/viewer/protos/viewer.pb.h>
#include <ydb/core/sys_view/common/events.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/defs.h>
#include <ydb/library/actors/core/event.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>

namespace NKikimr::NViewer {

using TTabletId = ui64;

inline TActorId MakeViewerID(ui32 node) {
    char x[12] = {'v','i','e','w','e','r'};
    return TActorId(node, TStringBuf(x, 12));
}

struct TEvViewer {
    enum EEv {
        // requests
        EvViewerRequest = EventSpaceBegin(TKikimrEvents::ES_VIEWER),
        EvViewerResponse,
        EvUpdateSharedCacheTabletRequest,
        EvUpdateSharedCacheTabletResponse,
        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_VIEWER), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_VIEWER)");

    struct TEvViewerRequest : TEventPB<TEvViewerRequest, NKikimrViewer::TEvViewerRequest, EvViewerRequest> {
        TEvViewerRequest() = default;
    };

    struct TEvViewerResponse : TEventPB<TEvViewerResponse, NKikimrViewer::TEvViewerResponse, EvViewerResponse> {
        TEvViewerResponse() = default;
    };

    struct TEvUpdateSharedCacheTabletRequest : TEventLocal<TEvUpdateSharedCacheTabletRequest, EvUpdateSharedCacheTabletRequest> {
        TTabletId TabletId;
        std::unique_ptr<IEventBase> Request;

        TEvUpdateSharedCacheTabletRequest(TTabletId tabletId, std::unique_ptr<IEventBase> request)
            : TabletId(tabletId)
            , Request(std::move(request))
        {}
    };

    struct TEvUpdateSharedCacheTabletResponse : TEventLocal<TEvUpdateSharedCacheTabletResponse, EvUpdateSharedCacheTabletResponse> {
        std::variant<
            std::shared_ptr<NSysView::TEvSysView::TEvGetGroupsResponse>,
            std::shared_ptr<NSysView::TEvSysView::TEvGetStoragePoolsResponse>,
            std::shared_ptr<NSysView::TEvSysView::TEvGetVSlotsResponse>,
            std::shared_ptr<NSysView::TEvSysView::TEvGetPDisksResponse>,
            std::shared_ptr<NSysView::TEvSysView::TEvGetStorageStatsResponse>> Response;

        TEvUpdateSharedCacheTabletResponse(std::shared_ptr<NSysView::TEvSysView::TEvGetGroupsResponse> response)
            : Response(std::move(response))
        {}

        TEvUpdateSharedCacheTabletResponse(std::shared_ptr<NSysView::TEvSysView::TEvGetStoragePoolsResponse> response)
            : Response(std::move(response))
        {}

        TEvUpdateSharedCacheTabletResponse(std::shared_ptr<NSysView::TEvSysView::TEvGetVSlotsResponse> response)
            : Response(std::move(response))
        {}

        TEvUpdateSharedCacheTabletResponse(std::shared_ptr<NSysView::TEvSysView::TEvGetPDisksResponse> response)
            : Response(std::move(response))
        {}

        TEvUpdateSharedCacheTabletResponse(std::shared_ptr<NSysView::TEvSysView::TEvGetStorageStatsResponse> response)
            : Response(std::move(response))
        {}
    };
};

struct TRequestSettings {
    ui64 ChangedSince = 0;
    bool AliveOnly = false;
    std::vector<ui32> FilterNodeIds;
    TString GroupFields;
    bool AllEnums = false;
    TString FilterFields;
    TString MergeFields;
    ui32 Timeout = 0;
    ui32 Retries = 0;
    TDuration RetryPeriod = TDuration::MilliSeconds(500);
    TString Format;
    std::optional<bool> StaticNodesOnly;
    std::vector<i32> FieldsRequired;

    bool Followers = true; // hive tablet info
    bool Metrics = true; // hive tablet info
    bool WithRetry = true; // pipes
};

IActor* CreateViewer(const TKikimrRunConfig& kikimrRunConfig);

struct TRequestState {
    std::variant<std::monostate, const NMon::TEvHttpInfo*, const NHttp::TEvHttpProxy::TEvHttpIncomingRequest*> Request;
    NWilson::TTraceId TraceId;

    TRequestState() = default;

    TRequestState(const NMon::TEvHttpInfo* request)
        : Request(request)
    {}

    TRequestState(const NMon::TEvHttpInfo* request, NWilson::TTraceId traceId)
        : Request(request)
        , TraceId(traceId)
    {}

    TRequestState(const NHttp::TEvHttpProxy::TEvHttpIncomingRequest* request)
        : Request(request)
    {}

    TRequestState(const NHttp::TEvHttpProxy::TEvHttpIncomingRequest* request, NWilson::TTraceId traceId)
        : Request(request)
        , TraceId(traceId)
    {}

    bool HasHeader(TStringBuf name) const {
        if (auto* request = std::get_if<const NMon::TEvHttpInfo*>(&Request)) {
            return (*request)->Request.GetHeaders().HasHeader(name);
        }
        if (auto* request = std::get_if<const NHttp::TEvHttpProxy::TEvHttpIncomingRequest*>(&Request)) {
            return NHttp::THeaders((*request)->Request->Headers).Has(name);
        }
        return false;
    }

    TString GetHeader(TStringBuf name) const {
        if (auto* request = std::get_if<const NMon::TEvHttpInfo*>(&Request)) {
            auto header = (*request)->Request.GetHeaders().FindHeader(name);
            return header ? header->Value() : TString();
        }
        if (auto* request = std::get_if<const NHttp::TEvHttpProxy::TEvHttpIncomingRequest*>(&Request)) {
            return TString(NHttp::THeaders((*request)->Request->Headers).Get(name));
        }
        return {};
    }

    TString GetRemoteAddr() const {
        if (auto* request = std::get_if<const NMon::TEvHttpInfo*>(&Request)) {
            return (*request)->Request.GetRemoteAddr();
        }
        if (auto* request = std::get_if<const NHttp::TEvHttpProxy::TEvHttpIncomingRequest*>(&Request)) {
            return (*request)->Request->Address->ToString();
        }
        return {};
    }

    TString GetUri() const {
        if (auto* request = std::get_if<const NMon::TEvHttpInfo*>(&Request)) {
            return TString((*request)->Request.GetUri());
        }
        if (auto* request = std::get_if<const NHttp::TEvHttpProxy::TEvHttpIncomingRequest*>(&Request)) {
            return TString((*request)->Request->URL);
        }
        return {};
    }

    TString GetUserTokenObject() const {
        if (auto* request = std::get_if<const NMon::TEvHttpInfo*>(&Request)) {
            return (*request)->UserToken;
        }
        if (auto* request = std::get_if<const NHttp::TEvHttpProxy::TEvHttpIncomingRequest*>(&Request)) {
            return (*request)->UserToken;
        }
        return {};
    }

    explicit operator bool() const {
        return Request.index() != 0;
    }
};

struct TViewerSharedCacheState;

class IViewer {
public:
    struct TBrowseContext {
        struct TPathEntry {
            NKikimrViewer::EObjectType Type;
            TString Name;
        };

        TString Path;
        TVector<TPathEntry> Paths;
        TActorId Owner;
        TString UserToken;

        TBrowseContext(const TActorId owner, const TString& userToken)
            : Owner(owner)
            , UserToken(userToken)
        {}

        const TString& GetMyName() const {
            static TString emptyName;
            if (Paths.empty()) {
                return emptyName;
            }
            return Paths.back().Name;
        }

        NKikimrViewer::EObjectType GetMyType() const {
            if (Paths.empty()) {
                return NKikimrViewer::EObjectType::Unknown;
            }
            return Paths.back().Type;
        }

        const TString& GetParentName() const {
            static TString emptyName;
            if (Paths.size() < 2) {
                return emptyName;
            }
            return (Paths.rbegin() + 1)->Name;
        }

        NKikimrViewer::EObjectType GetParentType() const {
            if (Paths.size() < 2) {
                return NKikimrViewer::EObjectType::Unknown;
            }
            return (Paths.rbegin() + 1)->Type;
        }
    };

    using TVirtualHandlerType = std::function<IActor*(const TActorId& owner, const TBrowseContext& context)>;

    struct TVirtualHandler {
        IViewer::TVirtualHandlerType BrowseHandler = nullptr;

        TVirtualHandler(IViewer::TVirtualHandlerType browseHandler)
            : BrowseHandler(browseHandler)
        {}
    };

    struct TContentRequestContext {
        // request settings
        TJsonSettings JsonSettings;
        TDuration Timeout = TDuration::MilliSeconds(10000);
        TString UserToken;

        // filter
        ui32 Limit = 50;
        ui32 Offset = 0;
        TString Key;

        // path to object
        TString Path;

        // object type and location
        NKikimrViewer::EObjectType Type = NKikimrViewer::Unknown;
        TString ObjectName;

        TString Dump() const;
    };

    using TContentHandler = std::function<IActor*(const TActorId&, const TContentRequestContext&)>;

    virtual const TKikimrRunConfig& GetKikimrRunConfig() const = 0;
    virtual TVector<const TVirtualHandler*> GetVirtualHandlers(NKikimrViewer::EObjectType type, const TString& path) const = 0;
    virtual void RegisterVirtualHandler(NKikimrViewer::EObjectType parentObjectType, TVirtualHandlerType handler) = 0;

    // returns nullptr if no such handler
    virtual TContentHandler GetContentHandler(NKikimrViewer::EObjectType objectType) const = 0;

    virtual void RegisterContentHandler(
        NKikimrViewer::EObjectType objectType,
        const TContentHandler& handler) = 0;

    virtual TString GetHTTPOK(const TRequestState& request, TString contentType = {}, TString response = {}, TInstant lastModified = {}) = 0;
    virtual TString GetChunkedHTTPOK(const TRequestState& request, TString contentType = {}) = 0;

    TString GetHTTPOKJSON(const TRequestState& request, TString response = {}, TInstant lastModified = {}) {
        return GetHTTPOK(request, "application/json", response, lastModified);
    }

    TString GetHTTPOKYAML(const TRequestState& request, TString response = {}, TInstant lastModified = {}) {
        return GetHTTPOK(request, "application/yaml", response, lastModified);
    }

    TString GetHTTPOKTEXT(const TRequestState& request, TString response = {}, TInstant lastModified = {}) {
        return GetHTTPOK(request, "text/plain", response, lastModified);
    }

    virtual TString GetHTTPGATEWAYTIMEOUT(const TRequestState& request, TString contentType = {}, TString response = {}) = 0;
    virtual TString GetHTTPBADREQUEST(const TRequestState& request, TString contentType = {}, TString response = {}) = 0;
    virtual TString GetHTTPFORBIDDEN(const TRequestState& request, TString contentType = {}, TString response = {}) = 0;
    virtual TString GetHTTPNOTFOUND(const TRequestState& request) = 0;
    virtual TString GetHTTPINTERNALERROR(const TRequestState& request, TString contentType = {}, TString response = {}) = 0;
    virtual TString GetHTTPFORWARD(const TRequestState& request, const TString& location) = 0;
    virtual bool CheckAccessViewer(const TRequestState& request) = 0;
    virtual bool CheckAccessAdministration(const TRequestState& request) = 0;
    virtual void TranslateFromBSC2Human(const NKikimrBlobStorage::TConfigResponse& response, TString& bscError, bool& forceRetryPossible) = 0;
    virtual TString MakeForward(const TRequestState& request, const std::vector<ui32>& nodes) = 0;

    virtual void AddRunningQuery(const TString& queryId, const TActorId& actorId) = 0;
    virtual void EndRunningQuery(const TString& queryId, const TActorId& actorId) = 0;
    virtual TActorId FindRunningQuery(const TString& queryId) = 0;

    virtual NJson::TJsonValue GetCapabilities() = 0;
    virtual int GetCapabilityVersion(const TString& name) = 0;

    void UpdateSharedCacheData(std::unique_ptr<TEvViewer::TEvUpdateSharedCacheTabletResponse> ev);
    void DeleteOldSharedCacheData();
    std::shared_ptr<TViewerSharedCacheState> CreateSharedCacheState();
    std::shared_ptr<TViewerSharedCacheState> SharedCacheState = CreateSharedCacheState();
};

void SetupPQVirtualHandlers(IViewer* viewer);
void SetupDBVirtualHandlers(IViewer* viewer);
void SetupKqpContentHandler(IViewer* viewer);

template <typename ValueType, typename OutputIteratorType>
void GenericSplitIds(TStringBuf source, char delim, OutputIteratorType it) {
    for (TStringBuf value = source.NextTok(delim); !value.empty(); value = source.NextTok(delim)) {
        const auto newValue = (value == ".") ? ValueType() : FromStringWithDefault<ValueType>(value, ValueType());
        *(it++) = newValue;
    }
}

template <typename ValueType>
void SplitIds(TStringBuf source, char delim, TVector<ValueType>& values) {
    GenericSplitIds<ValueType>(source, delim, std::back_inserter(values));
}

template <typename ValueType>
void SplitIds(TStringBuf source, char delim, std::vector<ValueType>& values) {
    GenericSplitIds<ValueType>(source, delim, std::back_inserter(values));
}

template <typename ValueType>
void SplitIds(TStringBuf source, char delim, std::unordered_set<ValueType>& values) {
    GenericSplitIds<ValueType>(source, delim, std::inserter(values, values.end()));
}

TString GetHTTPOKJSON();
TString GetHTTPGATEWAYTIMEOUT();
NKikimrViewer::EFlag GetFlagFromTabletState(NKikimrWhiteboard::TTabletStateInfo::ETabletState state);
NKikimrViewer::EFlag GetFlagFromTabletState(NKikimrHive::ETabletVolatileState state);
NKikimrViewer::EFlag GetPDiskStateFlag(const NKikimrWhiteboard::TPDiskStateInfo& info);
NKikimrViewer::EFlag GetPDiskOverallFlag(const NKikimrWhiteboard::TPDiskStateInfo& info);
NKikimrViewer::EFlag GetVDiskOverallFlag(const NKikimrWhiteboard::TVDiskStateInfo& info);

struct TBSGroupState {
    NKikimrViewer::EFlag Overall;
    ui32 MissingDisks = 0;
    ui32 SpaceProblems = 0;
};

NKikimrViewer::EFlag GetBSGroupOverallFlagWithoutLatency(
        const NKikimrWhiteboard::TBSGroupStateInfo& info,
        const TMap<NKikimrBlobStorage::TVDiskID, const NKikimrWhiteboard::TVDiskStateInfo&>& vDisksIndex,
        const TMap<std::pair<ui32, ui32>, const NKikimrWhiteboard::TPDiskStateInfo&>& pDisksIndex);
TBSGroupState GetBSGroupOverallStateWithoutLatency(
        const NKikimrWhiteboard::TBSGroupStateInfo& info,
        const TMap<NKikimrBlobStorage::TVDiskID, const NKikimrWhiteboard::TVDiskStateInfo&>& vDisksIndex,
        const TMap<std::pair<ui32, ui32>, const NKikimrWhiteboard::TPDiskStateInfo&>& pDisksIndex);
NKikimrViewer::EFlag GetBSGroupOverallFlag(
        const NKikimrWhiteboard::TBSGroupStateInfo& info,
        const TMap<NKikimrBlobStorage::TVDiskID, const NKikimrWhiteboard::TVDiskStateInfo&>& vDisksIndex,
        const TMap<std::pair<ui32, ui32>, const NKikimrWhiteboard::TPDiskStateInfo&>& pDisksIndex);
TBSGroupState GetBSGroupOverallState(
        const NKikimrWhiteboard::TBSGroupStateInfo& info,
        const TMap<NKikimrBlobStorage::TVDiskID, const NKikimrWhiteboard::TVDiskStateInfo&>& vDisksIndex,
        const TMap<std::pair<ui32, ui32>, const NKikimrWhiteboard::TPDiskStateInfo&>& pDisksIndex);

NKikimrViewer::EFlag GetFlagFromUsage(double usage);

NKikimrWhiteboard::EFlag GetWhiteboardFlag(NKikimrViewer::EFlag flag);
NKikimrViewer::EFlag GetViewerFlag(NKikimrWhiteboard::EFlag flag);

}
