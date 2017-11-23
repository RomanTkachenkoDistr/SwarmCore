// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerDelta.h"
#include "xdr/Stellar-ledger.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "xdrpp/printer.h"

namespace stellar
{
using xdr::operator==;

LedgerDelta::LedgerDelta(LedgerDelta& outerDelta)
    : mOuterDelta(&outerDelta)
    , mHeader(&outerDelta.getHeader())
    , mCurrentHeader(outerDelta.getHeader())
    , mPreviousHeaderValue(outerDelta.getHeader())
    , mDb(outerDelta.mDb)
    , mUpdateLastModified(outerDelta.mUpdateLastModified)
{
}

LedgerDelta::LedgerDelta(LedgerHeader& header, Database& db,
                         bool updateLastModified)
    : mOuterDelta(nullptr)
    , mHeader(&header)
    , mCurrentHeader(header)
    , mPreviousHeaderValue(header)
    , mDb(db)
    , mUpdateLastModified(updateLastModified)
{
}

LedgerDelta::~LedgerDelta()
{
    if (mHeader)
    {
        rollback();
    }
}

LedgerHeader&
LedgerDelta::getHeader()
{
    return mCurrentHeader.mHeader;
}

LedgerHeader const&
LedgerDelta::getHeader() const
{
    return mCurrentHeader.mHeader;
}

LedgerHeaderFrame&
LedgerDelta::getHeaderFrame()
{
    return mCurrentHeader;
}

void
LedgerDelta::checkState()
{
    if (mHeader == nullptr)
    {
        throw std::runtime_error(
            "Invalid operation: delta is already committed");
    }
}

void
LedgerDelta::addEntry(EntryFrame const& entry)
{
    addEntry(entry.copy());
}

void
LedgerDelta::deleteEntry(EntryFrame const& entry)
{
    deleteEntry(entry.copy());
}

void
LedgerDelta::modEntry(EntryFrame const& entry)
{
    modEntry(entry.copy());
}

void
LedgerDelta::recordEntry(EntryFrame const& entry)
{
    recordEntry(entry.copy());
}

void
LedgerDelta::addEntry(EntryFrame::pointer entry)
{
    checkState();
    auto k = entry->getKey();
    auto del_it = mDelete.find(k);
    if (del_it != mDelete.end())
    {
        // delete + new is an update
        mDelete.erase(del_it);
        mMod[k] = entry;
    }
    else
    {
        assert(mNew.find(k) == mNew.end()); // double new
        assert(mMod.find(k) == mMod.end()); // mod + new is invalid
        mNew[k] = entry;
    }
}

void
LedgerDelta::deleteEntry(EntryFrame::pointer entry)
{
    auto k = entry->getKey();
    deleteEntry(k);
}

void
LedgerDelta::deleteEntry(LedgerKey const& k)
{
    checkState();
    auto new_it = mNew.find(k);
    if (new_it != mNew.end())
    {
        // new + delete -> don't add it in the first place
        mNew.erase(new_it);
    }
    else
    {
        if(mDelete.find(k) == mDelete.end())
        {
            mDelete.insert(k);
        }// else already being deleted

        mMod.erase(k);
    }
}

void
LedgerDelta::modEntry(EntryFrame::pointer entry)
{
    checkState();
    auto k = entry->getKey();
    auto mod_it = mMod.find(k);
    if (mod_it != mMod.end())
    {
        // collapse mod
        mod_it->second = entry;
    }
    else
    {
        auto new_it = mNew.find(k);
        if (new_it != mNew.end())
        {
            // new + mod = new (with latest value)
            new_it->second = entry;
        }
        else
        {
            assert(mDelete.find(k) == mDelete.end()); // delete + mod is illegal
            mMod[k] = entry;
        }
    }
}

void
LedgerDelta::recordEntry(EntryFrame::pointer entry)
{
    checkState();
    // keeps the old one around
    mPrevious.insert(std::make_pair(entry->getKey(), entry));
}

void
LedgerDelta::mergeEntries(LedgerDelta& other)
{
    checkState();

    // propagates mPrevious for deleted & modified entries
    for (auto& d : other.mDelete)
    {
        deleteEntry(d);
        auto it = other.mPrevious.find(d);
        if (it != other.mPrevious.end())
        {
            recordEntry(*it->second);
        }
    }
    for (auto& n : other.mNew)
    {
        addEntry(n.second);
    }
    for (auto& m : other.mMod)
    {
        modEntry(m.second);
        auto it = other.mPrevious.find(m.first);
        if (it != other.mPrevious.end())
        {
            recordEntry(*it->second);
        }
    }
}

void
LedgerDelta::commit()
{
    checkState();
    // checks if we about to override changes that were made
    // outside of this LedgerDelta
    if (!(mPreviousHeaderValue == *mHeader))
    {
        throw std::runtime_error("unexpected header state");
    }

    if (mOuterDelta)
    {
        mOuterDelta->mergeEntries(*this);
        mOuterDelta = nullptr;
    }
    *mHeader = mCurrentHeader.mHeader;
    mHeader = nullptr;
}

void
LedgerDelta::rollback()
{
    checkState();
    mHeader = nullptr;

    for (auto& d : mDelete)
    {
        EntryFrame::flushCachedEntry(d, mDb);
    }
    for (auto& n : mNew)
    {
        EntryFrame::flushCachedEntry(n.first, mDb);
    }
    for (auto& m : mMod)
    {
        EntryFrame::flushCachedEntry(m.first, mDb);
    }
}

void
LedgerDelta::addCurrentMeta(LedgerEntryChanges& changes,
                            LedgerKey const& key) const
{
    auto it = mPrevious.find(key);
    if (it != mPrevious.end())
    {
        // if the old value is from a previous ledger we emit it
        auto const& e = it->second->mEntry;
        if (e.lastModifiedLedgerSeq != mCurrentHeader.mHeader.ledgerSeq)
        {
            changes.emplace_back(LEDGER_ENTRY_STATE);
            changes.back().state() = e;
        }
    }
}

LedgerEntryChanges
LedgerDelta::getChanges() const
{
    LedgerEntryChanges changes;

    for (auto const& k : mNew)
    {
        changes.emplace_back(LEDGER_ENTRY_CREATED);
        changes.back().created() = k.second->mEntry;
    }
    for (auto const& k : mMod)
    {
        addCurrentMeta(changes, k.first);
        changes.emplace_back(LEDGER_ENTRY_UPDATED);
        changes.back().updated() = k.second->mEntry;
    }

    for (auto const& k : mDelete)
    {
        addCurrentMeta(changes, k);
        changes.emplace_back(LEDGER_ENTRY_REMOVED);
        changes.back().removed() = k;
    }

    return changes;
}

std::vector<LedgerEntry>
LedgerDelta::getLiveEntries() const
{
    std::vector<LedgerEntry> live;

    live.reserve(mNew.size() + mMod.size());

    for (auto const& k : mNew)
    {
        live.push_back(k.second->mEntry);
    }
    for (auto const& k : mMod)
    {
        live.push_back(k.second->mEntry);
    }

    return live;
}

std::vector<LedgerKey>
LedgerDelta::getDeadEntries() const
{
    std::vector<LedgerKey> dead;

    dead.reserve(mDelete.size());

    for (auto const& k : mDelete)
    {
        dead.push_back(k);
    }
    return dead;
}

bool
LedgerDelta::updateLastModified() const
{
    return mUpdateLastModified;
}

void
LedgerDelta::markMeters(Application& app) const
{
    for (auto const& ke : mNew)
    {
        switch (ke.first.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "add"}, "entry")
                .Mark();
            break;
		case COINS_EMISSION_REQUEST:
			app.getMetrics()
				.NewMeter({ "ledger", "coins-emission-request", "add" }, "entry")
				.Mark();
			break;
        case FEE:
            app.getMetrics()
            .NewMeter({"ledger", "fee", "add"},"entry")
            .Mark();
            break;
        case COINS_EMISSION:
            app.getMetrics()
            .NewMeter({"ledger", "coins-emission", "add"}, "entry")
            .Mark();
            break;
        case BALANCE:
            app.getMetrics()
            .NewMeter({"ledger", "balance", "add"}, "entry")
            .Mark();
            break;
        case PAYMENT_REQUEST:
            app.getMetrics()
            .NewMeter({"ledger", "payment-request", "add"}, "entry")
            .Mark();
            break;
        case ASSET:
            app.getMetrics()
            .NewMeter({"ledger", "asset", "add"}, "entry")
            .Mark();
            break;
        case ACCOUNT_TYPE_LIMITS:
            app.getMetrics()
            .NewMeter({"ledger", "account-type-limits", "add"}, "entry")
            .Mark();
            break;
        case STATISTICS:
            app.getMetrics()
            .NewMeter({"ledger", "statistics", "add"}, "entry")
            .Mark();
            break;
        }
    }

    for (auto const& ke : mMod)
    {
        switch (ke.first.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "modify"}, "entry")
                .Mark();
            break;
		case COINS_EMISSION_REQUEST:
			app.getMetrics()
				.NewMeter({ "ledger", "coins-emission-request", "modify" }, "entry")
				.Mark();
			break;
        case FEE:
            app.getMetrics()
                .NewMeter({"ledger", "fee", "modify"}, "entry");
            break;
        case COINS_EMISSION:
            app.getMetrics()
                .NewMeter({"ledger", "coins-emission", "modify"}, "entry");
            break;
        case BALANCE:
            app.getMetrics()
                .NewMeter({"ledger", "balance", "modify"}, "entry");
            break;
        case PAYMENT_REQUEST:
            app.getMetrics()
                .NewMeter({"ledger", "payment-request", "modify"}, "entry");
            break;
        case ASSET:
            app.getMetrics()
                .NewMeter({"ledger", "asset", "modify"}, "entry");
            break;
        case ACCOUNT_TYPE_LIMITS:
            app.getMetrics()
                .NewMeter({"ledger", "account-type-limits", "modify"}, "entry");
            break;
        case STATISTICS:
            app.getMetrics()
                .NewMeter({"ledger", "statistics", "modify"}, "entry");
            break;
        }
    }

    for (auto const& ke : mDelete)
    {
        switch (ke.type())
        {
        case ACCOUNT:
            app.getMetrics()
                .NewMeter({"ledger", "account", "delete"}, "entry")
                .Mark();
            break;
		case COINS_EMISSION_REQUEST:
			app.getMetrics()
				.NewMeter({ "ledger", "coins-emission-request", "delete" }, "entry")
				.Mark();
			break;
        case FEE:
            app.getMetrics()
                .NewMeter({"ledger", "fee", "delete"}, "entry")
                .Mark();
            break;
        case COINS_EMISSION:
            app.getMetrics()
                .NewMeter({"ledger", "coins-emission", "delete"}, "entry")
                .Mark();
            break;
        case BALANCE:
            app.getMetrics()
                .NewMeter({"ledger", "balance", "delete"}, "entry")
                .Mark();
            break;
        case PAYMENT_REQUEST:
            app.getMetrics()
                .NewMeter({"ledger", "payment-request", "delete"}, "entry")
                .Mark();
            break;
        case ASSET:
            app.getMetrics()
                .NewMeter({"ledger", "asset", "delete"}, "entry")
                .Mark();
            break;
        }
    }
}

void
LedgerDelta::checkAgainstDatabase(Application& app) const
{
    if (!app.getConfig().PARANOID_MODE)
    {
        return;
    }
    auto& db = app.getDatabase();
    auto live = getLiveEntries();
    for (auto const& l : live)
    {
        EntryFrame::checkAgainstDatabase(l, db);
    }
    auto dead = getDeadEntries();
    for (auto const& d : dead)
    {
        if (EntryFrame::exists(db, d))
        {
            std::string s;
            s = "Inconsistent state ; entry should not exist in database: ";
            s += xdr::xdr_to_string(d);
            throw std::runtime_error(s);
        }
    }
}
}