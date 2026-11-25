> **بایگانی تاریخی**: این راهنما متعلق به نسخه INI قبلی است؛ برای نسخه فعلی از `docs/13_COMPILED_CONFIG_FA.md` استفاده کنید.

# تغییرات نسخه بدون توکن / INI — Qt 6.7

## فایل‌های INI

در این بسته **۵ فایل INI** وجود دارد:

1. `server/config/server.ini` — تنظیمات اصلی اجرایی سرور.
2. `tests/reference_config/server.ini` — تنظیم مرجع آزمایش.
3. `tests/reference_config/server.test.ini` — تست یکپارچه.
4. `tests/reference_config/server.worker.test.ini` — تست Worker.
5. `tests/reference_lab/server.lab.ini` — Docker/Lab ایزوله با رمز آزمایشی.

فقط **یک فایل INI برای اجرای عادی** Server لازم است؛ Producer و Consumer فایل INI لازم ندارند و host/port/transport را با آرگومان می‌گیرند.

## تغییر دقیق احراز هویت

- پیام `auth` و کلید `token` از درخواست‌های API و سورس کلاینت/سرور حذف شدند.
- در TCP کلاینت هنگام connect پیام `hello` می‌فرستد؛ سرور ثبت اتصال کرده و پاسخ `protocol`/`transport` می‌دهد.
- در UDP هر کلاینت در ابتدا `hello` می‌فرستد تا endpoint آن با lease ثبت شود؛ سپس heartbeat و ACKهای UDP کار می‌کنند. هیچ توکنی برای این کار نیاز نیست.
- تابع Qt تغییر کرده است:

```cpp
ArsClient client;
client.connectToServer("127.0.0.1", 7090, ArsClient::Transport::Tcp);
// یا
client.connectToServer("127.0.0.1", 7092, ArsClient::Transport::Udp);
```

- توکن/فایل توکن در Server، Producer، Consumer و کد اجرایی Build حذف شده است؛ کلاس `Credentials` لازم نیست.

## PostgreSQL

رمز اتصال مستقیم از `server.ini` خوانده می‌شود، نه از environment variable یا فایل پسورد مستقل:

```ini
[postgres]
host=127.0.0.1
port=5432
database=ars
user=ars_app
password=CHANGE_ME_TO_MATCH_POSTGRES_ROLE
```

`password` باید با رمز واقعی Role ساخته‌شده در PostgreSQL **یکسان باشد**. نمونه همراه بسته به‌تنهایی به معنی ساخته‌شدن دیتابیس یا کارکرد اتصال نیست. برای اعمال تغییرات INI، سرور را Restart کنید. Worker و تست Startup هم همین گزینه را می‌خوانند. ابزارهای backup, purge و init_database نیز `--config` دارند (پیش‌فرض: `server/config/server.ini`) و همین گزینه را می‌خوانند. فایل `server.ini` را با `chmod 600` فقط در اختیار حساب سرور بگذارید؛ نصب CMake هم آن را محدود می‌کند. اگر ZIP را به اشتراک می‌گذارید رمز واقعی را قبلاً از آن پاک کنید.

## نمونه Build / Run

```bash
cmake -S server -B server/build
cmake --build server/build --parallel 2
# رمز واقعی را در server/build/server.ini بگذارید
chmod 600 server/build/server.ini
./server/build/ars_server --check --config server/build/server.ini
./server/build/ars_server --config server/build/server.ini
```

ترمینال‌های دیگر:

```bash
cmake -S producer -B producer/build && cmake --build producer/build --parallel 2
cmake -S consumer -B consumer/build && cmake --build consumer/build --parallel 2
./consumer/build/ars_consumer --transport tcp --port 7090
./producer/build/ars_producer --transport tcp --port 7090
# یا هر کدام مستقل با --transport udp --port 7092
```

`--check` باید با سرویس‌های Redis و PostgreSQL واقعی اجرا شود. ۵ فایل INI تنظیمات **بیلد** یا Qt Creator نیستند؛ تنها تنظیمات runtime این سرویس‌اند.

## امنیت و محدودیت‌ها

در این حالت هیچ احراز هویتی در API وجود ندارد. هر کسی که به آدرس و پورت سرور دسترسی شبکه‌ای داشته باشد می‌تواند عملیات `save`، `publish`، `delete` و پرس‌وجو انجام دهد. برای محیط آزمایشی روی یک سیستم، `127.0.0.1` امن‌تر است؛ برای LAN فقط روی شبکه معتمد با Firewall و ACL استفاده کنید. `hello` احراز هویت محسوب نمی‌شود. UDP تحویل ذاتی تضمین‌شده ندارد و منطق retry/ack در اپلیکیشن همان‌طور باقی مانده است.

## وضعیت آزمون

بسته از نظر تغییر مسیرهای احراز هویت و رمز SQL و تست‌های Python بدون سرویس قابل بررسی است. به دلیل نبود Qt SDK و Redis/PostgreSQL در محیط ساخت، **Build واقعی Qt 6.7 و تست end-to-end هنوز تأیید نشده‌اند**؛ نتیجه اجرای عملی روی سیستم آزمایشی باید در گزارش تست ثبت شود.
