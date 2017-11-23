// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "invariant/Invariants.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/ByteSlice.h"
#include "transactions/TxTests.h"
#include "util/types.h"
#include "transactions/TransactionFrame.h"
#include "ledger/LedgerDelta.h"
#include "ledger/FeeFrame.h"
#include "ledger/StatisticsFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageBalanceOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/ManageCoinsEmissionRequestOpFrame.h"
#include "transactions/ManageForfeitRequestOpFrame.h"
#include "transactions/RecoverOpFrame.h"
#include "transactions/ReviewPaymentRequestOpFrame.h"
#include "transactions/ManageAssetOpFrame.h"
#include "transactions/ManageAssetPairOpFrame.h"
#include "transactions/DirectDebitOpFrame.h"
#include "transactions/SetLimitsOpFrame.h"
#include "transactions/ManageInvoiceOpFrame.h"
#include "ledger/CoinsEmissionFrame.h"
#include "ledger/AssetPairFrame.h"
#include "ledger/OfferFrame.h"
#include "ledger/InvoiceFrame.h"
#include "crypto/SHA.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
using xdr::operator==;

namespace txtest
{

FeeEntry createFeeEntry(FeeType type, int64_t fixed, int64_t percent,
    AssetCode asset, AccountID* accountID, AccountType* accountType, int64_t subtype,
    int64_t lowerBound, int64_t upperBound)
{
	FeeEntry fee;
	fee.feeType = type;
	fee.fixedFee = fixed;
	fee.percentFee = percent;
	fee.asset = asset;
    fee.subtype = subtype;
    if (accountID) {
        fee.accountID.activate() = *accountID;
    }
    if (accountType) {
        fee.accountType.activate() = *accountType;
    }
    fee.lowerBound = lowerBound;
    fee.upperBound = upperBound;

    fee.hash = FeeFrame::calcHash(type, asset, accountID, accountType, subtype);
    
	return fee;
}

PaymentFeeData getNoPaymentFee() {
    return getGeneralPaymentFee(0, 0);
}

PaymentFeeData getGeneralPaymentFee(uint64 fixedFee, uint64 paymentFee) {
    PaymentFeeData paymentFeeData;
    FeeData generalFeeData;
    generalFeeData.fixedFee = fixedFee;
    generalFeeData.paymentFee = paymentFee;
    paymentFeeData.sourceFee = generalFeeData;
    paymentFeeData.destinationFee = generalFeeData;
    paymentFeeData.sourcePaysForDest = true;
    
    return paymentFeeData;
}

bool
applyCheck(TransactionFramePtr tx, LedgerDelta& delta, Application& app)
{
    auto txSet = std::make_shared<TxSetFrame>(
            app.getLedgerManager().getLastClosedLedgerHeader().hash);
    txSet->add(tx);

	tx->clearCached();
    bool check = tx->checkValid(app);
    TransactionResult checkResult = tx->getResult();
    REQUIRE((!check || checkResult.result.code() == txSUCCESS));
    
    bool res;
    auto code = checkResult.result.code();

    if (code != txNO_ACCOUNT && code != txDUPLICATION)
    {
        tx->processSeqNum();
    }

    bool doApply = (code != txDUPLICATION);

    if (doApply)
    {
        res = tx->apply(delta, app);


        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        if (!check)
        {
            REQUIRE(checkResult == tx->getResult());
        }

		if (res)
		{
			delta.commit();
		}
    }
    else
    {
        res = check;
    }

    // verify modified accounts invariants
    auto const& changes = delta.getChanges();
    for (auto const& c : changes)
    {
        switch (c.type())
        {
        case LEDGER_ENTRY_CREATED:
            checkEntry(c.created(), app);
            break;
        case LEDGER_ENTRY_UPDATED:
            checkEntry(c.updated(), app);
            break;
        default:
            break;
        }
    }

    // validates db state
    app.getLedgerManager().checkDbState();
    app.getInvariants().check(txSet, delta);
    return res;
}

void
checkEntry(LedgerEntry const& le, Application& app)
{
    auto& d = le.data;
    switch (d.type())
    {
    case ACCOUNT:
        checkAccount(d.account().accountID, app);
        break;
	case COINS_EMISSION_REQUEST:
		checkAccount(d.coinsEmissionRequest().issuer, app);
		break;
    default:
        break;
    }
}


void
checkAccount(AccountID const& id, Application& app)
{
    AccountFrame::pointer res =
        AccountFrame::loadAccount(id, app.getDatabase());
    REQUIRE(!!res);
}

time_t
getTestDate(int day, int month, int year)
{
    std::tm tm = {0};
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday = day;
    tm.tm_mon = month - 1; // 0 based
    tm.tm_year = year - 1900;

    VirtualClock::time_point tp = VirtualClock::tmToPoint(tm);
    time_t t = VirtualClock::to_time_t(tp);

    return t;
}

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq, time_t closeTime)
{

	TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
		app.getLedgerManager().getLastClosedLedgerHeader().hash);
	return closeLedgerOn(app, ledgerSeq, closeTime, txSet);
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              TransactionFramePtr tx)
{
    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);

    if (tx)
    {
        txSet->add(tx);
        txSet->sortForHash();
    }

    return closeLedgerOn(app, ledgerSeq, day, month, year, txSet);
}

TxSetResultMeta closeLedgerOn(Application& app, uint32 ledgerSeq, time_t closeTime, TxSetFramePtr txSet)
{
	StellarValue sv(txSet->getContentsHash(), closeTime,
		emptyUpgradeSteps, StellarValue::_ext_t(LedgerVersion::EMPTY_VERSION));
	LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
	app.getLedgerManager().closeLedger(ledgerData);

	auto z1 = TransactionFrame::getTransactionHistoryResults(app.getDatabase(),
		ledgerSeq);
	auto z2 =
		TransactionFrame::getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

	REQUIRE(app.getLedgerManager().getLedgerNum() == (ledgerSeq + 1));

	TxSetResultMeta res;
	std::transform(z1.results.begin(), z1.results.end(), z2.begin(),
		std::back_inserter(res), [](TransactionResultPair const& r1,
			LedgerEntryChanges const& r2)
	{
		return std::make_pair(r1, r2);
	});

	return res;
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              TxSetFramePtr txSet)
{
	return closeLedgerOn(app, ledgerSeq, getTestDate(day, month, year), txSet);
    
}

void
upgradeToCurrentLedgerVersion(Application& app)
{
    auto const& lcl = app.getLedgerManager().getLastClosedLedgerHeader();
    auto const& lastHash = lcl.hash;
    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(lastHash);

    LedgerUpgrade upgrade(LEDGER_UPGRADE_VERSION);
    upgrade.newLedgerVersion() = app.getConfig().LEDGER_PROTOCOL_VERSION;
    xdr::xvector<UpgradeType, 6> upgrades;
    Value v(xdr::xdr_to_opaque(upgrade));
    upgrades.emplace_back(v.begin(), v.end());

    StellarValue sv(txSet->getContentsHash(), 1, upgrades, StellarValue::_ext_t(LedgerVersion::EMPTY_VERSION));
    LedgerCloseData ledgerData(1, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);
}

SecretKey
getRoot()
{
	return getMasterKP();
}

SecretKey
getIssuanceKey()
{
	return getIssuanceKP();
}

SecretKey
getAccount(const char* n)
{
	return getAccountSecret(n);
}


AccountFrame::pointer
loadAccount(SecretKey const& k, Application& app, bool mustExist)
{
	return loadAccount(k.getPublicKey(), app, mustExist);
}

AccountFrame::pointer
loadAccount(PublicKey const& k, Application& app, bool mustExist)
{
	AccountFrame::pointer res =
		AccountFrame::loadAccount(k, app.getDatabase());
	if (mustExist)
	{
		REQUIRE(res);
	}
	return res;
}

BalanceFrame::pointer
loadBalance(BalanceID bid, Application& app, bool mustExist)
{
	BalanceFrame::pointer res =
		BalanceFrame::loadBalance(bid, app.getDatabase());
	if (mustExist)
	{
		REQUIRE(res);
	}
	return res;
}


void
requireNoAccount(SecretKey const& k, Application& app)
{
    AccountFrame::pointer res = loadAccount(k, app, false);
    REQUIRE(!res);
}


CoinsEmissionRequestFrame::pointer loadCoinsEmissionRequest(uint64 requestID, Application& app, bool mustExist)
{
	CoinsEmissionRequestFrame::pointer res = CoinsEmissionRequestFrame::loadCoinsEmissionRequest(requestID, app.getDatabase());
	if (mustExist)
	{
		REQUIRE(res);
	}
	return res;
}

int64_t
getAccountBalance(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    std::vector<BalanceFrame::pointer> retBalances;
    BalanceFrame::loadBalances(account->getID(), retBalances, app.getDatabase());
    auto balanceFrame = retBalances[0];
    return balanceFrame->getBalance().amount;
}

int64_t
getAccountBalance(PublicKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    std::vector<BalanceFrame::pointer> retBalances;
    BalanceFrame::loadBalances(account->getID(), retBalances, app.getDatabase());
    auto balanceFrame = retBalances[0];
    return balanceFrame->getBalance().amount;
}

int64_t
getBalance(BalanceID const& k, Application& app)
{
    
    auto balance = BalanceFrame::loadBalance(k, app.getDatabase());
    assert(balance);
    return balance->getAmount();
}

void
checkTransaction(TransactionFrame& txFrame)
{
    REQUIRE(txFrame.getResult().feeCharged == 0); // default fee
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

void
checkTransactionForOpResult(TransactionFramePtr txFrame, Application& app, OperationResultCode opCode)
{
    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                app.getDatabase());
    applyCheck(txFrame, delta, app);
    REQUIRE(getFirstResult(*txFrame).code() == opCode);
}

TransactionFramePtr
transactionFromOperation(Hash const& networkID, SecretKey& from,
                         Salt salt, Operation const& op, SecretKey* signer = nullptr, TimeBounds* timeBounds = nullptr)
{
    if (!signer)
        signer = &from;
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.salt = salt;
    e.tx.operations.push_back(op);

    e.tx.timeBounds.minTime = 0;
    e.tx.timeBounds.maxTime = INT64_MAX / 2;
	if (timeBounds)
	{
		e.tx.timeBounds = *timeBounds;
	}

    TransactionFramePtr res =
        TransactionFrame::makeTransactionFromWire(networkID, e);

    res->addSignature(*signer);

    return res;
}


TransactionFramePtr
createCreateAccountTx(Hash const& networkID, SecretKey& from, SecretKey& to,
                      Salt seq, AccountType accountType, AccountID* referrer,
					  TimeBounds* timeBounds, int32 policies)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().destination = to.getPublicKey();
	op.body.createAccountOp().details.accountType(accountType);

	if (policies != -1)
	{
		op.body.createAccountOp().ext.v(ACCOUNT_POLICIES).policies() = policies;
	}

    if (referrer)
        op.body.createAccountOp().referrer.activate() = *referrer;
    return transactionFromOperation(networkID, from, seq, op, nullptr, timeBounds);
}


void
applyCreateAccountTx(Application& app, SecretKey& from, SecretKey& to,
                     Salt seq, AccountType accountType, SecretKey* signer,
                     AccountID* referrer, CreateAccountResultCode result, int32 policies)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createCreateAccountTx(app.getNetworkID(), from, to, seq,
        accountType, referrer, nullptr, policies);
	if (signer)
	{
		txFrame->getEnvelope().signatures.clear();
		txFrame->addSignature(*signer);
	}

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode =
        CreateAccountOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    AccountFrame::pointer toAccountAfter;
    toAccountAfter = loadAccount(to, app, false);

    if (innerCode != CREATE_ACCOUNT_SUCCESS)
    {
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
    }
    else
    {
        REQUIRE(toAccountAfter);
		REQUIRE(!toAccountAfter->isBlocked());
		REQUIRE(toAccountAfter->getAccountType() == accountType);
        
        auto statisticsFrame = StatisticsFrame::loadStatistics(to.getPublicKey(), app.getDatabase());
        REQUIRE(statisticsFrame);
        auto statistics = statisticsFrame->getStatistics();
        REQUIRE(statistics.dailyOutcome == 0);
        REQUIRE(statistics.weeklyOutcome == 0);
        REQUIRE(statistics.monthlyOutcome == 0);
        REQUIRE(statistics.annualOutcome == 0);
        
        if (!toAccount) {
            std::vector<BalanceFrame::pointer> balances;
            BalanceFrame::loadBalances(toAccountAfter->getAccount().accountID, balances, app.getDatabase());
            REQUIRE(balances.size() >= app.getBaseAssets().size());
			for (BalanceFrame::pointer balance : balances)
			{
				REQUIRE(balance->getBalance().amount == 0);
				REQUIRE(balance->getAccountID() == toAccountAfter->getAccount().accountID);
			}
            auto opResult = txResult.result.results()[0].tr().createAccountResult().success();
			int64_t feeSharePercent = 0;
			if (referrer)
			{
				auto referrerAccountID = *referrer;
				auto referrerFrame = AccountFrame::loadAccount(referrerAccountID, app.getDatabase());
				if (referrerFrame && !(referrerFrame->getID() == app.getMasterID()))
				{
					auto feeShare = FeeFrame::loadForAccount(REFERRAL_FEE, app.getBaseAsset(), 0, referrerFrame, 0, app.getDatabase());
					feeSharePercent = feeShare ? feeShare->getPercentFee() : 0;
				}
			}
			REQUIRE(opResult.referrerFee == feeSharePercent);
        }
    }
}


TransactionFramePtr createManageOffer(Hash const& networkID,
	SecretKey& source, Salt seq, uint64_t offerID, BalanceID const& baseBalance,
	BalanceID const& quoteBalance, int64_t amount, int64_t price, bool isBuy, int64_t fee)
{
	Operation op;
	op.body.type(MANAGE_OFFER);
	op.body.manageOfferOp().amount = amount;
	op.body.manageOfferOp().baseBalance = baseBalance;
	op.body.manageOfferOp().isBuy = isBuy;
	op.body.manageOfferOp().offerID = offerID;
	op.body.manageOfferOp().price = price;
	op.body.manageOfferOp().quoteBalance = quoteBalance;
	op.body.manageOfferOp().fee = fee;

	return transactionFromOperation(networkID, source, seq, op);
}

OfferFrame::pointer
loadOffer(SecretKey const& k, uint64 offerID, Application& app, bool mustExist)
{
	OfferFrame::pointer res =
		OfferFrame::loadOffer(k.getPublicKey(), offerID, app.getDatabase());
	if (mustExist)
	{
		REQUIRE(res);
	}
	return res;
}

ManageOfferResult
applyManageOfferTx(Application& app, SecretKey& source, Salt seq, uint64_t offerID, BalanceID const& baseBalance,
	BalanceID const& quoteBalance, int64_t amount, int64_t price, bool isBuy, int64_t fee,
	ManageOfferResultCode result)
{
	auto& db = app.getDatabase();
	
	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());
	uint64_t expectedOfferID = delta.getHeaderFrame().getLastGeneratedID() + 1;
	if (offerID != 0)
	{
		expectedOfferID = offerID;
	}
	
	TransactionFramePtr txFrame;

	txFrame = createManageOffer(app.getNetworkID(), source, seq,
		offerID, baseBalance, quoteBalance, amount, price, isBuy, fee);

	applyCheck(txFrame, delta, app);

	checkTransaction(*txFrame);

	auto& results = txFrame->getResult().result.results();

	REQUIRE(results.size() == 1);

	auto& manageOfferResult = results[0].tr().manageOfferResult();

	REQUIRE(manageOfferResult.code() == result);

	if (manageOfferResult.code() == MANAGE_OFFER_SUCCESS)
	{
		OfferFrame::pointer offer;

		auto& offerResult = manageOfferResult.success().offer;

		auto claimedOffers = manageOfferResult.success().offersClaimed;
		if (!claimedOffers.empty())
		{
			auto currentPrice = claimedOffers[claimedOffers.size() - 1].currentPrice;
			auto assetPair = AssetPairFrame::loadAssetPair(manageOfferResult.success().baseAsset, manageOfferResult.success().quoteAsset, db, nullptr);
			REQUIRE(assetPair->getCurrentPrice() == currentPrice);
		}

		switch (offerResult.effect())
		{
		case MANAGE_OFFER_CREATED:
		case MANAGE_OFFER_UPDATED:
		{
			offer = loadOffer(source, expectedOfferID, app);
			auto& offerEntry = offer->getOffer();
			REQUIRE(offerEntry == offerResult.offer());
			REQUIRE(offerEntry.price == price);
			REQUIRE(offerEntry.baseBalance == baseBalance);
			REQUIRE(offerEntry.quoteBalance == quoteBalance);
		}
		break;
		case MANAGE_OFFER_DELETED:
			REQUIRE(!loadOffer(source, expectedOfferID, app, false));
			break;
		default:
			abort();
		}
	}

	return manageOfferResult;
}

TransactionFramePtr
createManageBalanceTx(Hash const& networkID, SecretKey& from, SecretKey& account,
                      Salt seq, BalanceID balanceID, AssetCode asset, ManageBalanceAction action)
{
    Operation op;
    op.body.type(MANAGE_BALANCE);
    op.body.manageBalanceOp().destination = account.getPublicKey();
    op.body.manageBalanceOp().balanceID = balanceID;
    op.body.manageBalanceOp().action = action;
    op.body.manageBalanceOp().asset = asset;

    return transactionFromOperation(networkID, from, seq, op);
}

ManageBalanceResult
applyManageBalanceTx(Application& app, SecretKey& from, SecretKey& account,
                     Salt seq, BalanceID balanceID, AssetCode asset, ManageBalanceAction action,
                     ManageBalanceResultCode result)
{
    TransactionFramePtr txFrame;

    std::vector<BalanceFrame::pointer> balances;
    BalanceFrame::loadBalances(account.getPublicKey(), balances, app.getDatabase());
    
    
    txFrame = createManageBalanceTx(app.getNetworkID(), from, account, seq, balanceID, asset, action);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode =
        ManageBalanceOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);
    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    std::vector<BalanceFrame::pointer> balancesAfter;
    BalanceFrame::loadBalances(account.getPublicKey(), balancesAfter, app.getDatabase());

    auto opResult = txResult.result.results()[0].tr().manageBalanceResult();

    if (innerCode != MANAGE_BALANCE_SUCCESS)
    {
        REQUIRE(balances.size() == balancesAfter.size());
    }
    else
    {
        if (action == MANAGE_BALANCE_CREATE)
        {
            auto assetFrame = AssetFrame::loadAsset(asset, app.getDatabase());
            REQUIRE(balances.size() == balancesAfter.size() - 1);
            auto balance = BalanceFrame::loadBalance(balanceID, app.getDatabase());
            REQUIRE(balance);
            REQUIRE(balance->getBalance().accountID == account.getPublicKey());
            REQUIRE(balance->getBalance().amount == 0);
            REQUIRE(balance->getAsset() == asset);
        }
        else
        {
            REQUIRE(balances.size() == balancesAfter.size() + 1);
            REQUIRE(!BalanceFrame::loadBalance(balanceID, app.getDatabase()));
        }
    }
    return opResult;
}

TransactionFramePtr
createManageAssetTx(Hash const& networkID, SecretKey& source, Salt seq, AssetCode code,
    int32 policies, ManageAssetAction action)
{
    Operation op;
    op.body.type(MANAGE_ASSET);
    op.body.manageAssetOp().code = code;
    op.body.manageAssetOp().policies = policies;
    op.body.manageAssetOp().action = action;

    return transactionFromOperation(networkID, source, seq, op);
}

void
applyManageAssetTx(Application &app, SecretKey &source, Salt seq, AssetCode code, int32 policies,
                   ManageAssetAction action, ManageAssetResultCode result)
{
    TransactionFramePtr txFrame;

    uint64 assetsSize = AssetFrame::countObjects(app.getDatabase().getSession());
    txFrame = createManageAssetTx(app.getNetworkID(), source, seq, code, policies, action);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode =
        ManageAssetOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);
    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    uint64 assetsAfterSize = AssetFrame::countObjects(app.getDatabase().getSession());

    auto opResult = txResult.result.results()[0].tr().manageAssetResult();

    if (innerCode != MANAGE_ASSET_SUCCESS)
    {
        REQUIRE(assetsSize == assetsAfterSize);
    }
    else
    {
        if (action == MANAGE_ASSET_CREATE)
        {
            REQUIRE(assetsSize == assetsAfterSize - 1);
            auto asset = AssetFrame::loadAsset(code, app.getDatabase());
            REQUIRE(asset);
            REQUIRE(asset->getPolicies() == policies);
            REQUIRE(BalanceFrame::loadBalance(app.getMasterID(), code, app.getDatabase(), nullptr));
            REQUIRE(BalanceFrame::loadBalance(app.getCommissionID(), code, app.getDatabase(), nullptr));
        }
        else if (action == MANAGE_ASSET_UPDATE_POLICIES)
        {
            auto asset = AssetFrame::loadAsset(code, app.getDatabase());
            REQUIRE(asset);
            if (action == MANAGE_ASSET_UPDATE_POLICIES)
                REQUIRE(asset->getPolicies() == policies);
        }
    }
}

TransactionFramePtr createManageAssetPairTx(Hash const& networkID, SecretKey& source,
	Salt seq, AssetCode base, AssetCode quote,
	int64_t physicalPrice, int64_t physicalPriceCorrection, int64_t maxPriceStep, int32 policies, ManageAssetPairAction action)
{
	Operation op;
	op.body.type(MANAGE_ASSET_PAIR);
	auto& manageAssetPair = op.body.manageAssetPairOp();
	manageAssetPair.action = action;
	manageAssetPair.base = base;
	manageAssetPair.quote = quote;
	manageAssetPair.maxPriceStep = maxPriceStep;
	manageAssetPair.physicalPrice = physicalPrice;
	manageAssetPair.physicalPriceCorrection = physicalPriceCorrection;
	manageAssetPair.policies = policies;

	return transactionFromOperation(networkID, source, seq, op);
}

void
applyManageAssetPairTx(Application& app, SecretKey& source, Salt seq, AssetCode base, AssetCode quote,
	int64_t physicalPrice, int64_t physicalPriceCorrection, int64_t maxPriceStep, int32 policies,
	ManageAssetPairAction action,
	ManageAssetPairResultCode result)
{

	auto assetPairFrameBefore = AssetPairFrame::loadAssetPair(base, quote, app.getDatabase());
	auto countBefore = AssetPairFrame::countObjects(app.getDatabase().getSession());

	TransactionFramePtr txFrame;
	txFrame = createManageAssetPairTx(app.getNetworkID(), source, seq, base, quote, physicalPrice, physicalPriceCorrection, maxPriceStep, policies, action);

	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());
	applyCheck(txFrame, delta, app);

	checkTransaction(*txFrame);
	auto txResult = txFrame->getResult();
	auto innerCode =
		ManageAssetPairOpFrame::getInnerCode(txResult.result.results()[0]);
	REQUIRE(innerCode == result);
	REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

	auto opResult = txResult.result.results()[0].tr().manageAssetPairResult();

	bool isCreate = action == MANAGE_ASSET_PAIR_CREATE;
	auto countAfter = AssetPairFrame::countObjects(app.getDatabase().getSession());
	auto assetPairFrameAfter = AssetPairFrame::loadAssetPair(base, quote, app.getDatabase());

	if (innerCode != MANAGE_ASSET_PAIR_SUCCESS)
	{
		REQUIRE(countBefore == countAfter);
		if (assetPairFrameBefore)
			REQUIRE(assetPairFrameBefore->getAssetPair() == assetPairFrameAfter->getAssetPair());
		return;
	}

	REQUIRE(assetPairFrameAfter);

	auto assetPairAfter = assetPairFrameAfter->getAssetPair();
	REQUIRE(opResult.success().currentPrice == assetPairAfter.currentPrice);
	AssetPairEntry assetPairBefore;
	switch (action)
	{
	case MANAGE_ASSET_PAIR_CREATE:
		REQUIRE(countBefore == (countAfter - 1));
		assetPairBefore.base = base;
		assetPairBefore.quote = quote;
		assetPairBefore.currentPrice = physicalPrice;
		assetPairBefore.maxPriceStep = maxPriceStep;
		assetPairBefore.physicalPrice = physicalPrice;
		assetPairBefore.physicalPriceCorrection = physicalPriceCorrection;
		assetPairBefore.policies = policies;
		break;
	case MANAGE_ASSET_PAIR_UPDATE_PRICE:
	{
		REQUIRE(assetPairFrameBefore);
		int64_t premium = assetPairFrameBefore->getCurrentPrice() - assetPairFrameBefore->getAssetPair().physicalPrice;
		assetPairBefore = assetPairFrameBefore->getAssetPair();
		assetPairBefore.physicalPrice = physicalPrice;
		assetPairBefore.currentPrice = physicalPrice + premium;
		break;
	}
	case MANAGE_ASSET_PAIR_UPDATE_POLICIES:
		REQUIRE(assetPairFrameBefore);
		assetPairBefore = assetPairFrameBefore->getAssetPair();
		assetPairBefore.policies = policies;
		assetPairBefore.physicalPriceCorrection = physicalPriceCorrection;
		assetPairBefore.maxPriceStep = maxPriceStep;
		break;
	default:
		throw new std::runtime_error("Unexpected manage asset pair action");
	}
	
	REQUIRE(assetPairBefore == assetPairAfter);

}


// For base balance
TransactionFramePtr
createPaymentTx(Hash const& networkID, SecretKey& from, SecretKey& to,
                Salt seq, int64_t amount, PaymentFeeData paymentFee, bool isSourceFee, std::string subject, std::string reference, TimeBounds* timeBounds,
                    InvoiceReference* invoiceReference)
{
    return createPaymentTx(networkID, from, from.getPublicKey(), to.getPublicKey(), seq, amount,paymentFee,
        isSourceFee, subject, reference, timeBounds, invoiceReference);
}

TransactionFramePtr
createPaymentTx(Hash const& networkID, SecretKey& from, BalanceID fromBalanceID, BalanceID toBalanceID,
                Salt seq, int64_t amount, PaymentFeeData paymentFee, bool isSourceFee, std::string subject, std::string reference, TimeBounds* timeBounds,
                    InvoiceReference* invoiceReference)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
	op.body.paymentOp().feeData = paymentFee;
    op.body.paymentOp().subject = subject;
    op.body.paymentOp().sourceBalanceID = fromBalanceID;
    op.body.paymentOp().destinationBalanceID = toBalanceID;
    op.body.paymentOp().reference = reference;
    if (invoiceReference)
        op.body.paymentOp().invoiceReference.activate() = *invoiceReference;

    return transactionFromOperation(networkID, from, seq, op, nullptr, timeBounds);
}


// For base balance
PaymentResult
applyPaymentTx(Application& app, SecretKey& from, SecretKey& to,
               Salt seq, int64_t amount, PaymentFeeData paymentFee, bool isSourceFee,
               std::string subject, std::string reference, PaymentResultCode result,
               InvoiceReference* invoiceReference)
{
    return applyPaymentTx(app, from, from.getPublicKey(), to.getPublicKey(),
               seq, amount, paymentFee, isSourceFee, subject, reference, result, invoiceReference);
}

PaymentResult
applyPaymentTx(Application& app, SecretKey& from, BalanceID fromBalanceID,
        BalanceID toBalanceID, Salt seq, int64_t amount, PaymentFeeData paymentFee,
        bool isSourceFee, std::string subject, std::string reference, 
        PaymentResultCode result, InvoiceReference* invoiceReference)
{
    TransactionFramePtr txFrame;

    txFrame = createPaymentTx(app.getNetworkID(), from, fromBalanceID, toBalanceID,
        seq, amount, paymentFee, isSourceFee, subject, reference, nullptr, invoiceReference);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = PaymentOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);
    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    return txResult.result.results()[0].tr().paymentResult();
}


TransactionFramePtr
createManageForfeitRequestTx(Hash const &networkID, SecretKey &from, BalanceID fromBalance, Salt seq,
                             AccountID reviewer, int64_t amount, int64_t totalFee, std::string details)
{
    Operation op;
    op.body.type(MANAGE_FORFEIT_REQUEST);
    op.body.manageForfeitRequestOp().amount = amount;
    op.body.manageForfeitRequestOp().balance = fromBalance;
    op.body.manageForfeitRequestOp().details = details;
    op.body.manageForfeitRequestOp().totalFee = totalFee;
	op.body.manageForfeitRequestOp().reviewer = reviewer;

    return transactionFromOperation(networkID, from, seq, op);
}

ManageForfeitRequestResult
applyManageForfeitRequestTx(Application &app, SecretKey &from, BalanceID fromBalance, Salt seq, AccountID reviewer,
                            int64_t amount, int64_t totalFee, std::string details,
                            ManageForfeitRequestResultCode result)
{
    TransactionFramePtr txFrame;

    auto fromBalanceFrame = loadBalance(fromBalance, app, false);

    txFrame = createManageForfeitRequestTx(app.getNetworkID(), from, fromBalance, seq, reviewer, amount, totalFee,
                                           details);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());


    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = ManageForfeitRequestOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    auto fromBalanceAfterFrame = loadBalance(fromBalance, app, false);

    auto opResult = txResult.result.results()[0].tr().manageForfeitRequestResult();

    auto accountFrame = loadAccount(from, app, true);

    if (innerCode != MANAGE_FORFEIT_REQUEST_SUCCESS)
    {
		REQUIRE(fromBalanceFrame->getAmount() == fromBalanceAfterFrame->getAmount());            
    }
    else
    {
        REQUIRE(PaymentRequestFrame::loadPaymentRequest(opResult.success().paymentID, app.getDatabase()));
        return opResult;
    }
    return opResult;
}


TransactionFramePtr
createManageInvoice(Hash const& networkID, SecretKey& from, AccountID sender,
                BalanceID receiverBalance, int64_t amount, uint64_t invoiceID)
{
    Operation op;
    op.body.type(MANAGE_INVOICE);
    op.body.manageInvoiceOp().amount = amount;
    op.body.manageInvoiceOp().receiverBalance = receiverBalance;
    op.body.manageInvoiceOp().sender = sender;
    op.body.manageInvoiceOp().invoiceID = invoiceID;

    return transactionFromOperation(networkID, from, 0, op);
}

ManageInvoiceResult
applyManageInvoice(Application& app, SecretKey& from, AccountID sender,
                BalanceID receiverBalance, int64_t amount, uint64_t invoiceID,
                ManageInvoiceResultCode result)
{
    TransactionFramePtr txFrame;


    txFrame = createManageInvoice(app.getNetworkID(), from, sender, 
        receiverBalance, amount, invoiceID);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());


    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = ManageInvoiceOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    auto opResult = txResult.result.results()[0].tr().manageInvoiceResult();

    if (innerCode == MANAGE_INVOICE_SUCCESS)
    {
        if (invoiceID == 0)
        {
            auto createdInvoiceID = opResult.success().invoiceID;
            auto invoiceFrame = InvoiceFrame::loadInvoice(createdInvoiceID, app.getDatabase());
            REQUIRE(invoiceFrame);
            REQUIRE(invoiceFrame->getAmount() == amount);
            REQUIRE(invoiceFrame->getSender() == sender);
            REQUIRE(invoiceFrame->getReceiverBalance() == receiverBalance);
        }
        else
        {
            REQUIRE(!InvoiceFrame::loadInvoice(invoiceID, app.getDatabase()));
        }
    }

    return opResult;
}



TransactionFramePtr
createReviewPaymentRequestTx(Hash const& networkID, SecretKey& exchange, 
                Salt seq, int64 paymentID, bool accept)
{
    Operation op;
    op.body.type(REVIEW_PAYMENT_REQUEST);
    op.body.reviewPaymentRequestOp().paymentID = paymentID;
    op.body.reviewPaymentRequestOp().accept = accept;

    return transactionFromOperation(networkID, exchange, seq, op);
}

int32
applyReviewPaymentRequestTx(Application& app, SecretKey& from, Salt seq,
            int64 paymentID, bool accept, ReviewPaymentRequestResultCode result )
{
    TransactionFramePtr txFrame;


    txFrame = createReviewPaymentRequestTx(app.getNetworkID(), from, seq, paymentID, accept);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    auto requests = PaymentRequestFrame::countObjects(app.getDatabase().getSession());

    auto request = PaymentRequestFrame::loadPaymentRequest(paymentID, app.getDatabase(), nullptr);
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = ReviewPaymentRequestOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    auto newRequests = PaymentRequestFrame::countObjects(app.getDatabase().getSession());

    if (innerCode == REVIEW_PAYMENT_REQUEST_SUCCESS)
    {
        if (accept)
            REQUIRE(requests == newRequests + 1);
        REQUIRE(!PaymentRequestFrame::loadPaymentRequest(paymentID, app.getDatabase(), nullptr));
        return txResult.result.results()[0].tr().reviewPaymentRequestResult().reviewPaymentResponse().state;
    }
    else
    {
        REQUIRE(requests == newRequests);
        if (innerCode != REVIEW_PAYMENT_REQUEST_NOT_FOUND)
            REQUIRE(PaymentRequestFrame::loadPaymentRequest(paymentID, app.getDatabase(), nullptr));
        return -1;
    }
}

TransactionFramePtr createRecover(Hash const& networkID, SecretKey& source,
                                     Salt seq, AccountID account,
                                     PublicKey oldSigner, PublicKey newSigner)
{
    Operation op;
    op.body.type(RECOVER);
    op.body.recoverOp().account = account;
    op.body.recoverOp().oldSigner = oldSigner;
    op.body.recoverOp().newSigner = newSigner;
    

    return transactionFromOperation(networkID, source, seq, op);

}

void applyRecover(Application& app, SecretKey& source, Salt seq, AccountID account,
                    PublicKey oldSigner, PublicKey newSigner, RecoverResultCode targetResult)
{
    TransactionFramePtr txFrame;


    txFrame = createRecover(app.getNetworkID(), source, seq, account, oldSigner, newSigner);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    unsigned long signersSize = 0, signersSizeAfter = 0;
    auto acc = AccountFrame::loadAccount(account, app.getDatabase());
    assert(acc);
    signersSize = acc->getAccount().signers.size();
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = RecoverOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == targetResult);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());
    
    auto accAfter = AccountFrame::loadAccount(account, app.getDatabase());
    signersSizeAfter = accAfter->getAccount().signers.size();

    if (innerCode == RECOVER_SUCCESS)
    {
		REQUIRE(signersSizeAfter == 1);
		REQUIRE(accAfter->getMasterWeight() == 0);
    }
    else
    {
        REQUIRE(signersSizeAfter == signersSize);
    }

}

TransactionFramePtr
createSetOptions(Hash const& networkID, SecretKey& source, Salt seq, ThresholdSetter* thrs, Signer* signer, TrustData* trustData)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    SetOptionsOp& setOp = op.body.setOptionsOp();

    if (thrs)
    {
        if (thrs->masterWeight)
        {
            setOp.masterWeight.activate() = *thrs->masterWeight;
        }
        if (thrs->lowThreshold)
        {
            setOp.lowThreshold.activate() = *thrs->lowThreshold;
        }
        if (thrs->medThreshold)
        {
            setOp.medThreshold.activate() = *thrs->medThreshold;
        }
        if (thrs->highThreshold)
        {
            setOp.highThreshold.activate() = *thrs->highThreshold;
        }
    }

    if (signer)
    {
        setOp.signer.activate() = *signer;
    }

	if (trustData)
	{
		setOp.trustData.activate() = *trustData;
	}

    return transactionFromOperation(networkID, source, seq, op);
}

void
applySetOptions(Application& app, SecretKey& source, Salt seq,
                ThresholdSetter* thrs, Signer* signer, TrustData* trustData,
                SetOptionsResultCode result, SecretKey* txSiger)
{
    TransactionFramePtr txFrame;

    txFrame = createSetOptions(app.getNetworkID(), source, seq, thrs, signer, trustData);
	if (txSiger)
	{
		txFrame->getEnvelope().signatures.clear();
		txFrame->addSignature(*txSiger);
	}

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(SetOptionsOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createDirectDebitTx(Hash const& networkID, SecretKey& source, Salt seq, AccountID from,
	PaymentOp paymentOp)
{
	Operation op;
	op.body.type(DIRECT_DEBIT);
	op.body.directDebitOp().paymentOp = paymentOp;
	op.body.directDebitOp().from = from;

	return transactionFromOperation(networkID, source, seq, op);
}

DirectDebitResult
applyDirectDebitTx(Application& app, SecretKey& source, Salt seq,
	AccountID from, PaymentOp paymentOp, DirectDebitResultCode result)
{
	TransactionFramePtr txFrame;

	txFrame = createDirectDebitTx(app.getNetworkID(), source, seq, from, paymentOp);

	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());
	applyCheck(txFrame, delta, app);

	checkTransaction(*txFrame);
	auto txResult = txFrame->getResult();
	auto innerCode =
		DirectDebitOpFrame::getInnerCode(txResult.result.results()[0]);
	REQUIRE(innerCode == result);
	REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

	auto opResult = txResult.result.results()[0].tr().directDebitResult();

	return opResult;
}





TransactionFramePtr
createSetLimits(Hash const& networkID, SecretKey& source, Salt seq,
    AccountID* account, AccountType* accountType, Limits limits)
{
    Operation op;
    op.body.type(SET_LIMITS);

    SetLimitsOp& setLimitsOp = op.body.setLimitsOp();

    if (account)
        setLimitsOp.account.activate() = *account;
    if (accountType)
        setLimitsOp.accountType.activate() = *accountType;
    setLimitsOp.limits = limits;

    return transactionFromOperation(networkID, source, seq, op);
}

void
applySetLimits(Application& app, SecretKey& source, Salt seq,
                AccountID* account, AccountType* accountType, Limits limits,
                SetLimitsResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createSetLimits(app.getNetworkID(), source, seq,
        account, accountType, limits);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(SetLimitsOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}


TransactionFramePtr createCoinsEmissionRequest(Hash const& networkID, SecretKey& source, Salt seq,
    BalanceID receiver, uint64 requestID, int64 amount, AssetCode asset,
        std::string reference, ManageCoinsEmissionRequestAction action)
{
	Operation op;
	op.body.type(MANAGE_COINS_EMISSION_REQUEST);
	ManageCoinsEmissionRequestOp& request = op.body.manageCoinsEmissionRequestOp();
	request.requestID = requestID;
	request.amount = amount;
    request.action = action;
    request.asset = asset;
    request.reference = reference;
    request.receiver = receiver;
	return transactionFromOperation(networkID, source, seq, op);
}

uint64 applyCoinsEmissionRequest(Application& app, SecretKey& source, Salt seq, BalanceID receiver,
	int64 amount, uint64 requestID, AssetCode asset, std::string reference, ManageCoinsEmissionRequestAction action,
	ManageCoinsEmissionRequestResultCode targetResult, bool fulfilledAtOnce, int64_t* expectedAmount)
{
	TransactionFramePtr txFrame =
		createCoinsEmissionRequest(app.getNetworkID(), source, seq, receiver, requestID, amount, asset, reference, action);

	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());
	applyCheck(txFrame, delta, app);

	auto result = txFrame->getResult().result.results()[0].tr().manageCoinsEmissionRequestResult();

	REQUIRE(result.code() == targetResult);
    if (targetResult == MANAGE_COINS_EMISSION_REQUEST_SUCCESS)
    {
        auto request = CoinsEmissionRequestFrame::loadCoinsEmissionRequest(result.manageRequestInfo().requestID, app.getDatabase());
        if (action == MANAGE_COINS_EMISSION_REQUEST_CREATE)
        {
            REQUIRE(request);
            REQUIRE(request->getAsset() == asset);
            REQUIRE(request->getReference() == reference);
			if (expectedAmount == nullptr)
			{
				REQUIRE(request->getAmount() == amount);
			}
			else
			{
				REQUIRE(request->getAmount() == *expectedAmount);
			}
            REQUIRE(request->getReceiver() == receiver);
            REQUIRE(request->getIssuer() == source.getPublicKey());
            REQUIRE(request->getIsApproved() == fulfilledAtOnce);
        }
        else
        {
            REQUIRE(!request);
        }
        REQUIRE(result.manageRequestInfo().fulfilled == fulfilledAtOnce);
        return result.manageRequestInfo().requestID;
    }
	return 0;
}

TransactionFramePtr createReviewCoinsEmissionRequest(Hash const& networkID, SecretKey& source, Salt seq,
	CoinsEmissionRequestEntry request, bool isApproved, std::string reason)
{
	Operation op;
	op.body.type(REVIEW_COINS_EMISSION_REQUEST);
	auto& opBody = op.body.reviewCoinsEmissionRequestOp();
	opBody.request = request;
	opBody.approve = isApproved;
	opBody.reason = reason;
	return transactionFromOperation(networkID, source, seq, op);
}

void applyReviewCoinsEmissionRequest(Application& app, SecretKey& source, Salt seq,
	CoinsEmissionRequestEntry request, bool isApproved, std::string reason,
	ReviewCoinsEmissionRequestResultCode targetResult)
{
	TransactionFramePtr txFrame =
		createReviewCoinsEmissionRequest(app.getNetworkID(), source, seq, request, isApproved, reason);

	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());

    auto balanceBefore = getBalance(request.receiver, app);
	applyCheck(txFrame, delta, app);

	auto result = txFrame->getResult().result.results()[0].tr().reviewCoinsEmissionRequestResult();

	REQUIRE(result.code() == targetResult);

	if (targetResult != REVIEW_COINS_EMISSION_REQUEST_SUCCESS) {
		return;
	}

    auto balanceAfter = getBalance(request.receiver, app);
	auto storedRequest = loadCoinsEmissionRequest(request.requestID, app, false);
	if (!isApproved)
	{
		REQUIRE(!storedRequest);
        REQUIRE(balanceBefore == balanceAfter);
		return;
	}

    // manual emission
    if (request.requestID != 0)
	{
        REQUIRE(storedRequest);
        REQUIRE(storedRequest->getCoinsEmissionRequest().isApproved);
        REQUIRE(result.success().requestID == request.requestID);
    }
    REQUIRE(balanceBefore + request.amount == balanceAfter);
}


TransactionFramePtr createUploadPreemissions(Hash const& networkID, SecretKey& source, Salt seq,
	std::vector<PreEmission> preEmissions)
{
	Operation op;
	op.body.type(UPLOAD_PREEMISSIONS);
	auto& opBody = op.body.uploadPreemissionsOp();
    opBody.preEmissions.assign(preEmissions.begin(), preEmissions.end());
    assert(opBody.preEmissions.size() == preEmissions.size());
	return transactionFromOperation(networkID, source, seq, op);
}

void applyUploadPreemissions(Application& app, SecretKey& source, Salt seq,
	std::vector<PreEmission> preEmissions, UploadPreemissionsResultCode targetResult)
{
	TransactionFramePtr txFrame =
		createUploadPreemissions(app.getNetworkID(), source, seq, preEmissions);

	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());


    auto masterBalanceFrameBefore = BalanceFrame::loadBalance(app.getMasterID(),
            preEmissions.begin()->asset,
            app.getDatabase(), nullptr);


	applyCheck(txFrame, delta, app);

	auto result = txFrame->getResult().result.results()[0].tr().uploadPreemissionsResult();

	REQUIRE(result.code() == targetResult);

	if (targetResult != UPLOAD_PREEMISSIONS_SUCCESS) {
		return;
	}

    auto masterBalanceFrameAfter = BalanceFrame::loadBalance(app.getMasterID(),
            preEmissions.begin()->asset,
            app.getDatabase(), nullptr);

    uint64 total = 0;
    for(auto preEm : preEmissions)
    {
        total += preEm.amount;
    }
    REQUIRE(masterBalanceFrameBefore->getAmount() ==
        (masterBalanceFrameAfter->getAmount() - total));
}


TransactionFramePtr createManageAccount(Hash const& networkID, SecretKey& source, SecretKey& account, Salt seq, uint32 blockReasonsToAdd, uint32 blockReasonsToRemove, AccountType accountType)
{
	Operation op;
	op.body.type(MANAGE_ACCOUNT);
	auto& opBody = op.body.manageAccountOp();
	opBody.account = account.getPublicKey();
	opBody.blockReasonsToAdd = blockReasonsToAdd;
    opBody.blockReasonsToRemove = blockReasonsToRemove;
    opBody.accountType = accountType;
	return transactionFromOperation(networkID, source, seq, op);
}

void
applyManageAccountTx(Application& app, SecretKey& source, SecretKey& account, Salt seq, uint32 blockReasonsToAdd, uint32 blockReasonsToRemove, AccountType accountType,  ManageAccountResultCode targetResult)
{
	TransactionFramePtr txFrame = createManageAccount(app.getNetworkID(), source, account,
        seq, blockReasonsToAdd, blockReasonsToRemove, accountType);

	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());
    
    auto accountFrameBefore = loadAccount(account, app, false);

	applyCheck(txFrame, delta, app);

	auto result = txFrame->getResult().result.results()[0].tr().manageAccountResult();
	REQUIRE(result.code() == targetResult);
	if (result.code() == MANAGE_ACCOUNT_SUCCESS && accountFrameBefore)
	{
		auto accountFrameAfter = loadAccount(account, app);
        uint32 expectedReasons = (accountFrameBefore->getBlockReasons() | blockReasonsToAdd) & ~blockReasonsToRemove; 
        REQUIRE(accountFrameAfter->getBlockReasons() == expectedReasons);
        REQUIRE(accountFrameAfter->isBlocked() == (expectedReasons != 0));
        REQUIRE(result.success().blockReasons == expectedReasons);
	}
}

TransactionFramePtr createSetFees(Hash const& networkID, SecretKey& source, Salt seq, FeeEntry* fee, bool isDelete)
{
	Operation op;
	op.body.type(SET_FEES);
	auto& opBody = op.body.setFeesOp();
	if (fee)
		opBody.fee.activate() = *fee;
	opBody.isDelete = isDelete;

	return transactionFromOperation(networkID, source, seq, op);
}

void applySetFees(Application& app, SecretKey& source, Salt seq, FeeEntry* fee, bool isDelete, SecretKey* signer, SetFeesResultCode targetResult)
{
	auto txFrame = createSetFees(app.getNetworkID(), source, seq, fee, isDelete);
	if (signer)
	{
		txFrame->getEnvelope().signatures.clear();
		txFrame->addSignature(*signer);
	}

	LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
		app.getDatabase());

	applyCheck(txFrame, delta, app);

	auto result = txFrame->getResult().result.results()[0].tr().setFeesResult();
	REQUIRE(result.code() == targetResult);
	if (result.code() == SET_FEES_SUCCESS)
	{
		if (fee)
		{
			auto storedFee = FeeFrame::loadFee(fee->feeType, fee->asset,
				fee->accountID.get(), fee->accountType.get(), fee->subtype, fee->lowerBound, fee->upperBound, app.getDatabase(), nullptr);
			if (isDelete)
				REQUIRE(!storedFee);
			else {
				REQUIRE(storedFee);
				REQUIRE(storedFee->getFee() == *fee);
			}
		}
	}
}

CoinsEmissionRequestEntry makeCoinsEmissionRequest(PublicKey issuer, BalanceID receiver,
    uint64 requestID, int64 amount, AssetCode asset, std::string reference)
{
	CoinsEmissionRequestEntry result;
	result.amount = amount;
	result.requestID = requestID;
	result.isApproved = false;
	result.issuer = issuer;
    result.asset = asset;
    result.receiver = receiver;
    result.reference = reference;
	return result;
}

void uploadPreemissions(Application& app, SecretKey& source, SecretKey& issuance,
	Salt sourceSeq, int64 amount, AssetCode asset)
{
	int64 emittedAmount = 0;
	while (emittedAmount < amount) {
		std::vector<PreEmission> preEmissions;
		while (preEmissions.size() < app.getConfig().PREEMISSIONS_PER_OP && emittedAmount < amount)
		{
			auto preEmission = createPreEmission(issuance, app.getConfig().EMISSION_UNIT, SecretKey::random().getStrKeyPublic(), asset);
			emittedAmount += preEmission.amount;
			preEmissions.push_back(preEmission);
		}

		applyUploadPreemissions(app, source, sourceSeq++, preEmissions);
	}
}

void fundAccount(Application& app, SecretKey& source, SecretKey& issuance,
    Salt& sourceSeq, BalanceID to, int64 amount, AssetCode asset)
{
	uploadPreemissions(app, source, issuance, sourceSeq, amount, asset);
	auto validRequestEntry = makeCoinsEmissionRequest(source.getPublicKey(), to, 0, amount, asset);
	applyReviewCoinsEmissionRequest(app, source, sourceSeq++, validRequestEntry, true, "");
}

TransactionFramePtr createFundAccount(Hash const& networkID, SecretKey& source, SecretKey& issuance, Salt& sourceSeq, BalanceID to, int64 amount, AssetCode asset, uint32_t preemissionsPerOp, uint32_t emissionUnit, TimeBounds* timeBounds) {
	//auto check = (amount % emissionUnit) == 0;
	//REQUIRE(check);
	int64 emittedAmount = 0;
	auto validRequestEntry = makeCoinsEmissionRequest(source.getPublicKey(), to, 0, amount, asset);
	auto resultingEnvelope = createReviewCoinsEmissionRequest(networkID, source, sourceSeq++, validRequestEntry, true, "")->getEnvelope();
	if (timeBounds) {
		resultingEnvelope.tx.timeBounds = *timeBounds;
	}

	while (emittedAmount < amount) {
		std::vector<PreEmission> preEmissions;
		while (preEmissions.size() < preemissionsPerOp && emittedAmount < amount)
		{
			auto preEmission = createPreEmission(issuance, emissionUnit, SecretKey::random().getStrKeyPublic(), asset);
			emittedAmount += preEmission.amount;
			preEmissions.push_back(preEmission);
		}

		TransactionFramePtr txFrame =
			createUploadPreemissions(networkID, source, sourceSeq++, preEmissions);
		resultingEnvelope.tx.operations.push_back(txFrame->getEnvelope().tx.operations[0]);
	}

	TransactionFramePtr tx =
		TransactionFrame::makeTransactionFromWire(
			networkID, resultingEnvelope);

	tx->getEnvelope().signatures.clear();
	tx->addSignature(source);
	return tx;
}


OperationFrame const&
getFirstOperationFrame(TransactionFrame const& tx)
{
    return *(tx.getOperations()[0]);
}

OperationResult const&
getFirstResult(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResult();
}

OperationResultCode
getFirstResultCode(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResultCode();
}

Operation&
getFirstOperation(TransactionFrame& tx)
{
    return tx.getEnvelope().tx.operations[0];
}

void
reSignTransaction(TransactionFrame& tx, SecretKey& source)
{
    tx.getEnvelope().signatures.clear();
    tx.addSignature(source);
}

void
checkAmounts(int64_t a, int64_t b, int64_t maxd)
{
    int64_t d = b - maxd;
    REQUIRE(a >= d);
    REQUIRE(a <= b);
}

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected)
{
    REQUIRE(r[index].first.result.result.code() == expected);
};

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
        OperationResultCode code)
{
    checkTx(index, r, expected);
    REQUIRE(r[index].first.result.result.results()[0].code() == code);
};

PreEmission createPreEmission(SecretKey signer, int64_t amount, std::string serialNumber, AssetCode asset)
{
	auto preEmission = PreEmission();
	preEmission.serialNumber = serialNumber;
	preEmission.amount = amount;
    preEmission.asset = asset;
	auto data = std::string(serialNumber) + ":" + 
        std::to_string(preEmission.amount) + ":" + std::string(asset);
	DecoratedSignature decoratedSignature;
	decoratedSignature.signature = signer.sign(sha256(data));
	decoratedSignature.hint = PubKeyUtils::getHint(signer.getPublicKey());
	preEmission.signatures.push_back(decoratedSignature);
	return preEmission;
}

}
}