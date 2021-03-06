// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include <ledger/OfferHelper.h>
#include <ledger/AssetPairHelper.h>
#include <transactions/FeesManager.h>
#include "main/Application.h"
#include "ledger/AssetHelper.h"
#include "ledger/ReviewableRequestHelper.h"
#include "main/test.h"
#include "TxTests.h"
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
#include "ledger/OfferHelper.h"
#include "test_helper/ManageAssetPairTestHelper.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

uint64_t addNewParticipant(TestManager::pointer testManager, Account& root, Account& participant, const uint64_t saleID, const AssetCode baseAsset,
    const AssetCode quoteAsset, const uint64_t quoteAssetAmount, const uint64_t price, const uint64_t fee)
{
    auto quoteBalance = BalanceHelper::Instance()->loadBalance(participant.key.getPublicKey(), quoteAsset, testManager->getDB(), nullptr);
    REQUIRE(!!quoteBalance);
    // issue 1 more to ensure that it is enough to cover rounded up base amount
    IssuanceRequestHelper(testManager).applyCreateIssuanceRequest(root, quoteAsset, quoteAssetAmount + fee + 1, quoteBalance->getBalanceID(),
        SecretKey::random().getStrKeyPublic());
    auto accountID = participant.key.getPublicKey();
    auto balanceCreationResult = ManageBalanceTestHelper(testManager).applyManageBalanceTx(participant, accountID, baseAsset);
    const auto baseAssetAmount = bigDivide(quoteAssetAmount, ONE, price, ROUND_UP);
    auto manageOfferOp = OfferManager::buildManageOfferOp(balanceCreationResult.success().balanceID, quoteBalance->getBalanceID(),
        true, baseAssetAmount, price, fee, 0, saleID);
    auto result = ParticipateInSaleTestHelper(testManager).applyManageOffer(participant, manageOfferOp);
    return result.success().offer.offer().offerID;
}

uint64_t addNewParticipant(TestManager::pointer testManager, Account& root, const uint64_t saleID, const AssetCode baseAsset,
                       const AssetCode quoteAsset, const uint64_t quoteAssetAmount, const uint64_t price, const uint64_t fee)
{
    auto account = Account{ SecretKey::random(), 0 };
    CreateAccountTestHelper(testManager).applyCreateAccountTx(root, account.key.getPublicKey(), AccountType::NOT_VERIFIED);
    return addNewParticipant(testManager, root, account, saleID, baseAsset, quoteAsset, quoteAssetAmount, price, fee);
}

TEST_CASE("Sale in several quote assets", "[tx][sale_several_quote]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_POSTGRESQL);
    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();
    auto testManager = TestManager::make(app);

    Database& db = testManager->getDB();

    auto root = Account{ getRoot(), Salt(0) };

    AssetCode defaultQuoteAsset = "USD";
    uint64_t quoteMaxIssuance = INT64_MAX;
    auto assetTestHelper = ManageAssetTestHelper(testManager);
    auto assetCreationRequest = assetTestHelper.createAssetCreationRequest(defaultQuoteAsset, root.key.getPublicKey(), "{}", 0,
        uint32_t(AssetPolicy::BASE_ASSET));
    assetTestHelper.applyManageAssetTx(root, 0, assetCreationRequest);

    AssetCode quoteAssetBTC = "BTC";
    assetCreationRequest = assetTestHelper.createAssetCreationRequest(quoteAssetBTC, root.key.getPublicKey(), "{}", quoteMaxIssuance,
        uint32_t(AssetPolicy::BASE_ASSET), quoteMaxIssuance);
    assetTestHelper.applyManageAssetTx(root, 0, assetCreationRequest);

    AssetCode quoteAssetETH = "ETH";
    assetCreationRequest = assetTestHelper.createAssetCreationRequest(quoteAssetETH, root.key.getPublicKey(), "{}", quoteMaxIssuance,
        uint32_t(AssetPolicy::BASE_ASSET), quoteMaxIssuance);
    assetTestHelper.applyManageAssetTx(root, 0, assetCreationRequest);
    auto assetPairHelper = ManageAssetPairTestHelper(testManager);
    uint64_t btcUSDPrice = 10000 * ONE;
    assetPairHelper.applyManageAssetPairTx(root, quoteAssetBTC, defaultQuoteAsset, btcUSDPrice, 0, 0);
    uint64_t ethUSDPrice = 500 * ONE;
    assetPairHelper.applyManageAssetPairTx(root, quoteAssetETH, defaultQuoteAsset, ethUSDPrice, 0, 0);

    CreateAccountTestHelper createAccountTestHelper(testManager);
    SaleRequestHelper saleRequestHelper(testManager);
    IssuanceRequestHelper issuanceHelper(testManager);
    CheckSaleStateHelper checkStateHelper(testManager);

    auto syndicate = Account{ SecretKey::random(), 0 };
    auto syndicatePubKey = syndicate.key.getPublicKey();

    CreateAccountTestHelper(testManager).applyCreateAccountTx(root, syndicatePubKey, AccountType::SYNDICATE);
    const AssetCode baseAsset = "XAAU";
    const uint64_t maxIssuanceAmount = 2000 * ONE;
    const uint64_t preIssuedAmount = maxIssuanceAmount;
    assetCreationRequest = assetTestHelper.createAssetCreationRequest(baseAsset, syndicate.key.getPublicKey(), "{}",
        maxIssuanceAmount, 0, preIssuedAmount);
    assetTestHelper.createApproveRequest(root, syndicate, assetCreationRequest);

    uint64_t hardCap = 100000000 * ONE;
    uint64 softCap = 50000000 * ONE;

    const auto currentTime = testManager->getLedgerManager().getCloseTime();
    const auto endTime = currentTime + 1000;
    uint64_t xaauUSDPrice = (hardCap / maxIssuanceAmount) * ONE;
    uint64_t xaauBTCPrice = (xaauUSDPrice / btcUSDPrice) * ONE;
    uint64_t xaauETHPrice = (xaauUSDPrice / ethUSDPrice) * ONE;
    auto saleRequest = saleRequestHelper.createSaleRequest(baseAsset, defaultQuoteAsset, currentTime,
        endTime, softCap, hardCap, "{}", { saleRequestHelper.createSaleQuoteAsset(quoteAssetBTC, xaauBTCPrice),
            saleRequestHelper.createSaleQuoteAsset(quoteAssetETH, xaauETHPrice) });
    saleRequestHelper.createApprovedSale(root, syndicate, saleRequest);
    auto sales = SaleHelper::Instance()->loadSalesForOwner(syndicate.key.getPublicKey(), testManager->getDB());
    REQUIRE(sales.size() == 1);
    const auto saleID = sales[0]->getID();

    addNewParticipant(testManager, root, saleID, baseAsset, quoteAssetBTC, bigDivide(maxIssuanceAmount / 2, xaauBTCPrice, ONE, ROUND_UP), xaauBTCPrice, 0);
    addNewParticipant(testManager, root, saleID, baseAsset, quoteAssetETH, bigDivide(maxIssuanceAmount / 2, xaauETHPrice, ONE, ROUND_UP), xaauETHPrice, 0);

    CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root, saleID);

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

    auto root = Account{ getRoot(), Salt(0) };

    AssetCode quoteAsset = "USD";
    auto assetTestHelper = ManageAssetTestHelper(testManager);
    uint64_t quoteMaxIssuance = INT64_MAX;
    auto assetCreationRequest = assetTestHelper.createAssetCreationRequest(quoteAsset, root.key.getPublicKey(), "{}", quoteMaxIssuance,
                                                                           uint32_t(AssetPolicy::BASE_ASSET));
    assetTestHelper.applyManageAssetTx(root, 0, assetCreationRequest);

    CreateAccountTestHelper createAccountTestHelper(testManager);
    SaleRequestHelper saleRequestHelper(testManager);
    IssuanceRequestHelper issuanceHelper(testManager);
    CheckSaleStateHelper checkStateHelper(testManager);

    auto syndicate = Account{ SecretKey::random(), 0 };
    auto syndicatePubKey = syndicate.key.getPublicKey();

    CreateAccountTestHelper(testManager).applyCreateAccountTx(root, syndicatePubKey, AccountType::SYNDICATE);
    const AssetCode baseAsset = "BTC";
    // TODO: for now we need to keep maxIssuance = preIssuance to allow sale creation
    const uint64_t maxIssuanceAmount = 2000 * ONE;
    const uint64_t preIssuedAmount = maxIssuanceAmount;
    assetCreationRequest = assetTestHelper.createAssetCreationRequest(baseAsset, syndicate.key.getPublicKey(), "{}",
                                                                      maxIssuanceAmount,0, preIssuedAmount);
    assetTestHelper.createApproveRequest(root, syndicate, assetCreationRequest);
    const uint64_t price = 2 * ONE;
    const auto hardCap = static_cast<const uint64_t>(bigDivide(preIssuedAmount, price, ONE, ROUND_DOWN));
    const uint64_t softCap = hardCap / 2;
    const auto currentTime = testManager->getLedgerManager().getCloseTime();
    const auto endTime = currentTime + 1000;
    auto saleRequest = saleRequestHelper.createSaleRequest(baseAsset, quoteAsset, currentTime,
                                                           endTime, softCap, hardCap, "{}", { saleRequestHelper.createSaleQuoteAsset(quoteAsset, price)});
    SECTION("Happy path")
    {
        //set offer fee for sale owner and participants
        // TODO: use set fees
        auto sellerFeeFrame = FeeFrame::create(FeeType::OFFER_FEE, 0, int64_t(2 * ONE), quoteAsset, &syndicatePubKey);
        auto participantsFeeFrame = FeeFrame::create(FeeType::OFFER_FEE, 0, int64_t(1 * ONE), quoteAsset, nullptr);
        LedgerDelta delta(testManager->getLedgerManager().getCurrentLedgerHeader(), db);
        EntryHelperProvider::storeAddEntry(delta, db, sellerFeeFrame->mEntry);
        EntryHelperProvider::storeAddEntry(delta, db, participantsFeeFrame->mEntry);

        uint64_t quotePreIssued(0);
        participantsFeeFrame->calculatePercentFee(hardCap, quotePreIssued, ROUND_UP);
        quotePreIssued += hardCap + ONE;
        IssuanceRequestHelper(testManager).authorizePreIssuedAmount(root, root.key, quoteAsset, quotePreIssued, root);

        saleRequestHelper.createApprovedSale(root, syndicate, saleRequest);
        auto accountTestHelper = CreateAccountTestHelper(testManager);
        auto sales = SaleHelper::Instance()->loadSalesForOwner(syndicate.key.getPublicKey(), testManager->getDB());
        REQUIRE(sales.size() == 1);
        const auto saleID = sales[0]->getID();
        SECTION("Try to cancel sale offer as regular one")
        {
            auto account = Account{ SecretKey::random(), 0 };
            CreateAccountTestHelper(testManager).applyCreateAccountTx(root, account.key.getPublicKey(), AccountType::NOT_VERIFIED);
            uint64_t quoteAssetAmount = hardCap / 2;
            uint64_t feeToPay(0);
            participantsFeeFrame->calculatePercentFee(quoteAssetAmount, feeToPay, ROUND_UP);
            const auto offerID = addNewParticipant(testManager, root, account, saleID, baseAsset, quoteAsset, quoteAssetAmount, price, feeToPay);
            auto offer = OfferHelper::Instance()->loadOffer(account.key.getPublicKey(), offerID, testManager->getDB());
            REQUIRE(!!offer);
            const auto offerEntry = offer->getOffer();
            auto manageOfferOp = OfferManager::buildManageOfferOp(offerEntry.baseBalance, offerEntry.quoteBalance,
                true, 0, price, 0, offerEntry.offerID, 0);
            ParticipateInSaleTestHelper(testManager).applyManageOffer(account, manageOfferOp, ManageOfferResultCode::NOT_FOUND);
            manageOfferOp.orderBookID = saleID;
            ParticipateInSaleTestHelper(testManager).applyManageOffer(account, manageOfferOp);

        }
        SECTION("Reached hard cap")
        {
            const int numberOfParticipants = 10;
            const auto quoteAssetAmount = hardCap / numberOfParticipants;
            uint64_t feeToPay(0);
            participantsFeeFrame->calculatePercentFee(quoteAssetAmount, feeToPay, ROUND_UP);
            for (auto i = 0; i < numberOfParticipants; i++)
            {
                addNewParticipant(testManager, root, saleID, baseAsset, quoteAsset, quoteAssetAmount, price, feeToPay);
                if (i < numberOfParticipants - 1)
                {
                    CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root, saleID, CheckSaleStateResultCode::NOT_READY);
                }
            }

            CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root, saleID);
        }

        SECTION("Reached soft cap")
        {
            const int numberOfParticipants = 10;
            const uint64_t quoteAmount = softCap / numberOfParticipants;
            uint64_t feeToPay(0);
            participantsFeeFrame->calculatePercentFee(quoteAmount, feeToPay, ROUND_UP);
            const int64_t timeStep = (endTime - currentTime) / numberOfParticipants;
            for (int i = 0; i < numberOfParticipants - 1; i++)
            {
                addNewParticipant(testManager, root, saleID, baseAsset, quoteAsset, quoteAmount, price, feeToPay);
                testManager->advanceToTime(testManager->getLedgerManager().getCloseTime() + timeStep);
                checkStateHelper.applyCheckSaleStateTx(root, saleID, CheckSaleStateResultCode::NOT_READY);
            }
            // sale is still active
            participantsFeeFrame->calculatePercentFee(2 * quoteAmount, feeToPay, ROUND_UP);
            addNewParticipant(testManager, root, saleID, baseAsset, quoteAsset, 2 * quoteAmount, price, feeToPay);
            testManager->advanceToTime(endTime + 1);
            checkStateHelper.applyCheckSaleStateTx(root, saleID);
        }

        SECTION("Canceled")
        {
            const int numberOfParticipants = 10;
            const uint64_t quoteAmount = softCap / numberOfParticipants;
            uint64_t feeToPay(0);
            participantsFeeFrame->calculatePercentFee(quoteAmount, feeToPay, ROUND_UP);
            for (auto i = 0; i < numberOfParticipants - 1; i++)
            {
                addNewParticipant(testManager, root, saleID, baseAsset, quoteAsset, quoteAmount, price, feeToPay);
                checkStateHelper.applyCheckSaleStateTx(root, saleID, CheckSaleStateResultCode::NOT_READY);
            }
            // softcap is not reached, so no sale to close
            CheckSaleStateHelper(testManager).applyCheckSaleStateTx(root, saleID, CheckSaleStateResultCode::NOT_READY);
            // close ledger after end time
            testManager->advanceToTime(endTime + 1);
            auto checkRes = checkStateHelper.applyCheckSaleStateTx(root, saleID, CheckSaleStateResultCode::SUCCESS);
            REQUIRE(checkRes.success().effect.effect() == CheckSaleStateEffect::CANCELED);
        }
    }

    SECTION("Create SaleCreationRequest")
    {
        SECTION("Try to create sale with zero price")
        {
            saleRequest.quoteAssets[0].price = 0;
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
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 42, saleRequest,
            CreateSaleCreationRequestResultCode::REQUEST_NOT_FOUND);
        }
        SECTION("Base asset not found")
        {
            saleRequest.baseAsset = "GSC";
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::BASE_ASSET_OR_ASSET_REQUEST_NOT_FOUND);
        }
        SECTION("Quote asset not found")
        {
            saleRequest.quoteAssets[0].quoteAsset = "GSC";
            auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
            CreateSaleCreationRequestResultCode::QUOTE_ASSET_NOT_FOUND);
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

        SECTION("Max issuance or preissued amount is less then hard cap")
        {
            const AssetCode asset = "GSC";

            // Create asset creation request with max issuance 2000 * ONE
            assetCreationRequest = assetTestHelper.createAssetCreationRequest(asset, syndicatePubKey, "{}",
                                                                              maxIssuanceAmount, 0,
                                                                              preIssuedAmount);
            auto assetRequestCreationResult = assetTestHelper.applyManageAssetTx(syndicate, 0, assetCreationRequest);
            auto assetRequestID = assetRequestCreationResult.success().requestID;

            // Create sale creation request with hardCap 1000 * ONE
            saleRequest = saleRequestHelper.createSaleRequest(asset, quoteAsset, currentTime, currentTime + 1000, softCap, hardCap, "{}",
            { saleRequestHelper.createSaleQuoteAsset(quoteAsset, price)});
            auto saleRequestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest);
            auto saleRequestID = saleRequestCreationResult.success().requestID;

            SECTION("Hard cap will exceed max issuance")
            {
                // Update asset creation request with max issuance = 900 * ONE and pre issued = 500 * ONE
                assetCreationRequest.createAsset().maxIssuanceAmount = 900 * ONE;
                assetCreationRequest.createAsset().initialPreissuedAmount = 500 * ONE;
                assetRequestCreationResult = assetTestHelper.applyManageAssetTx(syndicate, assetRequestID,
                                                                                assetCreationRequest);
                // Approve asset creation request
                auto assetReviewer = ReviewAssetRequestHelper(testManager);
                assetReviewer.applyReviewRequestTx(root, assetRequestID, ReviewRequestOpAction::APPROVE, "");

                //Try to approve sale creation request
                saleReviewer.applyReviewRequestTx(root, saleRequestID, ReviewRequestOpAction::APPROVE, "",
                                                  ReviewRequestResultCode::INSUFFICIENT_PREISSUED_FOR_HARD_CAP);
            }
            SECTION("Preissued amount of base asset is not enough for hard cap")
            {
                // Update asset creation request with preissued = 500 * ONE
                assetCreationRequest.createAsset().initialPreissuedAmount = 500 * ONE;
                assetRequestCreationResult = assetTestHelper.applyManageAssetTx(syndicate, assetRequestID,
                                                                                assetCreationRequest);
                // Approve asset creation request
                auto assetReviewer = ReviewAssetRequestHelper(testManager);
                assetReviewer.applyReviewRequestTx(root, assetRequestID, ReviewRequestOpAction::APPROVE, "");

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
        const uint64_t assetMaxIssuanceAmount = 2000 * ONE;
        const uint64_t assetPreIssuedAmount = 1000 * ONE;

        // Owner creates asset creation request
        assetCreationRequest = assetTestHelper.createAssetCreationRequest(asset, ownerSyndicatePubKey, "{}",
                                                                          assetMaxIssuanceAmount,0,
                                                                          assetPreIssuedAmount);
        auto ownerRequestCreationResult = assetTestHelper.applyManageAssetTx(ownerSyndicate, 0, assetCreationRequest);
        auto ownerAssetRequestID = ownerRequestCreationResult.success().requestID;

        // Thief creates asset creation request
        assetCreationRequest = assetTestHelper.createAssetCreationRequest(asset, thiefSyndicatePubKey, "{}",
                                                                          assetMaxIssuanceAmount,0,
                                                                          assetPreIssuedAmount);
        auto thiefRequestCreationResult = assetTestHelper.applyManageAssetTx(thiefSyndicate, 0, assetCreationRequest);
        auto thiefAssetRequestID = thiefRequestCreationResult.success().requestID;

        // Thief creates sale creation request
        auto thiefSaleRequest = SaleRequestHelper::createSaleRequest(asset, quoteAsset, currentTime, currentTime + 1000,
                                                                     softCap, hardCap, "{}", {saleRequestHelper.createSaleQuoteAsset(quoteAsset, price)});

        auto thiefSaleRequestCreationResult = saleRequestHelper.applyCreateSaleRequest(thiefSyndicate, 0, thiefSaleRequest);
        auto thiefSaleRequestID = thiefSaleRequestCreationResult.success().requestID;

        auto assetReviewer = ReviewAssetRequestHelper(testManager);

        // Reviewer approves owner's asset creation request
        assetReviewer.applyReviewRequestTx(root, ownerAssetRequestID, ReviewRequestOpAction::APPROVE, "");

        // Reviewer approves thief's sale creation request
        saleReviewer.applyReviewRequestTx(root, thiefSaleRequestID, ReviewRequestOpAction::APPROVE, "",
        ReviewRequestResultCode::BASE_ASSET_DOES_NOT_EXISTS);
    }
    SECTION("Try to create sale, which is already started")
    {
        testManager->advanceToTime(2000);
        saleRequest.endTime = testManager->getLedgerManager().getCloseTime() + 1000;
        auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest);
        auto requestID = requestCreationResult.success().requestID;
        saleReviewer.applyReviewRequestTx(root, requestID, ReviewRequestOpAction::APPROVE, "");
    }
    SECTION("Try to create sale, which is already ended")
    {
        testManager->advanceToTime(2000);
        auto requestCreationResult = saleRequestHelper.applyCreateSaleRequest(syndicate, 0, saleRequest,
        CreateSaleCreationRequestResultCode::INVALID_END);
    }

    SECTION("Participation")
    {
        ParticipateInSaleTestHelper participateHelper(testManager);

        // create sale owner
        Account owner = Account{ SecretKey::random(), Salt(0) };
        createAccountTestHelper.applyCreateAccountTx(root, owner.key.getPublicKey(), AccountType::SYNDICATE);

        // create base asset
        const AssetCode baseAsset = "ETH";
        uint64_t maxIssuanceAmount = 10 * ONE;
        uint32 requiresKYCPolicy = static_cast<uint32>(AssetPolicy::REQUIRES_KYC);
        auto baseAssetRequest = assetTestHelper.createAssetCreationRequest(baseAsset, owner.key.getPublicKey(), "{}",
                                                                           maxIssuanceAmount, requiresKYCPolicy, maxIssuanceAmount);
        assetTestHelper.createApproveRequest(root, owner, baseAssetRequest);

        // create participant
        Account participant = Account{ SecretKey::random(), Salt(0) };
        AccountID participantID = participant.key.getPublicKey();
        createAccountTestHelper.applyCreateAccountTx(root, participantID, AccountType::GENERAL);

        // create base balance for participant:
        auto manageBalanceRes = ManageBalanceTestHelper(testManager).applyManageBalanceTx(participant, participantID, baseAsset);
        BalanceID baseBalance = manageBalanceRes.success().balanceID;
        BalanceID quoteBalance = BalanceHelper::Instance()->loadBalance(participantID, quoteAsset, db,
                                                                        nullptr)->getBalanceID();

        // pre-issue quote amount
        uint64_t quotePreIssued = quoteMaxIssuance - 1;
        issuanceHelper.authorizePreIssuedAmount(root, root.key, quoteAsset, quotePreIssued, root);

        SECTION("malformed manage offer")
        {
            // create sale to participate in:
            uint64_t startTime = testManager->getLedgerManager().getCloseTime() + 100;
            uint64_t endTime = startTime + 1000;
            uint64_t price = 2 * ONE;
            int64_t hardCap = bigDivide(maxIssuanceAmount, price, ONE, ROUND_UP);
            auto saleRequest = saleRequestHelper.createSaleRequest(baseAsset, quoteAsset, startTime, endTime,
                                                                   hardCap/2, hardCap, "{}", { saleRequestHelper.createSaleQuoteAsset(quoteAsset, price) });
            saleRequestHelper.createApprovedSale(root, owner, saleRequest);
            auto sales = SaleHelper::Instance()->loadSalesForOwner(owner.key.getPublicKey(), db);
            uint64_t saleID = sales[0]->getID();

            // fund participant with quote asset
            uint64_t quoteBalanceAmount = saleRequest.hardCap;
            issuanceHelper.applyCreateIssuanceRequest(root, quoteAsset, quoteBalanceAmount, quoteBalance,
                                                      SecretKey::random().getStrKeyPublic());

            // buy a half of sale in order to keep it active
            int64_t baseAmount = bigDivide(saleRequest.hardCap/2, ONE, saleRequest.quoteAssets[0].price, ROUND_UP);
            auto manageOffer = OfferManager::buildManageOfferOp(baseBalance, quoteBalance, true, baseAmount,
                saleRequest.quoteAssets[0].price, 0, 0, saleID);

            SECTION("try to participate in not started sale")
            {
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::SALE_IS_NOT_STARTED_YET);
            }

            // close ledger on start time
            testManager->advanceToTime(startTime);

            SECTION("successfully create participation then delete it")
            {
                participateHelper.applyManageOffer(participant, manageOffer);

                auto offers = OfferHelper::Instance()->loadOffersWithFilters(baseAsset, quoteAsset, &saleID, nullptr, db);
                REQUIRE(offers.size() == 1);

                manageOffer.amount = 0;
                manageOffer.offerID = offers[0]->getOfferID();
                participateHelper.applyManageOffer(participant, manageOffer);
            }
            SECTION("try to sell base asset being participant")
            {
                manageOffer.isBuy = false;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::MALFORMED);
            }
            SECTION("try to participate with negative amount")
            {
                manageOffer.amount = -1;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::INVALID_AMOUNT);
            }
            SECTION("try to participate with zero price")
            {
                manageOffer.price = 0;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::PRICE_IS_INVALID);
            }
            SECTION("overflow quote amount")
            {
                manageOffer.amount = bigDivide(INT64_MAX, ONE, manageOffer.price, ROUND_UP) + 1;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::OFFER_OVERFLOW);
            }
            SECTION("negative fee")
            {
                manageOffer.fee = -1;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::INVALID_PERCENT_FEE);
            }
            SECTION("base balance == quote balance")
            {
                manageOffer.baseBalance = manageOffer.quoteBalance;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::ASSET_PAIR_NOT_TRADABLE);
            }
            SECTION("base balance doesn't exist")
            {
                BalanceID nonExistingBalance = SecretKey::random().getPublicKey();
                manageOffer.baseBalance = nonExistingBalance;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::BALANCE_NOT_FOUND);
            }
            SECTION("quote balance doesn't exist")
            {
                BalanceID nonExistingBalance = SecretKey::random().getPublicKey();
                manageOffer.quoteBalance = nonExistingBalance;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::BALANCE_NOT_FOUND);
            }
            SECTION("base and quote balances mixed up")
            {
                manageOffer.baseBalance = quoteBalance;
                manageOffer.quoteBalance = baseBalance;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::ORDER_BOOK_DOES_NOT_EXISTS);
            }
            SECTION("try participate in non-existing sale")
            {
                uint64_t nonExistingSaleID = saleID + 1;
                manageOffer.orderBookID = nonExistingSaleID;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::ORDER_BOOK_DOES_NOT_EXISTS);
            }
            SECTION("base and quote balances are in the same asset")
            {
                //create one more balance in base asset:
                auto opRes = ManageBalanceTestHelper(testManager).applyManageBalanceTx(participant, participantID, baseAsset);
                auto baseBalanceID = opRes.success().balanceID;
                manageOffer.quoteBalance = baseBalanceID;

                // can't find a sale from base to base
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::ORDER_BOOK_DOES_NOT_EXISTS);
            }
            SECTION("price doesn't match sales price")
            {
                manageOffer.price = saleRequest.quoteAssets[0].price + 1;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::PRICE_DOES_NOT_MATCH);
            }
            SECTION("try to participate in own sale")
            {
                // load balances for owner
                auto quoteBalanceID = BalanceHelper::Instance()->loadBalance(owner.key.getPublicKey(), quoteAsset, db,
                                                                             nullptr)->getBalanceID();
                AccountID ownerID = owner.key.getPublicKey();
                auto baseBalanceID = ManageBalanceTestHelper(testManager).applyManageBalanceTx(owner, ownerID, baseAsset)
                                                                         .success().balanceID;
                manageOffer.baseBalance = baseBalanceID;
                manageOffer.quoteBalance = quoteBalanceID;
                participateHelper.applyManageOffer(owner, manageOffer, ManageOfferResultCode::CANT_PARTICIPATE_OWN_SALE);
            }
            SECTION("amount exceeds hard cap")
            {
                // fund account
                issuanceHelper.applyCreateIssuanceRequest(root, quoteAsset, 2 * ONE, quoteBalance,
                                                          SecretKey::random().getStrKeyPublic());
                SECTION("by less than ONE")
                {
                    int64_t baseAssetAmount = bigDivide(hardCap + ONE/2, ONE, price, ROUND_DOWN);
                    manageOffer.amount = baseAssetAmount;
                    participateHelper.applyManageOffer(participant, manageOffer);

                    checkStateHelper.applyCheckSaleStateTx(root, saleID, CheckSaleStateResultCode::SUCCESS);
                }
                SECTION("by more than ONE")
                {
                    int64_t baseAssetAmount = bigDivide(hardCap + 2 * ONE, ONE, price, ROUND_DOWN);
                    manageOffer.amount = baseAssetAmount;
                    participateHelper.applyManageOffer(participant, manageOffer,
                                                       ManageOfferResultCode::ORDER_VIOLATES_HARD_CAP);
                }
            }
            SECTION("underfunded")
            {
                // participent has ONE/2 less quote amount than he want to exchange
                int64_t baseAssetAmount = bigDivide(quoteBalanceAmount + ONE/2, ONE, price, ROUND_DOWN);
                manageOffer.amount = baseAssetAmount;
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::UNDERFUNDED);
            }
            SECTION("try to buy asset which requires KYC being NOT_VERIFIED")
            {
                Account notVerified = Account{SecretKey::random(), Salt(0)};
                AccountID notVerifiedID = notVerified.key.getPublicKey();
                createAccountTestHelper.applyCreateAccountTx(root, notVerifiedID, AccountType::NOT_VERIFIED);

                // create base balance
                auto baseBalanceID = ManageBalanceTestHelper(testManager).applyManageBalanceTx(notVerified, notVerifiedID,
                                                                                               baseAsset).success().balanceID;

                // fund with quote asset
                auto quoteBalanceID = BalanceHelper::Instance()->loadBalance(notVerifiedID, quoteAsset, db, nullptr)->getBalanceID();
                issuanceHelper.applyCreateIssuanceRequest(root, quoteAsset, quoteBalanceAmount, quoteBalanceID,
                                                          SecretKey::random().getStrKeyPublic());

                manageOffer.baseBalance = baseBalanceID;
                manageOffer.quoteBalance = quoteBalanceID;

                participateHelper.applyManageOffer(notVerified, manageOffer, ManageOfferResultCode::REQUIRES_KYC);
            }
            SECTION("delete participation")
            {
                // create sale participation:
                int64_t initialAmount = manageOffer.amount;
                participateHelper.applyManageOffer(participant, manageOffer);
                auto offers = OfferHelper::Instance()->loadOffersWithFilters(baseAsset, quoteAsset, &saleID, nullptr, db);
                REQUIRE(offers.size() == 1);
                uint64_t offerID = offers[0]->getOfferID();

                //delete offer
                manageOffer.amount = 0;
                manageOffer.offerID = offerID;

                SECTION("try to delete non-existing offer")
                {
                    //switch to non-existing offerID
                    manageOffer.offerID++;
                    participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::NOT_FOUND);
                }
                SECTION("try to delete from non-existing orderBook")
                {
                    uint64_t nonExistingOrderBookID = saleID + 1;
                    manageOffer.orderBookID = nonExistingOrderBookID;
                    participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::NOT_FOUND);
                }
                SECTION("try to delete closed sale")
                {
                    //participate again in order to close sale
                    int64_t baseHardCap = bigDivide(hardCap, ONE, price, ROUND_DOWN);
                    manageOffer.amount = baseHardCap - initialAmount;
                    manageOffer.offerID = 0;
                    participateHelper.applyManageOffer(participant, manageOffer);

                    //try to delete offer
                    manageOffer.offerID = offerID;
                    manageOffer.amount = 0;
                    participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::SALE_IS_NOT_ACTIVE);
                }
            }
            SECTION("try to participate after end time")
            {
                testManager->advanceToTime(endTime + 1);
                participateHelper.applyManageOffer(participant, manageOffer, ManageOfferResultCode::SALE_ALREADY_ENDED);
            }
        }
    }
}
