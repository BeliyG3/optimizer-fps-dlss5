# Контракт temporal-проходов

Источник списка — [`temporal_passes.def`](temporal_passes.def), типы и размер dispatch —
[`temporal_layout.h`](temporal_layout.h), точки входа — [`temporal_cs.hlsl`](temporal_cs.hlsl).
Fork копирует эти файлы вместе с `temporal.hlsl` и `fullscreen.hlsli` в `optimizer_fps/sdk/shaders/`.
Его собственный `shaders/temporal_cs.hlsl` содержит только include SDK.

Необязательные проходы поля неба (`TemporalSkyField`) описаны теми же столбцами в
[`temporal_sky_passes.def`](temporal_sky_passes.def): без их бинарников ядро не включает поле неба (одна строка
в логе), остальное работает как раньше.

Необязательные проходы маркера и проверки звена цепочки описаны теми же столбцами в
[`temporal_marker_passes.def`](temporal_marker_passes.def), каждый сам по себе: без `MarkerReason` выключен дамп
`DebugTemporalDumpAcc`, без `MarkerFail` правила 3/4 проверяют соседей прямо в ModelMotion (те же решения за один
проход), без `LinkFail` правило 3 `DebugTemporalChainRule` работает как правило 2, без `AccumulatePair` или
`RefinePair` у `DebugTemporalCarryChainRule` нет цепочки переноса (перенесённые кадры идут по основной).
Отсутствие любого — одна строка в логе, машина работает. У `RefinePair` есть вариант сетки модели
(`temporal_RefinePairModel_cs.dxbc`, `PW_T_MODEL_GRID`), тоже необязательный: без него хост с сеткой модели остаётся без
цепочки переноса. Функции проходов цепочки переноса — в [`temporal_chain_pair.hlsli`](temporal_chain_pair.hlsli), его
включает `temporal_cs.hlsl`; форку при синхронизации нужен и этот файл.

Каждая строка задаёт имя `CS<Name>` / `temporal_<Name>_cs.dxbc`, поле PSO, маску читаемых SRV,
число UAV и пространство размеров. Оба продукта используют строки для списка бинарников/PSO,
фильтрации SRV и выбора размеров dispatch. Группа — 16×8, у AccumulatePair своя (`PW_TEMPORAL_PAIR_THREADS_*`, 16×16:
голоса 5×5 считаются по находкам звеньев плитки группы и ореола 2 текселя в groupshared: каждый тексель проверяется
один раз, ореол — только если в плитке кто-то голосует, у края — клэмп к `PwTMotionRect`), неполные крайние группы
отсекаются.
`tests/temporal_contract.py` сверяет маски, UAV и константы с reflection скомпилированных DXBC.

| Проход | Отличающиеся от стандартных SRV | u0 | u1 | Размер |
|---|---|---|---|---|
| Residual | t1: новый ответ модели; t2: предыдущий residual | новый residual | — | native |
| Downsample | t2: новый residual | низкочастотный residual | — | ceil(native/16) |
| Accumulate | t4: предыдущая цепочка; `PwTSky.z` — правило перезапуска цепочки (`DebugTemporalChainRule`, 0..4, 0 — прежнее; с цепочкой переноса обе цепочки пишет AccumulatePair); t1: маска LinkFail (только правило 3), иначе null view R8_UINT | новая цепочка | — | motion texture |
| Refine | t4: только что накопленная цепочка | уточнённая цепочка | — | motion texture |
| Expect | t6: глубина предыдущего кадра; t11: предыдущая expectation | новая expectation | — | motion texture |
| ModelMotion | t4: текущая цепочка; `PwTParams.z` — правило маркера (`DebugTemporalMarkerRule`, 0..4, 0 — прежнее); `PwTParams.y` — порог 5×5 правил 3/4 (`DebugTemporalMarkerNeighbours`, 0 — 12); t1: маска MarkerFail при `PwTParams.x` = 1 (только правила 3/4), иначе null view | проверенные motion vectors модели | — | motion texture |
| MarkerReason | t4: текущая цепочка (только `DebugTemporalDumpAcc`); t1 и `PwTParams.x`/`.y`/`.z` как у ModelMotion | R8_UINT: причина маркера ModelMotion (0 нет, 1 за кадром, 2 глубина, 3 цвет, 4 не число) | — | motion texture |
| MarkerFail | t4: текущая цепочка (только правила 3/4, перед ModelMotion/MarkerReason с теми же входами); `PwTParams.z` как у ModelMotion | R8_UINT: базовый вердикт каждого текселя (коды MarkerReason без голосования 5×5) | — | motion texture |
| LinkFail | t4: предыдущая цепочка (только правило 3 `DebugTemporalChainRule` без цепочки переноса, перед Accumulate с теми же входами; с цепочкой переноса голосует AccumulatePair) | R8_UINT: вердикт правила 2 для непроверенной цепочки каждого текселя (1 — перезапуск, 0 — нет или звено не проверяется) | — | motion texture |
| AccumulatePair | t4/t2: предыдущие основная цепочка и цепочка переноса (или снова основная, если та её принимает); t8: копия основной прошлого кадра до Refine при `PwTParams.z` = 1 (правило 6), иначе null view; `PwTSky.z`/`.w` — правила; голоса 5×5 (правила 3, 5, 6) — в groupshared, без маски (см. выше) | основная цепочка | цепочка переноса | motion texture |
| RefinePair | t4: только что накопленная основная цепочка; t2: только что накопленная цепочка переноса | уточнённая основная | уточнённая цепочка переноса (где она равна основной — результат основной) | motion texture |
| ResidualOld | t1: новый residual; t2: предыдущий residual/замороженная смесь | предыдущий residual в координатах нового прохода | — | native |
| Apply | t0: исходный кадр; t2/t10: новый/старый residual; t5: глубина кадра; t20: поле неба (небо — только новый residual) | кадр с фазируемым residual | — | native |
| History, params.x=0 | t0: сохранённый цвет; t2: предыдущий residual | residual истории | цвет истории | ceil(native/3) |
| History, params.x=1 | t4: цепочка нового прохода к предыдущему; t5: глубина этого кадра (источник связи); t6: предыдущая глубина; t11: expectation | глубина истории | связь между проходами | ceil(native/6) |
| Reproject | t4: цепочка переноса, если она есть (`DebugTemporalCarryChainRule` 1..6), иначе основная; t10: фазируемый residual; t12..t19: история; t20/t21: поле неба и доля неба кадра (только для кадра в u0) | восстановленный кадр; перед Compose — null view, запись пропускается | addition/acceptance | native |
| Cells | t2: addition/acceptance | addition ячеек | цвет ячеек | ceil(native/16) |
| Compose | t1: цвет ячеек; t2: addition; t5: глубина кадра; t8: addition ячеек; t20/t21: поле неба и доля неба кадра | сглаженный кадр | — | native |
| Stats | t4: текущая основная цепочка | длина смещения | — | заданная сетка CPU readback |
| FlowLuma | t0: кадр optical-flow сессии | яркость для optical flow | — | заданный размер сессии |
| FlowChain | t9: S10.5 поле optical flow | цепочка в единицах motion texture | — | motion texture |
| SkyDown | t2: новый residual; t6: глубина этого прохода | нижний уровень поля неба (rgb — среднее по пикселям дальней плоскости, a — их доля) | — | ceil(native/16) |
| SkyPull | t2: более мелкий уровень; `PwTParams.x` — клеток нижнего уровня на сторону его текселя, `PwTFill.yz` — размер нижнего уровня | следующий уровень пирамиды (веса по числу покрытых клеток) | — | уровень поля |
| SkyPush | t1: заполненный более крупный уровень; t2: свой уровень | заполненный уровень (a — опора поля, доля неба 1x1) | — | уровень поля |
| SkyNow | t5: глубина переносимого кадра | нижний уровень доли неба кадра (a — доля решётки 4x4 на дальней плоскости, rgb = 0); до 1x1 его доводит SkyPull | — | ceil(native/16) |

Стандартные SRV: t0 цвет, t1 свежий ответ, t2 residual, t3 motion vectors, t4 цепочка,
t5 текущая глубина, t6 глубина residual-кадра, t7 цвет residual-кадра, t8 low residual,
t9 optical flow, t10 старый residual, t11 expected depth. t12/t13 — residual двух старых проходов,
t14/t15 — их цвет, t16/t17 — глубина, t18/t19 — связи. В каждой паре первым идёт более новый проход.
t20 — поле неба (`PwTSkyField`); null view означает «поле выключено». t21 — доля неба переносимого кадра
(`PwTSkyNow`, a текселя 1x1): вместе с опорой поля задаёт его вес (0 — обычный перенос, 1 — поле целиком).
Неиспользуемые слоты не получают активную ссылку на исходный ресурс.

b0 — 44 float-константы из `temporal.hlsl` (10-я строка `PwTSky`: x — дальняя плоскость, 1 или 0 при reversed Z;
y — accept-all DebugTemporalCarryProbe, только у Reproject, остальные проходы получают 0; z — `DebugTemporalChainRule`,
только у Accumulate, LinkFail и AccumulatePair, остальные проходы получают 0;
w — `DebugTemporalCarryChainRule`, только у AccumulatePair, остальные проходы получают 0; 11-я `PwTDepthFRect`: рамка
глубины в t6 такой, какой её описал собственный кадр снимка — у игр с динамическим разрешением она меняется от кадра к кадру; нулевой размер означает `PwTDepthRect`); b1 — восемь uint: размер цели, размер optical-flow
сессии, шаг поля, `PwTSkipOut0` (1 — u0 привязан как null view, его заполнит следующий проход)
и padding. Add-on передаёт их root constants, fork — CBV. Это один shader ABI,
но не одна root signature. s0 — linear clamp, s1 — point clamp.

## Что ещё остаётся в адаптерах

Таблица описывает **тип прохода**, а не весь граф исполнения. Выделение и срок жизни текстур,
ping-pong, копирование внешних ресурсов, barriers и расписание остаются в адаптерах.
Фоновый режим имеет отдельную pending-цепочку, кадр запуска K, кадр принятия T и две очереди;
у fork есть optical-flow сессия и предапскейльный маршрут. Эти условия нельзя корректно выразить
статическим линейным списком. Полный общий исполнитель потребовал бы отдельного протокола
владения ресурсами/очередями и существенно более широкого изменения.

Поэтому новая математическая операция требует общей функции, compute-entry и строки таблицы;
**новая стадия алгоритма всё ещё требует подключения в расписании каждого применимого адаптера**.
Одна строка не выделяет автоматически новую историю и не определяет её синхронизацию.
Для проверки обязательного паритета служит [таблица возможностей](../../docs/dev/temporal-parity.md).
