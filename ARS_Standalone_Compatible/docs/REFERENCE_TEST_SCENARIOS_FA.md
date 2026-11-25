> **مرجع تاریخی پروژه اصلی**: ممکن است از INI یا توکن استفاده کند؛ نسخه اجرایی فعلی مطابق این متن نیست.

# آزمایش کامل Redis Hot/Cold با دو برنامه ساده Qt/C++

این مثال **جزئی از پروژه اصلی ARS** است و از `ArsClient` موجود استفاده می‌کند. دو برنامه مستقل تولیدکننده و دریافت‌کننده به `ars_server` متصل می‌شوند. تنها `ars_server` به Redis و PostgreSQL دسترسی دارد.

```text
ars_test_sender (Qt console) ─────┐
                                  ├── TCP + token ──> ars_server ──> Redis (ID فعال)
ars_test_receiver (Qt console) ────┘                       │
                ↑ live events (from server)                └──────> PostgreSQL (آرشیو / checkpoint)
```

**نکته:** این ساختار عمداً اتصال مستقیم کلاینت به Redis ندارد؛ در نتیجه توکن دیتابیس و Redis در کلاینت‌ها نیست. `ars_server` سرور مرکزی است و درخواست‌ها را به مخزن مناسب هدایت می‌کند.

## ۱. سناریوهای تولیدکننده

با پیشوند پیش‌فرض `DEMO` این شناسه‌ها تولید می‌شوند:

| ID | رفتار تولیدکننده | آنچه باید مشاهده شود |
|---|---|---|
| `DEMO-HOT-001` | در هر تیک داده جدید | در Redis باقی می‌ماند، با checkpoint دوره‌ای در PostgreSQL هم ثبت می‌شود |
| `DEMO-REPEAT-001` | هر سه تیک یک رکورد با **ID تکراری** | رکوردها جایگزین نمی‌شوند؛ تاریخچه و sequence جدا دارند |
| `DEMO-COLD-001` | ۵ تیک اول فعال، سپس توقف پیش‌فرض **۱۴۵ ثانیه**، سپس فعال می‌شود | `ARCHIVED` و سپس `RESTORED` در لاگ سرور؛ هنگام بازگشت تمام تاریخچه قدیمی + رکورد جدید در Redis |
| `DEMO-LATE-001` | با timestamp مربوط به ۵ دقیقه پیش می‌رسد | `receivedAtMs` زمان دریافت واقعی را نشان می‌دهد، نه timestamp داخل داده |
| `DEMO-DELETE-001` | فقط در ۳ تیک اول فرستاده می‌شود | برای آزمایش حذف دائمی بدون بازتولید خودکار آماده است |
| `DEMO-SILENT-001` | هر هفت تیک با `saveRecord` ذخیره می‌شود | در تاریخچه هست؛ اما event زنده منتشر نمی‌کند |

تمام ارسال‌ها، پاسخ‌های سرور و زمان‌های UTC در کنسول ثبت می‌شوند. گیرنده در خط `[LIVE]` علاوه بر زمان رویداد، **زمان دریافت سرور** (`receivedAtMs`) و `sequence` را نیز نشان می‌دهد. `features`، موقعیت، سرعت، جهت، payload و source نیز همراه هر داده هستند.

## ۲. پیش‌نیاز و ساخت

از پوشه اصلی `redis_service`:

```bash
sudo apt install build-essential cmake qtbase5-dev libqt5sql5-psql \
  postgresql postgresql-client redis-server python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

سه فایل اجرایی مهم پس از ساخت:

- `build/central/ars_server` — سرویس مرکزی؛ Redis/PostgreSQL را مدیریت می‌کند.
- `build/central/ars_test_sender` — تولیدکننده مستقل و قابل تنظیم.
- `build/central/ars_test_receiver` — گیرنده زنده و خط فرمان تعاملی.

برای نمایش گزینه‌های برنامه‌های تست، `--help` را به نام هر برنامه اضافه کنید. برنامه‌های تست نیاز به GUI یا Qt Widgets ندارند.

## ۳. پایگاه داده جداگانه برای آزمایش

برای جلوگیری از تغییر داده‌های واقعی، فایل `central/config/server.test.ini` از **پورت 7091**، دیتابیس PostgreSQL با نام **`ars_test`** و prefix مستقل Redis با نام **`arscentral_demo_test`** استفاده می‌کند. پیش از اولین اجرا، PostgreSQL و Redis باید روشن باشند.

ساخت دیتابیس آزمایشی با اسکریپت موجود پروژه (روی ماشینی که دسترسی مدیریتی PostgreSQL دارد):

```bash
read -rsp 'Password for ars_app: ' ARS_DB_PASSWORD; echo
export ARS_DB_PASSWORD
sudo -u postgres env ARS_DB_NAME=ars_test ARS_DB_USER=ars_app \
  ARS_DB_PASSWORD="$ARS_DB_PASSWORD" python3 central/scripts/init_database.py
```

اگر نقش `ars_app` از قبل وجود دارد، اسکریپت **رمز آن را تغییر نمی‌دهد**. باید رمز واقعی همان نقش را در `ARS_DB_PASSWORD` وارد کنید. اگر مالک دیتابیس موجود متفاوت باشد، اسکریپت برای جلوگیری از تغییر ناخواسته متوقف می‌شود.

## ۴. اجرای سرویس و برنامه‌ها در سه ترمینال

توکن یکسان و تصادفی حداقل ۳۲ بایتی را در هر سه ترمینال به متغیر `ARS_API_TOKEN` بدهید. رمز PostgreSQL فقط در ترمینال سرور لازم است؛ اگر Redis رمز دارد، `ARS_REDIS_PASSWORD` را نیز فقط در سرور تنظیم کنید.

### ترمینال شماره ۱ — سرویس مرکزی ARS

```bash
# در پوشه redis_service
export ARS_API_TOKEN='YOUR_SAME_RANDOM_TOKEN_AT_LEAST_32_BYTES'
read -rsp 'PostgreSQL password: ' ARS_DB_PASSWORD; echo
export ARS_DB_PASSWORD
./build/central/ars_server "$PWD/central/config/server.test.ini" 2>&1 | tee ars_test_server.log
```

**حساس:** توکن نمونه بالا را با مقدار تصادفی واقعی جایگزین کنید؛ هر کلاینت باید همین توکن را داشته باشد.

### ترمینال شماره ۲ — گیرنده داده زنده و جست‌وجو

```bash
export ARS_API_TOKEN='YOUR_SAME_RANDOM_TOKEN_AT_LEAST_32_BYTES'
./build/central/ars_test_receiver --host 127.0.0.1 --port 7091
```

خروجی اولیه `Subscribed to ars:test:live` است. برنامه باز می‌ماند و داده‌های جدید را به صورت زنده چاپ می‌کند؛ همزمان می‌توانید در همان ترمینال دستور تایپ کنید.

### ترمینال شماره ۳ — تولیدکننده داده

```bash
export ARS_API_TOKEN='YOUR_SAME_RANDOM_TOKEN_AT_LEAST_32_BYTES'
./build/central/ars_test_sender --host 127.0.0.1 --port 7091 \
  --interval-ms 1000 --pause-sec 145 --after-resume-sec 20
```

این اجرای پیش‌فرض، ID سرد را بیش از دو دقیقه بی‌خبر می‌گذارد و سپس دوباره ارسال می‌کند. برای **تست سریع‌تر** با تنظیمات فایل آزمایشی (`inactiveAfterSec=8`) می‌توانید از `--pause-sec 20` استفاده کنید.

**توجه:** برای تست واقعی چنددقیقه‌ای با حد ۶۰ ثانیه، مقدار `inactiveAfterSec=60` را در `server.test.ini` قرار دهید و `--pause-sec 145` اجرا کنید. اگر طول توقف از زمان عدم‌فعالیت کوچک‌تر باشد، آرشیو شدن رخ نمی‌دهد.

## ۵. دستورات گیرنده

در همان ترمینال شماره ۲ تایپ کنید:

```text
status
history DEMO-HOT-001 20
history DEMO-REPEAT-001 5
next DEMO-REPEAT-001
last5 DEMO-LATE-001
latest DEMO-COLD-001
history DEMO-COLD-001 100
history DEMO-SILENT-001
hydrate DEMO-DELETE-001
evict DEMO-DELETE-001
history DEMO-DELETE-001
delete DEMO-DELETE-001
history DEMO-DELETE-001
help
quit
```

`history` به ترتیب `sequence` پاسخ می‌دهد؛ `next` صفحه بعد را می‌خواند. `last5` بر اساس **زمان دریافت در سرور** و نه timestamp تولیدکننده است. `evict` اول تغییرات باقی‌مانده را در PostgreSQL ثبت می‌کند و فقط کپی Redis را حذف می‌کند. `delete` رکوردهای این ID را از **هر دو مخزن به‌طور دائمی** حذف می‌کند. کلاینت نیازمند دسترسی معتبر است؛ فعلاً مجوز جداگانه برای حذف در API تعریف نشده است.

**نکته:** اگر `DEMO-HOT-001` را حین اجرای تولیدکننده حذف کنید، تیک بعدی همان ID را دوباره می‌سازد. برای آزمایش حذف بهتر است `DEMO-DELETE-001` را انتخاب کنید یا تولیدکننده را متوقف کنید.

## ۶. لاگ‌های مهم در ترمینال سرور

متن‌های زیر **نمونه خروجی مورد انتظار** هستند (اعداد دقیق به اجرای شما بستگی دارند):

```text
[CHECKPOINT] atUtc= 2026-11-12T10:01:30.000Z id= DEMO-HOT-001 destination=PostgreSQL records= 3 syncedHistoryCount= 12
[CHECKPOINT_DONE] success= true
[ARCHIVED] atUtc= 2026-11-12T10:02:00.000Z id= DEMO-COLD-001 source=Redis destination=PostgreSQL historyRecords= 5 reason=inactivity cacheEvicted=true
[RESTORED] atUtc= 2026-11-12T10:04:00.000Z id= DEMO-COLD-001 source=PostgreSQL destination=Redis historyRecords= 5
[DELETE_CACHE] id= DEMO-DELETE-001 archivePreserved=true
[DELETE_ALL] id= DEMO-DELETE-001 source=Redis+PostgreSQL
```

**نتیجه درست:** بعد از پیام `ARCHIVED`، فرمان `history DEMO-COLD-001` باید `source=postgres` بدهد. با دریافت رکورد جدید `DEMO-COLD-001`، پیام `RESTORED` ظاهر می‌شود و `history` باید `source=redis` و رکوردهای قدیمی **به‌علاوه** رکورد جدید را نشان دهد. اطلاعات `sequence` و `receivedAtMs` رکوردهای قدیمی باید تغییر نکرده باشند.

برای تأیید آرشیو از PostgreSQL (در دیتابیس تست):

```bash
PGPASSWORD="$ARS_DB_PASSWORD" psql -h 127.0.0.1 -U ars_app -d ars_test \
  -c "SELECT id, COUNT(*) AS count, MIN(received_at_ms), MAX(received_at_ms) FROM ars_records WHERE id LIKE 'DEMO-%' GROUP BY id ORDER BY id;"
```

## ۷. آزمایش خودکار End-to-End (بدون وابستگی Python اضافه)

**پس از روشن شدن `ars_server` با فایل آزمایشی**:

```bash
export ARS_API_TOKEN='YOUR_SAME_RANDOM_TOKEN_AT_LEAST_32_BYTES'
ARS_TEST_PORT=7091 python3 central/tests/hot_cold_end_to_end.py
```

تست IDهای تصادفی مخصوص خودش را تولید می‌کند و پس از اتمام فقط همان IDها را پاک می‌کند. برای اطمینان از اجرای درست، این موارد را بررسی می‌کند:

1. اتصال دو کلاینت، Redis و PostgreSQL؛
2. انتشار و دریافت زنده، و حفظ چند رکورد با ID تکراری؛
3. تفاوت زمان رویداد و زمان دریافت سرور؛
4. حفظ ID فعال در Redis و checkpoint دوره‌ای؛
5. آرشیو و خروج ID ساکت از Redis؛
6. حفظ تاریخچه کامل و زمان‌های قدیمی و بازگشت خودکار آن به Redis؛
   همچنین ارسال ۲۰۵ رکورد برای یک ID جهت تست چند batch آرشیو و pagination؛
7. حذف فقط کش، بارگذاری مجدد و حذف دائمی.

نتیجه مطلوب `ALL END-TO-END SCENARIOS PASSED` است. این تست یکپارچه به **سرویس‌های واقعی و در حال اجرا** نیاز دارد؛ صرف اجرای `unittest` Python، موفقیت چرخه Redis/PostgreSQL را ثابت نمی‌کند.

## ۸. تست‌های خطا و خرابی (فقط در محیط اختصاصی تست)

- **PostgreSQL در زمان آرشیو قطع شود:** `ARCHIVED` نباید ثبت شود، و تاریخچه فعال نباید از Redis حذف شود. بعد از بازگرداندن PostgreSQL و بازیابی اتصال، آرشیو را دوباره بررسی کنید. به این نکته توجه کنید که نسخه فعلی اتصال مجدد خودکار PostgreSQL را برای همه حالات تضمین نمی‌کند.
- **Redis هنگام ثبت داده قطع شود:** ممکن است ARS به ذخیره مستقیم در PostgreSQL برود. در این وضعیت به `storage=postgres` توجه کنید. **بازیابی و ادغام کامل خودکار پس از قطع Redis هنوز تضمین نشده است.**
- **ری‌استارت سرویس:** بررسی کنید که داده‌های checkpoint‌شده بازیابی شوند و IDهای فعال/غیرفعال رفتار صحیح داشته باشند. تنظیمات AOF/noeviction برای جلوگیری از از دست رفتن رکوردهای checkpoint‌نشده اهمیت دارد.
- **ارسال رکورد نامعتبر یا توکن اشتباه:** سرور باید درخواست را رد کند. این دو آزمایش به‌صورت خودکار در تست قدیمی `central/tests/integration.py` نیز پوشش داده شده‌اند.

برای آزمایش خرابی سرویس‌ها، **Redis/PostgreSQL تولیدی را قطع نکنید**. همچنین هیچ بخشی از تست، پشتیبان‌گیری مستقل با `pg_dump` را جایگزین نمی‌کند.

### ساخت با qmake (درصورت استفاده از Qt Creator)

برای دو برنامه تست می‌توانید فایل‌های `central/test_apps/sender.pro` و
`central/test_apps/receiver.pro` را جداگانه در Qt Creator باز کنید.
این فایل‌ها سورس مشترک `ArsClient` و `ArsDataRecord` را هم کامپایل می‌کنند؛
اما خود **سرور مرکزی** همچنان با CMake ساخته می‌شود.

## ۹. فایل‌ها و محل هر قطعه

```text
central/
  client/ArsClient.h, .cpp       <- API مشترک Qt/C++
  server/Server.h, .cpp          <- TCP / احراز هویت / live
  server/HotColdStore.h, .cpp    <- Redis active, lifecycle, checkpoint, restore
  server/PostgresStore.h, .cpp  <- آرشیو PostgreSQL
  config/server.test.ini        <- تنظیمات مخصوص آزمایش
  test_apps/sender.cpp          <- تولیدکننده ساده و مستقل
  test_apps/receiver.cpp        <- گیرنده تعاملی مستقل
  tests/hot_cold_end_to_end.py  <- تست End-to-End واقعی
  TEST_SCENARIOS_FA.md          <- همین راهنما
```

**محدودیت اعتبارسنجی بسته:** آزمون‌های provisioning موجود با Python اجرا شده‌اند؛ کامپایل C++ و تست End-to-End در محیط آماده‌سازی بسته به علت نبود Qt SDK، PostgreSQL و Redis در دسترس نبوده است. همه چیز باید روی ماشین Ubuntu دارای سرویس‌ها ساخته و تست شود.
