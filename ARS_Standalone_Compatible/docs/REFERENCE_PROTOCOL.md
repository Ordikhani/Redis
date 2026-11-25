> **مرجع تاریخی پروژه اصلی**: ممکن است از INI یا توکن استفاده کند؛ نسخه اجرایی فعلی مطابق این متن نیست.

# ARS TCP protocol v1

UTF-8 compact JSON followed by LF. One connection supports requests, replies and live events.
Requests may be fragmented/coalesced by TCP; parsing does not assume a packet per request.
No HTTP/WebSocket or direct Redis access is needed on clients.

Request:
```json
{"requestId":"unique-request-id","op":"latest","args":{"id":"Target_101"}}
```
Success/error:
```json
{"requestId":"unique-request-id","ok":true,"result":{"found":false}}
{"requestId":"unique-request-id","ok":false,"error":"Authenticate first"}
```
Authenticate first with `auth` and `args.token` (ARS_API_TOKEN). Authentication must finish within 10 seconds.
All authenticated peers have the same permissions. Transport has no built-in TLS.

| op | args | result |
|---|---|---|
| auth | token | protocol:1 |
| status | {} | postgres, redisCache, hotColdEnabled, inactiveAfterSec, checkpointEverySec, lastCheckpointAtMs, recoveryQueueDepth, protocol |
| save | record | stored, sequence, receivedAtMs, storage, checkpointed, cacheAvailable; for a cold archived ID the reply is held until its history is restored and then also carries restoredFromPostgres, restoredHistoryCount, heldForRecoveryMs |
| publish | channel, record | same as save; live event after Redis acceptance (or PostgreSQL fallback) |
| latest | id | found, optional record/sequence/receivedAtMs; source |
| history | id, fromMs=0, toMs=-1, limit=100, timeField="timestamp", afterSequence="", untilSequence="" | items, hasMore, nextSequence, untilSequence |
| subscribe | channel | channel, subscribed:true |
| unsubscribe | channel | channel, subscribed:false |
| delete | id, removeFromDisk=true | deleted count (archive) or cacheEvicted/archivePreserved |
| hydrate / recover | id | status, id, jobId; cold history recovery is queued asynchronously |
| recovery_status | jobId | queued/running/completed/failed, restoredCount, error if failed |

`timeField` accepts `timestamp` or `receivedAt`. Both bounds are inclusive; -1 means no upper bound.
History is ordered by insertion sequence ascending, not event timestamp. Limit: 1..1000.
Each item wraps `{record, sequence, receivedAtMs}`. To continue, pass nextSequence as afterSequence and
retain the returned untilSequence and original filters. Stop when hasMore=false.
untilSequence bounds the first page's maximum sequence; concurrent deletes can still remove rows.
There is no cross-request transactional snapshot and no durable live replay cursor.

Live event (not a response):
```json
{"event":"record","channel":"radar:live_targets","record":{"id":"Target_101","timestamp":1700000000000},"sequence":"123","receivedAtMs":1700000000010}
```
Subscribe acknowledgement is sent before later events. Wait for it before relying on a subscription.
Pub/Sub is best effort; slow clients can be disconnected. Durable history remains queryable.
Incoming request maximum: 1 MiB. Record maximum: 256 KiB. History response payload budget about 4 MiB.
Client output/read limit: 8 MiB. ID/channel max: 256 UTF-8 bytes. Max subscriptions per peer: 64.
There is no automatic retry or idempotency key. requestId correlates a response; it does NOT deduplicate writes.
A timeout/disconnect after submission leaves the write outcome uncertain. Every accepted save creates a new sequence.
Unknown operations, invalid fields, and database failures return ok=false. Capacity admission failures return `ok=false`, `code=HOT_CAPACITY_REACHED`, and a human-readable `error`; clients should apply backpressure and retry only after hot capacity is released. The Qt SDK preserves the legacy `requestFailed` signal and adds `requestFailedDetailed(requestId, operation, error, code)`. No raw SQL is accepted over the network.

## Redis-first note

For `save` and `publish`, `storage=redis` / `checkpointed=false` means the record was accepted by Redis, **not** that it has been committed to PostgreSQL. Periodic checkpoints and inactivity archiving are described in `HOT_COLD_ARCHITECTURE_FA.md`. A Redis failure can switch to `storage=postgres` / `checkpointed=true`. `ok=true` always means the record was stored; a write that could not be stored returns `ok=false`.


`restoredFromPostgres` and `restoredHistoryCount` are included in successful Redis-backed
`save`/`publish` responses in the Hot/Cold implementation. They report whether complete
previous history was brought back from PostgreSQL before appending the new record.
Live events are emitted by `publish`, not by plain `save`.

Qt `ArsClient` emits `liveRecordReceived(channel, record)` for existing users, and
`liveRecordDetailed(channel, record, sequence, receivedAtMs)` for clients that
need the server reception date on every realtime event.

## Hot Data Manager extension (backward-compatible)

| op | args | result |
|---|---|---|
| `search_hot` | `mode=latest/records`, `filters=[]`, `idContains`, `cursor="0:0"`, `limit=20` (max 50), `scanBudget=120` (max 200) | `items, source=redis, mode, scanned, hotIds, hasMore, nextCursor` |
| `recent` | `id`, `lookbackSec=1800` or `fromMs/toMs`, `limit=50` (max 100), `offset=0` | `items, source, indexed, fromMs, toMs, hasMore, nextOffset` |

`search_hot` searches only Redis memory, with every active ID's latest record or every active ID's full history. Filters support `field` paths under the record (`features.x`, `speed`, `timestamp`, etc.) or wrapper fields (`receivedAtMs`, `sequence`) and operations `eq/ne/gt/gte/lt/lte/contains` (all filters ANDed). `search_hot` scans a bounded number of candidates; it is not a global full-text index and cursors are best effort when concurrent changes reorder the hot IDs. Consult [HOT_DATA_MANAGER_FA.md](HOT_DATA_MANAGER_FA.md) for paging and disk management.


### Asynchronous cold-ID recovery

When `save`/`publish` targets an ID that is archived in PostgreSQL but not active in Redis, the server **does not block the client Event Loop** to restore it and **does not acknowledge the write early**. It queues one bounded recovery job and holds the request. When the job completes, the held writes for that ID are stored in arrival order (keeping their original `receivedAtMs`) and only then answered with `ok=true` (plus `restoredFromPostgres`, `restoredHistoryCount`, `heldForRecoveryMs`); `publish` events are sent at that point. If the job is still running after `[recovery] ackTimeoutMs` (default 10000, max 14000, below the SDK's 15 s request timeout), the write is answered `ok=false`, `code=RECOVERY_PENDING`, `recoveryJobId`: nothing was stored; resend it later. A failed job answers `ok=false` with `code=RECOVERY_FAILED` or `HOT_CAPACITY_REACHED`. At most 256 writes per ID and 4096 in total are held; beyond that `RECOVERY_PENDING` is returned immediately. Held writes are in server memory and unacknowledged: if the server stops, the client sees a disconnect, which, as for any in-flight write, leaves the outcome uncertain.

`hydrate` is now asynchronous for cold IDs: it returns `status=queued` and `jobId`; use `recovery_status` until terminal state. An already-active ID returns `status=completed`; an ID without archive returns `notFound=true`. The recovery queue is bounded by `[recovery] maxQueuedJobs` in `server.ini` (default 64), and one recovery job runs at a time to bound CPU, SQL connections, Redis command pressure, and RAM. The history is streamed from PostgreSQL and staged in Redis in bounded batches; only after all batches succeed is the staging history atomically published. On failure, the archive remains the source of truth and staging keys expire/are cleaned up. If `[memory] maxActiveIds` is nonzero, both the request admission and the final atomic publication check the capacity. A race that fills capacity while a job is staging ends with a failed job carrying `code=HOT_CAPACITY_REACHED`; the staged data is discarded and PostgreSQL history is retained.

The Qt SDK keeps `hydrateToCache(id)` and adds `recoveryStatus(jobId)` and `requestFailedDetailed(requestId, operation, error, code)`. Write failures with a code arrive through the legacy `requestFailed` signal too, so older applications see them as failures instead of silent success. The SDK does not auto-retry writes (a lost acknowledgement could duplicate them); `examples/independent_qt_app/TelemetryBridge` shows a bounded retry for `RECOVERY_PENDING`.

`status` also reports `postgres` (client-thread connection healthy), `postgresReconnecting` and `postgresReconnects`. The client-thread connection is probed every second and re-opened with backoff up to 30 s; re-opening re-acquires the single-writer advisory lock, so a second ARS that took over the database meanwhile keeps the original one from writing. `history`/`recent` reads that race with the worker's eviction of the same ID are answered from PostgreSQL (eviction happens only after a complete archive).

`recent` uses *server receivedAtMs* and does **not** hydrate inactive IDs. Returned `fromMs/toMs` must be reused for page 2 with `nextOffset`. For old hot lists lacking a complete time index, `indexed=false` signals slower scan fallback; cold queries use the SQL time index. The old `history` operation, including sequence-based pagination, is unchanged.
