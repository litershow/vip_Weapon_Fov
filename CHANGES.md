# 2.0-native

- Возврат к подтверждённому рабочему CameraServices-only механизму.
- Найдена и вызывается штатная внутренняя CameraServices SetFOV из libserver.so.
- Адрес не hard-coded: Linux executable segment сканируется по code shape, а
  смещения m_iFOV/m_hZoomOwner проверяются через runtime SchemaSystem.
- Удалены из рабочего пути m_iDesiredFOV, m_flViewmodelFOV, viewmodel XYZ,
  point_clientcommand и IVEngineServer2::ClientCommand.
- Добавлен watchdog 0.05 s, который вызывает SetFOV только после реального reset.
- Добавлен 40-frame repair burst после weapon_switch/item_equip.
- Добавлен 64-frame spawn burst.
- `!fovdiag` показывает target, cam, rate/time, native match/RVA и число repairs.
- `!fovcam off` штатно сбрасывает CameraServices через native SetFOV(...,0,0,0).
