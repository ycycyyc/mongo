/**
 * Test sorting with dotted field paths using a case-insensitive collation.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 * ]
 */
(function() {
"use strict";

const coll = db.sort_dotted_paths_collation;
coll.drop();

// Create a collection with a collation that is case-insensitive
const caseInsensitive = {
    collation: {locale: "en_US", strength: 2}
};
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));

// Test out sort({a:1,b:1}) on a collection where a is an array for some documents and b is an array
// for other documents.
assert.commandWorked(coll.insert([
    {_id: 1, a: "a", b: ["b"]},
    {_id: 2, a: "b", b: []},
    {_id: 3, a: [], b: "d"},
    {_id: 4, a: ["a"], b: "d"},
    {_id: 5, a: ["b", "C"], b: "b"},
    {_id: 6, a: "b", b: ["C", "e"]},
    {_id: 7, a: "a", b: ["a", "C"]},
    {_id: 8, a: ["a", "b"], b: "C"},
    {_id: 9, a: "b", b: "C"}
]));

assert.eq(
    coll.find({}, {_id: 1}).sort({a: 1, b: 1, _id: 1}).toArray(),
    [{_id: 3}, {_id: 7}, {_id: 1}, {_id: 8}, {_id: 4}, {_id: 2}, {_id: 5}, {_id: 6}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({a: 1, b: -1, _id: 1}).toArray(),
    [{_id: 3}, {_id: 4}, {_id: 7}, {_id: 8}, {_id: 1}, {_id: 6}, {_id: 9}, {_id: 5}, {_id: 2}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({a: -1, b: 1, _id: 1}).toArray(),
    [{_id: 5}, {_id: 2}, {_id: 6}, {_id: 8}, {_id: 9}, {_id: 7}, {_id: 1}, {_id: 4}, {_id: 3}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({a: -1, b: -1, _id: 1}).toArray(),
    [{_id: 5}, {_id: 6}, {_id: 8}, {_id: 9}, {_id: 2}, {_id: 4}, {_id: 7}, {_id: 1}, {_id: 3}]);

// Verify that sort({a:1,b:1}) fails with a "parallel arrays" error when there is at least one
// document where both a and b are arrays.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(
    coll.insert([{_id: 1, a: [], b: "a"}, {_id: 2, a: "a", b: []}, {_id: 3, a: [], b: []}]));

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {a: 1, b: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {_id: 1, a: 1, b: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {a: 1, _id: 1, b: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {a: 1, b: 1, _id: 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);

// Verify that sort({a:1,b:1}) does not fail with a "parallel arrays" error when documents where
// both a and b are arrays are filtered out.
const filter1 = {
    $or: [{a: {$not: {$type: "array"}}}, {b: {$not: {$type: "array"}}}]
};
const output1 = [{_id: 1, a: [], b: "a"}, {_id: 2, a: "a", b: []}];
assert.eq(coll.find(filter1).sort({a: 1, b: 1}).toArray(), output1);
assert.eq(coll.find(filter1).sort({_id: 1, a: 1, b: 1}).toArray(), output1);
assert.eq(coll.find(filter1).sort({a: 1, _id: 1, b: 1}).toArray(), output1);
assert.eq(coll.find(filter1).sort({a: 1, b: 1, _id: 1}).toArray(), output1);

// Basic tests for a sort pattern that contains a path of length 2.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert([
    {_id: 1, a: {b: "b"}},
    {_id: 2, a: {b: []}},
    {_id: 3, a: {b: ["b", "C"]}},
    {_id: 4, a: {c: "a"}},
    {_id: 5, a: []},
    {_id: 6, a: [{b: ["e"]}]},
    {_id: 7, a: [{b: ["d", "e"]}, {b: "G"}]},
    {_id: 8, a: [{b: ["a", "C"]}, {b: ["b", "e"]}]},
    {_id: 9, a: [{b: []}, {b: ["a", "C"]}]}
]));

assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": 1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 9}, {_id: 4}, {_id: 5}, {_id: 8}, {_id: 1}, {_id: 3}, {_id: 7}, {_id: 6}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": 1, _id: -1}).toArray(),
    [{_id: 9}, {_id: 2}, {_id: 5}, {_id: 4}, {_id: 8}, {_id: 3}, {_id: 1}, {_id: 7}, {_id: 6}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": -1, _id: 1}).toArray(),
    [{_id: 7}, {_id: 6}, {_id: 8}, {_id: 3}, {_id: 9}, {_id: 1}, {_id: 4}, {_id: 5}, {_id: 2}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": -1, _id: -1}).toArray(),
    [{_id: 7}, {_id: 8}, {_id: 6}, {_id: 9}, {_id: 3}, {_id: 1}, {_id: 5}, {_id: 4}, {_id: 2}]);

// Basic tests for a sort pattern that contains two paths of length 2 with a common prefix.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert([
    {_id: 1, a: [{b: "d", c: "a"}, {b: "a", c: "b"}]},
    {_id: 2, a: [{b: "b", c: "a"}, {b: "d", c: "b"}]},
    {_id: 3, a: [{b: "F", c: "a"}, {b: "I", c: "b"}]},
    {_id: 4, a: [{b: "d", c: "a"}, {b: "e", c: "C"}]},
    {_id: 5, a: [{b: "b", c: "a"}, {b: "G", c: "C"}]},
    {_id: 6, a: [{b: "e", c: "a"}, {b: "C", c: "C"}]},
    {_id: 7, a: [{b: "G", c: "b"}, {b: "b", c: "C"}]},
    {_id: 8, a: [{b: "C", c: "b"}, {b: "h", c: "C"}]},
    {_id: 9, a: [{b: "h", c: "b"}, {b: "F", c: "C"}]},
]));

assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": 1, "a.b": 1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 6}, {_id: 3}, {_id: 8}, {_id: 7}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": 1, "a.b": -1, _id: 1}).toArray(),
    [{_id: 3}, {_id: 6}, {_id: 1}, {_id: 4}, {_id: 2}, {_id: 5}, {_id: 9}, {_id: 7}, {_id: 8}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": -1, "a.b": 1, _id: 1}).toArray(),
    [{_id: 7}, {_id: 6}, {_id: 4}, {_id: 9}, {_id: 5}, {_id: 8}, {_id: 1}, {_id: 2}, {_id: 3}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": -1, "a.b": -1, _id: 1}).toArray(),
    [{_id: 8}, {_id: 5}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 3}, {_id: 2}, {_id: 1}]);

// Basic tests for a sort pattern that contains a path of length 2 and a path of length 1, where
// one path is a prefix of the other path.
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": 1, a: 1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 6}, {_id: 3}, {_id: 8}, {_id: 7}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": 1, a: -1, _id: 1}).toArray(),
    [{_id: 3}, {_id: 6}, {_id: 1}, {_id: 4}, {_id: 2}, {_id: 5}, {_id: 9}, {_id: 7}, {_id: 8}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": -1, a: 1, _id: 1}).toArray(),
    [{_id: 7}, {_id: 6}, {_id: 4}, {_id: 9}, {_id: 5}, {_id: 8}, {_id: 1}, {_id: 2}, {_id: 3}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": -1, a: -1, _id: 1}).toArray(),
    [{_id: 8}, {_id: 5}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 3}, {_id: 2}, {_id: 1}]);

// Verify that sort({"a.b":1,"a.c":1}) fails with a "parallel arrays" error when there is at least
// one document where both a.b and a.c are arrays.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert([{_id: 1, a: {b: ["a", "b"], c: ["C", "d"]}}]));

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {"a.b": 1, "a.c": 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);

// Verify that sort({"a.b":1,"c.d":1}) fails with a "parallel arrays" error when there is at least
// onw document where both a.b and c.d are arrays.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert({a: {b: ["a", "b"]}, c: {d: ["C", "d"]}}));

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {"a.b": 1, "c.d": 1}}),
                             [ErrorCodes.BadValue, ErrorCodes.CannotIndexParallelArrays]);

// More tests for a sort pattern that contains two paths of length 2 with a common prefix.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert([
    {_id: 1, a: [{b: ["d", "a"], c: "a"}, {b: ["a", "e"], c: "b"}]},
    {_id: 2, a: [{b: "b", c: ["a", "C"]}, {b: "d", c: ["b", "d"]}]},
    {_id: 3, a: [{b: ["F", "d"], c: "a"}, {b: ["I", "G"], c: "b"}]},
    {_id: 4, a: [{b: "d", c: ["a", "b"]}, {b: "e", c: ["C", "b"]}]},
    {_id: 5, a: [{b: ["b", "C"], c: "a"}, {b: ["G", "F"], c: "C"}]},
    {_id: 6, a: [{b: "e", c: []}, {b: "C", c: "C"}]},
    {_id: 7, a: [{b: [], c: "b"}, {b: "b", c: "C"}]},
    {_id: 8, a: [{b: "C", c: ["b"]}, {b: "h", c: ["C"]}]},
    {_id: 9, a: [{b: ["h"], c: "b"}, {b: ["F"], c: "C"}]},
]));

assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": 1, "a.b": 1, _id: 1}).toArray(),
    [{_id: 6}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 3}, {_id: 4}, {_id: 7}, {_id: 8}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": 1, "a.b": -1, _id: 1}).toArray(),
    [{_id: 6}, {_id: 3}, {_id: 1}, {_id: 4}, {_id: 5}, {_id: 2}, {_id: 9}, {_id: 8}, {_id: 7}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": -1, "a.b": 1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 7}, {_id: 6}, {_id: 4}, {_id: 5}, {_id: 9}, {_id: 8}, {_id: 1}, {_id: 3}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.c": -1, "a.b": -1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 8}, {_id: 5}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 3}, {_id: 1}]);

// More tests for a sort pattern that contains a path of length 2 and a path of length 1, where
// one path is a prefix of the other path.
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": 1, a: 1, _id: 1}).toArray(),
    [{_id: 7}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 6}, {_id: 8}, {_id: 4}, {_id: 3}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": 1, a: -1, _id: 1}).toArray(),
    [{_id: 7}, {_id: 1}, {_id: 5}, {_id: 2}, {_id: 8}, {_id: 6}, {_id: 3}, {_id: 4}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": -1, a: 1, _id: 1}).toArray(),
    [{_id: 3}, {_id: 8}, {_id: 9}, {_id: 5}, {_id: 6}, {_id: 4}, {_id: 1}, {_id: 2}, {_id: 7}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": -1, a: -1, _id: 1}).toArray(),
    [{_id: 3}, {_id: 9}, {_id: 8}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 6}, {_id: 2}, {_id: 7}]);

// Test out sort({"a.0.b":1}) on a collection of documents where field "a" and sub-field "b" are
// a mix of different types.
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.0.b": 1, _id: 1}).toArray(),
    [{_id: 7}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 8}, {_id: 3}, {_id: 4}, {_id: 6}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.0.b": 1, _id: -1}).toArray(),
    [{_id: 7}, {_id: 1}, {_id: 5}, {_id: 2}, {_id: 8}, {_id: 4}, {_id: 3}, {_id: 6}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.0.b": -1, _id: 1}).toArray(),
    [{_id: 9}, {_id: 3}, {_id: 6}, {_id: 1}, {_id: 4}, {_id: 5}, {_id: 8}, {_id: 2}, {_id: 7}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.0.b": -1, _id: -1}).toArray(),
    [{_id: 9}, {_id: 3}, {_id: 6}, {_id: 4}, {_id: 1}, {_id: 8}, {_id: 5}, {_id: 2}, {_id: 7}]);

// Tests for a sort pattern that contains two paths of length 2 that do not have a common prefix.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert([
    {_id: 1, a: {b: "a"}, c: [{d: ["d", "a"]}, {d: ["a", "e"]}]},
    {_id: 2, a: [{b: ["a", "C"]}, {b: ["b", "d"]}], c: {d: "b"}},
    {_id: 3, a: {b: "a"}, c: [{d: ["F", "d"]}, {d: ["I", "G"]}]},
    {_id: 4, a: [{b: ["b"]}, {b: ["C", "b"]}], c: {d: "d"}},
    {_id: 5, a: {b: "a"}, c: [{d: ["b", "C"]}, {d: ["G", "F"]}]},
    {_id: 6, a: [{b: []}, {b: "C"}], c: {d: "d"}},
    {_id: 7, a: {b: "b"}, c: [{d: []}, {d: "b"}]},
    {_id: 8, a: [{b: ["b"]}, {b: ["C"]}], c: {d: "C"}},
    {_id: 9, a: {b: "C"}, c: [{d: ["h"]}, {d: ["F"]}]},
]));

assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": 1, "c.d": 1, _id: 1}).toArray(),
    [{_id: 6}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 3}, {_id: 7}, {_id: 8}, {_id: 4}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": 1, "c.d": -1, _id: 1}).toArray(),
    [{_id: 6}, {_id: 3}, {_id: 5}, {_id: 1}, {_id: 2}, {_id: 4}, {_id: 8}, {_id: 7}, {_id: 9}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": -1, "c.d": 1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 8}, {_id: 4}, {_id: 6}, {_id: 9}, {_id: 7}, {_id: 1}, {_id: 5}, {_id: 3}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b": -1, "c.d": -1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 8}, {_id: 7}, {_id: 3}, {_id: 5}, {_id: 1}]);

// Basic tests for a sort pattern that contains a path of length 3.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert([
    {_id: 1, a: {b: {c: "b"}}},
    {_id: 2, a: {b: {c: []}}},
    {_id: 3, a: [{b: []}, {b: {c: []}}]},
    {_id: 4, a: [{b: {c: "a"}}]},
    {_id: 5, a: {b: [{c: "C"}, {d: "a"}]}},
    {_id: 6, a: {b: [{c: ["e"]}]}},
    {_id: 7, a: []},
    {_id: 8, a: {b: [{c: ["a", "C"]}, {c: ["b", "e"]}]}},
    {_id: 9, a: [{b: [{c: []}, {c: ["a", "C"]}]}]}
]));

assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b.c": 1, _id: 1}).toArray(),
    [{_id: 2}, {_id: 3}, {_id: 9}, {_id: 5}, {_id: 7}, {_id: 4}, {_id: 8}, {_id: 1}, {_id: 6}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b.c": 1, _id: -1}).toArray(),
    [{_id: 9}, {_id: 3}, {_id: 2}, {_id: 7}, {_id: 5}, {_id: 8}, {_id: 4}, {_id: 1}, {_id: 6}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b.c": -1, _id: 1}).toArray(),
    [{_id: 6}, {_id: 8}, {_id: 5}, {_id: 9}, {_id: 1}, {_id: 4}, {_id: 3}, {_id: 7}, {_id: 2}]);
assert.eq(
    coll.find({}, {_id: 1}).sort({"a.b.c": -1, _id: -1}).toArray(),
    [{_id: 8}, {_id: 6}, {_id: 9}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 7}, {_id: 3}, {_id: 2}]);
})();
