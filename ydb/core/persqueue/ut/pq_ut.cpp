#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/keyvalue/keyvalue_events.h>
#include <ydb/core/persqueue/events/global.h>
#include <ydb/core/persqueue/partition.h>
#include <ydb/core/persqueue/ut/common/pq_ut_common.h>
#include <ydb/core/security/ticket_parser.h>

#include <ydb/core/testlib/fake_scheme_shard.h>
#include <ydb/core/testlib/tablet_helpers.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/system/sanitizers.h>
#include <util/system/valgrind.h>

namespace NKikimr::NPQ {

const static TString TOPIC_NAME = "rt3.dc1--topic";

Y_UNIT_TEST_SUITE(TPQTest) {

TMaybe<ui64> PQGetStartOffset(TTestContext& tc)
{
    TAutoPtr<IEventHandle> handle;
    TEvPersQueue::TEvOffsetsResponse *result;
    THolder<TEvPersQueue::TEvOffsets> request;

    for (i32 retriesLeft = 3; retriesLeft > 0; --retriesLeft) {
        try {
            tc.Runtime->ResetScheduledCount();
            request.Reset(new TEvPersQueue::TEvOffsets);

            tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries());
            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvOffsetsResponse>(handle);
            UNIT_ASSERT(result);

            if (result->Record.PartResultSize() == 0 ||
                result->Record.GetPartResult(0).GetErrorCode() == NPersQueue::NErrorCode::INITIALIZING) {
                tc.Runtime->DispatchEvents();   // Dispatch events so that initialization can make progress
                retriesLeft = 3;
                continue;
            }

            UNIT_ASSERT(result->Record.PartResultSize());

            return result->Record.GetPartResult(0).GetStartOffset();
        } catch (const NActors::TSchedulingLimitReachedException&) {
            UNIT_ASSERT(retriesLeft > 0);
        }
    }

    return Nothing();
}

Y_UNIT_TEST(TestCmdReadWithLastOffset) {
    TTestContext tc;
    tc.EnableDetailedPQLog = true;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        activeZone = false;
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(1000);
        tc.Runtime->RegisterService(MakePQDReadCacheServiceActorId(), tc.Runtime->Register(
                CreatePQDReadCacheService(new NMonitoring::TDynamicCounters()))
        );

        PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"user1", true}}, tc);
        TVector<std::pair<ui64, TString>> data;
        i64 messageCount = 100;
        for (i64 i = 1; i <= messageCount; ++i) {
            data.push_back({i, TString(100_KB, 'a')});
        }
        CmdWrite(0, "sourceid0", data, tc, false, {}, false, "", -1, 0, false, false, true);
        TString sessionId = "session1";
        TString user = "user1";
        TPQCmdSettings sessionSettings{0, user, sessionId};
        sessionSettings.PartitionSessionId = 1;
        sessionSettings.KeepPipe = true;
        TPQCmdReadSettings readSettings{
            /*session=*/ sessionId,
            /*partition=*/ 0,
            /*offset=*/ 0,
            /*count=*/ static_cast<ui32>(messageCount),
            /*size=*/ 16_MB,
            /*resCount=*/ 0,
        };
        readSettings.PartitionSessionId = 1;
        readSettings.User = user;

        activeZone = false;
        Cerr << "Create session\n";
        auto pipe = CmdCreateSession(sessionSettings, tc);
        readSettings.Pipe = pipe;

        for (i64 offset = 0; offset < messageCount; offset += 10) {
            for (i64 lastOffset = 0; lastOffset <= messageCount; lastOffset += 10) {
                readSettings.Offset = offset;
                readSettings.LastOffset = lastOffset;
                readSettings.ResCount = lastOffset < offset ? 0 : static_cast<ui32>(lastOffset - offset);
                BeginCmdRead(readSettings, tc);

                TAutoPtr<IEventHandle> handle;
                auto* result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

                UNIT_ASSERT_C(result->Record.GetPartitionResponse().HasCmdReadResult(), result->Record.GetPartitionResponse().DebugString());
                auto res = result->Record.GetPartitionResponse().GetCmdReadResult();

                if (lastOffset) {
                    UNIT_ASSERT_C(readSettings.ResCount <= res.ResultSize(),
                                  "readSettings.ResCount=" << readSettings.ResCount << ", res.ResultSize()=" << res.ResultSize());
                }

                for (size_t i = 0; i < res.ResultSize(); ++i) {
                    UNIT_ASSERT_EQUAL(res.GetResult(i).GetOffset(), offset + i);
                    UNIT_ASSERT_EQUAL(res.GetResult(i).GetData(), data[offset + i].second);
                }
            }
        }
    });
}

Y_UNIT_TEST(TestDirectReadHappyWay) {
    TTestContext tc;
    tc.EnableDetailedPQLog = true;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        activeZone = false;
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(1000);
        tc.Runtime->RegisterService(MakePQDReadCacheServiceActorId(), tc.Runtime->Register(
                CreatePQDReadCacheService(new NMonitoring::TDynamicCounters()))
        );

        PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"user1", true}}, tc);
        TVector<std::pair<ui64, TString>> data;
        TString s{2_MB, 'c'};
        data.push_back({1, s});
        CmdWrite(0, "sourceid0", data, tc, false, {}, false, "", -1, 0, false, false, true);
        TString sessionId = "session1";
        TString user = "user1";
        TPQCmdSettings sessionSettings{0, user, sessionId};
        sessionSettings.PartitionSessionId = 1;
        sessionSettings.KeepPipe = true;

        TPQCmdReadSettings readSettings{sessionId, 0, 0, 1, 99999, 1};
        readSettings.PartitionSessionId = 1;
        readSettings.DirectReadId = 1;
        readSettings.User = user;

        activeZone = false;
        Cerr << "Create session\n";
        auto pipe = CmdCreateSession(sessionSettings, tc);
        TCmdDirectReadSettings publishSettings{0, sessionId, 1, 1, pipe, false};
        readSettings.Pipe = pipe;
        CmdRead(readSettings, tc);
        Cerr << "Run cmd publish\n";
        CmdPublishRead(publishSettings, tc);
        Cerr << "Run cmd forget\n";
        CmdForgetRead(publishSettings, tc);
    });
}

Y_UNIT_TEST(DirectReadBadSessionOrPipe) {
    TTestContext tc;
    tc.EnableDetailedPQLog = true;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(1000);

        PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"user1", true}}, tc);
        TVector<std::pair<ui64, TString>> data;
        TString s{2_MB, 'c'};
        data.push_back({1, s});
        CmdWrite(0, "sourceid2", data, tc, false, {}, false, "", -1, 0, false, false, true);
        TString sessionId = "session2";
        TString user = "user2";
        TPQCmdSettings sessionSettings{0, user, sessionId};
        sessionSettings.PartitionSessionId = 1;
        sessionSettings.KeepPipe = true;

        TPQCmdReadSettings readSettings(sessionId, 0, 0, 1, 99999, 1);
        readSettings.PartitionSessionId = 1;
        readSettings.DirectReadId = 1;
        readSettings.User = user;
        activeZone = false;

        readSettings.ToFail = true;
        //No pipe
        CmdRead(readSettings, tc);
        auto pipe = CmdCreateSession(sessionSettings, tc);
        readSettings.Pipe = pipe;
        readSettings.Session = "";
        // No session
        CmdRead(readSettings, tc);
        readSettings.Session = "bad-session";
        // Bad session
        CmdRead(readSettings, tc);
        activeZone = false;
        readSettings.Session = sessionId;
        CmdKillSession(0, user, sessionId,tc, pipe);
        activeZone = false;
        // Dead session
        CmdRead(readSettings, tc);

        activeZone = false;
        TCmdDirectReadSettings publishSettings{0, sessionId, 1, 1, pipe, true};
        readSettings.Pipe = pipe;
        activeZone = false;
        // Dead session
        Cerr << "Publish read\n";
        CmdPublishRead(publishSettings, tc);
        Cerr << "Forget read\n";
        CmdForgetRead(publishSettings, tc);
    });
}
Y_UNIT_TEST(DirectReadOldPipe) {
    TTestContext tc;
    tc.EnableDetailedPQLog = true;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(1000);

        PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"user1", true}}, tc);
        TString sessionId = "session2";
        TString user = "user2";
        TPQCmdSettings sessionSettings{0, user, sessionId};
        sessionSettings.PartitionSessionId = 1;
        sessionSettings.KeepPipe = true;

        TPQCmdReadSettings readSettings(sessionId, 0, 0, 1, 99999, 1);
        readSettings.PartitionSessionId = 1;
        readSettings.DirectReadId = 1;
        readSettings.ToFail = true;
        activeZone = false;

        auto pipe = CmdCreateSession(sessionSettings, tc);

        auto event = MakeHolder<TEvTabletPipe::TEvServerDisconnected>(0, pipe, TActorId{});
        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, event.Release(), 0, GetPipeConfigWithRetries());
        readSettings.Pipe = pipe;

        CmdRead(readSettings, tc);
    });
}



Y_UNIT_TEST(TestPartitionTotalQuota) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(1000);

        tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetPartitionReadQuotaIsTwiceWriteQuota(true);
        tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetMaxParallelConsumersPerPartition(1); //total partition quota is equal to quota per consumer. Very low.

        PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"important_user", true}}, tc);
        TVector<std::pair<ui64, TString>> data;
        TString s{2_MB, 'c'};
        data.push_back({1, s});
        CmdWrite(0, "sourceid0", data, tc, false, {}, false, "", -1, 0, false, false, true);

        //check throttling on total partition quota
        auto startTime = tc.Runtime->GetTimeProvider()->Now();
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc, {0}, 0, 0, "user1");
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc, {0}, 0, 0, "user2");
        auto diff = (tc.Runtime->GetTimeProvider()->Now() - startTime).Seconds();
        UNIT_ASSERT_C(diff >= 9, TStringBuilder() << "Expected >= 9, actual: " << diff); //read quota is twice write quota. So, it's 200kb per seconds and 200kb burst. (2mb - 200kb) / 200kb = 9 seconds needed to get quota
    });
}

Y_UNIT_TEST(TestAccountReadQuota) {
    TTestContext tc;
    TAtomic stop = 0;
    TAtomicCounter quoterRequests = 0;
    i64 prevQuoterReqCount = 0;
    Y_UNUSED(prevQuoterReqCount);
    TFinalizer finalizer(tc);
    tc.Prepare();
    tc.Runtime->SetObserverFunc(
        [&](TAutoPtr<IEventHandle>& ev) {
            if (auto* msg = ev->CastAsLocal<TEvQuota::TEvRequest>()) {
                Cerr << "Captured kesus quota request event from " << ev->Sender.ToString() << Endl;
                if (!AtomicGet(stop)) {
                    quoterRequests.Inc();
                    tc.Runtime->Send(new IEventHandle(
                        ev->Sender, TActorId{},
                        new TEvQuota::TEvClearance(TEvQuota::TEvClearance::EResult::Success), 0, ev->Cookie)
                    );
                }
                return TTestActorRuntimeBase::EEventAction::DROP;
            }
            return TTestActorRuntimeBase::EEventAction::PROCESS;
        }
    );
    tc.Runtime->SetScheduledLimit(1000);

    tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetPartitionReadQuotaIsTwiceWriteQuota(true);
    tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetEnableQuoting(true);
    tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetEnableReadQuoting(true);

    PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"important_user", true}}, tc);
    TVector<std::pair<ui64, TString>> data;
    TString s{100_KB, 'c'};
    data.push_back({1, s});

    auto runTest = [&]() {
        Cerr << "CmdWrite\n";
        CmdWrite(0, "sourceid0", data, tc, false, {}, false, "", -1, 0, false, false, true);
        data[0].first++;
        Cerr << "CmdRead\n";
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc, {0}, 0, 0, "user");
    };
    Cerr << "Run 1\n";
    runTest();
    Sleep(TDuration::Seconds(1));
    Cerr << "Currently have " << quoterRequests.Val() << " quoter requests\n";
    Cerr << "Run 2\n";
    runTest();
    Sleep(TDuration::Seconds(1));
    Cerr << "Currently have " << quoterRequests.Val() << " quoter requests\n";
    AtomicSet(stop, 1);
    Sleep(TDuration::Seconds(1));
}

Y_UNIT_TEST(TestPartitionPerConsumerQuota) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(1000);

        tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetPartitionReadQuotaIsTwiceWriteQuota(true);
        tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetMaxParallelConsumersPerPartition(1000); //total partition quota is 1 consumer quota * 1000. Very high.


        PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"important_user", true}}, tc);
        TVector<std::pair<ui64, TString>> data;
        TString s{2_MB, 'c'};
        data.push_back({1, s});
        CmdWrite(0, "sourceid0", data, tc, false, {}, false, "", -1, 0, false, false, true);

        //check throttling on per consumer quota
        auto startTimeReadWithSameConsumer = tc.Runtime->GetTimeProvider()->Now();
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc, {0}, 0, 0, "user1");
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc, {0}, 0, 0, "user1");
        auto diffReadWithSameConsumers = (tc.Runtime->GetTimeProvider()->Now() - startTimeReadWithSameConsumer).Seconds();
        UNIT_ASSERT(diffReadWithSameConsumers >= 9); //read quota is twice write quota. So, it's 200kb per seconds and 200kb burst. (2mb - 200kb) / 200kb = 9 seconds needed to get quota

        //check not throttling on total partition quota
        auto startTimeReadWithDifferentConsumers = tc.Runtime->GetTimeProvider()->Now();
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc, {0}, 0, 0, "user2");
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc, {0}, 0, 0, "user3");

        auto diffReadWithDifferentConsumers = (tc.Runtime->GetTimeProvider()->Now() - startTimeReadWithDifferentConsumers).Seconds();
        UNIT_ASSERT(diffReadWithDifferentConsumers <= 1); //different consumers. No throttling
    });
}

Y_UNIT_TEST(TestPartitionWriteQuota) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;

        tc.Runtime->SetScheduledLimit(1000);
        tc.Runtime->GetAppData(0).PQConfig.MutableQuotingConfig()->SetEnableQuoting(true);
        PQTabletPrepare({.partitions = 1, .writeSpeed = 100_KB}, {{"important_user", true}}, tc);

        tc.Runtime->SetObserverFunc(
            [&](TAutoPtr<IEventHandle>& ev) {
                if (auto* msg = ev->CastAsLocal<TEvQuota::TEvRequest>()) {
                    Cerr << "Captured kesus quota request event from " << ev->Sender.ToString() << Endl;
                    tc.Runtime->Send(new IEventHandle(
                        ev->Sender, TActorId{},
                        new TEvQuota::TEvClearance(TEvQuota::TEvClearance::EResult::Success), 0, ev->Cookie)
                    );
                    return TTestActorRuntimeBase::EEventAction::DROP;
                }
                return TTestActorRuntimeBase::EEventAction::PROCESS;
        });

        TVector<std::pair<ui64, TString>> data;
        TString s{2_MB, 'c'};
        data.push_back({1, s});
        auto startTime = tc.Runtime->GetTimeProvider()->Now();
        CmdWrite(0, "sourceid0", data, tc);
        data[0].first++;
        CmdWrite(0, "sourceid1", data, tc);
        data[0].first++;
        CmdWrite(0, "sourceid2", data, tc);
        //check throttling on total partition quota
        auto diff = (tc.Runtime->GetTimeProvider()->Now() - startTime).Seconds();
        UNIT_ASSERT_C(diff >= 3, TStringBuilder() << "Actual: " << diff);
    });
}

Y_UNIT_TEST(TestUserInfoCompatibility) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        TString client = "test";
        tc.Runtime->SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);

        PQTabletPrepare({.partitions=4, .specVersion=1,}, {{client, false}}, tc);

        TVector<std::pair<ui64, TString>> data;
        data.push_back({1, "s"});
        data.push_back({2, "q"});
        CmdWrite(0, "sourceid", data, tc);
        CmdWrite(1, "sourceid", data, tc);
        CmdWrite(2, "sourceid", data, tc);
        CmdWrite(3, "sourceid", data, tc);


        THolder<TEvKeyValue::TEvRequest> request(new TEvKeyValue::TEvRequest);
        FillUserInfo(request->Record.AddCmdWrite(), client, 0, 0);
        FillDeprecatedUserInfo(request->Record.AddCmdWrite(), client, 0, 0);
        FillUserInfo(request->Record.AddCmdWrite(), client, 1, 1);
        FillDeprecatedUserInfo(request->Record.AddCmdWrite(), client, 2, 1);
        FillUserInfo(request->Record.AddCmdWrite(), client, 2, 1);
        FillDeprecatedUserInfo(request->Record.AddCmdWrite(), client, 3, 0);
        FillUserInfo(request->Record.AddCmdWrite(), client, 3, 1);

        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries());
        TAutoPtr<IEventHandle> handle;
        TEvKeyValue::TEvResponse* result = tc.Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>(handle);
        Y_UNUSED(result);

        PQTabletRestart(tc);
        Cerr  << "AFTER RESTART\n";

        CmdGetOffset(0, client, 0, tc);
        CmdGetOffset(1, client, 1, tc);
        CmdGetOffset(2, client, 1, tc);
        CmdGetOffset(3, client, 1, tc);
    });
}

Y_UNIT_TEST(TestReadRuleVersions) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        TString client = "test";

        PQTabletPrepare({.partitions=3}, {{client, false}, {"another-user", false}}, tc);

        tc.Runtime->SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);

        TVector<std::pair<ui64, TString>> data;
        data.push_back({1, "s"});
        data.push_back({2, "q"});
        CmdWrite(0, "sourceid", data, tc);
        CmdWrite(1, "sourceid", data, tc);
        CmdWrite(2, "sourceid", data, tc);

        CmdSetOffset(0, client, 1, false, tc);
        CmdSetOffset(1, client, 2, false, tc);

        {
            THolder<TEvKeyValue::TEvRequest> request(new TEvKeyValue::TEvRequest);

            FillUserInfo(request->Record.AddCmdWrite(), "old_consumer", 0, 0);
            FillDeprecatedUserInfo(request->Record.AddCmdWrite(), "old_consumer", 0, 0);

            tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries());
            TAutoPtr<IEventHandle> handle;
            TEvKeyValue::TEvResponse* result = tc.Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>(handle);
            Y_UNUSED(result);

        }

        PQTabletRestart(tc);

        CmdGetOffset(0, client, 1, tc);
        CmdGetOffset(1, client, 2, tc);
        CmdGetOffset(0, "user", 0, tc);

        {
            THolder<TEvKeyValue::TEvRequest> request(new TEvKeyValue::TEvRequest);
            auto read = request->Record.AddCmdReadRange();
            auto range = read->MutableRange();
            NPQ::TKeyPrefix ikeyFrom(NPQ::TKeyPrefix::TypeInfo, TPartitionId(0), NPQ::TKeyPrefix::MarkUser);
            range->SetFrom(ikeyFrom.Data(), ikeyFrom.Size());
            range->SetIncludeFrom(true);
            NPQ::TKeyPrefix ikeyTo(NPQ::TKeyPrefix::TypeInfo, TPartitionId(1), NPQ::TKeyPrefix::MarkUser);
            range->SetTo(ikeyTo.Data(), ikeyTo.Size());
            range->SetIncludeTo(true);

            tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries());
            TAutoPtr<IEventHandle> handle;
            TEvKeyValue::TEvResponse* result = tc.Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>(handle);

            Cerr << result->Record << "\n";

            UNIT_ASSERT(result->Record.GetReadRangeResult(0).GetPair().size() == 7);
        }

        PQTabletPrepare({.partitions=3}, {}, tc);

        CmdGetOffset(0, client, 0, tc);
        CmdGetOffset(1, client, 0, tc);

        {
            THolder<TEvKeyValue::TEvRequest> request(new TEvKeyValue::TEvRequest);
            auto read = request->Record.AddCmdReadRange();
            auto range = read->MutableRange();
            NPQ::TKeyPrefix ikeyFrom(NPQ::TKeyPrefix::TypeInfo, TPartitionId(0), NPQ::TKeyPrefix::MarkUser);
            range->SetFrom(ikeyFrom.Data(), ikeyFrom.Size());
            range->SetIncludeFrom(true);
            NPQ::TKeyPrefix ikeyTo(NPQ::TKeyPrefix::TypeInfo, TPartitionId(1), NPQ::TKeyPrefix::MarkUser);
            range->SetTo(ikeyTo.Data(), ikeyTo.Size());
            range->SetIncludeTo(true);

            tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries());
            TAutoPtr<IEventHandle> handle;
            TEvKeyValue::TEvResponse* result = tc.Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>(handle);

            Cerr << result->Record << "\n";

            UNIT_ASSERT(result->Record.GetReadRangeResult(0).GetPair().size() == 3);
        }

        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, new NActors::NMon::TEvRemoteHttpInfo(TStringBuilder() << "localhost:8765/tablets/app?TabletID=" << tc.TabletId), 0, GetPipeConfigWithRetries());
        TAutoPtr<IEventHandle> handle;

        tc.Runtime->GrabEdgeEvent<NMon::TEvRemoteHttpInfoRes>(handle);
        TString rs = handle->Get<NMon::TEvRemoteHttpInfoRes>()->Html;
        Cerr << rs << "\n";
    });
}

Y_UNIT_TEST(TestSeveralOwners) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        // No important clients, lifetimeseconds=0 - delete all right now, except last datablob
        PQTabletPrepare({}, {}, tc);

        TVector<std::pair<ui64, TString>> data;

        TString s{32, 'c'};
        ui32 pp = 4 + 8 + 1 + 9;
        data.push_back({1, s.substr(pp)});
        data.push_back({2, s.substr(pp)});
        TString cookie1 = CmdSetOwner(0, tc, "owner1").first;
        TString cookie2 = CmdSetOwner(0, tc, "owner2").first;
        CmdWrite(0, "sourceid0", data, tc, false, {}, true, cookie1, 0, -1, true);

        CmdWrite(0, "sourceid1", data, tc, false, {}, false, cookie2, 0, -1, true);
        CmdWrite(0, "sourceid2", data, tc, false, {}, false, cookie1, 1, -1, true);

        TString cookie3 = CmdSetOwner(0, tc, "owner1").first;

        CmdWrite(0, "sourceid3", data, tc , true, {}, false, cookie1,  2, -1, true);
    });
}


Y_UNIT_TEST(TestWaitInOwners) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        // No important clients, lifetimeseconds=0 - delete all right now, except last datablob
        PQTabletPrepare({}, {}, tc);

        TVector<std::pair<ui64, TString>> data;

        TString s{32, 'c'};
        ui32 pp = 4 + 8 + 1 + 9;
        data.push_back({1, s.substr(pp)});
        data.push_back({2, s.substr(pp)});

        CmdSetOwner(0, tc, "owner", false);
        CmdSetOwner(0, tc, "owner", true); //will break last owner

        TActorId newPipe = SetOwner(0, tc, "owner", false); //this owner will wait

        auto p = CmdSetOwner(0, tc, "owner", true); //will break last owner

        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvResponse *result;
        try {
            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);
        } catch (NActors::TSchedulingLimitReachedException) {
            result = nullptr;
        }

        Y_ABORT_UNLESS(!result); //no answer yet

        CmdSetOwner(0, tc);
        CmdSetOwner(0, tc, "owner2"); //just to be dropped by next command

        WritePartData(0, "sourceid", 12, 1, 1, 5, 20, "value", tc, p.first, 0);

        result = tc.Runtime->GrabEdgeEventIf<TEvPersQueue::TEvResponse>(handle, [](const TEvPersQueue::TEvResponse& ev){
                if (ev.Record.HasPartitionResponse() && ev.Record.GetPartitionResponse().CmdWriteResultSize() > 0 || ev.Record.GetErrorCode() != NPersQueue::NErrorCode::OK)
                    return true;
                return false;
            }); //there could be outgoing reads in TestReadSubscription test

        UNIT_ASSERT(result);
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::BAD_REQUEST);

        try {
            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);
        } catch (NActors::TSchedulingLimitReachedException) {
            result = nullptr;
        }

        UNIT_ASSERT(result); //ok for newPipe because old owner is dead now
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);
        UNIT_ASSERT(result->Record.HasPartitionResponse());
        UNIT_ASSERT(result->Record.GetPartitionResponse().HasCmdGetOwnershipResult());

        SetOwner(0, tc, "owner", false); //will wait

        try {
            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);
        } catch (NActors::TSchedulingLimitReachedException) {
            result = nullptr;
        }

        Y_ABORT_UNLESS(!result); //no answer yet, waiting of dying of old ownership session

        tc.Runtime->Send(new IEventHandle(newPipe, tc.Edge, new TEvents::TEvPoisonPill()), 0, true); //will cause dying of pipe and old session

        TDispatchOptions options;
        options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTabletPipe::EvServerDisconnected));
        tc.Runtime->DispatchEvents(options);

        try {
            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);
        } catch (NActors::TSchedulingLimitReachedException) {
            result = nullptr;
        }

        UNIT_ASSERT(result); //now ok
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);
        UNIT_ASSERT(result->Record.HasPartitionResponse());
        UNIT_ASSERT(result->Record.GetPartitionResponse().HasCmdGetOwnershipResult());
    });
}




Y_UNIT_TEST(TestReserveBytes) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({}, {}, tc); //no important clients, lifetimeseconds=0 - delete all right now, except last datablob

        TVector<std::pair<ui64, TString>> data;

        TString s{32, 'c'};
        ui32 pp = 4 + 8 + 1 + 9;
        data.push_back({1, s.substr(pp)});
        data.push_back({2, s.substr(pp)});
        auto p = CmdSetOwner(0, tc);

        CmdReserveBytes(0, tc, p.first, 0, 20'000'000, p.second);
        CmdReserveBytes(0, tc, p.first, 1, 20'000'000, p.second, false, true);

        CmdReserveBytes(0, tc, p.first, 2, 40'000'000, p.second);

        CmdReserveBytes(0, tc, p.first, 3, 80'000'000, p.second, true);

        TString cookie = p.first;

        CmdWrite(0, "sourceid0", data, tc, false, {}, true, cookie, 4);

        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvResponse *result;
        try {
            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);
        } catch (NActors::TSchedulingLimitReachedException) {
            result = nullptr;
        }

        UNIT_ASSERT(!result);//no answer yet  40 + 80 > 90

        CmdWrite(0, "sourceid2", data, tc, false, {}, false, cookie, 5);

        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle); //now no inflight - 80 may fit

        UNIT_ASSERT(result);
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);

        CmdWrite(0, "sourceid3", data, tc, false, {}, false, cookie, 6);

        CmdReserveBytes(0, tc, p.first, 7, 80'000'000, p.second);
        p = CmdSetOwner(0, tc);
        CmdReserveBytes(0, tc, p.first, 0, 80'000'000, p.second);

    });
}




Y_UNIT_TEST(TestMessageNo) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({}, {}, tc); //no important clients, lifetimeseconds=0 - delete all right now, except last datablob

        TVector<std::pair<ui64, TString>> data;

        TString s{32, 'c'};
        ui32 pp = 4 + 8 + 1 + 9;
        data.push_back({1, s.substr(pp)});
        data.push_back({2, s.substr(pp)});
        TString cookie = CmdSetOwner(0, tc).first;
        CmdWrite(0, "sourceid0", data, tc, false, {}, true, cookie, 0);

        CmdWrite(0, "sourceid2", data, tc, false, {}, false, cookie, 1);

        WriteData(0, "sourceid1", data, tc, cookie, 2, -1);

        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvResponse *result;
        result = tc.Runtime->GrabEdgeEventIf<TEvPersQueue::TEvResponse>(handle, [](const TEvPersQueue::TEvResponse& ev){
            if (!ev.Record.HasPartitionResponse() || !ev.Record.GetPartitionResponse().HasCmdReadResult())
                return true;
            return false;
        }); //there could be outgoing reads in TestReadSubscription test

        UNIT_ASSERT(result);
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);

        UNIT_ASSERT(result->Record.GetPartitionResponse().CmdWriteResultSize() == data.size());
        for (ui32 i = 0; i < data.size(); ++i) {
            UNIT_ASSERT(result->Record.GetPartitionResponse().GetCmdWriteResult(i).HasAlreadyWritten());
            UNIT_ASSERT(result->Record.GetPartitionResponse().GetCmdWriteResult(i).HasOffset());
        }
        for (ui32 i = 0; i < data.size(); ++i) {
            auto res = result->Record.GetPartitionResponse().GetCmdWriteResult(i);
            UNIT_ASSERT(!result->Record.GetPartitionResponse().GetCmdWriteResult(i).GetAlreadyWritten());
        }

        CmdWrite(0, "sourceid3", data, tc , true, {}, false, cookie,  0);
    });
}


Y_UNIT_TEST(TestPartitionedBlobFails) {
    // TODO(abcdef): temporarily deleted
    return;

    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        // One important client, never delete
        PQTabletPrepare({.maxSizeInPartition=200_MB}, {{"user1", true}}, tc);

        TString ss{50_MB, '_'};
        char k = 0;
        TString s = "";
        s += k;
        s += ss;
        s += char((1) % 256);
        ++k;

        TVector<std::pair<ui64, TString>> data;
        data.push_back({1, s});

        TVector<TString> parts;
        ui32 size = 400_KB;
        ui32 diff = 50;
        for (ui32 pos = 0; pos < s.size();) {
            parts.push_back(s.substr(pos, size - diff));
            pos += size - diff;
        }
        Y_ABORT_UNLESS(parts.size() > 5);

        CmdWrite(0, "sourceid4", data, tc);
        {
            TString cookie = CmdSetOwner(0, tc).first;

            WritePartDataWithBigMsg(0, "sourceid0", 1, 1, 5, s.size(), parts[1], tc, cookie, 0, 12_MB);
            TAutoPtr<IEventHandle> handle;
            TEvPersQueue::TEvResponse *result;

            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

            UNIT_ASSERT(result);

            UNIT_ASSERT(result->Record.HasStatus());
            UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::INITIALIZING);
        }

        PQGetPartInfo(0, 1, tc);
        CmdWrite(0, "sourceid5", data, tc);
        PQTabletRestart(tc);
        PQGetPartInfo(0, 2, tc);

        ui32 toWrite = 5;
        for (ui32 i = 0; i < 2; ++i) {
            TString cookie = CmdSetOwner(0, tc).first;

            for (ui32 j = 0; j < toWrite + 1; ++j) {
                ui32 k = j;
                if (j == toWrite)
                    k = parts.size() - 1;
                WritePartData(0, "sourceid1", -1, j == toWrite ? 2 : 1, k, parts.size(), s.size(), parts[k], tc, cookie, j);

                TAutoPtr<IEventHandle> handle;
                TEvPersQueue::TEvResponse *result;

                result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

                UNIT_ASSERT(result);

                UNIT_ASSERT(result->Record.HasStatus());
                if (j == toWrite) {
                    UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::INITIALIZING);
                } else {
                    UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);

                    UNIT_ASSERT(result->Record.GetPartitionResponse().CmdWriteResultSize() == 1);
                    UNIT_ASSERT(result->Record.GetPartitionResponse().GetCmdWriteResult(0).HasAlreadyWritten());
                    UNIT_ASSERT(result->Record.GetPartitionResponse().GetCmdWriteResult(0).HasOffset());
                    UNIT_ASSERT(result->Record.GetPartitionResponse().GetCmdWriteResult(0).GetOffset() == 2);

                    auto res = result->Record.GetPartitionResponse().GetCmdWriteResult(0);
                    UNIT_ASSERT(!result->Record.GetPartitionResponse().GetCmdWriteResult(0).GetAlreadyWritten());
                }
            }
            PQGetPartInfo(0, i + 2, tc);
            toWrite = parts.size();
        }
        data.back().second.resize(64_KB);
        CmdWrite(0, "sourceid3", data, tc);
        CmdWrite(0, "sourceid5", data, tc);
        activeZone = true;
        data.back().second.resize(8_MB);
        CmdWrite(0, "sourceid7", data, tc);
        activeZone = false;
        {
            TString cookie = CmdSetOwner(0, tc).first;
            WritePartData(0, "sourceidX", 10, 1, 0, 5, s.size(), parts[1], tc, cookie, 0);

            TAutoPtr<IEventHandle> handle;
            TEvPersQueue::TEvResponse *result;

            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

            UNIT_ASSERT(result);
            UNIT_ASSERT(result->Record.HasStatus());
            UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);

            //check that after CmdSetOwner all partial data cleared
            cookie = CmdSetOwner(0, tc).first;
            WritePartData(0, "sourceidX", 12, 1, 0, 5, s.size(), parts[1], tc, cookie, 0);

            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

            UNIT_ASSERT(result);
            UNIT_ASSERT(result->Record.HasStatus());
            UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);

            //check gaps
            WritePartData(0, "sourceidX", 15, 1, 1, 5, s.size(), parts[1], tc, cookie, 1);

            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

            UNIT_ASSERT(result);
            UNIT_ASSERT(result->Record.HasStatus());
            UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::BAD_REQUEST);

            //check partNo gaps
            cookie = CmdSetOwner(0, tc).first;
            WritePartData(0, "sourceidX", 12, 1, 0, 5, s.size(), parts[1], tc, cookie, 0);

            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

            UNIT_ASSERT(result);
            UNIT_ASSERT(result->Record.HasStatus());
            UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);

            //check gaps
            WritePartData(0, "sourceidX", 12, 1, 4, 5, s.size(), parts[1], tc, cookie, 1);

            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

            UNIT_ASSERT(result);
            UNIT_ASSERT(result->Record.HasStatus());
            UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::INITIALIZING);

            //check very big msg
            cookie = CmdSetOwner(0, tc).first;
            WritePartData(0, "sourceidY", 13, 1, 0, 5, s.size(), TString{10_MB, 'a'}, tc, cookie, 0);

            result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

            UNIT_ASSERT(result);
            UNIT_ASSERT(result->Record.HasStatus());
            UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::BAD_REQUEST);
        }
        PQTabletRestart(tc);
    });
}

Y_UNIT_TEST(TestAlreadyWritten) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.EnableDetailedPQLog = true;
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({}, {}, tc); //no important clients, lifetimeseconds=0 - delete all right now, except last datablob
        activeZone = true;
        TVector<std::pair<ui64, TString>> data;

        TString s{32, 'c'};
        ui32 pp = 4 + 8 + 1 + 9;
        data.push_back({2, s.substr(pp)});
        data.push_back({1, s.substr(pp)});
        CmdWrite(0, "sourceid0", data, tc, false, {1}); //0 is written, 1 is already written
        data[0].first = 4;
        data[1].first = 3;
        CmdWrite(0, "sourceid0", data, tc, false, {3}); //0 is written, 1 is already written
        CmdWrite(0, "sourceid0", data, tc, false, {3, 4}); //all is already written
    });
}


Y_UNIT_TEST(TestAlreadyWrittenWithoutDeduplication) {
    // TODO(abcdef): temporarily deleted
    return;

    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({}, {}, tc); //no important clients, lifetimeseconds=0 - delete all right now, except last datablob
        TVector<std::pair<ui64, TString>> data;
        activeZone = true;

        TString s{32, 'c'};
        ui32 pp = 4 + 8 + 1 + 9;
        data.push_back({2, s.substr(pp)});
        CmdWrite(0, "sourceid0", data, tc, false, {}, false, "", -1, 0, false, false, true);
        data[0].first = 1;
        CmdWrite(0, "sourceid0", data, tc, false, {}, false, "", -1, 1, false, false, true);
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 2, false, tc, {0, 1});
    });
}


Y_UNIT_TEST(TestWritePQCompact) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        // No important clients <-> lifetimeseconds=0 - delete all right now, but last datablob
        PQTabletPrepare({.lowWatermark=(8_MB - 512_KB)}, {}, tc);

        TVector<std::pair<ui64, TString>> data;

        TString ss{1_MB - 100, '_'};
        TString s1{128_KB, 'a'};
        TString s2{2_KB, 'b'};
        TString s3{32, 'c'};
        ui32 pp = 4 + 8 + 2 + 9;
        for (ui32 i = 0; i < 8; ++i) {
            data.push_back({i + 1, ss.substr(pp)});
        }
        CmdWrite(0, "sourceid0", data, tc, false, {}, true); //now 1 blob
        PQGetPartInfo(0, 8, tc);
        data.clear();
        for (ui32 i = 0; i + s1.size() < 7_MB + 4 * s1.size(); i += s1.size()) {
            data.push_back({i + 1, s1.substr(pp)});
        }
        CmdWrite(0, "sourceid1", data, tc);
        PQGetPartInfo(0, 63 + 4, tc);
        data.clear();
        for (ui32 i = 0; i + s2.size() < s1.size(); i += s2.size()) {
            data.push_back({i + 1, s2.substr(pp)});
        }
        CmdWrite(0, "sourceid2", data, tc);
        PQGetPartInfo(8, 2 * 63 + 4, tc); //first is partial, not counted
        data.clear();
        for (ui32 i = 0; i + s3.size() + 540 < s2.size(); i += s3.size()) {
            data.push_back({i + 1, s3.substr(pp)});
        }
        CmdWrite(0, "sourceid3", data, tc); //now 1 blob and at most one

        PQGetPartInfo(8, 177, tc);
        data.resize(1);
        CmdWrite(0, "sourceid4", data, tc); //now 2 blobs, but delete will be done on next write
        //        PQGetUserInfo("aaa", 0, 8 + 88 * 3 + 1, -1, tc); dont check here, at may be deleted already(on restart OnWakeUp will occure)
        activeZone = true;
        CmdWrite(0, "sourceid5", data, tc); //next message just to force drop, don't wait for WakeUp
        activeZone = false;

        PQGetPartInfo(8, 179, tc);

    });
}


Y_UNIT_TEST(TestWritePQBigMessage) {
    // TODO(abcdef): temporarily deleted
    return;

    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({.lowWatermark=(8_MB - 512_KB)}, {{"user1", true}}, tc); //nothing dropped
                //no important clients, lifetimeseconds=0 - delete all right now, except last datablob

        TVector<std::pair<ui64, TString>> data;

        TString ss{50_MB - 100 - 2, '_'};
        TString s1{400_KB - 2, 'a'};
        ui32 pp = 4 + 8 + 2 + 9;
        char k = 0;
        TString s = "";
        s += k;
        s += ss.substr(pp);
        s += char((1) % 256);
        ++k;
        data.push_back({1, s});

        for (ui32 i = 0; i < 25;++i) {
            TString s = "";
            s += k;
            s += s1.substr(pp);
            s += char((i + 2) % 256);
            ++k;
            data.push_back({i + 2, s});
        }
        s = "";
        s += k;
        s += ss.substr(pp);
        s += char((1000) % 256);
        ++k;
        data.push_back({1000, s});
        CmdWrite(0, "sourceid0", data, tc, false, {}, true);
        PQGetPartInfo(0, 27, tc);

        CmdRead(0, 0, Max<i32>(), Max<i32>(), 1, false, tc);
        CmdRead(0, 1, Max<i32>(), Max<i32>(), 25, false, tc);
        CmdRead(0, 24, Max<i32>(), Max<i32>(), 2, false, tc);
        CmdRead(0, 26, Max<i32>(), Max<i32>(), 1, false, tc);

        activeZone = false;
    });
}


void TestWritePQImpl(bool fast) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        activeZone = false;
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(100);

        //tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(1'000);
        //tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsSize(200_MB);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        // Important client, lifetimeseconds=0 - never delete
        PQTabletPrepare({.partitions = 2, .writeSpeed = 200000000}, {{"user", true}}, tc);

        TVector<std::pair<ui64, TString>> data, data1, data2;
        activeZone = PlainOrSoSlow(true, false) && fast;

        TString ss{1_MB, '_'};
        TString s1{128_KB, 'a'};
        TString s2{2_KB, 'b'};
        TString s3{32, 'c'};
        ui32 pp = 4 + 8 + 2 + 9;

        TString sb{6_MB + 512_KB, '_'};
        data.push_back({1, sb.substr(pp)});
        CmdWrite(0, "sourceid0", data, tc, false, {}, true, "", -1, 100);
        activeZone = false;

        PQGetPartInfo(100, 101, tc);

        data1.push_back({1, s3.substr(pp)});
        data1.push_back({2, sb.substr(pp)});
        data2.push_back({1, s2.substr(pp)});
        data2.push_back({2, sb.substr(pp)});
        CmdWrite(0,"sourceid1", data1, tc);

        CmdWrite(0,"sourceid2", data2, tc);

        CmdWrite(0,"sourceid3", data1, tc);

        data.clear();
        data.push_back({1, s1.substr(pp)});
        data.push_back({2, ss.substr(pp)});
        CmdWrite(0,"sourceid4", data, tc);

        TString a1{8_MB - 1_KB, '_'};
        TString a2{2_KB, '_'};
        data.clear();
        data.push_back({1, a1.substr(pp)});
        data1.clear();
        data1.push_back({1, a2.substr(pp)});
        CmdWrite(0,"sourceid5", data, tc);
        CmdWrite(0,"sourceid6", data1, tc);
        CmdWrite(0,"sourceid7", data, tc);
        data.back().first = 4'296'000'000lu;
        CmdWrite(0,"sourceid8", data, tc);
        PQGetPartInfo(100, 113, tc);

        data1.push_back({2, a2.substr(pp)});
        CmdWrite(0,"sourceId9", data1, tc, false, {}, false, "", -1, 1000);
        PQGetPartInfo(100, 1002, tc);

        data1.front().first = 3;
        data1.back().first = 4;

        CmdWrite(0,"sourceId9", data1, tc, false, {}, false, "", -1, 2000);
        PQGetPartInfo(100, 2002, tc);

        activeZone = fast;

        data1.push_back(data1.back());
        data1[1].first = 3;
        CmdWrite(0,"sourceId10", data1, tc, false, {}, false, "", -1, 3000);
        PQGetPartInfo(100, 3003, tc);

        activeZone = false;
        if (fast) return;

        CmdWrite(1,"sourceId9", data1, tc, false, {}, false, "", -1, 2000); //to other partition

        data1.clear();
        data1.push_back({1, TString{200, 'a'}});
        for (ui32 i = 1; i <= NUM_WRITES; ++i) {
            data1.front().first = i;
            CmdWrite(1, "sourceidx", data1, tc, false, {}, false, "", -1);
        }

        //read all, check offsets
        CmdRead(0, 111, Max<i32>(), Max<i32>(), 8, false, tc, {111,112,1000,1001,2000,2001,3000,3002});

        //read from gap
        CmdRead(0, 500, Max<i32>(), Max<i32>(), 6, false, tc, {1000,1001,2000,2001,3000,3002});

        // Write long sourceId
        // The write should not be executed because the SourceId exceeds the maximum allowed size
        CmdWrite(0, TString(10_KB, '_'), data1, tc, true, {}, false, "", -1, 10000);

        // Write long sourceId
        // The write must be completed successfully because the SourceId has the maximum allowed size
        CmdWrite(0, TString(2_KB, '_'), data1, tc, false, {}, false, "", -1, 10000);
    });
}

Y_UNIT_TEST(TestWritePQ) {
    TestWritePQImpl(true);
    TestWritePQImpl(false);
}


Y_UNIT_TEST(TestSourceIdDropByUserWrites) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(200);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        PQTabletPrepare({}, {}, tc); //no important client, lifetimeseconds=0 - delete right now

        TVector<std::pair<ui64, TString>> data;
        activeZone = true;

        TString ss{32, '_'};

        data.push_back({1, ss});
        CmdWrite(0,"sourceid0", data, tc, false, {}, false, "", -1, 100);

        PQGetPartInfo(100, 101, tc);

        CmdWrite(0,"sourceidx", data, tc, false, {}, false, "", -1, 2000);
        CmdWrite(0,"sourceid1", data, tc, false, {}, false, "", -1, 3000);
        PQGetPartInfo(2000, 3001, tc);
        //fail - already written
        CmdWrite(0,"sourceid0", data, tc, false);
        PQGetPartInfo(2000, 3001, tc);

        tc.Runtime->UpdateCurrentTime(tc.Runtime->GetCurrentTime() + TDuration::Minutes(61));
        CmdWrite(0,"sourceid0", data, tc, false);
        CmdWrite(0,"sourceid0", data, tc, false); //second attempt just to be sure that DropOldSourceId is called after previos write, not only on Wakeup
        //ok, hour waited - record writted twice
        PQGetPartInfo(2000, 3002, tc);
    });
}


Y_UNIT_TEST(TestSourceIdDropBySourceIdCount) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(200);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        PQTabletPrepare({.sidMaxCount=3}, {}, tc); //no important client, lifetimeseconds=0 - delete right now

        TVector<std::pair<ui64, TString>> data;
        activeZone = true;

        TString ss{32, '_'};

        data.push_back({1, ss});
        CmdWrite(0,"sourceid0", data, tc, false, {}, false, "", -1, 100);
        Cout << "written sourceid0" << Endl;

        PQGetPartInfo(100, 101, tc);

        CmdWrite(0,"sourceidx", data, tc, false, {}, false, "", -1, 2000);
        Cout << "written sourceidx" << Endl;
        CmdWrite(0,"sourceid1", data, tc, false, {}, false, "", -1, 3000);
        Cout << "written sourceid1" << Endl;
        PQGetPartInfo(2000, 3001, tc);
        //fail - already written
        CmdWrite(0,"sourceid0", data, tc, false);
        Cout << "written sourceid0" << Endl;
        PQGetPartInfo(2000, 3001, tc);

        for (ui64 i=0; i < 5; ++i) {
            CmdWrite(0, TStringBuilder() << "sourceid_" << i, data, tc, false, {}, false, "", -1, 3001 + i);
            Cout << "written sourceid_" << i << Endl;
        }
        CmdWrite(0,"sourceid0", data, tc, false);
        Cout << "written sourceid0" << Endl;
        PQGetPartInfo(2000, 3007, tc);
    });
}


Y_UNIT_TEST(TestWriteOffsetWithBigMessage) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({.partitions=3}, {{{"user", true}}}, tc); //important client, lifetimeseconds=0 - never delete

        activeZone = false;

        TVector<std::pair<ui64, TString>> data;

        data.push_back({1, TString{10_MB, 'a'}});
        CmdWrite(1, "sourceIdx", data, tc, false, {}, false, "", -1, 80'000);
        data.front().first = 2;
        CmdWrite(1, "sourceIdx", data, tc, false, {}, false, "", -1, 160'000);

        data.clear();
        data.push_back({1, TString{100_KB, 'a'}});
        for (ui32 i = 0; i < 100; ++i) {
            data.push_back(data.front());
            data.back().first = i + 2;
        }
        CmdWrite(0, "sourceIdx", data, tc, false, {}, false, "", -1, 80'000);
        PQGetPartInfo(80'000, 80'101, tc);
        data.resize(70);
        CmdWrite(2, "sourceId1", data, tc, false, {}, false, "", -1, 0);
        CmdWrite(2, "sourceId2", data, tc, false, {}, false, "", -1, 80'000);
    });
}


Y_UNIT_TEST(TestWriteSplit) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(200);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        PQTabletPrepare({}, {{"user1", true}}, tc); //never delete
        const ui32 size  = PlainOrSoSlow(2_MB, 1_MB);

        TVector<std::pair<ui64, TString>> data;
        data.push_back({1, TString{size, 'b'}});
        data.push_back({2, TString{size, 'a'}});
        activeZone = PlainOrSoSlow(true, false);
        CmdWrite(0, "sourceIdx", data, tc, false, {}, false, "", -1, 40'000);
        PQTabletRestart(tc);
        activeZone = false;
        PQGetPartInfo(40'000, 40'002, tc);
    });
}

Y_UNIT_TEST(TestTimeRetention) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        activeZone = false;
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(100);

        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        TVector<std::pair<ui64, TString>> data;
        activeZone = PlainOrSoSlow(true, false);

        TString s{32, 'c'};
        ui32 pp = 8 + 4 + 2 + 9;
        for (ui32 i = 0; i < 10; ++i) {
            data.push_back({i + 1, s.substr(pp)});
        }
        PQTabletPrepare({.maxCountInPartition=1000, .deleteTime=TDuration::Seconds(1000).Seconds(),
                .lowWatermark=100}, {}, tc);
        CmdWrite(0, "sourceid0", data, tc, false, {}, true);
        CmdWrite(0, "sourceid1", data, tc, false);
        CmdWrite(0, "sourceid2", data, tc, false);
        PQGetPartInfo(0, 30, tc);

        PQTabletPrepare({.maxCountInPartition=1000, .deleteTime=0, .lowWatermark=100}, {}, tc);
        CmdWrite(0, "sourceid3", data, tc, false);
        CmdWrite(0, "sourceid4", data, tc, false);
        CmdWrite(0, "sourceid5", data, tc, false);
        PQGetPartInfo(50, 60, tc);
    });
}

Y_UNIT_TEST(TestComactifiedWithRetention) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        activeZone = false;
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(100);

        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        TVector<std::pair<ui64, TString>> data;
        activeZone = PlainOrSoSlow(true, false);

        TString s{32, 'c'};
        ui32 pp = 8 + 4 + 2 + 9;
        for (ui32 i = 0; i < 10; ++i) {
            data.push_back({i + 1, s.substr(pp)});
        }
        PQTabletPrepare({.maxCountInPartition=1000, .deleteTime=0, .lowWatermark=100, .enableCompactificationByKey = true}, {}, tc);
        CmdWrite(0, "sourceid0", data, tc, false, {}, true);
        CmdWrite(0, "sourceid1", data, tc, false);
        CmdWrite(0, "sourceid2", data, tc, false);
        PQGetPartInfo(0, 30, tc);
        PQTabletPrepare({.maxCountInPartition=1000, .deleteTime=0, .lowWatermark=100, .enableCompactificationByKey = false}, {}, tc);
        CmdWrite(0, "sourceid3", data, tc, false);
        CmdWrite(0, "sourceid4", data, tc, false);
        CmdWrite(0, "sourceid5", data, tc, false);
        PQGetPartInfo(50, 60, tc);
    });
}

Y_UNIT_TEST(TestStorageRetention) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        activeZone = false;
        tc.EnableDetailedPQLog = true;
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(100);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        TVector<std::pair<ui64, TString>> data;
        activeZone = PlainOrSoSlow(true, false);

        TString s{32, 'c'};
        ui32 pp = 8 + 4 + 2 + 9;
        for (ui32 i = 0; i < 10; ++i) {
            data.push_back({i + 1, s.substr(pp)});
        }
        PQTabletPrepare({.maxCountInPartition=1000, .lowWatermark=100, .storageLimitBytes=1_MB}, {}, tc);
        CmdWrite(0, "sourceid0", data, tc, false, {}, true); //now 1 blob
        CmdWrite(0, "sourceid1", data, tc, false);
        CmdWrite(0, "sourceid2", data, tc, false);
        PQGetPartInfo(0, 30, tc);

        PQTabletPrepare({.maxCountInPartition=1000, .lowWatermark=50, .storageLimitBytes=160}, {}, tc);
        CmdWrite(0, "sourceid3", data, tc, false);
        CmdWrite(0, "sourceid4", data, tc, false);
        PQGetPartInfo(40, 50, tc);
    });
}



Y_UNIT_TEST(TestPQPartialRead) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({}, {{"aaa", true}}, tc); //important client - never delete

        activeZone = false;
        TVector<std::pair<ui64, TString>> data;

        ui32 pp =  4 + 8 + 2 + 9 + 100 + 40; //pp is for size of meta
        TString tmp{1_MB - pp - 2, '-'};
        char k = 0;
        TString ss = "";
        ss += k;
        ss += tmp;
        ss += char(1);
        ++k;
        data.push_back({1, ss});

        CmdWrite(0, "sourceid0", data, tc, false, {}, true); //now 1 blob
        PQGetPartInfo(0, 1, tc);

        CmdRead(0, 0, 1, 1, 1, false, tc);
    });
}


Y_UNIT_TEST(TestPQRead) {
    // TODO(abcdef): temporarily deleted
    return;

    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(200);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        PQTabletPrepare({}, {{"aaa", true}}, tc); //important client - never delete

        activeZone = false;
        TVector<std::pair<ui64, TString>> data;

        ui32 pp =  4 + 8 + 2 + 9 + 100 + 40; //pp is for size of meta
        TString tmp{1_MB - pp - 2, '-'};
        char k = 0;
        for (ui32 i = 0; i < 26_MB;) { //3 full blobs and 2 in head
            TString ss = "";
            ss += k;
            ss += tmp;
            ss += char((i + 1) % 256);
            ++k;
            data.push_back({i + 1, ss});
            i += ss.size() + pp;
        }
        CmdWrite(0, "sourceid0", data, tc, false, {}, true); //now 1 blob
        PQGetPartInfo(0, 26, tc);

        CmdRead(0, 26, Max<i32>(), Max<i32>(), 0, true, tc);

        CmdRead(0, 0, Max<i32>(), Max<i32>(), 25, false, tc);
        CmdRead(0, 0, 10, 100_MB, 10, false, tc);
        CmdRead(0, 9, 1, 100_MB, 1, false, tc);
        CmdRead(0, 23, 3, 100_MB, 3, false, tc);

        CmdRead(0, 3, 1000, 511_KB, 12, false, tc);
        CmdRead(0, 3, 1000, 511_KB, 12, false, tc);
        CmdRead(0, 3, 1000, 1_KB, 12, false, tc); //at least one message will be readed always
        CmdRead(0, 25, 1000, 1_KB, 1, false, tc); //at least one message will be readed always, from head

        activeZone = true;
        CmdRead(0, 9, 1000, 3_MB, 14, false, tc);
        CmdRead(0, 9, 1000, 3_MB - 10_KB, 14, false, tc);
        CmdRead(0, 25, 1000, 512_KB, 1, false, tc); //from head
        CmdRead(0, 24, 1000, 512_KB, 1, false, tc); //from head

        CmdRead(0, 23, 1000, 98_MB, 3, false, tc);
    });
}


Y_UNIT_TEST(TestPQSmallRead) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(200);

        PQTabletPrepare({}, {{"aaa", true}}, tc); //important client - never delete

        activeZone = false;
        TVector<std::pair<ui64, TString>> data;

        ui32 pp =  4 + 8 + 2 + 9 ; //5 is for 8 blobs for header
        TString tmp{32 - pp - 2, '-'};
        char k = 0;
        TString ss = "";
        ss += k;
        ss += tmp;
        ss += char(1);
        data.push_back({1, ss});
        CmdWrite(0, "sourceid0", data, tc, false, {}, true);
        ++k; data[0].second = TString(1, k) + tmp + char(1);
        CmdWrite(0, "sourceid1", data, tc, false, {}, false);
        ++k; data[0].second = TString(1, k) + tmp + char(1);
        CmdWrite(0, "sourceid2", data, tc, false, {}, false);
        ++k; data[0].second = TString(1, k) + tmp + char(1);
        CmdWrite(0, "sourceid3", data, tc, false, {}, false);
        ++k; data[0].second = TString(1, k) + tmp + char(1);
        CmdWrite(0, "sourceid4", data, tc, false, {}, false);
        PQGetPartInfo(0, 5, tc);

        CmdRead(0, 5, Max<i32>(), Max<i32>(), 0, true, tc);
        CmdRead(0, 0, Max<i32>(), Max<i32>(), 5, false, tc);
        CmdRead(0, 0, 3, 100_MB, 3, false, tc);
        CmdRead(0, 3, 1000, 1_KB, 2, false, tc);
    });
}

Y_UNIT_TEST(TestPQReadAhead) {
    // TODO(abcdef): temporarily deleted
    return;

    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;

        tc.Runtime->SetScheduledLimit(200);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        PQTabletPrepare({}, {{"aaa", true}}, tc); //important client - never delete

        TVector<std::pair<ui64, TString>> data;

        ui32 pp = 8 + 4 + 2 + 9;
        TString tmp{1_MB - pp - 2, '-'};
        TString tmp0{32 - pp - 2, '-'};
        char k = 0;
        for (ui32 i = 0; i < 5; ++i) {
            TString ss = "";
            ss += k;
            ss += tmp0;
            ss += char((i + 1) % 256);
            ++k;
            data.push_back({i + 1, ss});
        }
        for (ui32 i = 0; i < 17_MB;) { //3 full blobs and 2 in head
            TString ss = "";
            ss += k;
            ss += tmp;
            ss += char((i + 10) % 256);
            ++k;
            data.push_back({i + 10, ss});
            i += ss.size() + pp;
        }
        CmdWrite(0, "sourceid0", data, tc, false, {}, true); //now 1 blob
        PQGetPartInfo(0, 22, tc);
        activeZone = true;

        CmdRead(0, 0, 1, 100_MB, 1, false, tc);
        CmdRead(0, 1, 1, 100_MB, 1, false, tc);
        CmdRead(0, 2, 1, 100_MB, 1, false, tc);
        CmdRead(0, 3, 1, 100_MB, 1, false, tc);
        CmdRead(0, 4, 10, 100_MB, 10, false, tc);

        CmdRead(0, 0, Max<i32>(), 100_KB, 12, false, tc);
        CmdRead(0, 1, Max<i32>(), 100_KB, 19, false, tc);
        CmdRead(0, 2, Max<i32>(), 100_KB, 18, false, tc);
        CmdRead(0, 3, Max<i32>(), 100_KB, 17, false, tc);
    });
}

Y_UNIT_TEST(TestOwnership) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(50);

        PQTabletPrepare({.maxCountInPartition=10}, {}, tc);

        TString cookie, cookie2;
        cookie = CmdSetOwner(0, tc).first;
        UNIT_ASSERT(!cookie.empty());
        cookie2 = CmdSetOwner(0, tc).first;
        UNIT_ASSERT(!cookie2.empty());
        UNIT_ASSERT(cookie2 != cookie);
    });
}

Y_UNIT_TEST(TestSetClientOffset) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(50);

        PQTabletPrepare({.maxCountInPartition=10}, {{"user1", false}}, tc);

        activeZone = true;

        TVector<std::pair<ui64, TString>> data;

        CmdSetOffset(0, "user1", 100, false, tc); //must be true , error
        CmdGetOffset(0, "user1", 0, tc); // must be -1

        activeZone = PlainOrSoSlow(true, false);

        CmdSetOffset(0, "user1", 0, false, tc);
        CmdGetOffset(0, "user1", 0, tc);
        CmdSetOffset(0, "user1", 0, false, tc);
        CmdGetOffset(0, "user1", 0, tc);
        CmdSetOffset(0, "user1", 0, false, tc);
        CmdGetOffset(0, "user1", 0, tc);
        CmdGetOffset(0, "user2", 0, tc);
    });
}

Y_UNIT_TEST(TestReadSessions) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(50);

        PQTabletPrepare({.maxCountInPartition=10}, {{"user1", false}}, tc);

        activeZone = true;

        TVector<std::pair<ui64, TString>> data;
        CmdCreateSession(TPQCmdSettings{0, "user1", "session1"}, tc);
        CmdSetOffset(0, "user1", 0, false, tc, "session1"); //all ok - session is set
        CmdSetOffset(0, "user1", 0, true, tc, "other_session"); //fails - session1 is active

        activeZone = PlainOrSoSlow(true, false);

        CmdSetOffset(0, "user1", 0, false, tc, "session1");

        CmdCreateSession(TPQCmdSettings{0, "user1", "session2", 0, 1, 1}, tc);
        CmdCreateSession(TPQCmdSettings{0, "user1", "session3", 0, 1, 1, true}, tc); //error on creation
        CmdCreateSession(TPQCmdSettings{0, "user1", "session3", 0, 0, 2, true}, tc); //error on creation
        CmdCreateSession(TPQCmdSettings{0, "user1", "session3", 0, 0, 0, true}, tc); //error on creation
        CmdSetOffset(0, "user1", 0, true, tc, "session1");
        CmdSetOffset(0, "user1", 0, true, tc, "session3");
        CmdSetOffset(0, "user1", 0, false, tc, "session2");

        activeZone = true;

        CmdKillSession(0, "user1", "session2", tc);
        CmdSetOffset(0, "user1", 0, true, tc, "session2"); //session is dead now
    });
}



Y_UNIT_TEST(TestGetTimestamps) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        tc.Runtime->SetScheduledLimit(50);

        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        tc.Runtime->UpdateCurrentTime(TInstant::Zero() + TDuration::Days(2));
        activeZone = false;

        PQTabletPrepare({.maxCountInPartition=10}, {{"user1", false}}, tc);

        TVector<std::pair<ui64, TString>> data;
        data.push_back({1, TString(1_KB, 'a')});
        data.push_back({2, TString(1_KB, 'a')});
        data.push_back({3, TString(1_KB, 'a')});
        data.push_back({4, TString(1_KB, 'a')});

        CmdWrite(0, "sourceid0", data, tc, false, {}, true, "", -1, 1);
        CmdGetOffset(0, "user1", 0, tc, -1);

        CmdSetOffset(0, "user1", 1, true, tc);
        CmdSetOffset(0, "user1", 0, true, tc);
        CmdGetOffset(0, "user1", 0, tc, Max<i64>());
        CmdSetOffset(0, "user1", 1, true, tc);
        CmdGetOffset(0, "user1", 1, tc, 1);
        CmdSetOffset(0, "user1", 3, true, tc);
        CmdGetOffset(0, "user1", 3, tc, 3);
        CmdSetOffset(0, "user1", 4, true, tc);
        CmdGetOffset(0, "user1", 4, tc, 4);
        CmdSetOffset(0, "user1", 5, true, tc);
        CmdGetOffset(0, "user1", 5, tc, 4);
        CmdSetOffset(0, "user1", 5, true, tc);
        CmdWrite(0, "sourceid1", data, tc, false, {}, false);
        CmdGetOffset(0, "user1", 5, tc, 5);
        PQTabletRestart(tc);
        CmdGetOffset(0, "user1", 5, tc, 5);

        CmdWrite(0, "sourceid2", data, tc, false, {}, false, "", -1,100);
        CmdRead(0, 100, Max<i32>(), Max<i32>(), 4, false, tc, {100, 101, 102, 103}); // all offsets will be putted in cache

        //check offset inside gap
        CmdSetOffset(0, "user", 50, true, tc);
        CmdGetOffset(0, "user", 50, tc, 100);

        CmdSetOffset(0, "user", 101, true, tc);
        CmdGetOffset(0, "user", 101, tc, 101);
    });
}


Y_UNIT_TEST(TestChangeConfig) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        activeZone = false;
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(50);

        TVector<std::pair<ui64, TString>> data;

        ui32 pp = 8 + 4 + 2 + 9;
        TString tmp0{32 - pp - 2, '-'};
        char k = 0;
        for (ui32 i = 0; i < 5; ++i) {
            TString ss = "";
            ss += k;
            ss += tmp0;
            ss += char((i + 1) % 256);
            ++k;
            data.push_back({i + 1, ss});
        }

        PQTabletPrepare({.maxCountInPartition=100, .deleteTime=TDuration::Days(2).Seconds(), .partitions=5},
                        {{"aaa", true}}, tc);
        CmdWrite(0, "sourceid0", data, tc, false, {}, true); //now 1 blob

        PQTabletPrepare({.maxCountInPartition=5, .maxSizeInPartition=1_MB,
                .deleteTime=TDuration::Days(1).Seconds(), .partitions=10}, {{"bbb", true}, {"ccc", true}}, tc);
        data.pop_back(); //to be sure that after write partition will no be full
        CmdWrite(0, "sourceid1", data, tc);
        CmdWrite(1, "sourceid2", data, tc);
        CmdWrite(9, "sourceid3", data, tc); //now 1 blob
    });
}

Y_UNIT_TEST(TestReadSubscription) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(600);
        tc.Runtime->SetScheduledEventFilter(&tc.ImmediateLogFlushAndRequestTimeoutFilter);

        TVector<std::pair<ui64, TString>> data;

        ui32 pp = 8 + 4 + 2 + 9;
        TString tmp0{32 - pp - 2, '-'};
        char k = 0;
        for (ui32 i = 0; i < 5; ++i) {
            TString ss = "";
            ss += k;
            ss += tmp0;
            ss += char((i + 1) % 256);
            ++k;
            data.push_back({i + 1, ss});
        }

        PQTabletPrepare({.maxCountInPartition=100, .deleteTime=TDuration::Days(2).Seconds(), .partitions=5},
                        {{"user1", true}}, tc);
        CmdWrite(0, "sourceid0", data, tc, false, {}, true);

        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvResponse *result;
        THolder<TEvPersQueue::TEvRequest> request;

        request.Reset(new TEvPersQueue::TEvRequest);
        auto req = request->Record.MutablePartitionRequest();
        req->SetPartition(0);
        auto read = req->MutableCmdRead();
        read->SetOffset(5);
        read->SetClientId("user1");
        read->SetCount(5);
        read->SetBytes(1'000'000);
        read->SetTimeoutMs(5000);

        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries());

        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);

        UNIT_ASSERT(result);
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK); //read without write must be timeouted
        UNIT_ASSERT_EQUAL(result->Record.GetPartitionResponse().GetCmdReadResult().ResultSize(), 0); //read without write must be timeouted

        request.Reset(new TEvPersQueue::TEvRequest);
        req = request->Record.MutablePartitionRequest();
        req->SetPartition(0);
        read = req->MutableCmdRead();
        read->SetOffset(5);
        read->SetClientId("user1");
        read->SetCount(3);
        read->SetBytes(1'000'000);
        read->SetTimeoutMs(5000);

        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries()); //got read

        CmdWrite(0, "sourceid1", data, tc); //write

        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle); //now got data

        UNIT_ASSERT(result);
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);
        UNIT_ASSERT_EQUAL(result->Record.GetPartitionResponse().GetCmdReadResult().ResultSize(), 3); //got response, but only for 3 from 5 writed blobs

        request.Reset(new TEvPersQueue::TEvRequest);
        req = request->Record.MutablePartitionRequest();
        req->SetPartition(0);
        read = req->MutableCmdRead();
        read->SetOffset(10);
        read->SetClientId("user1");
        read->SetCount(55);
        read->SetBytes(1'000'000);
        read->SetTimeoutMs(5000);

        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries()); //got read

        CmdWrite(0, "sourceid2", data, tc); //write

        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle); //now got data

        UNIT_ASSERT(result);
        UNIT_ASSERT(result->Record.HasStatus());
        UNIT_ASSERT_EQUAL(result->Record.GetErrorCode(), NPersQueue::NErrorCode::OK);
        UNIT_ASSERT_EQUAL(result->Record.GetPartitionResponse().GetCmdReadResult().ResultSize(), 5); //got response for whole written blobs
    });
}

//


Y_UNIT_TEST(TestPQCacheSizeManagement) {
    // TODO(abcdef): temporarily deleted
    return;

    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(200);

        activeZone = false;
        PQTabletPrepare({}, {{"aaa", true}}, tc); //important client - never delete

        TVector<std::pair<ui64, TString>> data;

        ui32 pp =  4 + 8 + 2 + 9 + 100;
        TString tmp{1_MB - pp - 2, '-'};
        char k = 0;
        for (ui32 i = 0; i < 26_MB;) {
            TString ss = "";
            ss += k;
            ss += tmp;
            ss += char((i + 1) % 256);
            ++k;
            data.push_back({i + 1, ss});
            i += ss.size() + pp;
        }
        CmdWrite(0, "sourceid0", data, tc, false, {}, true);
        PQGetPartInfo(0, 26, tc);

        TAutoPtr<IEventHandle> handle;
        for (ui32 i = 0; i < 10; ++i) {
            CmdRead(0, 0, 1, 100_MB, 1, false, tc);
            PQTabletRestart(tc);
        }
    });
}

Y_UNIT_TEST(TestOffsetEstimation) {
    std::deque<NPQ::TDataKey> container = {
        {NPQ::TKey::ForBody(NPQ::TKeyPrefix::EType::TypeNone, TPartitionId(0), 1, 0, 0, 0), 0, TInstant::Seconds(1), 10},
        {NPQ::TKey::ForBody(NPQ::TKeyPrefix::EType::TypeNone, TPartitionId(0), 2, 0, 0, 0), 0, TInstant::Seconds(1), 10},
        {NPQ::TKey::ForBody(NPQ::TKeyPrefix::EType::TypeNone, TPartitionId(0), 3, 0, 0, 0), 0, TInstant::Seconds(2), 10},
        {NPQ::TKey::ForBody(NPQ::TKeyPrefix::EType::TypeNone, TPartitionId(0), 4, 0, 0, 0), 0, TInstant::Seconds(2), 10},
        {NPQ::TKey::ForBody(NPQ::TKeyPrefix::EType::TypeNone, TPartitionId(0), 5, 0, 0, 0), 0, TInstant::Seconds(3), 10},
        {NPQ::TKey::ForBody(NPQ::TKeyPrefix::EType::TypeNone, TPartitionId(0), 6, 0, 0, 0), 0, TInstant::Seconds(3), 10},
    };
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate({}, TInstant::MilliSeconds(0), 9999), 9999);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(0), 9999), 1);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(500), 9999), 1);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(1000), 9999), 1);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(1500), 9999), 3);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(2000), 9999), 3);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(2500), 9999), 5);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(3000), 9999), 5);
    UNIT_ASSERT_EQUAL(NPQ::GetOffsetEstimate(container, TInstant::MilliSeconds(3500), 9999), 9999);
}

Y_UNIT_TEST(TestMaxTimeLagRewind) {
    TTestContext tc;

    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);

        tc.Runtime->SetScheduledLimit(200);
        tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

        PQTabletPrepare({}, {{"aaa", true}}, tc);
        activeZone = false;


        for (int i = 0; i < 5; i++) {
            TVector<std::pair<ui64, TString>> data;
            for (int j = 0; j < 7; j++) {
                data.push_back({7 * i + j + 1, TString(1_MB, 'a')});
            }
            CmdWrite(0, "sourceid0", data, tc, false, {}, i == 0);
            tc.Runtime->UpdateCurrentTime(tc.Runtime->GetCurrentTime() + TDuration::Minutes(1));
        }
        const auto ts = tc.Runtime->GetCurrentTime();

        CmdRead(0, 0, 1, Max<i32>(), 1, false, tc, {0});
        CmdRead(0, 0, 1, Max<i32>(), 1, false, tc, {21}, TDuration::Minutes(3).MilliSeconds());
        CmdRead(0, 22, 1, Max<i32>(), 1, false, tc, {22}, TDuration::Minutes(3).MilliSeconds());
        CmdRead(0, 4, 1, Max<i32>(), 1, false, tc, {34}, 1000);

        CmdRead(0, 0, 1, Max<i32>(), 1, false, tc, {21}, 0,
                (ts - TDuration::Minutes(3)).MilliSeconds());
        CmdRead(0, 22, 1, Max<i32>(), 1, false, tc, {22}, 0,
                (ts - TDuration::Minutes(3)).MilliSeconds());
        CmdRead(0, 4, 1, Max<i32>(), 1, false, tc, {34}, 0,
                (ts - TDuration::Seconds(1)).MilliSeconds());

        PQTabletPrepare({.readFromTimestampsMs=(ts - TDuration::Seconds(1)).MilliSeconds()},
                        {{"aaa", true}}, tc);
        CmdRead(0, 0, 1, Max<i32>(), 1, false, tc, {34});

    });
}


Y_UNIT_TEST(TestWriteTimeStampEstimate) {
    TTestContext tc;
    TFinalizer finalizer(tc);
    tc.Prepare();

    tc.Runtime->SetScheduledLimit(150);
    tc.Runtime->SetDispatchTimeout(TDuration::Seconds(1));
    tc.Runtime->SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);

    PQTabletPrepare({}, {{"aaa", true}}, tc);

    tc.Runtime->UpdateCurrentTime(TInstant::MilliSeconds(1'000'000));

    TVector<std::pair<ui64, TString>> data{{1,"abacaba"}};
    CmdWrite(0, "sourceid0", data, tc);

    CmdGetOffset(0, "user1", 0, tc, -1, 1'000'000);

    PQTabletPrepare({.localDC=false}, {{"aaa", true}}, tc);

    PQTabletRestart(tc);

    CmdGetOffset(0, "user1", 0, tc, -1, 0);

    tc.Runtime->UpdateCurrentTime(TInstant::MilliSeconds(2'000'000));

    data.front().first = 2;
    CmdWrite(0, "sourceid0", data, tc);

    CmdGetOffset(0, "user1", 0, tc, -1, 2'000'000);

    CmdUpdateWriteTimestamp(0, 3'000'000, tc);

    CmdGetOffset(0, "user1", 0, tc, -1, 3'000'000);

}



Y_UNIT_TEST(TestWriteTimeLag) {
    TTestContext tc;
    tc.EnableDetailedPQLog = true;
    TFinalizer finalizer(tc);
    tc.Prepare();

    tc.Runtime->SetScheduledLimit(150);
    tc.Runtime->SetDispatchTimeout(TDuration::Seconds(1));
    tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

    PQTabletPrepare({.maxSizeInPartition=1_TB}, {{"aaa", false}}, tc);

    TVector<std::pair<ui64, TString>> data{{1,TString(1_MB, 'a')}};
    for (ui32 i = 0; i < 20; ++i) {
        CmdWrite(0, TStringBuilder() << "sourceid" << i, data, tc);
    }

    // After restart all caches are empty.
    PQTabletRestart(tc);

    while (true) {
        auto startOffset = PQGetStartOffset(tc);
        if (startOffset > 0) {
            break;
        }
    }

    PQTabletPrepare({.maxSizeInPartition=1_TB}, {{"aaa", false}, {"important", true}, {"another", true}}, tc);
    PQTabletPrepare({.maxSizeInPartition=1_TB}, {{"aaa", false}, {"another1", true}, {"important", true}}, tc);
    PQTabletPrepare({.maxSizeInPartition=1_TB},
                    {{"aaa", false}, {"another1", true}, {"important", true}, {"another", false}}, tc);

    CmdGetOffset(0, "important", 12, tc, -1, 0);

    CmdGetOffset(0, "another1", 12, tc, -1, 0);
    CmdGetOffset(0, "another", 0, tc, -1, 0);
    CmdGetOffset(0, "aaa", 0, tc, -1, 0);
}

Y_UNIT_TEST(TestManyConsumers) {
    TTestContext tc;
    TFinalizer finalizer(tc);
    tc.Prepare();

    tc.Runtime->SetScheduledLimit(150);
    tc.Runtime->SetDispatchTimeout(TDuration::Seconds(1));
    tc.Runtime->SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);

    TVector<std::pair<TString, bool>> consumers;
    for (ui32 i = 0; i < 2000; ++i) {
        consumers.push_back(std::make_pair<TString, bool>(TStringBuilder() << "consumer_" << i, false));
    }

    PQTabletPrepare({}, consumers, tc);

    TFakeSchemeShardState::TPtr state{new TFakeSchemeShardState()};
    ui64 ssId = 325;
    BootFakeSchemeShard(*tc.Runtime, ssId, state);

    for (ui32 i = 0; i < 100; ++i) {
        PQBalancerPrepare(TOPIC_NAME, {{0,{tc.TabletId, 1}}}, ssId, tc, false, false);
    }

    for (ui32 i = 0; i < 100; ++i) {
        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, new TEvPersQueue::TEvStatus(), 0, GetPipeConfigWithRetries());

        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvStatusResponse *result;
        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvStatusResponse>(handle);
        Y_UNUSED(result);
    }
    PQBalancerPrepare(TOPIC_NAME, {{0,{tc.TabletId, 1}}}, ssId, tc, false, true);

}

Y_UNIT_TEST(TestStatusWithMultipleConsumers) {
    TTestContext tc;
    TFinalizer finalizer(tc);
    tc.Prepare();

    tc.Runtime->SetScheduledLimit(150);
    tc.Runtime->SetDispatchTimeout(TDuration::Seconds(1));
    tc.Runtime->SetLogPriority(NKikimrServices::PERSQUEUE, NLog::PRI_DEBUG);

    TVector<std::pair<TString, bool>> consumers {
        std::pair("consumer-0", false),
        std::pair("consumer-1", false)};

    PQTabletPrepare({}, consumers, tc);

    TVector<std::pair<ui64, TString>> data{{1,"foobar"}};
    CmdWrite(0, "sourceid0", data, tc);

    CmdSetOffset(0, "consumer-0", 1, false, tc);

    TFakeSchemeShardState::TPtr state {new TFakeSchemeShardState()};
    ui64 ssId = 325;
    BootFakeSchemeShard(*tc.Runtime, ssId, state);

    for (ui32 i = 0; i < 100; ++i) {
        PQBalancerPrepare(TOPIC_NAME, {{0,{tc.TabletId, 1}}}, ssId, tc, false, false);
    }

    {
        THolder<TEvPersQueue::TEvStatus> statusEvent = MakeHolder<TEvPersQueue::TEvStatus>();
        statusEvent->Record.AddConsumers("consumer-0");
        statusEvent->Record.AddConsumers("consumer-1");
        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, statusEvent.Release(), 0, GetPipeConfigWithRetries());
        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvStatusResponse *result;
        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvStatusResponse>(handle);
        UNIT_ASSERT_EQUAL(result->Record.GetPartResult()[0].GetConsumerResult().size(),  2);
        UNIT_ASSERT_EQUAL(result->Record.GetPartResult()[0].GetConsumerResult()[0].GetErrorCode(), NPersQueue::NErrorCode::OK);
        UNIT_ASSERT_EQUAL(result->Record.GetPartResult()[0].GetConsumerResult()[1].GetErrorCode(), NPersQueue::NErrorCode::OK);
    }

    {
        THolder<TEvPersQueue::TEvStatus> statusEvent = MakeHolder<TEvPersQueue::TEvStatus>();
        statusEvent->Record.AddConsumers("nonex-consumer-2");
        statusEvent->Record.AddConsumers("nonex-consumer-3");
        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, statusEvent.Release(), 0, GetPipeConfigWithRetries());
        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvStatusResponse *result;
        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvStatusResponse>(handle);
        UNIT_ASSERT_EQUAL(result->Record.GetPartResult()[0].GetConsumerResult().size(),  2);
        UNIT_ASSERT_EQUAL(result->Record.GetPartResult()[0].GetConsumerResult()[0].GetErrorCode(), NPersQueue::NErrorCode::SCHEMA_ERROR);
        UNIT_ASSERT_EQUAL(result->Record.GetPartResult()[0].GetConsumerResult()[1].GetErrorCode(), NPersQueue::NErrorCode::SCHEMA_ERROR);
    }

    {
        THolder<TEvPersQueue::TEvStatus> statusEvent = MakeHolder<TEvPersQueue::TEvStatus>();
        statusEvent->Record.AddConsumers("consumer-0");
        statusEvent->Record.AddConsumers("nonex-consumer");
        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, statusEvent.Release(), 0, GetPipeConfigWithRetries());
        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvStatusResponse *result;
        result = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvStatusResponse>(handle);

        auto consumer0Result = std::find_if(
            result->Record.GetPartResult()[0].GetConsumerResult().begin(),
            result->Record.GetPartResult()[0].GetConsumerResult().end(),
            [](const auto& consumerResult) { return consumerResult.GetConsumer() == "consumer-0"; });

        UNIT_ASSERT_EQUAL(consumer0Result->GetErrorCode(), NPersQueue::NErrorCode::OK);
        UNIT_ASSERT_EQUAL(consumer0Result->GetCommitedOffset(), 1);

        auto nonexConsumerResult =  std::find_if(
            result->Record.GetPartResult()[0].GetConsumerResult().begin(),
            result->Record.GetPartResult()[0].GetConsumerResult().end(),
            [](const auto& consumerResult) { return consumerResult.GetConsumer() == "nonex-consumer"; });

        UNIT_ASSERT_EQUAL(nonexConsumerResult->GetErrorCode(), NPersQueue::NErrorCode::SCHEMA_ERROR);
    }

    PQBalancerPrepare(TOPIC_NAME, {{0,{tc.TabletId, 1}}}, ssId, tc, false, true);

}


void CheckEventSequence(TTestContext& tc, std::function<void()> scenario, std::deque<ui32> expectedEvents) {
    tc.Runtime->SetObserverFunc([&expectedEvents](TAutoPtr<IEventHandle>& ev) {
        if (!expectedEvents.empty() && ev->Type == expectedEvents.front()) {
            expectedEvents.pop_front();
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    });

    TDispatchOptions options;
    options.CustomFinalCondition = [&expectedEvents](){
        return expectedEvents.empty();
    };
    options.FinalEvents.emplace_back(TEvPQ::EvEnd);  // dummy event to prevent early return from DispatchEvents

    scenario();

    UNIT_ASSERT(tc.Runtime->DispatchEvents(options));
    UNIT_ASSERT(expectedEvents.empty());
}

Y_UNIT_TEST(TestTabletRestoreEventsOrder) {
    TTestContext tc;
    TFinalizer finalizer(tc);
    tc.Prepare();

    // Scenario 1: expect EvTabletActive after empty tablet reboot
    CheckEventSequence(tc, /*scenario=*/[&tc]() {
        ForwardToTablet(*tc.Runtime, tc.TabletId, tc.Edge, new TEvents::TEvPoisonPill());
    }, /*expectedEvents=*/{
        TEvTablet::TEvRestored::EventType,
        TEvTablet::TEvTabletActive::EventType,
    });

    // Scenario 2: expect EvTabletActive only after partitions init complete
    CheckEventSequence(tc, /*scenario=*/[&tc]() {
        PQTabletPrepare({}, {{"aaa", true}}, tc);
        ForwardToTablet(*tc.Runtime, tc.TabletId, tc.Edge, new TEvents::TEvPoisonPill());
    }, /*expectedEvents=*/{
        TEvTablet::TEvRestored::EventType,
        TEvPQ::TEvInitComplete::EventType,
        TEvPQ::TEvInitComplete::EventType,
        TEvTablet::TEvTabletActive::EventType,
    });
}

Y_UNIT_TEST(TestReadAndDeleteConsumer) {
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(2000);
        tc.Runtime->SetScheduledEventFilter(&tc.ImmediateLogFlushAndRequestTimeoutFilter);

        TVector<std::pair<ui64, TString>> data;
        TString msg;
        msg.resize(102400, 'a');
        for (ui64 i = 1; i <= 1000; ++i) {
            data.emplace_back(i, msg);
        }

        static ui32 pqConfigVersion = 1'000;

        PQTabletPrepare({.maxCountInPartition=100, .deleteTime=TDuration::Days(2).Seconds(), .partitions=1, .specVersion=pqConfigVersion++},
                        {{"user1", true}, {"user2", true}}, tc);
        CmdWrite(0, "sourceid1", data, tc, false, {}, true);

        // Reset tablet cache
        PQTabletRestart(tc);

        TAutoPtr<IEventHandle> handle;
        TEvPersQueue::TEvResponse* readResult = nullptr;
        THolder<TEvPersQueue::TEvRequest> readRequest;
        TEvPersQueue::TEvUpdateConfigResponse* consumerDeleteResult = nullptr;
        THolder<TEvPersQueue::TEvUpdateConfig> consumerDeleteRequest;

        // Read request
        {
            readRequest.Reset(new TEvPersQueue::TEvRequest);
            auto req = readRequest->Record.MutablePartitionRequest();
            req->SetPartition(0);
            auto read = req->MutableCmdRead();
            read->SetOffset(1);
            read->SetClientId("user1");
            read->SetCount(1);
            read->SetBytes(1'000'000);
            read->SetTimeoutMs(5000);
        }

        // Consumer delete request
        {
            consumerDeleteRequest.Reset(new TEvPersQueue::TEvUpdateConfig());
            consumerDeleteRequest->MutableRecord()->SetTxId(42);
            auto& cfg = *consumerDeleteRequest->MutableRecord()->MutableTabletConfig();
            cfg.SetVersion(pqConfigVersion++);
            cfg.AddPartitionIds(0);
            cfg.AddPartitions()->SetPartitionId(0);
            cfg.SetLocalDC(true);
            cfg.SetTopic("topic");
            auto& cons = *cfg.AddConsumers();
            cons.SetName("user2");
            cons.SetImportant(true);
        }

        TActorId edge = tc.Runtime->AllocateEdgeActor();

        // Delete consumer during read request
        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, readRequest.Release(), 0, GetPipeConfigWithRetries());

        // Intercept TEvPQ::TEvBlobResponse event
        std::vector<TEvPQ::TEvBlobResponse::TPtr> capturedBlobResponses;
        auto captureBlobResponsesObserver = tc.Runtime->AddObserver<TEvPQ::TEvBlobResponse>([&](TEvPQ::TEvBlobResponse::TPtr& ev) {
            capturedBlobResponses.emplace_back().Swap(ev);
        });

        // Delete consumer while read request is still in progress
        tc.Runtime->SendToPipe(tc.TabletId, edge, consumerDeleteRequest.Release(), 0, GetPipeConfigWithRetries());
        consumerDeleteResult = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvUpdateConfigResponse>(handle);
        {
            //Cerr << "Got consumer delete response: " << consumerDeleteResult->Record << Endl;
            UNIT_ASSERT(consumerDeleteResult->Record.HasStatus());
            UNIT_ASSERT_VALUES_EQUAL((int)consumerDeleteResult->Record.GetStatus(), (int)NKikimrPQ::EStatus::OK);
        }

        // Resend intercepted blob responses and wait for read result
        captureBlobResponsesObserver.Remove();
        for (auto& ev : capturedBlobResponses) {
            tc.Runtime->Send(ev.Release(), 0, true);
        }

        readResult = tc.Runtime->GrabEdgeEvent<TEvPersQueue::TEvResponse>(handle);
        {
            //Cerr << "Got read response: " << readResult->Record << Endl;
            UNIT_ASSERT(readResult->Record.HasStatus());
            UNIT_ASSERT_EQUAL(readResult->Record.GetErrorCode(), NPersQueue::NErrorCode::BAD_REQUEST);
            UNIT_ASSERT_STRING_CONTAINS_C(readResult->Record.GetErrorReason(), "Consumer user1 is gone from partition", readResult->Record.Utf8DebugString());
        }
    });
}

Y_UNIT_TEST(PQ_Tablet_Removes_Blobs_Asynchronously)
{
    const TString firstMessageKey = "d0000000000_00000000000000000000_00000_0000000001_00000|";

    TTestContext tc;
    TFinalizer finalizer(tc);
    tc.EnableDetailedPQLog = true;
    tc.Prepare();

    tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

    bool needDropCmdDeleteFirstMessage = true;
    bool foundCmdDeleteFirstMessage = false;

    PQTabletPrepare({.partitions = 1}, {}, tc);

    auto observe = [&](TAutoPtr<IEventHandle>& ev) {
        if (auto* event = ev->CastAsLocal<TEvKeyValue::TEvRequest>()) {
            foundCmdDeleteFirstMessage = false;
            const auto& record = event->Record;
            for (size_t i = 0; i < record.CmdDeleteRangeSize(); ++i) {
                const auto& cmd = record.GetCmdDeleteRange(i);
                const auto& range = cmd.GetRange();
                if (range.GetFrom() == firstMessageKey) {
                    foundCmdDeleteFirstMessage = true;
                    return needDropCmdDeleteFirstMessage ?
                        TTestActorRuntimeBase::EEventAction::DROP : TTestActorRuntimeBase::EEventAction::PROCESS;
                }
            }
        }
        return TTestActorRuntimeBase::EEventAction::PROCESS;
    };
    tc.Runtime->SetObserverFunc(observe);

    Cerr << ">>> write #1" << Endl;

    TVector<std::pair<ui64, TString>> data;
    data.resize(1);

    data[0].first = 1;
    data[0].second = TString(1_KB, 'x');
    CmdWrite(0, "sourceid1", data, tc, false, {}, true, "", -1, 0);

    Cerr << ">>> write #2" << Endl;

    ++data[0].first;
    data[0].second = TString(1_MB, 'x');
    CmdWrite(0, "sourceid1", data, tc, false, {}, true, "", -1, 1);

    Cerr << ">>> wait for CmdDeleteRange" << Endl;

    TDispatchOptions options;
    options.CustomFinalCondition = [&] { return foundCmdDeleteFirstMessage; };
    tc.Runtime->DispatchEvents(options);

    auto keys = GetTabletKeys(tc);
    UNIT_ASSERT_C(keys.contains(firstMessageKey),
                  "not found key '" << firstMessageKey << "'");

    needDropCmdDeleteFirstMessage = false;

    Cerr << ">>> restart" << Endl;

    PQTabletRestart(tc);

    Cerr << ">>> write #3" << Endl;

    ++data[0].first;
    data[0].second = TString(1_KB, 'x');
    CmdWrite(0, "sourceid1", data, tc, false, {}, true, "", -1, 2);

    Cerr << ">>> write #1" << Endl;

    keys = GetTabletKeys(tc);
    UNIT_ASSERT_C(!keys.contains(firstMessageKey),
                  "the PQ tablet did not delete the '" << firstMessageKey << "' key during startup");
}

Y_UNIT_TEST(The_Value_Of_CreationUnixTime_Must_Not_Decrease)
{
    auto simulateSleep = [](TDuration d, TTestContext& tc) {
        tc.Runtime->AdvanceCurrentTime(d);
        tc.Runtime->SimulateSleep(TDuration::MilliSeconds(1));
    };

    auto writeMessages = [&](ui64 begin, ui64 end, size_t size, TTestContext& tc) {
        for (ui64 offset = begin; offset < end; ++offset) {
            TVector<std::pair<ui64, TString>> data;
            data.emplace_back(offset, TString(size, 'x'));

            CmdWrite(0, "sourceId", data, tc, false, {}, true, "", -1, offset);

            simulateSleep(TDuration::MilliSeconds(1234), tc);
        }
    };

    TTestContext tc;
    TFinalizer finalizer(tc);
    tc.Prepare();

    // Turn off the asynchronous compactor
    tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(300);
    tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsSize(50_MB);

    // Create a topic with a single batch. Blobs will not be deleted by retention
    PQTabletPrepare({.partitions = 1, .storageLimitBytes = 50_MB}, {}, tc);

    // Write multiple messages so that three zones appear
    writeMessages(1, 20, 1_MB, tc);
    writeMessages(20, 25, 40_KB, tc);

    // The asynchronous compactor will start working after restarting the tablet
    tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(300);
    tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsSize(8_MB);

    PQTabletRestart(tc);

    // Let the asynchronous compactor work
    Sleep(TDuration::Seconds(5));

    // We can read any of the written messages
    for (i32 i = 1; i < 25; ++i) {
        CmdRead(0, i, 1, Max<i32>(), 1, false, tc, {i});
    }

    // The list of keys in the PQ tablet
    TVector<TString> keys;

    for (auto& key : GetTabletKeys(tc)) {
        keys.push_back(std::move(key));
    }
    std::sort(keys.begin(), keys.end());

    // The value of `CreationUnixTime` must not decrease
    ui64 currentCreationUnixTime = 0;
    for (const auto& key : keys) {
        Y_ABORT_UNLESS(!key.empty());
        if (key.front() != TKeyPrefix::TypeData) {
            continue;
        }

        auto request = MakeHolder<TEvKeyValue::TEvRequest>();
        auto read = request->Record.AddCmdReadRange();
        auto range = read->MutableRange();
        range->SetFrom(key);
        range->SetIncludeFrom(true);
        range->SetTo(key);
        range->SetIncludeTo(true);

        tc.Runtime->SendToPipe(tc.TabletId, tc.Edge, request.Release(), 0, GetPipeConfigWithRetries());
        TAutoPtr<IEventHandle> handle;
        auto* result = tc.Runtime->GrabEdgeEvent<TEvKeyValue::TEvResponse>(handle);

        UNIT_ASSERT_VALUES_EQUAL(result->Record.ReadRangeResultSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(result->Record.GetReadRangeResult(0).PairSize(), 1);
        UNIT_ASSERT_LE_C(currentCreationUnixTime, result->Record.GetReadRangeResult(0).GetPair(0).GetCreationUnixTime(),
                       "key=" << key <<
                       ", currentCreationUnixTime=" << currentCreationUnixTime <<
                       ", result.CreationUnixTime=" << result->Record.GetReadRangeResult(0).GetPair(0).GetCreationUnixTime());

        currentCreationUnixTime = result->Record.GetReadRangeResult(0).GetPair(0).GetCreationUnixTime();
    }
}

Y_UNIT_TEST(PQ_Tablet_Does_Not_Remove_The_Blob_Until_The_Reading_Is_Complete)
{
    // The test verifies that the block is not deleted until the reading is finished. We write
    // down several large messages in the topic. We start reading and hold it. While we are reading
    // the message, we add a few more messages to the topic. The retention is triggered. We make
    // sure that the blobs are deleted only when the reading is finished

    auto writeMsgs = [](const TString& sourceId, size_t begin, size_t end, size_t size, TTestContext& tc) {
        for (size_t i = begin; i < end; ++i) {
            TVector<std::pair<ui64, TString>> data;
            data.emplace_back(i + 1, TString(size, 'x'));
            CmdWrite(0, sourceId, data, tc, false, {}, true, "", -1, i);
        }
    };

    TTestContext tc;
    TFinalizer finalizer(tc);
    tc.EnableDetailedPQLog = true;
    tc.Prepare();

    tc.Runtime->GetAppData(0).PQConfig.MutableCompactionConfig()->SetBlobsCount(0);

    const TString sessionId = "session1";
    const TString user = "user1";

    // Creating a topic with the retention settings
    PQTabletPrepare({.partitions = 1, .storageLimitBytes = 50_MB}, {{user, false}}, tc);

    // We record several messages. If you record another one, the first one will be deleted
    writeMsgs("sourceid1", 0, 7, 7_MB, tc);

    // Making sure that the blobs have not been deleted yet
    auto keys = GetTabletKeys(tc);

    UNIT_ASSERT(keys.contains("d0000000000_00000000000000000001_00000_0000000001_00014"));
    UNIT_ASSERT(keys.contains("d0000000000_00000000000000000002_00000_0000000001_00014"));
    UNIT_ASSERT(keys.contains("d0000000000_00000000000000000003_00000_0000000001_00014"));
    UNIT_ASSERT(keys.contains("d0000000000_00000000000000000004_00000_0000000001_00014"));

    // We are reading from topic 2 messages from offset 2
    TPQCmdSettings sessionSettings{0, user, sessionId};
    sessionSettings.PartitionSessionId = 1;
    sessionSettings.KeepPipe = true;

    TPQCmdReadSettings readSettings{sessionId, 0, 2, 2, 16_MB, 2, false, {2, 3}, 0, 0, user};
    readSettings.PartitionSessionId = 1;
    readSettings.User = user;
    readSettings.Pipe = CmdCreateSession(sessionSettings, tc);

    // The messages are large and will be cached. We intercept the response from the
    // cache and hold it. The reading will not end until the response from the cache arrives.
    // Except for the answers for the compaction.
    TAutoPtr<IEventHandle> blobResponseEvent;
    auto observe = [&](TAutoPtr<IEventHandle>& ev) {
        if (auto* event = ev->CastAsLocal<TEvPQ::TEvBlobResponse>()) {
            if (event->GetCookie() == 0) { // ERequestCookie::ReadBlobsForCompaction
                return TTestActorRuntimeBase::EEventAction::PROCESS;
            }
            blobResponseEvent = ev;
            return TTestActorRuntimeBase::EEventAction::DROP;
        }
        return TTestActorRuntimeBase::EEventAction::PROCESS;
    };
    tc.Runtime->SetObserverFunc(observe);

    BeginCmdRead(readSettings, tc);

    // Waiting for a response from the cache
    TDispatchOptions options;
    options.CustomFinalCondition = [&] { return !!blobResponseEvent; };
    tc.Runtime->DispatchEvents(options);

    // We're recording a few more big messages. The retention will be triggered and the first messages
    // will have to be deleted
    writeMsgs("sourceid1", 7, 14, 7_MB, tc);

    keys = GetTabletKeys(tc);

    // We make sure that the blobs with messages on offsets 2 and 3 have not been deleted
    UNIT_ASSERT(!keys.contains("d0000000000_00000000000000000001_00000_0000000001_00014"));
    UNIT_ASSERT(keys.contains("d0000000000_00000000000000000002_00000_0000000001_00014"));
    UNIT_ASSERT(keys.contains("d0000000000_00000000000000000003_00000_0000000001_00014"));
    UNIT_ASSERT(!keys.contains("d0000000000_00000000000000000004_00000_0000000001_00014"));

    tc.Runtime->Send(blobResponseEvent);

    // The reading ended without error
    UNIT_ASSERT_C(EndCmdRead(readSettings, tc), "CmdRead failed with an error");

    // We are writing a short message to launch the planned blob removals
    writeMsgs("sourceid1", 14, 15, 100_KB, tc);

    keys = GetTabletKeys(tc);

    // Making sure that the blobs for messages with offsets 2 and 3 are removed
    UNIT_ASSERT(!keys.contains("d0000000000_00000000000000000002_00000_0000000001_00014"));
    UNIT_ASSERT(!keys.contains("d0000000000_00000000000000000003_00000_0000000001_00014"));
}

} // Y_UNIT_TEST_SUITE(TPQTest)
} // namespace NKikimr::NPQ
