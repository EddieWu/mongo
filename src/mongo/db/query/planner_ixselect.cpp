/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/planner_ixselect.h"

#include <vector>

#include "mongo/db/geo/core.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner_common.h"

namespace mongo {

    static double fieldWithDefault(const BSONObj& infoObj, const string& name, double def) {
        BSONElement e = infoObj[name];
        if (e.isNumber()) { return e.numberDouble(); }
        return def;
    }

    /**
     * 2d indices don't handle wrapping so we can't use them for queries that wrap.
     */
    static bool twoDWontWrap(const Circle& circle, const IndexEntry& index) {
        // XXX: where does this really belong
        GeoHashConverter::Parameters params;
        params.bits = static_cast<unsigned>(fieldWithDefault(index.infoObj, "bits", 26));
        params.max = fieldWithDefault(index.infoObj, "max", 180.0);
        params.min = fieldWithDefault(index.infoObj, "min", -180.0);
        double numBuckets = (1024 * 1024 * 1024 * 4.0);
        params.scaling = numBuckets / (params.max - params.min);

        GeoHashConverter conv(params);

        // FYI: old code used flat not spherical error.
        double yscandist = rad2deg(circle.radius) + conv.getErrorSphere();
        double xscandist = computeXScanDistance(circle.center.y, yscandist);
        bool ret = circle.center.x + xscandist < 180
                && circle.center.x - xscandist > -180
                && circle.center.y + yscandist < 90
                && circle.center.y - yscandist > -90;
        return ret;
    }

    // static
    void QueryPlannerIXSelect::getFields(MatchExpression* node,
                                         string prefix,
                                         unordered_set<string>* out) {
        // Do not traverse tree beyond a NOR negation node
        MatchExpression::MatchType exprtype = node->matchType();
        if (exprtype == MatchExpression::NOR) {
            return;
        }

        // Leaf nodes with a path and some array operators.
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            out->insert(prefix + node->path().toString());
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // If the array uses an index on its children, it's something like
            // {foo : {$elemMatch: { bar: 1}}}, in which case the predicate is really over
            // foo.bar.
            //
            // When we have {foo: {$all: [{$elemMatch: {a:1}}], the path of the embedded elemMatch
            // is empty.  We don't want to append a dot in that case as the field would be foo..a.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }

            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
    }

    // static
    void QueryPlannerIXSelect::findRelevantIndices(const unordered_set<string>& fields,
                                                   const vector<IndexEntry>& allIndices,
                                                   vector<IndexEntry>* out) {
        for (size_t i = 0; i < allIndices.size(); ++i) {
            BSONObjIterator it(allIndices[i].keyPattern);
            verify(it.more());
            BSONElement elt = it.next();
            if (fields.end() != fields.find(elt.fieldName())) {
                out->push_back(allIndices[i]);
            }
        }
    }

    // static
    bool QueryPlannerIXSelect::compatible(const BSONElement& elt,
                                          const IndexEntry& index,
                                          MatchExpression* node) {
        // Historically one could create indices with any particular value for the index spec,
        // including values that now indicate a special index.  As such we have to make sure the
        // index type wasn't overridden before we pay attention to the string in the index key
        // pattern element.
        //
        // e.g. long ago we could have created an index {a: "2dsphere"} and it would
        // be treated as a btree index by an ancient version of MongoDB.  To try to run
        // 2dsphere queries over it would be folly.
        string indexedFieldType;
        if (String != elt.type() || (INDEX_BTREE == index.type)) {
            indexedFieldType = "";
        }
        else {
            indexedFieldType = elt.String();
        }

        // We know elt.fieldname() == node->path().
        MatchExpression::MatchType exprtype = node->matchType();

        if (indexedFieldType.empty()) {
            // Can't check for null w/a sparse index.
            if (exprtype == MatchExpression::EQ && index.sparse) {
                const EqualityMatchExpression* expr
                    = static_cast<const EqualityMatchExpression*>(node);
                if (expr->getData().isNull()) {
                    return false;
                }
            }

            // We can't use a btree-indexed field for geo expressions.
            if (exprtype == MatchExpression::GEO || exprtype == MatchExpression::GEO_NEAR) {
                return false;
            }

            // There are restrictions on when we can use the index if
            // the expression is a NOT.
            if (exprtype == MatchExpression::NOT) {
                // Prevent negated preds from using sparse or
                // multikey indices. We do so for sparse indices because
                // we will fail to return the documents which do not contain
                // the indexed fields.
                //
                // We avoid multikey indices because of the semantics of
                // negations on multikey fields. For example, with multikey
                // index {a:1}, the document {a: [1,2,3]} does *not* match
                // the query {a: {$ne: 3}}. We'd mess this up if we used
                // an index scan over [MinKey, 3) and (3, MaxKey] without
                // a filter.
                if (index.sparse || index.multikey) {
                    return false;
                }
                // Can't index negations of MOD or REGEX
                MatchExpression::MatchType childtype = node->getChild(0)->matchType();
                if (MatchExpression::REGEX == childtype ||
                    MatchExpression::MOD == childtype) {
                    return false;
                }
            }

            // We can only index EQ using text indices.  This is an artificial limitation imposed by
            // FTSSpec::getIndexPrefix() which will fail if there is not an EQ predicate on each
            // index prefix field of the text index.
            //
            // Example for key pattern {a: 1, b: "text"}:
            // - Allowed: node = {a: 7}
            // - Not allowed: node = {a: {$gt: 7}}

            if (INDEX_TEXT != index.type) {
                return true;
            }

            // If we're here we know it's a text index.  Equalities are OK anywhere in a text index.
            if (MatchExpression::EQ == exprtype) {
                return true;
            }

            // Not-equalities can only go in a suffix field of an index kp.  We look through the key
            // pattern to see if the field we're looking at now appears as a prefix.  If so, we
            // can't use this index for it.
            BSONObjIterator specIt(index.keyPattern);
            while (specIt.more()) {
                BSONElement elt = specIt.next();
                // We hit the dividing mark between prefix and suffix, so whatever field we're
                // looking at is a suffix, since it appears *after* the dividing mark between the
                // two.  As such, we can use the index.
                if (String == elt.type()) {
                    return true;
                }

                // If we're here, we're still looking at prefix elements.  We know that exprtype
                // isn't EQ so we can't use this index.
                if (node->path() == elt.fieldNameStringData()) {
                    return false;
                }
            }

            // NOTE: This shouldn't be reached.  Text index implies there is a separator implies we
            // will always hit the 'return true' above.
            invariant(0);
            return true;
        }
        else if (IndexNames::HASHED == indexedFieldType) {
            return exprtype == MatchExpression::MATCH_IN || exprtype == MatchExpression::EQ;
        }
        else if (IndexNames::GEO_2DSPHERE == indexedFieldType) {
            if (exprtype == MatchExpression::GEO) {
                // within or intersect.
                GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
                const GeoQuery& gq = gme->getGeoQuery();
                const GeometryContainer& gc = gq.getGeometry();
                return gc.hasS2Region();
            }
            else if (exprtype == MatchExpression::GEO_NEAR) {
                GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
                // Make sure the near query is compatible with 2dsphere.
                if (gnme->getData().centroid.crs == SPHERE || gnme->getData().isNearSphere) {
                    return true;
                }
            }
            return false;
        }
        else if (IndexNames::GEO_2D == indexedFieldType) {
            if (exprtype == MatchExpression::GEO_NEAR) {
                GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
                return gnme->getData().centroid.crs == FLAT;
            }
            else if (exprtype == MatchExpression::GEO) {
                // 2d only supports within.
                GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
                const GeoQuery& gq = gme->getGeoQuery();
                if (GeoQuery::WITHIN != gq.getPred()) {
                    return false;
                }

                const GeometryContainer& gc = gq.getGeometry();

                // 2d indices answer flat queries.
                if (gc.hasFlatRegion()) {
                    return true;
                }

                // 2d indices can answer centerSphere queries.
                if (NULL == gc._cap.get()) {
                    return false;
                }

                verify(SPHERE == gc._cap->crs);
                const Circle& circle = gc._cap->circle;

                // No wrapping around the edge of the world is allowed in 2d centerSphere.
                return twoDWontWrap(circle, index);
            }
            return false;
        }
        else if (IndexNames::TEXT == indexedFieldType) {
            return (exprtype == MatchExpression::TEXT);
        }
        else if (IndexNames::GEO_HAYSTACK == indexedFieldType) {
            return false;
        }
        else {
            warning() << "Unknown indexing for node " << node->toString()
                      << " and field " << elt.toString() << endl;
            verify(0);
        }
    }

    // static
    void QueryPlannerIXSelect::rateIndices(MatchExpression* node,
                                           string prefix,
                                           const vector<IndexEntry>& indices) {
        // Do not traverse tree beyond logical NOR node
        MatchExpression::MatchType exprtype = node->matchType();
        if (exprtype == MatchExpression::NOR) {
            return;
        }

        // Every indexable node is tagged even when no compatible index is
        // available.
        if (Indexability::isBoundsGenerating(node)) {
            string fullPath;
            if (MatchExpression::NOT == node->matchType()) {
                fullPath = prefix + node->getChild(0)->path().toString();
            }
            else {
                fullPath = prefix + node->path().toString();
            }

            verify(NULL == node->getTag());
            RelevantTag* rt = new RelevantTag();
            node->setTag(rt);
            rt->path = fullPath;

            // TODO: This is slow, with all the string compares.
            for (size_t i = 0; i < indices.size(); ++i) {
                BSONObjIterator it(indices[i].keyPattern);
                BSONElement elt = it.next();
                if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                    rt->first.push_back(i);
                }
                while (it.more()) {
                    elt = it.next();
                    if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                        rt->notFirst.push_back(i);
                    }
                }
            }

            // If this is a NOT, we have to clone the tag and attach
            // it to the NOT's child.
            if (MatchExpression::NOT == node->matchType()) {
                RelevantTag* childRt = static_cast<RelevantTag*>(rt->clone());
                childRt->path = rt->path;
                node->getChild(0)->setTag(childRt);
            }
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // See comment in getFields about all/elemMatch and paths.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
    }

    /**
     * Remove 'idx' from the RelevantTag lists for 'node'.  'node' must be a leaf.
     */
    static void removeIndexRelevantTag(MatchExpression* node, size_t idx) {
        RelevantTag* tag = static_cast<RelevantTag*>(node->getTag());
        verify(tag);
        vector<size_t>::iterator firstIt = std::find(tag->first.begin(),
                                                     tag->first.end(),
                                                     idx);
        if (firstIt != tag->first.end()) {
            tag->first.erase(firstIt);
        }

        vector<size_t>::iterator notFirstIt = std::find(tag->notFirst.begin(),
                                                        tag->notFirst.end(),
                                                        idx);
        if (notFirstIt != tag->notFirst.end()) {
            tag->notFirst.erase(notFirstIt);
        }
    }

    /**
     * Traverse the subtree rooted at 'node' to remove invalid RelevantTag assignments to text index
     * 'idx', which has prefix paths 'prefixPaths'.
     */
    static void stripInvalidAssignmentsToTextIndex(MatchExpression* node,
                                                   size_t idx,
            const unordered_set<StringData, StringData::Hasher>& prefixPaths) {

        // If we're here, there are prefixPaths and node is either:
        // 1. a text pred which we can't use as we have nothing over its prefix, or
        // 2. a non-text pred which we can't use as we don't have a text pred AND-related.
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            removeIndexRelevantTag(node, idx);
            return;
        }

        // Do not traverse tree beyond negation node.
        if (node->matchType() == MatchExpression::NOT
            || node->matchType() == MatchExpression::NOR) {

            return;
        }

        // For anything to use a text index with prefixes, we require that:
        // 1. The text pred exists in an AND,
        // 2. The non-text preds that use the text index's prefixes are also in that AND.

        if (node->matchType() != MatchExpression::AND) {
            // It's an OR or some kind of array operator.
            for (size_t i = 0; i < node->numChildren(); ++i) {
                stripInvalidAssignmentsToTextIndex(node->getChild(i), idx, prefixPaths);
            }
            return;
        }

        // If we're here, we're an AND.  Determine whether the children satisfy the index prefix for
        // the text index.
        invariant(node->matchType() == MatchExpression::AND);

        bool hasText = false;

        // The AND must have an EQ predicate for each prefix path.  When we encounter a child with a
        // tag we remove it from childrenPrefixPaths.  All children exist if this set is empty at
        // the end.
        unordered_set<StringData, StringData::Hasher> childrenPrefixPaths = prefixPaths;

        for (size_t i = 0; i < node->numChildren(); ++i) {
            MatchExpression* child = node->getChild(i);
            RelevantTag* tag = static_cast<RelevantTag*>(child->getTag());

            if (NULL == tag) {
                // 'child' could be a logical operator.  Maybe there are some assignments hiding
                // inside.
                stripInvalidAssignmentsToTextIndex(child, idx, prefixPaths);
                continue;
            }

            bool inFirst = tag->first.end() != std::find(tag->first.begin(),
                                                         tag->first.end(),
                                                         idx);

            bool inNotFirst = tag->notFirst.end() != std::find(tag->notFirst.begin(),
                                                               tag->notFirst.end(),
                                                               idx);

            if (inFirst || inNotFirst) {
                // Great!  'child' was assigned to our index.
                if (child->matchType() == MatchExpression::TEXT) {
                    hasText = true;
                }
                else {
                    childrenPrefixPaths.erase(child->path());
                    // One fewer prefix we're looking for, possibly.  Note that we could have a
                    // suffix assignment on the index and wind up here.  In this case the erase
                    // above won't do anything since a suffix isn't a prefix.
                }
            }
            else {
                // Recurse on the children to ensure that they're not hiding any assignments
                // to idx.
                stripInvalidAssignmentsToTextIndex(child, idx, prefixPaths);
            }
        }

        // Our prereqs for using the text index were not satisfied so we remove the assignments from
        // all children of the AND.
        if (!hasText || !childrenPrefixPaths.empty()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                stripInvalidAssignmentsToTextIndex(node->getChild(i), idx, prefixPaths);
            }
        }
    }

    // static
    void QueryPlannerIXSelect::stripInvalidAssignmentsToTextIndexes(
        MatchExpression* node,
        const vector<IndexEntry>& indices) {

        for (size_t i = 0; i < indices.size(); ++i) {
            const IndexEntry& index = indices[i];

            // We only care about text indices.
            if (INDEX_TEXT != index.type) {
                continue;
            }

            // Gather the set of paths that comprise the index prefix for this text index.
            // Each of those paths must have an equality assignment, otherwise we can't assign
            // *anything* to this index.
            unordered_set<StringData, StringData::Hasher> textIndexPrefixPaths;
            BSONObjIterator it(index.keyPattern);

            // We stop when we see the first string in the key pattern.  We know that
            // the prefix precedes "text".
            for (BSONElement elt = it.next(); elt.type() != String; elt = it.next()) {
                textIndexPrefixPaths.insert(elt.fieldName());
                verify(it.more());
            }

            // If the index prefix is non-empty, remove invalid assignments to it.
            if (!textIndexPrefixPaths.empty()) {
                stripInvalidAssignmentsToTextIndex(node, i, textIndexPrefixPaths);
            }
        }
    }

}  // namespace mongo
