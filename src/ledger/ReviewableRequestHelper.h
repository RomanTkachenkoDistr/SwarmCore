#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryHelper.h"
#include "ledger/LedgerManager.h"
#include <functional>
#include <unordered_map>
#include "ReviewableRequestFrame.h"

namespace soci
{
    class session;
}

namespace stellar
{
    class StatementContext;

    class ReviewableRequestHelper : public EntryHelper {
    public:
        ReviewableRequestHelper(ReviewableRequestHelper const&) = delete;
        ReviewableRequestHelper& operator= (ReviewableRequestHelper const&) = delete;

        static ReviewableRequestHelper *Instance() {
            static ReviewableRequestHelper singleton;
            return&singleton;
        }

        void dropAll(Database& db) override;
        void storeAdd(LedgerDelta& delta, Database& db, LedgerEntry const& entry) override;
        void storeChange(LedgerDelta& delta, Database& db, LedgerEntry const& entry) override;
        void storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key) override;
        bool exists(Database& db, LedgerKey const& key) override;
        LedgerKey getLedgerKey(LedgerEntry const& from) override;
        EntryFrame::pointer storeLoad(LedgerKey const& key, Database& db) override;
        EntryFrame::pointer fromXDR(LedgerEntry const& from) override;
        uint64_t countObjects(soci::session& sess) override;

        void loadRequests(StatementContext & prep, std::function<void(LedgerEntry const&)> requestsProcessor);

        ReviewableRequestFrame::pointer loadRequest(uint64 requestID, Database& db, LedgerDelta* delta = nullptr);
        ReviewableRequestFrame::pointer loadRequest(uint64 requestID, AccountID requestor, Database& db,
                                                    LedgerDelta* delta = nullptr);
        ReviewableRequestFrame::pointer loadRequest(uint64 requestID, AccountID requestor,
                                                    ReviewableRequestType requestType,
                                                    Database& db, LedgerDelta* delta = nullptr);

        std::vector<ReviewableRequestFrame::pointer> loadRequests(AccountID const& requestor, ReviewableRequestType requestType,
            Database& db);

        bool exists(Database & db, AccountID const & requestor, stellar::string64 reference, uint64_t requestID = 0);
        bool isReferenceExist(Database & db, AccountID const & requestor, string64 reference, uint64_t requestID = 0);

    private:
        ReviewableRequestHelper() { ; }
        ~ReviewableRequestHelper() { ; }

        void storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert, LedgerEntry const& entry);
    };
}