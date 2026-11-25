> **مرجع تاریخی پروژه اصلی**: ممکن است از INI یا توکن استفاده کند؛ نسخه اجرایی فعلی مطابق این متن نیست.

# اصلاحات قابلیت اطمینان Worker

این سند مخصوص نسخه اصلاحی پس از `ARS_HotDataManager_AsyncRecovery_ReleaseCandidate` است. گزارش‌های Validation قبلی، نتیجه تست این نسخه محسوب نمی‌شوند.

## اصلاحات کد

1. **Timeout ردیس:** در خطای دستور Worker، اتصال قطع می‌شود. `ArsRedisClient::disconnectFromHost()` اکنون از `QTcpSocket::abort()` استفاده می‌کند تا پاسخ دیرهنگام/باقی‌مانده در بافر سوکت نتواند پاسخ دستور بعدی تلقی شود. Worker پس از آن اتصال تازه می‌سازد.
2. **Race بین append و eviction:** اسکریپت Lua افزودن، در همان عملیات اتمیک بررسی می‌کند که اگر caller شناسه را فعال دیده، marker آن هنوز در `lastSeen` وجود داشته باشد. اگر Worker قبلاً شناسه را آرشیو و حذف کرده باشد، append رد می‌شود و مسیر Recovery اجرا می‌شود؛ رکورد جدید تا تکمیل Recovery ACK نمی‌گیرد.
3. **اتصال مجدد PostgreSQL:** Worker سلامت PostgreSQL را دوره‌ای بررسی می‌کند و در صورت قطع ارتباط، اتصال خودش را با backoff افزایشی تا ۳۰ ثانیه دوباره می‌سازد. سلامت Worker فقط وقتی true است که هر دو اتصال Redis و PostgreSQL سالم باشند. اتصال Worker با application_name جداگانه ثبت می‌شود.
4. **ظرفیت Hot ID:** خطای ظرفیت با کد `HOT_CAPACITY_REACHED` به کلاینت فرستاده می‌شود. عملیات `hydrate` نیز پیش از صف‌کردن Recovery سقف را بررسی می‌کند. اسکریپت انتشار نهایی Recovery هم سقف را اتمیک دوباره بررسی می‌کند تا race بین Admission و انتشار ایجاد نشود.
5. **تست‌های Worker:** تست‌های قرارداد منبع و تست Black-box جدید برای ظرفیت، Recovery و چرخه‌های سرد/فعال‌شدن اضافه شده‌اند. تست Black-box گزینه‌های اختیاری Fault Injection برای Redis `CLIENT PAUSE` و restart کردن PostgreSQL در محیط کاملاً ایزوله دارد.

## اجرای تست‌ها

تست‌های سریع قرارداد کد:

```bash
python3 -m unittest central.tests.test_worker_source_contract -v
```

تست یکپارچه Worker روی سرور آزمایشی جداگانه:

```bash
ARS_API_TOKEN='...' \
ARS_TEST_HOST=127.0.0.1 ARS_TEST_PORT=7091 \
python3 central/tests/worker_reliability_integration.py
```

فایل آماده `central/config/server.worker.test.ini` برای این سناریو فراهم شده است؛ قبل از اجرا دیتابیس `ars_worker_test` را بساز و مطمئن شو Redis DB شماره ۱ فقط مخصوص تست است. اگر فایل دیگری استفاده می‌کنی، باید حداقل این تنظیمات را داشته باشد:

```ini
[memory]
maxActiveIds=1

[lifecycle]
inactiveAfterSec=2
scanEveryMs=500
```

Fault injection اختیاری Redis:

```bash
ARS_TEST_REDIS_CLI=/usr/bin/redis-cli \
ARS_REDIS_HOST=127.0.0.1 ARS_REDIS_PORT=16379 \
ARS_API_TOKEN='...' ARS_TEST_PORT=7091 \
python3 central/tests/worker_reliability_integration.py
```

Fault injection توقف/راه‌اندازی PostgreSQL فقط روی cluster آزمایشی و با opt-in صریح:

```bash
ARS_TEST_PG_CTL=/usr/bin/pg_ctl ARS_TEST_PGDATA=/path/to/disposable/pgdata \
ARS_ALLOW_DB_RESTART_TEST=1 ARS_API_TOKEN='...' ARS_TEST_PORT=7091 \
python3 central/tests/worker_reliability_integration.py
```

## وضعیت اعتبارسنجی این بسته

- تست‌های قرارداد منبع: اجرا شده‌اند.
- بررسی syntax فایل‌های Python: اجرا شده است.
- تست Black-box با Redis/PostgreSQL واقعی: در محیط فعلی اجرا نشده است.
- کامپایل C++ و اجرای CTest: به علت نبود Qt Development (Core/Network/Sql) در محیط فعلی اجرا نشده است.
- Fault injection واقعی Redis/PostgreSQL: نیازمند اجرای تست روی سرور آزمایشی اختصاصی است.

تا زمان Build موفق و اجرای تست یکپارچه و Fault Injection، این بسته **Release تأییدشده Production نیست**.
