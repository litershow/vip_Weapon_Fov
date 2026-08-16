# VIP_FOVWeapon 2.0 Native (Linux CS2)

Эта версия сделана по результатам реального теста на сервере: именно запись в
`CCSPlayerBase_CameraServices::m_iFOV/m_iFOVStart` через `!fovcam` дала нужный
эффект (камера + оружие), но CS2 сбрасывал его при смене оружия.

## Что изменено

- Убраны `m_iDesiredFOV`-записи из рабочего пути.
- Убраны `m_flViewmodelFOV`, XYZ viewmodel и client commands.
- Плагин находит в **загруженном `libserver.so`** штатную внутреннюю функцию
  CameraServices `SetFOV` по сигнатуре и runtime Schema offsets.
- Никакого hard-code `libserver.so + 0x14EB7D0` в рабочем коде нет. Для той
  сборки, которую анализировали, диагностический RVA ожидается `0x14EB7D0`, но
  адрес определяется сканированием.
- После `weapon_switch` и `item_equip` запускается короткий frame-burst.
- Дополнительно работает watchdog раз в 0.05 сек. Он **ничего не пишет**, пока
  `cam FOV` уже равен target; если CS2 сбросил FOV, штатный `SetFOV` вызывается
  снова.
- При spawn выбранный VIP FOV восстанавливается длинным burst, чтобы пережить
  первичный deploy оружия.

## Очень важно перед тестом

На сервере оставьте только **один** FOV-модуль:

`vip_viewmodelfov.so`

Удалите/отключите старый `VIP_FOV.so`, `vip_fovweapon.so` и все предыдущие
тестовые копии `vip_viewmodelfov.so`. Иначе они будут менять FOV друг поверх
друга.

## Первый тест

После загрузки в server console должно быть примерно:

```
[VIP-FOVNative] schema: ... Camera=[FOV:0x178 Start:0x17C ... Owner:0x188] ...
[VIP-FOVNative] native CameraServices SetFOV found: libserver.so+0x14EB7D0 (1 unique match).
```

Главное: `native ... found` и `(1 unique match)`.

В игре:

```
!fovcam 120
!fovdiag
```

Ожидается:

```
[FOV] target=120 camera=120/120 native=yes
[FOV] target=120 cam=120/120 ...
[FOV] native=YES matches=1 rva=0x...
```

После этого много раз переключайте нож / пистолет / основное оружие. Камера и
оружие должны оставаться в том же отдалённом состоянии. Затем снова:

```
!fovdiag
```

Поле `fixes=` покажет, сколько раз плагин реально поймал сброс движка и вернул
FOV. Если после смен оружия `fixes` растёт — watchdog/burst ловит именно тот
reset, который раньше ломал эффект.

Отключить тестовый override и вернуть стандартный FOV:

```
!fovcam off
```

## VIP Core

Feature остаётся совместимой с исходным Pisex VIP_FOV:

```
"FOV" "90,100,110,120,130,150"
```

Cookie также остаётся `FOV_Value`.

## Если native scanner не нашёл функцию

`!fovdiag` покажет `native=NO/fallback`. Тогда плагин использует старый
CameraServices-only raw-write механизм для диагностики. При обновлении CS2
пришлите новый `libserver.so` и вывод консоли — сигнатуру можно обновить без
жёсткого адреса.
