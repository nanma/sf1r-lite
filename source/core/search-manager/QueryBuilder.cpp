#include "QueryBuilder.h"
#include "TermDocumentIterator.h"
#include "SearchTermDocumentIterator.h"
#include "RankTermDocumentIterator.h"
#include "ANDDocumentIterator.h"
#include "ORDocumentIterator.h"
#include "NOTDocumentIterator.h"
#include "WildcardDocumentIterator.h"
#include "WildcardPhraseDocumentIterator.h"
#include "ExactPhraseDocumentIterator.h"
#include "OrderedPhraseDocumentIterator.h"
#include "NearbyPhraseDocumentIterator.h"
#include "PersonalSearchDocumentIterator.h"
#include "FilterCache.h"

#include <common/TermTypeDetector.h>

#include <ir/index_manager/utility/BitVector.h>
#include <ir/index_manager/utility/BitMapIterator.h>

#include <util/get.h>

#include <vector>
#include <string>
#include <memory>
#include <algorithm>

#include <boost/token_iterator.hpp>

//#define VERBOSE_SERACH_MANAGER 1

using namespace std;

using izenelib::ir::indexmanager::TermDocFreqs;
using izenelib::ir::indexmanager::TermReader;

namespace sf1r{

QueryBuilder::QueryBuilder(
    boost::shared_ptr<IndexManager> indexManager,
    boost::shared_ptr<DocumentManager> documentManager,
    boost::shared_ptr<IDManager> idManager,
    boost::unordered_map<std::string, PropertyConfig>& schemaMap,
    size_t filterCacheNum
)
    :indexManagerPtr_(indexManager)
    ,documentManagerPtr_(documentManager)
    ,idManagerPtr_(idManager)
    ,schemaMap_(schemaMap)
{
    pIndexReader_ = indexManager->pIndexReader_;
    filterCache_.reset(new FilterCache(filterCacheNum));
}

QueryBuilder::~QueryBuilder()
{
}

void QueryBuilder::reset_cache()
{
    filterCache_->clear();
}

Filter* QueryBuilder::prepare_filter(const std::vector<QueryFiltering::FilteringType>& filtingList)
{
    boost::shared_ptr<EWAHBoolArray<uint32_t> > pDocIdSet;
    boost::shared_ptr<BitVector> pBitVector;
    unsigned int bitsNum = pIndexReader_->maxDoc() + 1;
    unsigned int wordsNum = bitsNum/(sizeof(uint32_t) * 8) + (bitsNum % (sizeof(uint32_t) * 8) == 0 ? 0 : 1);

    if (filtingList.size() == 1)
    {
        const QueryFiltering::FilteringType& filteringItem= filtingList[0];
        QueryFiltering::FilteringOperation filterOperation = filteringItem.first.first;
        const std::string& property = filteringItem.first.second;
        const std::vector<PropertyValue>& filterParam = filteringItem.second;
        if (!filterCache_->get(filteringItem, pDocIdSet))
        {
            pDocIdSet.reset(new EWAHBoolArray<uint32_t>());
            pBitVector.reset(new BitVector(bitsNum));
            indexManagerPtr_->makeRangeQuery(filterOperation, property, filterParam, pBitVector);
            //Compress bit vector
            pBitVector->compressed(*pDocIdSet);
            filterCache_->set(filteringItem, pDocIdSet);
        }
    }
    else
    {
        pDocIdSet.reset(new EWAHBoolArray<uint32_t>());
        pDocIdSet->addStreamOfEmptyWords(true, wordsNum);
        boost::shared_ptr<EWAHBoolArray<uint32_t> > pDocIdSet2;
        boost::shared_ptr<EWAHBoolArray<uint32_t> > pDocIdSet3;
        try
        {
            std::vector<QueryFiltering::FilteringType>::const_iterator iter = filtingList.begin();
            for (; iter != filtingList.end(); ++iter)
            {
                QueryFiltering::FilteringOperation filterOperation = iter->first.first;
                const std::string& property = iter->first.second;
                const std::vector<PropertyValue>& filterParam = iter->second;
                if (!filterCache_->get(*iter, pDocIdSet2))
                {
                    pDocIdSet2.reset(new EWAHBoolArray<uint32_t>());
                    pBitVector.reset(new BitVector(bitsNum));
                    indexManagerPtr_->makeRangeQuery(filterOperation, property, filterParam, pBitVector);
                    pBitVector->compressed(*pDocIdSet2);
                    filterCache_->set(*iter, pDocIdSet2);
                }
                pDocIdSet3.reset(new EWAHBoolArray<uint32_t>());
                (*pDocIdSet).rawlogicaland(*pDocIdSet2, *pDocIdSet3);
                (*pDocIdSet).swap(*pDocIdSet3);
            }
        }
        catch (std::exception& e)
        {
        }
    }
    return new Filter(pDocIdSet);
}

MultiPropertyScorer* QueryBuilder::prepare_dociterator(
    SearchKeywordOperation& actionOperation,
    collectionid_t colID,
    const property_weight_map& propertyWeightMap,
    const std::vector<std::string>& properties,
    const std::vector<unsigned int>& propertyIds,
    bool readPositions,
    const std::vector<std::map<termid_t, unsigned> >& termIndexMaps
)
{
    size_t size_of_properties = propertyIds.size();

    MultiPropertyScorer* docIterPtr = new MultiPropertyScorer(propertyWeightMap, propertyIds);
    if (pIndexReader_->isDirty())
    {
        pIndexReader_ = indexManagerPtr_->getIndexReader();
    }
    try{
    size_t success_properties = 0;
    for (size_t i = 0; i < size_of_properties; i++)
    {
        prepare_for_property_(
            docIterPtr,
            success_properties,
            actionOperation,
            colID,
            properties[i],
            propertyIds[i],
            readPositions,
            termIndexMaps[i]
        );
    }

    if (success_properties)
        return docIterPtr;
    else
        delete docIterPtr;

    return NULL;
    }catch(std::exception& e){
        delete docIterPtr;
        return NULL;
    }
}


void QueryBuilder::prepare_for_property_(
    MultiPropertyScorer* pScorer,
    size_t & success_properties,
    SearchKeywordOperation& actionOperation,
    collectionid_t colID,
    const std::string& property,
    unsigned int propertyId,
    bool readPositions,
    const std::map<termid_t, unsigned>& termIndexMapInProperty
)
{
    std::map<termid_t, std::vector<TermDocFreqs*> > termDocReaders;
    bool isNumericFilter = false;
    sf1r::PropertyDataType dataType;

    typedef boost::unordered_map<std::string, PropertyConfig>::const_iterator iterator;
    iterator found = schemaMap_.find(property);
    if (found == schemaMap_.end())
        return;

    dataType = found->second.getType();
    if(found->second.isIndex() && found->second.getIsFilter()
                    && found->second.getType() != STRING_PROPERTY_TYPE)
             isNumericFilter = true;

#if PREFETCH_TERMID
    std::vector<pair<termid_t, string> >termList;
    actionOperation.getQueryTermInfoList(property, termList);
    TermReader* pTermReader = NULL;
    if (!isNumericFilter)
    {
        pTermReader = pIndexReader_->getTermReader(colID);
        if (!pTermReader)
                return;
    }

    std::sort(termList.begin(), termList.end());
    for (std::vector<pair<termid_t, string> >::const_iterator it = termList.begin();
            it != termList.end(); ++it)
    {
        termid_t termId = (*it).first;
        const string& termStr = (*it).second;
        if(!isNumericFilter)
        {
            Term term(property.c_str(),termId);
            bool find = pTermReader->seek(&term);

            if (find)
            {
                TermDocFreqs* pTermDocReader = 0;
                if (readPositions)
                    pTermDocReader = pTermReader->termPositions();
                else
                    pTermDocReader = pTermReader->termDocFreqs();
                if(pTermDocReader) ///NULL when exception
                    termDocReaders[termId].push_back(pTermDocReader);
            }

        }
        else
        {
            boost::shared_ptr<EWAHBoolArray<uint32_t> > pDocIdSet;
            boost::shared_ptr<BitVector> pBitVector;

            if (!TermTypeDetector::isTypeMatch(termStr, dataType))
                continue;

            PropertyType value;
            PropertyValue2IndexPropertyType converter(value);
            boost::apply_visitor(converter, TermTypeDetector::propertyValue_.getVariant());
            bool find = indexManagerPtr_->seekTermFromBTreeIndex(colID, property, value);

            if (find)
            {
                QueryFiltering::FilteringType filteringRule;
                filteringRule.first.first = QueryFiltering::EQUAL;
                filteringRule.first.second = property;
                std::vector<PropertyValue> filterParam;
                filterParam.push_back(TermTypeDetector::propertyValue_);
                filteringRule.second = filterParam;
                if(!filterCache_->get(filteringRule, pDocIdSet))
                {
                    pDocIdSet.reset(new EWAHBoolArray<uint32_t>());
                    pBitVector.reset(new BitVector(pIndexReader_->maxDoc() + 1));

                    indexManagerPtr_->getDocsByNumericValue(colID, property, value, *pBitVector);
                    pBitVector->compressed(*pDocIdSet);
                    filterCache_->set(filteringRule, pDocIdSet);
                }
                TermDocFreqs* pTermDocReader = new BitMapIterator( pDocIdSet->bit_iterator() );
                termDocReaders[termId].push_back(pTermDocReader);
            }
        }
     }
     if (pTermReader)
        delete pTermReader;

#endif
    DocumentIterator* pIter = NULL;
    QueryTreePtr queryTree;
    if ( actionOperation.getQueryTree(property, queryTree) )
        do_prepare_for_property_(
            queryTree,
            colID,
            property,
            propertyId,
            dataType,
            isNumericFilter,
            readPositions,
            termIndexMapInProperty,
            pIter,
            termDocReaders,
            actionOperation.hasUnigramProperty_,
            actionOperation.isUnigramSearchMode_
        );

    if (pIter)
    {
        pScorer->add(propertyId, pIter, termDocReaders);
        ++success_properties;
    }
    else
    {
        for (std::map<termid_t, std::vector<TermDocFreqs*> >::iterator
                it = termDocReaders.begin(); it != termDocReaders.end(); ++it)
        {
            for (size_t j =0; j<it->second.size(); j ++ )
            {
                delete it->second[j];
            }
            it->second.clear();
        }
    }
}


bool QueryBuilder::do_prepare_for_property_(
    QueryTreePtr& queryTree,
    collectionid_t colID,
    const std::string& property,
    unsigned int propertyId,
    PropertyDataType propertyDataType,
    bool isNumericFilter,
    bool readPositions,
    const std::map<termid_t, unsigned>& termIndexMapInProperty,
    DocumentIteratorPointer& pDocIterator,
    std::map<termid_t, std::vector<TermDocFreqs*> >& termDocReaders,
    bool hasUnigramProperty,
    bool isUnigramSearchMode,
    int parentAndOrFlag
)
{
    switch (queryTree->type_)
    {
    case QueryTree::KEYWORD:
    case QueryTree::RANK_KEYWORD:
    {
        string keyword = queryTree->keyword_;
        termid_t keywordId = queryTree->keywordId_;
        unsigned termIndex = izenelib::util::getOr(
                                 termIndexMapInProperty,
                                 keywordId,
                                 (std::numeric_limits<unsigned>::max) ()
                             );

        TermDocumentIterator* pIterator = NULL;
        try{
        if (!isUnigramSearchMode)
        {
            pIterator = new TermDocumentIterator(
                keywordId,
                keyword,
                colID,
                pIndexReader_,
                indexManagerPtr_,
                property,
                propertyId,
                propertyDataType,
                isNumericFilter,
                termIndex,
                readPositions);
        }
        else
        {
            // term for searching
            if (queryTree->type_ == QueryTree::KEYWORD)
            {
                // termIndex is invalid in this case, it will not be used
                pIterator = new SearchTermDocumentIterator(keywordId,
                        colID,
                        pIndexReader_,
                        property,
                        propertyId,
                        termIndex,
                        readPositions);
            }
            // term for ranking
            else //if(queryTree->type_ == QueryTree::RANK_KEYWORD)
            {
                pIterator = new RankTermDocumentIterator(keywordId,
                        colID,
                        pIndexReader_,
                        property,
                        propertyId,
                        termIndex,
                        readPositions,
                        (parentAndOrFlag == 1));
            }
        }

#if PREFETCH_TERMID
        std::map<termid_t, std::vector<TermDocFreqs*> >::iterator constIt
        = termDocReaders.find(keywordId);
        if (constIt != termDocReaders.end() && !constIt->second.empty() )
        {
#ifdef VERBOSE_SERACH_MANAGER
            cout<<"have term  property "<<property<<" keyword ";
            queryTree->keywordUString_.displayStringValue(izenelib::util::UString::UTF_8);
            cout<<endl;
#endif
            pIterator->set(constIt->second.back() );
            if (NULL == pDocIterator)
                pDocIterator = pIterator;
            else
                pDocIterator->add(pIterator);

            constIt->second.pop_back();
        }
        else
#endif
        {
            if (pIterator->accept())
            {
#ifdef VERBOSE_SERACH_MANAGER
                cout<<"have term  property "<<property<<" keyword ";
                queryTree->keywordUString_.displayStringValue(izenelib::util::UString::UTF_8);
                cout<<endl;
#endif
                if (NULL == pDocIterator)
                    pDocIterator = pIterator;
                else
                    pDocIterator->add(pIterator);
            }
            else
            {
#ifdef VERBOSE_SERACH_MANAGER
                cout<<"do not have term property "<<property<<" ";
                queryTree->keywordUString_.displayStringValue(izenelib::util::UString::UTF_8);
                cout<<endl;
#endif
                delete pIterator;
                pIterator = NULL;
                if (!isNumericFilter)
                    return false;
            }
        }
        }catch(std::exception& e){
            delete pIterator;
            return false;
        }
        return true;
        break;
    } // end - QueryTree::KEYWORD
    case QueryTree::NOT:
    {
        if ( queryTree->children_.size() != 1 )
            return false;

        QueryTreePtr childQueryTree = *(queryTree->children_.begin());
        termid_t keywordId = childQueryTree->keywordId_;
        if (NULL == pDocIterator)
            ///only NOT is not permitted
            return false;
        unsigned termIndex = izenelib::util::getOr(
                                 termIndexMapInProperty,
                                 keywordId,
                                 (std::numeric_limits<unsigned>::max) ()
                             );
        TermDocumentIterator* pIterator =
            new TermDocumentIterator(keywordId,
                                     colID,
                                     pIndexReader_,
                                     property,
                                     propertyId,
                                     termIndex,
                                     readPositions);
        pIterator->setNot(true);
        try{
#if PREFETCH_TERMID
        std::map<termid_t, std::vector<TermDocFreqs*> >::iterator constIt
        = termDocReaders.find(keywordId);
        if (constIt != termDocReaders.end() && !constIt->second.empty() )
        {
            pIterator->set(constIt->second.back());
            pDocIterator->add(pIterator);
            constIt->second.pop_back();
        }
        else
#endif
        {
            if (pIterator->accept())
            {
                pDocIterator->add(pIterator);
            }
            else
                delete pIterator;
        }
        }catch(std::exception& e)
        {
            delete pIterator;
            return true;
        }
        break;
    } // end QueryTree::NOT
    case QueryTree::UNIGRAM_WILDCARD:
    {
        vector<termid_t> termIds;
        vector<unsigned> termIndexes;
        vector<size_t> asterisk_pos;
        vector<size_t> question_mark_pos;
        for (std::list<QueryTreePtr>::iterator childIter = queryTree->children_.begin();
                childIter != queryTree->children_.end(); ++childIter)
        {
            if ((*childIter)->type_ == QueryTree::ASTERISK )
            {
                asterisk_pos.push_back( termIds.size() );
            }
            else if ((*childIter)->type_ == QueryTree::QUESTION_MARK )
            {
                question_mark_pos.push_back( termIds.size() );
            }
            else
            {
                termid_t keywordId( (*childIter)->keywordId_ );
                unsigned termIndex = izenelib::util::getOr(
                                         termIndexMapInProperty,
                                         keywordId,
                                         (std::numeric_limits<unsigned>::max) ()
                                     );
                termIds.push_back(keywordId);
                termIndexes.push_back(termIndex);
            }
        }

        // if there is no unigram alias-ed property, perform on original property
        std::string unigramProperty = property;
        if (hasUnigramProperty)
        {
            unigramProperty += "_unigram";
        }
        unsigned int unigramPropertyId = 0;
        typedef boost::unordered_map<std::string, PropertyConfig>::const_iterator iterator;
        iterator found = schemaMap_.find(unigramProperty);
        if (found != schemaMap_.end())
        {
            unigramPropertyId = found->second.getPropertyId();
        }
        WildcardPhraseDocumentIterator* pIterator =
            new WildcardPhraseDocumentIterator(
            termIds,
            termIndexes,
            asterisk_pos,
            question_mark_pos,
            colID,
            pIndexReader_,
            unigramProperty,
            unigramPropertyId,
            documentManagerPtr_
        );
        if (NULL == pDocIterator)
            pDocIterator = pIterator;
        else
            pDocIterator->add(pIterator);
        break;
    } // end - QueryTree::UNIGRAM_WILDCARD
    case QueryTree::TRIE_WILDCARD:
    {
        if (queryTree->children_.size() == 0)
            return false;

        //5 top frequent terms
        static const int kWildcardTermThreshold = 5;
        WildcardDocumentIterator* pWildCardDocIter =
            new WildcardDocumentIterator(
            colID,
            pIndexReader_,
            property,
            propertyId,
            readPositions,
            kWildcardTermThreshold
        );
        for (QTIter iter = queryTree->children_.begin();
                iter != queryTree->children_.end(); ++iter)
        {
            termid_t termId = (*iter)->keywordId_;
            unsigned termIndex = izenelib::util::getOr(
                                     termIndexMapInProperty,
                                     termId,
                                     (std::numeric_limits<unsigned>::max) ()
                                 );

            pWildCardDocIter->add(termId, termIndex, termDocReaders);
        }
        if (0 == pWildCardDocIter->numIterators())
        {
            delete pWildCardDocIter;
            return false;
        }
        if (NULL == pDocIterator)
            pDocIterator = pWildCardDocIter;
        else
            pDocIterator->add(pWildCardDocIter);
        break;
    } // end - QueryTree::TRIE_WILDCARD
    case QueryTree::AND:
    {
#ifdef VERBOSE_SERACH_MANAGER
        cout<<"AND query "<<property<<endl;
#endif
        DocumentIterator* pIterator = new ANDDocumentIterator();
        bool ret = false;
        try{
        for (QTIter andChildIter = queryTree->children_.begin();
                andChildIter != queryTree->children_.end(); ++andChildIter)
        {
            ret = do_prepare_for_property_(
                      *andChildIter,
                      colID,
                      property,
                      propertyId,
                      propertyDataType,
                      isNumericFilter,
                      readPositions,
                      termIndexMapInProperty,
                      pIterator,
                      termDocReaders,
                      hasUnigramProperty,
                      isUnigramSearchMode,
                      1
                  );
            if (!ret)
            {
                delete pIterator;
                return false;
            }
        }
        if (! static_cast<ANDDocumentIterator*>(pIterator)->empty())
            if (NULL == pDocIterator)
                pDocIterator = pIterator;
            else
                pDocIterator->add(pIterator);
        else
            delete pIterator;
        }catch(std::exception& e){
            delete pIterator;
            return false;
        }
        break;
    } // end - QueryTree::AND
    case QueryTree::OR:
    {
#ifdef VERBOSE_SERACH_MANAGER
        cout<<"OR query "<<property<<endl;
#endif
        DocumentIterator* pIterator = new ORDocumentIterator();
        bool ret = false;
        try{
        for (QTIter orChildIter = queryTree->children_.begin();
                orChildIter != queryTree->children_.end(); ++orChildIter)
        {
            ret |= do_prepare_for_property_(
                       *orChildIter,
                       colID,
                       property,
                       propertyId,
                       propertyDataType,
                       isNumericFilter,
                       readPositions,
                       termIndexMapInProperty,
                       pIterator,
                       termDocReaders,
                       hasUnigramProperty,
                       isUnigramSearchMode,
                       0
                   );
        }
        if (!ret)
        {
            delete pIterator;
            return false;
        }
        if (! static_cast<ORDocumentIterator*>(pIterator)->empty())
            if (NULL == pDocIterator)
                pDocIterator = pIterator;
            else
                pDocIterator->add(pIterator);
        else
            delete pIterator;
        }catch(std::exception& e)            {
            delete pIterator;
            return false;
        }
        break;
    } // end - QueryTree::OR
    case QueryTree::AND_PERSONAL:
    {
        /// always return true in this case,
        /// it's tolerable that personal items are not indexed.
#ifdef VERBOSE_SERACH_MANAGER
        cout<<"AND-Personal query "<<property<<endl;
#endif
        DocumentIterator* pIterator = new PersonalSearchDocumentIterator(true);
        bool ret = false;
        try{
        for (QTIter andChildIter = queryTree->children_.begin();
                andChildIter != queryTree->children_.end(); ++andChildIter)
        {
            ret = do_prepare_for_property_(
                      *andChildIter,
                      colID,
                      property,
                      propertyId,
                      propertyDataType,
                      isNumericFilter,
                      readPositions,
                      termIndexMapInProperty,
                      pIterator,
                      termDocReaders
                  );
            if (!ret)
            {
                delete pIterator;
                ///return false;
                return true;
            }
        }
        if (! static_cast<PersonalSearchDocumentIterator*>(pIterator)->empty())
            if (NULL == pDocIterator)
                pDocIterator = pIterator;
            else
                pDocIterator->add(pIterator);
        else
            delete pIterator;
        }catch(std::exception& e){
            delete pIterator;
            return false;
        }
        break;
    } // end - QueryTree::AND_PERSONAL
    case QueryTree::OR_PERSONAL:
    {
        /// always return true in this case,
        /// it's tolerable that personal items are not indexed.
        /// OR_PERSONAL iterator is an AND semantic, but a child of OR.
#ifdef VERBOSE_SERACH_MANAGER
        cout<<"OR-Personal query "<<property<<endl;
#endif
        DocumentIterator* pIterator = new PersonalSearchDocumentIterator(false);
        bool ret = false;
        try{
        for (QTIter orChildIter = queryTree->children_.begin();
                orChildIter != queryTree->children_.end(); ++orChildIter)
        {
            ret = do_prepare_for_property_(
                      *orChildIter,
                      colID,
                      property,
                      propertyId,
                      propertyDataType,
                      isNumericFilter,
                      readPositions,
                      termIndexMapInProperty,
                      pIterator,
                      termDocReaders
                  );
            if (!ret)
            {
                delete pIterator;
                ///return false;
                return true;
            }
        }
        if (! static_cast<PersonalSearchDocumentIterator*>(pIterator)->empty())
            if (NULL == pDocIterator)
                pDocIterator = pIterator;
            else
                pDocIterator->add(pIterator);
        else
            delete pIterator;
        }catch(std::exception& e){
            delete pIterator;
            return false;
        }
        break;
    } // end - QueryTree::OR_PERSONAL
    case QueryTree::EXACT:
    {
#ifdef VERBOSE_SERACH_MANAGER
        cout<<"EXACT query "<<property<<endl;
#endif

        vector<termid_t> termIds;
        vector<unsigned> termIndexes;
        getTermIdsAndIndexesOfSiblings(
            queryTree,
            termIndexMapInProperty,
            property,
            termIds,
            termIndexes
        );

        // if there is no unigram alias-ed property, perform on original property
        std::string unigramProperty = property;
        if (hasUnigramProperty)
        {
            unigramProperty += "_unigram";
        }
        unsigned int unigramPropertyId = 0;
        typedef boost::unordered_map<std::string, PropertyConfig>::const_iterator iterator;
        iterator found = schemaMap_.find(unigramProperty);
        if (found != schemaMap_.end())
        {
            unigramPropertyId = found->second.getPropertyId();
        }
        ExactPhraseDocumentIterator* pIterator =
            new ExactPhraseDocumentIterator(
            termIds,
            termIndexes,
            colID,
            pIndexReader_,
            unigramProperty,
            unigramPropertyId
        );
        pIterator->setOrigProperty(property);
        if (NULL == pDocIterator)
            pDocIterator = pIterator;
        else
            pDocIterator->add(pIterator);

        break;
    }  // end - QueryTree::EXACT
    case QueryTree::NEARBY:
    {
        vector<termid_t> termIds;
        vector<unsigned> termIndexes;
        getTermIdsAndIndexesOfSiblings(
            queryTree,
            termIndexMapInProperty,
            property,
            termIds,
            termIndexes
        );

        NearbyPhraseDocumentIterator* pIterator =
            new NearbyPhraseDocumentIterator(
            termIds,
            termIndexes,
            colID,
            pIndexReader_,
            property,
            propertyId,
            queryTree->distance_
        );

        if (NULL == pDocIterator)
            pDocIterator = pIterator;
        else
            pDocIterator->add(pIterator);
        break;
    } // end - QueryTree::NEARBY
    case QueryTree::ORDER:
    {
        vector<termid_t> termIds;
        vector<unsigned> termIndexes;
        getTermIdsAndIndexesOfSiblings(
            queryTree,
            termIndexMapInProperty,
            property,
            termIds,
            termIndexes
        );

        OrderedPhraseDocumentIterator* pIterator =
            new OrderedPhraseDocumentIterator(
            termIds,
            termIndexes,
            colID,
            pIndexReader_,
            property,
            propertyId
        );
        if (NULL == pDocIterator)
            pDocIterator = pIterator;
        else
            pDocIterator->add(pIterator);
        break;
    } // end - QueryTree::ORDER
    default:
        break;
    } // end - switch

    return true;
}

void QueryBuilder::getTermIdsAndIndexesOfSiblings(
    QueryTreePtr& queryTree,
    const std::map<termid_t, unsigned>& termIndexMapInProperty,
    const std::string& property,
    std::vector<termid_t>& outTermIds,
    std::vector<unsigned>& outTermIndexes
)
{
    std::vector<termid_t> termIds;
    std::vector<unsigned> termIndexes;

    for (QTIter childIter = queryTree->children_.begin();
            childIter != queryTree->children_.end(); ++childIter)
    {
        termid_t keywordId( (*childIter)->keywordId_ );
        unsigned termIndex = izenelib::util::getOr(
                                 termIndexMapInProperty,
                                 keywordId,
                                 (std::numeric_limits<unsigned>::max) ()
                             );

        termIds.push_back(keywordId);
        termIndexes.push_back(termIndex);
    }

    termIds.swap(outTermIds);
    termIndexes.swap(outTermIndexes);
}

} // namespace sf1r
