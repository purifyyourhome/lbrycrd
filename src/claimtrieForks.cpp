#include "claimtrie.h"

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/locale/conversion.hpp>
#include <boost/locale/localization_backend.hpp>
#include <boost/locale.hpp>
#include <boost/scope_exit.hpp>

void CClaimTrieCacheExpirationFork::removeAndAddToExpirationQueue(expirationQueueRowType &row, int height, bool increment) const
{
    for (expirationQueueRowType::iterator e = row.begin(); e != row.end(); ++e)
    {
        // remove and insert with new expiration time
        removeFromExpirationQueue(e->name, e->outPoint, height);
        int extend_expiration = Params().GetConsensus().nExtendedClaimExpirationTime - Params().GetConsensus().nOriginalClaimExpirationTime;
        int new_expiration_height = increment ? height + extend_expiration : height - extend_expiration;
        nameOutPointType entry(e->name, e->outPoint);
        addToExpirationQueue(new_expiration_height, entry);
    }

}

void CClaimTrieCacheExpirationFork::removeAndAddSupportToExpirationQueue(expirationQueueRowType &row, int height, bool increment) const
{
    for (expirationQueueRowType::iterator e = row.begin(); e != row.end(); ++e)
    {
        // remove and insert with new expiration time
        removeSupportFromExpirationQueue(e->name, e->outPoint, height);
        int extend_expiration = Params().GetConsensus().nExtendedClaimExpirationTime - Params().GetConsensus().nOriginalClaimExpirationTime;
        int new_expiration_height = increment ? height + extend_expiration : height - extend_expiration;
        nameOutPointType entry(e->name, e->outPoint);
        addSupportToExpirationQueue(new_expiration_height, entry);
    }

}

bool CClaimTrieCacheExpirationFork::forkForExpirationChange(bool increment) const
{
    /*
    If increment is True, we have forked to extend the expiration time, thus items in the expiration queue
    will have their expiration extended by "new expiration time - original expiration time"

    If increment is False, we are decremented a block to reverse the fork. Thus items in the expiration queue
    will have their expiration extension removed.
    */

    // look through dirty expiration queues
    std::set<int> dirtyHeights;
    for (expirationQueueType::const_iterator i = base->dirtyExpirationQueueRows.begin(); i != base->dirtyExpirationQueueRows.end(); ++i)
    {
        int height = i->first;
        dirtyHeights.insert(height);
        expirationQueueRowType row = i->second;
        removeAndAddToExpirationQueue(row, height, increment);
    }

    std::set<int> dirtySupportHeights;
    for (expirationQueueType::const_iterator i = base->dirtySupportExpirationQueueRows.begin(); i != base->dirtySupportExpirationQueueRows.end(); ++i)
    {
        int height = i->first;
        dirtySupportHeights.insert(height);
        expirationQueueRowType row = i->second;
        removeAndAddSupportToExpirationQueue(row, height, increment);
    }


    //look through db for expiration queues, if we haven't already found it in dirty expiration queue
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&base->db)->NewIterator());
    pcursor->SeekToFirst();
    while (pcursor->Valid())
    {
        std::pair<char, int> key;
        if (pcursor->GetKey(key))
        {
            int height = key.second;
            // if we've looked through this in dirtyExprirationQueueRows, don't use it
            // because its stale
            if ((key.first == EXP_QUEUE_ROW) & (dirtyHeights.count(height) == 0))
            {
                expirationQueueRowType row;
                if (pcursor->GetValue(row))
                {
                    removeAndAddToExpirationQueue(row, height, increment);
                }
                else
                {
                    return error("%s(): error reading expiration queue rows from disk", __func__);
                }
            }
            else if ((key.first == SUPPORT_EXP_QUEUE_ROW) & (dirtySupportHeights.count(height) == 0))
            {
                expirationQueueRowType row;
                if (pcursor->GetValue(row))
                {
                    removeAndAddSupportToExpirationQueue(row, height, increment);
                }
                else
                {
                    return error("%s(): error reading support expiration queue rows from disk", __func__);
                }
            }

        }
        pcursor->Next();
    }

    return true;
}


bool CClaimTrieCacheNormalizationFork::shouldNormalize() const {
    return nCurrentHeight > Params().GetConsensus().nNormalizedNameForkHeight;
}

std::string CClaimTrieCacheNormalizationFork::normalizeClaimName(const std::string& name, bool force) const {
    if (!force && !shouldNormalize())
        return name;

    static std::locale utf8;
    static bool initialized = false;
    if (!initialized) {
        static boost::locale::localization_backend_manager manager =
                boost::locale::localization_backend_manager::global();
        manager.select("icu");

        static boost::locale::generator curLocale(manager);
        utf8 = curLocale("en_US.UTF8");
        initialized = true;
    }

    std::string normalized;
    try {

        // Check if it is a valid utf-8 string. If not, it will throw a
        // boost::locale::conv::conversion_error exception which we catch later
        normalized = boost::locale::conv::to_utf<char>(name, "UTF-8", boost::locale::conv::stop);
        if (normalized.empty())
            return name;

        normalized = boost::locale::normalize(normalized, boost::locale::norm_nfd, utf8);

        // Locale aware lowercase (the non-locale-aware version seemed to struggle with some international chars):
        normalized = boost::locale::to_lower(normalized, utf8);
    }
    catch (const boost::locale::conv::conversion_error& e){
        return name;
    }
    catch (const std::bad_cast& e) {
        LogPrintf("%s() is invalid or dependencies are missing: %s\n", __func__, e.what());
        throw;
    }
    catch (const std::exception& e) { // TODO: change to use ... with current_exception() in c++11
        LogPrintf("%s() had an unexpected exception: %s\n", __func__, e.what());
        return name;
    }

    return normalized;
}

bool CClaimTrieCacheNormalizationFork::insertClaimIntoTrie(const std::string& name, CClaimValue claim,
                                bool fCheckTakeover) const {
    return CClaimTrieCacheExpirationFork::insertClaimIntoTrie(normalizeClaimName(name, overrideInsertNormalization), claim, fCheckTakeover);
}

bool CClaimTrieCacheNormalizationFork::removeClaimFromTrie(const std::string& name, const COutPoint& outPoint,
                                CClaimValue& claim, bool fCheckTakeover) const {
    return CClaimTrieCacheExpirationFork::removeClaimFromTrie(normalizeClaimName(name, overrideRemoveNormalization), outPoint, claim, fCheckTakeover);
}

bool CClaimTrieCacheNormalizationFork::insertSupportIntoMap(const std::string& name, CSupportValue support,
                                bool fCheckTakeover) const {
    return CClaimTrieCacheExpirationFork::insertSupportIntoMap(normalizeClaimName(name, overrideInsertNormalization), support, fCheckTakeover);
}
bool CClaimTrieCacheNormalizationFork::removeSupportFromMap(const std::string& name, const COutPoint& outPoint,
                                CSupportValue& support, bool fCheckTakeover) const {
    return CClaimTrieCacheExpirationFork::removeSupportFromMap(normalizeClaimName(name, overrideRemoveNormalization), outPoint, support, fCheckTakeover);
}

bool CClaimTrieCacheNormalizationFork::normalizeAllNamesInTrieIfNecessary(insertUndoType& insertUndo, claimQueueRowType& removeUndo,
                                                             insertUndoType& insertSupportUndo, supportQueueRowType& expireSupportUndo,
                                                             std::vector<std::pair<std::string, int> >& takeoverHeightUndo) const {

    if (nCurrentHeight == Params().GetConsensus().nNormalizedNameForkHeight) {

        // run the one-time upgrade of all names that need to change
        std::vector<namedNodeType> nodes = flattenTrie(); // TODO: replace all these calls with a real iterator (coded in #106)
        for (std::vector<namedNodeType>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (it->second.claims.empty()) continue;
            const std::string normalized = normalizeClaimName(it->first, true);
            if (normalized == it->first) continue;

            supportMapEntryType supports;
            if (getSupportsForName(it->first, supports)) {
                BOOST_FOREACH(CSupportValue& support, supports) {
                    // if it's already going to expire just skip it
                    if (support.nHeight + base->nExpirationTime <= nCurrentHeight)
                        continue;

                    bool success = removeSupportFromMap(it->first, support.outPoint, support, false);
                    assert(success);
                    expireSupportUndo.push_back(std::make_pair(it->first, support));
                    success = insertSupportIntoMap(normalized, support, false);
                    assert(success);
                    insertSupportUndo.push_back(nameOutPointHeightType(it->first, support.outPoint, -1));
                }
            }

            BOOST_FOREACH(CClaimValue& claim, it->second.claims) {
                if (claim.nHeight + base->nExpirationTime <= nCurrentHeight)
                    continue;

                bool success = removeClaimFromTrie(it->first, claim.outPoint, claim, false);
                assert(success);
                removeUndo.push_back(std::make_pair(it->first, claim));

                success = insertClaimIntoTrie(normalized, claim, true);
                assert(success);
                insertUndo.push_back(nameOutPointHeightType(it->first, claim.outPoint, -1));
            }

            takeoverHeightUndo.push_back(std::make_pair(it->first, it->second.nHeightOfLastTakeover));
        }
        return true;
    }
    return false;
}

bool CClaimTrieCacheNormalizationFork::incrementBlock(insertUndoType& insertUndo,
                            claimQueueRowType& expireUndo,
                            insertUndoType& insertSupportUndo,
                            supportQueueRowType& expireSupportUndo,
                            std::vector<std::pair<std::string, int> >& takeoverHeightUndo) {
    overrideInsertNormalization = normalizeAllNamesInTrieIfNecessary(insertUndo, expireUndo, insertSupportUndo,
            expireSupportUndo, takeoverHeightUndo);
    BOOST_SCOPE_EXIT(&overrideInsertNormalization) { overrideInsertNormalization = false; } BOOST_SCOPE_EXIT_END
    return CClaimTrieCacheExpirationFork::incrementBlock(insertUndo, expireUndo, insertSupportUndo,
            expireSupportUndo, takeoverHeightUndo);
}

bool CClaimTrieCacheNormalizationFork::decrementBlock(insertUndoType& insertUndo,
                            claimQueueRowType& expireUndo,
                            insertUndoType& insertSupportUndo,
                            supportQueueRowType& expireSupportUndo,
                            std::vector<std::pair<std::string, int> >& takeoverHeightUndo) {

    overrideRemoveNormalization = shouldNormalize();
    BOOST_SCOPE_EXIT(&overrideRemoveNormalization) { overrideRemoveNormalization = false; } BOOST_SCOPE_EXIT_END
    return CClaimTrieCacheExpirationFork::decrementBlock(insertUndo, expireUndo, insertSupportUndo,
            expireSupportUndo, takeoverHeightUndo);
}

bool CClaimTrieCacheNormalizationFork::getProofForName(const std::string& name, CClaimTrieProof& proof) const {
    return CClaimTrieCacheExpirationFork::getProofForName(normalizeClaimName(name), proof);
}

bool CClaimTrieCacheNormalizationFork::getInfoForName(const std::string& name, CClaimValue& claim) const {
    return CClaimTrieCacheExpirationFork::getInfoForName(normalizeClaimName(name), claim);
}

claimsForNameType CClaimTrieCacheNormalizationFork::getClaimsForName(const std::string& name) const {
    return CClaimTrieCacheExpirationFork::getClaimsForName(normalizeClaimName(name));
}

int CClaimTrieCacheNormalizationFork::getDelayForName(const std::string& name, const uint160& claimId) const {
    return CClaimTrieCacheExpirationFork::getDelayForName(normalizeClaimName(name), claimId);
}

bool CClaimTrieCacheNormalizationFork::namesMatchForRemoval(const std::string& a, const std::string& b) const {
    return normalizeClaimName(a) == normalizeClaimName(b);
}