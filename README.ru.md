# Scarlett Gen 2 Tool

[English](README.md)

Утилита для Focusrite Scarlett 2i2 2nd Gen с USB ID `1235:8202`. Показывает версию прошивки и состояние Low Power Mode, переключает этот режим и записывает сборки 1051 и 1116. Поддерживаются обновление с 1051 до 1116 и возврат с 1116 на 1051.

## Запуск

Нужны Windows 10 или 11 x64 и установленный драйвер Focusrite 4.150.0.432. Утилита сверяет SHA-256 двух файлов драйвера, поэтому другие сборки могут дать ошибку обнаружения. Подключайте одну Scarlett 2i2 2nd Gen на время работы. [Загрузки для Scarlett 2i2 2nd Gen](https://downloads.focusrite.com/focusrite/scarlett-2nd-gen/scarlett-2i2-2nd-gen) · [Пакет драйвера 4.150.0.432](https://fael-downloads-prod.focusrite.com/customer/prod/downloads/focusritedriver_4.150.0_432.exe)

Распакуйте ZIP из [релиза v1.2.0](https://github.com/bgunn9in/scarlett-gen2-tool/releases/tag/v1.2.0).

- `Scarlett-Gen2-Tool-Setup.exe` устанавливает программу и оба образа для текущего пользователя и создаёт ярлык в меню «Пуск».
- Папка `Portable` содержит `scarlett-gen2.exe`, `8202-1051.bin` и `8202-1116.bin`. Запустите EXE из этой папки. Программа ищет BIN рядом с собой при любом рабочем каталоге.

Контрольная сумма ZIP опубликована в `SHA256SUMS.txt` в том же релизе.

```powershell
Get-FileHash .\scarlett-gen2-tool-v1.2.0-windows-x64.zip -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

## Версия, режим питания и запись

При первом запуске интерфейс на английском. Выберите `Русский` в списке `Language` в правом верхнем углу. Программа запомнит выбор.

При запуске окно считывает версию из USB-дескриптора и состояние Low Power Mode из сегмента `App_Env` и выводит их в отдельных блоках. Ниже находятся выбор прошивки и статус операции. После переподключения данные обновляются автоматически. Кнопка «Обновить статус» перечитывает их вручную.

- Для смены Low Power Mode нажмите «Включить» или «Выключить». Режим управляет питанием цепи фантомного питания. Утилита распознаёт два значения `PhantomPower` и показывает ошибку, если прочитать состояние не удалось.
- Для смены прошивки выберите 1051 или 1116, нажмите «Прошить» и подтвердите переход в диалоге. Оставьте USB подключённым до завершения. Утилита сверяет хеш выбранного образа, проверяет текущую прошивку и перед перезапуском считывает весь записанный сегмент для сравнения.

### Резервные копии и ошибки

Перед стиранием утилита сохраняет полную копию сегмента в `%LOCALAPPDATA%\FocusriteScarlettGen2\Backups`. В окне появится путь к папке с копией `App_Upgrade-<версия>.bin` для прошивки или `App_Env.bin` для режима, а также с журналом `update-state.txt`. Если в `App_Env` есть другие данные, утилита прервёт смену режима до стирания.

Если утилита сверила запись и просит переподключить Scarlett, переподключите её один раз и проверьте версию и состояние режима. При ошибке стирания, записи или сверки сохраните резервную копию и журнал. Прежде чем снова запускать запись, проверьте, видит ли Windows карту. Копия содержит один сегмент. В утилите нет команды для его восстановления или восстановления карты, которую Windows не видит.

## Образы прошивок

В `firmware/` лежат оригинальные образы для USB PID `8202`. Утилита передаёт выбранный образ в сегмент `App_Upgrade`.

| Файл | Источник и смещение в распакованном файле | SHA-256 |
| --- | --- | --- |
| [`8202-1051.bin`](firmware/8202-1051.bin) | Windows Updater 1.0.1.165, PE `Scarlett (2nd Gen) Firmware Updater.exe`, `0x183410`, 64 000 байт | `f8084934586ad894d5c740d4ac6d7232b00dd48da47f12f1c8206477261f021f` |
| [`8202-1116.bin`](firmware/8202-1116.bin) | macOS Updater 1.0.1.178, Mach-O `ScarlettUpdater`, ресурс `Scarlett_2i2S_bin`, `0x116420`, 64 256 байт | `49782a53d27664c5d58494e69d347410e7996f2af1687bb3ea8e2820ab720c98` |

Образ 1116 извлечён из апдейтера для macOS. Утилита Windows записывает его через интерфейс PAL штатного драйвера Focusrite. [Оригинальный пакет 1.0.1.165 для Windows](https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/focusrite/downloads/31626/scarlett-2nd-gen-firmware-updater-1.0.1.165.exe) · [Оригинальный пакет 1.0.1.178 для macOS](https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/focusrite/downloads/32269/scarlettupdater-1.0.1.178.dmg)

Чтобы повторить извлечение, запустите `prepare-firmware.ps1` с путями к [innoextract](https://github.com/UserUnknownFactor/innoextract_win/releases/tag/670) и [7-Zip](https://www.7-zip.org/). Скрипт скачает официальные пакеты, сверит их SHA-256 и проверит полученные образы. Установщики при извлечении не запускаются.

```powershell
.\prepare-firmware.ps1 -Innoextract 'C:\Tools\innoextract.exe' -SevenZip 'C:\Program Files\7-Zip\7z.exe'
```

Скрипт сохранит образы в `build\`. Для готового релиза можно указать папку с EXE параметром `-OutputDirectory`.

## Сборка и проверка

Откройте **Developer PowerShell for VS** для x64 с установленным MSVC и Windows SDK. Для сборки используются образы из `firmware/`.

```powershell
.\build.ps1
.\build\scarlett1116.exe --self-test-full .\build\8202-1051.bin .\build\8202-1116.bin
```

Скрипт собирает `build\scarlett-gen2.exe`, копирует к нему оба образа и собирает консольный `build\scarlett1116.exe` для тестов и диагностики.

Для сборки ZIP с установщиком укажите путь к компилятору [Inno Setup](https://jrsoftware.org/inno-setup/downloads). Готовые ZIP и SHA-256 появятся в `build\release-v1.2.0\`.

```powershell
.\package.ps1 -InnoCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' -Version 1.2.0
```

На Scarlett под Windows 11 с GUI проверили обнаружение, чтение версии 1116 (`0x045C`) и выключенный Low Power Mode. Отдельный запуск консольного апдейтера ранее подтвердил переход с 1051 на 1116 на карте. На имитаторе транспорта проверили запись в обе стороны, включение и выключение Low Power Mode, остановку при ошибке записи и отказ при несовпадении данных.
