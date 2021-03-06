#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "ReviewRequestTestHelper.h"
#include "ledger/ReviewableRequestFrame.h"
#include "ledger/AssetFrame.h"
#include "ledger/BalanceFrame.h"

namespace stellar
{
namespace txtest
{

class TwoStepWithdrawReviewChecker : public ReviewChecker
{
protected:
    std::shared_ptr<WithdrawalRequest> withdrawalRequest;
    BalanceFrame::pointer balanceBeforeTx;
    BalanceFrame::pointer commissionBalanceBeforeTx;
    AssetFrame::pointer assetBeforeTx;
    StatisticsFrame::pointer statsBeforeTx;
public:
    TwoStepWithdrawReviewChecker(TestManager::pointer testManager, uint64_t requestID);
    void checkApprove(ReviewableRequestFrame::pointer) override;
    void checkPermanentReject(ReviewableRequestFrame::pointer) override;
};

class ReviewTwoStepWithdrawRequestHelper : public ReviewRequestHelper
{

public:
    ReviewTwoStepWithdrawRequestHelper(TestManager::pointer testManager);

    static std::string externalDetails;

    using ReviewRequestHelper::applyReviewRequestTx;
    ReviewRequestResult applyReviewRequestTx(Account& source,
                                             uint64_t requestID,
                                             Hash requestHash,
                                             ReviewableRequestType requestType,
                                             ReviewRequestOpAction action,
                                             std::string rejectReason,
                                             ReviewRequestResultCode
                                             expectedResult =
                                                 ReviewRequestResultCode::
                                                 SUCCESS) override;

    TransactionFramePtr createReviewRequestTx(Account& source,
        uint64_t requestID, Hash requestHash, ReviewableRequestType requestType,
        ReviewRequestOpAction action, std::string rejectReason) override;

};
}
}
