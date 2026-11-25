# معماری واقعی ARS — نسخه بدون توکن (TCP و UDP)

این مستند بر پایه سورس اصلاح‌شده همین ZIP نوشته شده است. هر کلاینت از ابتدا **TCP یا UDP** را انتخاب می‌کند و تمام APIها را با همان transport ادامه می‌دهد. `Server::handleRequest` مسیر مشترک هر دو است. Redis و PostgreSQL فقط از Server/Worker قابل دسترسی هستند.

## کلاس‌ها و مسئولیت‌ها

| فایل/کلاس | مسئولیت |
|---|---|
| `server/server/main.cpp` | اجرای `--check`، شروع Server با تنظیمات کامپایل‌شده |
| `Startup` | خواندن `ServerConfig::instance()` و بررسی اتصال Redis/PostgreSQL و پورت‌ها |
| `Server` | Listenerهای TCP و UDP، پردازش JSON، `hello` بدون رمز، dispatch، Live و ACK |
| `HotColdStore` | حفظ تاریخچه گرم و عملیات `save`, `publish`, `latest`, `history`, `recent`, `search_hot` |
| `HotColdWorker` | checkpoint، archive، recover و پیگیری Worker |
| `ArsRedisClient` | دستورات Redis از سمت سرور |
| `PostgresStore` | اتصال QPSQL با رمز کامپایل‌شده از ServerConfig؛ تاریخچه سرد |
| `ArsDataRecord` | مدل داده مستقل از لایه شبکه |
| `ArsClient` در Producer/Consumer | API یکسان و انتخاب TCP/UDP، handshake بدون token |

**نکته:** `hello` صرفاً اعلام شروع ارتباط است و احراز هویت انجام نمی‌دهد. پورت‌های پیش‌فرض فقط روی `127.0.0.1` باز می‌شوند؛ دسترسی LAN را در صورت نیاز با Firewall کنترل کنید.

## ۱. Server Startup & Redis Initialization

شروع سرویس، خواندن مقادیر کامپایل‌شده از `ServerConfig`، بررسی Redis و PostgreSQL، ساخت Worker و بازکردن Listenerها.

```mermaid
sequenceDiagram
    autonumber
    participant Main as main.cpp
    participant Cfg as ServerConfig::instance
    participant Startup as Startup::check
    participant Server as Server::start
    participant Store as HotColdStore::open
    participant Worker as HotColdWorker::run
    participant Redis as ArsRedisClient / Redis
    participant PG as PostgresStore / PostgreSQL
    Main->>Startup: check()
    Startup->>Cfg: value(postgres/*, redis/*, server/*, udp/*)
    Cfg-->>Startup: Compiled values, including SQL credential
    Startup->>PG: QPSQL open + SELECT 1
    PG-->>Startup: Query OK
    Startup->>Redis: CONNECT, optional Redis AUTH, PING
    Redis-->>Startup: PONG
    Startup->>Startup: Probe TCP/UDP listening ports
    Startup-->>Main: ready=true
    Main->>Server: start()
    Server->>Cfg: instance()
    Server->>Store: open(config)
    Store->>PG: open(config), writer lock, migrate
    Store->>Redis: connect + durability check
    Store->>Worker: start()
    Worker->>Cfg: instance() (same compiled values)
    Worker->>PG: open(config, no writer lock)
    Worker->>Redis: connect
    Store-->>Server: initialized
    Server->>Server: QTcpServer.listen(compiled host/port)
    Server->>Server: QUdpSocket.bind(compiled host/port)
    Server-->>Main: [READY] listeners

```

## ۲. TCP Producer/Consumer Communication

هر کلاینت از همان TCP برای hello، Subscribe، Publish و Query استفاده می‌کند. انتشار Live بعد از ثبت موفق انجام می‌شود.

```mermaid
sequenceDiagram
    autonumber
    participant P as Producer / ArsClient
    participant T as QTcpServer / Server
    participant S as HotColdStore
    participant Redis as ArsRedisClient / Redis
    participant W as HotColdWorker
    participant PG as PostgresStore / PostgreSQL
    participant C as Consumer / ArsClient
    P->>T: TCP connect to 7090
    C->>T: TCP connect to 7090
    P->>T: JSON-LF hello({})
    T-->>P: ok protocol=1
    C->>T: JSON-LF hello({})
    T-->>C: ok protocol=1
    C->>T: JSON-LF subscribe(channel)
    T-->>C: subscribed=true
    P->>T: JSON-LF publish(channel,record)
    T->>S: save(record,receivedAtMs)
    alt Hot or new ID
        S->>Redis: EVAL append record/history
        Redis-->>S: append OK
    else Cold archived ID
        S->>W: enqueueRecovery(id)
        W->>PG: read archived history in batches
        PG-->>W: archived records
        W->>Redis: stage + publish restored history
        loop Server::processDeferred timer (25 ms)
            T->>S: recoveryStatus(jobId)
            S->>W: recoveryStatus(jobId)
            W-->>S: completed
            S-->>T: completed + restoredCount
        end
        T->>S: save held record
        S->>Redis: EVAL append
        Redis-->>S: append OK
    end
    S-->>T: result stored=true
    T-->>P: response ok=true + sequence
    T-->>C: event record(channel,record,sequence)
    C->>T: JSON-LF recent(id,lookbackSec=1800)
    T->>S: recent(args)
    S->>Redis: read indexed hot records (when active)
    opt ID inactive/cold
        S->>PG: query timed history (without hydrate)
    end
    S-->>T: items + pagination
    T-->>C: response ok=true result.items
```

## ۳. UDP Data Flow

هر کلاینت UDP با hello endpoint خود را ثبت می‌کند؛ همان فرمان‌های اصلی و پاسخ‌ها در UDP اجرا می‌شوند. ACK/Retry و fragmentation برقرار است.

```mermaid
sequenceDiagram
    autonumber
    participant P as Producer / ArsClient UDP
    participant U as Server::readUdp / QUdpSocket
    participant H as Server::handleRequest
    participant S as HotColdStore
    participant Redis as Redis / PostgreSQL
    participant C as Consumer / ArsClient UDP
    P->>U: UDP hello(requestId,args={})
    U->>H: handleRequest(nullptr,sender,port,packet)
    H->>H: Register producer endpoint for 60s lease
    H-->>P: ok protocol=1, transport=udp
    C->>U: UDP hello(requestId,args={})
    U->>H: handleRequest hello, register endpoint
    H-->>C: ok protocol=1
    C->>U: UDP subscribe(channel)
    U->>H: handleRequest subscribe
    H-->>C: subscribed=true
    P->>U: UDP publish(channel,record)
    opt JSON exceeds maxDatagramBytes
        P->>U: udpFrame chunks(frameId,index,count,payload)
        U->>U: Reassemble JSON payload
    end
    U->>U: Verify endpoint and requestId cache/pending
    U->>H: handleRequest publish
    H->>S: save(record,receivedAtMs)
    S->>Redis: Append to hot Redis or fallback PostgreSQL
    Redis-->>S: sequence, stored=true
    S-->>H: save result
    H-->>P: UDP response (fragmented if needed)
    H-->>C: event=record + eventId
    C->>U: ack_event(eventId)
    U->>U: Remove live pending event
    opt Request retry after packet loss
        P->>U: Same requestId, retry packet
        U-->>P: Cached response if present
    end
    C->>U: UDP recent/history/search_hot
    U->>H: Shared command handler
    H-->>C: UDP result (possibly fragmented)
```

## ۴. Realtime Publish/Subscribe

Consumer ابتدا مشترک می‌شود. Server پس از ذخیره publish، برای TCP یک event و برای UDP event دارای eventId می‌فرستد.

```mermaid
sequenceDiagram
    autonumber
    participant C as Consumer / ArsClient
    participant S as Server::handleRequest
    participant P as Producer / ArsClient
    participant Store as HotColdStore
    participant Redis as Redis
    C->>S: hello then subscribe(channel)
    S-->>C: subscribed=true
    P->>S: publish(channel,record)
    S->>Store: save(record)
    Store->>Redis: append history + latest
    Redis-->>Store: stored sequence
    Store-->>S: stored=true
    S-->>P: ok + receivedAtMs + sequence
    alt Consumer selected TCP
        S-->>C: JSON-LF event=record
    else Consumer selected UDP
        S-->>C: UDP event=record + eventId
        alt event received
            C->>S: ack_event(eventId)
            S-->>C: acknowledged=true
        else ACK absent
            S-->>C: retry same eventId, bounded attempts
            C->>C: duplicate-event suppression by eventId
        end
    end
    Note over C,Redis: save without publish does NOT emit live events
```

## ۵. Client Disconnect & Reconnection

Timeout و reconnect با همان پروتکل انتخاب‌شده انجام می‌شود. اشتراک‌های نگهداری‌شده بعد از hello دوباره ثبت می‌شوند.

```mermaid
sequenceDiagram
    autonumber
    participant Client as ArsClient / Producer or Consumer
    participant Timer as ArsClient timeout timer
    participant Server as Server
    Client->>Server: hello({}), subscribe(channel)
    Server-->>Client: hello OK, subscribed=true
    alt TCP transport
        Server--xClient: network disconnect
        Client->>Client: disconnected, pending requests fail
        Timer->>Client: reconnect every ~4s (if not manual)
        Client->>Server: TCP connect then hello
    else UDP transport
        Timer->>Server: heartbeat every ~15s
        Server--xTimer: heartbeat response lost
        Timer->>Timer: after 45s without response mark not ready
        Timer->>Server: retry hello over same UDP endpoint
    end
    Server-->>Client: hello OK
    Client->>Server: restore remembered subscriptions
    Server-->>Client: subscribed=true
    Client->>Server: history/recent query for gap recovery (application decision)
    Server-->>Client: durable history pages
    Note over Client,Server: Manual disconnectFromServer disables automatic reconnect
```

## آزمون و محدودیت

این Sequence Diagramها مسیرهای موجود در **کد اصلاح‌شده** را نشان می‌دهند؛ Build واقعی Qt 6.7 و اجرای end-to-end با Redis/PostgreSQL هنوز در محیط حاضر انجام نشده است. UDP تضمین تحویل سراسری ندارد. بازیابی gap رویدادهای Live با درخواست `recent/history` تصمیم برنامه مصرف‌کننده است و SDK آن را به‌صورت خودکار تضمین نمی‌کند.
