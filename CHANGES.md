# v4 native-detour

- Убран post-frame force-sync как основной механизм.
- Добавлен Linux inline detour внутреннего CameraServices::SetFOV.
- Сброс target=0 подменяется на VIP target до записи в CameraServices.
- Ненулевой zoom FOV движка не перехватывается.
- RVA не hardcoded: signature scanner + runtime Schema offsets.
- При неуникальной сигнатуре detour безопасно отключается.
- `!fovhook off` снимает override и вызывает штатный SetFOV(0).
- Диагностика: resetsBlocked и emergency repairs.
