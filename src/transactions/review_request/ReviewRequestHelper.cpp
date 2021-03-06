#include "ReviewRequestHelper.h"
#include "ReviewRequestOpFrame.h"
#include "ledger/AccountHelper.h"


namespace stellar {

ReviewRequestHelper::ReviewRequestHelper(Application &app, LedgerManager &ledgerManager, LedgerDelta &delta,
                                         ReviewableRequestFrame::pointer reviewableRequest)
        :mApp(app), mLedgerManager(ledgerManager), mDelta(delta), mRequest(reviewableRequest)
{
}

ReviewRequestResultCode ReviewRequestHelper::tryApproveRequest(TransactionFrame &parentTx, Application &app,
                                                               LedgerManager &ledgerManager, LedgerDelta &delta,
                                                               ReviewableRequestFrame::pointer reviewableRequest)
{
    Database& db = ledgerManager.getDatabase();
    // shield outer scope of any side effects by using
    // a sql transaction for ledger state and LedgerDelta
    soci::transaction reviewRequestTx(db.getSession());
    LedgerDelta reviewRequestDelta(delta);

    auto helper = ReviewRequestHelper(app, ledgerManager, reviewRequestDelta, reviewableRequest);
    auto resultCode = helper.tryApproveRequest(parentTx);
    if (resultCode != ReviewRequestResultCode::SUCCESS)
        return resultCode;

    reviewRequestTx.commit();
    reviewRequestDelta.commit();

    return resultCode;
}

ReviewRequestResultCode ReviewRequestHelper::tryApproveRequest(TransactionFrame &parentTx)
{
    auto result = tryReviewRequest(parentTx);
    bool isApplied = result.first;
    ReviewRequestResult reviewRequestResult = result.second;
    if (!isApplied)
    {
        return reviewRequestResult.code();
    }

    auto resultCode = reviewRequestResult.code();
    if (resultCode != ReviewRequestResultCode::SUCCESS) {
        CLOG(ERROR, Logging::OPERATION_LOGGER) << "Unexpected state: doApply returned true, but result code is not success: "
                                               << xdr::xdr_to_string(mRequest->getRequestEntry());
        throw std::runtime_error("Unexpected state: doApply returned true, but result code is not success.");
    }

    return resultCode;
}

std::pair<bool, ReviewRequestResult> ReviewRequestHelper::tryReviewRequest(TransactionFrame &parentTx)
{
    Operation op;
    auto reviewer = mRequest->getReviewer();
    op.sourceAccount = xdr::pointer<stellar::AccountID>(new AccountID(reviewer));
    op.body.type(OperationType::REVIEW_REQUEST);
    ReviewRequestOp& reviewRequestOp = op.body.reviewRequestOp();
    reviewRequestOp.action = ReviewRequestOpAction::APPROVE;
    reviewRequestOp.requestHash = mRequest->getHash();
    reviewRequestOp.requestID = mRequest->getRequestID();
    reviewRequestOp.requestDetails.requestType(mRequest->getRequestType());

    OperationResult opRes;
    opRes.code(OperationResultCode::opINNER);
    opRes.tr().type(OperationType::REVIEW_REQUEST);
    Database& db = mLedgerManager.getDatabase();
	auto accountHelper = AccountHelper::Instance();
    auto reviewerFrame = accountHelper->loadAccount(reviewer, db);
    if (!reviewerFrame) {
        CLOG(ERROR, Logging::OPERATION_LOGGER) << "Unexpected state: expected review to exist for request: "
                                               << xdr::xdr_to_string(mRequest->getRequestEntry());
        throw std::runtime_error("Unexpected state expected reviewer to exist");
    }

    auto reviewRequestOpFrame = ReviewRequestOpFrame::makeHelper(op, opRes, parentTx);
    reviewRequestOpFrame->setSourceAccountPtr(reviewerFrame);
    bool isApplied = reviewRequestOpFrame->doCheckValid(mApp) && reviewRequestOpFrame->doApply(mApp, mDelta, mLedgerManager);
    if (reviewRequestOpFrame->getResultCode() != OperationResultCode::opINNER)
    {
        throw std::runtime_error("Unexpected error code from review request operation");
    }

    return std::pair<bool, ReviewRequestResult>(isApplied, reviewRequestOpFrame->getResult().tr().reviewRequestResult());
}



}


