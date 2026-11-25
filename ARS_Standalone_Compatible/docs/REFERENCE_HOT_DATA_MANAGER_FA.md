> **مرجع تاریخی پروژه اصلی**: ممکن است از INI یا توکن استفاده کند؛ نسخه اجرایی فعلی مطابق این متن نیست.

# راهنمای سرویس مرکزی مدیریت دیتای گرم ARS (نسخه توسعه)

## ۱. ایده اصلی

```
Sender (Qt/C++) -- TCP + JSON --> ARS Server
                                      |
                       +--------------+--------------+
                       |                             |
                       v                             v
               Redis Hot RAM                 PostgreSQL Archive
             - IDهای فعال                  - IDهای غیرفعال
             - کل History فعال             - checkpoint دوره‌ای
             - جست‌وجوی RAM               - تاریخچه با SQL index
             - ایندکس زمان دریافت          - بک‌آپ با pg_dump
                       |
              Receiver / Query Client
```

**زمان `receivedAtMs`** زمان ورود پیام به سرور ARS است. `record.timestamp` زمان رویداد تولیدکننده است. فیلتر `recent` فقط با **زمان دریافت** کار می‌کند؛ ساعت کلاینت و زمان رویداد نمی‌توانند آن را تغییر دهند.

تا وقتی ID پیام دریافت می‌کند در Redis است؛ checkpoint دوره‌ای، رکوردهای جدیدش را در PostgreSQL نیز می‌نویسد و آن را از Redis پاک نمی‌کند. بعد از مدت عدم فعالیت، آخرین رکوردهای ثبت‌نشده ذخیره و ID از RAM خارج می‌شود. یک درخواست تاریخچه برای ID غیرفعال مستقیماً از PostgreSQL پاسخ داده می‌شود: **داده قدیمی بی‌جهت به Redis بازنمی‌گردد**. در عوض، با دریافت *پیام جدید* از ID غیرفعال، طبق قرارداد قبلی پروژه کل تاریخچه آن ID از PostgreSQL بازسازی می‌شود. این کار برای ID بسیار بزرگ سنگین است و در نسخه بعدی بهتر است قابل تنظیم شود.

## ۲. Redis خودش جست‌وجو دارد؟

بله. `SCAN` برای پیمایش مجموعه کلیدها، `ZRANGEBYSCORE` برای ایندکس‌های زمانی و **Redis Search / Query Engine** برای جست‌وجوی پیشرفته ایندکس‌شده وجود دارد. این نسخه برای ساده‌ماندن نیازمند Redis Search نیست.

در ARS:

- `search_hot` با `mode=latest` آخرین رکورد **تمام IDهای فعال** را می‌گردد.
- `search_hot` با `mode=records` **تمام رکوردهای** IDهای فعال در RAM را می‌گردد، حتی اگر ID تکراری داشته باشند.
- `recent` تاریخچه **فقط یک ID** را در بازه زمانی درخواست‌شده برمی‌گرداند؛ اگر ID در RAM باشد از ایندکس زمانی Redis، در غیر این‌صورت از ایندکس زمانی PostgreSQL استفاده می‌کند.
- `history` همچنان API قبلی است و بازه `fromMs/toMs` و `timeField` را دارد؛ سازگاری کلاینت قدیمی حفظ شده است.

**محدودیت سرعت:** فیلتر دلخواه `search_hot` یک جست‌وجوی صفحه‌بندی‌شده روی داده‌های گرم است؛ **ایندکس عمومی روی تک‌تک فیلدهای JSON نیست** و برای پیدا کردن همه نتایج باید همه صفحه‌ها بررسی شوند. در این نسخه هر درخواست حداکثر `scanBudget=200` رکورد را بررسی می‌کند (پیش‌فرض ۱۲۰)، `limit` بین ۱ تا ۵۰ است و سقف خروجی تقریباً ۳ MiB دارد. برای میلیون‌ها رکورد و فیلتر زیاد، Redis Search یا ایندکس اختصاصی فیلدهای پرتکرار لازم است. درخواست زنده همچنان از همان Event Loop سرور و Redis همگام عبور می‌کند؛ تأخیر کم تضمین‌شده نیست.

### کلیدهای Redis (برای فهمیدن کد)

| کلید | نوع | هدف |
|---|---|---|
| `<prefix>:hotv1:lastSeen` | Sorted Set | آخرین زمان دریافت IDهای فعال؛ تشخیص ID غیرفعال و پیمایش |
| `<prefix>:hotv1:history:<base64id>` | List | رکوردهای کامل یک ID به ترتیب ورود |
| `<prefix>:hotv1:latest:<base64id>` | String JSON | آخرین وضعیت انتخاب‌شده با زمان رویداد |
| `<prefix>:hotv1:recvIndex:<base64id>` | Sorted Set | score=receivedAtMs و member=offset لیست برای بازه زمانی |
| `<prefix>:hotv1:synced` | Hash | تعداد رکوردهای checkpoint شده هر ID؛ مانع از حذف ناایمن |

در نسخه‌هایی که قبلاً داده در Redis دارند، برخی IDها فاقد `recvIndex` کامل هستند. برای آن‌ها `recent` موقتاً به خواندن لیست و فیلتر کردن زمان برمی‌گردد (`indexed:false`). پس از یک چرخه آرشیو و فعال‌شدن مجدد، ایندکس ساخته می‌شود. **بدون دلیل Redis را FLUSHDB نکنید؛ داده‌های checkpoint نشده از دست می‌روند.**

## ۳. درخواست‌های شبکه (TCP JSON هر پیام یک خط)

ابتدا مانند قبل با `auth` و `ARS_API_TOKEN` احراز هویت شود. همه دستورات زیر از طریق **ARS** انجام می‌شوند و کلاینت به PostgreSQL یا Redis مستقیم وصل نمی‌شود.

جست‌وجوی آخرین رکوردهای تمام IDهای فعال:

```json
{"requestId":"s1","op":"search_hot","args":{"mode":"latest","idContains":"TARGET","filters":[{"field":"speed","op":"gte","value":200}],"limit":20,"scanBudget":120}}
```

جست‌وجو روی همه رکوردهای حاضر در RAM:

```json
{"requestId":"s2","op":"search_hot","args":{"mode":"records","filters":[{"field":"features.phase","op":"eq","value":"normal"}],"cursor":"0:0","limit":20}}
```

اپراتورها: `eq`, `ne`, `gt`, `gte`, `lt`, `lte`, `contains`. مسیر فیلد می‌تواند مثل `features.counter`، `source`، `speed`، `timestamp` یا `receivedAtMs` باشد. می‌توان حداکثر ۸ شرط گذاشت؛ شرط‌ها با AND ترکیب می‌شوند. جواب شامل `items`, `hasMore`, `nextCursor`, `scanned`, `hotIds` است. برای صفحه بعد `nextCursor` را بفرستید تا `hasMore=false` شود. **چون داده زنده تغییر می‌کند، Cursor جست‌وجوی سراسری Snapshot ثابت نیست؛ با تغییر IDهای فعال احتمال تکرار یا جاافتادگی وجود دارد.**

تاریخچه نیم‌ساعت گذشته بر اساس ساعت **سرور**:

```json
{"requestId":"r1","op":"recent","args":{"id":"TARGET-101","lookbackSec":1800,"limit":50}}
```

یک ساعت گذشته:

```json
{"requestId":"r2","op":"recent","args":{"id":"TARGET-101","lookbackSec":3600,"limit":50}}
```

بازه مشخص بر اساس میلی‌ثانیه از Unix epoch:

```json
{"requestId":"r3","op":"recent","args":{"id":"TARGET-101","fromMs":1700000000000,"toMs":1700003600000,"offset":0,"limit":50}}
```

پاسخ `recent` به این شکل است:

```json
{"requestId":"r1","ok":true,"result":{"source":"redis","indexed":true,"fromMs":1700000000000,"toMs":1700001800000,"items":[{"record":{"id":"TARGET-101","timestamp":1700000000000},"sequence":"128","receivedAtMs":1700000100000}],"hasMore":false,"nextOffset":1}}
```

برای صفحه بعد **دقیقاً** همان `fromMs` و `toMs` برگشتی همراه `offset=nextOffset` ارسال شود؛ در غیراین‌صورت پنجره زمانی با ساعت جدید تغییر می‌کند. ترتیب `recent` بر اساس زمان دریافت است (و در زمان مساوی ترتیب رکورد)، نه الزاماً `timestamp` داخل رکورد. حداکثر `limit=100` و `lookbackSec=2592000` (۳۰ روز) در حالت lookback است؛ بازه‌های صریح تاریخی محدود به این مدت نیستند. کلاینت لازم نیست آن همه تاریخچه را به RAM خود بار کند.

نکته: `recent` یک ID غیرفعال را خودکار به Redis منتقل نمی‌کند. دستور `hydrate` یا ارسال داده جدید، بازیابی غیرهمگام را در Worker صف می‌کند. برای write شناسه سرد، سرور پاسخ را تا پایان بازیابی نگه می‌دارد و بعد از ذخیره ACK می‌دهد؛ اگر بیش از `ackTimeoutMs` طول بکشد، `ok=false` با `code=RECOVERY_PENDING` برمی‌گردد و رکورد ذخیره نشده است.

## ۴. APIهای سادهٔ Qt

```cpp
client.searchHot("latest", QJsonArray{QJsonObject{{"field", "speed"},
                            {"op", "gte"}, {"value", 200}}});
client.searchHot("records", {}, "TARGET");
client.recentHistory("TARGET-101", 1800);  // نیم ساعت گذشته
client.recentHistory("TARGET-101", 3600);  // یک ساعت گذشته
// جواب در responseReceived(requestId, operation, result) دریافت می‌شود.
```

برای صفحه بعد تاریخچه:

```cpp
client.recentHistoryPage("TARGET-101", fromMs, toMs, nextOffset);
```

روش قدیمی `queryHistory(...)` و بقیه APIها تغییر نکرده‌اند.

### اجرای Receiver با دستورهای قابل فهم

پس از ساخت و راه‌اندازی پروژه:

```bash
./build/central/ars_test_receiver --port 7091
```

در ترمینال گیرنده:

```text
hot latest
hot records TARGET
hotnext
filter latest speed gte 200
filter records features.counter gt 10
recent TARGET-101 30m
recent TARGET-101 1h
recentnext TARGET-101
today TARGET-101
history TARGET-101 100
next TARGET-101
status
```

`today` از نیمه‌شب **منطقه زمانی سیستم اجرای گیرنده** تا لحظه درخواست است؛ زمان دریافتِ رکوردها همچنان زمان سرور است.

## ۵. بک‌آپ PostgreSQL و Redis روی دیسک

این عملیات برای ادمین سرور در نظر گرفته شده است، **نه** یک RPC عمومی قابل دسترسی با توکن هر کلاینت. بک‌آپ PostgreSQL فقط داده‌های checkpoint/آرشیوشده را دارد؛ برای ثبت داده‌های گرمِ checkpoint نشده باید Snapshot از Redis هم گرفته شود.

```bash
# رمزها را از طریق محیط امن تنظیم کن؛ در فایل سورس ذخیره نشوند.
export ARS_DB_PASSWORD='...'
export ARS_REDIS_PASSWORD='...'
export ARS_REDIS_HOST=127.0.0.1
export ARS_REDIS_PORT=6379
# پوشه مقصد باید هنوز وجود نداشته باشد.
python3 central/scripts/backup_all.py /srv/ars-backups/run-001
```

در خروجی: `archive.pgcustom`، `redis.rdb` و `manifest.json` شامل SHA256 فایل‌ها ایجاد می‌شوند. پیش‌نیاز: `pg_dump`, `redis-cli` و مجوز نوشتن در مقصد. **`redis-cli --rdb` کل Redis instance و همه DBهای منطقی آن را Snapshot می‌گیرد**؛ برای جداسازی کامل از Redis مستقل برای ARS استفاده کنید.

فقط آرشیو PostgreSQL (حاوی داده‌های فعال checkpoint‌نشده نیست):

```bash
python3 central/scripts/backup_all.py /srv/ars-backups/archive-001 --postgres-only
```

Snapshot این دو سامانه **اتمیک و همزمان** نیست؛ برای بازیابی دقیق، تولید داده را موقتاً متوقف کن و بک‌آپ را در محیط تست Restore و تطبیق تاریخچه کن. همچنین بک‌آپ دیتابیس با `checkpointEverySec` فرق دارد: checkpoint اطلاعات را در دیتابیس عملیاتی ثبت می‌کند، نه در یک فایل بک‌آپ مجزا.

## ۶. حذف **همه رکوردهای آرشیوِ دیسکی** (مهم)

در این پیاده‌سازی، «پاک‌سازی آرشیو» یعنی `TRUNCATE ars_records` در PostgreSQL. **RAM پاک نمی‌شود** و ایندکس شمارنده checkpoint قبل از حذف از Redis پاک می‌شود. IDهای فعالی که در RAM مانده‌اند در checkpoint بعدی دوباره به آرشیو برمی‌گردند. رکوردهای فقط در آرشیو (IDهای غیرفعال) حذف می‌شوند و دیگر قابل بازگشت نیستند.

۱. از فایل‌های بک‌آپ به‌صورت مستقل نسخه ایمن تهیه کن.
۲. سرویس `ars_server` را **متوقف** کن؛ Redis و PostgreSQL باید هنوز روشن باشند.
۳. تنظیمات و رمزهای اتصالات را بررسی کن.
۴. دستور زیر را با تأیید صریح اجرا کن:

```bash
python3 central/scripts/purge_archive.py \
    --config central/config/server.ini \
    --confirm 'DELETE ALL ARS ARCHIVE'
```

۵. سرویس ARS را راه‌اندازی کن. جدول باقی می‌ماند، شماره sequence ریست نمی‌شود و داده‌های فعال قابل checkpoint مجدد هستند.

ابزار اگر سرویس TCP ARS یا اتصال PostgreSQL با نام `ars_server` فعال باشد، عملیات را **رد** می‌کند. اگر Redis در دسترس نباشد، عملیات هم انجام نمی‌شود. این ابزار **پاک‌سازی فیزیکیِ غیرقابل‌بازیابی** نیست: فایل‌های بک‌آپ، WAL دیتابیس، Redis AOF/RDB و بلاک‌های قدیمی دیسک همچنان ممکن است داده را نگه دارند. برای حذف امن فیزیکی، رویه امنیتی مجزا بر اساس نوع دیسک و سیاست نگهداری لازم است. همچنین اسکریپت را فقط در محیط مدیریتی امن و روی نمونه Redis/PostgreSQL تعریف‌شده در INI اجرا کن.

## ۷. تست و محدودیت‌های نسخه

```bash
python3 -m unittest discover central/tests -p 'test_*.py' -v
ARS_TEST_PORT=7091 python3 central/tests/hot_cold_end_to_end.py
```

قبل از تست یک PostgreSQL مستقل (`ars_test`) و پیشوند Redis مستقل مطابق `server.test.ini` بساز، `ARS_API_TOKEN` حداقل ۳۲ بایتی تنظیم کن و `ars_server` را با `server.test.ini` اجرا کن. تست End-to-End شامل این حالات است: جست‌وجوی تمام IDهای فعال، جست‌وجوی تمام رکوردهای گرم، صفحه‌بندی، تاریخچه ۳۰ دقیقه اخیر، خروج ID غیرفعال از Redis، درخواست تاریخچه آرشیوی بدون restore، برگشت ID و بازسازی ایندکس زمانی.

محدودیت‌ها: عملیات Redis مسیر اصلی هنوز همگام و دارای timeout محدود است؛ checkpoint و recovery در Worker جدا از Event Loop کلاینت اجرا می‌شوند؛ جست‌وجوی فیلد دلخواه global index ندارد، بازسازی ID بزرگ در حافظهٔ محلی ARS به Batch محدود است، اما Redis باید فضای کافی برای نگهداری کل تاریخچهٔ فعال را داشته باشد، Cursor در وضعیت زنده Snapshot تراکنشی نیست، قطع Redis پس از fallback به PostgreSQL هنوز با تطبیق کامل خودکار recovery نشده است. همچنین مجموعه بک‌آپ PostgreSQL+Redis در حال نوشتن، restore تضمین‌شده بدون از دست‌رفتگی/دوباره‌نویسی ندارد. برای محیط عملیاتی حساس، این موارد باید برطرف شوند.
