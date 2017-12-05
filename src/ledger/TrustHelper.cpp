//
// Created by kirill on 04.12.17.
//

#include <lib/xdrpp/xdrpp/printer.h>
#include "TrustHelper.h"
#include "LedgerDelta.h"

using namespace soci;
using namespace std;

namespace stellar {
    using xdr::operator<;

    void TrustHelper::dropAll(Database &db) {
        db.getSession() << "DROP TABLE IF EXISTS trusts;";
        db.getSession() << "CREATE TABLE trusts"
                "("
                "allowed_account       VARCHAR(64)  NOT NULL,"
                "balance_to_use        VARCHAR(64)  NOT NULL,"
                "lastmodified          INT          NOT NULL,"
                "version               INT          NOT NULL DEFAULT 0,"
                "PRIMARY KEY (balance_to_use, allowed_account)"
                ");";

    }

    void TrustHelper::storeAdd(LedgerDelta &delta, Database &db, LedgerEntry const &entry) {
        storeUpdateHelper(delta, db, true, entry);
    }

    void TrustHelper::storeChange(LedgerDelta &delta, Database &db, LedgerEntry const &entry) {
        storeUpdateHelper(delta, db, false, entry);
    }

    void TrustHelper::storeDelete(LedgerDelta &delta, Database &db, LedgerKey const &key) {
        flushCachedEntry(key, db);

        std::string actIDStrKey = PubKeyUtils::toStrKey(key.trust().allowedAccount);
        std::string balIDStrKey = BalanceKeyUtils::toStrKey(key.trust().balanceToUse);
        {
            auto timer = db.getDeleteTimer("trusts");
            auto prep = db.getPreparedStatement(
                    "DELETE from trusts where allowed_account=:v1 AND balance_to_use=:v2");
            auto &st = prep.statement();
            st.exchange(soci::use(actIDStrKey));
            st.exchange(soci::use(balIDStrKey));

            st.define_and_bind();
            st.execute(true);
        }
        delta.deleteEntry(key);
    }

    bool TrustHelper::exists(Database &db, LedgerKey const &key) {
        return TrustHelper::exists(db, key.trust().allowedAccount, key.trust().balanceToUse);
    }

    LedgerKey TrustHelper::getLedgerKey(LedgerEntry const &from) {
        LedgerKey ledgerKey;
        ledgerKey.type(from.data.type());
        ledgerKey.trust().balanceToUse = from.data.trust().balanceToUse;
        ledgerKey.trust().allowedAccount = from.data.trust().allowedAccount;
        ledgerKey.trust().ext.v(from.data.trust().ext.v());
        return ledgerKey;
    }

    EntryFrame::pointer TrustHelper::storeLoad(LedgerKey const &key, Database &db) {
        return loadTrust(key.trust().allowedAccount, key.trust().balanceToUse, db);
    }

    EntryFrame::pointer TrustHelper::fromXDR(LedgerEntry const &from) {
        return std::make_shared<TrustFrame>(from);
    }

    uint64_t TrustHelper::countObjects(soci::session &sess) {
        uint64_t count = 0;
        sess << "SELECT COUNT(*) FROM trusts;", into(count);
        return count;
    }

    bool TrustHelper::exists(Database &db, AccountID allowedAccount, BalanceID balanceToUse) {
        std::string actIDStrKey = PubKeyUtils::toStrKey(allowedAccount);
        std::string balIDStrKey = BalanceKeyUtils::toStrKey(balanceToUse);

        int exists = 0;
        {
            auto timer = db.getSelectTimer("Trust-exists");
            auto prep =
                    db.getPreparedStatement(
                            "SELECT EXISTS (SELECT NULL FROM trusts WHERE allowed_account=:v1 AND balance_to_use=:v2)");
            auto &st = prep.statement();
            st.exchange(use(actIDStrKey));
            st.exchange(use(balIDStrKey));
            st.exchange(into(exists));
            st.define_and_bind();
            st.execute(true);
        }
        return exists != 0;
    }

    TrustFrame::pointer
    TrustHelper::loadTrust(AccountID const &allowedAccount, BalanceID const &balanceToUse, Database &db) {
        LedgerKey key;
        key.type(LedgerEntryType::TRUST);
        key.trust().allowedAccount = allowedAccount;
        key.trust().balanceToUse = balanceToUse;
        if (cachedEntryExists(key, db)) {
            auto p = getCachedEntry(key, db);
            return p ? std::make_shared<TrustFrame>(*p) : nullptr;
        }

        std::string actIDStrKey = PubKeyUtils::toStrKey(allowedAccount);
        std::string balIDStrKey = BalanceKeyUtils::toStrKey(balanceToUse);

        TrustFrame::pointer res = make_shared<TrustFrame>();
        TrustEntry &trust = res->getTrust();

        auto prep =
                db.getPreparedStatement("SELECT lastmodified "
                                                "FROM   trusts "
                                                "WHERE  allowed_account=:v1 AND balance_to_use=:v2");
        auto &st = prep.statement();
        st.exchange(into(res->getLastModified()));
        st.exchange(use(actIDStrKey));
        st.exchange(use(balIDStrKey));
        st.define_and_bind();
        {
            auto timer = db.getSelectTimer("trusts");
            st.execute(true);
        }

        if (!st.got_data()) {
            putCachedEntry(key, nullptr, db);
            return nullptr;
        }

        trust.allowedAccount = PubKeyUtils::fromStrKey(actIDStrKey);
        trust.balanceToUse = BalanceKeyUtils::fromStrKey(balIDStrKey);

        assert(res->isValid());
        putCachedEntry(key, &trust, db);
        return res;
    }

    uint64_t TrustHelper::countForBalance(Database &db, BalanceID balanceToUser) {
        uint64_t total = 0;
        auto timer = db.getSelectTimer("balance-count");
        auto prep =
                db.getPreparedStatement("SELECT COUNT(*) FROM trusts "
                                                "WHERE balance_to_use=:receiver;");
        auto& st = prep.statement();

        std::string actIDStrKey;
        actIDStrKey = BalanceKeyUtils::toStrKey(balanceToUser);

        st.exchange(use(actIDStrKey));
        st.exchange(into(total));
        st.define_and_bind();
        st.execute(true);

        return total;
    }
}