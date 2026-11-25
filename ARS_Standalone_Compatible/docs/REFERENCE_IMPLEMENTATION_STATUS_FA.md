> **مرجع تاریخی پروژه اصلی**: ممکن است از INI یا توکن استفاده کند؛ نسخه اجرایی فعلی مطابق این متن نیست.

# وضعیت پیاده‌سازی ARS Hot Data Manager

## تغییرات این بازبینی

1. Checkpoint و Sweep دوره‌ای به `HotColdWorker` منتقل شدند.
2. Worker در Thread مستقل، Redis socket و PostgreSQL connection اختصاصی خود را ایجاد می‌کند؛ اتصال‌های Qt بین Threadها مشترک نیستند.
3. Checkpoint به Batch محدود است و ترتیب Commit در PostgreSQL سپس ثبت offset در Redis رعایت می‌شود.
4. Sweep فقط با اسکریپت اتمیک Redis و پس از تکمیل آرشیو مجاز به Evict است.
5. وضعیت Worker در پاسخ `status` با `maintenanceWorker` و `maintenanceWorkerError` گزارش می‌شود.
6. تنظیم `lifecycle/workerIntervalMs` و سقف اختیاری `memory/maxActiveIds` به INI افزوده شد.
7. سقف شناسه‌های فعال با Backpressure اعمال می‌شود؛ تاریخچه unsynced هرگز برای آزادسازی حافظه حذف نمی‌شود.


## اصلاحات قابلیت اطمینان Worker (نسخه جاری)

1. خطای Redis در Worker باعث قطع اجباری سوکت با `QTcpSocket::abort()` می‌شود تا پاسخ دیرهنگام در بافر باقی نماند؛ اتصال بعدی از ابتدا ساخته می‌شود.
2. Lua مربوط به append، فعال‌بودن ID و سقف `maxActiveIds` را در همان عملیات اتمیک بررسی می‌کند. اگر Worker بین ZSCORE و append شناسه را Evict کرده باشد، write به‌عنوان ذخیره‌شده ACK نمی‌شود و مسیر Recovery اجرا می‌شود.
3. Worker سلامت PostgreSQL را پایش و اتصال مستقل خود را با backoff افزایشی دوباره ایجاد می‌کند. `maintenanceWorker` فقط در صورت سلامت هم‌زمان Redis و PostgreSQL true است.
4. `save` و `hydrate` خطای ظرفیت را با `HOT_CAPACITY_REACHED` برمی‌گردانند. انتشار Recovery نیز ظرفیت را در Lua دوباره بررسی می‌کند. Qt SDK سیگنال سازگار `requestFailed` را نگه می‌دارد و `requestFailedDetailed` را برای دریافت error code اضافه می‌کند.
5. پنج تست قرارداد منبع و یک تست Black-box برای چرخه Recovery/ظرفیت/Race اضافه شده‌اند. فایل `server.worker.test.ini` محیط جداگانه و محدودشده برای این تست‌ها فراهم می‌کند.

**اعتبارسنجی همین نسخه:** `python3 -m unittest discover -s central/tests -p 'test_*.py' -v` تعداد ۱۳ تست را اجرا و موفق کرد و ۹ تست Startup را به‌علت نبود binary/config رد کرد. تست‌های قرارداد Worker پنج مورد موفق‌اند. تست Black-box و Fault Injection واقعی به Redis/PostgreSQL اضافه شده‌اند ولی در این محیط اجرا نشده‌اند. CMake به‌علت نبود Qt SDK در `find_package(QT)` متوقف شد؛ بنابراین C++ هنوز کامپایل‌شده/Runtime-verified نیست.

## محدودیت‌ها و کارهای باز

- بازیابی تاریخچهٔ شناسهٔ سرد به صف محدود Worker منتقل شده است. `save`/`publish` برای شناسهٔ سرد تا پایان بازیابی نگه داشته و بعد از ذخیره ACK می‌شود (بخش «ادغام» در پایین). `hydrate` نیز برای شناسهٔ سرد job غیرهمگام برمی‌گرداند. Worker تاریخچه را از PostgreSQL به‌صورت stream و batch در staging Redis می‌سازد و در پایان آن را اتمیک فعال می‌کند. این تغییر هنوز به Build و آزمون Runtime نیاز دارد؛ بنابراین تأیید پایداری نهایی نشده است.
- `maxActiveIds` سقف دقیق RAM برحسب بایت نیست. محدودکردن تعداد رکوردهای هر شناسه بدون طراحی خواندن ترکیبی Hot/Cold می‌تواند تاریخچهٔ فعال را ناقص کند؛ بنابراین عمداً انجام نشده است.
- جست‌وجوی Hot همچنان bounded scan است، نه Redis Search ایندکس‌شده.
- این محیط فاقد Qt5/Qt6 development package و QPSQL runtime plugin است؛ بنابراین CMake در مرحله `find_package(QT)` متوقف شد و کامپایل/تست یکپارچه این تغییرات در این محیط ممکن نبود. قبل از استقرار باید در محیط دارای Qt و PostgreSQL/Redis build و تمام تست‌های integration/end-to-end اجرا شوند.

هیچ ادعایی دربارهٔ تضمین hard real-time یا throughput خاص در این تغییرات مطرح نمی‌شود.


## ادغام ۲۰۲۶-۱۱-۱۲ (پایه: WorkerReliability_Fixes)

1. **ACK تأخیری برای write روی شناسهٔ سرد.** `Server` درخواست را نگه می‌دارد (`processDeferred`، هر ۲۵ ms)، پس از `completed` شدن job همهٔ writeهای نگه‌داشته را به ترتیب رسیدن و با `receivedAtMs` اصلی ذخیره می‌کند و فقط بعد از ذخیره `ok=true` می‌دهد. پس `ok=true` دوباره یعنی «ذخیره شد» و کلاینت‌های قدیمی بی‌صدا داده از دست نمی‌دهند. writeهای بعدی همان ID پشت writeهای نگه‌داشته صف می‌شوند تا ترتیب حفظ شود. پس از `[recovery] ackTimeoutMs` (پیش‌فرض ۱۰ ثانیه) پاسخ `ok=false, code=RECOVERY_PENDING` است؛ job ناموفق `RECOVERY_FAILED` یا `HOT_CAPACITY_REACHED`. سقف: ۲۵۶ write برای هر ID و ۴۰۹۶ در کل. اولین پاسخ بعد از بازیابی `restoredFromPostgres` و `restoredHistoryCount` را مثل نسخهٔ همگام قدیمی دارد.
2. **SDK و مثال‌ها.** کپی `vendor` در `examples/independent_qt_app` با `ArsClient` مرکزی یکی شد (`requestFailedDetailed`، `recoveryStatus`). `TelemetryBridge` هر write را تا ACK نگه می‌دارد و برای `RECOVERY_PENDING` حداکثر سه بار دوباره می‌فرستد؛ سیگنال `failureDetailed` اضافه شد. `sender` کد خطا و `heldForRecoveryMs` را چاپ می‌کند.
3. **race سمت خواندن و محافظ `synced`.** `history`/`recent` اگر ID وسط خواندن evict شود از PostgreSQL جواب می‌دهند (`[READ_RACE]`). شمارندهٔ `synced` در Worker و مسیر delete فقط با `HotColdScripts::advanceSynced` جلو می‌رود (compare-and-set + تطبیق sequence اول/آخر دسته)؛ لایهٔ دوم پشت `abort()`.
4. **اتصال مجدد PostgreSQL در thread اصلی.** `HotColdStore::maintainPostgres()` هر ثانیه از `Server` صدا زده می‌شود؛ قطع اتصال را تشخیص می‌دهد و با backoff تا ۳۰ ثانیه دوباره وصل می‌شود و قفل تک‌نویسنده را دوباره می‌گیرد. `status` فیلدهای `postgresReconnecting` و `postgresReconnects` را دارد.
5. **اسکریپت‌های Lua** همه در `server/HotColdScripts.h` هستند (append، evict، advanceSynced، stageBatch، publishRecovery).
6. **تست‌ها.** `tests/test_hotcold_lua.py` (۱۳ تست) متن واقعی اسکریپت‌ها را روی redis-server اجرا می‌کند. `tests/test_worker.py` (۶ تست یکپارچه) سرور را خودش بالا می‌آورد: سلامت، کد ظرفیت، ACK تأخیری، کامل ماندن تاریخچه هنگام evict هم‌زمان، و اتصال مجدد PostgreSQL در Worker و thread اصلی. تست‌های قرارداد منبع به‌روز شدند و `hot_cold_end_to_end.py` و `worker_reliability_integration.py` با رفتار جدید هماهنگ شدند (تست restart PostgreSQL حالا اتصال thread اصلی و یک write با ID جدید را هم بررسی می‌کند).

**اعتبارسنجی این ادغام:** کل مجموعهٔ Python (`test_*.py`) اجرا شد: همهٔ تست‌های قابل‌اجرا موفق، ۱۳ تست Lua روی Redis 7.0.15 موفق؛ تست‌های Startup و `test_worker.py` به‌علت نبود binary رد شدند. کد C++ به‌علت نبود Qt در محیط ادغام کامپایل نشده است.
