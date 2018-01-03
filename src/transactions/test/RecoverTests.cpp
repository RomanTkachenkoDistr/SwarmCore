// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "ledger/AccountHelper.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "TxTests.h"
#include "ledger/LedgerDelta.h"
#include "crypto/SHA.h"
#include "test/test_marshaler.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("Recover", "[dep_tx][recover]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_POSTGRESQL);

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();
    closeLedgerOn(app, 2, 1, 7, 2014);

    // set up world
    SecretKey root = getRoot();
	Salt rootSeq = 1;

    auto accountA = SecretKey::random();

    SecretKey s1 = getAccount("S1");
    SecretKey s2 = getAccount("S2");

	auto accountHelper = AccountHelper::Instance();

	SECTION("basics")
	{
		applyCreateAccountTx(app, root, accountA, rootSeq++, AccountType::GENERAL);

		SECTION("recover to the same pubkey")
		{
			applyRecover(app, root, rootSeq++, accountA.getPublicKey(), accountA.getPublicKey(), accountA.getPublicKey(), RecoverResultCode::MALFORMED);
		}
		SECTION("Remove only recovery flag")
		{
			applyManageAccountTx(app, root, accountA, 0,
								 static_cast<int32_t>(BlockReasons::KYC_UPDATE) |
 			   					 static_cast<int32_t>(BlockReasons::RECOVERY_REQUEST), 0);
			applyRecover(app, root, rootSeq++, accountA.getPublicKey(), accountA.getPublicKey(), s1.getPublicKey());
			auto accAfter = accountHelper->loadAccount(accountA.getPublicKey(), app.getDatabase());
			REQUIRE(accAfter->getBlockReasons() == static_cast<int32_t>(BlockReasons::KYC_UPDATE));
		}
		SECTION("change master signer to new signer")
		{
			auto acc = accountHelper->loadAccount(accountA.getPublicKey(), app.getDatabase());
			REQUIRE(acc->getMasterWeight() == 1);

			applyRecover(app, root, rootSeq++, accountA.getPublicKey(), accountA.getPublicKey(), s1.getPublicKey());
			auto accAfter = accountHelper->loadAccount(accountA.getPublicKey(), app.getDatabase());
			REQUIRE(accAfter->getMasterWeight() == 0);
			auto signers = accAfter->getAccount().signers;
			REQUIRE(signers[0].identity == 0);
			REQUIRE(signers[0].weight == 1);
			REQUIRE(signers[0].pubKey == s1.getPublicKey());
			REQUIRE(signers[0].signerType == getAnySignerType());
			ThresholdSetter th;
			th.masterWeight = make_optional<uint8_t>(100);
			applySetOptions(app, accountA, 1, &th, nullptr, nullptr, SetOptionsResultCode::SUCCESS, &s1);
		}

		SECTION("Remove all old signers and add new with high threshold")
		{
			auto signerType = getAnySignerType();
			Signer sk1(s1.getPublicKey(), 3, signerType, 1, "", Signer::_ext_t{});
			auto aSeq = 1;
			applySetOptions(app, accountA, aSeq++, nullptr, &sk1);
			auto s2 = SecretKey::random();
			Signer sk2(s2.getPublicKey(), 3, signerType, 2, "", Signer::_ext_t{});
			applySetOptions(app, accountA, aSeq++, nullptr, &sk2);
			ThresholdSetter th;
			th.highThreshold = make_optional<uint8_t>(100);
			applySetOptions(app, accountA, 1, &th, nullptr, nullptr, SetOptionsResultCode::SUCCESS, &s1);

			auto accountAFrame = accountHelper->loadAccount(accountA.getPublicKey(), app.getDatabase());
			REQUIRE(accountAFrame->getMasterWeight() == 1);
			REQUIRE(accountAFrame->getAccount().signers.size() == 2);

			applyRecover(app, root, rootSeq++, accountA.getPublicKey(), s1.getPublicKey(), s2.getPublicKey());
			accountAFrame = accountHelper->loadAccount(accountA.getPublicKey(), app.getDatabase());
			auto signers = accountAFrame->getAccount().signers;
			REQUIRE(accountAFrame->getMasterWeight() == 0);
			REQUIRE(signers.size() == 1);
			REQUIRE(signers[0].identity == 0);
			REQUIRE(signers[0].weight == *th.highThreshold.get());
			REQUIRE(signers[0].pubKey == s2.getPublicKey());
			REQUIRE(signers[0].signerType == getAnySignerType());
		}
	}
}
