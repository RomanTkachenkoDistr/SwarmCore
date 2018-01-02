#include <ledger/AccountHelper.h>
#include <transactions/CreateAccountOpFrame.h>
#include <ledger/StatisticsHelper.h>
#include <ledger/BalanceHelper.h>
#include "CreateAccountTestHelper.h"

namespace stellar {
    namespace txtest {

        CreateAccountTestHelper::CreateAccountTestHelper(TestManager::pointer testManager) : TxHelper(testManager) {
        }

        Operation CreateAccountTestBuilder::buildOp() {
            Operation op;
            op.body.type(OperationType::CREATE_ACCOUNT);
            CreateAccountOp &createAccountOp = op.body.createAccountOp();
            createAccountOp.accountType = accountType;
            createAccountOp.destination = to;

            if (policies != -1)
                createAccountOp.policies = policies;
            if (referrer)
                createAccountOp.referrer.activate() = *referrer;
            return op;
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setFromAccount(Account from) {
            auto newTestHelper = *this;
            newTestHelper.source = from;
            return newTestHelper;
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setToPublicKey(PublicKey to) {
            auto newTestHelper = *this;
            newTestHelper.to = to;
            return newTestHelper;
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setType(AccountType accountType) {
            auto newTestHelper = *this;
            newTestHelper.accountType = accountType;
            return newTestHelper;
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setType(int32_t accountType) {
            return setType(static_cast<AccountType>(accountType));
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setSigner(Account *signer) {
            auto newTestHelper = *this;
            newTestHelper.signer = signer;
            return newTestHelper;
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setReferrer(AccountID *referrer) {
            auto newTestHelper = *this;
            newTestHelper.referrer = referrer;
            return newTestHelper;
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setPolicies(int32 policies) {
            auto newTestHelper = *this;
            newTestHelper.policies = policies;
            return newTestHelper;
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setPolicies(AccountPolicies policies) {
            return setPolicies(static_cast<int32_t>(policies));
        }

        CreateAccountTestBuilder CreateAccountTestBuilder::setResultCode(CreateAccountResultCode expectedResult) {
            auto newTestHelper = *this;
            newTestHelper.expectedResult = expectedResult;
            return newTestHelper;
        }

        CreateAccountResultCode
        CreateAccountTestHelper::applyCreateAccountTx(Account &from, PublicKey to, AccountType accountType,
                                                      Account *signer, AccountID *referrer, int32 policies,
                                                      CreateAccountResultCode expectedResult) {
            auto builder = CreateAccountTestBuilder()
                    .setFromAccount(from)
                    .setToPublicKey(to)
                    .setType(accountType)
                    .setSigner(signer)
                    .setReferrer(referrer)
                    .setPolicies(policies)
                    .setResultCode(expectedResult);
            return applyTx(builder);
        }

        CreateAccountResultCode CreateAccountTestHelper::applyTx(CreateAccountTestBuilder builder) {
            auto txFrame = builder.buildTx(mTestManager);
            mTestManager->applyCheck(txFrame);
            auto txResult = txFrame->getResult();
            auto opResult = txResult.result.results()[0];
            auto actualResultCode = CreateAccountOpFrame::getInnerCode(opResult);

            mustEqualsResultCode<CreateAccountResultCode>(actualResultCode, expectedResult);
            REQUIRE(txResult.feeCharged == mTestManager->getApp().getLedgerManager().getTxFee());

            auto checker = CreateAccountChecker(mTestManager);
            checker.doCheck(builder, actualResultCode);
            return actualResultCode;
        }

        void
        CreateAccountChecker::doCheck(CreateAccountTestBuilder builder,
            CreateAccountResultCode actualResultCode) {
            Database& db = mTestManager->getDB();

            auto accountHelper = AccountHelper::Instance();
            AccountFrame::pointer fromAccount = accountHelper->loadAccount(builder.source.key.getPublicKey(), db);
            AccountFrame::pointer toAccount = accountHelper->loadAccount(builder.to, db);

            AccountFrame::pointer toAccountAfter = accountHelper->loadAccount(builder.to, db);

            if (actualResultCode != CreateAccountResultCode::SUCCESS)
            {
                // check that the target account didn't change
                REQUIRE(!!toAccount == !!toAccountAfter);
                if (toAccount && toAccountAfter)
                {
                    REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
                }

                return;
            }
            REQUIRE(toAccountAfter);
            REQUIRE(!toAccountAfter->isBlocked());
            REQUIRE(toAccountAfter->getAccountType() == builder.accountType);

            auto statisticsHelper = StatisticsHelper::Instance();
            auto statisticsFrame = statisticsHelper->loadStatistics(builder.to, db);
            REQUIRE(statisticsFrame);
            auto statistics = statisticsFrame->getStatistics();
            REQUIRE(statistics.dailyOutcome == 0);
            REQUIRE(statistics.weeklyOutcome == 0);
            REQUIRE(statistics.monthlyOutcome == 0);
            REQUIRE(statistics.annualOutcome == 0);

            if (!toAccount)
            {
                auto balanceHelper = BalanceHelper::Instance();
                std::vector<BalanceFrame::pointer> balances;
                balanceHelper->loadBalances(toAccountAfter->getAccount().accountID, balances, db);
                for (const auto& balance : balances)
                {
                    REQUIRE(balance->getBalance().amount == 0);
                    REQUIRE(balance->getAccountID() == toAccountAfter->getAccount().accountID);
                }
            }

        }

        CreateAccountChecker::CreateAccountChecker(TestManager::pointer testManager) : mTestManager(testManager) {
        }
    }
}


