# Canon LBP-810 / Windows 11 / C++ — print bridge

## Статус

Тестовый v4-драйвер создаёт очередь `Canon LBP-810 CAPT App Queue`. Задание Windows рендерится в PWG Raster и сохраняется во временный файл; фоновая задача SYSTEM (`lbp810_spool.exe --watch-capture`) декодирует его постранично, переводит в 1-bit растр A4/600 dpi и отправляет на Canon по CAPT/SCoA через USBPRINT.

Версия 1.1 (2026-10-24):
- **исправлена полярность растра**: по конвенции PBM/libcapt установленный бит — это чёрная точка. Раньше белое кодировалось единицами, и страница уходила на принтер почти целиком чёрной;
- **многостраничные задания**: PWG и PBM читаются постранично, страницы печатаются одним CAPT-заданием с повтором страницы после замятия/отказа (по эталонной схеме `printpbm` из libcapt);
- **форматы PWG**: Srgb_8/Rgb_8/AdobeRgb_8, Sgray_8/Gray_8, Black_8, Sgray_1/Gray_1, Black_1 при 300 или 600 dpi; носитель меньше A4 размещается в левом верхнем углу, больше — обрезается;
- **исправлен PDC XML**: корневой элемент не задавал пространство имён по умолчанию, поэтому `DocumentFormatOptions` игнорировался и Windows отдавала RGB 300 dpi. Теперь объявлен `Sgray_8` 600 dpi (как в `MSPWGR.xml` Microsoft);
- **ожидание принтера**: нет бумаги, открыта крышка, замятие, прогрев — мост ждёт до 30 минут и пишет причину в журнал; если принтер не подключён — ждёт до 10 минут;
- **надёжный приём заданий**: файл захвата атомарно переименовывается, пока он открыт эксклюзивно; задания, оставшиеся после перезагрузки (`*.processing.pwg`), допечатываются при старте; весь вывод моста с отметками времени идёт в `bridge.log`;
- **установщик**: сам находит `signtool`/`Inf2Cat` в SDK/WDK, пересоздаёт и подписывает каталог, обновляет уже установленный драйвер и копирует мост в `%ProgramFiles%\CanonLBP810`, чтобы пересборка проекта не блокировалась работающей задачей.

Ограничения:
- после изменения PDC нужно переустановить пакет и на физическом принтере проверить, что Windows действительно присылает Sgray_8 600 dpi (видно в `bridge.log`: `color space 18, 8 bpp`) и что страница печатается правильно (не инвертирована);
- поддерживается только бумага A4 (код CAPT `0x09`); Letter и другие форматы обрезаются/дополняются до A4;
- локальная подпись self-signed предназначена только для разработки. Это не WHQL/production package и не официальный драйвер Canon;
- транспорт — фоновая задача, а не порт-монитор: статус принтера и ошибки не отображаются в очереди Windows, только в `bridge.log`.

## Структура

```
CanonLBP810_Win11_Cpp_Prototype/
├── CMakeLists.txt
├── README_RU.md
├── driver-package/          INF, PDC XML, manifest (каталог создаёт установщик)
├── tools/
│   └── Install-CanonLBP810TestQueue.ps1
└── src/
    └── main.cpp
```

## Сборка в Windows 11

1. Установите Visual Studio 2026 с workload **Desktop development with C++**, Windows SDK/WDK 10.0.28000 и CMake.
2. Откройте x64 **Developer PowerShell for Visual Studio**.
3. Перейдите в папку проекта и выполните:

```
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl.exe
cmake --build build-release
```

При первой конфигурации CMake скачает закреплённую версию `libcapt` с GitHub, поэтому нужен доступ в интернет. Исполняемый файл: `build-release\lbp810_spool.exe`.

## Проверка

```
build-release\lbp810_spool.exe --list                       # принтеры Windows
build-release\lbp810_spool.exe --usb-scan                   # USBPRINT, IEEE-1284 ID, байт статуса
build-release\lbp810_spool.exe --capt-info                  # CAPT printer info без печати
build-release\lbp810_spool.exe --print-test                 # тестовая A4-страница
build-release\lbp810_spool.exe --print-pbm "pages.pbm"      # один или несколько P4-образов подряд
build-release\lbp810_spool.exe --print-pwg "job.pwg"        # PWG Raster напрямую на принтер
build-release\lbp810_spool.exe --pwg-to-pbm "job.pwg" "pages.pbm"
build-release\lbp810_spool.exe --watch-capture --console    # мост в консоли, для отладки
```

PBM-образы: ширина до 4960 и кратна 8, высота до 7040 точек при 600 dpi; файл проверяется целиком до отправки. Перед каждой страницей мост дожидается готовности принтера.

## Очередь Windows

В повышенном PowerShell установите или обновите пакет, очередь и мост:
```
.\tools\Install-CanonLBP810TestQueue.ps1
```

Выберите `Canon LBP-810 CAPT App Queue` в диалоге печати. PWG-задания попадают в `%ProgramData%\CanonLBP810\Jobs`, задача `CanonLBP810 CAPT Bridge` печатает их. Журнал — `bridge.log`, неудачные задания сохраняются как `*.failed.pwg` (их можно повторить через `--print-pwg`).

Если после обновления пакета Windows продолжает присылать старый формат (в `bridge.log` видно `color space 19, 24 bpp` вместо `color space 18, 8 bpp`), пересоздайте очередь, чтобы сбросить кэш возможностей и настроек по умолчанию:
```
.\tools\Install-CanonLBP810TestQueue.ps1 -RecreateQueue
```

Удалить очередь, задачу и установленный мост:
```
.\tools\Install-CanonLBP810TestQueue.ps1 -RemoveQueue
```

Установка использует временный self-signed сертификат в LocalMachine Root/TrustedPublisher и не является способом выпуска публично доверенного драйвера. Не подключайте физический USB-порт напрямую к очереди: принтер понимает CAPT, а не PWG Raster.

## Следующие этапы

1. Переустановить пакет и проверить на принтере Word, PDF и изображение, включая многостраничный документ.
2. Проверить отказ по бумаге/крышке посреди задания и повтор страницы.
3. Заменить фоновую задачу на порт-монитор (статус в очереди Windows) и проверить на чистой Windows 11.
4. Заменить локальный test certificate на production-совместимую подпись.

## Ссылки на справочную основу

- libcapt (BSD-2-Clause, commit `8f1fd502e6fee893d409f09c5895248c189ead4`): https://github.com/darkvision77/libcapt
- Linux CAPT для LBP810/LBP1120: https://github.com/caxapyk/capt_lbp810-1120
- Документация Microsoft по печатным драйверам: https://learn.microsoft.com/en-us/windows-hardware/drivers/print/

При распространении бинарной сборки сохраняйте уведомления BSD-2-Clause из лицензии `libcapt`. Старый Linux CAPT проект указан как историческая справка; его GPLv2 код в этот проект не переносился.
