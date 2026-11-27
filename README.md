# Vocowder 1.30

Канальный вокодер для Zoom MS-70CDR (прошивка 2.10). Левый вход IN L — модулятор: голос, гитара, барабаны — то, что «говорит». Правый вход IN R — несущая: синт, аккорды — то, что звучит. Если IN R не подключён, педаль подаёт туда копию IN L; эффект это определяет и использует только шум.

Версия 1.30: ZDL-файл лежит в корне репозитория. Исходники — в `effects/`. Номер эффекта 503, категория Dynamics.

![Vocowder на экране MS-70CDR](docs/screen.png)

## Установка

1. Скопируй `Vocowder.ZDL` и `Vocowder.png` рядом в Effect Manager. PNG — превью эффекта.
2. Перед установкой удали старый Vocowder: Effect Manager не заменяет эффект с тем же номером.
3. Ставь Vocowder первым в цепочке — каждый следующий эффект видит только то, что предыдущий эффект передал, и настоящий IN R дальше по цепочке уже недоступен.
4. Если педаль зависла: выключи питание, зажми левую ручку, включи питание, выбери All Initialize, нажми футсвич.

## Режимы работы

Всё управление — девять ручек на трёх страницах. Значение первой ручки страницы показывается в окошке внизу слева.

| Стр. | Ручка | Что делает |
|---|---|---|
| 1 | Bands | Число полос: 4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80. Диапазон 120 Гц … 7 кГц. |
| 1 | Level | Громкость выхода: 0 — тишина, 50 — 0 дБ, 100 — +12 дБ. После ручки стоит мягкий ограничитель: громко — значит плотнее, а не цифровой клип. |
| 1 | Mix | Сухой / обработанный: 0 — только чистый модулятор, 100 — только вокодер. В режиме Out = Sep ручка не действует. |
| 2 | LoCut | Срез низа только у несущей, 20 … 800 Гц. |
| 2 | HiCut | Срез верха только у несущей: 0 — 16 кГц (открыт), 100 — 1 кГц. |
| 2 | Noise | Шум в несущую. В режимах Freez, FrzL и FRev задаёт длину петли или период смены снимка. |
| 3 | NType | Тип несущей — шум или один из режимов заморозки (см. ниже). |
| 3 | Out | Выход: Mix, Sep, автопанорама или синхронизация с sForm (см. ниже). |
| 3 | Gain | Усиление входов вокодера для тихого микрофона или звукоснимателей: центр — 0 дБ, вправо до +12 дБ модулятор, влево до +12 дБ несущая. Шаг 0,24 дБ. Чистый сигнал не меняется. |

### Типы несущей (NType)

| Режим | Что делает |
|---|---|
| White, Pink, Brown, Green, Blue, Crush | Встроенные шумы: белый, розовый, коричневый, полоса около 500 Гц, яркий «синий», грубый 3-битный с импульсами ~3 кГц. Шум добавляется к IN R; если входа нет, несущей становится только шум. |
| Freez | Заморозка IN R. При выборе режима эффект записывает правый вход; как только записана петля длиной Noise (0,1 … 2,9 с), она зацикливается как несущая. Петля плавно перетекает сама в себя — стыка не слышно. Длину можно крутить на ходу. Чтобы заморозить заново, переключи NType на другой шум и обратно. |
| FrzL | Заморозка самого левого входа. Гитара записывается снимками по 0,74 с; восемь перекрывающихся гранул по 0,37 с и диффузор превращают снимок в мягкую текстуру, которая медленно перетекает в следующий снимок. Noise задаёт период смены снимка 1 … 8 с. Гитара модулирует свою почти замороженную копию. IN R не используется. |
| FRev | Заморозка с ревером. Сразу после ноты слышен живой ревер, затем ~6 мс громкий микро-фриз, после чего звук плавно переходит в FrzL. Тихий яркий ревер (~12 мс) стоит перед Level; LoCut и HiCut режут и его. |

### Выход (Out)

| Режим | Что делает |
|---|---|
| Mix | Оба выхода несут один и тот же микс сухого и обработанного сигнала. |
| Sep | Левый выход — только вокодер, правый — чистый модулятор. |
| T.LFO | Панорама один раз на долю от BPM эффекта sForm (120 BPM, если sForm нет). |
| LFO A, LFO B | Панорама от LFO A или LFO B эффекта sForm через общий почтовый ящик. |
| P0.1 … P9.7 | Автопанорама: обработанный и чистый сигналы ходят по стерео навстречу друг другу со скоростью 0,1 … 9,7 Гц. |

### Что важно знать

- Громкость выхода идёт за несущей, а не за модулятором. Модулятор задаёт форму звука, несущая — громкость. Когда модулятор молчит, молчит и вокодер.
- В Out = Mix стоит гейт на −54 дБ по модулятору: между фразами тишина. В Sep и в режимах панорамы гейта нет.
- Если в IN R нет джека и педаль подаёт туда копию IN L, эффект определяет это по совпадению каналов и убирает копию из живой несущей — остаётся шум. Заморозка по-прежнему записывает IN R.
- Детектор огибающих полос фиксирован на значении, подобранном на слух; его ручка заменена на Level.
- Максимум 80 полос. Дальше растёт нагрузка на DSP и появляется DSP Full.

## Сборка из исходников

Кроме компилятора TI C6000 CGT 8.5.0.LTS всё лежит в репозитории: `effects/vocowder/`, общие заголовки в `effects/include/`, линкер ZDL и помощники экрана в `toolchain/zdl/`. Линкер взят из ZoomMultistompZDL (MIT, с нашими исправлениями; см. NOTICE.md).

1. Установи TI CGT 8.5.0.LTS (входит в Code Composer Studio). Если он не в стандартном месте, укажи `TI_CGT_C6000` в `paths.py`.
2. Запусти `python effects/vocowder/build.py` — он пишет `Vocowder.ZDL` и `Vocowder.png` в корень. Папку можно изменить переменной `ZDL_OUT`.

## История

См. CHANGELOG.md — там записаны все версии, включая тестовые и забракованные.

## Лицензия

MIT, см. LICENSE. Тулчейн ZDL тоже MIT (ZoomMultistompZDL, LICENSE.upstream).

---

# Vocowder 1.30 (English)

Channel vocoder for Zoom MS-70CDR (firmware 2.10). IN L is the modulator: voice, guitar or drums — what gets "spoken". IN R is the carrier: a synth or chords — what gets shaped. With no jack in IN R the pedal copies L there; the effect detects this and uses noise only.

Version 1.30: the ZDL file is at the repository root, sources are in `effects/`. Effect ID 503, category Dynamics.

## Install

1. Copy `Vocowder.ZDL` and `Vocowder.png` together into Effect Manager; the PNG is the thumbnail.
2. Delete an older Vocowder first — Effect Manager does not replace an effect with the same ID.
3. Put Vocowder first in the patch: later effects only see what earlier ones passed, so the real IN R is no longer available down the chain.
4. If the pedal hangs: power off, hold the left knob, power on, All Initialize, footswitch.

## Modes

Nine knobs on three pages. The first knob of the current page is shown in the value box at bottom left.

| Page | Knob | What it does |
|---|---|---|
| 1 | Bands | Band count: 4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80. Range 120 Hz … 7 kHz. |
| 1 | Level | Output level: 0 = silence, 50 = 0 dB, 100 = +12 dB. A soft clip follows, so loud means denser, not digital clipping. |
| 1 | Mix | Dry / wet: 0 = dry modulator only, 100 = vocoder only. Ignored when Out = Sep. |
| 2 | LoCut | High-pass on the carrier only, 20 … 800 Hz. |
| 2 | HiCut | Low-pass on the carrier only: 0 = 16 kHz (open), 100 = 1 kHz. |
| 2 | Noise | Noise added to the carrier. In Freez, FrzL and FRev it sets loop length or snapshot period. |
| 3 | NType | Carrier type: noise or one of the freeze modes (below). |
| 3 | Out | Output mode: Mix, Sep, auto-pan or sForm sync (below). |
| 3 | Gain | Input gain for a quiet mic or pickups: centre 0 dB, right boosts the modulator up to +12 dB, left boosts IN R up to +12 dB, 0.24 dB steps. Dry signal unaffected. |

### Carrier types (NType)

| Mode | What it does |
|---|---|
| White, Pink, Brown, Green, Blue, Crush | Built-in noises: white, pink, brown, a band around 500 Hz, bright "blue", coarse 3-bit noise with ~3 kHz steps. Noise is added to IN R; with no input it becomes the carrier. |
| Freez | Freezes IN R. Selecting the mode starts recording; when a loop of length Noise (0.1 … 2.9 s) is filled it loops as the carrier. The loop crossfades into itself, so there is no audible splice. Length can be changed while frozen. To freeze again, switch NType away and back. |
| FrzL | Freezes IN L itself. The guitar is recorded in 0.74 s snapshots; eight overlapping 0.37 s grains and a diffuser turn a snapshot into a soft texture that slowly morphs into the next one over 1 … 8 s (Noise). The guitar modulates its own almost frozen copy. IN R is not used. |
| FRev | Freeze with reverb. A live wash appears immediately, then a loud micro-freeze of the last ~6 ms, then a smooth crossfade into the FrzL freeze. A quiet bright reverb (~12 ms) sits before Level; LoCut and HiCut cut it too. |

### Output (Out)

| Mode | What it does |
|---|---|
| Mix | Both outputs carry the same dry/wet mix. |
| Sep | Left output is the vocoder only, right output is the clean modulator. |
| T.LFO | Pan once per beat at the BPM of the sForm effect (120 BPM if sForm is absent). |
| LFO A, LFO B | Pan driven by sForm's LFO A or LFO B through the shared mailbox. |
| P0.1 … P9.7 | Auto-pan: wet and dry signals travel across the stereo field in opposite directions at 0.1 … 9.7 Hz. |

### Behaviour worth knowing

- Output level follows the carrier, not the modulator. The modulator shapes the sound, the carrier sets loudness. When the modulator stops, the vocoder stops.
- In Out = Mix a −54 dB gate on the modulator silences the gaps between phrases. Sep and the pan modes are not gated.
- With no jack in IN R the pedal copies L there. The effect detects this and removes the copy from the live carrier, leaving noise only. Freeze still records IN R.
- The band envelope detector is fixed at a value chosen by ear; its knob became Level.
- Maximum 80 bands: more raises DSP load and causes DSP Full.

## Building from source

Apart from the TI C6000 CGT 8.5.0.LTS compiler, everything is here: `effects/vocowder/`, shared headers in `effects/include/`, the ZDL linker and screen helpers in `toolchain/zdl/` (from ZoomMultistompZDL, MIT, with our fixes; see NOTICE.md).

1. Install TI CGT 8.5.0.LTS (part of Code Composer Studio). If it is not in the default location, set `TI_CGT_C6000` in `paths.py`.
2. Run `python effects/vocowder/build.py` — it writes `Vocowder.ZDL` and `Vocowder.png` into the root. `ZDL_OUT` changes the folder.

## History

See CHANGELOG.md for every version, including test and rejected builds.

## License

MIT, see LICENSE. The ZDL toolchain is MIT as well (ZoomMultistompZDL, LICENSE.upstream).
