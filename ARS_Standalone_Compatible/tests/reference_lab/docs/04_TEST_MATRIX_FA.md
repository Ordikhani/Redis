> **مستند قدیمی**: مسیرهای INI/Build در این سند منسوخ‌اند. به `tests/reference_lab/run_lab.sh` جدید و `docs/13_COMPILED_CONFIG_FA.md` مراجعه کن.

# ماتریس کامل آزمایش و معیار قبولی سرویس

## روش آزمایش

ابتدا با `run_lab.sh up` و `run_lab.sh build` اجرا کن و سرور را در یک ترمینال با `run_lab.sh server` فعال نگه دار. سپس **بدون Sender** `./central/lab/run_lab.sh test` را اجرا کن. `test` ابتدا تست‌های مستقل Python و سپس سه تست سیاه‌جعبه پروتکل TCP را اجرا می‌کند؛ همه تست‌های شبکه مستلزم ARS/Redis/PostgreSQL فعال هستند. اجرای تست روی دیتابیس production **ممنوع** است. آزمایش‌ها از ID با پیشوند تصادفی و پاک‌سازی کنترل‌شده استفاده می‌کنند.

| کد | سناریو | انتظار |
|---|---|---|
| T01 | اتصال با token اشتباه | سرور authentication را رد کند |
| T02 | دو کلاینت مستقل متصل شوند | هر دو `ready` داشته باشند |
| T03 | دو رکورد با ID یکسان | هر دو حفظ شوند و `sequence` متفاوت باشد |
| T04 | `publish` و یک subscriber | گیرنده Live Event با Channel درست بگیرد |
| T05 | `save` بدون publish | تاریخچه تغییر کند ولی Live Event ارسال نشود |
| T06 | `latest` | آخرین وضعیت هدف بازگردد |
| T07 | `search_hot(mode=latest)` | جست‌وجوی همه IDهای فعال |
| T08 | `search_hot(mode=records)` | جست‌وجوی رکوردهای Hot با صفحه‌بندی |
| T09 | فیلتر `speed`, `source`, `features` | تنها رکوردهای منطبق بازگردند |
| T10 | تاریخچه 30m / 1h و بازه سفارشی | فقط رکوردهای دارای receivedAtMs در بازه |
| T11 | زمان رویداد قدیمی | receivedAtMs همچنان زمان دریافت واقعی سرور باشد |
| T12 | صفحه‌بندی `history` | رکوردهای تکراری/گم‌شده بین صفحات نباشند |
| T13 | ID فعال همزمان با checkpoint | در Redis بماند و SQL نیز رکورد جدید بگیرد |
| T14 | ID ساکت با timeout=8s | `[ARCHIVED]`، تاریخچه در SQL و نبودن Hot |
| T15 | خواندن `recent` از ID سرد | منبع `postgres`، عدم hydrate کامل |
| T16 | دوباره فعال شدن ID سرد | `[RESTORED]` و بازگشت تمام سابقه به Redis |
| T17 | بیش از ۱۰۰ رکورد ID واحد | checkpoint در batchهای متعدد با حفظ sequence |
| T18 | `evict` دستی | قبل از حذف cache، SQL کامل و قابل بازیابی باشد |
| T19 | `hydrate` دستی | history دوباره از Redis خوانده شود |
| T20 | `deleteById(id,true)` | فقط ID انتخابی در هر دو مخزن حذف شود |
| T21 | ورودی نامعتبر / limit غیرمجاز | پاسخ error و بدون crash |
| T22 | backup و `verify_backup.py` | archive.pgcustom، redis.rdb و hash معتبر |
| T23 | purge هنگام فعال بودن سرور | ابزار عملیات را رد کند |
| T24 | قطع Redis در حین پذیرش داده | fallback یا failure ثبت شود و lost ACK صادقانه گزارش شود؛ بازآشتی خودکار کامل تضمین‌شده نیست |
| T25 | بنچمارک نویسنده/خواننده و checkpoint | throughput، latency P50/P95/P99 با داده واقعی سنجیده شوند |

تست‌های خودکار `integration.py`، `hot_cold_end_to_end.py` و `lab/test_contract.py` بیشتر سناریوهای T01 تا T21 را پوشش می‌دهند؛ T22 هم نیازمند اجرای دستی بک‌آپ واقعی است. **T24 و T25 سناریوهای مهندسی/بار با نتیجه وابسته به محیط هستند و آزمون موفقیت کامل production محسوب نمی‌شوند.**

## آزمایش دستی ارسال و بازگشت ID

ترمینال گیرنده: `./central/lab/run_lab.sh receiver`.

ترمینال تولیدکننده: `./central/lab/run_lab.sh sender`.

ترمینال سرور: `./central/lab/run_lab.sh server` (از قبل روشن).

به لاگ زیر توجه کن (شکل تقریبی است، اعداد دقیق وابسته به زمان اجرا هستند):

```text
[NEW_HOT] id=LAB-COLD-001 storage=Redis
[CHECKPOINT] id=LAB-COLD-001 destination=PostgreSQL records=...
[ARCHIVED] id=LAB-COLD-001 source=Redis destination=PostgreSQL ...
[RESTORED] id=LAB-COLD-001 source=PostgreSQL destination=Redis historyRecords=...
```

**تفاوت تأیید مهم:** دیدن log کافی نیست؛ `recent LAB-COLD-001 30m` قبل از آرشیو باید `source=redis` و در وقفه `source=postgres` داشته باشد. بعد از بازگشت ID باید دوباره `source=redis` گزارش شود. در ضمن `sequence` و `receivedAtMs` رکوردهای قدیمی باید تغییر نکرده باشند.

## مشاهده دیسک و بک‌آپ

```bash
./central/lab/run_lab.sh inspect
./central/lab/run_lab.sh backup
# مسیر دقیق چاپ‌شده توسط backup را بردار:
python3 central/lab/verify_backup.py central/lab/backups/<folder-created-by-backup>
```

**در آزمایش بدون خطر، هیچ دستور عمومی برای پاک‌کردن تمام دیسک اجرا نمی‌شود.** اگر فقط آرشیو PostgreSQL مربوط به ARS را می‌خواهی پاک کنی، ابتدا از دیتابیس آزمایش backup معتبر بگیر، ARS server را با Ctrl+C متوقف کن، `run_lab.sh stop` را **اجرا نکن** (چون Redis و PostgreSQL باید روشن بمانند)، بعد مطابق راهنمای موجود `central/scripts/purge_archive.py` با پیکربندی `central/lab/server.lab.ini` و عبارت دقیق `--confirm 'DELETE ALL ARS ARCHIVE'` اقدام کن. این عملیات رکوردهای Cold را برای همیشه از PostgreSQL حذف می‌کند؛ Hot موجود در Redis با checkpoint بعدی مجدداً آرشیو می‌شود و رکوردهای Cold حذف‌شده غیرقابل بازگشت‌اند، مگر از بک‌آپ سالم. **این کار محتوای تمام دیسک Redis، WAL، AOF یا سایر دیتابیس‌ها را حذف نمی‌کند** و secure erase محسوب نمی‌شود.

## تست‌های مستقلاً موفق‌شده در محیط تهیه‌کننده

۸ تست Python موجود مربوط به ساخت DB و ایمنی ابزارهای backup/purge اجرا شدند و موفق بودند. `py_compile` و `bash -n` برای فایل‌های جدید پاس شدند. برای C++ در این محیط Qt SDK یافت نشد و Docker Compose/Redis/PostgreSQL عملیاتی در دسترس نبود؛ بنابراین ساخت GUI، تست‌های شبکه و performance در اینجا **اجرای واقعی/تأییدشده محسوب نمی‌شوند**. روی محیط خودت ابتدا T01 تا T23 را در sandbox تأیید کن، سپس سراغ ارزیابی بار برو.

## برنامه ارزیابی سرعت

هنگام تست کارایی Sender/Receiver تعاملی را متوقف کن؛ سپس اسکریپت قبلی `central/tests/ars_benchmark.py` را با گزینه‌های راهنمای خودش (`--help`) اجرا کن. پورت تست را تنظیم کن و معیارهای throughput، درصد timeout و P95/P99 را در دو حالت بدون checkpoint و هنگام checkpoint مقایسه کن. **بدون عدد واقعی از سخت‌افزار و بار، برای سرویس realtime نرخ قطعی اعلام نمی‌کنیم.**

## آزمون راه‌اندازی جدید
فایل central/tests/test_startup.py با ARS_SERVER_BINARY و ARS_TEST_CONFIG فعال می‌شود. پوشش: اجرای check از پوشه دیگر، تنظیمات محیطی، آرگومان قدیمی، فایل ناموجود، رمز نسبی، خطاهای همزمان وابستگی، پورت اشغال و اعتبارسنجی آرگومان. گزارش اجرای واقعی در central/VALIDATION_STARTUP_FA.md قرار دارد.
