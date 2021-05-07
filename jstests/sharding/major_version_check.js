//
// Tests that only a correct major-version is needed to connect to a shard via mongos
//
(function() {
    'use strict';

    // Test with default value of incrementChunkMajorVersionOnChunkSplits, which is false.
    (() => {
        var st = new ShardingTest({shards: 1, mongos: 2});

        var mongos = st.s0;
        var staleMongos = st.s1;
        var admin = mongos.getDB("admin");
        var config = mongos.getDB("config");
        var coll = mongos.getCollection("foo.bar");

        // Shard collection
        assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
        assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

        // Make sure our stale mongos is up-to-date with no splits
        staleMongos.getCollection(coll + "").findOne();

        // Run one split
        assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

        // Make sure our stale mongos is not up-to-date with the split
        printjson(admin.runCommand({getShardVersion: coll + ""}));
        printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

        // Compare strings b/c timestamp comparison is a bit weird
        assert.eq(Timestamp(1, 2), admin.runCommand({getShardVersion: coll + ""}).version);
        assert.eq(Timestamp(1, 0),
                  staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

        // See if our stale mongos is required to catch up to run a findOne on an existing
        // connection
        staleMongos.getCollection(coll + "").findOne();

        printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));
        assert.eq(Timestamp(1, 0),
                  staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

        // See if our stale mongos is required to catch up to run a findOne on a new connection
        staleMongos = new Mongo(staleMongos.host);
        staleMongos.getCollection(coll + "").findOne();

        printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));
        assert.eq(Timestamp(1, 0),
                  staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

        // Merge the just-split chunks and ensure the major version didn't go up
        assert.commandWorked(
            admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: MaxKey}]}));
        assert.eq(Timestamp(1, 3), admin.runCommand({getShardVersion: coll + ""}).version);

        st.stop();
    })();

    // Test with incrementChunkMajorVersionOnChunkSplits set to true.
    (() => {
        var st = new ShardingTest({
            shards: 1,
            mongos: 2,
            other:
                {configOptions: {setParameter: {incrementChunkMajorVersionOnChunkSplits: true}}}
        });

        var mongos = st.s0;
        var staleMongos = st.s1;
        var admin = mongos.getDB("admin");
        var config = mongos.getDB("config");
        var coll = mongos.getCollection("foo.bar");

        // Shard collection
        assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
        assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

        // Make sure our stale mongos is up-to-date with no splits
        staleMongos.getCollection(coll + "").findOne();

        // Run one split
        assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

        // Make sure our stale mongos is not up-to-date with the split
        printjson(admin.runCommand({getShardVersion: coll + ""}));
        printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

        // Compare strings b/c timestamp comparison is a bit weird
        assert.eq(Timestamp(2, 2), admin.runCommand({getShardVersion: coll + ""}).version);
        assert.eq(Timestamp(1, 0),
                  staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

        // See if our stale mongos is required to catch up to run a findOne on an existing
        // connection
        staleMongos.getCollection(coll + "").findOne();

        printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

        assert.eq(Timestamp(1, 0),
                  staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

        // See if our stale mongos is required to catch up to run a findOne on a new connection
        staleMongos = new Mongo(staleMongos.host);
        staleMongos.getCollection(coll + "").findOne();

        printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

        assert.eq(Timestamp(1, 0),
                  staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

        // Run another split on the original chunk, which does not exist anymore (but the stale
        // mongos thinks it exists). This should fail and cause a refresh on
        // the shard, updating its shard version.
        assert.commandFailed(staleMongos.getDB("admin").runCommand(
            {split: coll + "", bounds: [{_id: MinKey}, {_id: MaxKey}]}));

        // This findOne will cause a refresh on the router since the shard version has now been
        // increased.
        staleMongos.getCollection(coll + "").findOne();

        printjson(staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}));

        // The previously stale mongos should now be up-to-date.
        assert.eq(Timestamp(2, 2),
                  staleMongos.getDB("admin").runCommand({getShardVersion: coll + ""}).version);

        // Merge the just-split chunks and ensure the major version went up
        assert.commandWorked(
            admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: MaxKey}]}));
        assert.eq(Timestamp(3, 0), admin.runCommand({getShardVersion: coll + ""}).version);

        st.stop();
    })();
})();
