// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <transactions/ManageAssetPairOpFrame.h>
#include "ReviewSaleCreationRequestOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/AssetHelper.h"
#include "main/Application.h"
#include "xdrpp/printer.h"
#include "ledger/SaleHelper.h"
#include "ledger/AssetPairHelper.h"
#include "transactions/CreateSaleCreationRequestOpFrame.h"

namespace stellar
{
using namespace std;
using xdr::operator==;

bool ReviewSaleCreationRequestOpFrame::handleApprove(
    Application& app, LedgerDelta& delta, LedgerManager& ledgerManager,
    ReviewableRequestFrame::pointer request)
{
    if (request->getRequestType() != ReviewableRequestType::SALE)
    {
        CLOG(ERROR, Logging::OPERATION_LOGGER) <<
            "Unexpected request type. Expected SALE, but got " << xdr::
            xdr_traits<ReviewableRequestType>::
            enum_name(request->getRequestType());
        throw
            invalid_argument("Unexpected request type for review sale creation request");
    }

    auto& db = app.getDatabase();
    EntryHelperProvider::storeDeleteEntry(delta, db, request->getKey());

    auto& saleCreationRequest = request->getRequestEntry().body.saleCreationRequest();
    if (!CreateSaleCreationRequestOpFrame::areQuoteAssetsValid(db, saleCreationRequest.quoteAssets, saleCreationRequest.defaultQuoteAsset))
    {
        CLOG(ERROR, Logging::OPERATION_LOGGER) << "Unexpected state, quote asset does not exist: " << request->getRequestID();
        throw runtime_error("Quote asset does not exist");
    }

    auto baseAsset = AssetHelper::Instance()->loadAsset(saleCreationRequest.baseAsset, request->getRequestor(), db, &delta);
    if (!baseAsset)
    {
        innerResult().code(ReviewRequestResultCode::BASE_ASSET_DOES_NOT_EXISTS);
        return false;
    }

    // TODO: at current stage we do not allow to issue tokens before the sale. Must be fixed
    const uint64_t requiredBaseAssetForHardCap = baseAsset->getMaxIssuanceAmount();
    if (!baseAsset->lockIssuedAmount(requiredBaseAssetForHardCap))
    {
        CLOG(ERROR, Logging::OPERATION_LOGGER) << "Unexpected state, failed to lock issuance amount: " << request->getRequestID();
        innerResult().code(ReviewRequestResultCode::INSUFFICIENT_PREISSUED_FOR_HARD_CAP);
        return false;
    }

    AssetHelper::Instance()->storeChange(delta, db, baseAsset->mEntry);

    AccountManager accountManager(app, db, delta, ledgerManager);
    const auto balances = loadBalances(accountManager, request, saleCreationRequest);
    const auto saleFrame = SaleFrame::createNew(delta.getHeaderFrame().generateID(LedgerEntryType::SALE), baseAsset->getOwner(), saleCreationRequest,
        balances);
    SaleHelper::Instance()->storeAdd(delta, db, saleFrame->mEntry);
    createAssetPair(saleFrame, app, ledgerManager, delta);
    innerResult().code(ReviewRequestResultCode::SUCCESS);
    return true;
}

SourceDetails ReviewSaleCreationRequestOpFrame::getSourceAccountDetails(
    std::unordered_map<AccountID, CounterpartyDetails> counterpartiesDetails, int32_t ledgerVersion)
const
{
    auto allowedSigners = static_cast<int32_t>(SignerType::ASSET_MANAGER);

    auto newSignersVersion = static_cast<int32_t>(LedgerVersion::NEW_SIGNER_TYPES);
    if (ledgerVersion >= newSignersVersion)
    {
        allowedSigners = static_cast<int32_t>(SignerType::USER_ASSET_MANAGER);
    }

    return SourceDetails({AccountType::MASTER},
                         mSourceAccount->getHighThreshold(), allowedSigners);
}

void ReviewSaleCreationRequestOpFrame::createAssetPair(SaleFrame::pointer sale, Application &app,
                                                       LedgerManager &ledgerManager, LedgerDelta &delta) const
{
    for (const auto quoteAsset : sale->getSaleEntry().quoteAssets)
    {
        const auto assetPair = AssetPairHelper::Instance()->tryLoadAssetPairForAssets(sale->getBaseAsset(), quoteAsset.quoteAsset,
            ledgerManager.getDatabase());
        if (!!assetPair)
        {
            return;
        }

        //create new asset pair
        Operation op;
        op.body.type(OperationType::MANAGE_ASSET_PAIR);
        auto& manageAssetPair = op.body.manageAssetPairOp();
        manageAssetPair.action = ManageAssetPairAction::CREATE;
        manageAssetPair.base = sale->getBaseAsset();
        manageAssetPair.quote = quoteAsset.quoteAsset;
        manageAssetPair.physicalPrice = quoteAsset.price;

        OperationResult opRes;
        opRes.code(OperationResultCode::opINNER);
        opRes.tr().type(OperationType::MANAGE_ASSET_PAIR);
        ManageAssetPairOpFrame assetPairOpFrame(op, opRes, mParentTx);
        assetPairOpFrame.setSourceAccountPtr(mSourceAccount);
        const auto applied = assetPairOpFrame.doCheckValid(app) && assetPairOpFrame.doApply(app, delta, ledgerManager);
        if (!applied)
        {
            CLOG(ERROR, Logging::OPERATION_LOGGER) << "Unable to create asset pair for sale creation request: " << sale->getID();
            throw runtime_error("Unexpected state. Unable to create asset pair");
        }
    }
}

std::map<AssetCode, BalanceID> ReviewSaleCreationRequestOpFrame::loadBalances(
    AccountManager& accountManager, const ReviewableRequestFrame::pointer request, SaleCreationRequest const& saleCreationRequest)
{
    map<AssetCode, BalanceID> result;
    const auto baseBalanceID = accountManager.loadOrCreateBalanceForAsset(request->getRequestor(), saleCreationRequest.baseAsset);
    result.insert(make_pair(saleCreationRequest.baseAsset, baseBalanceID));
    for (auto quoteAsset : saleCreationRequest.quoteAssets)
    {
        const auto quoteBalanceID = accountManager.loadOrCreateBalanceForAsset(request->getRequestor(), quoteAsset.quoteAsset);
        result.insert(make_pair(quoteAsset.quoteAsset, quoteBalanceID));
    }
    return result;
}

ReviewSaleCreationRequestOpFrame::ReviewSaleCreationRequestOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx) :
                                                                           ReviewRequestOpFrame(op,
                                                                                                res,
                                                                                                parentTx)
{
}
}
