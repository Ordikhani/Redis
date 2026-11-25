> **مستند قدیمی**: مسیرهای INI/Build در این سند منسوخ‌اند. به `tests/reference_lab/run_lab.sh` جدید و `docs/13_COMPILED_CONFIG_FA.md` مراجعه کن.

> **به‌روزرسانی راه‌اندازی:** برای اجرای یک‌مرحله‌ای آزمایش، از `bash central/lab/run_lab.sh start` استفاده کن. برای عیب‌یابی از `bash central/lab/run_lab.sh check` استفاده کن. اجرای مستقیم `ars_server` بدون آرگومان نیز تنظیمات کنار executable را پیدا می‌کند. راهنمای کامل: `central/STARTUP_AND_CHANGES_FA.md`.

# راهنمای شروع سریع محیط آزمایش ARS (Ubuntu 22.04)

این محیط **آزمایشی و مستقل از استقرار واقعی** است. سرویس TCP روی `127.0.0.1:7093`، Redis آزمایشی روی `127.0.0.1:16379` و PostgreSQL آزمایشی روی `127.0.0.1:15432` اجرا می‌شوند. Docker دو Volume مجزا نگه می‌دارد؛ `stop` داده‌ها را پاک نمی‌کند. هیچ اسکریپتی نباید به دیتابیس واقعی متصل شود.

## ۱. پیش‌نیازها

نیاز به Ubuntu با Docker و پلاگین `docker compose`، Python 3، CMake، کامپایلر C++17، Qt Network/Core/Sql/Widgets و درایور PostgreSQL Qt داریم. مثال نصب برای Ubuntu 22.04 **در صورت دسترسی به مخازن و پس از بررسی خودت**:

```bash
sudo apt update
sudo apt install -y build-essential cmake python3 qtbase5-dev qt5-qmake \
  libqt5sql5-psql postgresql-client redis-tools
# Docker Engine و Docker Compose Plugin باید از قبل روی ماشین نصب و فعال باشند.
docker compose version
```

این دستورها برای ساخت و اجرای برنامه محلی هستند؛ **نصب Redis/PostgreSQL سیستم لازم نیست** چون سرویس‌های آزمایشی در Docker هستند. برای اجرای GUI باید محیط گرافیکی فعال باشد. `libqt5sql5-psql` برای اجرای Qt `QPSQL` ضروری است و خطای `QPSQL driver not loaded` معمولاً از نبود همین بسته است.

## ۲. مسیر پروژه

پس از خارج‌کردن ZIP:

```bash
cd redis_service
chmod +x central/lab/run_lab.sh
```

فایل `central/lab/run_lab.sh` هر بار توکن آزمایشی اختصاصی را از `central/lab/.lab-token` می‌خواند؛ اگر نباشد به‌صورت تصادفی می‌سازد. مقدار توکن در لاگ چاپ نمی‌شود. **فایل .lab-token را منتشر نکن.**

## ۳. راه‌اندازی چهار ترمینال

ترمینال شماره ۱، ایجاد دیتابیس و Redis اختصاصی و ساخت برنامه‌ها:

```bash
./central/lab/run_lab.sh up
./central/lab/run_lab.sh build
./central/lab/run_lab.sh server
```

ترمینال شماره ۲، گیرنده زنده و قابل کنترل:

```bash
./central/lab/run_lab.sh receiver
```

ترمینال شماره ۳، تولید جریان خودکار با ID فعال و ID غیرفعال و برگشت:

```bash
./central/lab/run_lab.sh sender
```

ترمینال شماره ۴، تست‌های خودکار:

```bash
./central/lab/run_lab.sh test
```

**توجه:** تست خودکار داده‌های مخصوص خودش را تولید و بعد پاک می‌کند. همزمانی این تست با Sender ممکن است نتایج زمانی را به‌دلیل بار زیاد غیرقطعی کند؛ برای نتیجه قابل تکرار ابتدا `test` را بدون Sender اجرا کن و سپس Sender و Receiver را بررسی کن.

GUI آزمایش نیز با دستور زیر قابل اجراست (ترمینال جداگانه):

```bash
./central/lab/run_lab.sh gui
```

در GUI دکمه `Connect` را بزن. Host و Port از قبل `127.0.0.1:7093` هستند؛ Token خودکار از `.lab-token` توسط اسکریپت وارد برنامه می‌شود. روی `Subscribe live`، `Publish + save`، `Recent 30 min`، `Search hot records` و `Server status` کلیک کن. با `Start 1/s` چند دقیقه داده تکراری تولید می‌شود. `Delete ID everywhere` تأیید می‌خواهد و فقط ID انتخابی را حذف می‌کند.

## ۴. مشاهده واقعی وضعیت حافظه و آرشیو

```bash
./central/lab/run_lab.sh inspect
```

این دستور بخشی از کلیدهای Hot در Redis و تعداد رکوردهای آرشیوشده به‌تفکیک ID در PostgreSQL را نشان می‌دهد. برای بررسی یک ID خاص:

```bash
docker compose -f central/lab/compose.yaml exec -T postgres \
  psql -U ars_app -d ars_test -c \
  "SELECT sequence,id,to_timestamp(received_at_ms/1000.0) AS received_at FROM ars_records WHERE id='LAB-COLD-001' ORDER BY sequence;"
```

برای بررسی زنده، در Receiver تایپ کن:

```text
status
hot latest LAB
hot records LAB-COLD-001
latest LAB-COLD-001
recent LAB-COLD-001 30m
recent LAB-COLD-001 1h
history LAB-COLD-001 20
filter latest speed gte 200
```

از اتصال Sender چند ثانیه بعد `LAB-COLD-001` ساکت می‌شود. چون `inactiveAfterSec=8` است، پس از مدتی در سرور لاگ `[ARCHIVED] ... destination=PostgreSQL` ظاهر می‌شود. در Sender پس از `--pause-sec 12` مجدد فعال می‌شود و سرور `[RESTORED]` را ثبت می‌کند. `recent` در دوره سکوت بدون پرکردن Redis، مستقیماً از PostgreSQL جواب می‌دهد.

## ۵. پشتیبان‌گیری و بررسی فایل

در حالی که سرویس‌ها اجرا هستند:

```bash
./central/lab/run_lab.sh backup
python3 central/lab/verify_backup.py central/lab/backups/backup_YYYYmmddTHHMMSSZ_PID
```

مسیر واقعیِ ساخته‌شده توسط دستور `backup` چاپ می‌شود؛ آن را جای نمونه بگذار. `verify_backup.py` هش فایل‌ها را مقایسه می‌کند، ولی **تست بازیابی** نیست. PostgreSQL dump و Redis RDB در حالت دریافت هم‌زمان، Snapshot اتمیک مشترک نیستند. برای نقطه بازیابی سازگار باید نویسنده‌ها را به‌طور کنترل‌شده متوقف و پروسه بازیابی در یک محیط جدا آزمایش شود.

## ۶. توقف بدون حذف داده

```bash
# اول با Ctrl+C سرور ARS را متوقف کن
./central/lab/run_lab.sh stop
```

از `docker compose down -v`، `FLUSHALL`، `TRUNCATE` یا ابزار پاک‌سازی آرشیو در این راهنمای شروع سریع استفاده نمی‌کنیم؛ حذف دائمی در فایل `04_TEST_MATRIX_FA.md` توضیح داده شده است.

## ۷. خطاهای متداول

| خطا | علت و راه‌حل |
|---|---|
| `Could not find Qt5Config.cmake` | نصب Qt SDK / تنظیم `CMAKE_PREFIX_PATH` |
| `QPSQL driver not loaded` | نصب `libqt5sql5-psql` سازگار با Qt |
| `ARS_API_TOKEN ... 32 bytes` | اجرای برنامه از طریق `run_lab.sh` به‌جای اجرای مستقیم |
| `Storage initialization failed` | بررسی فعال بودن containerها، رمز و پورت 15432 |
| `Redis hot store disabled` | بررسی Redis 16379، `appendonly yes`, `appendfsync always`, `noeviction` |
| `Permission denied Docker socket` | مجوز دسترسی به Docker باید توسط مدیر سیستم اصلاح شود؛ اسکریپت مجوزها را تغییر نمی‌دهد |
| `Address already in use` | پورت‌های آزمایش با برنامه دیگر تداخل دارند؛ از پورت آزاد استفاده کن و همه فایل‌های تنظیم را هماهنگ به‌روزرسانی کن |
| GUI اجرا نمی‌شود | روی سرور headless از Receiver/تست خودکار استفاده کن |
