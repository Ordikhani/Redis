# راهنمای پروتکل، API و استفاده در پروژه دیگر — TCP یا UDP

## قرارداد مشترک دو پروتکل

سرور برای هر درخواست، یک شیء JSON می‌پذیرد. مقدار `requestId` باید برای هر درخواست جدید یکتا باشد. قرارداد **دستور و پاسخ** برای TCP و UDP یکسان است؛ **قاب‌بندی شبکه** متفاوت است.

```json
{"requestId":"8d6f6e90","op":"publish","args":{"channel":"radar:live","record":{"id":"TARGET-101","timestamp":1794990000000,"speed":123.5,"features":{"kind":"radar"}}}}
```

پاسخ:

```json
{"requestId":"8d6f6e90","ok":true,"result":{"stored":true,"sequence":"245","receivedAtMs":1794990000010,"storage":"redis"}}
```

خطا:

```json
{"requestId":"8d6f6e90","ok":false,"code":"HOT_CAPACITY_REACHED","error":"..."}
```

`op=hello` با `args={}` آغاز می‌شود. در TCP نشست همان اتصال Socket است؛ در UDP، `hello` endpoint را برای ۶۰ ثانیه ثبت می‌کند و SDK آن را با `heartbeat` تمدید می‌کند. **هیچ توکن یا احراز هویت کلاینت وجود ندارد.** برای UDP باید همچنان ACK/Retry/Fragmentation فعال باشد. `hello` فقط پیام شروع ارتباط است.

### تفاوت قاب‌بندی

- TCP: `QJsonDocument::Compact` با یک `LF` (`\n`) انتهای هر JSON. TCP stream است و مرز هر `read` به معنی مرز پیام نیست.
- UDP: JSON کوچک در یک datagram؛ اگر بیشتر از `maxDatagramBytes` باشد، به چند JSON با قالب زیر تقسیم می‌شود؛ طرف گیرنده بر حسب `frameId` و `index`، `payload`های base64 را کنار هم می‌گذارد:

```json
{"udpFrame":1,"frameId":"uuid","index":0,"count":3,"payload":"eyJyZXF1ZXN0SWQi..."}
```

در SDK پیش‌فرض datagram ۱۲۰۰ بایت است، درخواست/پاسخ حداکثر ۱ MiB، و حداکثر ۶۴ اسمبلیِ هم‌زمان در سرور. یک درخواست UDP با همان `requestId` می‌تواند مجدد فرستاده شود؛ تا ۱۲۰ ثانیه نتیجه در cache همان `IP:port` نگهداری می‌شود. نتیجه نامعلوم پس از crash یا جابه‌جایی پورت تضمین Exactly-once ندارد. اگر پاسخ UDP بیش از ۱ MiB شد، با `RESPONSE_TOO_LARGE` مواجه می‌شوید؛ از `limit` کوچک‌تر و pagination استفاده کنید.

### ساختار رکورد

`id` (رشته، الزامی)، `timestamp` (integer milliseconds UTC، الزامی)، `latitude`, `longitude`, `altitude`, `speed`, `heading` (عدد اختیاری)، `source` و `payload` (رشته اختیاری)، `features` (JSON object دلخواه). خروجی `ArsDataRecord::toJsonObject()` را می‌توانید بدون شناخت لایه Redis/SQL بفرستید. `receivedAtMs` توسط **سرور** تعیین می‌شود و مستقل از `timestamp` تولیدکننده است. `sequence` شناسه ترتیبی درج است.

## APIهای واقعی Server::handleRequest

| `op` | `args` | کاربرد و رفتار |
|---|---|---|
| `hello` | `{}` | شروع ارتباط بدون توکن در TCP و ثبت endpoint در UDP |
| `status` | `{}` | وضعیت Redis، PostgreSQL، worker و UDP/TCP |
| `save` | `record` | ذخیره تاریخچه، بدون انتشار live |
| `publish` | `channel`, `record` | ذخیره و سپس live event برای مشترک‌ها |
| `subscribe` / `unsubscribe` | `channel` | افزودن یا حذف مشترک live |
| `latest` | `id` | آخرین رکورد بر اساس زمان رویداد |
| `history` | `id`, `fromMs`, `toMs`, `limit`, `timeField`, `afterSequence`, `untilSequence` | صفحه تاریخچه یک ID، ترتیب sequence؛ `timeField` یکی از `timestamp/receivedAt` |
| `recent` | `id`, `lookbackSec` یا `fromMs/toMs`, `limit`, `offset` | تاریخچه بر مبنای زمان ورود سرور؛ بدون hydrate اجباری |
| `search_hot` | `mode`, `filters`, `idContains`, `cursor`, `limit`, `scanBudget` | جست‌وجوی صفحه‌ای در **RAM/Redis**؛ `mode=latest/records` |
| `delete` | `id`, `removeFromDisk` | حذف دائمی از RAM+SQL یا evict فقط RAM |
| `hydrate` / `recover` | `id` | برگرداندن تاریخچه سرد؛ ممکن است `status=queued` باشد |
| `recovery_status` | `jobId` | وضعیت job، از queued تا completed/failed |
| `heartbeat` | `{}` | تمدید lease در UDP؛ کلاینت مدیریت می‌کند |
| `ack_event` | `eventId` | اعلام دریافت Live Event توسط کلاینت UDP |
| `disconnect` | `{}` | پایان نشست UDP یا بستن TCP |

محدودیت‌ها و قواعد اعتبارسنجی موجود: `history.limit` بین ۱ تا ۱۰۰۰، `recent.limit` بین ۱ تا ۱۰۰، `search_hot.limit` بین ۱ تا ۵۰، حداکثر ۸ شرط AND، و ۶۴ اشتراک در هر peer. فیلترها می‌توانند روی `features.category`، `speed` و ... با عملیات `eq`, `ne`, `gt`, `gte`, `lt`, `lte`, `contains` باشند. داده گرم اگر checkpoint شده باشد در PostgreSQL هم باقی می‌ماند؛ `delete` دائمی تنها با درخواست صریح انجام می‌شود.

## Realtime در دو پروتکل

Event نمونه:

```json
{"event":"record","channel":"radar:live","record":{"id":"TARGET-101","timestamp":1794990000000},"sequence":"245","receivedAtMs":1794990000010,"eventId":"udp-only-event-uuid"}
```

`eventId` فقط برای UDP اضافه می‌شود. سرور به ازای هر subscriber UDP، Event را حداکثر ۵ بار با همان `eventId` می‌فرستد تا `ack_event` دریافت شود. SDK رویداد تکراری را با `eventId` فیلتر می‌کند. برای Subscriber غیرفعال، lease بعد از ۶۰ ثانیه پاک می‌شود؛ SDK هر ۱۵ ثانیه `heartbeat` می‌دهد. **تحویل Live بعد از خرابی برنامه/شبکه یا اتمام retry تضمینی نیست**؛ consumerهایی که به دقت کامل نیاز دارند، sequence دریافتی را ذخیره و با تاریخچه reconcile کنند.

## استفاده در برنامه مستقل Qt

مثال سبک؛ همان `ArsClient` و `ArsDataRecord` کوچک که داخل `producer/vendor/ars` موجود است را می‌توان در پروژه Qt جدید کپی کرد؛ Redis و PostgreSQL و کتابخانه آن‌ها در client لازم نیست. همین API برای TCP/UDP استفاده می‌شود:

```cpp
#include "ArsClient.h"
#include <QCoreApplication>
#include <QDebug>

ArsClient client;
QObject::connect(&client, &ArsClient::ready, [&] {
    client.subscribeLive("radar:live");
    ArsDataRecord record("TARGET-101");
    record.setSource("MyQtApp").setMotion(250, 90).addFeature("class", "demo");
    client.publishLive("radar:live", record);
    client.getLatest("TARGET-101");
    client.recentHistory("TARGET-101", 1800, 50);
});
QObject::connect(&client, &ArsClient::responseReceived,
    [](QString reqId, QString op, QJsonObject result) { qDebug() << reqId << op << result; });
QObject::connect(&client, &ArsClient::liveRecordDetailed,
    [](QString ch, ArsDataRecord rec, QString seq, qint64 at) { qDebug() << ch << rec.id << seq << at; });

// فقط یکی را از ابتدا انتخاب کن؛ در ادامه همه APIها همان مسیر را می‌روند:
client.connectToServer("127.0.0.1", 7092, ArsClient::Transport::Udp);
// یا: client.connectToServer("127.0.0.1", 7090, ArsClient::Transport::Tcp);
```

Qt model import در **CMake**: فایل‌های `ArsClient.cpp/h`, `ArsDataRecord.cpp/h`, `ArsGlobal.h` را به Target اضافه کنید، `Qt::Core` و `Qt::Network` را link کنید، include pathهای `vendor/ars/client` و `vendor/ars/include` را قرار دهید و `ARS_STATIC` تعریف کنید. برای **qmake**: فایل‌های `producer/producer.pro` و `consumer/consumer.pro` نمونه کامل هستند.

برای استفاده **بدون SDK**: در TCP از `QTcpSocket` با UTF-8 JSON+LF، در UDP از `QUdpSocket` با JSON datagrams یا `udpFrame` چندبخشی استفاده کنید. تمام درخواست‌ها `requestId` و `args` دارند. پس از `hello` نتیجه درخواست‌ها را با `requestId` تطبیق دهید؛ روی TCP رویدادهای `event=record` را از response جدا کنید. در UDP ACK/Retry/fragmentation/heartbeat را پیاده کنید. `tests/udp_tcp_e2e.py` یک نمونه مستقل و کوتاه با Python بدون هیچ SDK اختصاصی است.

## نکته تنظیم اندازه UDP

اگر مقدار `udp/maxDatagramBytes` را در `ServerConfig.cpp` تغییر دادید و Server را مجدداً Build کردید، کلاینت Qt را نیز با `client.setUdpDatagramSize(n)` یا گزینه `--udp-mtu n` اجرا کنید. مقدار پیش‌فرض **۱۲۰۰** برای جلوگیری از fragmentation سطح IP پیشنهاد شده است. از این امکان برای حل محدودیت ذاتی UDP در ارسال چندبخشی بسیار بزرگ استفاده نکنید؛ نتایج طولانی را صفحه‌ای درخواست دهید.
