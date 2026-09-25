# bench12: причина расхождения direct core (план 2, задача 5)

Проверка 2026-09-22, 240 кадров, профиль `run_nrhost`.
Все перечисленные каталоги измерений находятся в `tools/bench12/run_nrhost/`
и исключены из Git. `reference_before_2026.9.1` не изменялся.

## Факты до исправления

- `candidate_p2t5_r1` и `candidate_p2t5_r2`: оба дампа (120, 239) побайтно совпали.
- `candidate_p2t5_timeline` против `reference_p2t5_timeline`: кадры 10–120
  с шагом 10 identical; первое расхождение на этой сетке — 130.
- Уточнение в `candidate_p2t5_boundary` / `reference_p2t5_boundary`:
  120 identical; 121 отличается (MAD 0.2283, max 21, 39.996% пикселей).
  Это первый кадр поворота камеры (`tools/bench12/camera.h`).
- NumPy, абсолютная разница RGB BMP, центральный прямоугольник
  `[192:1728, 108:972]` (зона 80% × 80%): MAD 0.298431, max 21,
  изменено 48.970% пикселей; остальная периферия: MAD 0.103496,
  max 8, изменено 24.042%. Ошибка не ограничена периферией или последним кадром.
- Общие бинарники и шейдеры timeline-прогонов совпали по SHA-256.
  Все 240 блоков NGX T0-аудита совпали после нормализации имени пути.
- В логах обоих путей одна модель 1728×972, без пересоздания и сообщений
  об исчерпании пулов/fallback. `no grid 120` — счётчик сглаживания движения,
  одинаковый в обоих путях. Инструментированный core до правки (`candidate_p2t5_status`):
  T0/T1 `fallbackFrames=0`, `submissionDrops=0`; T1 full=60, interp=179.

## Единственная проверенная гипотеза и исправление

В эталонном INI `Flags=0`; `CoreSession::Open` не задавал Flags и наследовал
`Flags=1` из схемы. Это `ConfigFlagExtendMotionAtEdge`: при нуле PackMotion
ограничивает текущую/предыдущую позиции границами кадра, при единице продолжает
преобразование за границей (`sdk/src/math.cpp`, `PackMotion(LayoutV2)`).
При движении отличаются упакованные векторы и история модели; описания текстур
и параметры NGX остаются одинаковыми. Одного явного `OFPS_SET_FLAGS=0`
в хосте стенда достаточно для устранения всех измеренных расхождений.
Скрипт эталонного `run_nrhost` также явно фиксирует `Flags=0`.

Рендеринг ядра и порядок Execute/уведомлений не менялись. В ядре добавлена
только строка диагностики submissionDrops через существующий StatusLines,
без расширения ABI. Стенд печатает её и OfpsStatus перед Release;
скрипты поддерживают произвольный массив `-DumpFrames`.

## Результат

`candidate_p2t5_flags0` против неизменяемых эталонов:

| Пара | 120 | 239 |
|---|---|---|
| core_warp / warp_t0 | identical, MAD 0 | identical, MAD 0 |
| core_off / off_t0 | identical, MAD 0 | identical, MAD 0 |
| core_warp_t1 / warp_t1 | identical, MAD 0 | identical, MAD 0 |

Все 33 уникальных T0-дампа сетки и уточнения 121–130 также identical.
Все три core-случая: fallbackFrames=0, submissionDrops=0, drain completed.
Допуск T1 ≤0.05 выполнен без ссылки на недетерминированность temporal.
Полные численные результаты: `candidate_p2t5_flags0/comparisons.json`.

Свежий аддон (`reference_p2t5_verified`) также identical исходным эталонам
во всех шести дампах. Аудиты warp T0 (240 блоков) и T1 (61 блок) полностью
совпадают с core. У Off сохраняется отдельное различие типов setter:
Width/Height uint32 против int32 и четыре ресурса d3d12 против pointer.
Значения и описания ресурсов одинаковы; это не полная идентичность NGX-контракта,
хотя изображения Off побайтно совпадают. Эти исходные setter в данной правке
не менялись; причина расхождения warped-кадров устранена одной настройкой Flags.

Проверки: `cmake --build --preset windows-x64-release`,
`ctest --preset windows-x64-release` (vcvars64), 21/21;
`tools\bench12\build.cmd` — успешно, CPU и WARP lifecycle checks прошли.
Логи сборок: `out/p2t5_core_checks.log`, `out/p2t5_bench_build.log`.
