//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2015 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/LedgerToJson.h>

namespace ripple {

template <typename Object>
void fillJson (Object& json, LedgerFill const& fill)
{
    using namespace ripple::RPC;

    auto& ledger = fill.ledger;

    bool const bFull (fill.options & LedgerFill::full);
    bool const bExpand (fill.options & LedgerFill::expand);
    bool const bBinary (fill.options & LedgerFill::binary);

    // DEPRECATED
    json[jss::seqNum]       = to_string (ledger.getLedgerSeq());
    json[jss::parent_hash]  = to_string (ledger.getParentHash());
    json[jss::ledger_index] = to_string (ledger.getLedgerSeq());

    if (ledger.isClosed() || bFull)
    {
        if (ledger.isClosed())
            json[jss::closed] = true;

        // DEPRECATED
        json[jss::hash] = to_string (ledger.getHash());

        // DEPRECATED
        json[jss::totalCoins]        = to_string (ledger.getTotalCoins());
        json[jss::ledger_hash]       = to_string (ledger.getHash());
        json[jss::transaction_hash]  = to_string (ledger.getTransHash());
        json[jss::account_hash]      = to_string (ledger.getAccountHash());
        json[jss::accepted]          = ledger.isAccepted();
        json[jss::total_coins]       = to_string (ledger.getTotalCoins());

        auto closeTime = ledger.getCloseTimeNC();
        if (closeTime != 0)
        {
            json[jss::close_time]            = closeTime;
            json[jss::close_time_human]
                    = boost::posix_time::to_simple_string (
                        ptFromSeconds (closeTime));
            json[jss::close_time_resolution] = ledger.getCloseResolution();

            if (!ledger.getCloseAgree())
                json[jss::close_time_estimated] = true;
        }
    }
    else
    {
        json[jss::closed] = false;
    }

    if (ledger.haveTxMap() && (bFull || fill.options & LedgerFill::dumpTxrp))
    {
        auto const& txMap = ledger.txMap();
        auto&& txns = setArray (json, jss::transactions);
        SHAMapTreeNode::TNType type;

        CountedYield count (
            fill.yieldStrategy.transactionYieldCount, fill.yield);
        for (auto item = txMap.peekFirstItem (type); item;
             item = txMap.peekNextItem (item->key(), type))
        {
            count.yield();
            if (bFull || bExpand)
            {
                if (type == SHAMapTreeNode::tnTRANSACTION_NM)
                {
                    if (bBinary)
                    {
                        auto&& obj = appendObject (txns);
                        obj[jss::tx_blob] = strHex (item->peekData ());
                    }
                    else
                    {
                        SerialIter sit (item->slice ());
                        STTx txn (sit);
                        txns.append (txn.getJson (0));
                    }
                }
                else if (type == SHAMapTreeNode::tnTRANSACTION_MD)
                {
                    if (bBinary)
                    {
                        SerialIter sit (item->slice ());

                        auto&& obj = appendObject (txns);
                        obj[jss::tx_blob] = strHex (sit.getVL ());
                        obj[jss::meta] = strHex (sit.getVL ());
                    }
                    else
                    {
                        // VFALCO This is making a needless copy
                        SerialIter sit (item->slice ());
                        auto const vl = sit.getVL();
                        SerialIter tsit (makeSlice(vl));
                        STTx txn (tsit);

                        TxMeta meta (
                            item->key(), ledger.getLedgerSeq(), sit.getVL ());

                        auto&& txJson = appendObject (txns);
                        copyFrom(txJson, txn.getJson (0));
                        txJson[jss::metaData] = meta.getJson (0);
                    }
                }
                else
                {
                    auto&& error = appendObject (txns);
                    error[to_string (item->key())] = (int) type;
                }
            }
            else
            {
                txns.append (to_string (item->key()));
            }
        }
    }

    if (ledger.haveStateMap() && (bFull || fill.options & LedgerFill::dumpState))
    {
        auto const& stateMap = ledger.stateMap();
        auto&& array = Json::setArray (json, jss::accountState);
        RPC::CountedYield count (
            fill.yieldStrategy.accountYieldCount, fill.yield);
        if (bFull || bExpand)
        {
             if (bBinary)
             {
                 stateMap.visitLeaves (
                     [&array] (std::shared_ptr<SHAMapItem const> const& smi)
                     {
                         auto&& obj = appendObject (array);
                         obj[jss::hash] = to_string(smi->key());
                         obj[jss::tx_blob] = strHex(smi->peekData ());
                     });
             }
             else
             {
                 ledger.visitStateItems (
                     [&array, &count] (SLE::ref sle)
                     {
                         count.yield();
                         array.append (sle->getJson(0));
                     });
             }
        }
        else
        {
            stateMap.visitLeaves(
                [&array, &count] (std::shared_ptr<SHAMapItem const> const& smi)
                {
                    count.yield();
                    array.append (to_string(smi->key()));
                });
        }
    }
}

/** Add Json to an existing generic Object. */
template <class Object>
void addJsonImpl (Object& json, LedgerFill const& fill)
{
    auto&& object = Json::addObject (json, jss::ledger);
    fillJson (object, fill);
}

void addJson(Json::Object& object, LedgerFill const& fill)
{
    addJsonImpl (object, fill);
}

void addJson(Json::Value& object, LedgerFill const& fill)
{
    addJsonImpl (object, fill);
}

Json::Value getJson (LedgerFill const& fill)
{
    Json::Value json;
    fillJson (json, fill);
    return json;
}

} // ripple