# Контракт temporal-проходов

Источник списка — [`temporal_passes.def`](temporal_passes.def), типы и размер dispatch —
[`temporal_layout.h`](temporal_layout.h), точки входа — [`temporal_cs.hlsl`](temporal_cs.hlsl).
Fork копирует эти файлы вместе с `temporal.hlsl` и `fullscreen.hlsli` в `optimizer_fps/sdk/shaders/`.
Его собственный `shaders/temporal_cs.hlsl` содержит только include SDK.

Каждая строка задаёт имя `CS<Name>` / `temporal_<Name>_cs.dxbc`, поле PSO, маску читаемых SRV,
число UAV и пространство размеров. Оба продукта используют строки для списка бинарников/PSO,
фильтрации SRV и выбора размеров dispatch. Группа — 16×8, неполные крайние группы отсекаются.
`tests/temporal_contract.py` сверяет маски, UAV и константы с reflection скомпилированных DXBC.

| Проход | Отличающиеся от стандартных SRV | u0 | u1 | Размер |
|---|---|---|---|---|
| Residual | t1: новый ответ модели; t2: предыдущий residual | новый residual | — | native |
| Downsample | t2: новый residual | низкочастотный residual | — | ceil(native/16) |
| Accumulate | t4: предыдущая цепочка | новая цепочка | — | motion texture |
| Refine | t4: только что накопленная цепочка | уточнённая цепочка | — | motion texture |
| Expect | t6: глубина предыдущего кадра; t11: предыдущая expectation | новая expectation | — | motion texture |
| ModelMotion | t4: текущая цепочка | проверенные motion vectors модели | — | motion texture |
| ResidualOld | t1: новый residual; t2: предыдущий residual/замороженная смесь | предыдущий residual в координатах нового прохода | — | native |
| Apply | t0: исходный кадр; t2/t10: новый/старый residual | кадр с фазируемым residual | — | native |
| History, params.x=0 | t0: сохранённый цвет; t2: предыдущий residual | residual истории | цвет истории | ceil(native/3) |
| History, params.x=1 | t4: цепочка нового прохода к предыдущему; t5: глубина этого кадра (источник связи); t6: предыдущая глубина; t11: expectation | глубина истории | связь между проходами | ceil(native/6) |
| Reproject | t10: фазируемый residual; t12..t19: история | восстановленный кадр | addition/acceptance | native |
| Cells | t2: addition/acceptance | addition ячеек | цвет ячеек | ceil(native/16) |
| Compose | t1: цвет ячеек; t2: addition; t8: addition ячеек | сглаженный кадр | — | native |
| Stats | t4: текущая цепочка | длина смещения | — | заданная сетка CPU readback |
| FlowLuma | t0: кадр optical-flow сессии | яркость для optical flow | — | заданный размер сессии |
| FlowChain | t9: S10.5 поле optical flow | цепочка в единицах motion texture | — | motion texture |

Стандартные SRV: t0 цвет, t1 свежий ответ, t2 residual, t3 motion vectors, t4 цепочка,
t5 текущая глубина, t6 глубина residual-кадра, t7 цвет residual-кадра, t8 low residual,
t9 optical flow, t10 старый residual, t11 expected depth. t12/t13 — residual двух старых проходов,
t14/t15 — их цвет, t16/t17 — глубина, t18/t19 — связи. В каждой паре первым идёт более новый проход.
Неиспользуемые слоты не получают активную ссылку на исходный ресурс.

b0 — 36 float-констант из `temporal.hlsl`; b1 — восемь uint: размер цели, размер optical-flow
сессии, шаг поля и padding. Add-on передаёт их root constants, fork — CBV. Это один shader ABI,
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
