#pragma once

// Redis Lua scripts shared by the client-facing HotColdStore and the maintenance
// HotColdWorker. One copy guarantees both threads apply identical guards, and
// central/tests/test_hotcold_lua.py loads these exact texts and runs them on Redis.
namespace HotColdScripts {

// Append one history row and update the per-ID indexes atomically.
// KEYS: history list, latest, lastSeen zset, recvIndex zset
// ARGV: itemJson, eventTimestampMs, receivedAtMs, id, requireActive ("1"/"0"), maxActiveIds (0 = unlimited)
// Returns the new history length; -1 = caller saw the ID hot but the worker evicted it meanwhile
// (nothing written); -2 = a new hot ID would exceed maxActiveIds (nothing written).
inline constexpr const char* append = R"LUA(
-- If the caller observed an active ID, require that it is still active at
-- the exact append point. A worker may have archived/evicted it meanwhile.
local active=redis.call('ZSCORE',KEYS[3],ARGV[4])
if ARGV[5]=='1' and not active then return -1 end
local cap=tonumber(ARGV[6] or '0')
if (not active) and cap>0 and redis.call('ZCARD',KEYS[3])>=cap then return -2 end
local old=redis.call('GET',KEYS[2])
local count=redis.call('RPUSH',KEYS[1],ARGV[1])
redis.call('ZADD',KEYS[4],ARGV[3],string.format('%016d',count-1))
if (not old) or tonumber(cjson.decode(old).record.timestamp)<=tonumber(ARGV[2]) then
  redis.call('SET',KEYS[2],ARGV[1])
end
redis.call('ZADD',KEYS[3],ARGV[3],ARGV[4])
return count
)LUA";

// Evict an ID only if it is still idle and every history row is archived.
// KEYS: history list, latest, lastSeen zset, synced hash, recvIndex zset
// ARGV: id, cutoffMs. Returns 1 when evicted, 0 when skipped.
inline constexpr const char* evict = R"LUA(
local last=redis.call('ZSCORE',KEYS[3],ARGV[1])
if (not last) or tonumber(last)>tonumber(ARGV[2]) then return 0 end
local n=redis.call('LLEN',KEYS[1])
local synced=tonumber(redis.call('HGET',KEYS[4],ARGV[1]) or '0')
if n~=synced then return 0 end
redis.call('DEL',KEYS[1],KEYS[2],KEYS[5])
redis.call('HDEL',KEYS[4],ARGV[1])
redis.call('ZREM',KEYS[3],ARGV[1])
return 1
)LUA";

// Advance the archived-row counter only after PostgreSQL committed exactly the rows
// [oldOffset, newOffset): compare-and-set on the old offset, and the first/last sequence
// of the committed batch must match the rows Redis holds at those indexes. Second line of
// defence behind ArsRedisClient::abort(): a mismatched reply can never mark unarchived
// rows as archived, so the evict script can never delete them.
// KEYS: synced hash, history list
// ARGV: id, oldOffset, newOffset, firstSequence, lastSequence
// Returns 1 = advanced, -1 = offset changed, -2 = rows missing, -3 = batch mismatch.
inline constexpr const char* advanceSynced = R"LUA(
local current=tonumber(redis.call('HGET',KEYS[1],ARGV[1]) or '0')
if current~=tonumber(ARGV[2]) then return -1 end
local first=redis.call('LINDEX',KEYS[2],tonumber(ARGV[2]))
local last=redis.call('LINDEX',KEYS[2],tonumber(ARGV[3])-1)
if (not first) or (not last) then return -2 end
if tostring(cjson.decode(first).sequence)~=ARGV[4] or
   tostring(cjson.decode(last).sequence)~=ARGV[5] then return -3 end
redis.call('HSET',KEYS[1],ARGV[1],ARGV[3])
return 1
)LUA";

// Recovery: append a batch of archived rows to the staging list and its time index.
// KEYS: staging list, staging index zset
// ARGV: ttlSec, n, firstIndex, then n pairs (itemJson, receivedAtMs)
inline constexpr const char* stageBatch = R"LUA(
local n=tonumber(ARGV[2]); local first=tonumber(ARGV[3])
for i=1,n do
  redis.call('RPUSH',KEYS[1],ARGV[4+(i-1)*2])
  redis.call('ZADD',KEYS[2],ARGV[5+(i-1)*2],string.format('%016d',first+i-1))
end
redis.call('EXPIRE',KEYS[1],tonumber(ARGV[1]))
redis.call('EXPIRE',KEYS[2],tonumber(ARGV[1]))
return n
)LUA";

// Recovery: publish a completed staging history as the ID's hot history, atomically.
// KEYS: staging, history, latest, synced, lastSeen, staging index, recvIndex
// ARGV: count, bestItemJson, id, nowMs, maxActiveIds
// Returns 1 = published, 0 = ID already active (staging discarded by caller),
// -1 = maxActiveIds reached.
inline constexpr const char* publishRecovery = R"LUA(
if redis.call('ZSCORE',KEYS[5],ARGV[3]) then return 0 end
local cap=tonumber(ARGV[5] or '0')
if cap>0 and redis.call('ZCARD',KEYS[5])>=cap then return -1 end
redis.call('RENAME',KEYS[1],KEYS[2]); redis.call('PERSIST',KEYS[2])
redis.call('RENAME',KEYS[6],KEYS[7]); redis.call('PERSIST',KEYS[7])
redis.call('SET',KEYS[3],ARGV[2])
redis.call('HSET',KEYS[4],ARGV[3],ARGV[1])
redis.call('ZADD',KEYS[5],ARGV[4],ARGV[3])
return 1
)LUA";

} // namespace HotColdScripts
