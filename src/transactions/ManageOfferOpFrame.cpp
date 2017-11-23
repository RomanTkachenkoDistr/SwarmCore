// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/ManageCoinsEmissionRequestOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/types.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

ManageOfferOpFrame::ManageOfferOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mManageOffer(mOperation.body.manageOfferOp())
{
}

BalanceFrame::pointer ManageOfferOpFrame::loadBalanceValidForTrading(BalanceID const& balanceID, medida::MetricsRegistry& metrics, Database& db, LedgerDelta & delta)
{
	auto balance = BalanceFrame::loadBalance(balanceID, db, &delta);
	if (!balance || !(balance->getAccountID() == getSourceID()))
	{
		metrics
			.NewMeter({ "op-manage-offer", "invalid", "balance-not-found" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_BALANCE_NOT_FOUND);
		return nullptr;
	}

	return balance;

}

AssetPairFrame::pointer ManageOfferOpFrame::loadTradableAssetPair(medida::MetricsRegistry& metrics, Database& db, LedgerDelta& delta)
{
	AssetPairFrame::pointer assetPair = AssetPairFrame::loadAssetPair(mBaseBalance->getAsset(), mQuoteBalance->getAsset(), db, &delta);
	if (assetPair && assetPair->checkPolicy(ASSET_PAIR_TRADEABLE))
		return assetPair;

	metrics
		.NewMeter({ "op-manage-offer", "invalid", "asset-pair-not-tradable" },
			"operation")
		.Mark();
	innerResult().code(MANAGE_OFFER_ASSET_PAIR_NOT_TRADABLE);
	return nullptr;
}

bool ManageOfferOpFrame::checkPhysicalPriceRestrictionMet(AssetPairFrame::pointer assetPair, medida::MetricsRegistry& metrics)
{
	if (!assetPair->checkPolicy(ASSET_PAIR_PHYSICAL_PRICE_RESTRICTION))
		return true;

	int64_t minPriceInTermsOfPhysical = assetPair->getMinPriceInTermsOfPhysical();
	if (minPriceInTermsOfPhysical <= mManageOffer.price)
		return true;

	metrics
		.NewMeter({ "op-manage-offer", "invalid", "violates-physical-price-restrictions" },
			"operation")
		.Mark();
	innerResult().code(MANAGE_OFFER_PHYSICAL_PRICE_RESTRICTION);
	innerResult().physicalPriceRestriction().physicalPrice = minPriceInTermsOfPhysical;
	return false;
}

bool ManageOfferOpFrame::checkCurrentPriceRestrictionMet(AssetPairFrame::pointer assetPair, medida::MetricsRegistry& metrics)
{
	if (!assetPair->checkPolicy(ASSET_PAIR_CURRENT_PRICE_RESTRICTION))
		return true;

	int64_t minPriceInTermsOfCurrent = assetPair->getMinPriceInTermsOfCurrent();
	if (minPriceInTermsOfCurrent <= mManageOffer.price)
		return true;

	metrics
		.NewMeter({ "op-manage-offer", "invalid", "violates-current-price-restrictions" },
			"operation")
		.Mark();
	innerResult().code(MAANGE_OFFER_CURRENT_PRICE_RESTRICTION);
	innerResult().currentPriceRestriction().currentPrice = minPriceInTermsOfCurrent;
	return false;
}

bool
ManageOfferOpFrame::checkOfferValid(Application& app, LedgerManager& lm, Database& db, LedgerDelta& delta)
{
	assert(mManageOffer.amount != 0);

	mBaseBalance = ManageOfferOpFrame::loadBalanceValidForTrading(mManageOffer.baseBalance, app.getMetrics(), db, delta);
	if (!mBaseBalance)
		return false;

	mQuoteBalance = ManageOfferOpFrame::loadBalanceValidForTrading(mManageOffer.quoteBalance, app.getMetrics(), db, delta);
	if (!mQuoteBalance)
		return false;

	if (mBaseBalance->getAsset() == mQuoteBalance->getAsset())
	{
		app.getMetrics()
			.NewMeter({ "op-manage-offer", "invalid", "can't-trade-same-asset" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_ASSET_PAIR_NOT_TRADABLE);
		return false;
	}

	mAssetPair = loadTradableAssetPair(app.getMetrics(), db, delta);
	if (!mAssetPair)
		return false;

	if (!checkPhysicalPriceRestrictionMet(mAssetPair, app.getMetrics()))
		return false;

	if (!checkCurrentPriceRestrictionMet(mAssetPair, app.getMetrics()))
		return false;

    return true;
}

void ManageOfferOpFrame::removeOffersBelowPrice(Database& db, LedgerDelta& delta, AssetPairFrame::pointer assetPair, int64_t price)
{
	if (price <= 0)
		return;
	std::vector<OfferFrame::pointer> offersToRemove;
	OfferFrame::loadOffersWithPriceLower(assetPair->getBaseAsset(), assetPair->getQuoteAsset(), price, offersToRemove, db);
	for (OfferFrame::pointer offerToRemove : offersToRemove)
	{
		delta.recordEntry(*offerToRemove);
		ManageOfferOpFrame::deleteOffer(offerToRemove, db, delta);
	}
}


void ManageOfferOpFrame::deleteOffer(OfferFrame::pointer offerFrame, Database&db, LedgerDelta& delta) {
	BalanceID balanceID;
	int64_t amountToUnlock;
	auto& offer = offerFrame->getOffer();
	if (offer.isBuy)
	{
		balanceID = offer.quoteBalance;
		amountToUnlock = offer.quoteAmount + offer.fee;
		assert(amountToUnlock >= 0);
	}
	else
	{
		balanceID = offer.baseBalance;
		amountToUnlock = offer.baseAmount;
	}
	auto balanceFrame = BalanceFrame::loadBalance(balanceID, db, &delta);
	if (!balanceFrame)
		throw new runtime_error("Invalid database state: failed to load balance to cancel order");

	if (balanceFrame->lockBalance(-amountToUnlock) != BalanceFrame::Result::SUCCESS)
		throw new runtime_error("Invalid database state: failed to unlocked locked amount for offer");

	offerFrame->storeDelete(delta, db);
	balanceFrame->storeChange(delta, db);
}

bool ManageOfferOpFrame::deleteOffer(medida::MetricsRegistry& metrics, Database& db, LedgerDelta & delta)
{
	auto offer = OfferFrame::loadOffer(getSourceID(), mManageOffer.offerID, db, &delta);
	if (!offer)
	{
		metrics
			.NewMeter({ "op-manage-offer", "invalid", "not-found" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_NOT_FOUND);
		return false;
	}

	deleteOffer(offer, db, delta);

	
	innerResult().code(MANAGE_OFFER_SUCCESS);
	innerResult().success().offer.effect(MANAGE_OFFER_DELETED);

	metrics.NewMeter({ "op-create-offer", "success", "apply" }, "operation")
		.Mark();

	return true;
}

bool ManageOfferOpFrame::lockSellingAmount(OfferEntry const& offer)
{
	BalanceFrame::pointer sellingBalance;
	int64_t sellingAmount;
	if (offer.isBuy)
	{
		sellingBalance = mQuoteBalance;
		sellingAmount = offer.quoteAmount + offer.fee;
	}
	else {
		sellingBalance = mBaseBalance;
		sellingAmount = offer.baseAmount;
	}

	if (sellingAmount <= 0)
		return false;
	return sellingBalance->lockBalance(sellingAmount) == BalanceFrame::Result::SUCCESS;
}

bool ManageOfferOpFrame::setFeeToBeCharged(OfferEntry& offer, AssetCode const& quoteAsset, Database& db)
{
	offer.fee = 0;
	offer.percentFee = 0;

	auto feeFrame = FeeFrame::loadForAccount(OFFER_FEE, quoteAsset, FeeFrame::SUBTYPE_ANY, mSourceAccount, offer.quoteAmount, db);
	if (!feeFrame)
		return true;

	offer.percentFee = feeFrame->getFee().percentFee;
	if (offer.percentFee == 0)
		return true;

	return OfferExchange::setFeeToPay(offer.fee, offer.quoteAmount, offer.percentFee);
}

std::unordered_map<AccountID, CounterpartyDetails> ManageOfferOpFrame::getCounterpartyDetails(Database & db, LedgerDelta * delta) const
{
	// no counterparties
	return{};
}

SourceDetails ManageOfferOpFrame::getSourceAccountDetails(std::unordered_map<AccountID, CounterpartyDetails> counterpartiesDetails) const
{
	uint32_t allowedBlockedReasons = 0;
	if (mManageOffer.offerID != 0 && mManageOffer.amount == 0)
		allowedBlockedReasons = getAnyBlockReason();
	return SourceDetails({GENERAL, NOT_VERIFIED}, mSourceAccount->getMediumThreshold(), SIGNER_BALANCE_MANAGER, allowedBlockedReasons);
}

bool
ManageOfferOpFrame::doApply(Application& app, LedgerDelta& delta,
                            LedgerManager& ledgerManager)
{
    Database& db = ledgerManager.getDatabase();
	// deleting offer
	if (mManageOffer.offerID)
	{
		return deleteOffer(app.getMetrics(), db, delta);
	}

	if (!checkOfferValid(app, ledgerManager, db, delta))
	{
		return false;
	}

	auto offerFrame = buildOffer(mManageOffer, mBaseBalance->getAsset(), mQuoteBalance->getAsset());
	if (!offerFrame) {
		app.getMetrics()
			.NewMeter({ "op-manage-offer", "invalid", "overflow" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_OVERFLOW);
		return false;
	}

	auto& offer = offerFrame->getOffer();
	offer.createdAt = ledgerManager.getCloseTime();
	if (!setFeeToBeCharged(offer, mQuoteBalance->getAsset(), db))
	{
		app.getMetrics()
			.NewMeter({ "op-manage-offer", "invalid", "overflow" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_OVERFLOW);
		return false;
	}


	bool isFeeCorrect = offer.fee <= mManageOffer.fee;
	if (!isFeeCorrect)
	{
		app.getMetrics()
			.NewMeter({ "op-manage-offer", "invalid", "calculated-fee-does-not-match-fee" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_MALFORMED);
		return false;
	}

	offer.fee = mManageOffer.fee;

	if (offer.quoteAmount <= offer.fee)
	{
		app.getMetrics()
			.NewMeter({ "op-manage-offer", "invalid", "fee-exceeds-quote-amount" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_MALFORMED);
		return false;
	}

	if (!lockSellingAmount(offer))
	{
		app.getMetrics()
			.NewMeter({ "op-manage-offer", "invalid", "underfunded" },
				"operation")
			.Mark();
		innerResult().code(MANAGE_OFFER_UNDERFUNDED);
		return false;
	}

    innerResult().code(MANAGE_OFFER_SUCCESS);

	BalanceFrame::pointer commissionBalance = BalanceFrame::loadBalance(app.getCommissionID(), mAssetPair->getQuoteAsset(), db, &delta);
	assert(commissionBalance);

	AccountManager accountManager(app, db, delta, ledgerManager);
	auto commissionAccount = AccountFrame::loadAccount(delta, commissionBalance->getAccountID(), db);

	OfferExchange oe(accountManager, delta, ledgerManager, mAssetPair, commissionBalance);

	int64_t price = offer.price;
        OfferExchange::ConvertResult r = oe.convertWithOffers(offer, mBaseBalance, mQuoteBalance,
			[this, &price](OfferFrame const& o) {
				bool isPriceBetter = o.getOffer().isBuy ? o.getPrice() >= price : o.getPrice() <= price;
                if (!isPriceBetter)
                {
                    return OfferExchange::eStop;
                }
                if (o.getOffer().ownerID == getSourceID())
                {
                    // we are crossing our own offer
                    innerResult().code(MANAGE_OFFER_CROSS_SELF);
                    return OfferExchange::eStop;
                }
                return OfferExchange::eKeep;
            });

        switch (r)
        {
        case OfferExchange::eOK:
        case OfferExchange::ePartial:
            break;
        case OfferExchange::eFilterStop:
            if (innerResult().code() != MANAGE_OFFER_SUCCESS)
            {
                return false;
            }
            break;
		default:
			throw std::runtime_error("Unexpected offer exchange result");
        }

        // updates the result with the offers that got taken on the way
		auto takenOffers = oe.getOfferTrail();

        for (auto const& oatom : takenOffers)
        {
            innerResult().success().offersClaimed.push_back(oatom);
        }

		if (!takenOffers.empty()) {
			int64_t currentPrice = takenOffers[takenOffers.size() - 1].currentPrice;
			mAssetPair->setCurrentPrice(currentPrice);
			mAssetPair->storeChange(delta, db);

			commissionBalance->storeChange(delta, db);
		}
    

    if (oe.offerNeedsMore(offer))
    {

		offerFrame->mEntry.data.offer().offerID = delta.getHeaderFrame().generateID();
		innerResult().success().offer.effect(MANAGE_OFFER_CREATED);
		offerFrame->storeAdd(delta, db);
		mSourceAccount->storeChange(delta, db);
        innerResult().success().offer.offer() = offer;
    }
    else
    {

		OfferExchange::unlockBalancesForTakenOffer(*offerFrame, mBaseBalance, mQuoteBalance);
        innerResult().success().offer.effect(MANAGE_OFFER_DELETED);
    }

	innerResult().success().baseAsset = mAssetPair->getBaseAsset();
	innerResult().success().quoteAsset = mAssetPair->getQuoteAsset();
	mBaseBalance->storeChange(delta, db);
	mQuoteBalance->storeChange(delta, db);


    app.getMetrics()
        .NewMeter({"op-create-offer", "success", "apply"}, "operation")
        .Mark();
    return true;
}

// makes sure the currencies are different
bool
ManageOfferOpFrame::doCheckValid(Application& app)
{
	bool isPriceInvalid = mManageOffer.amount < 0 || mManageOffer.price <= 0;
	bool isTryingToUpdate = mManageOffer.offerID > 0 && mManageOffer.amount > 0;
	bool isDeleting = mManageOffer.amount == 0 && mManageOffer.offerID > 0;
	bool isQuoteAmountFits = isDeleting || getQuoteAmount() > 0;
    if (isPriceInvalid || isTryingToUpdate || !isQuoteAmountFits || mManageOffer.fee < 0)
    {
        app.getMetrics()
            .NewMeter({"op-manage-offer", "invalid", "negative-or-zero-values"}, "operation")
            .Mark();
        innerResult().code(MANAGE_OFFER_MALFORMED);
        return false;
    }

    if (mManageOffer.baseBalance == mManageOffer.quoteBalance)
    {
        app.getMetrics()
            .NewMeter({"op-manage-offer", "invalid", "invalid-balances"},
                      "operation")
            .Mark();
        innerResult().code(MANAGE_OFFER_ASSET_PAIR_NOT_TRADABLE);
        return false;
    }

    if (mManageOffer.offerID == 0 && mManageOffer.amount == 0)
    {
        app.getMetrics()
            .NewMeter({"op-manage-offer", "invalid", "create-with-zero"},
                      "operation")
            .Mark();
        innerResult().code(MANAGE_OFFER_NOT_FOUND);
        return false;
    }

    return true;
}

int64_t ManageOfferOpFrame::getQuoteAmount()
{
	// 1. Check quote amount fits minimal presidion 
	int64_t result;
	if (!bigDivide(result, mManageOffer.amount, mManageOffer.price, ONE, ROUND_DOWN))
		return 0;

	if (result == 0)
		return 0;

	// 2. Calculate amount to be spent
	if (!bigDivide(result, mManageOffer.amount, mManageOffer.price, ONE, ROUND_UP))
		return 0;
	return result;
}

OfferFrame::pointer
ManageOfferOpFrame::buildOffer(ManageOfferOp const& op, AssetCode const& base,
	AssetCode const& quote)
{
    OfferEntry o;
	o.base = base;
	o.baseAmount = op.amount;
	o.baseBalance = op.baseBalance;
	o.quoteBalance = op.quoteBalance;
	o.isBuy = op.isBuy;
	o.offerID = op.offerID;
	o.ownerID = getSourceID();
	o.price = op.price;
	o.quote = quote;
	o.quoteAmount = getQuoteAmount();

	LedgerEntry le;
	le.data.type(OFFER_ENTRY);
	le.data.offer() = o;
	return std::make_shared<OfferFrame>(le);
}
}