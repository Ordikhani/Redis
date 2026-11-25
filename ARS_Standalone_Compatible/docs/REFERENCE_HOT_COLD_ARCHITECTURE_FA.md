> **مرجع تاریخی پروژه اصلی**: ممکن است از INI یا توکن استفاده کند؛ نسخه اجرایی فعلی مطابق این متن نیست.

# معماری Hot/Cold ذخیره‌سازی ARS (نسخه Redis-first)

## هدف

داده‌های ID فعال با **کل تاریخچه** در Redis نگهداری می‌شوند. PostgreSQL آرشیو دائمی و محل checkpoint دوره‌ای است. کلاینت فقط به `ars_server` متصل می‌شود و به هیچ‌کدام از دیتابیس‌ها دسترسی مستقیم ندارد.

```text
Qt / C++ Clients
       | JSON over TCP, auth
       v
   central/server/Server.cpp
       |
       v
 HotColdStore.cpp ── Redis (active / full histories / latest / lastSeen)
       |
       +────────────── PostgreSQL (inactive archives / periodic checkpoints)
```

## تعریف زمان‌ها

- `record.timestamp`: زمان **رویداد** فرستاده‌شده توسط کلاینت؛ ممکن است دیر برسد.
- `receivedAtMs`: زمان **دریافت در ARS server**، بر حسب Unix milliseconds و UTC. برای تعیین عدم فعالیت استفاده می‌شود.
- `sequence`: عدد صحیح یکتای افزایشی به شکل رشته؛ برای مرتب‌سازی دقیق و صفحه‌بندی، حتی در یک میلی‌ثانیه.
- هنگام بازیابی تاریخچهٔ قدیمی، این سه مقدار **دست‌نخورده** باقی می‌مانند؛ فقط رکورد جدید، `receivedAtMs` تازه می‌گیرد.

## چرخهٔ یک ID

1. `save` یا `publish`: اگر ID سرد باشد، تاریخچهٔ کامل آن با یک query ترتیبی از PostgreSQL خوانده و داخل یک Redis staging list قرار می‌گیرد. پس از پایان موفق، staging به‌صورت اتمیک فعال می‌شود. برای ID بدون تاریخچه، لیست خالی شروع می‌شود.
2. رکورد جدید به انتهای لیست تاریخچه افزوده می‌شود. کلید latest بر اساس `record.timestamp` (و در تساوی ترتیب دریافت) حفظ می‌شود. `lastSeen` با زمان فعلی سرور به‌روز می‌شود. این سه عمل در یک Lua script اتمیک انجام می‌شوند.
3. پاسخ `stored=true, storage=redis, checkpointed=false` به این معنی است که رکورد توسط Redis **پذیرفته شده** و هنوز لزوماً در PostgreSQL ثبت نشده است. انتشار زنده نیز پس از همین پذیرش اتفاق می‌افتد.
4. هر `checkpointEverySec` ثانیه، تمام رکوردهای ذخیره‌نشدهٔ IDهای فعال در batchهای تراکنشی به PostgreSQL افزوده می‌شوند. یک offset در Redis نشان می‌دهد چه تعداد با موفقیت checkpoint شده‌اند. ID در Redis باقی می‌ماند.
5. اگر `inactiveAfterSec` ثانیه از آخرین دریافت بگذرد، سرور ابتدا تمام رکوردهای باقی‌مانده را در PostgreSQL commit می‌کند و **فقط پس از موفقیت** تاریخچه، latest و وضعیت ID را از Redis حذف می‌کند. اگر ذخیره ناموفق باشد، داده در Redis می‌ماند و در sweep بعدی دوباره امتحان می‌شود.
6. اگر ID قدیمی دوباره برسد، تمام تاریخچهٔ آرشیو با `sequence` و `receivedAtMs` اصلی‌اش از PostgreSQL به Redis بازمی‌گردد و رکورد جدید اضافه می‌شود.

**مثال**: ID `A7` تعداد ۱۰۰۰ آپدیت دارد، در PostgreSQL قبلاً ۷۰۰ آپدیت checkpoint شده است. بک‌آپ دوره‌ای فقط ۳۰۰ مورد باقی‌مانده را commit می‌کند. بعد از ۶۰ ثانیه عدم دریافت، همهٔ ۱۰۰۰ مورد در PostgreSQL هستند و حافظهٔ Redis آزاد می‌شود. در دریافت آپدیت شماره ۱۰۰۱، همهٔ ۱۰۰۰ مورد برمی‌گردند و سپس آپدیت ۱۰۰۱ اضافه می‌شود.

## سازگاری API

نام عملیات قبلی `save`, `publish`, `latest`, `history`, `hydrate`, `delete`, `status`, `subscribe`, `unsubscribe` و متدهای کلاس `ArsClient` تغییر نکرده‌اند. تفاوت معنایی مهم: پاسخ موفق save اکنون به معنای پذیرش در Redis است نه commit در PostgreSQL. `hydrate` تاریخچهٔ **کامل** را در صورت سرد بودن به Redis برمی‌گرداند؛ `delete(removeFromDisk=false)` تاریخچهٔ فعال را ابتدا archive می‌کند و سپس از Redis خارج می‌کند. تاریخچهٔ غیرفعال از PostgreSQL، تاریخچهٔ فعال از Redis خوانده می‌شود. در نبود Redis، عملیات ذخیره مستقیماً در PostgreSQL انجام می‌شوند.

پاسخ `status` شامل `hotColdEnabled`, `inactiveAfterSec`, `checkpointEverySec`, `lastCheckpointAtMs` نیز هست.

## نمونهٔ تنظیمات

```ini
[lifecycle]
inactiveAfterSec=60
checkpointEverySec=300
scanEveryMs=1000
batchSize=100
maxBatchesPerSweep=4
```

تنظیمات فوق در `central/config/server.ini` قرار گرفته‌اند و قابل تغییرند. زمان‌های checkpoint و inactivity مستقل هستند.

## پایداری و قطع برق: بسیار مهم

**تفاوت پذیرش در Redis و ذخیرهٔ دائمی:** وقتی قبل از checkpoint پاسخ موفق می‌گیری، PostgreSQL هنوز آن رکورد را ندارد. برای جلوگیری از نابودی دادهٔ پذیرفته‌شده در قطع برق یا restart Redis، باید Redis با AOF و ترجیحاً `appendfsync always` اجرا شود. `appendfsync everysec` عملکرد بهتری دارد ولی در حادثهٔ سخت‌افزاری ممکن است داده‌های ثانیهٔ اخیر از دست بروند. همچنین Redis نباید با سیاست eviction دادهٔ فعال را حذف کند؛ از `maxmemory-policy noeviction` و RAM کافی استفاده کن. از snapshot/replication هم به عنوان حفاظت تکمیلی استفاده کن.

نمونهٔ گزینه‌های `redis.conf`:

```conf
appendonly yes
appendfsync always
maxmemory-policy noeviction
```

با قراردادن `requireDurability=true` در بخش `[redis]`، اگر بررسی تنظیمات `appendonly=yes`، `appendfsync=always` و `maxmemory-policy=noeviction` موفق نباشد، سرور به حالت PostgreSQL-only می‌رود. مقدار پیش‌فرض این گزینه برای محیط توسعه `false` است؛ در محیط تولید همراه با تنظیمات درست Redis آن را `true` بگذار.

این پروژه **تنظیمات سرویس Redis را خودکار تغییر نمی‌دهد**. AOF روی دیسک بار I/O دارد؛ پیش از استفاده با نرخ بالا اندازه‌گیری کن. در زمان در دسترس نبودن Redis سرور به write مستقیم PostgreSQL برمی‌گردد، اما بازیابی رکوردهای **هنوز checkpoint‌نشده** وابسته به دوام Redis است. اگر پاسخ Redis در میانهٔ شبکه گم شود، مشخص نیست write قبول شده یا نه: کلاینت باید قبل از retry تاریخچه را بررسی کند؛ پروتکل فعلی کلید idempotency اختصاصی ندارد.

بک‌آپ `pg_dump` موجود، بک‌آپ فیزیکی/منطقی از PostgreSQL است و **جایگزین** checkpointهای برنامه نیست. برای تهیهٔ بک‌آپ شامل رکوردهای Redis، اول باید checkpoint کامل انجام شده باشد؛ صرف فرا رسیدن زمان‌بندی تضمین نمی‌کند تراکنش‌های ناموفق یا در صف تمام شده باشند.

## محدوده‌ها و نکات عملکردی

- با درخواست «کل تاریخچه در Redis» مصرف RAM می‌تواند زیاد شود؛ هیچ `MAXLEN`/`EXPIRE` روی تاریخچه‌های فعال اعمال نشده است. ظرفیت حافظه باید بر اساس حجم واقعی رکوردها و تعداد IDهای فعال تعیین شود.
- سرور فعلی تک-thread و SQL/Redis I/O همگام است. بازیابی چندمیلیون رکورد یا checkpoint انبوه می‌تواند event loop را موقتاً متوقف کند. برای بار واقعی سنگین، worker/queue و pipeline/bulk insert، backpressure و سقف همزمانی لازم است؛ این نسخه **benchmark مقیاس بالا** نیست.
- فقط **یک instance نویسنده ARS** برای namespace مشخص Redis مجاز است. چند نویسندهٔ همزمان/چند سرور، مهاجرت اتمیک و read consistency تضمین نشده است.
- Redis `keyPrefix` را ثابت نگه دار و از Redis db مختص ARS استفاده کن. namespace `:hotv1` پایدار است تا دادهٔ ذخیره‌نشده با restart فراموش نشود. از `FLUSHDB` و حذف مستقیم کلیدهای ARS پرهیز کن.
- با اجرای PostgreSQL-only، درخواست history و latest از PG انجام می‌شود و دادهٔ realtime در Redis نگهداری نمی‌شود؛ این حالت degraded است. **اگر Redis با تاریخچهٔ checkpoint‌نشده از دسترس خارج شود و در این مدت نوشته‌های جدید مستقیم به PG بروند، پس از بازگشت/راه‌اندازی مجدد سرور تطبیق (reconciliation) دو تاریخچه به صورت کامل خودکار پیاده نشده است.** تا اعمال مکانیزم reconciliation، بررسی سلامت و بازیابی کنترل‌شده لازم است؛ این محدودیت برای محیط تولیدی مهم است.
- فایل‌های `src/*` قدیمی از جمله `ArsRedisService` هنگام `ARS_BUILD_LEGACY=OFF` در سرور مرکزی استفاده نمی‌شوند. برای ادغام در پروژه‌های دیگر از `central/client/ArsClient.h` استفاده کن.
- برای حذف کامل (`removeFromDisk=true`) رکوردهای Redis ابتدا checkpoint و از Redis خارج می‌شوند، سپس آرشیو PostgreSQL حذف می‌شود. این دو مرحله **تراکنش توزیع‌شده** نیستند؛ خطای میان مراحل نیاز به retry/تطبیق دارد.

## راه‌اندازی سریع و آزمون

مراحل نصب دیتابیس، ساخت CMake و متغیرهای توکن و رمز مشابه `README_FA.md` هستند. بعد از ساخت و اجرای Redis/PostgreSQL:

```bash
python3 central/tests/integration.py
```

برای آزمون کوتاه چرخهٔ inactivity در محیط تست، تنظیمات `inactiveAfterSec=2` و `checkpointEverySec=2` بگذار و بعد از اجرا:

```bash
ARS_RUN_LIFECYCLE=1 python3 central/tests/integration.py
```

این تست یک ID اختصاصی ایجاد می‌کند، آرشیو پس از سکوت و بازیابی کامل هنگام reactivation را می‌سنجد. از اجرای آن روی سیستم تولیدی با زمان‌بندی کوتاه پرهیز کن.


## Recovery غیرهمگام

بازیابی ID سرد در مسیر درخواست شبکه اجرا نمی‌شود. `HotColdWorker` یک صف محدود با سقف `[recovery] maxQueuedJobs` دارد و یک Job را در هر لحظه اجرا می‌کند. PostgreSQL با Keyset Pagination و صفحه‌های ۱۰۰تایی خوانده می‌شود؛ هر صفحه SQL مستقل است و Worker رکوردها را با Batch محدود و مکث قابل تنظیم در staging Redis می‌نویسد. پس از پایان کامل، Lua script تاریخچه، latest، ایندکس زمان دریافت، offset آرشیو و lastSeen را در یک عملیات اتمیک منتشر می‌کند.

در زمان بازیابی، write برای همان ID در حافظه‌ی سرور نگه داشته می‌شود و **ACK نمی‌گیرد**؛ پس از اتمام بازیابی به ترتیب رسیدن ذخیره و بعد ACK می‌شود (حداکثر ۲۵۶ write برای هر ID و ۴۰۹۶ در کل). اگر بازیابی از `[recovery] ackTimeoutMs` بیشتر شود، `ok=false` با `code=RECOVERY_PENDING` برمی‌گردد. چون هیچ write نگه‌داشته‌شده‌ای پیش از ذخیره ACK نمی‌شود، crash سرور هیچ write تأییدشده‌ای را از بین نمی‌برد؛ کلاینت فقط قطع اتصال می‌بیند که مثل هر write در جریان، نتیجه‌اش نامشخص است. حذف ID نیز تا پایان Recovery رد می‌شود. درخواست‌های سایر IDها در Event Loop اصلی قابل‌پردازش‌اند، هرچند Redis یک سرویس مشترک است و فشار I/O بازیابی می‌تواند روی latency اثر بگذارد؛ برای همین batchها محدود و بین آن‌ها مکث اعمال می‌شود.
