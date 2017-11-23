#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include <functional>
#include <unordered_map>

namespace soci
{
class session;
}

namespace stellar
{
class StatementContext;

class InvoiceFrame : public EntryFrame
{
    static void
    loadInvoices(StatementContext& prep,
               std::function<void(LedgerEntry const&)> InvoiceProcessor);

    InvoiceEntry& mInvoice;

    InvoiceFrame(InvoiceFrame const& from);

    void storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert);

  public:
    typedef std::shared_ptr<InvoiceFrame> pointer;

    InvoiceFrame();
    InvoiceFrame(LedgerEntry const& from);

    InvoiceFrame& operator=(InvoiceFrame const& other);

    EntryFrame::pointer
    copy() const override
    {
        return EntryFrame::pointer(new InvoiceFrame(*this));
    }

    InvoiceEntry const&
    getInvoice() const
    {
        return mInvoice;
    }
    InvoiceEntry&
    getInvoice()
    {
        return mInvoice;
    }

    int64_t
    getAmount()
    {
        return mInvoice.amount;
    }

    uint64_t
    getID()
    {
        return mInvoice.invoiceID;
    }

    AccountID
    getSender()
    {
        return mInvoice.sender;
    }

    BalanceID
    getReceiverBalance()
    {
        return mInvoice.receiverBalance;
    }

    InvoiceState
    getState()
    {
        return mInvoice.state;
    }

    void
    setState(InvoiceState state)
    {
        mInvoice.state = state;
    }

    static bool isValid(InvoiceEntry const& oe);
    bool isValid() const;

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) override;
    void storeAdd(LedgerDelta& delta, Database& db) override;

    // Static helpers that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);

    static int64 countForReceiverAccount(Database& db, AccountID account);

	static bool exists(Database& db, LedgerKey const& key);
	static bool exists(Database& db, int64 paymentID, AccountID exchange);
    static uint64_t countObjects(soci::session& sess);

    // database utilities
    static pointer loadInvoice(int64 invoiceID, Database& db, LedgerDelta* delta = nullptr);
    static void loadInvoices(AccountID const& accountID,
                       std::vector<InvoiceFrame::pointer>& retInvoices,
                       Database& db);
    static void dropAll(Database& db);
    static const char* kSQLCreateStatement1;
};
}