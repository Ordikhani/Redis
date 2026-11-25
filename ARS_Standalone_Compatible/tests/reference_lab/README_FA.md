# آزمایشگاه مستقل بدون فایل INI

`tests/reference_lab/compose.yaml` دیتابیس جداگانه `ars_test` در PostgreSQL پورت 15432 و Redis پورت 16379 ایجاد می‌کند. کد سرور در زمان Build با `-DARS_LAB_PROFILE=ON` روی TCP 7093 و UDP 7094 اجرا می‌شود. تنظیمات از `ServerConfig.cpp` کامپایل می‌شوند.

```bash
./tests/reference_lab/run_lab.sh up
./tests/reference_lab/run_lab.sh build
./tests/reference_lab/run_lab.sh check
./tests/reference_lab/run_lab.sh server
```

در ترمینال دیگر `./tests/reference_lab/run_lab.sh tcp-test` را اجرا کن. این تست واقعاً داده در دیتابیس آزمایشگاه ثبت و حذف می‌کند؛ **هرگز آن را به محیط عملیاتی وصل نکن**. برای متوقف‌کردن کانتینرها `./tests/reference_lab/run_lab.sh stop` اجرا کن (داده باقی می‌ماند). راهنمای توسعه‌ای کامل در `docs/13_COMPILED_CONFIG_FA.md` است.
