// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "ledger/AssetHelper.h"
#include "ledger/ReviewableRequestHelper.h"
#include "main/test.h"
#include "TxTests.h"
#include "util/Timer.h"
#include "crypto/SHA.h"
#include "test_helper/TestManager.h"
#include "test_helper/Account.h"
#include "test_helper/ManageAssetTestHelper.h"
#include "ledger/BalanceHelper.h"
#include "test_helper/CreateAccountTestHelper.h"
#include "test_helper/SaleRequestHelper.h"
#include "test_helper/IssuanceRequestHelper.h"
#include "test_helper/ManageBalanceTestHelper.h"
#include "test_helper/ParticipateInSaleTestHelper.h"
#include "transactions/dex/OfferManager.h"
#include "ledger/SaleHelper.h"
#include "test_helper/CheckSaleStateTestHelper.h"
#include "test_helper/ReviewAssetRequestHelper.h"
#include "test_helper/ReviewSaleRequestHelper.h"
#include "test/test_marshaler.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

void addNewParticipant(TestManager::pointer testManager, Account& root, const uint64_t saleID, const AssetCode baseAsset,
                       const AssetCode quoteAsset, const uint64_t quoteAssetAmount, const uint64_t price, const uint64_t fee)
{
    auto account = Account{ SecretKey::random(), 0 };
    CreateAccountTestHelper(testManager).applyCreateAccountTx(root, account.key.getPublicKey(), AccountType::NOT_VERIFIED);
    auto quoteBalance = BalanceHelper::Instance()->loadBalance(account.key.getPublicKey(), quoteAsset, testManager->getDB(), nullptr);
    REQUIRE(!!quoteBalance);
    IssuanceRequestHelper(testManager).applyCreateIssuanceRequest(root, quoteAsset, quoteAssetAmount, quoteBalance->getBalanceID(),
        SecretKey::random().getStrKeyPublic());
    auto accountID = account.key.getPublicKey();
    auto balanceCreationResult = ManageBalanceTestHelper(testManager).applyManageBalanceTx(account, accountID, baseAsset);
    const auto baseAssetAmount = bigDivide(quoteAssetAmount, ONE, price, ROUND_UP);
    auto manageOfferOp = OfferManager::buildManageOfferOp(balanceCreationResult.success().balanceID, quoteBalance->getBalanceID(),
        true, baseAssetAmount, price, 0, fee, saleID);
    ParticipateInSaleTestHelper(testManager).applyManageOffer(account, manageOfferOp);
}

TEST_CASE("Sale", "[tx][sale]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_POSTGRESQL);
    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();
    auto testManager = TestManager::make(app);

    Database& db = testManager->getDB();

    LedgerDelta& delta = testManager->getLedgerDelta();

    auto root = Account{ getRoot(), Salt(0) };

    const AssetCode quoteAsset = "USD";
    auto assetTestHelper = ManageAssetTestHelper(testManager);
    auto assetCreationRequest = assetTestHelper.createAssetCreationRequest(quoteAsset, root.key.getPublicKey(), "{}", INT64_MAX, uint32_t(AssetPolicy::BASE_ASSET));
    assetTestHelper.applyManageAssetTx(root, 0, assetCreationRequest);

    auto syndicate = Account{ SecretKey::random(), 0 };
    auto syndicatePubKey = syndicate.key.getPublicKey();
    auto offerFeeFrame = FeeFrame::create(FeeType::OFFER_FEE, 0,
                                          int64_t(0.2 * ONE), quoteAsset,
                                          &syndicatePubKey);
    auto offerFee = offerFeeFrame->getFee();

    CreateAccountTestHelper(testManager).applyCreateAccountTx(root, syndicatePubKey, AccountType::SYNDICATE);
    const AssetCode baseAsset = "BTC";
    const uint64_t maxIssuanceAmount = 2000 * ONE;
    const uint64_t preIssuedAmount = 1000 * ONE;
    assetCreationRequest = assetTestHelper.createAssetCreationRequest(baseAsset, syndicate.key.getPublicKey(), "{}",
                                                                      maxIssuanceAmount,0, preIssuedAmount);
    assetTestHelper.createApproveRequest(root, syndicate, assetCreationRequest);
    auto saleRequestHelper = SaleRequestHelper(testManager);
    const auto currentTime = testManager->getLedgerManager().getCloseTime();
    const uint64_t price = 2 * ONE;
    const uint64_t hardCap = static_cast<const uint64_t>(bigDivide(preIssuedAmount, price, ONE, ROUND_DOWN));
    const uint64_t softCap = hardCap / 2;
    auto saleRequest = saleRequestHelper.createSaleRequest(baseAsset, quoteAsset, currentTime,
                                                           currentTime + 1000, price, softCap, hardCap, "{}");
    SECTION("Happy path")
    {
        saleRequestHelper.createApprovedSale(root, syndicate, saleRequest);
        auto accountTestHelper = CreateAccountTestHelper(testManager);
        IssuanceRequestHelper(testManager).authorizePreIssuedAmount(root, root.key, quoteAsset, hardCap, root);
        auto sales = SaleHelper::Instance()->loadSales(baseAsset, quoteAsset, testManager->getDB());
        REQUIRE(sales.size() == 1);
        const auto saleID = sales[0]->getID();
        SECTION("Reached hard cap")
        {
            const int numberOfParticipants = 10;
            for (auto i = 0; i < numberOfParticipants; i++)
            {
                const auto quoteAssetAmount = hardCap / numberOfParticipants;
                addNewParticipant(testManager, root, saleID, baseAsset, quoteAsset, quoteAssetAmount, price, 0);
                if (i < numberOfParticipants - 1)
                {
                    CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root, CheckSaleStateResultCode::NO_SALES_FOUND);
                }
            }

            CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root);
        }
        SECTION("Canceled")
        {
            const int numberOfParticipants = 10;
            for (auto i = 0; i < numberOfParticipants - 1; i++)
            {

                addNewParticipant(testManager, root, saleID, baseAsset, quoteAsset, softCap / numberOfParticipants, price, 0);
                CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root, CheckSaleStateResultCode::NO_SALES_FOUND);
            }
            // hardcap is not reached, so no sale to close
            CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root, CheckSaleStateResultCode::NO_SALES_FOUND);
            // TODO close ledger after end time of the sale and check sale state
        }
    }
    SECTION("Create SaleCreationRequest")
    {
        SECTION("Try to create sale with zero price")
        {
            saleRequest.price = 0;
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::INVALID_PRICE);
        }
        SECTION("Try to create sale that's ends before begins")
        {
            saleRequest.endTime = saleRequest.startTime;
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::START_END_INVALID);
        }
        SECTION("Try to create sale with hardCap less than softCap")
        {
            saleRequest.hardCap = saleRequest.softCap - 1;
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::INVALID_CAP);
        }
        SECTION("Try to update not existent request")
        {
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest);
            auto requestID = requestCreationResult.success().requestID;
            auto requestFrame = ReviewableRequestHelper::Instance()->loadRequest(requestID, db);
            ReviewableRequestHelper::Instance()->storeDelete(delta, db, requestFrame->getKey());
            requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, requestID, saleRequest,
            CreateSaleCreationRequestResultCode::REQUEST_NOT_FOUND);
        }
        SECTION("Base asset not found")
        {
            auto baseAssetFrame = AssetHelper::Instance()->loadAsset(baseAsset, db);
            AssetHelper::Instance()->storeDelete(delta, db, baseAssetFrame->getKey());
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::BASE_ASSET_OR_ASSET_REQUEST_NOT_FOUND);
        }
        SECTION("Quote asset not found")
        {
            auto quoteAssetFrame = AssetHelper::Instance()->loadAsset(quoteAsset, db);
            AssetHelper::Instance()->storeDelete(delta, db, quoteAssetFrame->getKey());
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::QUOTE_ASSET_NOT_FOUND);
        }
        SECTION("Insufficient max issuance")
        {
            saleRequest.hardCap = UINT64_MAX;
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::INSUFFICIENT_MAX_ISSUANCE);
        }
        SECTION("Insufficient preissued")
        {
            saleRequest.hardCap = static_cast<uint64>(maxIssuanceAmount + preIssuedAmount);
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::INSUFFICIENT_PREISSUED);
        }
        SECTION("Try to create request that's already exists")
        {
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest);
            requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::REQUEST_OR_SALE_ALREADY_EXISTS);
        }
    }

    auto saleReviewer = ReviewSaleRequestHelper(testManager);

    SECTION("Review SaleCreationRequest")
    {
        auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest);
        auto requestID = requestCreationResult.success().requestID;

        SECTION("Quote asset does not exist")
        {
            auto quoteAssetFrame = AssetHelper::Instance()->loadAsset(quoteAsset, db);
            AssetHelper::Instance()->storeDelete(delta, db, quoteAssetFrame->getKey());
            saleReviewer.applyReviewRequestTx(root, requestID, ReviewRequestOpAction::APPROVE, "",
            ReviewRequestResultCode::QUOTE_ASSET_DOES_NOT_EXISTS);
        }
        SECTION("Base asset does not exist")
        {
            auto baseAssetFrame = AssetHelper::Instance()->loadAsset(baseAsset, db);
            AssetHelper::Instance()->storeDelete(delta, db, baseAssetFrame->getKey());
            saleReviewer.applyReviewRequestTx(root, requestID, ReviewRequestOpAction::APPROVE, "",
            ReviewRequestResultCode::BASE_ASSET_DOES_NOT_EXISTS);
        }
        SECTION("Max issuance or preissued amount is less then hard cap")
        {
            const AssetCode asset = "GSC";

            // Create asset creation request with max issuance 2000 * ONE
            assetCreationRequest = assetTestHelper.createAssetCreationRequest(asset, syndicatePubKey, "{}",
                                                                              maxIssuanceAmount, 0,
                                                                              preIssuedAmount);
            auto assetRequestCreationResult = assetTestHelper.applyManageAssetTx(syndicate, 0, assetCreationRequest);
            auto assetRequestID = assetRequestCreationResult.success().requestID;
            auto assetRequestFrame = ReviewableRequestHelper::Instance()->loadRequest(assetRequestID, db);

            // Create sale creation request with hardCap 1000 * ONE
            saleRequest = saleRequestHelper.createSaleRequest(asset, quoteAsset, currentTime, currentTime + 1000,
                                                              price, softCap, hardCap, "{}");
            auto saleRequestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest);
            auto saleRequestID = saleRequestCreationResult.success().requestID;

            SECTION("Hard cap will exceed max issuance")
            {
                // Update asset creation request with max issuance = 900 * ONE and pre issued = 500 * ONE
                assetCreationRequest.createAsset().maxIssuanceAmount = 900 * ONE;
                assetCreationRequest.createAsset().initialPreissuedAmount = 500 * ONE;
                assetRequestCreationResult = assetTestHelper.applyManageAssetTx(syndicate, assetRequestID,
                                                                                assetCreationRequest);
                assetRequestFrame = ReviewableRequestHelper::Instance()->loadRequest(assetRequestID, db);

                // Approve asset creation request
                auto assetReviewer = ReviewAssetRequestHelper(testManager);
                assetReviewer.applyReviewRequestTx(root, assetRequestID, assetRequestFrame->getHash(),
                                                   assetRequestFrame->getRequestType(),
                                                   ReviewRequestOpAction::APPROVE, "");

                //Try to approve sale creation request
                saleReviewer.applyReviewRequestTx(root, saleRequestID, ReviewRequestOpAction::APPROVE, "",
                                                  ReviewRequestResultCode::HARD_CAP_WILL_EXCEED_MAX_ISSUANCE);
            }
            SECTION("Preissued amount of base asset is not enough for hard cap")
            {
                // Update asset creation request with preissued = 500 * ONE
                assetCreationRequest.createAsset().initialPreissuedAmount = 500 * ONE;
                assetRequestCreationResult = assetTestHelper.applyManageAssetTx(syndicate, assetRequestID,
                                                                                assetCreationRequest);
                assetRequestFrame = ReviewableRequestHelper::Instance()->loadRequest(assetRequestID, db);

                // Approve asset creation request
                auto assetReviewer = ReviewAssetRequestHelper(testManager);
                assetReviewer.applyReviewRequestTx(root, assetRequestID, assetRequestFrame->getHash(),
                                                   assetRequestFrame->getRequestType(),
                                                   ReviewRequestOpAction::APPROVE, "");

                //Try to approve sale creation request
                saleReviewer.applyReviewRequestTx(root, saleRequestID, ReviewRequestOpAction::APPROVE, "",
                                                  ReviewRequestResultCode::INSUFFICIENT_PREISSUED_FOR_HARD_CAP);
            }
        }
    }
    SECTION("Try to steal token by creating sale for stranger asset")
    {
        auto ownerSyndicate = Account{ SecretKey::random(), 0 };
        auto ownerSyndicatePubKey = ownerSyndicate.key.getPublicKey();
        CreateAccountTestHelper(testManager).applyCreateAccountTx(root, ownerSyndicatePubKey, AccountType::SYNDICATE);

        auto thiefSyndicate = Account{ SecretKey::random(), 0 };
        auto thiefSyndicatePubKey = thiefSyndicate.key.getPublicKey();
        CreateAccountTestHelper(testManager).applyCreateAccountTx(root, thiefSyndicatePubKey, AccountType::SYNDICATE);

        const AssetCode asset = "GSC";
        const auto assetMaxIssuanceAmount = 2000 * ONE;
        const auto assetPreIssuedAmount = 1000 * ONE;

        // Owner creates asset creation request
        assetCreationRequest = assetTestHelper.createAssetCreationRequest(asset, ownerSyndicatePubKey, "{}",
                                                                          assetMaxIssuanceAmount,0,
                                                                          assetPreIssuedAmount);
        auto ownerRequestCreationResult = assetTestHelper.applyManageAssetTx(ownerSyndicate, 0, assetCreationRequest);
        auto ownerAssetRequestID = ownerRequestCreationResult.success().requestID;
        auto ownerRequestFrame = ReviewableRequestHelper::Instance()->loadRequest(ownerAssetRequestID, db);

        // Thief creates asset creation request
        assetCreationRequest = assetTestHelper.createAssetCreationRequest(asset, thiefSyndicatePubKey, "{}",
                                                                          assetMaxIssuanceAmount,0,
                                                                          assetPreIssuedAmount);
        auto thiefRequestCreationResult = assetTestHelper.applyManageAssetTx(thiefSyndicate, 0, assetCreationRequest);
        auto thiefAssetRequestID = thiefRequestCreationResult.success().requestID;
        auto thiefRequestFrame = ReviewableRequestHelper::Instance()->loadRequest(thiefAssetRequestID, db);

        // Thief creates sale creation request
        auto thiefSaleRequest = SaleRequestHelper::createSaleRequest(asset, quoteAsset, currentTime, currentTime + 1000,
                                                                     price, softCap, hardCap, "{}");

        auto thiefSaleRequestCreationResult = saleRequestHelper.applyCreateSaleRequest(thiefSyndicate, 0, thiefSaleRequest);
        auto thiefSaleRequestID = thiefSaleRequestCreationResult.success().requestID;

        auto assetReviewer = ReviewAssetRequestHelper(testManager);

        // Reviewer approves owner's asset creation request
        assetReviewer.applyReviewRequestTx(root, ownerAssetRequestID, ownerRequestFrame->getHash(),
                                           ownerRequestFrame->getRequestType(),
                                           ReviewRequestOpAction::APPROVE, "");

        // Reviewer rejects thief's asset creation request
        assetReviewer.applyReviewRequestTx(root, thiefAssetRequestID, thiefRequestFrame->getHash(),
                                           thiefRequestFrame->getRequestType(),
                                           ReviewRequestOpAction::PERMANENT_REJECT, "Because");

        // Reviewer approves thief's sale creation request
        saleReviewer.applyReviewRequestTx(root, thiefSaleRequestID, ReviewRequestOpAction::APPROVE, "",
        ReviewRequestResultCode::BASE_ASSET_DOES_NOT_EXISTS);
    }

}
